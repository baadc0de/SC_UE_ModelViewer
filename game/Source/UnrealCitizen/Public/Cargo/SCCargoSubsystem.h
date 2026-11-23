// Cargo subsystem (WP-K1): discovers inventory-container grids on the loaded vehicle and environment, owns cargo
// boxes, drones and transfer jobs, and exposes read-only views plus transfer preview/submit/cancel to the UI and to
// `sc.Cargo.*` console commands. The UI uses only this API and SCCargoTypes.h.
#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Cargo/SCCargoTypes.h"
#include "Cargo/SCCargoTug.h"
#include "SCCargoSubsystem.generated.h"

class UPrimitiveComponent;
struct FHitResult;

UCLASS()
class UNREALCITIZEN_API USCCargoSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    static USCCargoSubsystem* Get(const UObject* WorldContext);

    /** Incremented on every structural change (grids found/lost, boxes placed/removed, jobs added/finished): views
     *  that only rebuild on change compare it. Positions of moving boxes and drones change without a bump. */
    int32 GetRevision() const { return Revision; }

    /** Grids within RadiusM of Viewer (all grids when RadiusM <= 0), nearest first, DistanceM filled in. */
    TArray<FSCCargoGridView> GetGrids(const FVector& Viewer, float RadiusM) const;
    bool GetGrid(int32 GridId, FSCCargoGridView& Out) const;
    /** The settings' "nearby" radius (metres) the mobiGlas uses. */
    float GetNearbyRadiusM() const;

    /** Commodities stored on the given grids (all grids when empty). */
    TArray<FSCCargoCommodityView> GetCommodities(const TArray<int32>& Grids) const;
    TArray<FSCCargoDroneView> GetDrones() const;
    /** Boxes not resting on a grid (GridId == INDEX_NONE: lifted, in transit, being placed) with live WorldCenter /
     *  WorldBounds and their JobId; the UI's world-space callouts label them. Grid views do not list these boxes.
     *  Added by K1c (mobiGlas callouts); implemented by K1b (lifted, in-transit, placing and held boxes). */
    TArray<FSCCargoBoxView> GetMovingBoxes() const;
    /** Active jobs first, then the most recent finished ones. */
    TArray<FSCCargoJobView> GetJobs() const;
    bool IsAccessDoorBusy(const class ASCVehicle* Ship, int32 Door) const;
    bool IsVehicleTransferActive(const class ASCVehicle* Ship) const;

    // Freestyle cargo beacons (WP-K2, SCCargoBeacons.cpp): loose boxes at rest, grouped; a beacon's id is a location id
    // (like a grid's, never equal to one) that transfers take as a source.
    /** Beacons within RadiusM of Viewer (all when RadiusM <= 0), nearest first, DistanceM filled in. */
    TArray<FSCCargoBeaconView> GetBeacons(const FVector& Viewer, float RadiusM) const;
    bool GetBeacon(int32 BeaconId, FSCCargoBeaconView& Out) const;
    bool IsBeacon(int32 LocationId) const;
    /** A location's name for people: a grid's "group · grid", a beacon's name. */
    FText LocationLabel(int32 LocationId) const;
    /** Puts Count boxes of a ready package down loose: at At (each further one a box length aside), or each at a free
     *  floor spot with a clear lift corridor drawn from Rng (reachable by drones); they fall, come to rest and join or found
     *  a beacon. The ids placed; OutError says why any was not. */
    TArray<int32> AddLooseBoxes(const FString& Entity, int32 Count, const FTransform* At, FRandomStream* Rng, FName Commodity, const FText& Label, FName Owner,
                                FString& OutError);

    /** Plan without side effects: which boxes go where, refusals with reasons, the achievable amounts. */
    FSCCargoTransferPreview Preview(const FSCCargoTransferRequest& Request) const;
    /** Plan and start a job; returns its id, or INDEX_NONE with OutError set. */
    int32 Submit(const FSCCargoTransferRequest& Request, FText& OutError);
    bool Cancel(int32 JobId);
    /** Extend (bOpen) or retract a grid's owner item; false with OutError when refused (a loaded grid cannot
     *  retract, a fixed grid has nothing to move). Declared by K1c per the coordinator's API note (Hull A carries cargo
     *  only extended); implemented by K1b (the owner rig's extend / retract clips). */
    bool RequestGridAccess(int32 GridId, bool bOpen, FText& OutError);

    // UTickableWorldSubsystem
    virtual void Tick(float DeltaSeconds) override;
    virtual TStatId GetStatId() const override;
    virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // Console / routes (sc.Cargo.*)
    /** Grid ids for a selector: a grid id, "ship" / "vehicle", "elevator" / "environment", or text in a grid's labels. */
    TArray<int32> ResolveGrids(const FString& Selector, FName Owner = NAME_None) const; // "ship": Owner's ships (the player's by default)
    /** Commodity key for a name or label (case-insensitive); None when unknown. */
    FName ResolveCommodity(const FString& Name) const;
    void SetDebugDraw(bool bOn);
    bool IsDebugDraw() const;
    /** One line per grid, box, drone and job (sc.Cargo.List). */
    TArray<FString> Describe() const;
    /** Logs what surrounds a grid as the route queries see it (CARGO_SECTION: upright and level sections through its middle). */
    void LogSections(int32 GridId, double ReachM, double StepM) const;

    // RPC harness and state log (WP-H1, SCCargoRpc.cpp)
    /** Why the cargo is not idle, one short key per cause (empty: idle): packages loading, the scenario not yet placed,
     *  active jobs, grids extending or retracting, drones away from their docks, boxes off their grids. */
    TArray<FString> ActivityReasons() const;
    /** Grids, boxes (pose, velocity, slot error), drones (pose, velocity, beam force and lengths), docks and jobs, in
     *  metres, seconds and newtons. */
    TSharedRef<class FJsonObject> StateJson() const;
    /** The fastest thing cargo moves (cm/s): simulated boxes and drones. */
    double MaxSpeedCmS() const;
    /** Requested box packages: entity, state and occupancy. */
    TSharedRef<class FJsonObject> CatalogJson() const;
    /** Requests a box entity's package (export and import once): 1 ready, 0 still loading, -1 failed (OutError). */
    int32 RequestBoxPackage(const FString& Entity, FString& OutError);
    /** Puts Count boxes of a ready package on the grid, best fit; the ids placed. OutError says why any was not. */
    TArray<int32> AddBoxes(int32 GridId, const FString& Entity, int32 Count, FName Commodity, const FText& Label, FName Owner, FString& OutError);
    /** Removes a box. One a job holds is refused unless bForce: then that job is abandoned (as when one of its grids is
     *  unloaded: drones let go and fly home, its boxes in the air go) and the box goes. OutRemoved: every box removed. */
    bool RemoveBox(int32 BoxId, bool bForce, TArray<int32>& OutRemoved, FString& OutError);
    /** A drone dock with Drones drones on the grid; false (OutError) when it does not fit. */
    bool AddDock(int32 GridId, int32 Drones, FString& OutError);
    /** WP-O1-4: stop applying the scenario's cargo setup (a game scenario that failed): nothing more of it is placed, loaded
     *  or docked, and no setup is read from the settings afterwards. What is already placed stays with its grids (the
     *  failed scenario unloads them); the drone definition stays for drones already out. */
    void DropScenario();
    /** WP-O1-4b-3: every box the prepared cargo setup names has come (its template ready) or failed; true without one. The
     *  scenario reports running only then. */
    bool ArePreparedBoxesSettled() const;
    /** A cargo setup's environment is requested when it is read (replacing the one the viewer restored at start) unless a
     *  game scenario owns the scene, the setup is in space or names none, or the command line names the scene
     *  (-SCEnvironment=, -SCLoad=). */
    static bool TakesScenarioEnvironment(const TCHAR* CommandLine, bool bGameScenario, const struct FSCCargoScenario& Scenario);
    /** Verification (sc.Cargo.Interrupt): the first load in flight (of JobId; any job when INDEX_NONE) fails as a lost route
     *  would; false when none is in flight. */
    bool InterruptLoad(int32 JobId);
    /** A job's state (any job of the session, finished ones too); false when there is no such job. */
    bool FindJobState(int32 JobId, ESCCargoJobState& OutState, FText& OutReason) const;

    // Live spawning (SCCargoSpawnLive.cpp; developer console only: sc.Cargo.Spawn / Clear / Catalog): boxes of the game's
    // box records put on grids while the viewer runs, placed like scenario stock.
    /** Starts reading the game's box records (once per session, only when asked: the entity search CargoBoxSearch,
     *  each hit's DataCore record) and says how far that is; OutNote: progress or the error. */
    ESCCargoCatalogState RequestSpawnCatalog(FText* OutNote = nullptr);
    /** The commodities the box records offer (empty until read). */
    TArray<FSCCargoSpawnCommodity> GetSpawnCommodities() const;
    /** Logs the commodities whose key or label contains Text (CARGO_CATALOG_COMMODITY); asked before the records are
     *  read, once they are. */
    void ListSpawnCatalog(const FString& Text);
    /** Where Request's boxes would go now, or why not (refused: grid, retracted, full, size, amount, commodity, catalog). */
    FSCCargoSpawnPreview PreviewSpawn(const FSCCargoSpawnRequest& Request) const;
    /** Plans Request and queues its boxes: they are put down once their packages are exported (and, without a scenario,
     *  once the default drone fleet is in place), re-planned then (CARGO_SPAWN). The spawn's id, or INDEX_NONE when
     *  refused now (OutPreview says why; CARGO_SPAWN refused=). Box records still being read: it waits for them. */
    int32 Spawn(const FSCCargoSpawnRequest& Request, FSCCargoSpawnPreview& OutPreview);
    bool GetSpawnStatus(int32 SpawnId, FSCCargoSpawnStatus& Out) const;
    /** Removes the boxes on a grid (or a beacon) that no transfer holds, except those beneath a box or a slot that
     *  stays (CARGO_CLEAR). The ids removed; OutKept: the boxes left. False with OutError for an unknown location. */
    bool ClearLocation(int32 LocationId, TArray<int32>& OutRemoved, int32& OutKept, FString& OutError);

