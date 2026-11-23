// USCPrepareCommandlet (SCPrepareCommandlet.h): a scenario's environment and vehicle prepared as levels.
#include "SCPrepareCommandlet.h"
#include "SCCookLibrary.h"
#include "SCHangarIndex.h"
#include "SCImporter.h"
#include "SCPackageReader.h"
#include "SCPackageSource.h"
#include "SCPlayerKit.h"
#include "SCPrepareAudio.h"
#include "SCVehicle.h"
#include "SCViewOptions.h"
#include "SCViewerSubsystem.h"
#include "UCScenario.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "LevelEditorSubsystem.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCPrepare, Log, All);

namespace
{
constexpr double FloorZ = -5.0;             // USCViewerSubsystem's floor: the prepared levels stream in at the origin
constexpr float ArchiveTimeoutSeconds = 900.f; // a first open builds the archive's index

FString StampFile(const FString& Level)
{
    return FUCPrepared::StampFile(Level);
}
/** The level exists and was prepared in the current format (its stamp is written only once it saved). */
bool Current(const FString& Level)
{
    return FUCPrepared::IsLevelCurrent(Level);
}

/** The same paint in two manifests (the package read before the import and the imported actor's). */
bool SamePaint(const FSCPaintVariant& A, const FSCPaintVariant& B)
{
    return A.PaletteId == B.PaletteId && A.SubgeometryTag == B.SubgeometryTag && A.DisplayName == B.DisplayName;
}
/** A paint's level variant: its palette id (else its subgeometry tag, else its name) for reading, and a hash of all three,
 *  so variants whose names differ only in characters a level name drops stay apart (USCViewerSubsystem::ShipLevelPath). */
FString PaintVariant(const FSCPaintVariant& Paint)
{
    const FString Name = !Paint.PaletteId.IsEmpty() ? Paint.PaletteId : !Paint.SubgeometryTag.IsEmpty() ? Paint.SubgeometryTag : Paint.DisplayName;
    const uint32 Hash = FCrc::StrCrc32(*(Paint.PaletteId + TEXT("|") + Paint.SubgeometryTag + TEXT("|") + Paint.DisplayName));
    return FString::Printf(TEXT("%s_%08x"), *Name, Hash);
}

class FPrepare
{
public:
    bool bForce = false;
    USCViewerSubsystem* Viewer = nullptr;

    /** The archive, opened once on first need. */
    bool OpenArchive(FString& Error)
    {
        if (Viewer) return true;
        Viewer = USCViewerSubsystem::CreateHeadless();
        if (!Viewer)
        {
            Error = TEXT("no headless viewer to open the archive with");
            return false;
        }
        Viewer->SetEnvironment(TEXT("none")); // the headless viewer restores no saved environment of its own
        UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE opening the archive (%s)"), *Viewer->GetArchiveLabel());
        if (!Viewer->WaitForArchive(ArchiveTimeoutSeconds))
        {
            if (Viewer->GetArchiveState() == ESCArchiveState::Opening)
            {
                // The open cannot be cancelled and the viewer's teardown waits for it: leave the process at once.
                UE_LOG(LogSCPrepare, Error, TEXT("SC_PREPARE_FAILED the archive did not open within %.0f s"), ArchiveTimeoutSeconds);
                GLog->Flush();
                FPlatformMisc::RequestExitWithStatus(true, 1);
            }
            Error = TEXT("the archive did not open: ") + Viewer->GetArchiveError();
            return false;
        }
        return true;
    }

