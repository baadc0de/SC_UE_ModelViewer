// Incremental package import: parsing and mesh derivation on workers, UObject creation on the game thread in
// bounded steps so a host can render frames, show per-phase progress and cancel between steps.
#pragma once
#include "CoreMinimal.h"
#include "UObject/GCObject.h"
#include "SCImporter.h"

enum class ESCImportStage : uint8
{
    Reading,
    Meshes,
    Materials,
    Assembling,
    Succeeded,
    Failed,
    Cancelled
};

/** Wall seconds per import phase. Derive/Collision are worker time, Build/Materials/Assemble game-thread time. */
struct SCBRIDGE_API FSCImportTimings
{
    double Read = 0, Derive = 0, Collision = 0, Build = 0, Materials = 0, Assemble = 0;
};

class SCBRIDGE_API FSCImportJob : public FGCObject
{
public:
    /** Source must outlive the job unless SourceOwner keeps it alive. Options.World is required. */
    FSCImportJob(const FSCPackageSource& Source, FSCImportOptions Options, TSharedPtr<const FSCPackageSource> SourceOwner = nullptr);
    virtual ~FSCImportJob() override;

    /** Game thread. Non-blocking ticks return when a worker result is pending or Budget seconds are spent; blocking
     *  ticks wait for workers and run to completion. Returns true once Succeeded, Failed or Cancelled. */
    bool Tick(bool bBlocking, double BudgetSeconds = 0.05);
    /** Takes effect at the next step boundary: workers are drained and the partial actor is destroyed. */
    void Cancel();

    ESCImportStage GetStage() const;
    bool IsFinished() const;
    /** Human-readable current step, e.g. "Building mesh 3/45". */
    FString GetPhase() const;
    /** 0..1 over the whole import. */
    float GetFraction() const;
    const FSCImportTimings& GetTimings() const;
    /** Filled progressively; Vehicle is set only on success. */
    const FSCImportResult& GetResult() const;
    FSCImportResult& GetResult();

    virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
    virtual FString GetReferencerName() const override { return TEXT("FSCImportJob"); }

private:
    struct FState;
    TUniquePtr<FState> S;
};