protected:
    int32 Revision = 0;

private:
#if WITH_DEV_AUTOMATION_TESTS
    friend class FSCCargoEnvironmentAccessTest;
#endif
    TSharedPtr<struct FSCCargoState> State;
    void Discover();
    void DiscoverOn(class ASCVehicle* Actor, ESCCargoGridKind Kind);
    void ForgetGrids(ESCCargoGridKind Kind, const class ASCVehicle* Only = nullptr);
    /** Fails an active job: every open move lets go of its box and gives back what it holds, its drones fly home (the
     *  job ends when they are back). True when drones are returning. */
    bool AbandonJob(struct FSCCargoJob& Job, const FText& Why, double Now);
    /** Destroys a box's actor and forgets it, with its cell on its grid (and its place in a beacon). */
    void ForgetBox(int32 BoxId);
    // Loose cargo (WP-K2)
    /** Every frame: loose boxes that came to rest are frozen where they lie and join or found a beacon; one that fell out
     *  of the world is forgotten. */
    void TickLoose(double Now);
    /** Lets a box go where it is, under gravity, loose (from OriginId). */
    void LetDown(struct FSCCargoBox& Box, int32 OriginId);
    void JoinBeacon(struct FSCCargoBox& Box);
    void LeaveBeacon(struct FSCCargoBox& Box);
    /** Loose boxes in Region (all when null) are let go to settle again: what they rested on may be gone. */
    void WakeLoose(const FBox* Region);
    /** A load let go of in flight (Why): the nearest open grid its owner may use with a free slot, not tried before, takes
     *  it (reservation moved, a new approach and route). False when none is left: the caller lets it down. */
    bool RetargetLoad(struct FSCCargoJob& Job, struct FSCCargoMove& Move, struct FSCCargoBox* Box, class ASCCargoContainer* A, const FString& Why, double Now);
    /** A free floor spot for a box of SizeCm with room above it for its lift and drones, drawn from Rng in the environment. */
    bool FindLooseSpot(const FVector& SizeCm, FRandomStream& Rng, FTransform& Out) const;
    FVector LocationCentre(int32 LocationId) const;
    /** Owners (multi-owner hangars): the label of an owner ("You", "Shared" for None); the requester of a request. */
    FText OwnerLabel(FName Owner) const;
    FSCCargoBoxView OwnedBoxView(const struct FSCCargoBox& Box) const;
    FName RequesterOf(const FSCCargoTransferRequest& Request) const;
    void TryScenario();
    void ApplyScenario();
    /** Without a scenario, live spawns bring the default drone fleet (CargoFleet: a drone definition and docks). */
    void RequestFleet();
    void LoadFleet();
    /** Every frame: the box records being read, and queued spawns put down once ready. */
    void TickCatalog();
    void TickSpawns();
    class USCCargoGridComponent* FindGrid(int32 GridId) const;
    class USCCargoGridComponent* SelectGrid(const struct FSCCargoGridSelector& Selector) const;
    struct FSCCargoPlanInput PlanInput(const FSCCargoTransferRequest& Request, bool bIgnoreOtherJobs = false) const;
    /** The reasons a requester cannot use some of the request (grids or boxes of other owners). */
    void OwnerRefusals(const FSCCargoTransferRequest& Request, FSCCargoTransferPreview& Preview) const;
    /** Moves, reservations and labels of a job from its plan (Submit, or a queued job whose turn came). */
    void Materialize(struct FSCCargoJob& Job, const FSCCargoTransferPreview& Plan);
    /** NoSupport: items on the grid that bear nothing (a live spawn: reserved incoming slots, boxes about to leave). */
    int32 SpawnBox(class USCCargoGridComponent* Grid, const FString& Entity, FName Commodity, const FText& Label, const TOptional<FIntVector>& Origin = {}, bool bRotated = false,
                   const TSet<int32>* NoSupport = nullptr);
    /** A grid as a live spawn plans on it: its lattice model (holds lengthwise to their entrance), with the return slots of
     *  boxes held aside by a dig-out; OutNoSupport gains what occupies cells but bears nothing. */
    struct FSCCargoGridModel SpawnModel(const class USCCargoGridComponent* Grid, TSet<int32>& OutNoSupport) const;
    /** A spawn waits for the default fleet: asked for and not loaded, or a dock of it on a grid that is here not yet placed. */
    bool FleetPending() const;
    void SpawnDock(class USCCargoGridComponent* Grid, int32 Drones);
    void UpdateAccess(class USCCargoGridComponent* Grid, double Now);
    bool RequestAccess(class USCCargoGridComponent* Grid, bool bOpen, double Now, FName Requester = NAME_None, FString* OutRefusal = nullptr);
    /** Whether Grid's mechanism may retract now (nothing on the grids it carries, no transfer using them). */
    bool CanClose(const class USCCargoGridComponent* Grid, FText* OutWhy = nullptr, int32 IgnoreJob = INDEX_NONE) const;
    void TickJobs(float DeltaSeconds);
    void TickMove(struct FSCCargoJob& Job, struct FSCCargoMove& Move, double Now, double DeltaSeconds);
    /** Claim drones and launch a waiting move when its box can be picked now (pick failures: substitute, then fail). */
    bool StartMove(struct FSCCargoJob& Job, struct FSCCargoMove& Move, double Now);
    /** Whether the box can be drawn off its grid now: nothing resting on it, its lift corridor along the grid normal free. */
    bool CanPick(int32 BoxId, FString& Why) const;
    /** How far the box can be drawn along Normal before it flies free (its height plus the clearance, less when a ceiling
     *  is near) and how long the beam can hang above it (the tractor's full-strength distance, down to its minimum
     *  distance under a ceiling). False with Why when there is not even room to lift it clear. */
    bool LiftPlan(const class ASCCargoContainer* Box, const FTransform& Pose, const FVector& Normal, double& OutLiftCm, double& OutHangCm, FString* OutWhy = nullptr) const;
    /** Releases a move's reservations (its destination slot) and its box back to Stored. */
    void ReleaseMove(struct FSCCargoMove& Move);
    /** Sweep / overlap queries for a flying shape (box half extent, centre offset from the planned point). */
    struct FSCCargoRouteQuery RouteQuery(const FVector& HalfExtent, const FVector& Offset, const AActor* Carried, double MinZ, double MaxZ, bool bIgnoreDocks = false, const FQuat& Rotation = FQuat::Identity,
                                         bool bIgnoreFlying = false, const TArray<const UPrimitiveComponent*>* IgnoreComponents = nullptr,
                                         const struct FSCCargoMove* Move = nullptr, const class ASCCargoDrone* Drone = nullptr, TConstArrayView<FVector> Ends = {}) const;
    void SendHome(struct FSCCargoMove& Move, double Now);
    /** A ship's interior hold containing P (its grid opened by the access rule), or null. */
    const class USCCargoGridComponent* HoldAt(const FVector& P) const;
    /** Which lattice axis an open hold is entered along (a drone-sized probe out of its middle; 0 = none or not a hold). */
    int32 EntranceAxis(class USCCargoGridComponent* Grid) const;
    /** Whether the flying shape sweeps free from the middle of Hold at height Z straight out of lattice side Side (0 +x,
     *  1 -x, 2 +y, 3 -y) to OutPoint, a few metres beyond the lattice (OutDir: level, outwards). */
    bool HoldExit(const class USCCargoGridComponent* Hold, const struct FSCCargoRouteQuery& Query, double Z, int32 Side, FVector& OutPoint, FVector& OutDir) const;
    bool RightOfWayYield(const struct FSCCargoMove& Move, int32 BoxId, const class ASCCargoContainer* Box, const FVector& PA, const FVector& PB,
                         bool bAnyLoad) const;
    bool InsideWaitingLoadAt(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, const FVector& P) const;
    bool StretchAheadFree(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, const FVector& Here, const FVector& Ahead) const;
    bool ReSlotMove(const struct FSCCargoJob& Job, struct FSCCargoMove& Move, const struct FSCCargoBox* Box, class USCCargoGridComponent* To,
                    const TCHAR* Why);
    bool ReplanLoad(const struct FSCCargoJob& Job, struct FSCCargoMove& Move, const struct FSCCargoBox* Box, class ASCCargoContainer* A,
                    class USCCargoGridComponent* To, const FVector& From, const TCHAR* Why, double Now);
    /** Plans a claimed move before anything flies: lift along the source normal, the load's route to its goal and each
     *  drone's route out to the box, all swept free (CARGO_PATH). False with Why when any of them has no free route. */
    bool PrepareMove(struct FSCCargoJob& Job, struct FSCCargoMove& Move, FString& Why);
    bool PlanMove(struct FSCCargoJob& Job, struct FSCCargoMove& Move, FString& Why);
    /** The queries for the hanging load of Move (the box and its drones: over it with their beams, or at its ends, as
     *  Grip says: 0 over, 1 the ends, -1 the move's grip now) and for a lone drone. */
    struct FSCCargoRouteQuery LoadQuery(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, bool bIgnoreFlying = false,
                                        const FQuat* Rotation = nullptr, int32 Grip = -1) const;
    /** The drones of Move's box at its ends (FSCCargoEndGrip), MarginCm around their hulls; bSwap: each drone's whole way
     *  between its place over the box and its end (a swap of grip). OutTopCm: the shape's top over the box's centre. */
    struct FSCCargoRouteQuery EndsQuery(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, const FQuat& Rotation, double MarginCm, bool bIgnoreFlying,
                                        const TArray<const UPrimitiveComponent*>* Ignore, bool bSwap = false, double* OutTopCm = nullptr) const;
    /** Whether the team can swap grips with its box held at P (Rotation): the drones over it, at its ends and on their
     *  ways between, free. OutWhy names what is in the way. */
    bool SwapClear(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, const FVector& P, const FQuat& Rotation, FString* OutWhy = nullptr) const;
    /** Where a load arriving from its ends swaps back over the box before its goal (at Rotation): the nearest point over
     *  To's lattice at the goal's height where the swap is clear and from which the load held over sweeps straight to
     *  Goal. False: none (it is held from its ends down onto its slot). */
    bool SwapOutPoint(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, const class USCCargoGridComponent* To, const FVector& Goal, const FQuat& Rotation,
                      FVector& OutPoint, FString* OutNote = nullptr) const;
    /** The ends grip for Box held by a team of Team (one or two) of Drones: from the tractor record, the load and the
     *  drones' hulls (round 5: also the geometry every team's grip places are drawn from). */
    FSCCargoEndGrip EndGrip(const class ASCCargoContainer* Box, double MassKg, const TArray<class ASCCargoDrone*>& Drones, int32 Team) const;
    /** One tick of a swap of grip (Move.SwapTo): the box held at Q / Rotation, each drone along its way round it. True
     *  once every drone is at its new station (Move.Grip is then SwapTo). */
    bool SwapGrip(struct FSCCargoMove& Move, class ASCCargoContainer* Box, const TArray<class ASCCargoDrone*>& Drones, const FVector& Q, const FQuat& Rotation, double Now, double Dt);
    /** Where drone I of Move hovers (its emitter's station, box frame) in a grip (0 over, 1 the ends: its end, EndOf). */
    FVector GripStation(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, int32 I, int32 Grip) const;
    /** Round 5, where a drone holds from (box frame; Rotation: the box as it hangs on its route now). Drone I's planned
     *  place: its end of the ends grip, else over its formation offset at the hang (inside the route's shape: the
     *  fallback); where it holds from now (its place, or on its way round the box to its next: the box's surface nearest
     *  to it, no face). */
    FSCCargoGripSpot PlannedSpot(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, int32 I, const FQuat& Rotation) const;
    /** The order the ways of holding a box are planned in (FSCCargoMove::Tier: 0 ends, 1 over with room, 2 over tight). */
    TArray<int32> TierOrder(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, int32 Team) const;
    FSCCargoGripSpot GripNow(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, int32 I, const FQuat& Rotation, bool* bOutMoving = nullptr) const;
    /** Every drone at its planned place, each looking again at its own staggered moment. */
    void ResetSpots(struct FSCCargoMove& Move, const class ASCCargoContainer* Box, const TArray<class ASCCargoDrone*>& Drones, const FQuat& Rotation, double Now);
    /** One tick of the drones searching for a good grip (FSCCargoGripPick), the box at Q / Rotation on its route: each
     *  moves on round the box to the place it picked; or picks again when its time comes; or goes back to its planned
     *  place when its own no longer fits along the route ahead, the route's piece ends within it (a turn, a swap, the
     *  descent) or it is in the precise zone (CARGO_GRIP_PICK). */
    void GripTick(struct FSCCargoMove& Move, class ASCCargoContainer* Box, const TArray<class ASCCargoDrone*>& Drones, const FVector& Q, const FQuat& Rotation,
                  bool bSearch, double Now, double Dt);
    /** Whether a drone's hull fits at Spot along the route ahead (Move's lookahead from where its load is), and along its
     *  ways there from From and from there back to Back (its planned place), with the load's margin. OutWhy: what stops it. */
    bool SpotFits(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, const FSCCargoGripSpot& From, const FSCCargoGripSpot& Spot,
                  const FSCCargoGripSpot& Back, const FQuat& Rotation, FString* OutWhy = nullptr) const;
    /** The route's margin for a load (cm): the gusts' envelope (SwayEnvelopeCm) and, round 5, the most its drones' wobble
     *  moves it through their holds (drone_art.json motion.load_transfer x the longest hold of Move's places x the sine of
     *  max_tilt_deg, plus its drift and hunt; the box's spring passes no more than that on). */
    double LoadEnvelopeCm(const struct FSCCargoMove& Move) const;
    /** A drone of another move in Move's destination's precise zone (its interior, grown by the reach of Box's drones):
     *  its id, else INDEX_NONE (round 5: the zone is entered clear). */
    int32 ZoneOccupant(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box) const;
    /** Whether Drone counts as in Move's destination zone for ZoneOccupant (its interior grown by the reach of Box's drones
     *  and the distance drones keep). */
    bool InZoneOf(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, const class ASCCargoDrone* Drone) const;
    /** Another load in the air (not Box) whose box and drones anywhere round it (FSCCargoGripPick::Reach), moving on for
     *  Seconds at its route's speed, can reach into Region (world): what it is, or empty. */
    FString OtherLoadNear(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, const FBox& Region, double Seconds) const;
    /** The turn onto the destination lattice in free air, swept: the load shape at rotations from FromQ to ToQ (steps of
     *  at most TurnStepDegrees) while moving from FromP to ToP, all free. OutWhy names the first blocker. */
    bool TurnClear(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, const FVector& FromP, const FQuat& FromQ, const FVector& ToP,
                   const FQuat& ToQ, FString* OutWhy = nullptr) const;
    /** The descent from the hover point onto the slot along the grid normal, at the slot's rotation: the box, 2 cm
     *  inside its own size (it ends flush with its neighbours and the grid), and, when Move holds it from its ends, the
     *  drones going down beside its end faces. */
    bool DescentClear(const class ASCCargoContainer* Box, const FQuat& Rotation, const FVector& From, const FVector& To, FString* OutWhy = nullptr,
                      const struct FSCCargoMove* Move = nullptr) const;
    /** The way onto a slot: a hover point on its normal line (out from MinCm) from which the descent along the normal
     *  is free, and the turn point where the load (arriving at FromQ) turns onto the slot's rotation, swept: the hover
     *  point itself, or above it when the turn needs room (a long box tilting onto a vertical plate near the floor), with
     *  the turned box's way down to the hover point swept too. False: no room within reach. */
    bool SlotApproach(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, const FTransform& Slot, const FVector& Normal,
                      const FQuat& FromQ, double MinCm, FVector& OutTurn, FVector& OutHover, FString* OutWhy = nullptr) const;
    /** Where the load lifted to Lifted off From can turn level onto ToRotation before it flies, in free air (the box and
     *  its drones swept through the turn): Lifted itself, else the nearest point over From's lattice at that height it
     *  reaches in a straight sweep. False: no such point, or not a level turn. Kept on the move per situation. */
    bool EarlyTurnPoint(struct FSCCargoMove& Move, const class ASCCargoContainer* Box, const class USCCargoGridComponent* From, const FVector& Lifted,
                        const FQuat& ToRotation, FVector& OutPoint) const;
    /** Where a drone in the air begins its route: InOutStart itself when its routing shape is free there, else the
     *  nearest point a short straight step away where it is (and that its hull sweeps to freely), else the first
     *  such point back along the way it came (OutVia: the places between). False: none. */
    bool ClearStart(const class ASCCargoDrone* Drone, const struct FSCCargoRouteQuery& Query, FVector& InOutStart, TArray<FVector>* OutVia = nullptr) const;
    /** A lone drone's query. Ends: where the route begins and ends (kept space there is left out: its bay beside another's). */
    struct FSCCargoRouteQuery DroneQuery(const class ASCCargoDrone* Drone, const struct FSCCargoMove* Move = nullptr, TConstArrayView<FVector> Ends = {}, bool bTight = false) const;
    /** The drones and beams over Move's box hanging at Rotation (a level box along the formation), MarginCm around them;
     *  with Move.bRoomy at least a drone's half clearance over their hulls. */
    struct FSCCargoRouteQuery ColumnQuery(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, const FQuat& Rotation, double MarginCm,
                                          bool bIgnoreFlying, const TArray<const UPrimitiveComponent*>* Ignore, double* OutTopCm = nullptr) const;
    /** Plans a route with Query and logs its proof (CARGO_PATH: found, minimum clearance, sweep hits). */
    /** A route for the move's load or a drone, searched over frames (the frame's search budget): false with
     *  Move.bRoutePending while the search goes on (call again next tick with the same Key), false when there is none.
     *  bKeep keeps the result for repeated calls (a move being prepared over several ticks). */
    bool PlanRoute(const struct FSCCargoJob& Job, struct FSCCargoMove& Move, const TCHAR* What, const FVector& Start, const FVector& Goal,
                   const struct FSCCargoRouteQuery& Query, TArray<FVector>& OutPath, const FString& Key = FString(), bool bKeep = false);
    /** A drone's flight home (route to its dock's exit, then its bay); false while no free route exists. */
    FVector Departure(class ASCCargoDrone* Drone, const FVector& BayExit) const;
    bool HomeFlight(const struct FSCCargoJob& Job, struct FSCCargoMove& Move, class ASCCargoDrone* Drone, struct FSCCargoFlight& Out);
    /** The hook where an environment grid is made reachable (its doors opened, its lights on): K1e provides it. */
    void SetJobState(struct FSCCargoJob& Job, ESCCargoJobState NewState, const FText& Reason = FText());
    void FinishMove(struct FSCCargoJob& Job, struct FSCCargoMove& Move, bool bOK);
    void DebugDraw() const;
    double Gravity() const;
    /** Air around the drones (gusts) and the furthest a gust moves one (cm; 0 in vacuum): added to every swept margin. */
    bool InAtmosphere() const;
    double SwayEnvelopeCm() const;
    /** The precise zone of a move: inside its destination's lattice and a lead before it, when that is an interior grid (a
     *  hold, a lift room: cargo stands flush there). In it the load flies slowly, without gusts, with the placing margin.
     *  OutToLattice: world to the lattice's frame; OutZone: the zone in that frame. */
    bool PreciseZone(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, FTransform& OutToLattice, FBox& OutZone) const;
    bool InPreciseZone(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, const FVector& P) const;
    /** Base with the vehicles' own (hull) colliders left out per position: where the shape (a box of HalfExtent at
     *  Rotation) is in a vehicle's physics grid, that vehicle's; elsewhere none (WP-P3). One rule for every
     *  query: a hull's opening caps are no colliders (WP-P4), so an open ramp or door is an opening by position alone. */
    TSharedRef<struct FSCCargoHullRule> HullRule(const struct FCollisionQueryParams& Base, const FVector& HalfExtent, const FQuat& Rotation) const;
    /** A drone's hull rule (its shape, level): a drone holding its box is also in the grids its load is in. */
    TSharedRef<struct FSCCargoHullRule> DroneHullRule(const struct FCollisionQueryParams& Base, const class ASCCargoDrone* Drone) const;
    /** The tractor record as a beam (Cargo/SCCargoTug.h), and the shortest beam that holds a load's weight shared by a
     *  team (cm): what a load's route reserves over the box for its drones, since they can tuck in that far. */
    struct FSCCargoBeam TractorBeam() const;
    double MinHangCm(double LoadKg, int32 Team) const;
    /** The acceleration (m/s^2) a loaded team's route is flown at, along it and across it (in its bends): what the
     *  team can give straight up, where its beams have the least to spare, less the scenario's min_load_accel kept
     *  for the springs to correct with, over root two (so both at once stay within it). */
    double RouteAccel(int32 Team, double LoadKg) const;
    double MinHangCm(const class ASCCargoContainer* Box) const;
    /** How far the drones reach above their emitters (cm): the tallest drone's hull over its emitter (HullAboveCm), the
     *  room a load's route keeps over its beams' hang; SCCargoFlightTuning::DefaultDroneHeightCm before any drone exists. */
    double DroneHeightCm() const;
    /** Space kept for drones in a route query (Cargo/SCCargoTug.h): the stations of teams on their way out to their
     *  boxes, the drones hovering attached to a box at rest or holding one, and (a load's query) the drones carrying
     *  a load this query takes as an obstacle. A load's query keeps their hulls; a lone drone's (Drone) keeps the
     *  distance drones keep from each other, also from its teammates' stations and from any drone standing in the
     *  air, but not at the route's own Ends. Move's own drones are otherwise left out. */
    TSharedRef<struct FSCCargoKeepOut> KeptSpace(const struct FSCCargoMove* Move, const class ASCCargoDrone* Drone, int32 CarriedBoxId, bool bIgnoreFlying,
                                                 TConstArrayView<FVector> Ends = {}) const;
    /** Whether a docked drone can leave its bay now: its hull fits at the bay's exit and sweeps free from there to the
     *  end of its dock's corridor (not with a retracted cargo plate beside its dock, say). Looked at once a second;
     *  CARGO_DRONE_BAY_BLOCKED when it changes. Such a drone is not sent. */
    bool CanLeaveBay(class ASCCargoDrone* Drone) const;
    /** Finds what Drone's hull meets standing in its bay (ASCCargoDrone::BayOwn); logs it once per drone (CARGO_DRONE_BAY). */
    void FindBayOwn(class ASCCargoDrone* Drone) const;
    /** A drone's way out of its dock (bay, exit, the point clear of the dock's grid): its corridor while it launches or docks. */
    TArray<FVector> DockWay(const class ASCCargoDrone* Drone) const;
    /** How far a lone drone may fly along Flight now (an arclength to stop at; < 0: on): a docked one stays in its bay
     *  while a drone in the air is within the kept distance of its way out; one in the air stops short of the dock
     *  corridor another drone is using (launching, or docking with the right of way: the lower id). */
    double FlightFree(const class ASCCargoDrone* Drone, const struct FSCCargoFlight& Flight) const;
    /** Where a drone on its way out to its box holds short (an arclength of Flight; < 0: nowhere): while a load
     *  planned before its own move has yet to fly through its station or across its way there (its lift, its turn at the top, its route up
     *  to where it is past the station), or another team's drone is within the kept distance of it (hovering there
     *  now, or due there first), the last point of its way that is clear of them. Where it is is in such a load's way
     *  already, with none of its way behind clear (it was given this move where it set its last box down, in the hold the
     *  load comes into): OutRefuge, a place near it out of the loads' ways that it can fly to straight (round 5). */
    double HoldShort(const struct FSCCargoMove& Move, const class ASCCargoDrone* Drone, const struct FSCCargoFlight& Flight, FVector* OutRefuge = nullptr) const;
    /** One tick of a team tugging its box along its route: the box at Q / Rotation moving at RouteV, accelerating at
     *  RouteA (cm). SCCargoTugStep.cpp. */
    void TugStep(struct FSCCargoMove& Move, class ASCCargoContainer* Box, const TArray<class ASCCargoDrone*>& Drones, const FVector& Q, const FQuat& Rotation,
                 const FVector& RouteV, const FVector& RouteA, double Now, double Dt);
