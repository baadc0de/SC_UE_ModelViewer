// USCCargoSubsystem flight planning against the scene's collision: routes searched over frames with their proof
// (CARGO_PATH: found, how the search ended, minimum clearance, sweep hits, its cost per frame; CARGO_PATH_FRONT: what each
// front of a failed search reached), and a move's preparation before anything flies: lift along the source normal, the
// load's route to its hold point or approach (into a hold: to a turn point outside it that the load can reach), the
// drones' routes out to the box. Nothing flies a route that was not found free. Query shapes: SCCargoQueries.cpp.
// How the drones hold the box on the way is planned in turn (WP-K4 rounds 4 and 5, CARGO_GRIP / CARGO_GRIP_TIER, TierOrder):
// over it with a drone's clearance kept over their hulls; else from its ends (a lone drone at the end trailing its route,
// pushing, or a pair at both, low; swapped to them after the lift or the turn at a hold's entrance, and back over the box
// where there is room before its slot, or kept down onto it); else over it as tight as it fits. That planned place is in
// the route's shape; on the way each drone re-picks its own, the ends mostly, where they fit (SCCargoGrip.cpp).
#include "Cargo/SCCargoSubsystem.h"
#include "HAL/PlatformTime.h"
#include "SCCargoState.h"
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoDrone.h"
#include "Cargo/SCCargoGridComponent.h"
#include "SCVehicle.h"
#include "SCViewerSubsystem.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "GameFramework/PlayerController.h"

using namespace SCCargoFlightTuning;

namespace
{
constexpr double SearchRestartCm = 100; // a search whose start moved further than this begins again
constexpr double EntranceStepCm = 200;  // a hold's turn point is looked for outwards in steps of this
constexpr double MaxEntranceOutCm = 4000; // ... up to this far beyond the first point outside the hold (viewer choice)
constexpr double BesideRampKeeps = 5;   // a teammate's route eases away from the first's over this many kept distances
constexpr double PlanRetrySeconds = 1;  // a plan stopped by space kept for other teams' drones is tried again after this
FSCCargoPlacement SlotOf(const FSCCargoMovePlan& P)
{
    FSCCargoPlacement S;
    S.ItemId = P.BoxId;
    S.Origin = P.Origin;
    S.Cells = P.Cells;
    S.bRotated = P.bRotated;
    return S;
}
} // namespace

FString USCCargoSubsystem::SearchKey(const FString& Key, const FSCCargoRouteQuery& Query)
{
    return Key + TEXT("|") + Query.Identity;
}

bool USCCargoSubsystem::PlanRoute(const FSCCargoJob& Job, FSCCargoMove& M, const TCHAR* What, const FVector& Start, const FVector& Goal,
                                  const FSCCargoRouteQuery& Query, TArray<FVector>& OutPath, const FString& Key, bool bKeep)
{
    // Resume the search under Key (begun again when its ends moved, or when what the query tests changed: its identity is
    // part of the key), within what is left of this frame's budget.
    M.bRoutePending = false;
    M.RouteEnd = ESCCargoRouteEnd::None;
    if (State->SearchFrame != GFrameCounter)
    {
        State->SearchFrame = GFrameCounter;
        State->SearchSpent = 0;
    }
    const FString K = SearchKey(Key.IsEmpty() ? FString(What) : Key, Query);
    TSharedPtr<FSCCargoRouteSearch>& Search = M.Searches.FindOrAdd(K);
    if (!Search || FVector::Dist(Search->Start, Start) > SearchRestartCm || FVector::Dist(Search->Goal, Goal) > 10)
    {
        Search = MakeShared<FSCCargoRouteSearch>();
        Search->Begin(Start, Goal, Query);
    }
    else if (Search->IsDone())
    {
        // Kept from an earlier tick (reported then): the same answer.
        const bool bFound = Search->Result.bFound;
        M.RouteEnd = Search->Result.End;
        if (bFound) OutPath = Search->Result.Samples;
        if (!bKeep) M.Searches.Remove(K); // Search (a reference into the map) is gone after this
        return bFound;
    }
    if (!Search->IsDone())
    {
        const double Began = FPlatformTime::Seconds();
        const bool bDone = Search->Step(FMath::Max(FSCCargoState::SearchBudgetSeconds - State->SearchSpent, 0.0));
        State->SearchSpent += FPlatformTime::Seconds() - Began;
        if (!bDone)
        {
            M.bRoutePending = true;
            return false;
        }
    }
    const FSCCargoRoute Route = Search->Result;
    const double SearchMs = Search->Seconds * 1000, MaxStepMs = Search->MaxStepSeconds * 1000;
    const int32 Steps = Search->Steps;
    State->SearchMaxStepMs = FMath::Max(State->SearchMaxStepMs, MaxStepMs);
    ++State->SearchesReported;
    if (!bKeep) M.Searches.Remove(K);
    M.RouteEnd = Route.End;
    // The route's clearance and hits were measured by the search itself, within its budget (search_ms and max_frame_ms
    // include it).
    const int32 Hits = Route.Hits;
    const double Clearance = Route.Clearance;
    // found=0: length, clearance and hits say nothing (the samples are the straight line; see CARGO_PATH_FRONT).
    UE_LOG(LogSCCargo, Display,
           TEXT("CARGO_PATH job=%d move=%d box=%d leg=%s found=%d end=%s direct=%d waypoints=%d expansions=%d length_m=%.1f minClearanceCm=%.0f hits=%d search_ms=%.1f "
                "frames=%d max_frame_ms=%.2f"),
           Job.Id, M.MoveId, M.Plan.BoxId, What, Route.bFound, FSCCargoRoute::EndName(Route.End), Route.bDirect, Route.Waypoints.Num(), Route.Expansions,
           Route.bFound ? Route.Length() / 100 : 0.0, Clearance, Hits, SearchMs, Steps, MaxStepMs);
    if (!Route.bFound)
    {
        // Why: whether an end is inside something, and what each front reached: how much it searched, the bounds of what it
        // could reach, the reached point nearest the other end and what stops the load going straight on from there.
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_PATH_BLOCKED job=%d move=%d leg=%s end=%s start=%s goal=%s start_blocked=%d goal_blocked=%d straight_line_hits=%s"), Job.Id,
               M.MoveId, What, FSCCargoRoute::EndName(Route.End), *Start.ToCompactString(), *Goal.ToCompactString(), Query.Blocked(Start), Query.Blocked(Goal),
               Query.Describe ? *Query.Describe(Start, Goal) : TEXT("?"));
        auto Front = [&](const TCHAR* Side, const FSCCargoRouteFront& F, const FVector& Other) {
            if (!F.Expansions) return;
            UE_LOG(LogSCCargo, Display,
                   TEXT("CARGO_PATH_FRONT job=%d move=%d leg=%s side=%s expansions=%d reached_min=%s reached_max=%s nearest=%s nearest_to_other_m=%.1f blocked_cells=%d "
                        "blocked_steps=%d onward_hits=%s"),
                   Job.Id, M.MoveId, What, Side, F.Expansions, *F.Reached.Min.ToCompactString(), *F.Reached.Max.ToCompactString(), *F.Nearest.ToCompactString(),
                   FVector::Dist(F.Nearest, Other) / 100, F.BlockedCells, F.BlockedSteps, Query.Describe ? *Query.Describe(F.Nearest, Other) : TEXT("?"));
        };
        Front(TEXT("start"), Route.FromStart, Goal);
        Front(TEXT("goal"), Route.FromGoal, Start);
        return false;
    }
    OutPath = Route.Samples;
    return true;
}

