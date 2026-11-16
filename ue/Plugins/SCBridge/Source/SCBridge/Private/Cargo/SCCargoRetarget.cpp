// USCCargoSubsystem: what a load in the air decides on the way (split from SCCargoMoves.cpp): the right of way
// between flying loads, the guard's sweep of the stretch ahead, a destination slot checked again against the grid as it
// is now, a new route from where the load is (searched over frames), and drones sent home.
#include "Cargo/SCCargoSubsystem.h"
#include "SCCargoState.h"
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoDrone.h"
#include "Cargo/SCCargoFlightPath.h"
#include "Cargo/SCCargoGridComponent.h"
#include "Engine/World.h"

using EStep = FSCCargoMove::EStep;
namespace
{
constexpr double HoldExitCm = 600; // a hold's entrance waypoint lies this far outside its lattice
constexpr double HoverProbeCm = 30;    // a hold cell's probe reaches this far into the cell above (a box's hover)
constexpr double HoldProbeCm = 100; // half-size of the probe that finds a hold's entrance side
}

void USCCargoSubsystem::SendHome(FSCCargoMove& M, double Now)
{
    M.bRoutePending = false;
    M.DroneFlights.Reset();
    M.FlightAt.Reset();
    const FSCCargoJob* Job = State->Jobs.FindByPredicate([&](const FSCCargoJob& J) { return J.Moves.ContainsByPredicate([&](const FSCCargoMove& O) { return &O == &M; }); });
    for (const int32 Id : M.Drones)
    {
        ASCCargoDrone* D = FindDrone(*State, Id);
        FSCCargoFlight& F = M.DroneFlights.AddDefaulted_GetRef();
        if (!D) continue;
        D->SetBeam(false);
        D->CarryingBoxId = INDEX_NONE;
        D->State = ESCCargoDroneState::Returning;
        D->MoveId = INDEX_NONE; // free for the swarm while it flies home
        // No home route yet (its search goes on next frame, or nothing is free now): the flight stays unstarted, so the
        // drone hovers where it is and plans again (TickMove, Returning). A one-point flight here would count as
        // arrived and put the drone straight into its bay, from wherever it is.
        if (Job) HomeFlight(*Job, M, D, F);
    }
    M.Step = EStep::Returning;
    M.StepStart = Now;
    M.NextRetry = M.bRoutePending ? Now : Now + 2; // a home search not finished this frame goes on next frame
}

// Right of way between loads: the load with the lower box id goes first. True when a load with the right of way is
// flying within reach of the stretch A..B this one would take next (it waits; the other routes around it). At lift-off
// it is first come, first served (bAnyLoad): a box stays on its grid while any flying load is near its way up.
bool USCCargoSubsystem::RightOfWayYield(const FSCCargoMove& M, int32 BoxId, const ASCCargoContainer* A, const FVector& PA, const FVector& PB, bool bAnyLoad) const
{
    const FVector Size = A ? A->SizeCm() : FVector::ZeroVector;
    // As far as either load's drones may be out from it: their beams' full-strength distance and the drone itself.
    const double Reach = Size.Size() / 2 + FMath::Max(M.HangCm, State->Scenario.Drone.FullStrengthDistanceM * 100.0) + SCCargoFlightTuning::DroneHeightCm;
    for (const auto& Pair : State->Boxes)
    {
        const FSCCargoBox& O = Pair.Value;
        const ASCCargoContainer* OA = O.Actor.Get();
        if (O.Id == BoxId || (!bAnyLoad && O.Id > BoxId) || !OA || !SCCargoIsFlying(O.State)) continue;
        if (FMath::PointDistToSegment(OA->GetActorLocation(), PA, PB) < Reach + OA->SizeCm().Size() / 2 + 100) return true;
    }
    return false;
}

// Inside the margin of a flying load (one giving way to this): flying loads are left out of the free space.
bool USCCargoSubsystem::InsideWaitingLoadAt(const FSCCargoMove& M, const ASCCargoContainer* A, const FVector& P) const
{
    return LoadQuery(M, A).Blocked(P) && !LoadQuery(M, A, true).Blocked(P);
}

