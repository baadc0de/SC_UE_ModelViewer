// Cargo subsystem (WP-K1): discovers inventory-container grids on the loaded vehicle and environment, owns cargo
// boxes, drones and transfer jobs, and exposes read-only views plus transfer preview/submit/cancel to the UI and to
// `sc.Cargo.*` console commands. The UI uses only this API and SCCargoTypes.h.
#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Cargo/SCCargoTypes.h"
#include "SCCargoSubsystem.generated.h"

class UPrimitiveComponent;

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

protected:
    int32 Revision = 0;

private:
    TSharedPtr<struct FSCCargoState> State;
    void Discover();
    void DiscoverOn(class ASCVehicle* Actor, ESCCargoGridKind Kind);
    void ForgetGrids(ESCCargoGridKind Kind, const class ASCVehicle* Only = nullptr);
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
                                         const struct FSCCargoMove* Move = nullptr, const class ASCCargoDrone* Drone = nullptr) const;
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
    struct FSCCargoRouteQuery DroneQuery(const class ASCCargoDrone* Drone) const;
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
    FVector Departure(const class ASCCargoDrone* Drone, const FVector& BayExit) const;
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
     *  Rotation) is in a vehicle's physics grid, that vehicle's; elsewhere none (WP-P3). The vehicles Move loads or unloads
     *  (its grids', its drones' docks', Drone's dock's) are left out for it everywhere. */
    TSharedRef<struct FSCCargoHullRule> HullRule(const struct FCollisionQueryParams& Base, const FVector& HalfExtent, const FQuat& Rotation,
                                                 const struct FSCCargoMove* Move = nullptr, const class ASCCargoDrone* Drone = nullptr) const;
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
