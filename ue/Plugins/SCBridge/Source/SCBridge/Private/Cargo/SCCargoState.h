// Internal state of USCCargoSubsystem (WP-K1): grids found on the loaded actors, boxes, docks, drones and transfer jobs,
// shared by the subsystem's translation units (discovery, scenario, jobs, views, console). Not part of the UI contract.
#pragma once
#include "CoreMinimal.h"
#include "UObject/ObjectKey.h"
#include "CollisionQueryParams.h"
#include "Cargo/SCCargoPlanner.h"
#include "Cargo/SCCargoScenario.h"
#include "Cargo/SCCargoFlightPath.h"
#include "Cargo/SCCargoTug.h"
#include "SCCargoBoxLibrary.h"

class ASCVehicle;
class ASCCargoContainer;
class ASCCargoDrone;
class ASCCargoDroneDock;
class USCCargoGridComponent;
class USCRigComponent;
class USCCargoSubsystem;

/** Every object type the ship's and hangar's parts (render meshes with complex collision, the game's colliders) and the
 *  boxes use; pawns (the player) are not obstacles for drones. */
inline FCollisionObjectQueryParams SCCargoObstacles()
{
    FCollisionObjectQueryParams Types;
    for (const ECollisionChannel C : {ECC_WorldStatic, ECC_WorldDynamic, ECC_PhysicsBody, ECC_Vehicle, ECC_Destructible}) Types.AddObjectTypesToQuery(C);
    return Types;
}

/** Which vehicles' own (hull) colliders a cargo query leaves out at a position (WP-P3: they act only on what is outside
 *  the vehicle's physics grid): the query's parameters with every hull in (All), with one vehicle's out, with all out. */
struct FSCCargoHullRule
{
    using FMemo = TMap<TTuple<uint32, FIntVector, int32>, bool>; // (vehicle, 25 cm cell, half height) -> in its grid
    struct FHull
    {
        TWeakObjectPtr<const ASCVehicle> Vehicle;
        FCollisionQueryParams Without;
    };
    FCollisionQueryParams All, None;
    TArray<FHull> Hulls;      // vehicles with hull colliders
    TArray<FVector> Probes;   // points of the shape, from its centre: upright axes through them are tested
    double HalfZ = 0;         // the shape's half height
    TSharedPtr<FMemo> Memo;   // shared by the queries of one world state
    /** Bit per Hulls entry whose grid the shape centred at Centre is in. */
    uint32 Touched(const FVector& Centre) const;
    const FCollisionQueryParams& For(uint32 Mask) const;
    const FCollisionQueryParams& At(const FVector& Centre) const { return For(Touched(Centre)); }
    const FCollisionQueryParams& Along(const FVector& A, const FVector& B) const { return For(Touched(A) | Touched(B)); }
};

/** A hold's structure cell (a lattice cell meeting the ship's own geometry): placement ids below -1000. */
constexpr int32 SCCargoStructureItemBase = -1000;
inline bool SCCargoIsStructure(int32 ItemId) { return ItemId <= SCCargoStructureItemBase; }

/** Why a request cannot run: the planner's own answer first (its refusals come before the ownership notes), "Nothing to
 *  move" when there is none. */
inline FText SCCargoMainReason(const FSCCargoTransferPreview& P)
{
    for (const FSCCargoRefusal& R : P.Refusals)
        if (R.BoxId == INDEX_NONE) return R.Reason;
    return P.Refusals.IsEmpty() ? NSLOCTEXT("SCCargo", "NothingToMove", "Nothing to move") : P.Refusals.Last().Reason;
}

/** A move's destination slot as a lattice placement. */
inline FSCCargoPlacement SCCargoSlotOf(const FSCCargoMovePlan& P)
{
    FSCCargoPlacement S;
    S.ItemId = P.BoxId;
    S.Origin = P.Origin;
    S.Cells = P.Cells;
    S.bRotated = P.bRotated;
    return S;
}

/** A box on its way between grids (lifted, flying, being placed); a held box hovers in place and is not. */
inline bool SCCargoIsFlying(ESCCargoBoxState S)
{
    return S == ESCCargoBoxState::Lifting || S == ESCCargoBoxState::InTransit || S == ESCCargoBoxState::Placing;
}