// The guard's sweep of the stretch ahead. Within something's margin (climbing out of a placed neighbour's, as planned)
// the sweep cannot tell: the planned climb is flown.
bool USCCargoSubsystem::StretchAheadFree(const FSCCargoMove& M, const ASCCargoContainer* A, const FVector& Here, const FVector& Ahead) const
{
    const FSCCargoRouteQuery Guard = LoadQuery(M, A, InsideWaitingLoadAt(M, A, Here));
    if (Guard.Blocked(Here) || Guard.Free(Here, Ahead)) return true;
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_GUARD move=%d box=%d here=%s ahead=%s box_at=%s hits=%s"), M.MoveId, M.Plan.BoxId, *Here.ToCompactString(), *Ahead.ToCompactString(),
           *A->GetActorLocation().ToCompactString(), *Guard.Describe(Here, Ahead));
    return false;
}

// The destination slot checked against the grid as it is now: kept while free and supported, else the best free slot
// (its reservation moves with it). False when nothing fits.
bool USCCargoSubsystem::ReSlotMove(const FSCCargoJob& Job, FSCCargoMove& M, const FSCCargoBox* Box, USCCargoGridComponent* To, const TCHAR* Why)
{
    if (!To || !Box) return false;
    FSCCargoItem Item;
    Item.Id = Box->Id;
    Item.SizeM = Box->SizeM();
    Item.SCU = Box->Occupancy.StandardCargoUnits;
    Item.MassKg = Box->Occupancy.Mass;
    Item.SubType = Box->Occupancy.ItemSubType;
    Item.Commodity = Box->Commodity;
    FSCCargoPlacement Back;
    if (!FSCCargoPlanner::ReturnSlot(To->Model(), Item, SCCargoSlotOf(M.Plan), Back)) return false;
    if (Back.Origin == M.Plan.Origin && Back.Cells == M.Plan.Cells && Back.bRotated == M.Plan.bRotated) return true;
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_RESLOT job=%d move=%d box=%d reason=%s slot=%s planned=%s"), Job.Id, M.MoveId, Box->Id, Why,
           *Back.Origin.ToString(), *M.Plan.Origin.ToString());
    To->Placed.RemoveAll([&](const FSCCargoPlacement& P) { return P.ItemId == Box->Id; });
    To->Placed.Add(Back);
    To->Reserved.Add(Box->Id);
    M.Plan.Origin = Back.Origin;
    M.Plan.Cells = Back.Cells;
    M.Plan.bRotated = Back.bRotated;
    return true;
}

