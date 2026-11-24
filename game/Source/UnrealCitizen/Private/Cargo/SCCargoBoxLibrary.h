// Box package library (WP-K1): one decomposed package export per cargo-box entity through the viewer's FFI client and
// FSCImportJob, imported once into a hidden template actor whose meshes every box of that entity reuses; the package's
// root occupancy (dimensions, SCU, mass) comes with it. One export at a time, after the ship and environment loads. A
// prepared scenario's box (WP-O1-4b-3, UUCSettings::ScenarioCargoLevels) streams its prepared level instead: its actor is
// the template, nothing is exported or imported.
#pragma once
#include "CoreMinimal.h"
#include "SCPackageSource.h"
#include "UObject/GCObject.h"
#include "Tasks/Task.h"
#include "SCArchive.h"
#include "Cargo/SCCargoTypes.h"

class ASCVehicle;
class ULevelStreamingDynamic;
class FSCImportJob;
class FSCTarPackageSource;
class USCViewerSubsystem;

class FSCCargoBoxLibrary : public FGCObject
{
public:
    enum class EState : uint8 { Queued, Streaming, Exporting, Parsing, Importing, Ready, Failed };
    struct FEntry
    {
        EState State = EState::Queued;
        FString Error;
        FSCItemOccupancy Occupancy;
        TObjectPtr<ASCVehicle> Template;
        /** A prepared box's level (its actor is the template). */
        TObjectPtr<ULevelStreamingDynamic> Level;
        double Started = 0;
    };

    void Request(const FString& Entity);
    /** Game thread, every frame: advances the one running entry. */
    void Tick(USCViewerSubsystem* Viewer, UWorld* World);
    const FEntry* Find(const FString& Entity) const { return Entries.Find(Entity); }
    /** Every requested entity (RPC catalogue). */
    TArray<FString> Entities() const { TArray<FString> Out; Entries.GetKeys(Out); return Out; }
    /** Every requested entity is Ready or Failed. */
    bool IsSettled() const;
    void Reset();

    virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
    virtual FString GetReferencerName() const override { return TEXT("FSCCargoBoxLibrary"); }

private:
    void Fail(const FString& Entity, const FString& Error);
    /** The template is ready: its occupancy checked, the entry Ready (From: how it came, for the log). */
    void Adopt(ASCVehicle* Template, const TCHAR* From);
    /** Releases a prepared box's level; kept until the world removes it (one whose load fails meanwhile is removed then). */
    void Unload(ULevelStreamingDynamic* Level);
    void SweepReleased(UWorld* World);
    TArray<TObjectPtr<ULevelStreamingDynamic>> Released;
    TMap<FString, FEntry> Entries;
    TArray<FString> Order;
    FString Running;
    TSharedPtr<FSCExportProgress, ESPMode::ThreadSafe> Progress;
    UE::Tasks::TTask<FSCExportResult> Export;
    UE::Tasks::TTask<TSharedPtr<FSCTarPackageSource>> Parse;
    TSharedPtr<FSCTarPackageSource> Source;
    TSharedPtr<FSCImportJob> Job;
};
