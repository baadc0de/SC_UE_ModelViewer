// USCViewerSubsystem: archive open, async search, the load pipeline with phase progress and cancel, unload with
// GC accounting, reuse by entity + options hash, camera mode/focus/isolate and the editor Keep hook.
#include "SCViewerSubsystem.h"
#include "EngineUtils.h"
#include "SCViewerAutomation.h"
#include "SCViewerPawn.h"
#include "SCFirstPersonCharacter.h"
#include "SCPlayerBody.h"
#include "SCRigComponent.h"
#include "Camera/CameraComponent.h"
#include "SCViewOptions.h"
#include "SCViewerSettings.h"
#include "SCImportJob.h"
#include "SCPackageSource.h"
#include "SCVehicle.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/IConsoleManager.h"
#include "Components/SkyLightComponent.h"
#include "Components/LightComponent.h"
#include "Engine/Light.h"
#include "Engine/StaticMeshActor.h"
#include "SCPlayerKit.h"
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
// Display convention shared with USCImportLibrary::ImportFolder: the model's lowest point sits this far above the
// Viewer floor plane (Z = 0). A presentation constant, not a game-data value.
constexpr double FloorClearanceCm = 100.0;
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
    Client = MakeShared<FSCBridgeClient, ESPMode::ThreadSafe>();
    CacheDir = USCViewerSettings::Get()->ResolveCacheDir();
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &USCViewerSubsystem::Tick));
    Automation = MakeShared<FSCViewerAutomation>(*this);
    // The -SCPackage folder harness (WP-U1..U3 review runs) imports from disk and needs no archive.
    FString HarnessPackage;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SCPackage="), HarnessPackage)) OpenArchive(FString());
    // Environment: -SCEnvironment wins; -SCDefaults (reproducible review routes) keeps the procedural hangar; else
    // the saved choice.
    FString EnvironmentArg;
    if (FParse::Value(FCommandLine::Get(), TEXT("SCEnvironment="), EnvironmentArg)) SetEnvironment(EnvironmentArg);
    else if (!FParse::Param(FCommandLine::Get(), TEXT("SCDefaults"))) SetEnvironment(USCViewerSettings::Get()->Environment);
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
    UnloadInternal(false); // the subsystem owns the loaded vehicle; the host collects garbage afterwards
    Client.Reset();
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
    if (ArchiveState == ESCArchiveState::Opening || IsBusy())
    {
        UE_LOG(LogSCViewer, Warning, TEXT("OpenArchive ignored: an open or load is in progress"));
        return;
    }
    ArchivePath = P4kPath.IsEmpty() ? USCViewerSettings::Get()->ResolveArchivePath() : P4kPath;
    ArchiveState = ESCArchiveState::Opening;
    ++ArchiveGeneration; // a different installation may follow: no reuse of loaded objects, no stale search results
    ArchiveError = FText();
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
        Result.Error = NSLOCTEXT("SCViewer", "ArchiveNotReady", "The archive is not open yet");
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
           bEnvironmentPending || EnvJob.IsValid() || EnvExport.IsValid() || EnvParse.IsValid();
}

void USCViewerSubsystem::SetEnvironment(const FString& Folder)
{
    if (EnvProgress) EnvProgress->RequestCancel();
    if (EnvExport.IsValid()) EnvExport.Wait();
    if (EnvParse.IsValid()) EnvParse.Wait();
    EnvExport = {};
    EnvParse = {};
    EnvJob.Reset();
    EnvSource.Reset();
    if (Environment.IsValid()) Environment->Destroy();
    Environment.Reset();
    EnvironmentFolder = Folder.TrimQuotes();
    bEnvironmentPending = !EnvironmentFolder.IsEmpty() && EnvironmentFolder != TEXT("none");
    if (!bEnvironmentPending) ShowMapHangar(true);
    UE_LOG(LogSCViewer, Display, TEXT("ENVIRONMENT request folder=\"%s\""), *EnvironmentFolder);
}

void USCViewerSubsystem::ShowMapHangar(bool bShow)
{
    UWorld* World = GetViewerWorld();
    if (!World) return;
    int32 Changed = 0;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* A = *It;
        if (Cast<ASCVehicle>(A) || A->GetLevel() != World->PersistentLevel) continue;
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
    EnvOptions.bReplacePrevious = false;
    EnvJob = MakeShared<FSCImportJob>(*EnvSource, EnvOptions, EnvSource);
}

