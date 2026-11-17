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
class SCBRIDGE_API USCCargoSubsystem : public UTickableWorldSubsystem
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
    /** Verification (sc.Cargo.Interrupt): the first load in flight (of JobId; any job when INDEX_NONE) fails as a lost route
     *  would; false when none is in flight. */
    bool InterruptLoad(int32 JobId);
    /** A job's state (any job of the session, finished ones too); false when there is no such job. */
    bool FindJobState(int32 JobId, ESCCargoJobState& OutState, FText& OutReason) const;

protected:
    int32 Revision = 0;

private:
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
    class USCCargoGridComponent* FindGrid(int32 GridId) const;
    class USCCargoGridComponent* SelectGrid(const struct FSCCargoGridSelector& Selector) const;
    struct FSCCargoPlanInput PlanInput(const FSCCargoTransferRequest& Request, bool bIgnoreOtherJobs = false) const;
    /** The reasons a requester cannot use some of the request (grids or boxes of other owners). */
    void OwnerRefusals(const FSCCargoTransferRequest& Request, FSCCargoTransferPreview& Preview) const;
    /** Moves, reservations and labels of a job from its plan (Submit, or a queued job whose turn came). */
    void Materialize(struct FSCCargoJob& Job, const FSCCargoTransferPreview& Plan);
    int32 SpawnBox(class USCCargoGridComponent* Grid, const FString& Entity, FName Commodity, const FText& Label, const TOptional<FIntVector>& Origin = {}, bool bRotated = false);
    void SpawnDock(class USCCargoGridComponent* Grid, int32 Drones);
    void UpdateAccess(class USCCargoGridComponent* Grid, double Now);
    bool RequestAccess(class USCCargoGridComponent* Grid, bool bOpen, double Now);
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
    /** The queries for the hanging load of Move (box + beam + drones above it) and for a lone drone. */
    struct FSCCargoRouteQuery LoadQuery(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, bool bIgnoreFlying = false,
                                        const FQuat* Rotation = nullptr) const;
    /** The turn onto the destination lattice in free air, swept: the load shape at rotations from FromQ to ToQ (steps of
     *  at most TurnStepDegrees) while moving from FromP to ToP, all free. OutWhy names the first blocker. */
    bool TurnClear(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, const FVector& FromP, const FQuat& FromQ, const FVector& ToP,
                   const FQuat& ToQ, FString* OutWhy = nullptr) const;
    /** The descent from the hover point onto the slot along the grid normal, at the slot's rotation: the box alone,
     *  2 cm inside its own size (it ends flush with its neighbours and the grid). */
    bool DescentClear(const class ASCCargoContainer* Box, const FQuat& Rotation, const FVector& From, const FVector& To, FString* OutWhy = nullptr) const;
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
    /** The drones and beams over Move's box hanging at Rotation (a level box along the formation), MarginCm around them. */
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
    bool EnvironmentAccess(class USCCargoGridComponent* Grid, bool bOpen);
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
    /** The tractor record as a beam (Cargo/SCCargoTug.h), and the shortest beam that holds a load's weight shared by a
     *  team (cm): what a load's route reserves over the box for its drones, since they can tuck in that far. */
    struct FSCCargoBeam TractorBeam() const;
    double MinHangCm(double LoadKg, int32 Team) const;
    /** The acceleration (m/s^2) a loaded team's route is flown at, along it and across it (in its bends): what the
     *  team can give straight up, where its beams have the least to spare, less the scenario's min_load_accel kept
     *  for the springs to correct with, over root two (so both at once stay within it). */
    double RouteAccel(int32 Team, double LoadKg) const;
    double MinHangCm(const class ASCCargoContainer* Box) const;
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
     *  planned before its own move has yet to fly through its station (its lift, its turn at the top, its route up
     *  to where it is past the station), or another team's drone is within the kept distance of it (hovering there
     *  now, or due there first), the last point of its way that is clear of them. */
    double HoldShort(const struct FSCCargoMove& Move, const class ASCCargoDrone* Drone, const struct FSCCargoFlight& Flight) const;
    /** The room along a drone's beam with the box centre at Q on Move's route (cm from the attach point, Attach from Q;
     *  the beam along Dir): its shape swept out from the shortest beam to the full-strength distance, against the
     *  surroundings and the boxes at rest. OutTucked: where the shape's centre is on the shortest beam. */
    double BeamRoom(const struct FSCCargoMove& Move, const class ASCCargoContainer* Box, const class ASCCargoDrone* Drone, const FVector& Q, const FVector& Attach,
                    const FVector& Dir, double ShortestCm, FVector* OutTucked = nullptr) const;
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
