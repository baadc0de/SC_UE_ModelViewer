// USCViewerSubsystem: archive open, async search, the load pipeline with phase progress and cancel, unload with
// GC accounting, reuse by entity + options hash, camera mode/focus/isolate and the editor Keep hook.
#include "SCViewerSubsystem.h"
#include "SCImport.h"
#include "SCPlacement.h"
#include "SCLook.h"
#include "SCHangarIndex.h"
#include "SCViewerShell.h"
#include "SCEnvironment.h"
#include "Misc/PackageName.h"
#include "SCDirt.h"
#include "SCEmissiveLights.h"
#include "Engine/Level.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "SCViewerAutomation.h"
#include "SCViewerPawn.h"
#include "SCFirstPersonCharacter.h"
#include "SCPlayerBody.h"
#include "SCRigComponent.h"
#include "Camera/CameraComponent.h"
#include "SCViewOptions.h"
#include "SCViewerSettings.h"
#include "SCPackageSource.h"
#include "SCVehicle.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/IConsoleManager.h"
#include "Components/SkyLightComponent.h"
#include "Components/LightComponent.h"
#include "Engine/Light.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "GameFramework/WorldSettings.h"
#include "Engine/StaticMeshActor.h"
#include "SCPlayerKit.h"
#include "SCPlayerKitData.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "RenderingThread.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCViewer, Log, All);

namespace
{
// The Viewer map's floor top (viewer/scripts/create_viewer.py FLOOR_TOP): a loaded model rests on it by its lowest visible
// point (landing gear), and an environment's ship floor is moved to it.
constexpr double ViewerFloorZ = -5.0;
// Progress-bar shares of the pipeline phases (export dominates cold loads, import dominates cache hits).
constexpr float ExportShare = 0.45f, ParseShare = 0.05f;
// Game-thread time per frame given to the import job; the rest of the frame renders the UI and viewport.
constexpr double ImportBudgetSeconds = 0.030;

template <typename T> int32 CountLive()
{
    int32 Count = 0;
    for (TObjectIterator<T> It; It; ++It)
        if (!It->IsTemplate() && IsValid(*It)) ++Count;
    return Count;
}
FBox SubtreeBounds(USceneComponent* Root)
{
    FBox Box(ForceInit), Hidden(ForceInit);
    if (!Root) return Box;
    TArray<USceneComponent*> All;
    Root->GetChildrenComponents(true, All);
    All.Add(Root);
    for (USceneComponent* C : All)
        if (auto* P = Cast<UPrimitiveComponent>(C); P && P->IsRegistered())
            (P->IsVisible() ? Box : Hidden) += P->Bounds.GetBox();
    return Box.IsValid ? Box : Hidden;
}
} // namespace

void USCViewerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    Options = NewObject<USCViewOptions>(this, TEXT("ViewOptions"));
    LoadOptions = NewObject<USCViewOptions>(this, TEXT("LoadOptions"));
    // Both are the importer plugin's (SCBridgeImport); a build without it (a packaged game) has neither and plays
    // prepared content only: no archive opens, nothing imports.
    Client = ISCArchive::Create();
    ISCImporting* const Importer = ISCImporting::Get();
    CacheDir = USCViewerSettings::Get()->ResolveCacheDir();
    if (!CacheDir.IsEmpty() && Importer)
        ProcessedCache = Importer->NewProcessedCache(CacheDir / TEXT("processed"),
            int64(USCViewerSettings::Get()->ProcessedCacheMiB) * (1ll << 20));
#if WITH_EDITOR
    if (Importer && !GIsEditor && !FParse::Param(FCommandLine::Get(), TEXT("SCMaxQuality")) &&
        FParse::Param(FCommandLine::Get(), TEXT("SCStreamGeometry")))
        GeometryStreaming = Importer->NewGeometryStreaming();
#endif
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &USCViewerSubsystem::Tick));
    Automation = MakeShared<FSCViewerAutomation>(*this);
    VehicleInstances = MakeShared<FSCVehicleInstances>(*this);
    // The game user settings applied their scalability before the game instance started (UGameEngine::Init): capped here,
    // before the first frame, which is the one that hung at Cinematic without ray tracing.
    SCLook::CapQualityWithoutRayTracing();
    bMaxQualityPending = FParse::Param(FCommandLine::Get(), TEXT("SCMaxQuality"));
    bShowcase = FParse::Param(FCommandLine::Get(), TEXT("SCShowcase"));
    // The -SCPackage folder harness (WP-U1..U3 review runs) imports from disk and needs no archive.
    FString HarnessPackage;
    const bool bHarness = FParse::Value(FCommandLine::Get(), TEXT("SCPackage="), HarnessPackage);
    if (!bHarness && Client) OpenArchive(FString());
    if (bHarness && !Importer)
    {
        // The harness is the importer plugin's: without it nothing would import, verify or exit.
        UE_LOG(LogSCViewer, Error, TEXT("SC_IMPORT_UNAVAILABLE -SCPackage=%s: this build has no importer (the SCBridgeImport plugin)"), *HarnessPackage);
        FPlatformMisc::RequestExitWithStatus(true, 1); // forced: a graceful request exits 0 from GuardedMain
    }
    // Environment: -SCEnvironment wins; -SCDefaults (reproducible review routes) keeps the procedural hangar; else
    // the saved choice.
    FString EnvironmentArg;
    if (FParse::Value(FCommandLine::Get(), TEXT("SCEnvironment="), EnvironmentArg)) SetEnvironment(EnvironmentArg);
    else if (!FParse::Param(FCommandLine::Get(), TEXT("SCDefaults"))) SetEnvironment(USCViewerSettings::Get()->Environment, true);
}
void USCViewerSubsystem::Deinitialize()
{
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    Automation.Reset();
    if (ExportProgress) ExportProgress->RequestCancel();
    if (ExportTask.IsValid()) ExportTask.Wait();
    if (ParseTask.IsValid()) ParseTask.Wait();
    if (OpenTask.IsValid()) OpenTask.Wait();
    for (auto& Search : Searches)
        Search.Task.Wait();
    Searches.Empty();
    Job.Reset(); // destroys a partial actor if any
    if (KitProgress) KitProgress->RequestCancel();
    if (KitExport.IsValid()) KitExport.Wait();
    if (KitParse.IsValid()) KitParse.Wait();
    if (KitClipsTask.IsValid()) KitClipsTask.Wait();
    KitJob.Reset();
    KitSource.Reset();
    if (EnvProgress) EnvProgress->RequestCancel();
    if (EnvExport.IsValid()) EnvExport.Wait();
    if (EnvParse.IsValid()) EnvParse.Wait();
    if (EnvListTask.IsValid()) EnvListTask.Wait();
    EnvJob.Reset();
    if (GeometryStreaming) GeometryStreaming->PublishWrites();
    UnloadInternal(false); // the subsystem owns the loaded vehicle; the host collects garbage afterwards
    VehicleInstances.Reset();
    Client.Reset();
    ProcessedCache.Reset();
    GeometryStreaming.Reset();
    Super::Deinitialize();
}

USCViewerSubsystem* USCViewerSubsystem::Get(const UObject* WorldContext)
{
    const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
    return World && World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<USCViewerSubsystem>() : nullptr;
}
USCViewerSubsystem* USCViewerSubsystem::CreateHeadless()
{
    UGameInstance* Instance = NewObject<UGameInstance>(GEngine);
    Instance->AddToRoot(); // lives for the rest of the process, like a game's instance
    Instance->InitializeStandalone(TEXT("SCViewerHeadless"));
    return Instance->GetSubsystem<USCViewerSubsystem>();
}
int32 USCViewerSubsystem::DestroyHeadless(USCViewerSubsystem* Viewer)
{
    UGameInstance* Instance = Viewer ? Viewer->GetGameInstance() : nullptr;
    if (!Instance || !Instance->IsRooted()) return -1; // only instances made by CreateHeadless
    UWorld* World = Instance->GetWorld();
    // Account for everything still owned: the loaded vehicle and an import that may be mid-flight with workers running.
    TArray<TWeakObjectPtr<UObject>> Tracked;
    if (ASCVehicle* Loaded = Viewer->Vehicle.Get()) TrackSession(Loaded, Loaded->SessionPath, Tracked);
    const bool bInFlight = Viewer->IsBusy();
    if (Viewer->Job) TrackSession(Viewer->Job->GetSpawnedVehicle(), Viewer->Job->GetSessionPath(), Tracked);
    // The real teardown path: Deinitialize cancels the export, waits for tasks and drops the job (drains workers,
    // destroys a partial actor). No member access after Shutdown.
    Instance->Shutdown();
    if (World)
    {
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
    }
    Instance->RemoveFromRoot();
    const int32 Alive = CollectAndCount(Tracked);
    UE_LOG(LogSCViewer, Display, TEXT("VIEWER_HEADLESS_TEARDOWN in_flight=%d tracked=%d alive_after_gc=%d"), bInFlight, Tracked.Num(), Alive);
    return Alive;
}
void USCViewerSubsystem::TrackSession(const UObject* Actor, const FString& Session, TArray<TWeakObjectPtr<UObject>>& Out)
{
    if (!Actor && Session.IsEmpty()) return;
    for (TObjectIterator<UObject> It; It; ++It) // includes garbage-marked objects (destroyed, not yet collected)
        if ((Actor && (*It == Actor || It->IsIn(Actor))) || (!Session.IsEmpty() && It->GetOutermost()->GetName().StartsWith(Session)))
            Out.Add(*It);
}
int32 USCViewerSubsystem::CollectAndCount(const TArray<TWeakObjectPtr<UObject>>& Tracked)
{
    FlushRenderingCommands();
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    int32 Alive = 0;
    for (const auto& Object : Tracked)
        if (Object.IsValid(/*bEvenIfPendingKill*/ true)) ++Alive;
    return Alive;
}
UWorld* USCViewerSubsystem::GetViewerWorld() const
{
    return GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
}
ASCViewerPawn* USCViewerSubsystem::GetViewerPawn() const
{
    UWorld* World = GetViewerWorld();
    APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
    return PC ? Cast<ASCViewerPawn>(PC->GetPawn()) : nullptr;
}
ASCVehicle* USCViewerSubsystem::GetVehicle() const
{
    return Vehicle.Get();
}

// ---------------------------------------------------------------- archive

void USCViewerSubsystem::OpenArchive(const FString& P4kPath)
{
    // Not busy (IsBusy) includes a saved hangar waiting for a failed or closed archive: Retry opens, the hangar follows.
    // A pick waiting for a ready archive's next tick is busy: it is not carried over to another archive.
    if (ArchiveState == ESCArchiveState::Opening || IsBusy())
    {
        UE_LOG(LogSCViewer, Warning, TEXT("OpenArchive ignored: an open or load is in progress"));
        return;
    }
    if (!Client)
    {
        // The archive stays closed: no reader in this build (ISCArchive::Create, the importer plugin's).
        UE_LOG(LogSCViewer, Warning, TEXT("VIEWER_ARCHIVE_UNAVAILABLE this build has no archive reader (the SCBridgeImport plugin)"));
        return;
    }
    ArchivePath = P4kPath.IsEmpty() ? USCViewerSettings::Get()->ResolveArchivePath() : P4kPath;
    ArchiveState = ESCArchiveState::Opening;
    ++ArchiveGeneration; // a different installation may follow: no reuse of loaded objects, no stale search results
    ArchiveError = FText();
    Catalog = FSCViewerCatalog(); // the lists of the previous archive are not this one's
    Catalog.Revision = ArchiveGeneration;
    EnvironmentChoices.Reset();
    bCatalogWanted = false; // asked for again by a consumer of this archive's lists (an open menu asks every tick)
    if (!CacheDir.IsEmpty()) IFileManager::Get().MakeDirectory(*CacheDir, true);
    UE_LOG(LogSCViewer, Display, TEXT("VIEWER_ARCHIVE_OPEN p4k=%s cache=%s"), ArchivePath.IsEmpty() ? TEXT("<auto-discover>") : *ArchivePath,
           CacheDir.IsEmpty() ? TEXT("<disabled>") : *CacheDir);
    OpenTask = Client->Open(ArchivePath, CacheDir);
}
FString USCViewerSubsystem::GetArchiveLabel() const
{
    return ArchivePath.IsEmpty() ? TEXT("auto-discovered Data.p4k") : ArchivePath;
}
bool USCViewerSubsystem::WaitForArchive(float TimeoutSeconds)
{
    if (OpenTask.IsValid()) OpenTask.Wait(FTimespan::FromSeconds(TimeoutSeconds));
    Tick(0);
    return ArchiveState == ESCArchiveState::Ready;
}

// ---------------------------------------------------------------- search

void USCViewerSubsystem::SearchAsync(const FString& Query, int32 Limit, TFunction<void(const FSCSearchResult&)> Callback)
{
    if (ArchiveState != ESCArchiveState::Ready)
    {
        FSCSearchResult Result;
        Result.Query = Query;
        Result.Error = CanReadArchive() ? NSLOCTEXT("SCViewer", "ArchiveNotReady", "The archive is not open yet")
                                        : NSLOCTEXT("SCViewer", "NoArchiveReader", "This build reads no archive");
        Callback(Result);
        return;
    }
    Searches.Add({Client->Search(Query, Limit), MoveTemp(Callback), ArchiveGeneration});
}
TArray<FSCEntityHit> USCViewerSubsystem::SearchEntities(const FString& Query, int32 Limit)
{
    if (ArchiveState != ESCArchiveState::Ready) return {};
    auto Task = Client->Search(Query, Limit);
    Task.Wait();
    if (!Task.GetResult().Error.IsEmpty()) UE_LOG(LogSCViewer, Warning, TEXT("Search failed: %s"), *Task.GetResult().Error.ToString());
    PollClientPanic();
    return Task.GetResult().Hits;
}
void USCViewerSubsystem::CompleteSearches(bool bBlocking)
{
    for (int32 I = 0; I < Searches.Num();)
    {
        if (!bBlocking && !Searches[I].Task.IsCompleted())
        {
            ++I;
            continue;
        }
        FPendingSearch Done = MoveTemp(Searches[I]);
        Searches.RemoveAt(I);
        if (Done.Generation != ArchiveGeneration) continue; // issued against an earlier archive session: drop
        FSCSearchResult Result = Done.Task.GetResult();
        Result.Generation = Done.Generation;
        Done.Callback(Result);
    }
}

// ---------------------------------------------------------------- load pipeline