// A new route for the load from where it is to its goal (blocked ahead, or a hold flying back); false keeps it in place.
// A search not finished this frame keeps the load where it is (false, Move.bRoutePending) and goes on next tick; only
// finished attempts count against the replan limit.
bool USCCargoSubsystem::ReplanLoad(const FSCCargoJob& Job, FSCCargoMove& M, const FSCCargoBox* Box, ASCCargoContainer* A, USCCargoGridComponent* To,
                                   const FVector& From, const TCHAR* Why, double Now)
{
    RefreshMove(M);
    // A new search (none under way): the cells sampled before may have been taken or left by other teams' drones since
    // (the space kept for them is no part of a query's identity).
    if (!M.bReplanning)
    {
        M.bReplanning = true;
        M.bKeptHit = false;
        M.CellCaches.Empty();
    }
    const int64 KeptBefore = *State->KeptHits;
    const FSCCargoDroneDefinition& Def = State->Scenario.Drone;
    const FVector Up = FVector::UpVector;
    const FSCCargoRouteQuery Load = LoadQuery(M, A);
    // Boxes placed since the move was planned (a neighbour landed in its slot): the hover point over the slot moves
    // out along the grid's normal until the hanging load fits there again, and a load stopped within a placed box's
    // margin first rises out of it (a short climb, the only way it can go).
    const FVector GoalNormal = M.Plan.Kind == ESCCargoMoveKind::Hold && !M.bRestoring ? Up : M.ToNormal;
    const bool bToEntrance = !M.SecondPath.IsEmpty(); // first leg into a hold: to its entrance
    if (!bToEntrance && Load.Blocked(M.Goal))
    {
        if (M.Plan.Kind == ESCCargoMoveKind::Hold && !M.bRestoring)
        {
            for (double Off = 25; Load.Blocked(M.Goal) && Off <= 500; Off += 25)
                if (!Load.Blocked(M.Goal + GoalNormal * Off)) M.Approach = M.Goal += GoalNormal * Off;
        }
        else if (To)
        {
            const FTransform Slot = To->SlotToWorld(SCCargoSlotOf(M.Plan), Box->SizeM());
            FVector Turn, Hover;
            if (SlotApproach(M, A, Slot, M.ToNormal, A->GetActorQuat(), FVector::DotProduct(M.Approach - Slot.GetLocation(), M.ToNormal), Turn, Hover))
            {
                M.Goal = Turn;
                M.Approach = Hover;
            }
        }
    }
    // Out of the margin: up under gravity; in zero g a box may lie above it (an underside plate), so out along the plate's
    // normal or back the way it came first (FSCCargoRoute::EscapeStart).
    FVector Start = From;
    FSCCargoRoute::EscapeStart(From, Load, Gravity() > 0, M.ToNormal, -M.Carry.Tangent(), Start);
    TArray<FVector> Path;
    FSCCargoRouteQuery Cached = Load;
    const FVector Target = bToEntrance ? M.SecondPath[0] : M.Goal;
    bool bFound = PlanRoute(Job, M, TEXT("replan"), Start, Target, Cached, Path, TEXT("replan"), true);
    if (bFound && Start != From) Path.Insert(From, 0);
    // Stopped inside the margin of a load that gives way to this one (it waits): plan as if flying loads were not
    // there; the right of way keeps the two apart.
    if (!bFound && !M.bRoutePending && InsideWaitingLoadAt(M, A, From))
        bFound = PlanRoute(Job, M, TEXT("replan_through_waiting"), From, Target, LoadQuery(M, A, true), Path, FString(), true);
    M.bKeptHit |= *State->KeptHits != KeptBefore;
    if (!bFound && M.bRoutePending)
    {
        M.NextRetry = Now; // the search goes on next frame
        return false;
    }
    M.bReplanning = false;
    for (auto It = M.Searches.CreateIterator(); It; ++It)
        if (It->Key.StartsWith(TEXT("replan"))) It.RemoveCurrent();
    M.Searches.Remove(TEXT("replan_through_waiting"));
    // Stopped by space kept for another team's drones alone: they move on, and it is not counted against the limit.
    const bool bKept = !bFound && M.bKeptHit;
    if (!bKept) ++M.Replans;
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_REPLAN job=%d move=%d box=%d reason=%s found=%d replans=%d%s"), Job.Id, M.MoveId, M.Plan.BoxId, Why, bFound, M.Replans,
           bKept ? TEXT(" waits_for_drones=1") : TEXT(""));
    if (!bFound) return false;
    M.FromRotation = A->GetActorQuat();
    M.Carry.Start(Path, Def.MaxSpeedMS * 100.0, M.LoadAccel, M.LoadJerk);
    return true;
}

const USCCargoGridComponent* USCCargoSubsystem::HoldAt(const FVector& P) const
{
    for (const auto& Pair : State->Grids)
        if (const USCCargoGridComponent* G = Pair.Value.Get(); G && G->bAccessRule && G->WorldBounds().ExpandBy(100).IsInside(P))
            return G;
    return nullptr;
}

bool USCCargoSubsystem::HoldExit(const USCCargoGridComponent* Hold, const FSCCargoRouteQuery& Query, double Z, int32 Side, FVector& OutPoint, FVector& OutDir) const
{
    // From the hold's middle at height Z straight out of lattice side Side (+x, -x, +y, -y), a few metres beyond it:
    // true when the shape sweeps there free (that side is open to it).
    const FTransform Frame = Hold->LatticeToWorld();
    FVector C = Hold->WorldCenter();
    C.Z = Z;
    const FVector Half = Hold->Lattice.InteriorM * 50.0;
    const FVector Axis = Frame.GetUnitAxis(Side < 2 ? EAxis::X : EAxis::Y) * (Side % 2 == 0 ? 1.0 : -1.0);
    OutDir = FVector(Axis.X, Axis.Y, 0).GetSafeNormal();
    OutPoint = C + OutDir * ((Side < 2 ? Half.X : Half.Y) + HoldExitCm);
    return !Query.Blocked(OutPoint) && Query.Free(C, OutPoint);
}

