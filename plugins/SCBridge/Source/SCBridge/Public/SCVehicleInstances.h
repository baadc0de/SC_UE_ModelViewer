// Extra vehicle instances beside the loaded one (WP-K1 mixed ownership: several ships in one hangar). Each instance is
// its own ASCVehicle (components, rigs and their pose, collision, lights, exploration data, paint slots); instances of
// an entity already imported are assembled from that import's retained assets (FSCImportedAssets: shared meshes, rigs,
// textures, material instances), others are exported (package cache) and imported once, then shared the same way.
// Auto-placed instances stand on free hangar floor (FSCVehiclePlacement); the set is cleared with the loaded vehicle.
#pragma once
#include "CoreMinimal.h"
#include "SCImport.h"
#include "SCPackageSource.h"
#include "Tasks/Task.h"
#include "SCArchive.h"

class AActor;
class UWorld;
class ASCVehicle;
class FSCImportedAssets;
class FSCTarPackageSource;
class USCViewerSubsystem;

enum class ESCVehicleInstanceState : uint8
{
    Queued,    // waiting for the loaded vehicle / a running load / the instance before it
    Exporting, // package export (cache) and parse: the entity has no retained import yet
    Importing, // full import, or assembly from retained assets
    Ready,
    Failed
};

/** One vehicle of the set (USCViewerSubsystem::GetVehicleInstances). */
struct SCBRIDGE_API FSCVehicleInstanceInfo
{
    /** 0 = the loaded vehicle; extra instances count from 1 and ids are never reused in a session. */
    int32 Id = INDEX_NONE;
    FString Entity;
    ESCVehicleInstanceState State = ESCVehicleInstanceState::Queued;
    TWeakObjectPtr<ASCVehicle> Actor;
    /** Placed on free floor (re-placed when the environment changes); false: the requested transform. */
    bool bAutoPlaced = false;
    /** Assembled from retained assets (no export, derivation or mesh/texture build). */
    bool bShared = false;
    /** Wall seconds from start to ready (queue wait excluded). */
    double Seconds = 0;
    float Progress = 0.f;
    FString Error;
};

DECLARE_MULTICAST_DELEGATE(FSCOnVehiclesChanged);

/** Free-floor placement: where a vehicle can stand in an environment without touching anything. */
struct SCBRIDGE_API FSCVehiclePlacementQuery
{
    /** What the vehicle occupies in its actor frame (cm), every mechanism's reach included (see Envelope). */
    FBox LocalEnvelope = FBox(ForceInit);
    /** The search starts here and goes outwards (nearest free spot wins). */
    FVector Anchor = FVector::ZeroVector;
    /** The floor the vehicle stands on: a spot's floor must be within FloorToleranceCm of it, and flat to that much. */
    double FloorZ = 0;
    double FloorToleranceCm = 30;
    /** Free space kept around the envelope on every side (cm): a walkway between parked ships, not game data. */
    double ClearanceCm = 300;
    /** World boxes the envelope (with clearance) must not overlap: other vehicles' envelopes, access lanes. */
    TArray<FBox> KeepOut;
    /** Actors the geometry test ignores (the vehicle being placed, vehicles about to move). */
    TArray<const AActor*> Ignore;
    /** Candidate yaws (degrees), the preferred first; every free spot of the first yaw beats the next yaw's. */
    TArray<double> Yaws = {0, 180, 90, 270};
    /** Bounds of the search (world XY); invalid: 200 m around the anchor. */
    FBox SearchArea = FBox(ForceInit);
    int32 MaxCandidates = 60000;
};
struct SCBRIDGE_API FSCVehiclePlacementResult
{
    bool bFound = false;
    /** Actor transform: location XY and yaw of the spot; Z puts the envelope's bottom on GroundZ. */
    FTransform Transform;
    double GroundZ = 0;
    int32 Tested = 0;
    double Seconds = 0;
    FString Reason; // why nothing was found
};
namespace FSCVehiclePlacement
{
/** The vehicle's envelope in its actor frame: its solid bounds (FSCEnvironment::SolidBounds) in the current pose and at
 *  the last frame of every bound clip (extended plates, open ramps, deployed gear), the actor at its current transform.
 *  Poses the rigs and resets them to the bind pose: call before resting the vehicle. */
SCBRIDGE_API FBox Envelope(ASCVehicle* Vehicle);
/** The world box of a local envelope at an actor transform (axis-aligned). */
SCBRIDGE_API FBox WorldBox(const FBox& Local, const FTransform& Actor);
/** Access lanes of an environment's inventory containers (freight elevators): each lattice box (bottom-centre origin,
 *  interior dimensions) widened on every horizontal side by its largest permitted item edge, the reach an item leaving
 *  the container needs. World boxes. */
SCBRIDGE_API TArray<FBox> AccessLanes(const ASCVehicle* Environment);
/** The nearest spot to Query.Anchor where the envelope plus clearance overlaps no geometry (Visibility channel; floor
 *  contact excluded) and no keep-out box, and the floor under its footprint is flat and at FloorZ. */
SCBRIDGE_API FSCVehiclePlacementResult FindFreeFloor(UWorld* World, const FSCVehiclePlacementQuery& Query);
}

