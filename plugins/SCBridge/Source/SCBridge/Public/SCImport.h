// WP-O1-4c-3b: importing a package, as the runtime asks for it: the import options and result, an import job, the
// processed-asset cache and geometry streaming the viewer keeps, and the live import's helpers, behind interfaces the
// importer implements (FSCImportJob, FSCProcessedCache, FSCGeometryStreaming; ISCImporting registered by the importer's
// module). In a build without the importer (a packaged game) ISCImporting::Get is null.
#pragma once
#include "CoreMinimal.h"
#include "SCMeshes.h"
class ASCVehicle;
class FSCPackageSource;
class UTexture2D;
class UWorld;
class ISCProcessedCache;
class ISCGeometryStreaming;

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


/** Phase wall durations. Derive and Prepare sum worker durations and may overlap other phases.
 *  DeriveWait and PrepareWait measure how long each required result holds up the game-thread import cursor. */
struct SCBRIDGE_API FSCImportTimings
{
    double Read = 0, Derive = 0, DeriveWait = 0, Prepare = 0, PrepareWait = 0;
    double Collision = 0, Build = 0, Materials = 0, Assemble = 0;
    int32 DeferredRigidParts = 0;
};


struct SCBRIDGE_API FSCImportOptions
{
    UWorld* World = nullptr;
    FString ScenePath;
    bool bBuildCollision = true;
    bool bImportMaterials = true; // false keeps WP-U1's placeholder (timing baseline / geometry-only checks)
    bool bReplacePrevious = true;
    /** false: the actor is neither registered as the world's current import nor given exploration data
     *  (player kits are owned by the viewer subsystem, not the vehicle slot). */
    bool bPublish = true;
    /** true: the package is an environment (a hangar): its waypoints are chosen (SCHangarWaypoints::Select) and anchored
     *  for ASCVehicle::BuildWaypoints. */
    bool bEnvironment = false;
    /** true: every mesh with skin weights becomes a rig even without a binding clip (player kits are posed by
     *  USCPlayerBodyComponent, not by the clip catalogue). */
    bool bRigSkinnedMeshes = false;
    /** false: clip sidecars are not read or bound (player kits sample body clips themselves). */
    bool bImportClips = true;
    /** Environments: placements drawn only with VFX shaders (e.g. a hangar's landing hologram) are runtime state
     *  effects in the game (shown while a ship is delivered); they are not placed. */
    bool bHideEffectOnlyPlacements = false;
    /** Environments: identical rigid static parts (same mesh, materials, shadow/lighting flags, collision, channels and
     *  custom data, not under a rig) are drawn as one instanced component each; the render thread's per-primitive
     *  visibility cost dominated a hangar's frame (LOOK-6). Ships keep per-part components (hierarchy, focus). */
    bool bInstanceRepeatedParts = false;
    /** Keep what the import built after success (ISCImportJob::GetRetainedAssets) so more actors of the package can be
     *  assembled from it without deriving or building again (extra vehicle instances). The package bytes are released. */
    bool bRetainAssets = false;
    /** WP-O1-4b: keep each rig mesh's soft source data (ASCVehicle::RigSources) so an editor host can make the rig a
     *  savable asset (the prep step, Keep as assets). Editor hosts only by default. */
    bool bRetainRigSources = GIsEditor;
    /** Only a source associated with an identified archive build may use on-disk processed assets. The cache and the
     *  streaming must come from the importer the job is made by (ISCImporting::NewProcessedCache / NewGeometryStreaming);
     *  an import ignores (and warns about) one made elsewhere. */
    TSharedPtr<ISCProcessedCache, ESPMode::ThreadSafe> ProcessedCache;
    FString CacheArchiveBuild;
    bool bRebuildProcessed = false;
    TSharedPtr<ISCGeometryStreaming> GeometryStreaming;
    FTransform ActorTransform = FTransform::Identity;
};

struct SCBRIDGE_API FSCImportResult
{
    TWeakObjectPtr<ASCVehicle> Vehicle;
    FString Error;
    TArray<FString> Warnings; // Each recoverable diagnostic includes its asset/instance path.
    TMap<FString, FSCMeshBuildStats> MeshStats;
    TMap<FString, FString> ContentHashes;
    double TotalSeconds = 0, MaterialSeconds = 0;
    int32 MeshCount = 0, InstanceCount = 0, TextureCount = 0, MaterialCount = 0;
    int32 CachedMeshes = 0, MeshBuilds = 0, CollisionBuilds = 0;
    /** WP-I1 SCREEN_BIND: instances with a bound screen face; screen slots (with triangles) bound / dark / dark because
     *  faces of different outcomes share them. */
    int32 ScreenBoundInstances = 0, ScreenBoundSlots = 0, ScreenDarkSlots = 0, ScreenAmbiguousSlots = 0;
};