void USCViewerSubsystem::StepEnvironment()
{
    // The archive's hangars for the picker: listed once per archive session.
    if (ArchiveState == ESCArchiveState::Ready && Client && EnvListedGeneration != ArchiveGeneration && !EnvListTask.IsValid())
        EnvListTask = Client->SearchEnvironments(TEXT("hangar"), 2000);
    if (EnvListTask.IsValid() && EnvListTask.IsCompleted())
    {
        const FSCTextResult Listed = EnvListTask.GetResult();
        EnvListTask = {};
        EnvListedGeneration = ArchiveGeneration;
        EnvironmentChoices.Reset();
        TArray<TSharedPtr<FJsonValue>> Values;
        if (Listed.bOK && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Listed.Json), Values))
            for (const auto& V : Values) EnvironmentChoices.Add(V->AsString());
        UE_LOG(LogSCViewer, Display, TEXT("ENVIRONMENT_CHOICES hangars=%d"), EnvironmentChoices.Num());
    }
    const bool bShipIdle = Phase != ESCLoadPhase::Exporting && Phase != ESCLoadPhase::Parsing && Phase != ESCLoadPhase::Importing;
    if (bEnvironmentPending && !EnvJob && !EnvExport.IsValid() && !EnvParse.IsValid() && bShipIdle && GetViewerWorld())
    {
        if (EnvironmentFolder.EndsWith(TEXT(".socpak"), ESearchCase::IgnoreCase))
        {
            if (ArchiveState != ESCArchiveState::Ready || !Client) return; // wait for the archive
            bEnvironmentPending = false;
            sc_options Ffi;
            LoadOptions->ToFfi(Ffi);
            Ffi.mip = FMath::Max<uint32>(Ffi.mip, 1u); // environments: half-resolution textures (tar under 2 GB)
            EnvProgress = MakeShared<FSCExportProgress, ESPMode::ThreadSafe>();
            EnvExport = Client->ExportEnvironment(EnvironmentFolder, Ffi, EnvProgress.ToSharedRef());
            EnvStart = FPlatformTime::Seconds();
            return;
        }
        bEnvironmentPending = false;
        EnvStart = FPlatformTime::Seconds();
        BeginEnvironmentImport(MakeShared<FSCFolderPackageSource>(EnvironmentFolder));
    }
    if (EnvExport.IsValid())
    {
        if (!EnvExport.IsCompleted()) return;
        FSCExportResult Exported = EnvExport.GetResult();
        EnvExport = {};
        if (!Exported.bOK)
        {
            UE_LOG(LogSCViewer, Error, TEXT("ENVIRONMENT failed: export %s: %s"), *EnvironmentFolder, *Exported.Error.ToString());
            return;
        }
        UE_LOG(LogSCViewer, Display, TEXT("ENVIRONMENT_EXPORT socpak=%s bytes=%d seconds=%.1f cache_hit=%d"), *EnvironmentFolder, Exported.Tar.Num(),
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
        UE_LOG(LogSCViewer, Display, TEXT("ENVIRONMENT_IMPORT seconds=%.1f meshes=%d instances=%d lights=%d bounds=%s"),
               FPlatformTime::Seconds() - EnvStart, Result.MeshCount, Result.InstanceCount, Environment->Lights.Num(), *Bounds.ToString());
        // Placement: the environment's main floor is the first large (>= 20 m across), upward-facing surface below
        // its origin (a top-loading hangar's origin is on its roof doors); it is moved to the map floor (MapFloorZ),
        // where the ship rests. Small objects on the way down (lights, cranes, props) are passed through.
        constexpr double MapFloorZ = -5.0, MinFloorSpan = 2000.0;
        FCollisionQueryParams Q(SCENE_QUERY_STAT(SCEnvFloor), true);
        if (Vehicle.IsValid()) Q.AddIgnoredActor(Vehicle.Get());
        double FloorZ = TNumericLimits<double>::Max(), From = -200.0;
        for (int32 Step = 0; Step < 64 && From > Bounds.Min.Z; ++Step)
        {
            FHitResult Hit;
            if (!GetViewerWorld()->LineTraceSingleByChannel(Hit, FVector(0, 0, From), FVector(0, 0, Bounds.Min.Z - 100), ECC_Visibility, Q)) break;
            const FBox Part = Hit.GetComponent() ? Hit.GetComponent()->Bounds.GetBox() : FBox(ForceInit);
            const FVector Size = Part.GetSize();
            UE_LOG(LogSCViewer, Display, TEXT("ENVIRONMENT_FLOOR_PROBE z=%.1f normal_z=%.2f span=%.0fx%.0f comp=%s"), Hit.ImpactPoint.Z, Hit.ImpactNormal.Z,
                   Size.X, Size.Y, Hit.GetComponent() && Hit.GetComponent()->ComponentTags.Num() ? *Hit.GetComponent()->ComponentTags[0].ToString() : TEXT("-"));
            if (Hit.ImpactNormal.Z > 0.9 && FMath::Min(Size.X, Size.Y) >= MinFloorSpan) { FloorZ = Hit.ImpactPoint.Z; break; }
            if (Hit.GetComponent()) Q.AddIgnoredComponent(Hit.GetComponent());
            From = Hit.ImpactPoint.Z;
        }
        if (FloorZ < TNumericLimits<double>::Max()) Environment->AddActorWorldOffset(FVector(0, 0, MapFloorZ - FloorZ));
        UE_LOG(LogSCViewer, Display, TEXT("ENVIRONMENT_PLACE floor_z=%.1f offset_z=%.1f"), FloorZ < TNumericLimits<double>::Max() ? FloorZ : 0.,
               FloorZ < TNumericLimits<double>::Max() ? MapFloorZ - FloorZ : 0.);
    }
    else UE_LOG(LogSCViewer, Error, TEXT("ENVIRONMENT failed: %s"), *Result.Error);
    EnvJob.Reset();
    EnvSource.Reset();
}
bool USCViewerSubsystem::LoadEntity(const FString& Entity, USCViewOptions* InOptions)
{
    if (Entity.IsEmpty() || IsBusy() || ArchiveState != ESCArchiveState::Ready)
    {
        UE_LOG(LogSCViewer, Warning, TEXT("LoadEntity(%s) refused: %s"), *Entity,
               Entity.IsEmpty() ? TEXT("no entity") : IsBusy() ? TEXT("a load is running") : TEXT("archive not open"));
        return false;
    }
    LoadOptions->CopyFrom(InOptions ? InOptions : Options.Get());
    Report = FSCLoadReport();
    Report.Entity = Entity;
    Report.Options = LoadOptions->Describe();
    PendingEntity = Entity;
    Report.ArchiveGeneration = int32(ArchiveGeneration);
    PendingKey = FString::Printf(TEXT("%u|%s|%s"), ArchiveGeneration, *Entity.ToLower(), *LoadOptions->GetHash());
    LoadStart = FPlatformTime::Seconds();
    UE_LOG(LogSCViewer, Display, TEXT("VIEWER_LOAD_START entity=%s options=\"%s\""), *Entity, *Report.Options);
    if (PendingKey == LoadedKey && Vehicle.IsValid())
    {
        Report.bReused = true; // identical request: keep the live objects, skip export and import
        FinishLoad(ESCLoadPhase::Succeeded, FString());
        return true;
    }
    sc_options Ffi;
    LoadOptions->ToFfi(Ffi);
    ExportProgress = MakeShared<FSCExportProgress, ESPMode::ThreadSafe>();
    ExportTask = Client->ExportDecomposed(Entity, Ffi, ExportProgress.ToSharedRef());
    Phase = ESCLoadPhase::Exporting;
    return true;
}
void USCViewerSubsystem::CancelLoad()
{
    if (!IsBusy()) return;
    Report.MeshesBuiltAtCancel = Job ? Job->GetResult().MeshStats.Num() : 0;
    Report.bWorkersPendingAtCancel = Job ? Job->HasPendingWorkers() : (ExportTask.IsValid() && !ExportTask.IsCompleted()) ||
                                                                          (ParseTask.IsValid() && !ParseTask.IsCompleted());
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
    const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
    while (IsBusy() && FPlatformTime::Seconds() < Deadline)
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
            Job->Tick(false, ImportBudgetSeconds);
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
    case ESCLoadPhase::Importing: return TEXT("Import: ") + (Job ? Job->GetPhase() : FString());
    case ESCLoadPhase::Succeeded: return Report.bReused ? TEXT("Reused loaded objects") : TEXT("Loaded");
    case ESCLoadPhase::Failed: return TEXT("Failed: ") + Report.Error;
    case ESCLoadPhase::Cancelled: return TEXT("Cancelled");
    }
    return FString();
}