bool USCViewerSubsystem::IsBusy() const
{
    return Phase == ESCLoadPhase::Exporting || Phase == ESCLoadPhase::Parsing || Phase == ESCLoadPhase::Importing ||
           (bEnvironmentPending && !EnvironmentAwaitsArchive()) || EnvJob.IsValid() || EnvExport.IsValid() || EnvParse.IsValid() ||
           EnvPlaceInTicks > 0 || (EnvLevel && !Environment.IsValid());
}
bool USCViewerSubsystem::EnvironmentAwaitsArchive() const
{
    return bEnvironmentPending && (ArchiveState == ESCArchiveState::Failed || ArchiveState == ESCArchiveState::Closed) &&
           EnvironmentFolder.EndsWith(TEXT(".socpak"), ESearchCase::IgnoreCase) && CookedEnvironmentLevel(EnvironmentFolder).IsEmpty();
}
FString USCViewerSubsystem::GetCatalogError() const
{
    if (Catalog.bReady) return FString();
    if (!Catalog.Error.IsEmpty()) return Catalog.Error;
    if (ArchiveState == ESCArchiveState::Failed) return ArchiveError.ToString();
    if (ArchiveState == ESCArchiveState::Closed) return CanReadArchive() ? TEXT("No archive is open") : TEXT("This build reads no archive");
    return FString(); // opening, or listing
}

void USCViewerSubsystem::SetEnvironment(const FString& Folder, bool bFitToShip)
{
    PreferredEnvironment = Folder.TrimQuotes();
    bFitEnvironment = bFitToShip && USCViewerSettings::Get()->bFitHangarToShip;
    RequestEnvironment(PreferredEnvironment);
    if (bFitEnvironment && Vehicle.IsValid()) FitEnvironmentToShip();
}

bool USCViewerSubsystem::SetPaint(int32 Index)
{
    ASCVehicle* V = Vehicle.Get();
    if (!V || !V->SetPaint(Index)) return false;
    USCViewerSettings* Settings = USCViewerSettings::Get();
    // Keyed by the vehicle in the scene: Report.Entity already names a load in progress.
    if (Index == INDEX_NONE) Settings->Paints.Remove(LoadedEntity);
    else Settings->Paints.Add(LoadedEntity, V->Manifest.Paints[Index].PaletteId);
    Settings->Save();
    // The colours saved for this paint come back with it.
    if (const TMap<FString, FLinearColor> Saved = SavedPaintColors(LoadedEntity, Index == INDEX_NONE ? FString() : V->Manifest.Paints[Index].PaletteId);
        !Saved.IsEmpty())
        V->SetPaintColors(Saved);
    return true;
}

FString USCViewerSubsystem::PaintColorsFile() { return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PaintColors.json")); }

FString USCViewerSubsystem::ToHex(const FLinearColor& Linear)
{
    const FColor C = Linear.ToFColorSRGB();
    return FString::Printf(TEXT("#%02X%02X%02X"), C.R, C.G, C.B);
}

bool USCViewerSubsystem::FromHex(const FString& Hex, FLinearColor& OutLinear)
{
    FString Digits = Hex.TrimStartAndEnd();
    Digits.RemoveFromStart(TEXT("#"));
    if (Digits.Len() != 6) return false;
    for (const TCHAR Ch : Digits)
        if (!FChar::IsHexDigit(Ch)) return false;
    OutLinear = FLinearColor(FColor::FromHex(Digits)); // sRGB bytes to linear
    return true;
}

namespace
{
/** PaintColors.json as JSON (an empty store when the file is absent or unreadable). */
TSharedPtr<FJsonObject> ReadPaintColorStore()
{
    FString Text;
    TSharedPtr<FJsonObject> Store;
    if (FFileHelper::LoadFileToString(Text, *USCViewerSubsystem::PaintColorsFile()))
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Store);
    if (!Store) Store = MakeShared<FJsonObject>();
    Store->SetNumberField(TEXT("version"), 1);
    return Store;
}
bool SameEntry(const TSharedPtr<FJsonObject>& E, const FString& Entity, const FString& PaletteId)
{
    FString Vehicle, Palette;
    return E && E->TryGetStringField(TEXT("vehicle"), Vehicle) && Vehicle.Equals(Entity, ESearchCase::IgnoreCase) &&
           (E->TryGetStringField(TEXT("palette"), Palette) ? Palette : FString()) == PaletteId;
}
} // namespace

TMap<FString, FLinearColor> USCViewerSubsystem::SavedPaintColors(const FString& Entity, const FString& PaletteId) const
{
    TMap<FString, FLinearColor> Out;
    const TSharedPtr<FJsonObject> Store = ReadPaintColorStore();
    const TArray<TSharedPtr<FJsonValue>>* Entries;
    if (!Store->TryGetArrayField(TEXT("paints"), Entries)) return Out;
    for (const TSharedPtr<FJsonValue>& V : *Entries)
    {
        const TSharedPtr<FJsonObject> E = V.IsValid() && V->Type == EJson::Object ? V->AsObject() : nullptr;
        const TSharedPtr<FJsonObject>* Colors;
        if (!SameEntry(E, Entity, PaletteId) || !E->TryGetObjectField(TEXT("colors"), Colors)) continue;
        for (const auto& [Channel, Value] : (*Colors)->Values)
        {
            FLinearColor C;
            if (Value.IsValid() && Value->Type == EJson::String && ASCVehicle::PaintChannels().Contains(FString(Channel)) && FromHex(Value->AsString(), C)) Out.Add(FString(Channel), C);
            else UE_LOG(LogSCViewer, Warning, TEXT("PAINT_COLORS %s: %s %s is not a channel with a #RRGGBB colour (ignored)"), *PaintColorsFile(), *Entity, *Channel);
        }
    }
    return Out;
}

bool USCViewerSubsystem::SetPaintColors(const TMap<FString, FLinearColor>& Overrides, bool bSave)
{
    ASCVehicle* V = Vehicle.Get();
    if (!V || !V->SetPaintColors(Overrides)) return false;
    if (!bSave) return true;
    const int32 Paint = V->GetPaint();
    const FString PaletteId = Paint == INDEX_NONE ? FString() : V->Manifest.Paints[Paint].PaletteId;
    const TSharedPtr<FJsonObject> Store = ReadPaintColorStore();
    TArray<TSharedPtr<FJsonValue>> Entries;
    const TArray<TSharedPtr<FJsonValue>>* Existing;
    if (Store->TryGetArrayField(TEXT("paints"), Existing))
        for (const TSharedPtr<FJsonValue>& E : *Existing)
            if (!SameEntry(E.IsValid() && E->Type == EJson::Object ? E->AsObject() : nullptr, LoadedEntity, PaletteId)) Entries.Add(E);
    if (!Overrides.IsEmpty())
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("vehicle"), LoadedEntity);
        Entry->SetStringField(TEXT("palette"), PaletteId);
        Entry->SetStringField(TEXT("paint"), Paint == INDEX_NONE ? FString(TEXT("Default paint")) : V->Manifest.Paints[Paint].Label());
        TSharedPtr<FJsonObject> Colors = MakeShared<FJsonObject>();
        for (const FString& Channel : ASCVehicle::PaintChannels())
            if (const FLinearColor* C = Overrides.Find(Channel)) Colors->SetStringField(Channel, ToHex(*C));
        Entry->SetObjectField(TEXT("colors"), Colors);
        Entries.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Store->SetArrayField(TEXT("paints"), Entries);
    FString Text;
    FJsonSerializer::Serialize(Store.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
    if (!FFileHelper::SaveStringToFile(Text, *PaintColorsFile(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        UE_LOG(LogSCViewer, Warning, TEXT("PAINT_COLORS cannot write %s"), *PaintColorsFile());
    return true;
}

void USCViewerSubsystem::FitEnvironmentToShip()
{
    ASCVehicle* Ship = Vehicle.Get();
    const FString Preferred = PreferredEnvironment.Replace(TEXT("\\"), TEXT("/"));
    if (!bFitEnvironment || !Ship || !Preferred.EndsWith(TEXT(".socpak"), ESearchCase::IgnoreCase)) return; // the map's hangar is not measured
    const USCHangarIndex* Index = USCHangarIndex::Load();
    if (!Index) return;
    const FString Style = FPaths::GetCleanFilename(FPaths::GetPath(Preferred));
    const FSCHangarCapacity* Chosen = Index->Find(Style, FPaths::GetBaseFilename(Preferred));
    if (!Chosen) return; // not cooked: its capacity is unknown, keep it
    // The ship stands on the bay centre (world origin) at the viewer floor.
    FBox ShipBox = FSCEnvironment::SolidBounds(Ship);
    if (!ShipBox.IsValid) return;
    ShipBox = ShipBox.ShiftBy(FVector(0, 0, -ViewerFloorZ));
    const FSCHangarCapacity* Best = nullptr;
    const FSCHangarCapacity* Largest = nullptr;
    auto Area = [](const FSCHangarCapacity* H) { return H->BayCm.X * H->BayCm.Y; };
    auto SameKind = [&](const FSCHangarCapacity* H) { return H->bTopLoading == Chosen->bTopLoading; };
    // Smallest that holds the ship, the chosen entrance kind (roof hatch / front doors) first; when none holds it, the
    // largest of the style, again the chosen kind first.
    for (const FSCHangarCapacity& H : Index->Hangars)
    {
        if (H.Style != Style) continue;
        if (H.Holds(ShipBox) && (!Best || (SameKind(&H) && !SameKind(Best)) || (SameKind(&H) == SameKind(Best) && Area(&H) < Area(Best))))
            Best = &H;
        if (!Largest || (SameKind(&H) && !SameKind(Largest)) || (SameKind(&H) == SameKind(Largest) && Area(&H) > Area(Largest))) Largest = &H;
    }
    const FSCHangarCapacity* Pick = Best ? Best : Largest;
    const FString Choice = FPaths::GetPath(Preferred) / Pick->Name + TEXT(".socpak");
    UE_LOG(LogSCViewer, Display, TEXT("ENVIRONMENT_FIT ship_m=%.1fx%.1fx%.1f (x %.1f..%.1f y %.1f..%.1f) preferred=%s holds=%d choice=%s bay_m=%.1fx%.1f clearance_m=%.1f"),
           ShipBox.GetSize().X / 100, ShipBox.GetSize().Y / 100, ShipBox.Max.Z / 100, ShipBox.Min.X / 100, ShipBox.Max.X / 100, ShipBox.Min.Y / 100,
           ShipBox.Max.Y / 100, *Chosen->Name, Chosen->Holds(ShipBox), *Pick->Name, Pick->BayCm.X / 100, Pick->BayCm.Y / 100, Pick->ClearanceCm / 100);
    if (Choice != EnvironmentFolder.Replace(TEXT("\\"), TEXT("/"))) RequestEnvironment(Choice);
}

void USCViewerSubsystem::CancelEnvironmentWork()
{
    if (EnvProgress && (EnvExport.IsValid() || EnvParse.IsValid())) EnvProgress->RequestCancel();
}

void USCViewerSubsystem::RequestEnvironment(const FString& Folder)
{
    if (EnvProgress) EnvProgress->RequestCancel();
    if (EnvExport.IsValid()) EnvExport.Wait();
    if (EnvParse.IsValid()) EnvParse.Wait();
    EnvExport = {};
    EnvParse = {};
    EnvJob.Reset();
    EnvSource.Reset();
    if (Environment.IsValid() && !EnvLevel)
    {
        ForgetGeometry(Environment.Get());
        Environment->Destroy(); // a cooked environment leaves with its level
    }
    Environment.Reset();
    UnloadEnvironmentLevel();
    EnvironmentFolder = Folder.TrimQuotes();
    bEnvironmentPending = !EnvironmentFolder.IsEmpty() && EnvironmentFolder != TEXT("none");
    if (!bEnvironmentPending) ShowMapHangar(true);
    UE_LOG(LogSCViewer, Display, TEXT("ENVIRONMENT request folder=\"%s\""), *EnvironmentFolder);
}

void USCViewerSubsystem::UnloadEnvironmentLevel()
{
    if (!EnvLevel) return;
    ReleaseLevel(EnvLevel);
    EnvLevel = nullptr;
}
namespace
{
FString LevelName(FString Name)
{
    Name.TrimStartAndEndInline();
    for (TCHAR& C : Name)
        if (!FChar::IsAlnum(C) && C != TEXT('_')) C = TEXT('_');
    return Name;
}
} // namespace
FString USCViewerSubsystem::EnvironmentLevelPath(const FString& Folder)
{
    const FString Path = Folder.Replace(TEXT("\\"), TEXT("/")).TrimStartAndEnd();
    if (!Path.EndsWith(TEXT(".socpak"), ESearchCase::IgnoreCase)) return FString();
    // Readable from the last two parts, told apart by a hash of the whole path (archive paths ignore case): containers
    // whose last two parts make the same name are other levels.
    const uint32 Hash = FCrc::StrCrc32(*Path.ToLower());
    return FString::Printf(TEXT("/Game/SC/Hangars/Maps/%s_%08x"),
                           *LevelName(FPaths::GetCleanFilename(FPaths::GetPath(Path)) + TEXT("_") + FPaths::GetBaseFilename(Path)), Hash);
}
FString USCViewerSubsystem::ShipLevelPath(const FString& Entity, const FString& Variant)
{
    // "__" separates the paint variant: an entity's own part never has it (IsShipPaintLevel).
    FString Name = LevelName(Entity);
    while (Name.Contains(TEXT("__"))) Name.ReplaceInline(TEXT("__"), TEXT("_"));
    if (Name.IsEmpty()) return FString();
    return TEXT("/Game/SC/Ships/Maps/") + Name + (Variant.IsEmpty() ? FString() : TEXT("__") + LevelName(Variant));
}
FString USCViewerSubsystem::PlayerLevelPath(const FString& Gender, const FString& Loadout, const TArray<FString>& Weapons, bool bSpareAttachments)
{
    // Archive paths and entity classes ignore case; the exporter keeps the named weapons whatever their order and however often
    // one is named, and asks for the client's default gender when none is given: one body, one key.
    TArray<FString> Kept;
    for (const FString& Weapon : Weapons)
        if (const FString W = Weapon.TrimStartAndEnd().ToLower(); !W.IsEmpty()) Kept.AddUnique(W);
    Kept.Sort();
    const FString Sex = Gender.TrimStartAndEnd().IsEmpty() ? FString(ISCArchive::DefaultPlayerGender) : Gender.TrimStartAndEnd().ToLower();
    // The exporter reads a loadout under Data/ whether or not the path names it: one loadout, one key.
    FString Path = Loadout.Replace(TEXT("\\"), TEXT("/")).TrimStartAndEnd().ToLower();
    if (!Path.IsEmpty() && !Path.StartsWith(TEXT("data/"))) Path = TEXT("data/") + Path;
    const FString Key = FString::Printf(TEXT("%s|%s|%s|%d"), *Sex, *Path, *FString::Join(Kept, TEXT(",")), bSpareAttachments ? 1 : 0);
    const FString Name = LevelName(Sex);
    return FString::Printf(TEXT("/Game/SC/Players/Maps/Player_%s_%08x"), *Name, FCrc::StrCrc32(*Key));
}
FString USCViewerSubsystem::TraversalClipsPath(const FString& PlayerLevel, const FString& VehicleLevel, const FString& EnvironmentLevel)
{
    const FString Key = FString::Printf(TEXT("%s|%s|%s"), *PlayerLevel.ToLower(), *VehicleLevel.ToLower(), *EnvironmentLevel.ToLower());
    return FString::Printf(TEXT("/Game/SC/Players/Traversal/Traversal_%08x"), FCrc::StrCrc32(*Key));
}
void USCViewerSubsystem::SetPreparedTraversalClips(const FString& Package)
{
    PreparedTraversal = Package;
    TraversalData = nullptr;
    bTraversalWarned = false;
    if (Package.IsEmpty()) return;
    // Loaded in the background while the scenario's levels stream; a request before it finished loads it at once.
    TWeakObjectPtr<USCViewerSubsystem> Weak(this);
    LoadPackageAsync(Package, FLoadPackageAsyncDelegate::CreateLambda([Weak, Package](const FName&, UPackage* Loaded, EAsyncLoadingResult::Type) {
        if (!Weak.IsValid() || Weak->PreparedTraversal != Package || Weak->TraversalData || !Loaded) return;
        Weak->TraversalData = FindObject<USCTraversalClipsData>(Loaded, *FPackageName::GetShortName(Package));
    }));
}
TSharedPtr<const FSCPlayerClipMap> USCViewerSubsystem::GetPreparedTraversalClips()
{
    if (PreparedTraversal.IsEmpty()) return nullptr;
    if (!TraversalData)
        TraversalData = LoadObject<USCTraversalClipsData>(nullptr, *(PreparedTraversal + TEXT(".") + FPackageName::GetShortName(PreparedTraversal)));
    const TCHAR* Problem = !TraversalData ? TEXT("does not load")
        : !TraversalData->Clips ? TEXT("does not read")
        : TraversalData->BodyLevel != PreparedPlayerLevel ? TEXT("was read for another body") : nullptr;
    if (!Problem) return TraversalData->Clips;
    if (!bTraversalWarned)
        UE_LOG(LogSCViewer, Warning, TEXT("TRAVERSAL_CLIPS %s %s (body %s)"), *PreparedTraversal, Problem, *PreparedPlayerLevel);
    bTraversalWarned = true;
    return nullptr;
}
bool USCViewerSubsystem::IsShipPaintLevel(const FString& Level)
{
    return FPackageName::GetShortName(Level).Contains(TEXT("__"));
}
FString USCViewerSubsystem::CookedEnvironmentLevel(const FString& Folder)
{
    if (!SCViewerShell::UsesCookedLevels() || FParse::Param(FCommandLine::Get(), TEXT("SCNoCooked")))
        return FString(); // -SCNoCooked: as if none was cooked (StepEnvironment and IsBusy decide alike)
    const FString Level = EnvironmentLevelPath(Folder);
    if (Level.IsEmpty()) return FString();
    // A level the app's check refuses (UnrealCitizen: not prepared in the current format) is not used.
    if (FPackageName::DoesPackageExist(Level)) return SCViewerShell::PassesCookedLevelCheck(Level) ? Level : FString();
    // A level cooked before WP-O1-4 (named by the last two path parts only, without the path hash).
    const FString Legacy = Level.LeftChop(9);
    return FPackageName::DoesPackageExist(Legacy) && SCViewerShell::PassesCookedLevelCheck(Legacy) ? Legacy : FString();
}
void USCViewerSubsystem::SetSpace(bool bInSpace)
{
    UWorld* World = GetViewerWorld();
    if (!World) return;
    bSpace = bInSpace;
    if (bSpace) SetEnvironment(TEXT("none")); // no room around the ship (ShowMapHangar keeps the map's hidden in space)
    ShowMapHangar(!bSpace);
    // Gravity: none in space (boxes let go of float on), the project's default otherwise.
    if (AWorldSettings* Settings = World->GetWorldSettings())
    {
        Settings->bWorldGravitySet = bSpace;
        Settings->WorldGravityZ = 0.f;
        if (!bSpace) Settings->GetGravityZ();
    }
    // A sun: the only light out there (viewer choice: the archive's star lighting is not imported).
    if (bSpace && !SpaceSun.IsValid())
    {
        FActorSpawnParameters Params;
        Params.ObjectFlags |= RF_Transient;
        ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector::ZeroVector, FRotator(-35, 40, 0), Params);
        if (Sun)
        {
            Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable);
            Sun->GetLightComponent()->SetIntensity(SpaceSunLux);
            Sun->GetLightComponent()->SetCastShadows(true);
            // The one directional light forward shading, translucency and fog use (the fill below has the default 0).
            if (UDirectionalLightComponent* C = Cast<UDirectionalLightComponent>(Sun->GetLightComponent())) C->SetForwardShadingPriority(1);
            SpaceSun = Sun;
        }
        // A weak fill from the other side, no shadows (viewer choice: starlight and nebula glow; without it the sides
        // away from the sun are black).
        ADirectionalLight* Fill = World->SpawnActor<ADirectionalLight>(FVector::ZeroVector, FRotator(35, 40 + 180, 0), Params);
        if (Fill)
        {
            Fill->GetLightComponent()->SetMobility(EComponentMobility::Movable);
            Fill->GetLightComponent()->SetIntensity(SpaceSunLux * SpaceFillShare);
            Fill->GetLightComponent()->SetCastShadows(false);
            Fill->GetLightComponent()->SetLightColor(FLinearColor(0.75f, 0.82f, 1.f));
            SpaceFill = Fill;
        }
    }
    else if (!bSpace && SpaceSun.IsValid())
    {
        SpaceSun->Destroy();
        SpaceSun.Reset();
        if (SpaceFill.IsValid()) SpaceFill->Destroy();
        SpaceFill.Reset();
    }
    UE_LOG(LogSCViewer, Display, TEXT("VIEWER_SPACE on=%d gravity_z=%.1f sun=%d"), bSpace, World->GetGravityZ(), SpaceSun.IsValid());
}

void USCViewerSubsystem::SetSpaceSunLux(float Lux)
{
    SpaceSunLux = FMath::Max(Lux, 0.f);
    if (SpaceSun.IsValid()) SpaceSun->GetLightComponent()->SetIntensity(SpaceSunLux);
    if (SpaceFill.IsValid()) SpaceFill->GetLightComponent()->SetIntensity(SpaceSunLux * SpaceFillShare);
    UE_LOG(LogSCViewer, Display, TEXT("VIEWER_SPACE sun_lux=%.0f sun=%d"), SpaceSunLux, SpaceSun.IsValid());
}

static FAutoConsoleCommandWithWorldAndArgs GSCSpaceSun(
    TEXT("sc.Space.Sun"), TEXT("sc.Space.Sun [lux]: the space sun's illuminance (no argument logs it)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCViewerSubsystem* V = World && World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<USCViewerSubsystem>() : nullptr;
        if (!V) return;
        if (Args.IsValidIndex(0)) V->SetSpaceSunLux(FCString::Atof(*Args[0]));
        else UE_LOG(LogSCViewer, Display, TEXT("VIEWER_SPACE sun_lux=%.0f"), V->GetSpaceSunLux());
    }));