public:
    /** A drone's shape (its hull's bounds, level) swept A -> B (actor locations) against the surroundings and every
     *  box; the docks, the drones and a vehicle's own colliders inside its physics grid are left out. True when
     *  blocked, with the fraction flown free and the surface's normal. DroneOverlap: what the shape overlaps at P. */
    bool DroneSweep(const class ASCCargoDrone* Drone, const FVector& A, const FVector& B, double& OutFraction, FVector& OutNormal, FString* OutWhat = nullptr) const;
    bool DroneOverlap(const class ASCCargoDrone* Drone, const FVector& P, FString& OutWhat) const;
    /** The boxes in the air that are not Drone's own (another team's load lifted, flying, being set down or held), with
     *  their velocities: what its step keeps out of the way of (FSCCargoDroneSteer::Evade). */
    void DroneLoads(const class ASCCargoDrone* Drone, TArray<FSCCargoDroneSteer::FLoad, TInlineAllocator<8>>& Out) const;
    /** The box Drone carries (its pose and half size), while that box is in the air: its step keeps off the face its
     *  beam holds (FSCCargoDroneSteer::OffFace). False when it carries none. */
    bool DroneOwnBox(const class ASCCargoDrone* Drone, FTransform& OutPose, FVector& OutHalf) const;
    /** Drone's bay (the pose a drone rests in, facing out of it); false without its dock. */
    bool DroneBay(const class ASCCargoDrone* Drone, FTransform& OutBay) const;
    /** The game's effects the drones and boxes play (WP-K4c; SCCargoFx.h); null before the scenario's tractor is read. */
    class FSCCargoFx* GetFx() const;
    /** A carried box hit something (its body's hit event): against a ship, that ship's collision effect at the contact,
     *  over the threshold and rate limit (SCCargoFx.h). */
    void OnBoxContact(class ASCCargoContainer* Box, AActor* Other, const FHitResult& Hit, const FVector& NormalImpulse);

private:
    /** The move (not ended) of the box that Box is, or null. */
    const struct FSCCargoMove* MoveOf(const AActor* Box) const;
    const struct FSCCargoMove* MoveOfDrone(const class ASCCargoDrone* Drone) const;
    /** The collision world apart from the cargo, hashed per frame: part of a route query's identity. */
    uint32 WorldHash() const;
    /** Drops what a move keeps between ticks (searches, cells, turn points) when the world or the stock changed. */
    void RefreshMove(struct FSCCargoMove& Move) const;
    /** The environment's bounds (invalid without one), per frame. */
    FBox EnvironmentBounds() const;
    /** Where the boxes at rest are (a hash, per frame): part of a route query's identity. */
    uint32 StockAtRestHash() const;
    /** The key a search is kept under: the caller's key and what the query tests. */
    static FString SearchKey(const FString& Key, const struct FSCCargoRouteQuery& Query);
    double CeilingZ() const;
};
