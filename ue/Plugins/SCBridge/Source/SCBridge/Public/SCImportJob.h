// Incremental package import: parsing and mesh derivation on workers, UObject creation on the game thread in
// bounded steps so a host can render frames, show per-phase progress and cancel between steps.
#pragma once
#include "CoreMinimal.h"
#include "UObject/GCObject.h"
#include "SCImporter.h"

class ASCVehicle;
/** What a finished import built (meshes, rigs and their collision data, clips, textures, material instances, the
 *  assembly inputs), kept to assemble more actors of the same package (FSCImportOptions::bRetainAssets). Opaque; it
 *  holds the shared objects for the garbage collector until the last holder lets go. */
class FSCImportedAssets;

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
    /** Assembles one more actor from a finished import's retained assets: no package read, mesh derivation, mesh or
     *  texture build; the actor references the same meshes, rigs, textures and material instances and gets its own
     *  components, rigs (pose, collision), lights, exploration data and paint slots. Options gives World,
     *  ActorTransform, bPublish and bReplacePrevious; everything else is the original import's. One assembly per asset
     *  set at a time (a second one fails at once). */
    FSCImportJob(TSharedRef<FSCImportedAssets> Assets, FSCImportOptions Options);
    virtual ~FSCImportJob() override;
    /** After success with Options.bRetainAssets (or an assembly from assets): the asset set; null otherwise. */
    TSharedPtr<FSCImportedAssets> GetRetainedAssets() const { return Assets; }
    /** Shared objects an asset set holds (meshes, rigs, textures, material instances); for logs. */
    static int32 CountRetainedObjects(const FSCImportedAssets& Assets);

    /** Game thread. Non-blocking ticks return when a worker result is pending or Budget seconds are spent (checked
     *  after every step; a step is at most one static-mesh variant build); blocking ticks wait for workers and run to
     *  completion. Returns true once Succeeded, Failed or Cancelled. */
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
    /** Package root of this import's transient objects (/Temp/SC/<guid>); empty before Reading completes. */
    FString GetSessionPath() const;
    /** The actor spawned for this import, even after a cancel destroyed it (until GC); for leak accounting. */
    ASCVehicle* GetSpawnedVehicle() const;
    /** True while any worker (read, derive, prepare, material decode) has not completed. */
    bool HasPendingWorkers() const;

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