int32 USCCargoSubsystem::EntranceAxis(USCCargoGridComponent* G) const
{
    if (!G || !G->bAccessRule || G->Access != ESCCargoGridAccess::Open) return 0;
    if (G->EntranceAxis >= 0) return G->EntranceAxis;
    const FSCCargoRouteQuery Probe = RouteQuery(FVector(HoldProbeCm), FVector::ZeroVector, nullptr, -1e9, 1e9);
    FVector C = G->WorldCenter();
    C.Z = G->WorldBounds().Min.Z + HoldProbeCm * 2; // low in the hold, where the boxes go in
    const FTransform Frame = G->LatticeToWorld();
    const FVector Half = G->Lattice.InteriorM * 50.0;
    G->EntranceAxis = 0;
    for (int32 Axis = 1; Axis <= 2 && !G->EntranceAxis; ++Axis)
        for (const double Sign : {1.0, -1.0})
        {
            const FVector D = Frame.GetUnitAxis(Axis == 1 ? EAxis::X : EAxis::Y) * Sign;
            const FVector Out = C + FVector(D.X, D.Y, 0).GetSafeNormal() * ((Axis == 1 ? Half.X : Half.Y) + HoldExitCm);
            if (Probe.Free(C, Out)) { G->EntranceAxis = Axis; break; }
        }
    // The hold's own structure inside its lattice (slanted walls, ribs, fittings): a cell whose volume, or the hover above
    // a box standing in it, meets the ship is not usable (a box goes in and down only where it fits).
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCCargoHoldCells), /*bTraceComplex=*/true);
    for (const auto& Pair : State->Boxes) Params.AddIgnoredActor(Pair.Value.Actor.Get());
    for (const TWeakObjectPtr<ASCCargoDrone>& D : State->Drones) Params.AddIgnoredActor(D.Get());
    for (const FSCCargoDock& D : State->Docks) Params.AddIgnoredActor(D.Actor.Get());
    const FVector CellCm = G->Lattice.CellM * 100.0;
    const FCollisionShape Cell = FCollisionShape::MakeBox(FVector(CellCm.X / 2 - 5, CellCm.Y / 2 - 5, CellCm.Z / 2 + HoverProbeCm / 2 - 5));
    const TSharedRef<FSCCargoHullRule> CellHulls = HullRule(Params, Cell.GetExtent(), Frame.GetRotation());
    int32 Structure = 0;
    for (int32 Z = 0; Z < G->Lattice.Cells.Z; ++Z)
        for (int32 Y = 0; Y < G->Lattice.Cells.Y; ++Y)
            for (int32 X = 0; X < G->Lattice.Cells.X; ++X)
            {
                const FVector Local = (FVector(X, Y, Z) + FVector(0.5)) * CellCm + FVector(0, 0, HoverProbeCm / 2);
                if (!GetWorld()->OverlapAnyTestByObjectType(Frame.TransformPosition(Local), Frame.GetRotation(), SCCargoObstacles(), Cell, CellHulls->At(Frame.TransformPosition(Local)))) continue;
                FSCCargoPlacement P;
                P.ItemId = SCCargoStructureItemBase - (X + G->Lattice.Cells.X * (Y + G->Lattice.Cells.Y * Z));
                P.Origin = FIntVector(X, Y, Z);
                P.Cells = FIntVector(1);
                if (!G->IsFreeCell(P.Origin)) continue;
                G->Placed.Add(P);
                ++Structure;
            }
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_HOLD_ENTRANCE grid=%d axis=%d structure_cells=%d of %d"), G->GridId, G->EntranceAxis, Structure, G->Lattice.NumCells());
    return G->EntranceAxis;
}
