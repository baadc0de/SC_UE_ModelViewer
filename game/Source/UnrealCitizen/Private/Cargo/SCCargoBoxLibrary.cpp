// FSCCargoBoxLibrary: export -> tar parse -> import of cargo-box packages into hidden, shared template actors.
#include "SCCargoBoxLibrary.h"
#include "SCImport.h"
#include "SCPackageSource.h"
#include "SCPlayerKit.h"
#include "SCVehicle.h"
#include "SCViewOptions.h"
#include "SCViewerSubsystem.h"
#include "UCSettings.h"
#include "Engine/Level.h"
#include "Engine/LevelStreamingDynamic.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCCargoBoxes, Log, All);

namespace
{
constexpr double ImportBudgetSeconds = 0.010; // per frame: boxes are small, the frame belongs to the scene
}

void FSCCargoBoxLibrary::Request(const FString& Entity)
{
    if (Entity.IsEmpty() || Entries.Contains(Entity)) return;
    Entries.Add(Entity);
    Order.Add(Entity);
}

bool FSCCargoBoxLibrary::IsSettled() const
{
    for (const auto& Pair : Entries)
        if (Pair.Value.State != EState::Ready && Pair.Value.State != EState::Failed) return false;
    return true;
}

void FSCCargoBoxLibrary::Reset()
{
    if (Progress) Progress->RequestCancel();
    if (Export.IsValid()) Export.Wait();
    if (Parse.IsValid()) Parse.Wait();
    if (Job) Job->Cancel();
    for (auto& Pair : Entries)
    {
        if (Pair.Value.Template && IsValid(Pair.Value.Template)) Pair.Value.Template->Destroy();
        Unload(Pair.Value.Level); // a prepared box goes with its level
    }
    Entries.Reset();
    Order.Reset();
    Running.Reset();
    Export = {};
    Parse = {};
    Source.Reset();
    Job.Reset();
}

void FSCCargoBoxLibrary::Unload(ULevelStreamingDynamic* Level)
{
    if (!Level) return;
    if (Level->GetLevelStreamingState() == ELevelStreamingState::FailedToLoad)
    {
        if (UWorld* World = Level->GetWorld()) World->RemoveStreamingLevel(Level); // past the states a removal request is handled in
        return;
    }
    Level->SetShouldBeLoaded(false);
    Level->SetShouldBeVisible(false);
    Level->SetIsRequestingUnloadAndRemoval(true);
    Released.Add(Level); // a load still in flight that fails stops where a removal request is no longer handled
}

void FSCCargoBoxLibrary::SweepReleased(UWorld* World)
{
    Released.RemoveAll([World](const TObjectPtr<ULevelStreamingDynamic>& Level) {
        if (!Level || !World || !World->GetStreamingLevels().Contains(Level)) return true; // the world removed it
        if (Level->GetLevelStreamingState() != ELevelStreamingState::FailedToLoad) return false;
        World->RemoveStreamingLevel(Level);
        return true;
    });
}

void FSCCargoBoxLibrary::Fail(const FString& Entity, const FString& Error)
{
    if (FEntry* E = Entries.Find(Entity))
    {
        E->State = EState::Failed;
        E->Error = Error;
        Unload(E->Level);
        E->Level = nullptr;
    }
    UE_LOG(LogSCCargoBoxes, Warning, TEXT("CARGO_BOX_PACKAGE entity=%s ok=0 error=\"%s\""), *Entity, *Error);
    Running.Reset();
    Source.Reset();
    Job.Reset();
}