    /** One package from the archive: an environment (an object container) or an entity, at full detail. */
    TSharedPtr<FSCTarPackageSource> Export(bool bEnvironment, const FString& What, FString& Error)
    {
        if (!OpenArchive(Error)) return nullptr;
        USCViewOptions* Options = NewObject<USCViewOptions>(GetTransientPackage());
        Options->ResetToDefaults();
        sc_options Ffi;
        Options->ToFfi(Ffi);
        Ffi.lod = 0;
        Ffi.mip = 0; // full resolution: the shared texture pool keys a texture by its source, not by a resolution
        const TSharedRef<FSCExportProgress, ESPMode::ThreadSafe> Progress = MakeShared<FSCExportProgress, ESPMode::ThreadSafe>();
        const TSharedPtr<FSCBridgeClient, ESPMode::ThreadSafe> Client = Viewer->GetClient();
        UE::Tasks::TTask<FSCExportResult> Task = bEnvironment ? Client->ExportEnvironment(What, Ffi, Progress) : Client->ExportDecomposed(What, Ffi, Progress);
        while (!Task.Wait(FTimespan::FromSeconds(15)))
            UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE exporting %s: %s %.0f%%"), *What, *Progress->GetPhase(), Progress->GetFraction() * 100.f);
        FSCExportResult Result = MoveTemp(Task.GetResult());
        if (!Result.bOK)
        {
            Error = FString::Printf(TEXT("export %s: %s"), *What, *Result.Error.ToString());
            return nullptr;
        }
        UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE exported %s bytes=%lld seconds=%.1f cache_hit=%d"), *What, Result.Tar.Num(), Result.Seconds,
               Result.Cache.bLastHit);
        TSharedPtr<FSCTarPackageSource> Source = MakeShared<FSCTarPackageSource>(MoveTemp(Result.Tar));
        if (!Source->IsValid())
        {
            Error = FString::Printf(TEXT("export %s: %s"), *What, *Source->GetError());
            return nullptr;
        }
        return Source;
    }