/** The processed-asset cache's state, as the viewer shows and sets it. */
struct SCBRIDGE_API FSCProcessedCacheStats
{
    int64 Bytes = 0;
    int32 Entries = 0;
};
class SCBRIDGE_API ISCProcessedCache
{
public:
    virtual ~ISCProcessedCache() = default;
    /** The importer that made it: an import uses only its own importer's cache (FSCImportOptions::ProcessedCache). */
    virtual const class ISCImporting* GetImporter() const = 0;
    virtual bool IsEnabled() const = 0;
    virtual FSCProcessedCacheStats Stats() const = 0;
    virtual int64 GetCapBytes() const = 0;
    virtual void SetCapBytes(int64 Bytes) = 0;
    virtual void Clear() = 0;
};

/** Coarse geometry now, fine builds and cache writes later (the viewer's import streaming). */
class SCBRIDGE_API ISCGeometryStreaming
{
public:
    virtual ~ISCGeometryStreaming() = default;
    /** The importer that made it: an import uses only its own importer's streaming (FSCImportOptions::GeometryStreaming). */
    virtual const class ISCImporting* GetImporter() const = 0;
    virtual void Forget(ASCVehicle* Owner) = 0;
    virtual void Tick(UWorld* World) = 0;
    virtual void FlushForCapture(UWorld* World) = 0;
    virtual void FinishWrites(UWorld* World) = 0;
    virtual void PublishWrites() = 0;
    virtual void DropWrites() = 0;
    virtual void LogStatus(UWorld* World) const = 0;
};

/** Incremental package import: parsing and mesh derivation on workers, UObject creation on the game thread in bounded
 *  steps so a host can render frames, show per-phase progress and cancel between steps. */
class SCBRIDGE_API ISCImportJob
{
public:
    virtual ~ISCImportJob() = default;
    /** Advance; true once finished. Non-blocking ticks return after BudgetSeconds of game-thread work (checked after
     *  every step; a step is at most one static-mesh variant build); blocking ticks wait for workers and run to the end. */
    virtual bool Tick(bool bBlocking, double BudgetSeconds = 0.05) = 0;
    virtual void Cancel() = 0;
    virtual ESCImportStage GetStage() const = 0;
    virtual bool IsFinished() const = 0;
    virtual FString GetPhase() const = 0;
    virtual float GetFraction() const = 0;
    virtual const FSCImportTimings& GetTimings() const = 0;
    virtual const FSCImportResult& GetResult() const = 0;
    virtual FSCImportResult& GetResult() = 0;
    virtual FString GetSessionPath() const = 0;
    virtual ASCVehicle* GetSpawnedVehicle() const = 0;
    virtual bool HasPendingWorkers() const = 0;
    /** What the import built, kept after success (FSCImportOptions::bRetainAssets). */
    virtual TSharedPtr<FSCImportedAssets> GetRetainedAssets() const = 0;
};

/** The importer, as the runtime uses it; registered by the importer's module. */
class SCBRIDGE_API ISCImporting
{
public:
    virtual ~ISCImporting() = default;
    /** The registered importer; null in a build without one. */
    static ISCImporting* Get();
    static void Set(ISCImporting* Importer);

    /** An import of Source (SourceOwner keeps a shared source alive for the job). */
    virtual TSharedPtr<ISCImportJob> NewJob(const FSCPackageSource& Source, FSCImportOptions Options,
                                            TSharedPtr<const FSCPackageSource> SourceOwner = nullptr) = 0;
    /** Another actor assembled from a finished import's retained assets (no deriving or building again). */
    virtual TSharedPtr<ISCImportJob> NewJob(TSharedRef<FSCImportedAssets> Assets, FSCImportOptions Options) = 0;
    /** UObjects a retained asset set holds (for release logs). */
    virtual int32 CountRetainedObjects(const FSCImportedAssets& Assets) const = 0;
    /** The processed-asset cache at Root, capped at CapBytes. */
    virtual TSharedPtr<ISCProcessedCache, ESPMode::ThreadSafe> NewProcessedCache(const FString& Root, int64 CapBytes) = 0;
    virtual TSharedPtr<ISCGeometryStreaming> NewGeometryStreaming() = 0;
    /** Unload the world's current import. */
    virtual void Unload(UWorld* World) = 0;
    /** A projector's gobo from a package (FSCLightImporter::LoadGobo). */
    virtual UTexture2D* LoadGobo(const FSCPackageSource& Source, ASCVehicle* Vehicle, const FString& Path) = 0;
};