void USCViewerSubsystem::StepLoad(bool bBlocking)
{
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
        // Free the previous ship before building the next one so peak memory holds one ship.
        UnloadInternal(true);
        FSCImportOptions ImportOptions;
        ImportOptions.World = GetViewerWorld();
        ImportOptions.bBuildCollision = LoadOptions->bCollision;
        Job = MakeShared<FSCImportJob>(*Source, ImportOptions, Source);
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
        Report.Warnings = Result.Warnings.Num();
        const ESCImportStage Stage = Job->GetStage();
        const FString Error = Result.Error;
        if (Stage == ESCImportStage::Succeeded)
        {
            Vehicle = Result.Vehicle;
            const FBox Bounds = Vehicle->GetComponentsBoundingBox(true);
            if (Bounds.IsValid) Vehicle->AddActorWorldOffset(FVector(0, 0, FloorClearanceCm - Bounds.Min.Z));
            LoadedEntity = PendingEntity;
            LoadedKey = PendingKey;
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
    Phase = Result;
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
    OnLoadFinished.Broadcast(Report);
}

void USCViewerSubsystem::UnloadInternal(bool bCollect)
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
    if (Previous)
    {
        TrackSession(Previous, Previous->SessionPath, Tracked);
        UWorld* World = Previous->GetWorld();
        if (World) FSCImporter::Unload(World);
        // During engine exit the world tears its actors down itself.
        if (IsValid(Previous) && !Previous->IsActorBeingDestroyed() && World && !World->bIsTearingDown) Previous->Destroy();
    }
    Vehicle = nullptr;
    LoadedEntity.Empty();
    LoadedKey.Empty();
    if (!bCollect || Tracked.IsEmpty()) return;
    const int32 Alive = CollectAndCount(Tracked);
    Report.PreviousObjects = Tracked.Num();
    Report.PreviousObjectsAlive = Alive;
    UE_LOG(LogSCViewer, Display, TEXT("VIEWER_UNLOAD tracked=%d alive_after_gc=%d live_static_meshes=%d live_textures=%d live_mids=%d"),
           Tracked.Num(), Alive, CountLive<UStaticMesh>(), CountLive<UTexture2D>(), CountLive<UMaterialInstanceDynamic>());
}