void USCViewerSubsystem::ShowMapHangar(bool bShow)
{
    UWorld* World = GetViewerWorld();
    if (!World) return;
    bShow &= !bSpace; // in space the map's hangar stays hidden
    int32 Changed = 0;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* A = *It;
        if (Cast<ASCVehicle>(A) || A->GetLevel() != World->PersistentLevel || A == SpaceSun.Get() || A == SpaceFill.Get()) continue; // the lights of space stay
        const bool bMesh = A->IsA<AStaticMeshActor>(), bLight = A->IsA<ALight>();
        if (!bMesh && !bLight) continue;
        A->SetActorHiddenInGame(!bShow);
        A->SetActorEnableCollision(bShow);
        if (bLight) A->ForEachComponent<ULightComponent>(false, [bShow](ULightComponent* L) { L->SetVisibility(bShow); });
        ++Changed;
    }
    for (TObjectIterator<USkyLightComponent> It; It; ++It)
        if (It->GetWorld() == World) It->RecaptureSky();
    UE_LOG(LogSCViewer, Display, TEXT("ENVIRONMENT map_hangar=%s actors=%d"), bShow ? TEXT("shown") : TEXT("hidden"), Changed);
}

void USCViewerSubsystem::BeginEnvironmentImport(TSharedPtr<FSCPackageSource> Source)
{
    EnvSource = Source;
    const FString Scene = FSCPlayerKit::FindRootScene(*EnvSource);
    if (Scene.IsEmpty())
    {
        UE_LOG(LogSCViewer, Error, TEXT("ENVIRONMENT failed: no single root scene.json in %s"), *EnvironmentFolder);
        EnvSource.Reset();
        return;
    }
    FSCImportOptions EnvOptions;
    EnvOptions.World = GetViewerWorld();
    EnvOptions.ScenePath = Scene;
    EnvOptions.bBuildCollision = true;
    EnvOptions.bPublish = false;        // not the vehicle: routes, first person and light pairing keep the ship
    EnvOptions.bEnvironment = true;     // its waypoints (the first-person teleport menu's Hangar section)
    EnvOptions.bReplacePrevious = false;
    EnvOptions.bHideEffectOnlyPlacements = true;
    EnvOptions.bInstanceRepeatedParts = true;
    ConfigureProcessedImport(EnvOptions, !EnvironmentFolder.EndsWith(TEXT(".socpak"), ESearchCase::IgnoreCase));
    ISCImporting* const Importer = ISCImporting::Get();
    if (!Importer)
    {
        UE_LOG(LogSCViewer, Error, TEXT("ENVIRONMENT failed: %s"), TEXT("this build has no importer (the SCBridgeImport plugin)"));
        EnvSource.Reset();
        return;
    }
    EnvJob = Importer->NewJob(*EnvSource, EnvOptions, EnvSource);
}