    /** A new, empty level at Level in the editor world. An older one is deleted first with its materials (Root; the shared
     *  pool stays), and its stamp is removed until the new one saves. */
    UWorld* NewLevel(const FString& Level, const FString& Root, FString& Error)
    {
        IFileManager& Files = IFileManager::Get();
        // The stamp goes first: from here until the new level saved, no record can run this level.
        Files.Delete(*StampFile(Level), false, true, true);
        if (Files.FileExists(*StampFile(Level)))
        {
            Error = FString::Printf(TEXT("cannot delete %s: delete it and run again"), *StampFile(Level));
            return nullptr;
        }
        // The older level and its materials go by their files (nothing has loaded them: loading a level to delete it would
        // load its whole pool), then the asset registry forgets them (it lists what it scanned at start).
        if (FindPackage(nullptr, *Level))
        {
            Error = FString::Printf(TEXT("the older %s is loaded: it cannot be replaced in this process"), *Level);
            return nullptr;
        }
        const FString LevelFile = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(Level, FPackageName::GetMapPackageExtension()));
        const FString RootFolder = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(Root + TEXT("/")));
        // The level's file is rescanned also when an earlier run removed it and the registry still lists it.
        TArray<FString> Removed = {LevelFile};
        Files.FindFilesRecursive(Removed, *RootFolder, TEXT("*.uasset"), true, false, false);
        const bool bOlder = Files.FileExists(*LevelFile) || Removed.Num() > 1;
        if ((Files.FileExists(*LevelFile) && !Files.Delete(*LevelFile, false, true, true)) ||
            (Files.DirectoryExists(*RootFolder) && !Files.DeleteDirectory(*RootFolder, false, true)))
        {
            Error = FString::Printf(TEXT("cannot delete the older %s or %s: delete them and run again"), *LevelFile, *RootFolder);
            return nullptr;
        }
        IAssetRegistry::GetChecked().ScanModifiedAssetFiles(Removed);
        if (bOlder) UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE removed the older %s and its materials (%d files)"), *Level, Removed.Num());
        ULevelEditorSubsystem* Levels = GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
        if (!Levels || !Levels->NewLevel(Level, false))
        {
            Error = TEXT("cannot create the level ") + Level;
            return nullptr;
        }
        return GEditor->GetEditorWorldContext().World();
    }

    /** Audio staged, the actor cooked, the level saved and stamped. */
    bool Finish(ASCVehicle* Actor, const FString& Level, const FString& Root, const FString& Shared, const FSCPackageSource& Source,
                const FString& Scene, FString& Error)
    {
        const FString Name = FPackageName::GetShortName(Level);
        const FString AudioFolder = TEXT("SCAudio/") + Name;
        int32 AudioFiles = 0;
        if (!SCPrepareAudio::Stage(Source, Scene, FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / AudioFolder), AudioFiles, Error)) return false;
        Actor->CookedAudio = AudioFolder;
        Actor->CookedAudioScene = Scene;
        if (!USCCookLibrary::CookEnvironment(Actor, Root, Shared, Viewer ? Viewer->GetArchiveBuildId() : FString()))
        {
            Error = TEXT("the cook of ") + Level + TEXT(" did not save every asset");
            return false;
        }
        ULevelEditorSubsystem* Levels = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
        if (!Levels->SaveCurrentLevel())
        {
            Error = TEXT("cannot save ") + Level;
            return false;
        }
        // The stamp is published whole: written beside it first, then moved into place (a level counts as current only with
        // a complete stamp, FUCPrepared::IsLevelCurrent).
        const FString Stamp = StampFile(Level), Partial = Stamp + TEXT(".partial");
        IFileManager& Files = IFileManager::Get();
        if (!FFileHelper::SaveStringToFile(FString::Printf(TEXT("%d\n%s\n"), FUCPrepared::FormatVersion, *Viewer->GetArchiveBuildId()), *Partial) ||
            !Files.Move(*Stamp, *Partial, true, true))
        {
            Files.Delete(*Partial, false, true, true);
            Files.Delete(*Stamp, false, true, true);
            Error = TEXT("cannot write ") + Stamp;
            return false;
        }
        UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE saved level=%s audio_files=%d format=%d"), *Level, AudioFiles, FUCPrepared::FormatVersion);
        return true;
    }

    bool PrepareEnvironment(const FString& Container, const FString& Level, FString& Error)
    {
        const TSharedPtr<FSCTarPackageSource> Source = Export(true, Container, Error);
        if (!Source) return false;
        const FString Scene = FSCPlayerKit::FindRootScene(*Source);
        if (Scene.IsEmpty())
        {
            Error = Container + TEXT(": no single root scene.json in the export");
            return false;
        }
        const FString Name = FPackageName::GetShortName(Level);
        UWorld* World = NewLevel(Level, TEXT("/Game/SC/Hangars/") + Name, Error);
        if (!World) return false;
        ASCVehicle* Environment = USCImportLibrary::ImportEnvironmentFrom(World, *Source, Scene);
        if (!Environment)
        {
            Error = Container + TEXT(": the import failed (IMPORT_FAILED in the log)");
            return false;
        }
        Environment->SetActorLabel(TEXT("Environment ") + Name);
        const FVector Offset = USCImportLibrary::PresentEnvironment(Environment, TEXT("open"), FloorZ);
        const FString Path = Container.Replace(TEXT("\\"), TEXT("/"));
        bool bMeasured = false;
        if (!USCCookLibrary::RecordHangarCapacity(Environment, FPaths::GetCleanFilename(FPaths::GetPath(Path)), FPaths::GetBaseFilename(Path), bMeasured))
        {
            Error = TEXT("cannot save the hangar index ") + FString(USCHangarIndex::AssetPath);
            return false;
        }
        UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE environment %s offset=%s capacity_recorded=%d"), *Container, *Offset.ToCompactString(), bMeasured);
        return Finish(Environment, Level, TEXT("/Game/SC/Hangars/") + Name, TEXT("/Game/SC/Hangars/Shared"), *Source, Scene, Error);
    }

    /** The vehicle's level with the scenario's paint (the variant names it); Level is decided once the package lists its
     *  paints. */
    bool PrepareVehicle(const FUCScenario& Scenario, FString& Level, FString& Error)
    {
        const TSharedPtr<FSCTarPackageSource> Source = Export(false, Scenario.Vehicle, Error);
        if (!Source) return false;
        const FString Scene = FSCPlayerKit::FindRootScene(*Source);
        if (Scene.IsEmpty())
        {
            Error = Scenario.Vehicle + TEXT(": no single root scene.json in the export");
            return false;
        }
        // The paints as the import reads them (FSCPackageReader leaves out a variant whose files are unavailable): a skin
        // names the same paint here as -SCSkin does on the imported vehicle.
        FSCSceneManifest Manifest;
        if (!FSCPackageReader::Read(*Source, Manifest, Error, Scene))
        {
            Error = Scenario.Vehicle + TEXT(": ") + Error;
            return false;
        }
        const TArray<FSCPaintVariant>& Paints = Manifest.Paints;
        int32 Paint = INDEX_NONE;
        if (!USCViewerSubsystem::ResolvePaint(Paints, Scenario.Skin, Paint))
        {
            TArray<FString> Names;
            for (const FSCPaintVariant& P : Paints) Names.Add(FString::Printf(TEXT("%s (%s)"), *P.Label(), *P.PaletteId));
            Error = FString::Printf(TEXT("%s has no paint \"%s\"; its paints: %s"), *Scenario.Vehicle, *Scenario.Skin,
                                    Names.IsEmpty() ? TEXT("none") : *FString::Join(Names, TEXT(", ")));
            return false;
        }
        Level = USCViewerSubsystem::ShipLevelPath(Scenario.Vehicle, Paint == INDEX_NONE ? FString() : PaintVariant(Paints[Paint]));
        if (Current(Level) && !bForce)
        {
            UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE kept %s (prepared in format %d)"), *Level, FUCPrepared::FormatVersion);
            return true;
        }
        const FString Name = FPackageName::GetShortName(Level);
        UWorld* World = NewLevel(Level, TEXT("/Game/SC/Ships/") + Name, Error);
        if (!World) return false;
        ASCVehicle* Ship = USCImportLibrary::ImportVehicleFrom(World, *Source, true, Scene);
        if (!Ship)
        {
            Error = Scenario.Vehicle + TEXT(": the import failed (IMPORT_FAILED in the log)");
            return false;
        }
        Ship->SetActorLabel(TEXT("Ship ") + Name);
        // The same paint on the imported actor.
        const int32 Imported = Paint == INDEX_NONE ? INDEX_NONE
            : Ship->Manifest.Paints.IndexOfByPredicate([&](const FSCPaintVariant& P) { return SamePaint(P, Paints[Paint]); });
        if (Paint != INDEX_NONE && (Imported == INDEX_NONE || !Ship->SetPaint(Imported)))
        {
            Error = FString::Printf(TEXT("%s: the paint \"%s\" (%s) did not go on"), *Scenario.Vehicle, *Scenario.Skin, *Paints[Paint].Label());
            return false;
        }
        const int32 GearClips = USCImportLibrary::RestOnFloor(Ship, FloorZ);
        UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE vehicle %s paint=%s gear_clips=%d"), *Scenario.Vehicle,
               Paint == INDEX_NONE ? TEXT("default") : *Paints[Paint].Label(), GearClips);
        return Finish(Ship, Level, TEXT("/Game/SC/Ships/") + Name, TEXT("/Game/SC/Ships/Shared"), *Source, Scene, Error);
    }
};