FVector USCCargoSubsystem::Departure(ASCCargoDrone* D, const FVector& Exit) const
{
    // Where a drone leaving (or entering) its bay is clear of the grid its dock stands on: along that grid's normal by
    // its routing shape (DroneQuery: the hull's bounds, level, half the clearance and the gusts around them) as far
    // as that reaches along the normal, and the clearance; further out while that shape does not fit there (a box
    // standing beside the dock: the drone rises past it before its free route begins). Kept on the drone: the
    // end of its dock's corridor.
    const FSCCargoDock* Dock = State->Docks.FindByPredicate([&](const FSCCargoDock& K) { return K.ItemId == D->DockItem; });
    const USCCargoGridComponent* Grid = Dock ? FindGrid(Dock->GridId) : nullptr;
    const FVector N = Grid ? Grid->LatticeToWorld().GetRotation().GetUpVector() : FVector::UpVector;
    const FVector Half = D->HullHalf + FVector(DroneClearanceCm / 2 + SwayEnvelopeCm());
    const double Least = FMath::Abs(N.X) * Half.X + FMath::Abs(N.Y) * Half.Y + FMath::Abs(N.Z) * Half.Z + DroneClearanceCm;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCCargoDeparture), /*bTraceComplex=*/true, D);
    for (const TWeakObjectPtr<ASCCargoDrone>& Other : State->Drones) Params.AddIgnoredActor(Other.Get());
    for (const FSCCargoDock& K : State->Docks) Params.AddIgnoredActor(K.Actor.Get());
    if (const APlayerController* PC = GetWorld()->GetFirstPlayerController(); PC && PC->GetPawn()) Params.AddIgnoredActor(PC->GetPawn());
    for (const auto& Pair : State->Boxes)
        if (SCCargoIsFlying(Pair.Value.State)) Params.AddIgnoredActor(Pair.Value.Actor.Get());
    const TSharedRef<FSCCargoHullRule> Hulls = HullRule(Params, Half, FQuat::Identity);
    D->DockClear = Exit + N * Least;
    for (double Off = Least; Off <= Least + MaxApproachCm; Off += 25)
    {
        const FVector P = Exit + N * Off + D->HullCentre;
        if (GetWorld()->OverlapAnyTestByObjectType(P, FQuat::Identity, SCCargoObstacles(), FCollisionShape::MakeBox(Half), Hulls->At(P))) continue;
        D->DockClear = Exit + N * Off;
        break;
    }
    D->bDockClear = true;
    return D->DockClear;
}

bool USCCargoSubsystem::HomeFlight(const FSCCargoJob& Job, FSCCargoMove& M, ASCCargoDrone* D, FSCCargoFlight& Out)
{
    RefreshMove(M);
    const FSCCargoDroneDefinition& Def = State->Scenario.Drone;
    const FSCCargoDock* Dock = State->Docks.FindByPredicate([&](const FSCCargoDock& K) { return K.ItemId == D->DockItem; });
    const ASCCargoDroneDock* DockActor = Dock ? Dock->Actor.Get() : nullptr;
    if (!DockActor) return false;
    TArray<FVector> Path;
    const FVector Exit = DockActor->BayExit(D->Bay), Clear = Departure(D, Exit);
    const FVector Ends[] = {D->GetActorLocation(), Clear};
    FSCCargoRouteQuery Query = DroneQuery(D, &M, Ends);
    if (Query.Blocked(Ends[0]) || Query.Blocked(Ends[1])) Query = DroneQuery(D, &M, Ends, /*bTight=*/true); // an end within the margin: the bare hull
    Query.CellCache = M.Cells(Query.Identity);
    FVector Start = D->GetActorLocation();
    TArray<FVector> Via;
    if (!ClearStart(D, Query, Start, &Via) || !PlanRoute(Job, M, TEXT("home"), Start, Clear, Query, Path, FString::Printf(TEXT("home:%d"), D->DroneId), false))
    {
        if (M.bRoutePending) M.NextRetry = 0; // the search goes on next frame
        return false;
    }
    if (!Start.Equals(D->GetActorLocation(), 1.0))
    {
        Path.Insert(Via, 0);
        Path.Insert(D->GetActorLocation(), 0);
    }
    Path.Add(Exit);
    Path.Add(DockActor->BayTransform(D->Bay).GetLocation());
    Out.Start(Path, Def.MaxSpeedMS * 100.0, Def.MaxAccelMS2 * 100.0, Def.MaxJerkMS3 * 100.0);
    return true;
}

bool USCCargoSubsystem::EnvironmentAccess(USCCargoGridComponent* Grid, bool bOpen)
{
    // FSCEnvironmentAccess opens its way: an environment's at discovery (kept open on re-pose), a ship's hold on this first
    // request (RequestAccess); a job waits until that is done.
    return RequestAccess(Grid, bOpen, GetWorld()->GetTimeSeconds());
}