void USCViewerSubsystem::StepEnvironment()
{
    if (EnvPlaceInTicks > 0 && --EnvPlaceInTicks == 0) PlaceEnvironment();
    // The pick lists (WP-O1-3): chosen by the core from game data, listed once per archive session when asked for.
    if (bCatalogWanted && ArchiveState == ESCArchiveState::Ready && Client && EnvListedGeneration != ArchiveGeneration && !EnvListTask.IsValid())
    {
        EnvListTask = Client->ViewerCatalog();
        EnvListIssued = ArchiveGeneration;
    }
    // Completion is tested once; a listing issued for an earlier archive session is dropped (asked again next tick).
    if (EnvListTask.IsValid() && EnvListTask.IsCompleted())
    {
        const FSCTextResult Listed = EnvListTask.GetResult();
        EnvListTask = {};
        if (EnvListIssued == ArchiveGeneration)
        {
            EnvListedGeneration = ArchiveGeneration;
            Catalog = FSCViewerCatalog();
            Catalog.Revision = ArchiveGeneration;
            Catalog.bReady = Listed.bOK && FSCViewerCatalog::Parse(Listed.Json, Catalog, Catalog.Error);
            if (!Listed.bOK) Catalog.Error = Listed.Error.ToString();
            if (!Catalog.bReady && Catalog.Error.IsEmpty()) Catalog.Error = TEXT("the catalogue did not list"); // a failure is always named
            EnvironmentChoices.Reset();
            for (const FSCCatalogEntry& E : Catalog.Environments) EnvironmentChoices.Add(E.Id);
            UE_LOG(LogSCViewer, Display, TEXT("VIEWER_CATALOG environments=%d vehicles=%d%s%s"), Catalog.Environments.Num(), Catalog.Vehicles.Num(),
                   Catalog.Error.IsEmpty() ? TEXT("") : TEXT(" error="), *Catalog.Error);
        }
    }
    if (EnvLevel && !Environment.IsValid())
    {
        if (EnvLevel->GetLevelStreamingState() == ELevelStreamingState::FailedToLoad)
        {
            UE_LOG(LogSCViewer, Error, TEXT("ENVIRONMENT failed: the cooked level %s did not load"), *EnvLevel->GetWorldAssetPackageName());
            UnloadEnvironmentLevel();
            return;
        }
        ULevel* Level = EnvLevel->GetLoadedLevel();
        if (!Level || !EnvLevel->IsLevelVisible()) return;
        for (AActor* Actor : Level->Actors)
            if (auto* Cooked = Cast<ASCVehicle>(Actor)) { Environment = Cooked; break; }
        if (!Environment.IsValid())
        {
            UE_LOG(LogSCViewer, Error, TEXT("ENVIRONMENT failed: cooked level %s holds no environment actor"), *EnvLevel->GetWorldAssetPackageName());
            UnloadEnvironmentLevel();
            return;
        }
        // Runtime-only state the cook does not keep: emissive Lumen proxies, dirt parameters, the light shadow policy.
        ShowMapHangar(false);
        Environment->RefreshLightShadowPolicy();
        FSCDirt::Push(GetViewerWorld());
        if (!Environment->Lights.IsEmpty()) FSCEmissive::SetLumen(Environment.Get(), USCViewerSettings::Get()->bEmissiveInLumen);
        if (ASCViewerPawn* Pawn = GetViewerPawn()) Pawn->SetOrbitObstacle(Environment.Get());
        // A state asked for while the level streamed in (the prep saved its own pose, open): posed now, then placed on the
        // floor in that pose (its bay under the ship, as a live import places it), its waypoints following.
        const FString Wanted = EnvironmentState == TEXT("open") || EnvironmentState == TEXT("closed") ? EnvironmentState : FString(TEXT("bind"));
        if (Environment->ClipCatalog && Environment->PresentedState != Wanted)
        {
            FSCEnvironment::SetState(Environment.Get(), EnvironmentState);
            FSCEnvironment::PlaceOnFloor(Environment.Get(), ViewerFloorZ, Vehicle.Get());
            Environment->BuildWaypoints();
        }
        UE_LOG(LogSCViewer, Display, TEXT("ENVIRONMENT_COOKED level=%s seconds=%.1f"), *EnvLevel->GetWorldAssetPackageName(), FPlatformTime::Seconds() - EnvStart);
        return;
    }
    const bool bShipIdle = Phase != ESCLoadPhase::Exporting && Phase != ESCLoadPhase::Parsing && Phase != ESCLoadPhase::Importing;
    if (bEnvironmentPending && !EnvJob && !EnvExport.IsValid() && !EnvParse.IsValid() && bShipIdle && GetViewerWorld())
    {
        if (const FString Cooked = CookedEnvironmentLevel(EnvironmentFolder); !Cooked.IsEmpty())
        {
            bEnvironmentPending = false;
            bool bOK = false;
            EnvStart = FPlatformTime::Seconds();
            EnvArchiveGeneration = ArchiveGeneration;
            EnvLevel = ULevelStreamingDynamic::LoadLevelInstance(GetViewerWorld(), Cooked, FVector::ZeroVector, FRotator::ZeroRotator, bOK);
            UE_LOG(LogSCViewer, Display, TEXT("ENVIRONMENT_COOKED_LOAD level=%s ok=%d"), *Cooked, bOK);
            if (!bOK) EnvLevel = nullptr;
            return;
        }
        if (EnvironmentFolder.EndsWith(TEXT(".socpak"), ESearchCase::IgnoreCase))
        {
            if (!Client)
            {
                // No archive reader in this build (a packaged game): only a prepared environment can show; nothing to wait for.
                bEnvironmentPending = false;
                UE_LOG(LogSCViewer, Error, TEXT("ENVIRONMENT failed: %s is not prepared and this build has no archive reader"), *EnvironmentFolder);
                return;
            }
            if (ArchiveState != ESCArchiveState::Ready) return; // wait for the archive
            bEnvironmentPending = false;
            FSCExportOptions Ffi;
            LoadOptions->ToExportOptions(Ffi);
            Ffi.Mip = FMath::Max<uint32>(Ffi.Mip, 1u); // environments: half-resolution textures (tar under 2 GB)
            EnvProgress = MakeShared<FSCExportProgress, ESPMode::ThreadSafe>();
            EnvExport = Client->ExportEnvironment(EnvironmentFolder, Ffi, EnvProgress.ToSharedRef());
            EnvArchiveGeneration = ArchiveGeneration;
            EnvStart = FPlatformTime::Seconds();
            return;
        }
        bEnvironmentPending = false;
        EnvStart = FPlatformTime::Seconds();
        EnvArchiveGeneration = ArchiveGeneration; // a folder is no archive's: this session's
        BeginEnvironmentImport(MakeShared<FSCFolderPackageSource>(EnvironmentFolder));
    }
    if (EnvExport.IsValid())
    {
        if (!EnvExport.IsCompleted()) return;
        FSCExportResult Exported = MoveTemp(EnvExport.GetResult());
        EnvExport = {};
        if (EnvProgress && EnvProgress->IsCancelRequested())
        {
            UE_LOG(LogSCViewer, Display, TEXT("ENVIRONMENT cancelled: export %s"), *EnvironmentFolder); // a newer choice follows
            return;
        }
        if (!Exported.bOK)
        {
            UE_LOG(LogSCViewer, Error, TEXT("ENVIRONMENT failed: export %s: %s"), *EnvironmentFolder, *Exported.Error.ToString());
            return;
        }
        UE_LOG(LogSCViewer, Display, TEXT("ENVIRONMENT_EXPORT socpak=%s bytes=%lld seconds=%.1f cache_hit=%d"), *EnvironmentFolder, Exported.Tar.Num(),
               Exported.Seconds, Exported.Cache.bLastHit);
        EnvParse = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Tar = MoveTemp(Exported.Tar)]() mutable {
            return TSharedPtr<FSCTarPackageSource>(MakeShared<FSCTarPackageSource>(MoveTemp(Tar)));
        });
        return;
    }
    if (EnvParse.IsValid())
    {
        if (!EnvParse.IsCompleted()) return;
        TSharedPtr<FSCTarPackageSource> Parsed = EnvParse.GetResult();
        EnvParse = {};
        if (EnvProgress && EnvProgress->IsCancelRequested())
        {
            UE_LOG(LogSCViewer, Display, TEXT("ENVIRONMENT cancelled: parse %s"), *EnvironmentFolder);
            return;
        }
        if (!Parsed || !Parsed->IsValid())
        {
            UE_LOG(LogSCViewer, Error, TEXT("ENVIRONMENT failed: %s"), Parsed ? *Parsed->GetError() : TEXT("cannot read the package"));
            return;
        }
        BeginEnvironmentImport(Parsed);
    }
    if (!EnvJob || !EnvJob->Tick(false, ImportBudgetSeconds)) return;
    const FSCImportResult& Result = EnvJob->GetResult();
    if (EnvJob->GetStage() == ESCImportStage::Succeeded && Result.Vehicle.IsValid())
    {
        Environment = Result.Vehicle;
        ShowMapHangar(false);
        const FBox Bounds = Environment->GetComponentsBoundingBox(true);
        UE_LOG(LogSCViewer, Display, TEXT("ENVIRONMENT_IMPORT seconds=%.3f meshes=%d instances=%d lights=%d bounds=%s cache_hits=%d mesh_builds=%d collision_builds=%d"),
               FPlatformTime::Seconds() - EnvStart, Result.MeshCount, Result.InstanceCount, Environment->Lights.Num(), *Bounds.ToString(),
               Result.CachedMeshes, Result.MeshBuilds, Result.CollisionBuilds);
        EnvPlaceInTicks = 2; // components settle; placement uses the bind pose, then the presented state is applied
    }
    else UE_LOG(LogSCViewer, Error, TEXT("ENVIRONMENT failed: %s"), *Result.Error);
    EnvJob.Reset();
    EnvSource.Reset();
}

void USCViewerSubsystem::PlaceEnvironment()
{
    ASCVehicle* EnvironmentActor = Environment.Get();
    if (!EnvironmentActor) return;
    if (ASCViewerPawn* Pawn = GetViewerPawn()) Pawn->SetOrbitObstacle(EnvironmentActor);
    SetEnvironmentState(EnvironmentState); // placement measures the presented pose (the ship elevator raised or lowered)
    FSCEnvironment::PlaceOnFloor(EnvironmentActor, ViewerFloorZ, Vehicle.Get());
    EnvironmentActor->BuildWaypoints(); // the presented pose (the ship elevator raised); a cooked one keeps its own
    SCLook::UpdateShortRangeAO(EnvironmentActor->GetWorld());
    SCLook::RefreshGlassRefraction(EnvironmentActor->GetWorld());
}

void USCViewerSubsystem::SetEnvironmentState(const FString& State)
{
    EnvironmentState = State.ToLower();
    FSCEnvironment::SetState(Environment.Get(), EnvironmentState);
    if (Environment.IsValid()) Environment->BuildWaypoints(); // the new pose moves pads and kiosks riding a mechanism
}
FString USCViewerSubsystem::CookedShipLevel(const FString& EntityOrRecord)
{
    // A record's name (an adopted package's manifest: EntityClassDefinition.<entity>) is its entity's level.
    const FString Entity = EntityName(EntityOrRecord);
    if (Entity.IsEmpty() || !SCViewerShell::UsesCookedLevels() || FParse::Param(FCommandLine::Get(), TEXT("SCNoCooked"))) return FString();
    const FString Level = ShipLevelPath(Entity);
    return !Level.IsEmpty() && FPackageName::DoesPackageExist(Level) && SCViewerShell::PassesCookedLevelCheck(Level) ? Level : FString();
}
FString USCViewerSubsystem::EntityName(const FString& EntityOrRecord)
{
    FString Name = EntityOrRecord.TrimStartAndEnd();
    Name.RemoveFromStart(TEXT("EntityClassDefinition."), ESearchCase::IgnoreCase);
    return Name;
}
FString USCViewerSubsystem::PreparedShipLevelFor(const FString& Entity) const
{
    const FString Asked = EntityName(Entity), Loaded = EntityName(LoadedEntity);
    if (!LoadedShipLevel.IsEmpty() && Asked.Equals(Loaded, ESearchCase::IgnoreCase)) return LoadedShipLevel;
    if (FString Own = CookedShipLevel(Asked); !Own.IsEmpty()) return Own;
    // Only the level it was stored from (a package holding just a scenario's paint).
    const FStoredShip* Stored = StoredShipLevels.Find(Asked.ToLower());
    return Stored && FPackageName::DoesPackageExist(Stored->Level) ? Stored->Level : FString();
}
FString USCViewerSubsystem::GarageShipLevel(const FString& Entity) const
{
    const FStoredShip* Stored = StoredShipLevels.Find(EntityName(Entity).ToLower());
    if (Stored && FPackageName::DoesPackageExist(Stored->Level)) return Stored->Level;
    return CookedShipLevel(Entity);
}
bool USCViewerSubsystem::CanLoadGarage(const FString& Entity) const
{
    return !IsBusy() && (ArchiveState == ESCArchiveState::Ready || !GarageShipLevel(Entity).IsEmpty());
}
void USCViewerSubsystem::SetStoredShipLevel(const FString& Entity, const FString& Level)
{
    const FString Name = EntityName(Entity);
    if (Name.IsEmpty()) return;
    if (Level.IsEmpty()) StoredShipLevels.Remove(Name.ToLower());
    else StoredShipLevels.Add(Name.ToLower(), FStoredShip{Name, Level});
}
TArray<FString> USCViewerSubsystem::GetStoredShipEntities() const
{
    TArray<FString> Out;
    for (const auto& [Key, Stored] : StoredShipLevels)
        if (FPackageName::DoesPackageExist(Stored.Level)) Out.Add(Stored.Entity);
    return Out;
}
void USCViewerSubsystem::CancelPendingScript(bool bSceneGoing, bool bFinal)
{
    // Not while a ship is loaded or loading (whoever asked for it): the route runs on that one.
    const bool bShipLoading = Phase == ESCLoadPhase::Exporting || Phase == ESCLoadPhase::Parsing || Phase == ESCLoadPhase::Importing;
    // Final without an archive reader, or when the owner says so (with a reader, a later load may still bring a ship for
    // an owner that is not done).
    if (Automation && (bFinal || !CanReadArchive()) && (bSceneGoing || (!Vehicle.IsValid() && !bShipLoading)))
        Automation->CancelUnstartedScript();
}
bool USCViewerSubsystem::CanLoad(const FString& Entity) const
{
    return !IsBusy() && (ArchiveState == ESCArchiveState::Ready || !CookedShipLevel(Entity).IsEmpty());
}

bool USCViewerSubsystem::LoadEntity(const FString& Entity, USCViewOptions* InOptions)
{
    return StartLoad(Entity, InOptions, CookedShipLevel(Entity));
}
bool USCViewerSubsystem::LoadPreparedEntity(const FString& Entity, const FString& Level)
{
    if (Level.IsEmpty() || !FPackageName::DoesPackageExist(Level))
    {
        UE_LOG(LogSCViewer, Warning, TEXT("LoadPreparedEntity(%s) refused: no level %s"), *Entity, *Level);
        return false;
    }
    return StartLoad(Entity, nullptr, Level);
}
bool USCViewerSubsystem::StartLoad(const FString& Entity, USCViewOptions* InOptions, const FString& Cooked)
{
    if (Entity.IsEmpty() || IsBusy() || (ArchiveState != ESCArchiveState::Ready && Cooked.IsEmpty()))
    {
        UE_LOG(LogSCViewer, Warning, TEXT("LoadEntity(%s) refused: %s"), *Entity,
               Entity.IsEmpty() ? TEXT("no entity") : IsBusy() ? TEXT("a load is running")
               : CanReadArchive() ? TEXT("archive not open") : TEXT("the ship is not prepared and this build reads no archive"));
        return false;
    }
    LoadOptions->CopyFrom(InOptions ? InOptions : Options.Get());
    ++LoadSerial;
    bLoadCancelRequested = false;
    Report = FSCLoadReport();
    Report.Entity = Entity;
    Report.Options = LoadOptions->Describe();
    // A prepared ship by its entity's name (any spelling naming the same level is the same ship: one key, one loaded name).
    const FString Name = Cooked.IsEmpty() ? Entity : EntityName(Entity);
    PendingEntity = Name;
    Report.ArchiveGeneration = int32(ArchiveGeneration);
    PendingKey = FString::Printf(TEXT("%u|%s|%s"), ArchiveGeneration, *Name.ToLower(), *LoadOptions->GetHash());
    if (!Cooked.IsEmpty()) PendingKey += TEXT("|") + Cooked; // another level of the entity (a prepared paint) is another load
    PendingShipLevel = Cooked;
    LoadStart = FPlatformTime::Seconds();
    UE_LOG(LogSCViewer, Display, TEXT("VIEWER_LOAD_START entity=%s options=\"%s\""), *Entity, *Report.Options);
    if (!bRebuildProcessed && PendingKey == LoadedKey && Vehicle.IsValid())
    {
        Report.bReused = true; // identical request: keep the live objects, skip export and import
        FinishLoad(ESCLoadPhase::Succeeded, FString());
        return true;
    }
    if (!Cooked.IsEmpty())
    {
        UnloadInternal(true, bGarageLoad);
        bool bOK = false;
        ShipLevel = ULevelStreamingDynamic::LoadLevelInstance(GetViewerWorld(), Cooked, FVector::ZeroVector, FRotator::ZeroRotator, bOK);
        if (!bOK || !ShipLevel)
        {
            ShipLevel = nullptr;
            FinishLoad(ESCLoadPhase::Failed, TEXT("cannot stream the cooked level ") + Cooked);
            return false;
        }
        ImportStart = FPlatformTime::Seconds();
        Phase = ESCLoadPhase::Importing;
        UE_LOG(LogSCViewer, Display, TEXT("VIEWER_LOAD_COOKED entity=%s level=%s"), *Entity, *Cooked);
        return true;
    }
    FSCExportOptions Ffi;
    LoadOptions->ToExportOptions(Ffi);
    ExportProgress = MakeShared<FSCExportProgress, ESPMode::ThreadSafe>();
    ExportTask = Client->ExportDecomposed(Entity, Ffi, ExportProgress.ToSharedRef());
    Phase = ESCLoadPhase::Exporting;
    return true;
}
bool USCViewerSubsystem::LoadGarageVehicle(const FString& Entity)
{
    if (Vehicle.IsValid() || !CanLoadGarage(Entity)) return false;
    bGarageLoad = true;
    if (StartLoad(Entity, GetOptions(), GarageShipLevel(Entity))) return true;
    bGarageLoad = false;
    return false;
}

int32 USCViewerSubsystem::SpawnGarageFloorVehicle(const FString& Entity)
{
    return VehicleInstances ? VehicleInstances->Spawn(Entity, true, FTransform::Identity, true, true) : INDEX_NONE;
}

bool USCViewerSubsystem::StoreGaragePrimary()
{
    if (!Vehicle.IsValid() || IsBusy()) return false;
    // It comes back from the level it came from (a scenario's paint; a package may hold no other); one from the archive
    // (no level) clears what an earlier store of the entity recorded.
    SetStoredShipLevel(LoadedEntity, LoadedShipLevel);
    UnloadInternal(true, true);
    return true;
}