/** Margins of the flying shapes (viewer choices; the game has no cargo drones). */
namespace SCCargoFlightTuning
{
constexpr double DroneClearanceCm = 40; // kept around a drone's bounds and a load's footprint
constexpr double TurnStepDegrees = 10;
constexpr double TurnMarginCm = 5;      // around the box while it turns in free air
constexpr double MaxApproachCm = 600;   // how far out along a grid's normal a hover point is looked for
constexpr double MaxTurnRiseCm = 400;   // how far above the hover point the load may turn
constexpr double DroneHeightCm = 60;    // beam emitter to rotor tops above the box (the drone's bounds when known)
constexpr double GuardStretchCm = 50;   // the guard sweeps the route ahead in stretches of this length (as a search samples its route)
constexpr double PreciseLeadCm = 100;   // the precise zone of an interior grid begins this far before the load reaches its lattice
constexpr double PreciseSpeedCmS = 200; // flight speed in it (slow: the box follows its beam closely)
constexpr double FlushInsetCm = 2;      // a box swept onto its slot, or in beside boxes at rest, is taken this much inside its size
} // namespace SCCargoFlightTuning

/** Why a route search found nothing, for a failure reason. */
inline FString SCCargoRouteEndText(ESCCargoRouteEnd End)
{
    switch (End)
    {
    case ESCCargoRouteEnd::GoalBlocked: return TEXT("the load does not fit where it has to go");
    case ESCCargoRouteEnd::StartEnclosed: return TEXT("the load cannot get out of where it is lifted");
    case ESCCargoRouteEnd::GoalEnclosed: return TEXT("nothing the load fits through leads to where it has to go");
    case ESCCargoRouteEnd::Budget: return TEXT("the search for a way was given up at its limit");
    case ESCCargoRouteEnd::Smoothing: return TEXT("the way found is too tight to fly");
    default: return TEXT("no way found");
    }
}

/** The clips that extend / retract the grid hung from Anchor on Vehicle's rigs (SCCargoMovers.cpp). */
TArray<struct FSCCargoMover> SCCargoFindMovers(const class ASCVehicle* Vehicle, const class USceneComponent* Anchor, const FVector& GridPoint);

DECLARE_LOG_CATEGORY_EXTERN(LogSCCargo, Log, All);

struct FSCCargoBox
{
    int32 Id = INDEX_NONE;
    FString Entity;
    FName Commodity;
    FText CommodityLabel;
    FSCItemOccupancy Occupancy;
    int32 GridId = INDEX_NONE;
    FSCCargoPlacement Slot;
    ESCCargoBoxState State = ESCCargoBoxState::Stored;
    int32 JobId = INDEX_NONE;
    FName Owner; // whose box it is
    TWeakObjectPtr<ASCCargoContainer> Actor;
    // Loose cargo (WP-K2): its beacon once at rest, the location it was carried from, and since when it has been still.
    int32 BeaconId = INDEX_NONE;
    int32 OriginId = INDEX_NONE;
    double StillSince = -1;
    FVector SizeM() const { return FSCCargoLattice::FromSC(Occupancy.Dimensions); }
    /** Occupancy volume in SCU (the unit the beam's maxVolume is read in): authored in SStandardCargoUnit or
     *  SMicroCargoUnit (1e-6 SCU); else the standard cargo units. */
    double VolumeSCU() const
    {
        if (Occupancy.VolumeUnit == TEXT("SStandardCargoUnit") && Occupancy.Volume > 0) return Occupancy.Volume;
        if (Occupancy.VolumeUnit == TEXT("SMicroCargoUnit") && Occupancy.Volume > 0) return Occupancy.Volume * 1e-6;
        return Occupancy.StandardCargoUnits;
    }
};

/** A freestyle cargo beacon (WP-K2; FSCCargoBeaconView): a virtual location grouping loose boxes at rest. Its id comes
 *  from the grid id counter (a location id). */
struct FSCCargoBeacon
{
    int32 Id = INDEX_NONE;
    FString Name;
    int32 OriginId = INDEX_NONE;
    FText OriginLabel;
    FName Owner;
    TArray<int32> Boxes;
    double Created = 0;
};