int32 Failed(const FString& Id, const FString& Error)
{
    UE_LOG(LogSCPrepare, Error, TEXT("SC_PREPARE_FAILED scenario=%s %s"), *Id, *Error);
    return 1;
}
} // namespace

USCPrepareCommandlet::USCPrepareCommandlet()
{
    // A client: the levels it builds need a render scene (skeletal meshes, runtime textures, Nanite builds).
    IsClient = true;
    IsEditor = true;
    LogToConsole = true;
    ShowErrorCount = true;
}

int32 USCPrepareCommandlet::Main(const FString& Params)
{
    FString Id;
    if (!FParse::Value(*Params, TEXT("Scenario="), Id) || Id.IsEmpty())
    {
        UE_LOG(LogSCPrepare, Error, TEXT("SC_PREPARE_FAILED no -Scenario=<name> (a file under Config/Scenarios, without .json)"));
        return 1;
    }
    FUCScenario Scenario;
    FString Error;
    if (!FUCScenario::Load(Id, Scenario, Error)) return Failed(Id, Error);
    if (!IsAllowCommandletRendering())
        return Failed(Id, TEXT("the prep step needs a renderer for runtime textures and Nanite builds: add -AllowCommandletRendering"));
    FPrepare Prepare;
    // The archive's headless viewer goes before the process exits, also after a failure (its world's subsystems).
    ON_SCOPE_EXIT
    {
        if (Prepare.Viewer) USCViewerSubsystem::DestroyHeadless(Prepare.Viewer);
    };
    Prepare.bForce = FParse::Param(*Params, TEXT("Force"));

    FUCPrepared Record;
    Record.Format = FUCPrepared::FormatVersion;
    Record.Scenario = Id;
    Record.Definition = Scenario.Json;
    Record.CargoDefinition = Scenario.CargoJson;
    const double Start = FPlatformTime::Seconds();
    if (!Scenario.Environment.IsEmpty())
    {
        Record.EnvironmentLevel = USCViewerSubsystem::EnvironmentLevelPath(Scenario.Environment);
        if (Current(Record.EnvironmentLevel) && !Prepare.bForce)
        {
            UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE kept %s (prepared in format %d)"), *Record.EnvironmentLevel, FUCPrepared::FormatVersion);
        }
        else if (!Prepare.PrepareEnvironment(Scenario.Environment, Record.EnvironmentLevel, Error))
        {
            return Failed(Id, Error);
        }
    }
    // A named paint's level is known only from the package's paints (PrepareVehicle exports it, then keeps a current level).
    const bool bDefaultPaint = Scenario.Skin.IsEmpty() || Scenario.Skin.Equals(TEXT("default"), ESearchCase::IgnoreCase);
    const FString Known = bDefaultPaint ? USCViewerSubsystem::ShipLevelPath(Scenario.Vehicle) : FString();
    if (Current(Known) && !Prepare.bForce)
    {
        Record.VehicleLevel = Known;
        UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE kept %s (prepared in format %d)"), *Known, FUCPrepared::FormatVersion);
    }
    else if (!Prepare.PrepareVehicle(Scenario, Record.VehicleLevel, Error))
    {
        return Failed(Id, Error);
    }
    // The archive builds the levels were prepared from (each level's stamp names its own).
    TArray<FString> Builds;
    for (const FString& Level : {Record.EnvironmentLevel, Record.VehicleLevel})
        if (const FString Build = FUCPrepared::LevelBuild(Level); !Level.IsEmpty()) Builds.AddUnique(Build.IsEmpty() ? FString(TEXT("unknown")) : Build);
    Record.Archive = FString::Join(Builds, TEXT(", "));
    Record.PreparedAt = FDateTime::UtcNow().ToIso8601();
    IFileManager::Get().MakeDirectory(*FUCPrepared::Directory(), true);
    if (!Record.Save(Error)) return Failed(Id, Error);
    UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE_DONE scenario=%s environment=%s vehicle=%s record=%s seconds=%.0f"), *Id,
           Record.EnvironmentLevel.IsEmpty() ? TEXT("none") : *Record.EnvironmentLevel, *Record.VehicleLevel, *FUCPrepared::File(Id),
           FPlatformTime::Seconds() - Start);
    return 0;
}