FSCCacheStats USCViewerSubsystem::GetProcessedCacheStats() const
{
    FSCCacheStats Stats;
    if (ProcessedCache)
    {
        Stats.bEnabled = ProcessedCache->IsEnabled();
        Stats.CapBytes = uint64(ProcessedCache->GetCapBytes());
        Stats.bLastHit = Report.CachedMeshes > 0 && Report.MeshBuilds == 0;
        const FSCProcessedCacheStats Disk = ProcessedCache->Stats();
        Stats.Bytes = uint64(Disk.Bytes);
        Stats.Entries = uint64(Disk.Entries);
    }
    return Stats;
}

void USCViewerSubsystem::ConfigureProcessedImport(FSCImportOptions& Import, bool bFolderPackage) const
{
    // Keep as Assets needs editable source models. Game mode can load cooked render data.
    if (GIsEditor && KeepHandler().IsBound()) return;
    Import.ProcessedCache = ProcessedCache;
    // A folder may be used without opening any archive. The cache still keys each mesh on its
    // path and GLB content and each texture on its bytes. Separate folders from archive exports.
    Import.CacheArchiveBuild = bFolderPackage ? TEXT("folder-package") : ArchiveBuildId;
    if (GeometryStreaming && !bGeometryCapturePrepared)
    {
        Import.GeometryStreaming = GeometryStreaming;
        Import.bRetainAssets = false; // streaming replacements belong to a single assembled actor
    }
}

bool USCViewerSubsystem::PrepareGeometryForCapture()
{
    if (!GeometryStreaming || bGeometryCapturePrepared) return true;
    if (IsBusy() || (VehicleInstances && VehicleInstances->IsSpawning())) return false;
    bGeometryCapturePrepared = true; // cuts and console-triggered loads in a capture use exact geometry
    GeometryStreaming->FlushForCapture(GetViewerWorld());
    GeometryStreaming->FinishWrites(GetViewerWorld());
    return true;
}

void USCViewerSubsystem::ForgetGeometry(ASCVehicle* Owner)
{
    if (GeometryStreaming) GeometryStreaming->Forget(Owner);
}

void USCViewerSubsystem::LogGeometryStreaming() const
{
    if (GeometryStreaming) GeometryStreaming->LogStatus(GetViewerWorld());
}

static FAutoConsoleCommandWithWorldAndArgs GSCGeometryStatus(
    TEXT("sc.Geometry.Status"), TEXT("Report pending fine meshes and processed cache writes."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World) {
        if (USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(World))
            Viewer->LogGeometryStreaming();
    }));

void USCViewerSubsystem::ClearProcessedCache()
{
    if (IsBusy() || !ProcessedCache) return;
    if (GeometryStreaming) GeometryStreaming->DropWrites();
    ProcessedCache->Clear();
}

void USCViewerSubsystem::SetProcessedCacheCap(int32 MiB)
{
    if (IsBusy()) return;
    USCViewerSettings* Settings = USCViewerSettings::Get();
    Settings->ProcessedCacheMiB = FMath::Max(0, MiB);
    Settings->Save();
    if (ProcessedCache) ProcessedCache->SetCapBytes(int64(Settings->ProcessedCacheMiB) * (1ll << 20));
}

bool USCViewerSubsystem::CanRebuildLoadedEntity() const
{
    return !IsBusy() && !LoadedEntity.IsEmpty() && CookedShipLevel(LoadedEntity).IsEmpty();
}

bool USCViewerSubsystem::RebuildLoadedEntity()
{
    if (!CanRebuildLoadedEntity()) return false;
    bRebuildProcessed = true;
    const FString Entity = LoadedEntity;
    if (LoadEntity(Entity, LoadOptions)) return true;
    bRebuildProcessed = false;
    return false;
}

void USCViewerSubsystem::CancelLoad()
{
    if (!IsBusy()) return;
    if (ShipLevel && !Vehicle.IsValid())
    {
        UnloadShipLevel();
        FinishLoad(ESCLoadPhase::Cancelled, FString());
        return;
    }
    Report.MeshesBuiltAtCancel = Job ? Job->GetResult().MeshStats.Num() : 0;
    Report.bWorkersPendingAtCancel = Job ? Job->HasPendingWorkers() : (ExportTask.IsValid() && !ExportTask.IsCompleted()) ||
                                                                          (ParseTask.IsValid() && !ParseTask.IsCompleted());
    // A vehicle load being stopped, until it finishes (FinishLoad); an environment-only busy state is not one.
    bLoadCancelRequested = Phase == ESCLoadPhase::Exporting || Phase == ESCLoadPhase::Parsing || Phase == ESCLoadPhase::Importing;
    if (ExportProgress) ExportProgress->RequestCancel(); // export: FFI callback returns nonzero; parse: checked after
    if (Job) Job->Cancel();
}
void USCViewerSubsystem::Unload()
{
    CancelLoad();
    WaitForLoad(60.f); // cancellation is cooperative; drain it before tearing down
    UnloadInternal(true);
}
bool USCViewerSubsystem::WaitForLoad(float TimeoutSeconds)
{
    // The vehicle load only: StepLoad advances nothing else, and environment work (IsBusy counts it) goes on in the tick.
    const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
    while ((Phase == ESCLoadPhase::Exporting || Phase == ESCLoadPhase::Parsing || Phase == ESCLoadPhase::Importing) &&
           FPlatformTime::Seconds() < Deadline)
        StepLoad(true);
    return Phase == ESCLoadPhase::Succeeded;
}
bool USCViewerSubsystem::WaitForPhase(ESCLoadPhase Target, float TimeoutSeconds)
{
    const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
    while (Phase != Target && IsBusy() && FPlatformTime::Seconds() < Deadline)
    {
        // Advance one phase at a time: the import job is created on entering Importing and not stepped here.
        const ESCLoadPhase Before = Phase;
        if (Phase == ESCLoadPhase::Importing)
        {
            if (Job) Job->Tick(false, ImportBudgetSeconds);
            else StepLoad(true); // a prepared level streams: no import job
        }
        else if (Phase == ESCLoadPhase::Exporting && !ExportTask.IsCompleted())
            ExportTask.Wait();
        else if (Phase == ESCLoadPhase::Parsing && !ParseTask.IsCompleted())
            ParseTask.Wait();
        if (Phase == Before && Phase != ESCLoadPhase::Importing) StepLoad(false);
        if (Phase == ESCLoadPhase::Importing && Job && Job->IsFinished()) StepLoad(false);
    }
    return Phase == Target;
}
bool USCViewerSubsystem::WaitForImportedMeshes(int32 MinMeshes, float TimeoutSeconds)
{
    const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
    if (Phase != ESCLoadPhase::Importing && !WaitForPhase(ESCLoadPhase::Importing, TimeoutSeconds)) return false;
    while (Phase == ESCLoadPhase::Importing && FPlatformTime::Seconds() < Deadline)
    {
        if (!Job) return false; // a prepared level streams: no mesh workers to observe
        if (Job->GetResult().MeshStats.Num() >= MinMeshes && Job->HasPendingWorkers()) return true;
        if (Job->IsFinished())
        {
            StepLoad(false); // finalize the load; the requested mid-import state was not observed
            return false;
        }
        Job->Tick(false, 0.0); // a zero budget runs at most one step
        FPlatformProcess::SleepNoStats(0.0005f);
    }
    return false;
}
float USCViewerSubsystem::GetLoadProgress() const
{
    switch (Phase)
    {
    case ESCLoadPhase::Exporting: return ExportShare * (ExportProgress ? ExportProgress->GetFraction() : 0.f);
    case ESCLoadPhase::Parsing: return ExportShare;
    case ESCLoadPhase::Importing: return ExportShare + ParseShare + (1.f - ExportShare - ParseShare) * (Job ? Job->GetFraction() : 0.f);
    case ESCLoadPhase::Succeeded: return 1.f;
    default: return 0.f;
    }
}
float USCViewerSubsystem::GetEnvironmentProgress() const
{
    if (EnvLevel && !Environment.IsValid()) return 0.5f;
    if (EnvExport.IsValid()) return ExportShare * (EnvProgress ? EnvProgress->GetFraction() : 0.f);
    if (EnvParse.IsValid()) return ExportShare;
    if (EnvJob) return ExportShare + ParseShare + (1.f - ExportShare - ParseShare) * EnvJob->GetFraction();
    return EnvPlaceInTicks > 0 ? 1.f : 0.f;
}
FString USCViewerSubsystem::GetEnvironmentPhaseText() const
{
    if (bEnvironmentPending) return TEXT("waiting for the model");
    if (EnvLevel && !Environment.IsValid()) return TEXT("Loading cooked level");
    if (EnvExport.IsValid())
    {
        const FString Step = EnvProgress ? EnvProgress->GetPhase() : FString();
        return Step.IsEmpty() ? TEXT("Export: starting") : TEXT("Export: ") + Step;
    }
    if (EnvParse.IsValid()) return TEXT("Reading package");
    if (EnvJob) return TEXT("Import: ") + EnvJob->GetPhase();
    return EnvPlaceInTicks > 0 ? TEXT("Placing") : FString();
}
FString USCViewerSubsystem::GetLoadPhaseText() const
{
    switch (Phase)
    {
    case ESCLoadPhase::Idle: return TEXT("Idle");
    case ESCLoadPhase::Exporting:
    {
        const FString Step = ExportProgress ? ExportProgress->GetPhase() : FString();
        return Step.IsEmpty() ? TEXT("Export: starting") : TEXT("Export: ") + Step;
    }
    case ESCLoadPhase::Parsing: return TEXT("Reading package");
    case ESCLoadPhase::Importing: return Job ? TEXT("Import: ") + Job->GetPhase() : FString(TEXT("Loading cooked level"));
    case ESCLoadPhase::Succeeded: return Report.bReused ? TEXT("Reused loaded objects") : TEXT("Loaded");
    case ESCLoadPhase::Failed: return TEXT("Failed: ") + Report.Error;
    case ESCLoadPhase::Cancelled: return TEXT("Cancelled");
    }
    return FString();
}

void USCViewerSubsystem::StepLoad(bool bBlocking)
{
    if (ShipLevel && !Vehicle.IsValid())
    {
        if (bBlocking)
            if (UWorld* World = GetViewerWorld()) World->FlushLevelStreaming(); // commandlets and tests: no world tick streams it
        StepCookedShip();
        return;
    }
    if (Phase == ESCLoadPhase::Exporting)
    {
        if (!ExportTask.IsCompleted())
        {
            if (!bBlocking) return;
            ExportTask.Wait();
        }
        FSCExportResult Result = MoveTemp(ExportTask.GetResult());
        ExportTask = {};
        Report.ExportSeconds = Result.Seconds;
        Report.bExportCacheHit = Result.Cache.bLastHit;
        CacheStats = Result.Cache;
        Report.CacheBytes = int64(Result.Cache.Bytes);
        Report.CacheEntries = int64(Result.Cache.Entries);
        if (!Result.bOK)
        {
            PollClientPanic();
            FinishLoad(Result.bCancelled ? ESCLoadPhase::Cancelled : ESCLoadPhase::Failed, Result.Error.ToString());
            return;
        }
        Report.TarBytes = Result.Tar.Num();
        ParseStart = FPlatformTime::Seconds();
        // Indexing the tar (headers, PAX paths, checksums) runs on a worker; the source owns the bytes.
        ParseTask = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Tar = MoveTemp(Result.Tar)]() mutable {
            return TSharedPtr<FSCTarPackageSource>(MakeShared<FSCTarPackageSource>(MoveTemp(Tar)));
        });
        Phase = ESCLoadPhase::Parsing;
        if (!bBlocking) return;
    }
    if (Phase == ESCLoadPhase::Parsing)
    {
        if (!ParseTask.IsCompleted())
        {
            if (!bBlocking) return;
            ParseTask.Wait();
        }
        TSharedPtr<FSCTarPackageSource> Source = ParseTask.GetResult();
        ParseTask = {};
        Report.ParseSeconds = FPlatformTime::Seconds() - ParseStart;
        if (ExportProgress && ExportProgress->IsCancelRequested())
        {
            FinishLoad(ESCLoadPhase::Cancelled, TEXT("Cancelled"));
            return;
        }
        if (!Source->IsValid())
        {
            FinishLoad(ESCLoadPhase::Failed, Source->GetError());
            return;
        }
        // Free the previous ship before building the next one so peak memory holds one ship (requests for this one stay).
        UnloadInternal(true, bGarageLoad, /*bForPendingLoad=*/true);
        FSCImportOptions ImportOptions;
        ImportOptions.World = GetViewerWorld();
        ImportOptions.bBuildCollision = LoadOptions->bCollision;
        ImportOptions.bRetainAssets = FSCVehicleInstances::ShareAssets(); // extra instances of it are assembled from them
        ConfigureProcessedImport(ImportOptions);
        ImportOptions.bRebuildProcessed = bRebuildProcessed;
        ISCImporting* const Importer = ISCImporting::Get();
        if (!Importer)
        {
            FinishLoad(ESCLoadPhase::Failed, TEXT("this build has no importer (the SCBridgeImport plugin)"));
            return;
        }
        Job = Importer->NewJob(*Source, ImportOptions, Source);
        ImportStart = FPlatformTime::Seconds();
        Phase = ESCLoadPhase::Importing;
    }
    if (Phase == ESCLoadPhase::Importing)
    {
        if (!Job->Tick(bBlocking, ImportBudgetSeconds)) return;
        const FSCImportResult& Result = Job->GetResult();
        const FSCImportTimings& T = Job->GetTimings();
        Report.ImportSeconds = FPlatformTime::Seconds() - ImportStart;
        Report.DeriveSeconds = T.Derive;
        Report.BuildSeconds = T.Build;
        Report.CollisionSeconds = T.Collision;
        Report.MaterialSeconds = T.Materials;
        Report.AssembleSeconds = T.Assemble;
        Report.Assets = Result.MeshCount;
        Report.Instances = Result.InstanceCount;
        Report.CachedMeshes = Result.CachedMeshes;
        Report.MeshBuilds = Result.MeshBuilds;
        Report.CollisionBuilds = Result.CollisionBuilds;
        Report.Warnings = Result.Warnings.Num();
        const ESCImportStage Stage = Job->GetStage();
        const FString Error = Result.Error;
        if (Stage == ESCImportStage::Succeeded)
        {
            Vehicle = Result.Vehicle;
            USCPlacementLibrary::RestOnFloor(Vehicle.Get(), ViewerFloorZ);
            LoadedEntity = PendingEntity;
            LoadedShipLevel = PendingShipLevel;
            LoadedKey = PendingKey;
            if (VehicleInstances) VehicleInstances->SetPrimary(Vehicle.Get(), Job->GetRetainedAssets(), LoadedKey);
            Job.Reset();
        }
        else
        {
            // The job destroyed its partial actor; account for what it had created and prove GC releases it.
            TArray<TWeakObjectPtr<UObject>> Partial;
            TrackSession(Job->GetSpawnedVehicle(), Job->GetSessionPath(), Partial);
            Job.Reset(); // drains any remaining workers
            Report.PartialObjects = Partial.Num();
            Report.PartialObjectsAlive = CollectAndCount(Partial);
            UE_LOG(LogSCViewer, Display, TEXT("VIEWER_IMPORT_CLEANUP stage=%d meshes_built_at_cancel=%d workers_pending_at_cancel=%d tracked=%d alive_after_gc=%d"),
                   int32(Stage), Report.MeshesBuiltAtCancel, Report.bWorkersPendingAtCancel, Report.PartialObjects, Report.PartialObjectsAlive);
        }
        FinishLoad(Stage == ESCImportStage::Succeeded ? ESCLoadPhase::Succeeded
                   : Stage == ESCImportStage::Cancelled ? ESCLoadPhase::Cancelled
                                                        : ESCLoadPhase::Failed,
                   Error);
    }
}