struct FSCCargoDock
{
    int32 ItemId = INDEX_NONE;
    int32 GridId = INDEX_NONE;
    TWeakObjectPtr<ASCCargoDroneDock> Actor;
};

/** One task of a job (a planned move: carry, relocate or hold): drones fly out to the box, demagnetize it and lift it
 *  along its grid's normal, carry it hanging from their beams along a planned route (a hold hovers at a holding point
 *  until the box beneath is out, then flies back), lower it along the destination's normal, lock it, fly on. */
struct FSCCargoMove
{
    enum class EStep : uint8 { Waiting, Outbound, Attaching, Lifting, Transit, Lowering, Placing, Returning, Done, Failed, Holding };
    int32 MoveId = INDEX_NONE;            // unique across jobs: drones are claimed by it
    FSCCargoMovePlan Plan;
    EStep Step = EStep::Waiting;
    TArray<int32> Drones;                 // drone ids, the lead (holding the box) first
    TArray<FSCCargoFlight> DroneFlights;  // per drone: its own flight out to the box or home (empty, nimble)
    TArray<FVector> Offsets;              // per drone: formation offset in the box's frame (cm)
    TArray<FVector> Stations;             // per drone: where it hovers attached to the box at rest (world; its flight out ends there)
    TArray<double> FlightAt, FlightSince; // per drone: how far its flight had got, and since when it has not got on
    FSCCargoFlight Carry;                 // the box centre's flight of the current step (lift, transit, lower)
    FVector FromNormal = FVector::UpVector, ToNormal = FVector::UpVector; // source / destination grid normals (world)
    FQuat FromRotation = FQuat::Identity, ToRotation = FQuat::Identity;
    FTransform CatchFrom;                 // the swinging box's pose when the destination's approach takes it over
    double CatchStart = -1;               // when the catch began (after the drones of a box that tilts have moved out of its way); < 0: not yet
    FVector HoldPoint = FVector::ZeroVector;
    TArray<FVector> TransitPath;          // the load's planned route from the lift to the hold point / approach (swept free)
    FVector Goal = FVector::ZeroVector;   // its end: where the load turns onto the destination lattice
    FVector Approach = FVector::ZeroVector; // on the slot's normal line: from Goal straight to here (turned), then down
    double NextRetry = 0;                 // a blocked route is planned again from here on
    int32 Replans = 0;
    uint32 WorldStamp = 0;                // the world and stock what it keeps below was worked out in (RefreshMove)
    bool bReeledIn = false, bRefine = false; // planning: the beam at its minimum; half steps in the load's searches
    int32 EntranceOut = 0;                // into a hold: turn points nearer than this many steps out cannot be reached from outside
    // Where the lifted load can turn onto the hold's lattice before it flies (EarlyTurnPoint's answers per situation; unset: nowhere).
    TMap<FString, TOptional<FVector>> TurnPoints;
    // Into a ship's hold the load flies to the hold's entrance at its pickup rotation, turns there in free air onto the
    // hold's lattice (swept) and flies in lengthwise: SecondPath / SecondRotation (TurnStart: when that turn began).
    FQuat PickRotation = FQuat::Identity;
    double TurnStart = -1;
    TArray<FVector> SecondPath;
    FQuat SecondRotation = FQuat::Identity;
    // Route searches spread over frames (key -> search), kept until their result is used; the move waits in Planning
    // (Step Waiting, drones claimed) while one runs. Occupancy cells sampled per flying shape, shared by its searches.
    bool bPlanning = false;
    bool bRoutePending = false;
    ESCCargoRouteEnd RouteEnd = ESCCargoRouteEnd::None; // how the last finished route search of this move ended
    TMap<FString, TSharedPtr<FSCCargoRouteSearch>> Searches;
    mutable TMap<FString, TSharedPtr<TMap<FIntVector, bool>>> CellCaches;
    TSharedPtr<TMap<FIntVector, bool>> Cells(const FString& Shape) const
    {
        TSharedPtr<TMap<FIntVector, bool>>& C = CellCaches.FindOrAdd(Shape);
        if (!C) C = MakeShared<TMap<FIntVector, bool>>();
        return C;
    }
    bool bStalled = false;                // no route from where the load stopped: it holds position and plans again
    bool bRestoring = false;              // a hold flying its box back
    double StepStart = 0, LastSample = 0, LastTugSample = 0;
    // The guard: the route is known free up to ClearTo (arclength of Carry), swept in short stretches as the load comes
    // to need them (its braking distance ahead); bBlockedAhead: the stretch after ClearTo is not free, the load brakes
    // to rest there. GuardStamp: the world the stretches were swept in.
    double ClearTo = 0;
    bool bBlockedAhead = false;
    FVector RouteV = FVector::ZeroVector; // the route's velocity at the last tug (cm/s), for its acceleration
    bool bRouteMoving = false;            // RouteV is from the tick before
    bool bTurning = false, bTilting = false; // the box is being turned this tick (at the top of its lift, at a hold's entrance, as it is caught); and tilted by that turn
    bool bNoStation = false;              // a drone of the team found no room for even its shortest beam at the route point
    bool bLeaveNoStation = false;         // a route planned because a drone had no station: flown even so until it has one again
    bool bDroneBehind = false;            // a drone of the team is further from its station than its beam keeps full strength
    // The room over the route being flown, per drone and stretch (BeamRoom), in the world RoomStamp names.
    mutable TMap<int64, double> Rooms;
    mutable uint32 RoomStamp = 0;
    // Planning: when the move was planned (who was first: a team holds short of its station for loads planned before
    // it), and a plan that failed against space kept for other teams' drones is tried again from here on.
    double PlannedAt = 0, PlanRetryAt = 0;
    bool bReplanning = false;             // a replan's search is under way
    bool bKeptHit = false;                // this planning (or replan) was stopped somewhere by space kept for other teams' drones
    TArray<double> HoldAt;                // per drone: where its flight out holds short (HoldShort), checked at HoldChecked
    double HoldChecked = -1;
    bool bRigid = false;                  // in the precise zone: the box held rigidly on its route (not simulated)
    FVector RigidOffset = FVector::ZeroVector; // what it was off the route by when it came in, dying away
    FQuat RigidRotation = FQuat::Identity;
    uint32 GuardStamp = 0;
    double HangCm = 0, LoadAccel = 0, LoadJerk = 0; // cm, cm/s^2, cm/s^3 of the loaded team
    double DistanceCm = 0;                // planned flight length (ETA)
    FString Failure;
    bool bInterrupt = false;     // verification (sc.Cargo.Interrupt): fail this move in flight as a lost route would
    bool bLooseSource = false;   // the box comes from loose cargo (a beacon): lifted straight up, levelled, no source grid
    TSet<int32> TargetsTried;    // destinations a load let go of in flight was sent to and could not reach (RetargetLoad)
};