void USCViewerSubsystem::PollClientPanic()
{
    FText Error;
    if (!Client || !Client->TakePanic(Error)) return;
    ArchiveState = ESCArchiveState::Failed;
    ArchiveError = Error;
    ++ArchiveGeneration; // the next open is a new session; nothing from this one may be reused
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
        ArchiveError = Result.Error;
        if (Result.bOK)
        {
            CacheStats = Client->CacheStats();
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
    CompleteSearches(false);
    StepLoad(false);
    StepPlayerKit(false);
    StepEnvironment();
    PollClientPanic();
    LogPhaseChange();
    if (Automation) Automation->Tick();
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
    // -SCPlayerPackage=<kit tar|export folder> reads a kit from disk (no archive needed: folder harness, tests).
    FString Package;
    if (FParse::Value(FCommandLine::Get(), TEXT("SCPlayerPackage="), Package))
    {
        Package = FPaths::ConvertRelativePathToFull(FPlatformMisc::LaunchDir(), Package);
        KitParse = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Package]() -> TSharedPtr<FSCTarPackageSource> {
            TArray<uint8> Bytes;
            if (!FFileHelper::LoadFileToArray(Bytes, *Package)) return nullptr;
            return MakeShared<FSCTarPackageSource>(MoveTemp(Bytes));
        });
        KitState = EPlayerKitState::Parsing;
        UE_LOG(LogSCViewer, Display, TEXT("PLAYER_KIT_START package=%s"), *Package);
        return true;
    }
    if (ArchiveState != ESCArchiveState::Ready)
    {
        FailPlayerKit(TEXT("the archive is not open"));
        return false;
    }
    const USCViewerSettings* Settings = USCViewerSettings::Get();
    const FString Gender = Settings->ResolvePlayerGender(), Loadout = Settings->ResolvePlayerLoadout();
    KitProgress = MakeShared<FSCExportProgress, ESPMode::ThreadSafe>();
    KitExport = Client->ExportPlayer(Gender, Loadout, KitProgress.ToSharedRef());
    KitState = EPlayerKitState::Exporting;
    UE_LOG(LogSCViewer, Display, TEXT("PLAYER_KIT_START gender=%s loadout=%s"), Gender.IsEmpty() ? TEXT("<default>") : *Gender,
           Loadout.IsEmpty() ? TEXT("<exporter default>") : *Loadout);
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
    if (KitActor && IsValid(KitActor))
    {
        if (FirstPerson && FirstPerson->Body->GetKit() == KitActor) FirstPerson->Body->Detach();
        KitActor->SetActorHiddenInGame(true);
        KitActor->Destroy();
    }
    KitActor = nullptr;
    KitData.Reset();
    KitClips.Reset();
    int32 LiveVehicles = 0;
    if (UWorld* World = GetViewerWorld())
        for (TActorIterator<ASCVehicle> It(World); It; ++It) LiveVehicles += IsValid(*It) && !It->IsActorBeingDestroyed();
    UE_LOG(LogSCViewer, Display, TEXT("PLAYER_KIT_RELEASED live_vehicles=%d (the loaded ship counts one)"), LiveVehicles);
    UE_LOG(LogSCViewer, Warning, TEXT("PLAYER_KIT result=Failed seconds=%.3f error=\"%s\" (first person stays capsule-only)"),
           FPlatformTime::Seconds() - KitStart, *Error);
    if (FirstPerson) FirstPerson->BodyStatus = GetPlayerKitStatus();
}
FString USCViewerSubsystem::GetPlayerKitStatus() const
{
    switch (KitState)
    {
    case EPlayerKitState::Exporting:
    {
        const FString Step = KitProgress ? KitProgress->GetPhase() : FString();
        return TEXT("Player body: exporting") + (Step.IsEmpty() ? FString(TEXT("…")) : TEXT(" (") + Step + TEXT(")"));
    }
    case EPlayerKitState::Parsing: return TEXT("Player body: reading package…");
    case EPlayerKitState::Importing: return TEXT("Player body: ") + (KitJob ? KitJob->GetPhase() : FString(TEXT("importing")));
    case EPlayerKitState::Clips: return TEXT("Player body: reading animations…");
    case EPlayerKitState::Ready: return TEXT("Player body ready");
    case EPlayerKitState::Failed: return TEXT("Player body unavailable (capsule only): ") + KitError;
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
void USCViewerSubsystem::StepPlayerKit(bool bBlocking)
{
    if (KitState == EPlayerKitState::Exporting)
    {
        if (!KitExport.IsCompleted())
        {
            if (!bBlocking) return;
            KitExport.Wait();
        }
        FSCExportResult Result = MoveTemp(KitExport.GetResult());
        KitExport = {};
        UE_LOG(LogSCViewer, Display, TEXT("PLAYER_KIT_EXPORT ok=%d seconds=%.3f bytes=%d cache_hit=%d"), Result.bOK, Result.Seconds, Result.Tar.Num(),
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
        KitJob = MakeShared<FSCImportJob>(*KitSource, USCPlayerBodyComponent::KitImportOptions(GetViewerWorld(), Scene), KitSource);
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
        KitSource.Reset(); // the package bytes are no longer needed: meshes, textures and clips are built
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
    }
}

// ---------------------------------------------------------------- camera and inspection

void USCViewerSubsystem::AdoptPackageVehicle(ASCVehicle* Imported)
{
    if (!Imported || Imported->GetWorld() != GetViewerWorld()) return;
    Vehicle = Imported;
    LoadedEntity = Imported->Manifest.Root.EntityName;
    Report.Entity = LoadedEntity;
    Phase = Report.Result = ESCLoadPhase::Succeeded;
    FrameAll();
}
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
            UE_LOG(LogSCViewer, Warning, TEXT("FIRST_PERSON no safe spawn; choose a surface with Spawn where I look"));
            return false;
        }
        if (InspectionPawn.IsValid())
        {
            for (int32 Axis=0; Axis<3; ++Axis) InspectionPawn->SetMoveKey(Axis,0);
            InspectionPawn->SetActorTickEnabled(false);
        }
        PC->Possess(FirstPerson);
        PC->SetControlRotation(FirstPerson->GetActorRotation());
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
    if (ASCViewerPawn* Pawn = GetViewerPawn()) Pawn->SetCameraMode(Mode);
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