void USCViewerSubsystem::FinishLoad(ESCLoadPhase Result, const FString& Error)
{
    bGarageLoad = false;
    bRebuildProcessed = false;
    Phase = Result;
    bLoadCancelRequested = false;
    Report.Result = Result;
    Report.Error = Error;
    Report.TotalSeconds = FPlatformTime::Seconds() - LoadStart;
    Report.StaticMeshes = CountLive<UStaticMesh>();
    Report.SkeletalMeshes = CountLive<USkeletalMesh>();
    Report.Textures = CountLive<UTexture2D>();
    Report.MaterialInstances = CountLive<UMaterialInstanceDynamic>();
    Report.Vehicles = CountLive<ASCVehicle>();
    ExportProgress.Reset();
    if (Result == ESCLoadPhase::Succeeded)
    {
        USCViewerSettings* Settings = USCViewerSettings::Get();
        Settings->AddRecent(Report.Entity);
        Settings->Save();
        LoadOptions->SaveConfig(); // last used options become the next session's defaults
        if (!Report.bReused) FocusComponent(nullptr);
    }
    UE_LOG(LogSCViewer, Display,
           TEXT("VIEWER_LOAD result=%s entity=%s reused=%d export_s=%.3f export_cache_hit=%d tar_bytes=%lld parse_s=%.3f import_s=%.3f "
                "derive_s=%.3f build_s=%.3f collision_s=%.3f materials_s=%.3f assemble_s=%.3f total_s=%.3f assets=%d instances=%d "
                "warnings=%d live_static_meshes=%d live_skeletal_meshes=%d live_textures=%d live_mids=%d live_vehicles=%d "
                "previous_objects=%d previous_alive=%d error=\"%s\""),
           *UEnum::GetValueAsString(Result), *Report.Entity, Report.bReused, Report.ExportSeconds, Report.bExportCacheHit, Report.TarBytes,
           Report.ParseSeconds, Report.ImportSeconds, Report.DeriveSeconds, Report.BuildSeconds, Report.CollisionSeconds,
           Report.MaterialSeconds, Report.AssembleSeconds, Report.TotalSeconds, Report.Assets, Report.Instances, Report.Warnings,
           Report.StaticMeshes, Report.SkeletalMeshes, Report.Textures, Report.MaterialInstances, Report.Vehicles, Report.PreviousObjects,
           Report.PreviousObjectsAlive, *Error);
    if (Result == ESCLoadPhase::Succeeded)
    {
        // The paint remembered for this vehicle (its palette id), when the package still offers it.
        if (ASCVehicle* V = Vehicle.Get())
            if (const FString* Saved = USCViewerSettings::Get()->Paints.Find(Report.Entity))
                if (const int32 Index = V->Manifest.Paints.IndexOfByPredicate([&](const FSCPaintVariant& P) { return P.PaletteId == *Saved; }); Index != INDEX_NONE)
                    V->SetPaint(Index);
        // ... with the custom colours saved for it.
        if (ASCVehicle* V = Vehicle.Get())
            if (const TMap<FString, FLinearColor> Colors = SavedPaintColors(Report.Entity, V->GetPaint() == INDEX_NONE ? FString() : V->Manifest.Paints[V->GetPaint()].PaletteId);
                !Colors.IsEmpty())
                V->SetPaintColors(Colors);
        ApplyStartSkin();
        FitEnvironmentToShip();
    }
    if (UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr)
    {
        SCLook::UpdateShortRangeAO(World);
        SCLook::RefreshGlassRefraction(World); // cooked glass loaded while the look is on
    }
    OnLoadFinished.Broadcast(Report);
}

void USCViewerSubsystem::StepCookedShip()
{
    if (Phase == ESCLoadPhase::Cancelled || Phase == ESCLoadPhase::Failed) return;
    if (ShipLevel->GetLevelStreamingState() == ELevelStreamingState::FailedToLoad)
    {
        const FString Name = ShipLevel->GetWorldAssetPackageName();
        UnloadShipLevel();
        FinishLoad(ESCLoadPhase::Failed, TEXT("the cooked ship level did not load: ") + Name);
        return;
    }
    ULevel* Level = ShipLevel->GetLoadedLevel();
    if (!Level || !ShipLevel->IsLevelVisible()) return;
    ASCVehicle* Cooked = nullptr;
    for (AActor* Actor : Level->Actors)
        if ((Cooked = Cast<ASCVehicle>(Actor)) != nullptr) break;
    if (!Cooked)
    {
        UnloadShipLevel();
        FinishLoad(ESCLoadPhase::Failed, TEXT("the cooked ship level holds no vehicle actor"));
        return;
    }
    // The entity asked for (a level name drops characters: another entity's level can answer a mistyped name).
    const FString Held = EntityName(Cooked->Manifest.Root.EntityName), Wanted = EntityName(PendingEntity);
    if (Held.IsEmpty() || !Held.Equals(Wanted, ESearchCase::IgnoreCase))
    {
        UnloadShipLevel();
        FinishLoad(ESCLoadPhase::Failed, FString::Printf(TEXT("the cooked ship level holds %s, not %s"), Held.IsEmpty() ? TEXT("no named entity") : *Held, *Wanted));
        return;
    }
    // Runtime-only state the cook does not keep (as for a cooked environment): the light shadow policy, dirt parameters.
    Vehicle = Cooked;
    Cooked->RefreshLightShadowPolicy();
    FSCDirt::Push(GetViewerWorld());
    Report.ImportSeconds = FPlatformTime::Seconds() - ImportStart;
    LoadedEntity = PendingEntity;
    LoadedShipLevel = PendingShipLevel;
    LoadedKey = PendingKey;
    // Extra instances of it stream their own instance of the level (FSCVehicleInstances, WP-O1-4b-3c): none retained here.
    if (VehicleInstances) VehicleInstances->SetPrimary(Cooked, nullptr, LoadedKey);
    FinishLoad(ESCLoadPhase::Succeeded, FString());
}
void USCViewerSubsystem::UnloadShipLevel()
{
    if (!ShipLevel) return;
    ReleaseLevel(ShipLevel);
    ShipLevel = nullptr;
}

void USCViewerSubsystem::UnloadInternal(bool bCollect, bool bKeepExtras, bool bForPendingLoad)
{
    UWorld* ViewerWorld = GetViewerWorld();
    if (!ViewerWorld || ViewerWorld->bIsTearingDown || IsEngineExitRequested())
    {
        // The world destroys its own actors while tearing down: no possession, spawning or destroying.
        FirstPerson = nullptr;
        InspectionPawn = nullptr;
        CameraMode = ESCCameraMode::Orbit;
    }
    else if (FirstPerson)
        SetCameraMode(ESCCameraMode::Orbit);
    IsolateComponent(nullptr);
    Focused = nullptr;
    ASCVehicle* Previous = Vehicle.Get();
    TArray<TWeakObjectPtr<UObject>> Tracked;
    // Extra vehicle instances and their retained assets go with the loaded vehicle (counted in the same accounting).
    if (VehicleInstances)
    {
        if (bKeepExtras) VehicleInstances->SetPrimary(nullptr, nullptr, FString());
        else if (const bool bWorldAlive = ViewerWorld && !ViewerWorld->bIsTearingDown && !IsEngineExitRequested(); bForPendingLoad)
            VehicleInstances->ResetForLoad(bCollect ? &Tracked : nullptr, bWorldAlive, LoadSerial);
        else VehicleInstances->Reset(bCollect ? &Tracked : nullptr, bWorldAlive);
    }
    if (ShipLevel)
    {
        // A cooked ship leaves with its level (its assets are packages, not an import session).
        UnloadShipLevel();
        Previous = nullptr;
    }
    if (Previous)
    {
        ForgetGeometry(Previous);
        const FString Session = bKeepExtras && VehicleInstances && VehicleInstances->RetainsSession(Previous->SessionPath)
            ? FString() : Previous->SessionPath;
        TrackSession(Previous, Session, Tracked);
        UWorld* World = Previous->GetWorld();
        // Without an importer nothing was imported: a prepared vehicle has nothing of its to unload.
        if (ISCImporting* const Importer = ISCImporting::Get(); World && Importer) Importer->Unload(World);
        // During engine exit the world tears its actors down itself.
        if (IsValid(Previous) && !Previous->IsActorBeingDestroyed() && World && !World->bIsTearingDown) Previous->Destroy();
    }
    Vehicle = nullptr;
    LoadedEntity.Empty();
    LoadedKey.Empty();
    LoadedShipLevel.Empty();
    if (!bCollect || Tracked.IsEmpty()) return;
    const int32 Alive = CollectAndCount(Tracked);
    Report.PreviousObjects = Tracked.Num();
    Report.PreviousObjectsAlive = Alive;
    UE_LOG(LogSCViewer, Display, TEXT("VIEWER_UNLOAD tracked=%d alive_after_gc=%d live_static_meshes=%d live_textures=%d live_mids=%d"),
           Tracked.Num(), Alive, CountLive<UStaticMesh>(), CountLive<UTexture2D>(), CountLive<UMaterialInstanceDynamic>());
}

void USCViewerSubsystem::PollClientPanic()
{
    // While an open is in progress a pending notice is left alone: the open retires the replaced session's notice
    // itself (ISCArchive::Open), so one still pending once its result is applied belongs to the new session.
    FText Error;
    if (ArchiveState == ESCArchiveState::Opening || !Client || !Client->TakePanic(Error)) return;
    ArchiveState = ESCArchiveState::Failed;
    ArchiveError = Error;
    ++ArchiveGeneration; // the next open is a new session; nothing from this one may be reused
    // The lists of the lost session go with it; the failure is the catalogue's answer (RPC /catalog environments_error).
    Catalog = FSCViewerCatalog();
    Catalog.Revision = ArchiveGeneration;
    Catalog.Error = Error.ToString();
    EnvironmentChoices.Reset();
    UE_LOG(LogSCViewer, Error, TEXT("VIEWER_ARCHIVE_LOST %s"), *Error.ToString());
}

bool USCViewerSubsystem::Tick(float)
{
    if (OpenTask.IsValid() && OpenTask.IsCompleted())
    {
        const FSCOpenResult Result = OpenTask.GetResult();
        OpenTask = {};
        OpenSeconds = Result.Seconds;
        ArchiveState = Result.bOK ? ESCArchiveState::Ready : ESCArchiveState::Failed;
        ArchiveBuildId = Result.ArchiveBuildId;
        ArchiveError = Result.Error;
        Catalog.Error = Result.bOK ? FString() : Result.Error.ToString(); // nothing will list after a failure: that is the answer
        if (Result.bOK)
        {
            CacheStats = Result.Cache;
            // Remember an explicitly chosen archive; auto-discovery stays implicit.
            USCViewerSettings* Settings = USCViewerSettings::Get();
            if (!ArchivePath.IsEmpty() && Settings->ArchivePath != ArchivePath)
            {
                Settings->ArchivePath = ArchivePath;
                Settings->Save();
            }
        }
        UE_LOG(LogSCViewer, Display, TEXT("VIEWER_ARCHIVE ok=%d seconds=%.3f index_cache_hit=%d cache_bytes=%llu cache_entries=%llu error=\"%s\""),
               Result.bOK, Result.Seconds, Result.bIndexCacheHit, CacheStats.Bytes, CacheStats.Entries, *Result.Error.ToString());
    }
    if (bBasicLightingPending && GetViewerWorld())
    {
        bBasicLightingPending = false; // after the game user settings applied their scalability at startup
        SCLook::ApplyBasicLighting();
    }
    if (bMaxQualityPending && GetViewerWorld())
    {
        bMaxQualityPending = false; // after the game user settings applied their scalability at startup
        SCLook::ApplyMaxQuality();
    }
    CompleteSearches(false);
    StepLoad(false);
    StepPlayerKit(false);
    StepEnvironment();
    if (VehicleInstances) VehicleInstances->Tick();
    PollClientPanic();
    LogPhaseChange();
    if (Automation) Automation->Tick();
    if (GeometryStreaming && !bGeometryCapturePrepared && !IsBusy() &&
        (KitState == EPlayerKitState::None || KitState == EPlayerKitState::Ready || KitState == EPlayerKitState::Failed))
        GeometryStreaming->Tick(GetViewerWorld());
    return true;
}
void USCViewerSubsystem::LogPhaseChange()
{
    // One line per step kind (counters stripped) so a log shows the pipeline moving through every phase.
    FString Step = IsBusy() ? GetLoadPhaseText() : FString();
    for (TCHAR& C : Step)
        if (FChar::IsDigit(C) || C == TEXT('/')) C = TEXT('#');
    if (Step == LastPhaseKey) return;
    LastPhaseKey = Step;
    if (!Step.IsEmpty())
        UE_LOG(LogSCViewer, Display, TEXT("VIEWER_PHASE t=%.3f progress=%.3f %s"), FPlatformTime::Seconds() - LoadStart, GetLoadProgress(),
               *GetLoadPhaseText());
}

// ---------------------------------------------------------------- player body (WP-P2)