struct FSCCargoJob
{
    int32 Id = INDEX_NONE;
    ESCCargoJobState State = ESCCargoJobState::Planned;
    FSCCargoTransferRequest Request;
    FSCCargoTransferPreview Preview;
    TArray<FSCCargoMove> Moves;
    TSet<int32> OpenedGrids; // grids this job opened (closed again when it ends)
    bool bCancelRequested = false;
    int32 PickFailures = 0;      // consecutive failed picks with nothing picked since
    bool bLiftFailures = false, bRouteFailures = false; // what they were: not liftable / no way to its place
    TSet<int32> Unroutable;      // boxes whose routes were searched and not found (not offered as substitutes)
    TArray<FString> PickReasons; // their reasons (the last ones go into the failure text)
    FText Summary, Failure;
    FText QueueReason;   // Queued: what it waits for
    double NextQueueTry = 0;
    double Created = 0, StateSince = 0, Finished = 0;
    bool IsActive() const { return State != ESCCargoJobState::Done && State != ESCCargoJobState::Cancelled && State != ESCCargoJobState::Failed; }
};

/** Where a mover clip is on one rig: extended (played to its end) or at rest, and until when it is still moving. */
struct FSCCargoMoverState
{
    bool bOpen = false;
    double Until = 0;
};

struct FSCCargoState
{
    TMap<TPair<TObjectKey<USCRigComponent>, int32>, FSCCargoMoverState> Movers;
    TMap<int32, TWeakObjectPtr<USCCargoGridComponent>> Grids;
    int32 NextGridId = 1;
    TWeakObjectPtr<ASCVehicle> SeenEnvironment;
    // Vehicles (multi-vehicle hangars): the instances whose grids are discovered (instance id -> actor), their owners.
    TMap<int32, TWeakObjectPtr<ASCVehicle>> SeenVehicles;
    int32 SeenVehiclesRevision = INDEX_NONE;
    TMap<int32, FName> VehicleOwners;   // instance id -> owner (default: the player)
    TArray<int32> ScenarioVehicleIds;   // scenario vehicle index -> instance id (0 = the loaded vehicle)
    bool bScenarioVehiclesRequested = false;
    // Owners: the player and the others the scenario names (labels); the shared drone pool served fairly.
    FName Player = TEXT("you");
    TMap<FName, FText> OwnerLabels;
    TMap<FName, double> SharedServedAt; // owner -> when a move of theirs last took shared drones
    TMap<int32, FSCCargoBox> Boxes;
    TArray<FSCCargoDock> Docks;
    TMap<int32, FSCCargoBeacon> Beacons; // loose cargo (WP-K2), by location id
    TArray<TWeakObjectPtr<ASCCargoDrone>> Drones;
    int32 NextItemId = 1, NextDroneId = 1, NextJobId = 1, NextMoveId = 1;
    TArray<FSCCargoJob> Jobs;
    FSCCargoScenario Scenario;
    bool bScenarioTried = false, bScenarioLoadRequested = false;
    uint8 AppliedKinds = 0; // bit per ESCCargoGridKind whose scenario stock and docks are placed
    bool bScenarioPlaced = false; // both kinds placed once (later re-placement after a ship is lost is not waited on)
    bool bLoosePlaced = false;   // the scenario's loose stock is down (WP-K2)
    TArray<int32> ObstacleIds;   // vehicle instances the scenario placed as obstacles (WP-K3)
    double LastScenarioWait = -1000;
    FSCCargoBoxLibrary Library;
    bool bDebug = false;
    enum class ELookup : uint8 { None, Pending, Done };
    ELookup TractorLookup = ELookup::None; // the tractor record read from the archive (or given up on)
    // Route searches of all moves share SearchBudgetSeconds of each frame (from the frame's first search on).
    static constexpr double SearchBudgetSeconds = 0.002;
    double SearchUntil = 0;
    uint64 SearchFrame = 0;
    double SearchMaxStepMs = 0; // the longest single search step so far (what a search costs a frame at worst)
    int32 SearchesReported = 0; // searches finished and logged (CARGO_PATH)
    // Per-frame values route queries are built from: where the boxes at rest are, the environment's bounds.
    uint64 StockHashFrame = MAX_uint64, BoundsFrame = MAX_uint64;
    uint32 StockHash = 0;
    // The collision world apart from the cargo (vehicles, where they stand, every rig's pose), hashed per frame, and the
    // physics-grid answers kept while it stays as it is.
    uint64 WorldHashFrame = MAX_uint64;
    uint32 WorldHashValue = 0;
    TSharedPtr<FSCCargoHullRule::FMemo> GridMemo;
    FBox Bounds = FBox(ForceInit);
    double ScuVolumeM3 = 0; // one SCU's volume, from the first standard cargo box's occupancy (data)
    // Times a route query was stopped by space kept for drones alone (not by the world): a plan that fails with these
    // may succeed once the drones have moved on.
    TSharedRef<int64> KeptHits = MakeShared<int64>(0);
    // The space kept for drones, per kind of query, for this frame (USCCargoSubsystem::KeptSpace).
    uint64 KeptFrame = MAX_uint64;
    TMap<TTuple<const FSCCargoMove*, const ASCCargoDrone*, int32, bool>, TSharedRef<struct FSCCargoKeepOut>> KeptByQuery;
};

/** The drone with DroneId, or null. */
class UWorld;
ASCCargoDrone* FindDrone(const FSCCargoState& State, int32 DroneId);
/** A box actor for a package at Pose: its occupancy collision and mass, and the package meshes (SpawnBox, loose boxes). */
ASCCargoContainer* SCCargoSpawnBoxActor(UWorld* World, const FSCCargoBoxLibrary::FEntry& Package, const FString& Entity, const FTransform& Pose);
