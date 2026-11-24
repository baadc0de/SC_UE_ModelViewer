// Incremental package import: parsing and mesh derivation on workers, UObject creation on the game thread in
// bounded steps so a host can render frames, show per-phase progress and cancel between steps.
#pragma once
#include "CoreMinimal.h"
#include "UObject/GCObject.h"
#include "SCImporter.h"

class ASCVehicle;



class SCBRIDGEIMPORT_API FSCImportJob : public ISCImportJob, public FGCObject
{
public:
    /** Source must outlive the job unless SourceOwner keeps it alive. Options.World is required. */
    FSCImportJob(const FSCPackageSource& Source, FSCImportOptions Options, TSharedPtr<const FSCPackageSource> SourceOwner = nullptr);
    /** Assembles one more actor from a finished import's retained assets: no package read, mesh derivation, mesh or
     *  texture build; the actor references the same meshes, rigs, textures and material instances and gets its own
     *  components, rigs (pose, collision), lights, exploration data and paint slots. Options gives World,
     *  ActorTransform, bPublish and bReplacePrevious; everything else is the original import's. One assembly per asset
     *  set at a time (a second one fails at once). */
    FSCImportJob(TSharedRef<FSCImportedAssets> Assets, FSCImportOptions Options);
    virtual ~FSCImportJob() override;
    /** After success with Options.bRetainAssets (or an assembly from assets): the asset set; null otherwise. */
    virtual TSharedPtr<FSCImportedAssets> GetRetainedAssets() const override { return Assets; }
    /** Shared objects an asset set holds (meshes, rigs, textures, material instances); for logs. */
    static int32 CountRetainedObjects(const FSCImportedAssets& Assets);

    /** Game thread. Non-blocking ticks return when a worker result is pending or Budget seconds are spent (checked
     *  after every step; a step is at most one static-mesh variant build); blocking ticks wait for workers and run to
     *  completion. Returns true once Succeeded, Failed or Cancelled. */
    virtual bool Tick(bool bBlocking, double BudgetSeconds = 0.05) override;
    /** Takes effect at the next step boundary: workers are drained and the partial actor is destroyed. */
    virtual void Cancel() override;

    virtual ESCImportStage GetStage() const override;
    virtual bool IsFinished() const override;
    /** Human-readable current step, e.g. "Building mesh 3/45". */
    virtual FString GetPhase() const override;
    /** 0..1 over the whole import. */
    virtual float GetFraction() const override;
    virtual const FSCImportTimings& GetTimings() const override;
    /** Filled progressively; Vehicle is set only on success. */
    virtual const FSCImportResult& GetResult() const override;
    virtual FSCImportResult& GetResult() override;
    /** Package root of this import's transient objects (/Temp/SC/<guid>); empty before Reading completes. */
    virtual FString GetSessionPath() const override;
    /** The actor spawned for this import, even after a cancel destroyed it (until GC); for leak accounting. */
    virtual ASCVehicle* GetSpawnedVehicle() const override;
    /** True while any worker (read, derive, prepare, material decode) has not completed. */
    virtual bool HasPendingWorkers() const override;

    virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
    virtual FString GetReferencerName() const override { return TEXT("FSCImportJob"); }

private:
    friend class FSCImportedAssets;
    struct FState;
    void RetainAssets();
    // Shared with the asset set once retained: an assembly from assets runs on the original import's state.
    TSharedPtr<FState> S;
    TSharedPtr<FSCImportedAssets> Assets;
};