bool USCViewerSubsystem::RequestPlayerKit()
{
    if (KitState != EPlayerKitState::None && KitState != EPlayerKitState::Failed) return true; // running or ready
    KitError.Empty();
    KitStart = FPlatformTime::Seconds();
    // A prepared body (WP-O1-4b-2): its level holds the kit actor with its rigs, kit and clips; nothing is exported or read.
    if (!PreparedPlayerLevel.IsEmpty())
    {
        bool bOK = false;
        UWorld* World = GetViewerWorld();
        KitLevel = World ? ULevelStreamingDynamic::LoadLevelInstance(World, PreparedPlayerLevel, FVector::ZeroVector, FRotator::ZeroRotator, bOK)
                         : nullptr;
        if (!bOK || !KitLevel)
        {
            KitLevel = nullptr;
            FailPlayerKit(TEXT("cannot stream the prepared player level ") + PreparedPlayerLevel);
            return false;
        }
        KitState = EPlayerKitState::Streaming;
        UE_LOG(LogSCViewer, Display, TEXT("PLAYER_KIT_START prepared=%s"), *PreparedPlayerLevel);
        return true;
    }
    // -SCPlayerPackage=<kit tar|export folder> reads a kit from disk (no archive needed: folder harness, tests).
    FString Package;
    if (FParse::Value(FCommandLine::Get(), TEXT("SCPlayerPackage="), Package))
    {
        Package = FPaths::ConvertRelativePathToFull(FPlatformMisc::LaunchDir(), Package);
        KitParse = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Package]() -> TSharedPtr<FSCTarPackageSource> {
            TArray64<uint8> Bytes;
            if (!FFileHelper::LoadFileToArray(Bytes, *Package)) return nullptr;
            return MakeShared<FSCTarPackageSource>(MoveTemp(Bytes));
        });
        KitState = EPlayerKitState::Parsing;
        UE_LOG(LogSCViewer, Display, TEXT("PLAYER_KIT_START package=%s"), *Package);
        return true;
    }
    if (ArchiveState != ESCArchiveState::Ready)
    {
        FailPlayerKit(CanReadArchive() ? TEXT("the archive is not open") : TEXT("the player body is not prepared and this build reads no archive"));
        return false;
    }
    const USCViewerSettings* Settings = USCViewerSettings::Get();
    const FString Gender = Settings->ResolvePlayerGender(), Loadout = Settings->ResolvePlayerLoadout();
    const TArray<FString> Weapons = Settings->ResolvePlayerWeapons();
    const bool bSpares = Settings->ResolvePlayerSpareAttachments();
    KitProgress = MakeShared<FSCExportProgress, ESPMode::ThreadSafe>();
    KitExport = Client->ExportPlayer(Gender, Loadout, Weapons, bSpares, KitProgress.ToSharedRef());
    KitState = EPlayerKitState::Exporting;
    UE_LOG(LogSCViewer, Display, TEXT("PLAYER_KIT_START gender=%s loadout=%s weapons=%s spares=%d"), Gender.IsEmpty() ? TEXT("<default>") : *Gender,
           Loadout.IsEmpty() ? TEXT("<exporter default>") : *Loadout, Weapons.IsEmpty() ? TEXT("<all>") : *FString::Join(Weapons, TEXT(",")), bSpares);
    return true;
}
void USCViewerSubsystem::FailPlayerKit(const FString& Error)
{
    KitState = EPlayerKitState::Failed;
    KitError = Error;
    KitJob.Reset();
    KitSource.Reset();
    KitProgress.Reset();
    // A rejected kit leaves nothing behind: its actor (meshes, materials, textures) and parsed data go, so a
    // retry starts clean and repeated failures stay bounded.
    ReleaseKitObjects();
    int32 LiveVehicles = 0;
    if (UWorld* World = GetViewerWorld())
        for (TActorIterator<ASCVehicle> It(World); It; ++It) LiveVehicles += IsValid(*It) && !It->IsActorBeingDestroyed();
    UE_LOG(LogSCViewer, Display, TEXT("PLAYER_KIT_RELEASED live_vehicles=%d (the loaded ship counts one)"), LiveVehicles);
    UE_LOG(LogSCViewer, Warning, TEXT("PLAYER_KIT result=Failed seconds=%.3f error=\"%s\" (first person stays capsule-only)"),
           FPlatformTime::Seconds() - KitStart, *Error);
    if (FirstPerson) FirstPerson->BodyStatus = GetPlayerKitStatus();
}
void USCViewerSubsystem::ReleaseKitObjects()
{
    if (KitActor && IsValid(KitActor))
    {
        if (FirstPerson && FirstPerson->Body->GetKit() == KitActor) FirstPerson->Body->Detach();
        KitActor->SetActorHiddenInGame(true);
        KitActor->Destroy();
    }
    KitActor = nullptr;
    UnloadKitLevel(); // a prepared body goes with its level
    KitData.Reset();
    KitClips.Reset();
}
void USCViewerSubsystem::ReleasePreparedPlayerKit()
{
    if (KitState != EPlayerKitState::Streaming && KitState != EPlayerKitState::Ready && KitState != EPlayerKitState::Failed) return;
    if (!KitLevel && KitState != EPlayerKitState::Failed && PreparedPlayerLevel.IsEmpty()) return; // a body from the archive stays
    ReleaseKitObjects();
    KitState = EPlayerKitState::None;
    KitError.Reset();
    UE_LOG(LogSCViewer, Display, TEXT("PLAYER_KIT_RELEASED prepared=%s"), *PreparedPlayerLevel);
    if (FirstPerson) FirstPerson->BodyStatus = GetPlayerKitStatus();
}
FString USCViewerSubsystem::GetPlayerKitStatus() const
{
    switch (KitState)
    {
    case EPlayerKitState::Exporting:
    {
        const FString Step = KitProgress ? KitProgress->GetPhase() : FString();
        return TEXT("Preparing your character…") + (Step.IsEmpty() ? FString() : TEXT(" (") + Step + TEXT(")"));
    }
    case EPlayerKitState::Parsing: return TEXT("Preparing your character…");
    case EPlayerKitState::Importing: return TEXT("Preparing your character…") + (KitJob ? TEXT(" (") + KitJob->GetPhase() + TEXT(")") : FString());
    case EPlayerKitState::Clips: return TEXT("Preparing your character… (animations)");
    case EPlayerKitState::Streaming: return TEXT("Preparing your character…");
    case EPlayerKitState::Ready: return TEXT("Character ready");
    case EPlayerKitState::Failed: return TEXT("Your character could not be prepared; you walk without one: ") + KitError;
    default: return FString();
    }
}
bool USCViewerSubsystem::WaitForPlayerKit(float TimeoutSeconds)
{
    const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
    while (KitState != EPlayerKitState::None && KitState != EPlayerKitState::Ready && KitState != EPlayerKitState::Failed &&
           FPlatformTime::Seconds() < Deadline)
        StepPlayerKit(true);
    return KitState == EPlayerKitState::Ready;
}
void USCViewerSubsystem::StepPreparedPlayerKit(bool bBlocking)
{
    if (!KitLevel || KitLevel->GetLevelStreamingState() == ELevelStreamingState::FailedToLoad)
    {
        FailPlayerKit(TEXT("the prepared player level did not load: ") + PreparedPlayerLevel);
        return;
    }
    if (bBlocking && !KitLevel->IsLevelVisible())
        if (UWorld* World = GetViewerWorld()) World->FlushLevelStreaming(EFlushLevelStreamingType::Full); // nothing ticks the world while it waits
    ULevel* Level = KitLevel->GetLoadedLevel();
    if (!Level || !KitLevel->IsLevelVisible()) return;
    ASCVehicle* Body = nullptr;
    for (AActor* Actor : Level->Actors)
        if (ASCVehicle* Candidate = Cast<ASCVehicle>(Actor); Candidate && Candidate->PlayerKit)
        {
            Body = Candidate;
            break;
        }
    if (!Body || !Body->PlayerKit->HasData())
    {
        FailPlayerKit(Body ? TEXT("the prepared player body's kit does not read: prepare the scenario again") : TEXT("the prepared player level holds no player body"));
        return;
    }
    KitActor = Body;
    KitActor->SetActorHiddenInGame(true); // unpublished until it is attached
    if (!USCPlayerBodyComponent::FindLeader(KitActor))
    {
        FailPlayerKit(TEXT("the prepared player body has no body rig"));
        return;
    }
    KitData = Body->PlayerKit->Kit;
    KitClips = Body->PlayerKit->Clips;
    KitState = EPlayerKitState::Ready;
    for (const FString& D : KitData->Diagnostics) UE_LOG(LogSCViewer, Display, TEXT("PLAYER_KIT_DIAGNOSTIC %s"), *D);
    UE_LOG(LogSCViewer, Display, TEXT("PLAYER_KIT result=Ready prepared=1 seconds=%.3f gender=%s loadout=%s weapons=%d states=%d blend_spaces=%d clips=%d rigs=%d"),
           FPlatformTime::Seconds() - KitStart, *KitData->Gender, *KitData->Loadout, KitData->Weapons.Num(), KitData->States.Num(), KitData->BlendSpaces.Num(),
           KitClips->Num(), KitActor->Rigs.Num());
    AttachPlayerKit();
}
void USCViewerSubsystem::UnloadKitLevel()
{
    if (!KitLevel) return;
    ReleaseLevel(KitLevel);
    KitLevel = nullptr;
}
void USCViewerSubsystem::ReleaseLevel(ULevelStreamingDynamic* Level)
{
    if (!Level) return;
    // A level that failed to load is past the streaming states a removal request is handled in: the world drops it now, else
    // each retry would leave one more streaming level behind.
    if (Level->GetLevelStreamingState() == ELevelStreamingState::FailedToLoad)
    {
        if (UWorld* World = Level->GetWorld()) World->RemoveStreamingLevel(Level);
        return;
    }
    Level->SetShouldBeLoaded(false);
    Level->SetShouldBeVisible(false);
    Level->SetIsRequestingUnloadAndRemoval(true);
    // Kept until the world removes it: a load still in flight that fails stops at FailedToLoad, where a removal request is no
    // longer handled (SweepReleasedKitLevels removes it then).
    ReleasedKitLevels.Add(Level);
}
void USCViewerSubsystem::SweepReleasedKitLevels()
{
    if (ReleasedKitLevels.IsEmpty()) return;
    UWorld* World = GetViewerWorld();
    ReleasedKitLevels.RemoveAll([World](const TObjectPtr<ULevelStreamingDynamic>& Level) {
        if (!Level || !World || !World->GetStreamingLevels().Contains(Level)) return true; // the world removed it
        if (Level->GetLevelStreamingState() != ELevelStreamingState::FailedToLoad) return false;
        World->RemoveStreamingLevel(Level);
        return true;
    });
}
void USCViewerSubsystem::StepPlayerKit(bool bBlocking)
{
    SweepReleasedKitLevels();
    if (KitState == EPlayerKitState::Streaming) StepPreparedPlayerKit(bBlocking);
    if (KitState == EPlayerKitState::Exporting)
    {
        if (!KitExport.IsCompleted())
        {
            if (!bBlocking) return;
            KitExport.Wait();
        }
        FSCExportResult Result = MoveTemp(KitExport.GetResult());
        KitExport = {};
        UE_LOG(LogSCViewer, Display, TEXT("PLAYER_KIT_EXPORT ok=%d seconds=%.3f bytes=%lld cache_hit=%d"), Result.bOK, Result.Seconds, Result.Tar.Num(),
               Result.Cache.bLastHit);
        if (!Result.bOK)
        {
            PollClientPanic();
            FailPlayerKit(Result.Error.ToString());
            return;
        }
        KitParse = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Tar = MoveTemp(Result.Tar)]() mutable {
            return TSharedPtr<FSCTarPackageSource>(MakeShared<FSCTarPackageSource>(MoveTemp(Tar)));
        });
        KitState = EPlayerKitState::Parsing;
        if (!bBlocking) return;
    }
    if (KitState == EPlayerKitState::Parsing)
    {
        if (!KitParse.IsCompleted())
        {
            if (!bBlocking) return;
            KitParse.Wait();
        }
        KitSource = KitParse.GetResult();
        KitParse = {};
        if (!KitSource || !KitSource->IsValid())
        {
            FailPlayerKit(KitSource ? KitSource->GetError() : FString(TEXT("cannot read the player package")));
            return;
        }
        const FString Scene = FSCPlayerKit::FindRootScene(*KitSource);
        if (Scene.IsEmpty())
        {
            FailPlayerKit(TEXT("the package has no single root scene.json"));
            return;
        }
        ISCImporting* const Importer = ISCImporting::Get();
        if (!Importer)
        {
            FailPlayerKit(TEXT("this build has no importer (the SCBridgeImport plugin)"));
            return;
        }
        KitJob = Importer->NewJob(*KitSource, USCPlayerBodyComponent::KitImportOptions(GetViewerWorld(), Scene), KitSource);
        KitState = EPlayerKitState::Importing;
    }
    if (KitState == EPlayerKitState::Importing)
    {
        if (!KitJob->Tick(bBlocking, ImportBudgetSeconds)) return;
        if (KitJob->GetStage() != ESCImportStage::Succeeded)
        {
            FailPlayerKit(KitJob->GetResult().Error);
            return;
        }
        KitActor = KitJob->GetResult().Vehicle.Get();
        if (KitActor) KitActor->SetActorHiddenInGame(true); // unpublished: never visible before it is validated
        UE_LOG(LogSCViewer, Display, TEXT("PLAYER_KIT_IMPORT seconds=%.3f rigs=%d meshes=%d warnings=%d"), KitJob->GetResult().TotalSeconds,
               KitActor ? KitActor->Rigs.Num() : 0, KitActor ? KitActor->Meshes.Num() : 0, KitJob->GetResult().Warnings.Num());
        KitJob.Reset();
        USCRigComponent* Leader = USCPlayerBodyComponent::FindLeader(KitActor);
        if (!Leader)
        {
            FailPlayerKit(TEXT("the package has no body rig"));
            return;
        }
        KitActor->SetActorHiddenInGame(true);
        KitClipsTask = UE::Tasks::Launch(UE_SOURCE_LOCATION,
            [Source = KitSource, Scene = KitActor->Manifest.ScenePath, Bones = USCPlayerBodyComponent::BoneMap(Leader)] {
                FKitClips Out;
                if (!SCLoadPlayerKit(*Source, Scene, Bones, Out.Kit, Out.Clips, Out.Failed, Out.Error)) Out.Kit.Reset();
                return Out;
            });
        KitState = EPlayerKitState::Clips;
    }
    if (KitState == EPlayerKitState::Clips)
    {
        if (!KitClipsTask.IsCompleted())
        {
            if (!bBlocking) return;
            KitClipsTask.Wait();
        }
        FKitClips Result = KitClipsTask.GetResult();
        KitClipsTask = {};
        // The personal lights' cookies (gobos) are built while the package is open; the body's torch takes them at attach.
        if (ISCImporting* const Importer = ISCImporting::Get(); Result.Kit && KitSource && KitActor && Importer)
            for (const FSCFlashlight& Light : Result.Kit->Flashlights)
                if (!Importer->LoadGobo(*KitSource, KitActor, Light.ProjectorTexture))
                    UE_LOG(LogSCViewer, Warning, TEXT("PLAYER_FLASHLIGHT gobo unavailable entity=%s cookie=%s package=%s"), *Light.Entity, *Light.Cookie,
                           *Light.ProjectorTexture);
        KitSource.Reset(); // the package bytes are no longer needed: meshes, textures, gobos and clips are built
        if (!Result.Kit)
        {
            FailPlayerKit(Result.Error);
            return;
        }
        KitData = Result.Kit;
        KitClips = Result.Clips;
        KitState = EPlayerKitState::Ready;
        for (const FString& D : KitData->Diagnostics) UE_LOG(LogSCViewer, Display, TEXT("PLAYER_KIT_DIAGNOSTIC %s"), *D);
        UE_LOG(LogSCViewer, Display, TEXT("PLAYER_KIT result=Ready seconds=%.3f gender=%s loadout=%s weapons=%d states=%d blend_spaces=%d clips=%d clip_failures=%d"),
               FPlatformTime::Seconds() - KitStart, *KitData->Gender, *KitData->Loadout, KitData->Weapons.Num(), KitData->States.Num(),
               KitData->BlendSpaces.Num(), KitClips->Num(), Result.Failed);
        AttachPlayerKit();
    }
    if (FirstPerson && !FirstPerson->Body->HasBody()) FirstPerson->BodyStatus = GetPlayerKitStatus();
}
void USCViewerSubsystem::AttachPlayerKit()
{
    if (!FirstPerson) return;
    FirstPerson->BodyStatus = GetPlayerKitStatus();
    if (KitState != EPlayerKitState::Ready || !KitActor || FirstPerson->Body->HasBody()) return;
    FString Error;
    if (!FirstPerson->Body->Attach(KitActor, KitData, KitClips, Error))
    {
        UE_LOG(LogSCViewer, Warning, TEXT("PLAYER_BODY attach failed: %s"), *Error);
        FailPlayerKit(Error); // capsule only; the kit actor and data are released
        return;
    }
    SCLook::RefreshGlassRefraction(GetViewerWorld()); // the kit's glass (optics, visor) takes the kit profile once it is the body's
}

