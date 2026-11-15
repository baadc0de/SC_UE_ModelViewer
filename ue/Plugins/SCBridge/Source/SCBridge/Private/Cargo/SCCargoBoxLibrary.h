// Box package library (WP-K1): one decomposed package export per cargo-box entity through the viewer's FFI client and
// FSCImportJob, imported once into a hidden template actor whose meshes every box of that entity reuses; the package's
// root occupancy (dimensions, SCU, mass) comes with it. One export at a time, after the ship and environment loads.
#pragma once
#include "CoreMinimal.h"
#include "UObject/GCObject.h"
#include "Tasks/Task.h"
#include "SCBridgeClient.h"
#include "Cargo/SCCargoTypes.h"

class ASCVehicle;
class FSCImportJob;
class FSCTarPackageSource;
class USCViewerSubsystem;

class FSCCargoBoxLibrary : public FGCObject
{
public:
    enum class EState : uint8 { Queued, Exporting, Parsing, Importing, Ready, Failed };
    struct FEntry
    {
        EState State = EState::Queued;
        FString Error;
        FSCItemOccupancy Occupancy;
        TObjectPtr<ASCVehicle> Template;
        double Started = 0;
    };

    void Request(const FString& Entity);
    /** Game thread, every frame: advances the one running entry. */
    void Tick(USCViewerSubsystem* Viewer, UWorld* World);
    const FEntry* Find(const FString& Entity) const { return Entries.Find(Entity); }
    /** Every requested entity is Ready or Failed. */
    bool IsSettled() const;
    void Reset();

    virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
    virtual FString GetReferencerName() const override { return TEXT("FSCCargoBoxLibrary"); }

private:
    void Fail(const FString& Entity, const FString& Error);
    TMap<FString, FEntry> Entries;
    TArray<FString> Order;
    FString Running;
    TSharedPtr<FSCExportProgress, ESPMode::ThreadSafe> Progress;
    UE::Tasks::TTask<FSCExportResult> Export;
    UE::Tasks::TTask<TSharedPtr<FSCTarPackageSource>> Parse;
    TSharedPtr<FSCTarPackageSource> Source;
    TSharedPtr<FSCImportJob> Job;
};