bool USCCargoSubsystem::PrepareMove(FSCCargoJob& Job, FSCCargoMove& M, FString& Why)
{
    // Planned (PlanMove) against the space kept for other teams' drones as it is now. A plan that fails where only
    // that space stopped it is not a failure of the box: the drones move on, and it is planned again then.
    const double Now = GetWorld()->GetTimeSeconds();
    if (Now < M.PlanRetryAt)
    {
        M.bRoutePending = true;
        return false;
    }
    const int64 Before = *State->KeptHits;
    const bool bPlanned = PlanMove(Job, M, Why);
    M.bKeptHit |= *State->KeptHits != Before;
    if (bPlanned)
    {
        M.PlannedAt = Now;
        M.bKeptHit = false;
        return true;
    }
    if (M.bRoutePending || !M.bKeptHit) return false;
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_PLAN_WAIT job=%d move=%d box=%d planned again shortly (space kept for other teams' drones, or its own drone has no way from where it hovers): \"%s\""), Job.Id, M.MoveId, M.Plan.BoxId, *Why);
    M.bKeptHit = false;
    M.PlanRetryAt = Now + PlanRetrySeconds;
    M.Searches.Empty();
    M.CellCaches.Empty();
    M.TurnPoints.Empty();
    M.EndsFinishes.Empty();
    M.Entrances.Empty();
    M.EntranceOut = 0;
    M.bRefine = false;
    M.Tier = M.TierStep = 0; // every way of holding it again
    M.bRoutePending = true;
    return false;
}

bool USCCargoSubsystem::PlanMove(FSCCargoJob& Job, FSCCargoMove& M, FString& Why)
{
    RefreshMove(M);
    M.bRoutePending = false; // set again by a search not finished this frame
    const FSCCargoDroneDefinition& Def = State->Scenario.Drone;
    FSCCargoBox* Box = State->Boxes.Find(M.Plan.BoxId);
    ASCCargoContainer* A = Box ? Box->Actor.Get() : nullptr;
    USCCargoGridComponent* From = FindGrid(M.Plan.FromGrid);
    USCCargoGridComponent* To = FindGrid(M.Plan.ToGrid);
    TArray<ASCCargoDrone*> Drones;
    for (const int32 Id : M.Drones)
        if (ASCCargoDrone* D = FindDrone(*State, Id)) Drones.Add(D);
    // Loose cargo (WP-K2) has no source grid: it is lifted straight up from where it lies and levelled at the top.
    const bool bLoose = M.bLooseSource && !From;
    if (!A || (!From && !bLoose) || !To || Drones.IsEmpty()) { Why = TEXT("the box, its grids or its drones are gone"); return false; }
    const FString FromLabel = From ? From->GridLabel.ToString() : LocationLabel(M.Plan.FromGrid).ToString();
    const FVector Size = A->SizeCm();
    const int32 N = Drones.Num();
    M.FromNormal = From ? From->LatticeToWorld().GetRotation().GetUpVector() : FVector::UpVector;
    M.ToNormal = To->LatticeToWorld().GetRotation().GetUpVector();
    M.FromRotation = M.PickRotation = A->GetActorQuat();
    M.TurnStart = -1;
    // The load's envelope reserves its drones at the shortest beam that holds the box (they can tuck in that far);
    // in flight each drone pays its beam out where it finds room (TugStep).
    double Lift = 0, Hang = 0;
    LiftPlan(A, A->GetActorTransform(), M.FromNormal, Lift, Hang);
    M.HangCm = MinHangCm(Box->Occupancy.Mass, N);
    const FTransform Slot = To->SlotToWorld(SlotOf(M.Plan), Box->SizeM());
    const bool bHold = M.Plan.Kind == ESCCargoMoveKind::Hold;
    const double LoadAccel = RouteAccel(N, Box->Occupancy.Mass);
    M.LoadAccel = LoadAccel * 100.0;
    M.LoadJerk = Def.MaxJerkMS3 * 100.0 * LoadAccel / FMath::Max<double>(Def.MaxAccelMS2, 1e-3);
    // Formation along the box's long side (the load's route reserves room for its spread).
    const bool bLongX = Size.X >= Size.Y;
    const double Long = bLongX ? Size.X : Size.Y;
    const double Spacing = N > 1 ? FMath::Max(Long * 0.6 / (N - 1), Def.FormationSpacingM * 100.0) : 0;
    M.Offsets.Reset();
    for (int32 I = 0; I < N; ++I)
    {
        const double Along = (I - (N - 1) / 2.0) * Spacing;
        M.Offsets.Add(bLongX ? FVector(Along, 0, 0) : FVector(0, Along, 0));
    }
    // How the drones hold it on the way (WP-K4 round 5), tried in turn: 0 from its ends (a team of one or two, low; held
    // over, with a drone's clearance over their hulls, where it is lifted), 1 over it with that clearance (bRoomy), 2 over
    // it as tight as it fits (the old way), in the order TierOrder gives: over with room first (the planned place is the
    // fallback; every route ends beside boxes standing flush), the ends where that finds no way. On the way each drone
    // holds from where it picks (SCCargoGrip.cpp): the ends about 3 of 4 times, where they fit.
    using EGrip = FSCCargoMove::EGrip;
    const TArray<int32> Order = TierOrder(M, A, N);
    M.TierStep = FMath::Clamp(M.TierStep, 0, Order.Num() - 1);
    M.Tier = Order[M.TierStep];
    M.Grip = EGrip::Over;
    M.EndsPiece = -1;
    M.OverLeg.Reset();
    M.bEndsToSlot = false;
    M.Piece = 0;
    M.bRoomy = M.Tier < 2; // held over on the way (and at the lift when held from its ends later): with the room
    const bool bEndsTier = M.Tier == 0;
    // Each drone's end (a lone drone: the end trailing its route, so it pushes the box ahead of it; it pulls in reverse
    // where the route doubles back), and the geometry every drone's places round the box are drawn from.
    M.EndOf.Reset();
    if (N == 1)
    {
        const FVector Axis = M.FromRotation.RotateVector(A->SizeCm().X >= A->SizeCm().Y ? FVector::ForwardVector : FVector::RightVector);
        M.EndOf.Add(FVector::DotProduct(Axis, Slot.GetLocation() - A->GetActorLocation()) >= 0 ? 0 : 1);
    }
    else if (N == 2) M.EndOf = {0, 1};
    M.Places = EndGrip(A, Box->Occupancy.Mass, Drones, FMath::Clamp(N, 1, 2));
    M.Ends = bEndsTier ? M.Places : FSCCargoEndGrip();
    M.Spots.Reset();
    M.SpotFlights.Reset();
    M.Picks.Reset();
    if (M.Ends.bValid)
    {
        // Held from its ends the beams have less to spare for the route (their pulls out, their forces near their
        // lines): the route is flown no harder than both allow (as RouteAccel: the minimum kept, over root two).
        const double Ends = FMath::Max(M.Ends.MaxAccelMS2 - Def.MinLoadAccelMS2, FMath::Min<double>(M.Ends.MaxAccelMS2, Def.MinLoadAccelMS2)) / UE_DOUBLE_SQRT_2;
        M.LoadAccel = FMath::Min(M.LoadAccel, Ends * 100.0);
        M.LoadJerk = Def.MaxJerkMS3 * 100.0 * (M.LoadAccel / 100.0) / FMath::Max<double>(Def.MaxAccelMS2, 1e-3);
    }
    // The load's route at the beam's hang; under low ceilings and through doors the drones reel the box in to the beam's
    // minimum distance and plan again.
    const double BaseLift = Lift;
    auto PlanLoad = [&](FString& OutWhy) {
        M.RouteEnd = ESCCargoRouteEnd::None;
        M.Grip = EGrip::Over; // lifted (and turned) held over
        if (bEndsTier && !M.Ends.bValid) { OutWhy = TEXT("it cannot be held from its ends: ") + M.Ends.Why; return false; }
        const FSCCargoRouteQuery Load = LoadQuery(M, A);
        // The free route begins and ends where the whole hanging load (box, beam, drones above it) is clear: drawn along the
        // grid normal past the box's own lift until it is (the drawn stretch swept with the box itself).
        // Loads in the air are left out: a box stays on its grid while one is near its way up (the right of way at lift-off).
        const FSCCargoRouteQuery Fit = LoadQuery(M, A, true);
        auto ClearAlong = [&](const FTransform& Pose, const FVector& N, double From, double& Out) {
            FCollisionQueryParams Params(SCENE_QUERY_STAT(SCCargoClear), true, A);
            for (const TWeakObjectPtr<ASCCargoDrone>& D : State->Drones) Params.AddIgnoredActor(D.Get());
            for (const auto& Pair : State->Boxes)
                if (SCCargoIsFlying(Pair.Value.State)) Params.AddIgnoredActor(Pair.Value.Actor.Get());
            FCollisionObjectQueryParams Types;
            for (const ECollisionChannel C : {ECC_WorldStatic, ECC_WorldDynamic, ECC_PhysicsBody, ECC_Vehicle, ECC_Destructible}) Types.AddObjectTypesToQuery(C);
            for (double D = From; D <= From + 800; D += 50)
            {
                const FVector P = Pose.GetLocation() + N * D;
                if (Fit.Blocked(P)) continue;
                const FVector Start = Pose.GetLocation() + N * 8;
                const TSharedRef<FSCCargoHullRule> Hulls = HullRule(Params, Size / 2 - FVector(6), Pose.GetRotation());
                if (GetWorld()->SweepTestByObjectType(Start, P, Pose.GetRotation(), Types, FCollisionShape::MakeBox(Size / 2 - FVector(6)), Hulls->Along(Start, P))) return false;
                Out = D;
                return true;
            }
            return false;
        };
        double LiftAt = BaseLift;
        if (!ClearAlong(A->GetActorTransform(), M.FromNormal, LiftAt, LiftAt)) { OutWhy = FString::Printf(TEXT("no clear space along the normal of %s"), *FromLabel); return false; }
        const FVector Lifted = A->GetActorLocation() + M.FromNormal * LiftAt;
        M.FromRotation = bLoose ? FQuat(FRotator(0, M.PickRotation.Rotator().Yaw, 0)) : M.PickRotation; // a loose box flies level
        M.SecondPath.Reset();
        // What was planned, once per finished search of the load's route: from where, to where, how it hangs.
        auto ReportLoad = [&](bool bFound, const FVector& GoalAt, const TCHAR* Turn, const FVector& TurnAt) {
            UE_LOG(LogSCCargo, Display,
                   TEXT("CARGO_LOAD job=%d move=%d box=%d found=%d end=%s lifted=%s lift_m=%.2f goal=%s turn=%s turn_at=%s pick_yaw=%.0f flight_yaw=%.0f hang_m=%.2f drones=%d "
                        "spread_m=%.2f size_m=%s margin_cm=%.0f tier=%d grip=%s"),
                   Job.Id, M.MoveId, M.Plan.BoxId, bFound, FSCCargoRoute::EndName(M.RouteEnd), *Lifted.ToCompactString(), LiftAt / 100, *GoalAt.ToCompactString(), Turn,
                   *TurnAt.ToCompactString(), M.PickRotation.Rotator().Yaw, M.FromRotation.Rotator().Yaw, M.HangCm / 100, N,
                   M.Offsets.Num() > 1 ? M.Offsets.Last().Size() / 100 : 0.0, *(Size / 100).ToCompactString(), DroneClearanceCm / 2 + SwayEnvelopeCm(), M.Tier,
                   bEndsTier ? (M.bEndsToSlot ? TEXT("ends_to_slot") : TEXT("ends_then_over")) : TEXT("over"));
        };
        // Held from its ends (tier 1): the way onto the slot arriving at Arrive. Over the slot held over when it can be
        // swapped back there first (SwapOutPoint: OverLeg), else held from its ends down onto it (a level slot). OutTarget:
        // where the route held from its ends ends. Leaves M.Grip at the ends.
        auto EndsFinishNow = [&](const FQuat& Arrive, FVector& OutTarget, FString& OutEndsWhy) {
            M.Grip = EGrip::Over;
            M.OverLeg.Reset();
            M.bEndsToSlot = false;
            FString ApproachWhy;
            FVector OverGoal, OverApproach, T;
            FString Note = TEXT("swap_point none: no room over the slot held over: ");
            const bool bOver = SlotApproach(M, A, Slot, M.ToNormal, Arrive, Def.NormalClearanceM * 50.0, OverGoal, OverApproach, &ApproachWhy);
            if (!bOver) Note += ApproachWhy;
            const bool bSwap = bOver && SwapOutPoint(M, A, To, OverGoal, Arrive, T, &Note);
            if (Note != M.GripNote)
            {
                M.GripNote = Note; // said once (planning goes over this every tick while a search runs)
                UE_LOG(LogSCCargo, Display, TEXT("CARGO_GRIP job=%d move=%d box=%d %s"), Job.Id, M.MoveId, M.Plan.BoxId, *Note);
            }
            if (bSwap)
            {
                M.Goal = OverGoal;
                M.Approach = OverApproach;
                if (!T.Equals(OverGoal, 1.0)) M.OverLeg = {T, OverGoal};
                OutTarget = T;
                M.Grip = EGrip::Ends;
                return true;
            }
            M.Grip = EGrip::Ends;
            if (FVector::DotProduct(M.ToNormal, FVector::UpVector) < 0.999 || Slot.GetRotation().GetUpVector().Z < 0.999)
            {
                OutEndsWhy = TEXT("no room to swap back over the box, and the slot is not level (held from its ends a box goes down level only)");
                return false;
            }
            if (!SlotApproach(M, A, Slot, M.ToNormal, Arrive, Def.NormalClearanceM * 50.0, M.Goal, M.Approach, &ApproachWhy))
            {
                OutEndsWhy = TEXT("no room over the slot held over or from its ends: ") + ApproachWhy;
                return false;
            }
            M.bEndsToSlot = true;
            OutTarget = M.Goal;
            return true;
        };
        // Once per situation (FSCCargoMove::EndsFinishes).
        auto EndsFinish = [&](const FQuat& Arrive, FVector& OutTarget, FString& OutEndsWhy) {
            RefreshMove(M);
            const FString Key = FString::Printf(TEXT("%s@%u/%d"), *Arrive.ToString(), M.WorldStamp, int32(M.bRefine));
            if (const FSCCargoMove::FEndsFinish* Known = M.EndsFinishes.Find(Key))
            {
                M.Grip = EGrip(Known->Grip);
                M.bEndsToSlot = Known->bEndsToSlot;
                M.OverLeg = Known->OverLeg;
                if (Known->bOK)
                {
                    M.Goal = Known->Goal;
                    M.Approach = Known->Approach;
                    OutTarget = Known->Target;
                }
                OutEndsWhy = Known->Why;
                return Known->bOK;
            }
            FSCCargoMove::FEndsFinish Out;
            Out.bOK = EndsFinishNow(Arrive, Out.Target, Out.Why);
            Out.Grip = uint8(M.Grip);
            Out.bEndsToSlot = M.bEndsToSlot;
            Out.OverLeg = M.OverLeg;
            Out.Goal = M.Goal;
            Out.Approach = M.Approach;
            if (Out.bOK) M.EndsFinishes.Add(Key, Out); // a miss may clear (kept space): looked for again
            OutTarget = Out.Target;
            OutEndsWhy = Out.Why;
            return Out.bOK;
        };
        // Into a ship's hold from outside it, a long box goes in lengthwise (the hold's entrance passes it only so). It turns
        // onto the hold's lattice where it has room, in free air (swept): by where it was lifted when it can (then it flies
        // the whole way as it will go in, through whatever leads there: a ramp, a channel between tail booms), else
        // outside the hold's entrance (below). Held from its ends, the drones swap to its ends after that turn.
        const bool bIntoHold = !bHold && To->bAccessRule && To->Kind == ESCCargoGridKind::Vehicle && HoldAt(Lifted) != To;
        if (FVector TurnAt; bIntoHold && EarlyTurnPoint(M, A, From, Lifted, Slot.GetRotation(), TurnAt))
        {
            const FQuat InHold = Slot.GetRotation();
            M.ToRotation = InHold;
            FString ApproachWhy;
            FVector Target;
            bool bReady = false;
            if (bEndsTier)
            {
                bReady = SwapClear(M, A, TurnAt, InHold, &ApproachWhy) && EndsFinish(InHold, Target, ApproachWhy);
                if (const FString Note = FString::Printf(TEXT("turn_by_lift=%s not held from its ends there: %s"), *TurnAt.ToCompactString(), *ApproachWhy); !bReady && Note != M.GripNote)
                {
                    M.GripNote = Note; // said once (planning goes over this every tick while a search runs)
                    UE_LOG(LogSCCargo, Display, TEXT("CARGO_GRIP job=%d move=%d box=%d %s"), Job.Id, M.MoveId, M.Plan.BoxId, *Note);
                }
            }
            else
            {
                bReady = SlotApproach(M, A, Slot, M.ToNormal, InHold, Def.NormalClearanceM * 50.0, M.Goal, M.Approach, &ApproachWhy);
                Target = M.Goal;
            }
            if (bReady)
            {
                const int32 Reported = State->SearchesReported;
                const bool bIn = PlanRoute(Job, M, TEXT("load_turned"), TurnAt, Target, LoadQuery(M, A, false, &InHold, bEndsTier ? 1 : 0), M.SecondPath,
                                              FString::Printf(TEXT("load_turned:%.0f"), M.HangCm), true);
                if (State->SearchesReported != Reported) ReportLoad(bIn, Target, TEXT("by_lift"), TurnAt);
                if (bIn)
                {
                    M.SecondRotation = InHold;
                    M.TransitPath = TurnAt.Equals(Lifted, 1.0) ? TArray<FVector>{Lifted} : FSCCargoRoute::Spline({Lifted, TurnAt}, 50);
                    M.EndsPiece = bEndsTier ? 1 : -1;
                    return true;
                }
                if (M.bRoutePending) return false; // searching: PrepareMove is called again next tick
                M.SecondPath.Reset();
            }
            M.Grip = EGrip::Over;
            M.OverLeg.Reset();
            M.bEndsToSlot = false;
        }
        // Out of a room (the lift room, a hold) lengthwise through its entrance: a long box lying across the way out turns
        // a quarter at the top of its lift, in free air (swept), before it flies.
        if (const int32 Axis = From ? EntranceAxis(From) : 0; Axis > 0 && HoldAt(Lifted) == From)
        {
            const FVector Way = From->LatticeToWorld().GetUnitAxis(Axis == 1 ? EAxis::X : EAxis::Y).GetSafeNormal2D();
            const FVector Long = M.PickRotation.RotateVector(Size.X >= Size.Y ? FVector::ForwardVector : FVector::RightVector).GetSafeNormal2D();
            const FQuat Quarter = FQuat(FVector::UpVector, UE_HALF_PI) * M.PickRotation;
            if (FMath::Abs(FVector::DotProduct(Way, Long)) < 0.7 && TurnClear(M, A, Lifted, M.PickRotation, Lifted, Quarter)) M.FromRotation = Quarter;
        }
        // Else the turn outside the hold: to it at the pickup rotation, turned there in free air, in lengthwise. Turn points:
        // out of each lattice side the load leaves lengthwise in a straight sweep, from just outside outwards, the first
        // where it fits as it arrives and turns free. One the load cannot reach from outside (the search from it ends
        // enclosed: a closed end of the hold, a channel between tail booms) is passed over (Move.EntranceOut counts them).
        FVector Entrance = FVector::ZeroVector;
        int32 EntranceIndex = 0;
        if (bIntoHold)
        {
            // In through the entrance as it goes in (held from its ends: swapped to them after the turn there).
            const FQuat InHold = Slot.GetRotation();
            // Once per situation (planning goes over this every tick while a search runs; FSCCargoMove::Entrances).
            RefreshMove(M);
            const FString Key = FString::Printf(TEXT("%u/%d/%d/%d/%s"), M.WorldStamp, M.EntranceOut, M.Tier, int32(M.bRefine), *M.FromRotation.ToString());
            bool bRoom = false, bSide = false;
            if (const FSCCargoMove::FEntrance* Known = M.Entrances.Find(Key))
            {
                bRoom = Known->bRoom;
                bSide = Known->bSide;
                Entrance = Known->At;
                EntranceIndex = Known->Index;
                OutWhy = Known->Why;
            }
            else
            {
                const FSCCargoRouteQuery Aligned = LoadQuery(M, A, false, &InHold, bEndsTier ? 1 : 0), Arriving = LoadQuery(M, A, false, nullptr, 0);
                const double Z = Slot.GetLocation().Z + Size.Z / 2 + Def.NormalClearanceM * 50.0;
                const int32 MaxSteps = FMath::FloorToInt32(MaxEntranceOutCm / EntranceStepCm);
                for (int32 Side = 0; Side < 4 && !bRoom; ++Side)
                {
                    FVector First, Out;
                    if (!HoldExit(To, Aligned, Z, Side, First, Out)) continue;
                    bSide = true;
                    for (int32 Step = 0; Step <= MaxSteps && !bRoom; ++Step)
                    {
                        if (EntranceIndex++ < M.EntranceOut) continue;
                        Entrance = First + Out * EntranceStepCm * Step;
                        bRoom = !Arriving.Blocked(Entrance) && TurnClear(M, A, Entrance, M.FromRotation, Entrance, InHold, &OutWhy) && Aligned.Free(Entrance, First) &&
                                (!bEndsTier || SwapClear(M, A, Entrance, InHold, &OutWhy));
                    }
                }
                --EntranceIndex; // the one that fits
                if (bRoom) M.Entrances.Add(Key, {bRoom, bSide, Entrance, EntranceIndex, OutWhy}); // a miss may clear (kept space): looked for again
            }
            if (!bRoom)
            {
                OutWhy = !bSide               ? FString::Printf(TEXT("no way into %s lengthwise"), *To->GridLabel.ToString())
                         : M.EntranceOut > 0 ? FString::Printf(TEXT("no point outside %s where the box can turn in is reachable"), *To->GridLabel.ToString())
                                             : FString::Printf(TEXT("no room to turn the box at the entrance of %s: %s"), *To->GridLabel.ToString(), *OutWhy);
                return false;
            }
        }
        FVector EndsTarget = FVector::ZeroVector; // held from its ends: where that route ends (a swap back over, or the goal)
        if (bHold)
        {
            // A holding point beside the lift corridor, at the lifted height: stepping out along the grid's axes by the box's
            // size, nearest first, the first place the hanging load fits that it can reach in a straight sweep.
            const FTransform Lattice = From->LatticeToWorld();
            const double Reach = FMath::Max(Size.X, Size.Y) + 100;
            // The way up of the box beneath (its bounds swept along the normal over the lift and the hang) stays free.
            FBox Corridor(ForceInit);
            if (const FSCCargoBox* Target = State->Boxes.Find(M.Plan.ForBoxId); Target && Target->Actor.IsValid())
            {
                const FBox B = Target->Actor->GetComponentsBoundingBox(true);
                Corridor = B + B.ShiftBy(M.FromNormal * (B.GetSize().Size() + LiftAt + M.HangCm + DroneHeightCm()));
            }
            bool bFound = false;
            for (int32 K = 1; K <= 6 && !bFound; ++K)
                for (const FVector& Axis : {Lattice.GetUnitAxis(EAxis::X), -Lattice.GetUnitAxis(EAxis::X), Lattice.GetUnitAxis(EAxis::Y), -Lattice.GetUnitAxis(EAxis::Y)})
                {
                    const FVector C = Lifted + FVector(Axis.X, Axis.Y, 0).GetSafeNormal() * Reach * K;
                    const FBox Held = FBox::BuildAABB(C, FVector(FMath::Max3(Size.X, Size.Y, Size.Z) / 2 + DroneClearanceCm));
                    if ((Corridor.IsValid && Corridor.Intersect(Held)) || Load.Blocked(C) || !Load.Free(Lifted, C)) continue;
                    M.HoldPoint = C;
                    bFound = true;
                    break;
                }
            if (!bFound) { OutWhy = FString::Printf(TEXT("no free holding point beside %s"), *From->GridLabel.ToString()); return false; }
            M.Goal = M.Approach = M.HoldPoint;
            M.ToRotation = M.FromRotation;
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_HOLD job=%d move=%d box=%d point=%s"), Job.Id, M.MoveId, M.Plan.BoxId, *M.HoldPoint.ToCompactString());
        }
        else if (bEndsTier)
        {
            // Held from its ends: into a hold from its entrance (swapped there), else from the top of its lift (swapped
            // there, after any turn out of its room); then the way onto the slot.
            M.ToRotation = Slot.GetRotation();
            if (!bIntoHold && !SwapClear(M, A, Lifted, M.FromRotation, &OutWhy))
            {
                OutWhy = TEXT("no room to swap to the box's ends at the top of its lift: ") + OutWhy;
                return false;
            }
            FString EndsWhy;
            if (!EndsFinish(bIntoHold ? Slot.GetRotation() : M.FromRotation, EndsTarget, EndsWhy))
            {
                OutWhy = FString::Printf(TEXT("held from its ends onto the slot on %s: %s"), *To->GridLabel.ToString(), *EndsWhy);
                return false;
            }
        }
        else
        {
            // The hover point over the slot: the load arrives at its pickup rotation, turns onto the lattice there (swept)
            // and goes down along the normal at the slot's rotation (swept).
            M.ToRotation = Slot.GetRotation();
            FString ApproachWhy;
            // From just off the slot outwards: the first hover point the arriving load fits (a low hold has no room for more).
            if (!SlotApproach(M, A, Slot, M.ToNormal, bIntoHold ? Slot.GetRotation() : M.FromRotation, Def.NormalClearanceM * 50.0, M.Goal, M.Approach, &ApproachWhy))
            {
                OutWhy = FString::Printf(TEXT("no room over the slot on %s: %s"), *To->GridLabel.ToString(), *ApproachWhy);
                return false;
            }
        }
        // The first piece held from its ends unless it goes into a hold (then the second, from its entrance).
        const int32 FirstGrip = bEndsTier && !bIntoHold ? 1 : 0;
        FSCCargoRouteQuery Cached = FirstGrip == 0 && M.FromRotation.Equals(M.PickRotation) ? Load : LoadQuery(M, A, false, nullptr, FirstGrip); // at the flight rotation
        const FVector Arrival = bEndsTier ? EndsTarget : M.Goal;
        if (bIntoHold)
        {
            // In: from the entrance to the hover point, at the hold's rotation.
            const FQuat InHold = Slot.GetRotation();
            FSCCargoRouteQuery In = LoadQuery(M, A, false, &InHold, bEndsTier ? 1 : 0);
            if (!PlanRoute(Job, M, TEXT("load_in"), Entrance, Arrival, In, M.SecondPath, FString::Printf(TEXT("load_in:%.0f"), M.HangCm), true))
            {
                if (M.bRoutePending) return false;
                OutWhy = FString::Printf(TEXT("no free route into %s from its entrance"), *To->GridLabel.ToString());
                return false;
            }
            M.SecondRotation = InHold;
        }
        const int32 Reported = State->SearchesReported;
        const bool bRoute = PlanRoute(Job, M, TEXT("load"), Lifted, bIntoHold ? Entrance : Arrival, Cached, M.TransitPath, FString::Printf(TEXT("load:%.0f"), M.HangCm), true);
        if (State->SearchesReported != Reported)
            ReportLoad(bRoute, bIntoHold ? Entrance : Arrival, bIntoHold ? TEXT("at_entrance") : M.FromRotation.Equals(M.PickRotation) ? TEXT("at_slot") : TEXT("quarter_at_lift"),
                    bIntoHold ? Entrance : Arrival);
        if (bRoute && bEndsTier) M.EndsPiece = bIntoHold ? 1 : 0;
        if (!bRoute && !M.bRoutePending && bIntoHold && M.RouteEnd == ESCCargoRouteEnd::GoalEnclosed)
        {
            // The turn point cannot be reached from outside: the next one further out, planned from the next tick on.
            M.EntranceOut = EntranceIndex + 1;
            M.bRoutePending = true;
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_ENTRANCE job=%d move=%d box=%d grid=%d enclosed_at=%s next=%d"), Job.Id, M.MoveId, M.Plan.BoxId, To->GridId,
                   *Entrance.ToCompactString(), M.EntranceOut);
            return false;
        }
        if (!bRoute)
        {
            if (M.bRoutePending) return false; // searching: PrepareMove is called again next tick
            OutWhy = FString::Printf(TEXT("no free route for the %d SCU box from %s to %s (%s)"), Box->Occupancy.StandardCargoUnits, *FromLabel,
                                  bHold ? TEXT("a holding point") : *To->GridLabel.ToString(), *SCCargoRouteEndText(M.RouteEnd));
            return false;
        }
        return true;
    };
    // Each way of holding it (M.Tier) on the full-cell lattice first; held from its ends or over as tight as it fits, when
    // no way is found, once more with half steps where a full step is blocked (bRefine: a passage narrower than a cell;
    // the costly search, so the last resort). The roomy way over goes straight on to the tight one.
    const bool bPlanned = PlanLoad(Why);
    M.Grip = EGrip::Over; // the move starts held over (lifted so)
    if (!bPlanned)
    {
        if (M.bRoutePending) return false;
        const bool bNoWay = M.RouteEnd == ESCCargoRouteEnd::StartEnclosed || M.RouteEnd == ESCCargoRouteEnd::GoalEnclosed || M.RouteEnd == ESCCargoRouteEnd::Budget ||
                            M.RouteEnd == ESCCargoRouteEnd::Smoothing;
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_GRIP_TIER job=%d move=%d box=%d tier=%d refine=%d failed=\"%s\""), Job.Id, M.MoveId, M.Plan.BoxId, M.Tier, M.bRefine, *Why);
        const TArray<int32> Tried = TierOrder(M, A, N);
        if (M.Tier != 1 && bNoWay && !M.bRefine) // the roomy way over goes straight on to the next
        {
            M.bRefine = true;
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_PATH job=%d move=%d box=%d refine half_steps=1"), Job.Id, M.MoveId, M.Plan.BoxId);
        }
        else if (M.TierStep + 1 < Tried.Num())
        {
            ++M.TierStep;
            M.bRefine = false;
        }
        else return false;
        M.EntranceOut = 0;
        M.bRoutePending = true; // planned again from the next tick on
        return false;
    }
    M.DistanceCm = 0;
    for (int32 I = 1; I < M.TransitPath.Num(); ++I) M.DistanceCm += FVector::Dist(M.TransitPath[I - 1], M.TransitPath[I]);
    for (int32 I = 1; I < M.SecondPath.Num(); ++I) M.DistanceCm += FVector::Dist(M.SecondPath[I - 1], M.SecondPath[I]);
    // Each drone out to its place over the box.
    M.DroneFlights.Reset();
    TArray<FVector> FirstRoute;
    int32 Apart = 0;
    // Where each hovers attached: its emitter the hang over the box's top, over the box as it stands (the formation
    // then holds it there). Kept space for every other drone's route from now on, its teammates' included.
    M.Stations.Reset();
    for (int32 I = 0; I < N; ++I)
        M.Stations.Add(A->GetActorLocation() + M.FromNormal * (Size.Z / 2 + M.HangCm + FVector::Dist(Drones[I]->GetActorLocation(), Drones[I]->BodyEmitter())) + M.PickRotation.RotateVector(M.Offsets[I]));
    for (int32 I = 0; I < N; ++I)
    {
        ASCCargoDrone* D = Drones[I];
        const FVector Over = M.Stations[I];
        TArray<FVector> Path = {D->GetActorLocation()};
        FVector Start = D->GetActorLocation();
        if (D->State == ESCCargoDroneState::Docked)
        {
            const FSCCargoDock* Dock = State->Docks.FindByPredicate([&](const FSCCargoDock& K) { return K.ItemId == D->DockItem; });
            if (Dock && Dock->Actor.IsValid())
            {
                // Out of the bay, then off the grid along its normal: the free route starts clear of the dock's grid.
                Path.Add(Dock->Actor->BayExit(D->Bay));
                Path.Add(Start = Departure(D, Path.Last()));
            }
        }
        TArray<FVector> Route;
        const FVector Ends[] = {Start, Over};
        // Its routing shape; with an end inside that shape's margin (hovering close over a box it set down beside a
        // plate), the bare hull and its gusts: its steps are swept with the hull whatever the route's margin.
        FSCCargoRouteQuery Query = DroneQuery(D, &M, Ends);
        if (Query.Blocked(Start) || Query.Blocked(Over)) Query = DroneQuery(D, &M, Ends, /*bTight=*/true);
        Query.CellCache = M.Cells(Query.Identity);
        // In the air and close to something (the box it has just set down): a short straight step clear of it first.
        if (TArray<FVector> Via; D->State != ESCCargoDroneState::Docked && ClearStart(D, Query, Start, &Via) && !Start.Equals(D->GetActorLocation(), 1.0))
        {
            Path.Append(Via);
            Path.Add(Start);
        }
        // Apart from its teammates: beside the route of the one planned first, the kept distance to its side for each
        // place further along the box (FSCCargoDroneSteer::Beside), swept like any route. Where that is not free (a
        // door they all leave by), its own plain route: the drones then follow each other, each keeping its
        // distance by its own steps.
        bool bApart = false;
        if (const double Keep = FSCCargoDroneSteer::Keep(D->HullRadius(), D->HullRadius()); I > 0 && FirstRoute.Num() >= 2 && FVector::Dist(Start, FirstRoute[0]) < BesideRampKeeps * Keep) // they leave one dock
        {
            TArray<FVector> Side = FSCCargoDroneSteer::Beside(FirstRoute, Start, Over, I * Keep, BesideRampKeeps * Keep);
            bApart = Side.Num() >= 2;
            for (int32 K = 1; bApart && K < Side.Num(); ++K) bApart = Query.Free(Side[K - 1], Side[K]);
            if (bApart) Route = MoveTemp(Side);
        }
        if (!bApart && !PlanRoute(Job, M, TEXT("out"), Start, Over, Query, Route, FString::Printf(TEXT("out:%d"), D->DroneId), true))
        {
            if (M.bRoutePending) return false;
            Why = FString::Printf(TEXT("no free route for drone %d to the box on %s"), D->DroneId, *FromLabel);
            // A drone in the air has no way from where it hovers just now (its surroundings are in motion: a box
            // being set down beside it, a plate): planned again shortly, not a failure of the box.
            if (D->State != ESCCargoDroneState::Docked) M.bKeptHit = true;
            return false;
        }
        if (I == 0) FirstRoute = Route;
        if (I > 0) Apart += bApart;
        Path.Append(Route);
        M.DroneFlights.AddDefaulted_GetRef().Start(Path, Def.MaxSpeedMS * 100.0, Def.MaxAccelMS2 * 100.0, Def.MaxJerkMS3 * 100.0);
    }
    if (N > 1) UE_LOG(LogSCCargo, Display, TEXT("CARGO_APART job=%d move=%d box=%d teammates=%d beside_the_first=%d"), Job.Id, M.MoveId, M.Plan.BoxId, N - 1, Apart);
    if (M.EndsPiece >= 0)
    {
        UE_LOG(LogSCCargo, Display,
               TEXT("CARGO_GRIP job=%d move=%d box=%d ends team=%d end=%d piece=%d to_slot=%d over_leg=%d angle_deg=%.1f beam_cm=%.0f out_cm=%.0f rise_cm=%.0f attach_z_cm=%.0f "
                    "share_kN=%.1f max_accel_m_s2=%.2f route_accel_m_s2=%.2f envelope_cm=%.1f shape_cm=%s..%s"),
               Job.Id, M.MoveId, M.Plan.BoxId, N, M.EndOf.Num() == 1 ? M.EndOf[0] : -1, M.EndsPiece, M.bEndsToSlot, M.OverLeg.Num(), FMath::RadiansToDegrees(M.Ends.AngleRad),
               M.Ends.BeamCm, M.Ends.OutCm, M.Ends.RiseCm, M.Ends.AttachZCm, M.Ends.ShareN / 1000, M.Ends.MaxAccelMS2, M.LoadAccel / 100, LoadEnvelopeCm(M),
               *M.Ends.Shape().Min.ToCompactString(), *M.Ends.Shape().Max.ToCompactString());
    }
    else
    {
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_GRIP job=%d move=%d box=%d over team=%d tier=%d roomy=%d places=%d envelope_cm=%.1f%s%s"), Job.Id, M.MoveId, M.Plan.BoxId, N, M.Tier,
               M.bRoomy, M.Places.bValid, LoadEnvelopeCm(M), M.Places.bValid ? TEXT("") : TEXT(" why="), M.Places.bValid ? TEXT("") : *M.Places.Why);
    }
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_MOVE job=%d move=%d box=%d team=%d load_kg=%.0f loaded_accel_m_s2=%.2f empty_accel_m_s2=%.2f lift_m=%.2f hang_m=%.2f"),
           Job.Id, M.MoveId, M.Plan.BoxId, N, Box->Occupancy.Mass, LoadAccel, Def.MaxAccelMS2, Lift / 100, M.HangCm / 100);
    return true;
}