/** WP-O1-3: -SCSkin=<index|default|name part> on the first vehicle the command line brings (a load or an adopted
 *  -SCPackage import), as sc.Paint. */
void USCViewerSubsystem::ApplyStartSkin()
{
    FString Skin;
    if (bStartSkinApplied || !Vehicle.IsValid() || !FParse::Value(FCommandLine::Get(), TEXT("SCSkin="), Skin)) return;
    bStartSkinApplied = true;
    int32 Index = INDEX_NONE;
    if (!ResolvePaint(Vehicle->Manifest.Paints, Skin, Index) || !SetPaint(Index))
        UE_LOG(LogSCViewer, Warning, TEXT("PAINT no paint \"%s\" for -SCSkin (sc.Paint lists them)"), *Skin);
}
bool USCViewerSubsystem::ResolvePaint(const TArray<FSCPaintVariant>& Paints, const FString& InSkin, int32& OutIndex)
{
    const FString Skin = InSkin.TrimStartAndEnd();
    OutIndex = INDEX_NONE;
    if (Skin.IsEmpty() || Skin.Equals(TEXT("default"), ESearchCase::IgnoreCase)) return true;
    OutIndex = Skin.IsNumeric() ? FCString::Atoi(*Skin)
        : Paints.IndexOfByPredicate([&](const FSCPaintVariant& P) { return P.Label().Contains(Skin) || P.PaletteId.Contains(Skin); });
    return Paints.IsValidIndex(OutIndex);
}

// ---------------------------------------------------------------- camera and inspection

void USCViewerSubsystem::AdoptPackageVehicle(ASCVehicle* Imported)
{
    if (!Imported || Imported->GetWorld() != GetViewerWorld()) return;
    Vehicle = Imported;
    LoadedEntity = EntityName(Imported->Manifest.Root.EntityName); // the manifest names the record; loads key the bare entity
    LoadedShipLevel.Empty();
    Report.Entity = LoadedEntity;
    Phase = Report.Result = ESCLoadPhase::Succeeded;
    if (VehicleInstances) VehicleInstances->SetPrimary(Imported, nullptr, FString()); // folder harness: instances import fresh
    ApplyStartSkin();
    FrameAll();
}

// ---------------------------------------------------------------- extra vehicle instances (SCVehicleInstances.cpp)

int32 USCViewerSubsystem::SpawnVehicleInstance(const FString& Entity)
{
    return VehicleInstances ? VehicleInstances->Spawn(Entity, true, FTransform::Identity, true) : INDEX_NONE;
}
int32 USCViewerSubsystem::SpawnVehicleInstance(const FString& Entity, const FTransform& World, bool bRest, const FString& Level)
{
    return VehicleInstances ? VehicleInstances->Spawn(Entity, false, World, bRest, false, Level) : INDEX_NONE;
}
bool USCViewerSubsystem::DestroyVehicleInstance(int32 Id) { return VehicleInstances && VehicleInstances->Destroy(Id); }
TArray<ASCVehicle*> USCViewerSubsystem::GetVehicles() const
{
    if (VehicleInstances) return VehicleInstances->GetVehicles();
    TArray<ASCVehicle*> Out;
    if (Vehicle.IsValid()) Out.Add(Vehicle.Get());
    return Out;
}
TArray<FSCVehicleInstanceInfo> USCViewerSubsystem::GetVehicleInstances() const
{
    return VehicleInstances ? VehicleInstances->GetInstances() : TArray<FSCVehicleInstanceInfo>();
}
int32 USCViewerSubsystem::GetVehicleInstanceId(const ASCVehicle* InVehicle) const
{
    return VehicleInstances ? VehicleInstances->GetId(InVehicle) : (InVehicle && InVehicle == Vehicle.Get() ? 0 : INDEX_NONE);
}
bool USCViewerSubsystem::IsSpawningVehicles() const { return VehicleInstances && VehicleInstances->IsSpawning(); }
int32 USCViewerSubsystem::GetVehiclesRevision() const { return VehicleInstances ? VehicleInstances->GetRevision() : 0; }
ASCFirstPersonCharacter* USCViewerSubsystem::GetFirstPerson() const
{
    return CameraMode == ESCCameraMode::FirstPerson ? FirstPerson.Get() : nullptr;
}
bool USCViewerSubsystem::SetCameraMode(ESCCameraMode Mode)
{
    if (Mode == CameraMode) return true;
    UWorld* World = GetViewerWorld();
    APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
    if (Mode == ESCCameraMode::FirstPerson)
    {
        if (!Vehicle.IsValid() || !Vehicle->bCollisionReady || !PC || IsBusy()) return false;
        InspectionPawn = GetViewerPawn();
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        FirstPerson = World->SpawnActor<ASCFirstPersonCharacter>(ASCFirstPersonCharacter::StaticClass(), FTransform::Identity, Params);
        if (!FirstPerson || !FirstPerson->Explore(Vehicle.Get()))
        {
            if (FirstPerson) FirstPerson->Destroy();
            FirstPerson = nullptr;
            UE_LOG(LogSCViewer, Warning, TEXT("FIRST_PERSON no safe spawn; choose a surface with Walk from here"));
            return false;
        }
        if (InspectionPawn.IsValid())
        {
            for (int32 Axis=0; Axis<3; ++Axis) InspectionPawn->SetMoveKey(Axis,0);
            InspectionPawn->SetActorTickEnabled(false);
        }
        PC->Possess(FirstPerson);
        // A start at an outside access (entrance, seat access, door, hull) facing away from the vehicle turns to it, level:
        // the player came to see the ship. Interior places and seats keep the spawn's own facing.
        FRotator View = FirstPerson->GetActorRotation();
        if (Vehicle->SpawnPoints.IsValidIndex(FirstPerson->CurrentSpawn))
        {
            const FName Kind = Vehicle->SpawnPoints[FirstPerson->CurrentSpawn].Kind;
            const FBox Bounds = Vehicle->GetComponentsBoundingBox(true);
            const FVector ToShip = Bounds.IsValid ? (Bounds.GetCenter() - FirstPerson->GetActorLocation()).GetSafeNormal2D() : FVector::ZeroVector;
            const bool bOutside = Kind != FSCSpawnKind::Interior && Kind != FSCSpawnKind::Seat;
            if (bOutside && !ToShip.IsZero() && FVector::DotProduct(View.Vector().GetSafeNormal2D(), ToShip) < 0)
                View = FRotator(0, ToShip.Rotation().Yaw, 0);
        }
        PC->SetControlRotation(View);
        RequestPlayerKit(); // first entry starts the player body; WP-P1 capsule-only until it is ready
        AttachPlayerKit();
    }
    else if (FirstPerson)
    {
        const FVector Position = FirstPerson->Camera->GetComponentLocation();
        const FRotator Rotation = PC ? PC->GetControlRotation() : FirstPerson->GetActorRotation();
        // Orbit around what the player was looking at, from the same viewpoint.
        FHitResult Look;
        FCollisionQueryParams LookParams(SCENE_QUERY_STAT(SCLeaveFirstPerson), true, FirstPerson);
        const double Pivot = World->LineTraceSingleByChannel(Look, Position, Position + Rotation.Vector() * 100000, ECC_Visibility, LookParams)
            ? Look.Distance : -1.0;
        FirstPerson->ClearInput();
        if (PC && InspectionPawn.IsValid()) PC->Possess(InspectionPawn.Get());
        FirstPerson->Destroy();
        FirstPerson = nullptr;
        if (InspectionPawn.IsValid())
        {
            InspectionPawn->SetActorTickEnabled(true);
            InspectionPawn->SetView(Position, Rotation, Pivot);
        }
    }
    CameraMode = Mode;
    if (ASCViewerPawn* Pawn = GetViewerPawn())
    {
        Pawn->SetOrbitObstacle(Environment.Get());
        Pawn->SetCameraMode(Mode);
    }
    UE_LOG(LogSCViewer, Display, TEXT("FIRST_PERSON_MODE mode=%d"), int32(Mode));
    return true;
}
bool USCViewerSubsystem::SpawnWhereLooking()
{
    ASCViewerPawn* Pawn = GetViewerPawn();
    if (!Pawn || !Vehicle.IsValid() || !Vehicle->bCollisionReady || IsBusy()) return false;
    const FVector Start = Pawn->Camera->GetComponentLocation();
    FHitResult Hit;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(SCSpawnLook), true, Pawn);
    if (!GetViewerWorld()->LineTraceSingleByChannel(Hit, Start, Start + Pawn->Camera->GetForwardVector() * 100000,
            ECC_Visibility, Query) || Hit.GetActor() != Vehicle.Get()) return false;
    const int32 Index = Vehicle->SetSelectedSpawn(FTransform(Hit.ImpactPoint));
    if (!SetCameraMode(ESCCameraMode::FirstPerson)) return false;
    return FirstPerson->TeleportToSpawn(Index);
}
void USCViewerSubsystem::FocusComponent(USceneComponent* Component)
{
    ASCVehicle* V = Vehicle.Get();
    if (!V) return;
    if (Component && Component->GetOwner() != V) Component = nullptr;
    Focused = Component;
    const FBox Box = SubtreeBounds(Component ? Component : V->GetRootComponent());
    if (ASCViewerPawn* Pawn = GetViewerPawn(); Pawn && Box.IsValid) Pawn->FocusBounds(Box);
    OnComponentFocused.Broadcast(Component);
}
void USCViewerSubsystem::FrameAll()
{
    IsolateComponent(nullptr); // restores the imported visibility first, so the whole vehicle is framed
    FocusComponent(nullptr);   // clears focus and broadcasts nullptr: the hierarchy drops selection and isolate
}
void USCViewerSubsystem::IsolateComponent(USceneComponent* Component)
{
    for (const auto& Pair : IsolationRestore)
        if (UPrimitiveComponent* P = Pair.Key.Get()) P->SetVisibility(Pair.Value);
    IsolationRestore.Empty();
    Isolated = nullptr;
    ASCVehicle* V = Vehicle.Get();
    if (!V || !Component || Component->GetOwner() != V) return;
    Isolated = Component;
    TArray<UPrimitiveComponent*> Primitives;
    V->GetComponents<UPrimitiveComponent>(Primitives);
    for (UPrimitiveComponent* P : Primitives)
    {
        IsolationRestore.Add(P, P->IsVisible());
        if (P != Component && !P->IsAttachedTo(Component)) P->SetVisibility(false);
    }
}

FSCKeepHandler& USCViewerSubsystem::KeepHandler()
{
    static FSCKeepHandler Handler;
    return Handler;
}
bool USCViewerSubsystem::CanKeep() const
{
    return KeepHandler().IsBound() && Vehicle.IsValid() && !IsBusy();
}
FSCKeepResult USCViewerSubsystem::Keep()
{
    if (!CanKeep())
    {
        FSCKeepResult Result;
        Result.Message = TEXT("Keep needs the editor host and a loaded entity");
        return Result;
    }
    return KeepHandler().Execute(Vehicle.Get(), LoadedEntity);
}

static FAutoConsoleCommandWithWorldAndArgs GSCEnvironment(
    TEXT("sc.Environment"), TEXT("sc.Environment <package folder|none>: import a game environment (e.g. a hangar socpak package) beside the ship"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(World)) Viewer->SetEnvironment(Args.IsValidIndex(0) ? FString::Join(Args, TEXT(" ")) : FString());
    }));

static FAutoConsoleCommandWithWorldAndArgs GSCPaint(
    TEXT("sc.Paint"), TEXT("sc.Paint [index|default|name part]: list the vehicle's paints, or put one on"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(World);
        ASCVehicle* V = Viewer ? Viewer->GetVehicle() : nullptr;
        if (!V) return;
        const TArray<FSCPaintVariant>& Paints = V->Manifest.Paints;
        if (Args.IsEmpty())
        {
            for (int32 I = 0; I < Paints.Num(); ++I)
                UE_LOG(LogSCViewer, Display, TEXT("PAINT_CHOICE index=%d label=\"%s\" palette=%s sidecar=%s active=%d"), I, *Paints[I].Label(), *Paints[I].PaletteId,
                       *Paints[I].ExteriorSidecar, V->GetPaint() == I);
            return;
        }
        const FString Arg = FString::Join(Args, TEXT(" "));
        const bool bDefault = Arg.Equals(TEXT("default"), ESearchCase::IgnoreCase);
        const int32 Index = Arg.IsNumeric() ? FCString::Atoi(*Arg) : bDefault ? INDEX_NONE
            : Paints.IndexOfByPredicate([&](const FSCPaintVariant& P) { return P.Label().Contains(Arg) || P.PaletteId.Contains(Arg); });
        if ((Index == INDEX_NONE && !bDefault) || !Viewer->SetPaint(Index))
            UE_LOG(LogSCViewer, Warning, TEXT("PAINT no paint \"%s\" (sc.Paint lists them)"), *Arg);
    }));
static FAutoConsoleCommandWithWorldAndArgs GSCEnvironmentState(
    TEXT("sc.Environment.State"), TEXT("sc.Environment.State <bind|open|closed>: pose the environment's mechanisms (doors, elevators)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(World)) Viewer->SetEnvironmentState(Args.IsValidIndex(0) ? Args[0] : FString());
    }));
