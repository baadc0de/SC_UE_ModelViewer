// USCPrepareCommandlet (SCPrepareCommandlet.h): a scenario's environment, vehicle and player body prepared as levels.
#include "SCPrepareCommandlet.h"
#include "SCPlacement.h"
#include "SCCookLibrary.h"
#include "SCHangarIndex.h"
#include "SCImporter.h"
#include "SCLightImporter.h"
#include "SCPackageReader.h"
#include "SCPackageSource.h"
#include "SCPlayerAnimator.h"
#include "SCPlayerBody.h"
#include "SCPlayerKit.h"
#include "SCPlayerKitData.h"
#include "SCPrepareAudio.h"
#include "SCRigComponent.h"
#include "SCVehicle.h"
#include "SCViewOptions.h"
#include "SCViewerSubsystem.h"
#include "UCScenario.h"
#include "Cargo/SCCargoScenario.h"
#include "Cargo/SCCargoFxMapping.h"
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
#include "UObject/SavePackage.h"

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
        return Wait(bEnvironment ? Client->ExportEnvironment(What, Ffi, Progress) : Client->ExportDecomposed(What, Ffi, Progress), What, Progress, Error);
    }
    /** The scenario's player body (its definition's "player"), as the viewer exports one (USCViewerSubsystem::RequestPlayerKit). */
    TSharedPtr<FSCTarPackageSource> ExportPlayer(const FUCScenario& Scenario, FString& Error)
    {
        if (!OpenArchive(Error)) return nullptr;
        const TSharedRef<FSCExportProgress, ESPMode::ThreadSafe> Progress = MakeShared<FSCExportProgress, ESPMode::ThreadSafe>();
        return Wait(Viewer->GetClient()->ExportPlayer(Scenario.PlayerGender, Scenario.PlayerLoadout, Scenario.PlayerWeapons, Scenario.bPlayerSpares, Progress),
                    TEXT("the player body"), Progress, Error);
    }
    TSharedPtr<FSCTarPackageSource> Wait(UE::Tasks::TTask<FSCExportResult> Task, const FString& What,
                                         const TSharedRef<FSCExportProgress, ESPMode::ThreadSafe>& Progress, FString& Error)
    {
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

    /** Audio staged (a player body's too, as its live import registers it: its items' audio actions), the actor cooked, the
     *  level saved and stamped. */
    bool Finish(ASCVehicle* Actor, const FString& Level, const FString& Root, const FString& Shared, const FSCPackageSource& Source,
                const FString& Scene, FString& Error, bool bInstanceParts = true)
    {
        const FString Name = FPackageName::GetShortName(Level);
        const FString AudioFolder = TEXT("SCAudio/") + Name;
        int32 AudioFiles = 0;
        if (!SCPrepareAudio::Stage(Source, Scene, FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / AudioFolder), AudioFiles, Error)) return false;
        // The media: SoundWave assets in the level's folder (deleted with an older level, NewLevel).
        if (!SCPrepareAudio::MakeWaves(Source, Scene, Root / TEXT("Audio"), Actor->CookedAudioMedia, Error)) return false;
        Actor->CookedAudio = AudioFolder;
        Actor->CookedAudioScene = Scene;
        if (!USCCookLibrary::CookEnvironment(Actor, Root, Shared, Viewer ? Viewer->GetArchiveBuildId() : FString(), /*bKeepRigs*/ true, bInstanceParts))
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
        if (!WriteStamp(Level, Viewer->GetArchiveBuildId(), FString(), Error)) return false;
        UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE saved level=%s audio_files=%d audio_media=%d format=%d"), *Level, AudioFiles, Actor->CookedAudioMedia.Num(), FUCPrepared::FormatVersion);
        return true;
    }
    /** The stamp is published whole: written beside it first, then moved into place (a level or asset counts as current only
     *  with a complete stamp, FUCPrepared::IsLevelCurrent). Its id is new with each save; Sources (the traversal clips'): the
     *  ids of the levels it was made from. */
    bool WriteStamp(const FString& Package, const FString& Build, const FString& Sources, FString& Error)
    {
        const FString Stamp = StampFile(Package), Partial = Stamp + TEXT(".partial");
        IFileManager& Files = IFileManager::Get();
        const FString Text = FString::Printf(TEXT("%d\n%s\n%s\n"), FUCPrepared::FormatVersion, Build.IsEmpty() ? TEXT("unknown") : *Build,
                                             *FGuid::NewGuid().ToString(EGuidFormats::Digits)) +
            (Sources.IsEmpty() ? FString() : Sources + TEXT("\n"));
        if (!FFileHelper::SaveStringToFile(Text, *Partial) ||
            !Files.Move(*Stamp, *Partial, true, true))
        {
            Files.Delete(*Partial, false, true, true);
            Files.Delete(*Stamp, false, true, true);
            Error = TEXT("cannot write ") + Stamp;
            return false;
        }
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
        const FVector Offset = USCPlacementLibrary::PresentEnvironment(Environment, TEXT("open"), FloorZ);
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
        const int32 GearClips = USCPlacementLibrary::RestOnFloor(Ship, FloorZ);
        UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE vehicle %s paint=%s gear_clips=%d"), *Scenario.Vehicle,
               Paint == INDEX_NONE ? TEXT("default") : *Paints[Paint].Label(), GearClips);
        return Finish(Ship, Level, TEXT("/Game/SC/Ships/") + Name, TEXT("/Game/SC/Ships/Shared"), *Source, Scene, Error);
    }

    /** The player body's level (WP-O1-4b-2): the kit actor as the viewer imports it, its rigs kept, with its kit and body clips
     *  parsed once here (USCPlayerKitData), its personal lights' cookies and its package audio, so a run attaches it without
     *  the archive. */
    bool PreparePlayer(const FUCScenario& Scenario, const FString& Level, FString& Error)
    {
        const TSharedPtr<FSCTarPackageSource> Source = ExportPlayer(Scenario, Error);
        if (!Source) return false;
        const FString Scene = FSCPlayerKit::FindRootScene(*Source);
        if (Scene.IsEmpty())
        {
            Error = TEXT("the player body: no single root scene.json in the export");
            return false;
        }
        const FString Name = FPackageName::GetShortName(Level);
        UWorld* World = NewLevel(Level, TEXT("/Game/SC/Players/") + Name, Error);
        if (!World) return false;
        FSCImportResult Result;
        if (!FSCImporter::Import(*Source, USCPlayerBodyComponent::KitImportOptions(World, Scene), Result) || !Result.Vehicle.Get())
        {
            Error = TEXT("the player body: the import failed: ") + Result.Error;
            return false;
        }
        ASCVehicle* Body = Result.Vehicle.Get();
        const USCRigComponent* Leader = USCPlayerBodyComponent::FindLeader(Body);
        if (!Leader)
        {
            Error = TEXT("the player body: the package has no body rig");
            return false;
        }
        TSharedPtr<FSCPlayerKit> Kit;
        TSharedPtr<FSCPlayerClipMap> Clips;
        int32 FailedClips = 0;
        if (!SCLoadPlayerKit(*Source, Body->Manifest.ScenePath, USCPlayerBodyComponent::BoneMap(Leader), Kit, Clips, FailedClips, Error))
        {
            Error = TEXT("the player body: ") + Error;
            return false;
        }
        int32 Cookies = 0;
        for (const FSCFlashlight& Light : Kit->Flashlights)
        {
            if (FSCLightImporter::LoadGobo(*Source, Body, Light.ProjectorTexture)) ++Cookies;
            else UE_LOG(LogSCPrepare, Warning, TEXT("SC_PREPARE player light %s: cookie %s unavailable (%s)"), *Light.Entity, *Light.Cookie, *Light.ProjectorTexture);
        }
        USCPlayerKitData* Data = NewObject<USCPlayerKitData>(Body, TEXT("PlayerKit"));
        Data->Kit = Kit;
        Data->Clips = Clips;
        Body->PlayerKit = Data;
        Body->SetActorHiddenInGame(true); // unpublished until a run attaches it
        Body->SetActorLabel(TEXT("Player ") + Name);
        UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE player gender=%s loadout=%s weapons=%d states=%d blend_spaces=%d clips=%d clip_failures=%d rigs=%d cookies=%d"),
               *Kit->Gender, *Kit->Loadout, Kit->Weapons.Num(), Kit->States.Num(), Kit->BlendSpaces.Num(), Clips->Num(), FailedClips, Body->Rigs.Num(), Cookies);
        return Finish(Body, Level, TEXT("/Game/SC/Players/") + Name, TEXT("/Game/SC/Players/Shared"), *Source, Scene, Error);
    }

    /** A cargo box entity's level (WP-O1-4b-3): the box as the box library imports it (no collision: the box body collides;
     *  no clips), hidden, its parts not instanced (each box copies them); a run streams it as the box's template. */
    bool PrepareBox(const FString& Entity, const FString& Level, FString& Error)
    {
        const TSharedPtr<FSCTarPackageSource> Source = Export(false, Entity, Error);
        if (!Source) return false;
        const FString Scene = FSCPlayerKit::FindRootScene(*Source);
        if (Scene.IsEmpty())
        {
            Error = Entity + TEXT(": no single root scene.json in the export");
            return false;
        }
        const FString Name = FPackageName::GetShortName(Level);
        UWorld* World = NewLevel(Level, TEXT("/Game/SC/Cargo/") + Name, Error);
        if (!World) return false;
        FSCImportOptions Options;
        Options.World = World;
        Options.ScenePath = Scene;
        Options.bBuildCollision = false;
        Options.bPublish = false;
        Options.bReplacePrevious = false;
        Options.bImportClips = false;
        FSCImportResult Result;
        if (!FSCImporter::Import(*Source, Options, Result) || !Result.Vehicle.Get())
        {
            Error = Entity + TEXT(": the import failed: ") + Result.Error;
            return false;
        }
        ASCVehicle* Box = Result.Vehicle.Get();
        const FSCItemOccupancy& Occupancy = Box->Manifest.RootOccupancy;
        if (!Occupancy.bValid || Occupancy.StandardCargoUnits <= 0)
        {
            Error = Entity + TEXT(": the package's root is not a standard cargo unit item");
            return false;
        }
        Box->SetActorHiddenInGame(true);
        Box->SetActorEnableCollision(false);
        Box->SetActorLabel(TEXT("Box ") + Name);
        UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE box %s scu=%d size_m=%s mass=%.0f"), *Entity, Occupancy.StandardCargoUnits,
               *Occupancy.Dimensions.ToString(), Occupancy.Mass);
        return Finish(Box, Level, TEXT("/Game/SC/Cargo/") + Name, TEXT("/Game/SC/Cargo/Shared"), *Source, Scene, Error, /*bInstanceParts*/ false);
    }

    /** The tractor record the cargo setup names, as DataCore answers it (WP-O1-4b-3): a run applies it instead of asking.
     *  Empty when the setup names none; a record that does not apply is reported and left out (the run then asks the
     *  archive, or keeps the setup's copy of the values). */
    bool PrepareTractor(const FSCCargoScenario& Cargo, FString& OutJson, FSCCargoDroneDefinition& OutDrone, FString& Error)
    {
        OutJson.Reset();
        OutDrone = Cargo.Drone;
        const FString Record = !Cargo.Drone.TractorRecord.IsEmpty() ? Cargo.Drone.TractorRecord : Cargo.Drone.TractorRecordGuid;
        if (Record.IsEmpty()) return true;
        if (!OpenArchive(Error)) return false;
        UE::Tasks::TTask<FSCTextResult> Task = Viewer->GetClient()->DataCoreRecord(Record);
        Task.Wait();
        const FSCTextResult& R = Task.GetResult();
        FSCCargoDroneDefinition Check = Cargo.Drone;
        FString Why = R.Error.ToString();
        if (!R.bOK || !Check.ApplyTractorRecord(R.Json, Why))
        {
            UE_LOG(LogSCPrepare, Warning, TEXT("SC_PREPARE tractor record %s left out: %s"), *Record, *Why);
            return true;
        }
        OutJson = R.Json;
        OutDrone = Check;
        UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE tractor record=%s bytes=%d max_force=%.0f"), *Record, OutJson.Len(), Check.MaxForceN);
        return true;
    }

    /** The cargo effects (WP-O1-4b-3b): the tractor's hit and the box contacts with the scenario's ship and its obstacles,
     *  resolved as the run would from the archive (FSCCargoFxMapping). A read that fails fails the prep (kept as "none", a
     *  prepared run would never look again). */
    bool PrepareFx(const FSCCargoScenario& Cargo, const FSCCargoDroneDefinition& Drone, const FString& Vehicle, FString& OutJson, FString& Error)
    {
        OutJson.Reset();
        if (!OpenArchive(Error)) return false;
        TArray<FString> Entities = {Cargo.Ship.IsEmpty() ? Vehicle : Cargo.Ship};
        for (const FSCCargoScenario::FObstacle& O : Cargo.Obstacles) Entities.AddUnique(O.Entity);
        FSCCargoFxMapping Mapping;
        FString Why;
        if (!FSCCargoFxMapping::Resolve(*Viewer->GetClient(), Drone, Entities, Mapping, Why))
        {
            Error = TEXT("the cargo effects: ") + Why;
            return false;
        }
        OutJson = Mapping.ToJson();
        int32 Contacts = 0;
        for (const auto& [Entity, E] : Mapping.Contacts) Contacts += !E.Reference.IsEmpty();
        UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE cargo effects box_group=%s hit=%s contacts=%d/%d"), *Mapping.BoxGroup,
               Mapping.Hit.Reference.IsEmpty() ? TEXT("none") : *Mapping.Hit.Reference, Contacts, Mapping.Contacts.Num());
        return true;
    }

    /** The scenario's traversal clips (WP-O1-4b-2b): every seat and ladder action its vehicle and environment can ask for
     *  (read from their prepared levels' records), exported once and read against its prepared body's skeleton, saved as one
     *  asset (USCTraversalClipsData) a run serves seats and ladders from. */
    bool PrepareTraversal(const FUCPrepared& Record, FString& Error)
    {
        // From here until the new clips saved, no record can use the older ones.
        IFileManager::Get().Delete(*StampFile(Record.TraversalClips), false, true, true);
        // The prepared levels' actors, as saved (a level this run prepared is the package it saved).
        auto Actors = [](const FString& Level, TFunctionRef<void(ASCVehicle&)> Visit) {
            UPackage* Package = Level.IsEmpty() ? nullptr : LoadPackage(nullptr, *Level, LOAD_None);
            UWorld* World = Package ? UWorld::FindWorldInPackage(Package) : nullptr;
            if (!World || !World->PersistentLevel) return false;
            for (AActor* Actor : World->PersistentLevel->Actors)
                if (ASCVehicle* Vehicle = Cast<ASCVehicle>(Actor)) Visit(*Vehicle);
            return true;
        };
        TArray<FString> Names;
        for (const FString& Level : {Record.VehicleLevel, Record.EnvironmentLevel})
            if (!Level.IsEmpty() && !Actors(Level, [&](ASCVehicle& Vehicle) {
                    for (const FString& Name : SCTraversal::ClipNames(Vehicle)) Names.AddUnique(Name);
                }))
            {
                Error = TEXT("the traversal clips: cannot load ") + Level;
                return false;
            }
        Names.Sort();
        TMap<FString, int32> Bones;
        FString Gender;
        Actors(Record.PlayerLevel, [&](ASCVehicle& Vehicle) {
            const USCRigComponent* Leader = Vehicle.PlayerKit && Vehicle.PlayerKit->HasData() ? USCPlayerBodyComponent::FindLeader(&Vehicle) : nullptr;
            // A body whose rig mesh did not load (a partial copy of the prepared content) has no skeleton to read against.
            if (!Leader || !Leader->GetSkeletalMeshAsset() || !Bones.IsEmpty()) return;
            Bones = USCPlayerBodyComponent::BoneMap(Leader);
            Gender = Vehicle.PlayerKit->Kit->Gender;
        });
        if (Bones.IsEmpty())
        {
            Error = TEXT("the traversal clips: no player body with its body rig's mesh in ") + Record.PlayerLevel + TEXT(" (prepare it with -Force)");
            return false;
        }
        FSCPlayerClipMap Clips;
        TArray<FString> Skipped;
        if (!Names.IsEmpty())
        {
            if (!OpenArchive(Error)) return false;
            const TSharedRef<FSCExportProgress, ESPMode::ThreadSafe> Progress = MakeShared<FSCExportProgress, ESPMode::ThreadSafe>();
            const TSharedPtr<FSCTarPackageSource> Source = Wait(Viewer->GetClient()->ExportPlayerClips(Gender, Names, Progress), TEXT("the traversal clips"), Progress, Error);
            if (!Source) return false;
            if (!SCTraversal::ReadClips(*Source, Gender, Names, Bones, /*bSkipMissing*/ true, Clips, Skipped, Error))
            {
                Error = TEXT("the traversal clips: ") + Error;
                return false;
            }
        }
        // A seat lists every action it has, some of which no route uses: one the archive does not supply is left out (a route
        // that needs it reports it when it asks).
        for (const FString& Why : Skipped) UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE traversal clip left out: %s"), *Why);
        const FString& Path = Record.TraversalClips;
        const FString File = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
        // The older clips go by their file, as an older level does (NewLevel): nothing here has loaded them.
        if (FindPackage(nullptr, *Path))
        {
            Error = FString::Printf(TEXT("the older %s is loaded: it cannot be replaced in this process"), *Path);
            return false;
        }
        if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true, true))
        {
            Error = FString::Printf(TEXT("cannot delete the older %s: delete it and run again"), *File);
            return false;
        }
        IAssetRegistry::GetChecked().ScanModifiedAssetFiles({FPaths::ConvertRelativePathToFull(File)});
        UPackage* Package = CreatePackage(*Path);
        USCTraversalClipsData* Data = NewObject<USCTraversalClipsData>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone);
        Data->BodyLevel = Record.PlayerLevel;
        Data->Clips = MakeShared<FSCPlayerClipMap>(MoveTemp(Clips));
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.SaveFlags = SAVE_NoError;
        const FSavePackageResultStruct Saved = UPackage::Save(Package, Data, *File, Args);
        if (Saved.Result != ESavePackageResult::Success)
        {
            Error = FString::Printf(TEXT("cannot save %s (result %d)"), *Path, int32(Saved.Result));
            return false;
        }
        if (Record.TraversalSources().IsEmpty())
        {
            Error = TEXT("the traversal clips: a level of the record has no id in its stamp (prepare it with -Force)");
            return false;
        }
        // The archive build of the export, or (nothing asked of it) the body's.
        if (!WriteStamp(Path, Viewer ? Viewer->GetArchiveBuildId() : FUCPrepared::LevelBuild(Record.PlayerLevel), Record.TraversalSources(), Error)) return false;
        UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE saved traversal=%s names=%d clips=%d left_out=%d gender=%s format=%d"), *Path, Names.Num(),
               Data->Clips->Num(), Skipped.Num(), *Gender, FUCPrepared::FormatVersion);
        return true;
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
    // The player body: one level per body (its definition's "player"), shared by the scenarios that name the same one.
    Record.PlayerLevel = Scenario.PlayerLevel();
    if (Current(Record.PlayerLevel) && !Prepare.bForce)
    {
        UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE kept %s (prepared in format %d)"), *Record.PlayerLevel, FUCPrepared::FormatVersion);
    }
    else if (!Prepare.PreparePlayer(Scenario, Record.PlayerLevel, Error))
    {
        return Failed(Id, Error);
    }
    // The cargo setup's boxes (a level per box entity, shared by scenarios) and its tractor record (WP-O1-4b-3).
    if (!Scenario.Cargo.IsEmpty())
    {
        FSCCargoScenario Cargo;
        if (!FSCCargoScenario::Parse(Scenario.CargoJson, Cargo, Error)) return Failed(Id, TEXT("cargo setup ") + Scenario.Cargo + TEXT(": ") + Error);
        for (const FString& Entity : Cargo.Entities())
        {
            const FString Level = FUCPrepared::CargoLevelPath(Entity);
            if (Level.IsEmpty()) return Failed(Id, TEXT("cargo box entity \"") + Entity + TEXT("\" names no level"));
            Record.CargoLevels.Add(Entity, Level);
            if (Current(Level) && !Prepare.bForce)
            {
                UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE kept %s (prepared in format %d)"), *Level, FUCPrepared::FormatVersion);
            }
            else if (!Prepare.PrepareBox(Entity, Level, Error))
            {
                return Failed(Id, Error);
            }
        }
        FSCCargoDroneDefinition Drone;
        if (!Prepare.PrepareTractor(Cargo, Record.TractorRecordJson, Drone, Error)) return Failed(Id, Error);
        if (!Prepare.PrepareFx(Cargo, Drone, Scenario.Vehicle, Record.CargoFxJson, Error)) return Failed(Id, Error);
    }
    // The seat and ladder clips for that body: read again unless they were read from these very levels (their stamps' ids:
    // a level saved again since, by this run or another scenario's sharing it, makes them stale). Their names come from the
    // levels' records, their bones from the body.
    Record.TraversalClips = USCViewerSubsystem::TraversalClipsPath(Record.PlayerLevel, Record.VehicleLevel, Record.EnvironmentLevel);
    if (Current(Record.TraversalClips) && !Prepare.bForce && !Record.TraversalSources().IsEmpty() &&
        FUCPrepared::RecordedSources(Record.TraversalClips) == Record.TraversalSources())
    {
        UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE kept %s (prepared in format %d)"), *Record.TraversalClips, FUCPrepared::FormatVersion);
    }
    else if (!Prepare.PrepareTraversal(Record, Error))
    {
        return Failed(Id, Error);
    }
    // The archive builds the levels were prepared from (each level's stamp names its own).
    TArray<FString> Builds;
    TArray<FString> Prepared = {Record.EnvironmentLevel, Record.VehicleLevel, Record.PlayerLevel, Record.TraversalClips};
    for (const auto& [Entity, Level] : Record.CargoLevels) Prepared.Add(Level);
    for (const FString& Level : Prepared)
        if (const FString Build = FUCPrepared::LevelBuild(Level); !Level.IsEmpty()) Builds.AddUnique(Build.IsEmpty() ? FString(TEXT("unknown")) : Build);
    Record.Archive = FString::Join(Builds, TEXT(", "));
    Record.PreparedAt = FDateTime::UtcNow().ToIso8601();
    IFileManager::Get().MakeDirectory(*FUCPrepared::Directory(), true);
    if (!Record.Save(Error)) return Failed(Id, Error);
    UE_LOG(LogSCPrepare, Display, TEXT("SC_PREPARE_DONE scenario=%s environment=%s vehicle=%s player=%s record=%s seconds=%.0f"), *Id,
           Record.EnvironmentLevel.IsEmpty() ? TEXT("none") : *Record.EnvironmentLevel, *Record.VehicleLevel, *Record.PlayerLevel, *FUCPrepared::File(Id),
           FPlatformTime::Seconds() - Start);
    return 0;
}