void FSCCargoBoxLibrary::Tick(USCViewerSubsystem* Viewer, UWorld* World)
{
    if (!Viewer || !World) return;
    SweepReleased(World);
    if (Running.IsEmpty())
    {
        // Next queued entity once the archive is open and the ship / environment loads are done.
        const FString* Next = Order.FindByPredicate([&](const FString& E) { return Entries[E].State == EState::Queued; });
        if (!Next || Viewer->IsBusy()) return;
        if (const FString* Prepared = UUCSettings::Get()->ScenarioCargoLevels.Find(*Next))
        {
            // A prepared scenario's box: its level, streamed (no archive).
            Running = *Next;
            FEntry& E = Entries[Running];
            E.Started = FPlatformTime::Seconds();
            bool bOK = false;
            E.Level = ULevelStreamingDynamic::LoadLevelInstance(World, *Prepared, FVector::ZeroVector, FRotator::ZeroRotator, bOK);
            if (!bOK || !E.Level)
            {
                E.Level = nullptr;
                Fail(Running, TEXT("cannot stream the prepared box level ") + *Prepared);
                return;
            }
            E.State = EState::Streaming;
            return;
        }
        // No archive reader in this build (a packaged game) fails too: the archive never opens, waiting would hold the queue.
        if (Viewer->GetArchiveState() == ESCArchiveState::Failed || !Viewer->GetClient())
        {
            Fail(*Next, Viewer->CanReadArchive() ? TEXT("the archive is not available (box packages are exported from Data.p4k)")
                                                 : TEXT("the box is not prepared and this build reads no archive"));
            return;
        }
        if (Viewer->GetArchiveState() != ESCArchiveState::Ready) return;
        Running = *Next;
        FEntry& E = Entries[Running];
        E.State = EState::Exporting;
        E.Started = FPlatformTime::Seconds();
        FSCExportOptions Ffi;
        if (Viewer->GetOptions()) Viewer->GetOptions()->ToExportOptions(Ffi);
        Progress = MakeShared<FSCExportProgress, ESPMode::ThreadSafe>();
        Export = Viewer->GetClient()->ExportDecomposed(Running, Ffi, Progress.ToSharedRef());
        return;
    }
    FEntry& E = Entries[Running];
    if (E.State == EState::Streaming)
    {
        if (E.Level->GetLevelStreamingState() == ELevelStreamingState::FailedToLoad)
        {
            Fail(Running, TEXT("the prepared box level did not load: ") + E.Level->GetWorldAssetPackageName());
            return;
        }
        ULevel* Level = E.Level->GetLoadedLevel();
        if (!Level || !E.Level->IsLevelVisible()) return;
        ASCVehicle* Template = nullptr;
        for (AActor* Actor : Level->Actors)
            if ((Template = Cast<ASCVehicle>(Actor)) != nullptr) break;
        if (!Template)
        {
            Fail(Running, TEXT("the prepared box level holds no box"));
            return;
        }
        Adopt(Template, TEXT("prepared"));
        return;
    }
    if (E.State == EState::Exporting)
    {
        if (!Export.IsCompleted()) return;
        FSCExportResult Result = MoveTemp(Export.GetResult());
        Export = {};
        if (!Result.bOK) { Fail(Running, Result.Error.ToString()); return; }
        Parse = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Tar = MoveTemp(Result.Tar)]() mutable {
            return TSharedPtr<FSCTarPackageSource>(MakeShared<FSCTarPackageSource>(MoveTemp(Tar)));
        });
        E.State = EState::Parsing;
        return;
    }
    if (E.State == EState::Parsing)
    {
        if (!Parse.IsCompleted()) return;
        Source = Parse.GetResult();
        Parse = {};
        if (!Source || !Source->IsValid()) { Fail(Running, Source ? Source->GetError() : FString(TEXT("cannot read the package"))); return; }
        const FString Scene = FSCPlayerKit::FindRootScene(*Source);
        if (Scene.IsEmpty()) { Fail(Running, TEXT("the package has no single root scene.json")); return; }
        FSCImportOptions Options;
        Options.World = World;
        Options.ScenePath = Scene;
        Options.bBuildCollision = false; // the box body collides, not the mesh
        Options.bPublish = false;
        Options.bReplacePrevious = false;
        Options.bImportClips = false;
        ISCImporting* const Importer = ISCImporting::Get();
        if (!Importer) { Fail(Running, TEXT("this build has no importer (the SCBridgeImport plugin)")); return; }
        Job = Importer->NewJob(*Source, Options, Source);
        E.State = EState::Importing;
        return;
    }
    if (E.State == EState::Importing)
    {
        if (!Job->Tick(false, ImportBudgetSeconds)) return;
        ASCVehicle* Template = Job->GetStage() == ESCImportStage::Succeeded ? Job->GetResult().Vehicle.Get() : nullptr;
        if (!Template) { Fail(Running, Job->GetResult().Error); return; }
        Adopt(Template, TEXT("archive"));
    }
}

void FSCCargoBoxLibrary::Adopt(ASCVehicle* Template, const TCHAR* From)
{
    FEntry& E = Entries[Running];
    Template->SetActorHiddenInGame(true);
    Template->SetActorEnableCollision(false);
    E.Template = Template;
    E.Occupancy = Template->Manifest.RootOccupancy;
    if (!E.Occupancy.bValid || E.Occupancy.StandardCargoUnits <= 0)
    {
        Fail(Running, TEXT("the package's root is not a standard cargo unit item (root_entity.inventory_occupancy / standard_cargo_units)"));
        return;
    }
    E.State = EState::Ready;
    UE_LOG(LogSCCargoBoxes, Display, TEXT("CARGO_BOX_PACKAGE entity=%s ok=1 source=%s seconds=%.2f size_m=%s scu=%d mass=%.0f meshes=%d"), *Running, From,
           FPlatformTime::Seconds() - E.Started, *E.Occupancy.Dimensions.ToString(), E.Occupancy.StandardCargoUnits, E.Occupancy.Mass,
           Template->Meshes.Num());
    Running.Reset();
    Source.Reset();
    Job.Reset();
}

void FSCCargoBoxLibrary::AddReferencedObjects(FReferenceCollector& Collector)
{
    for (auto& Pair : Entries)
    {
        Collector.AddReferencedObject(Pair.Value.Template);
        Collector.AddReferencedObject(Pair.Value.Level);
    }
    Collector.AddReferencedObjects(Released);
}