/** The set of extra instances, owned by USCViewerSubsystem (which forwards its public API here). Game thread. */
class SCBRIDGE_API FSCVehicleInstances
{
public:
    explicit FSCVehicleInstances(USCViewerSubsystem& Viewer);
    ~FSCVehicleInstances();

    /** Queue an instance. bAuto: free floor next to the loaded vehicle; else World (bRest: landing gear rested and the
     *  lowest visible point on World's Z). Entity empty or "loaded": the loaded vehicle's entity. Returns its id. */
    int32 Spawn(const FString& Entity, bool bAuto, const FTransform& World, bool bRest, bool bGarageFloor = false);
    bool Destroy(int32 Id);
    /** The loaded vehicle first, then every ready instance by id. */
    TArray<ASCVehicle*> GetVehicles() const;
    TArray<FSCVehicleInstanceInfo> GetInstances() const;
    int32 GetId(const ASCVehicle* Vehicle) const;
    ASCVehicle* Find(int32 Id) const;
    bool IsSpawning() const;
    /** Incremented on every change of GetVehicles(); USCViewerSubsystem::OnVehiclesChanged is broadcast with it. */
    int32 GetRevision() const { return Revision; }

    /** The loaded vehicle finished loading (Key: archive generation | entity | options hash, as the viewer's reuse key);
     *  Assets: its retained import (null: none, e.g. a cooked level). */
    void SetPrimary(ASCVehicle* Primary, const TSharedPtr<FSCImportedAssets>& Assets, const FString& Key);
    bool RetainsSession(const FString& Session) const;
    /** The loaded vehicle goes: every instance and retained asset set with it. Their objects are added to Tracked
     *  (unload accounting) when given; bWorldAlive false: the world is tearing down and destroys the actors itself. */
    void Reset(TArray<TWeakObjectPtr<UObject>>* Tracked, bool bWorldAlive);
    void Tick();
    /** Share assets between instances (sc.Vehicle.ShareAssets, default 1); 0: every instance is a fresh import. */
    static bool ShareAssets();

private:
    struct FInstance
    {
        FSCVehicleInstanceInfo Info;
        FString Key;
        FTransform Requested = FTransform::Identity;
        bool bRest = true;
        bool bGarageFloor = false;
        double Started = 0, ExportSeconds = 0, ImportSeconds = 0;
    };
    FInstance* FindInstance(int32 Id);
    void Start(FInstance& I);
    void StepRunning();
    void Finish(FInstance& I, ASCVehicle* Actor);
    void Fail(FInstance& I, const FString& Error);
    /** Place I's actor (auto: free floor; else the requested transform). Others: instances to ignore while placing. */
    bool Place(FInstance& I, const TArray<const AActor*>& Others, FString& OutError);
    void ReplaceAll();
    void Changed();
    FString KeyFor(const FString& Entity) const;
    double FloorZ() const;

    USCViewerSubsystem& Viewer;
    TArray<FInstance> Instances;
    int32 NextId = 1;
    int32 Running = INDEX_NONE; // id
    int32 Revision = 0;
    TWeakObjectPtr<ASCVehicle> Primary;
    FString PrimaryKey;
    TMap<FString, TSharedPtr<FSCImportedAssets>> AssetSets; // by key
    TMap<FString, FString> SetSessions;                      // key -> import session the set's objects live in
    TMap<FString, FBox> Envelopes;                           // local envelope by key
    TWeakObjectPtr<ASCVehicle> PlacedIn;                     // environment the auto instances were placed in
    TArray<FString> CommandLine;                             // -SCExtraVehicles, queued once the loaded vehicle exists
    // The running instance's pipeline.
    TSharedPtr<FSCExportProgress, ESPMode::ThreadSafe> Progress;
    UE::Tasks::TTask<FSCExportResult> Export;
    UE::Tasks::TTask<TSharedPtr<FSCTarPackageSource>> Parse;
    TSharedPtr<ISCImportJob> Job;
};
