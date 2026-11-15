// USCCargoSubsystem flight planning against the scene's collision: routes searched over frames with their proof
// (CARGO_PATH: found, how the search ended, minimum clearance, sweep hits, its cost per frame; CARGO_PATH_FRONT: what each
// front of a failed search reached), and a move's preparation before anything flies: lift along the source normal, the
// load's route to its hold point or approach (into a hold: to a turn point outside it that the load can reach), the
// drones' routes out to the box. Nothing flies a route that was not found free. Query shapes: SCCargoQueries.cpp.
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
        State->SearchUntil = FPlatformTime::Seconds() + FSCCargoState::SearchBudgetSeconds;
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
    if (!Search->IsDone() && !Search->Step(FMath::Max(State->SearchUntil - FPlatformTime::Seconds(), 0.0)))
    {
        M.bRoutePending = true;
        return false;
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

FVector USCCargoSubsystem::Departure(const ASCCargoDrone* D, const FVector& Exit) const
{
    // Where a drone leaving (or entering) its bay is clear of the grid its dock stands on: along that grid's normal by
    // the drone's routing half-size and its clearance.
    const FSCCargoDock* Dock = State->Docks.FindByPredicate([&](const FSCCargoDock& K) { return K.ItemId == D->DockItem; });
    const USCCargoGridComponent* Grid = Dock ? FindGrid(Dock->GridId) : nullptr;
    const FVector N = Grid ? Grid->LatticeToWorld().GetRotation().GetUpVector() : FVector::UpVector;
    return Exit + N * (D->GetComponentsBoundingBox(false).GetExtent().GetMax() + DroneClearanceCm * 2);
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
    FSCCargoRouteQuery Query = DroneQuery(D);
    Query.CellCache = M.Cells(Query.Identity);
    if (!PlanRoute(Job, M, TEXT("home"), D->GetActorLocation(), Clear, Query, Path, FString::Printf(TEXT("home:%d"), D->DroneId), false))
    {
        if (M.bRoutePending) M.NextRetry = 0; // the search goes on next frame
        return false;
    }
    Path.Add(Exit);
    Path.Add(DockActor->BayTransform(D->Bay).GetLocation());
    Out.Start(Path, Def.MaxSpeedMS * 100.0, Def.MaxAccelMS2 * 100.0, Def.MaxJerkMS3 * 100.0);
    return true;
}

bool USCCargoSubsystem::EnvironmentAccess(USCCargoGridComponent* Grid, bool bOpen)
{
    // FSCEnvironmentAccess opened its way at discovery (and keeps it open on re-pose); a job waits until that is done.
    (void)bOpen;
    return Grid->Access == ESCCargoGridAccess::Open;
}

bool USCCargoSubsystem::PrepareMove(FSCCargoJob& Job, FSCCargoMove& M, FString& Why)
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
    if (!A || !From || !To || Drones.IsEmpty()) { Why = TEXT("the box, its grids or its drones are gone"); return false; }
    const FVector Size = A->SizeCm();
    const int32 N = Drones.Num();
    M.FromNormal = From->LatticeToWorld().GetRotation().GetUpVector();
    M.ToNormal = To->LatticeToWorld().GetRotation().GetUpVector();
    M.FromRotation = M.PickRotation = A->GetActorQuat();
    M.TurnStart = -1;
    // The beam hangs as long as the tractor's full strength allows, shorter under a ceiling at either end.
    double Lift = 0, LiftTo = 0, HangTo = 0;
    LiftPlan(A, A->GetActorTransform(), M.FromNormal, Lift, M.HangCm);
    const FTransform Slot = To->SlotToWorld(SlotOf(M.Plan), Box->SizeM());
    const bool bHold = M.Plan.Kind == ESCCargoMoveKind::Hold;
    if (!bHold && LiftPlan(A, Slot, M.ToNormal, LiftTo, HangTo)) M.HangCm = FMath::Min(M.HangCm, HangTo);
    const double LoadAccel = SCCargoLoadedAccel(N, Def.MaxForceN, Box->Occupancy.Mass, Def.DroneMassKg, Gravity(), Def.MaxAccelMS2);
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
    // The load's route at the beam's hang; under low ceilings and through doors the drones reel the box in to the beam's
    // minimum distance and plan again.
    const double BaseLift = Lift;
    auto PlanLoad = [&](FString& OutWhy) {
        M.RouteEnd = ESCCargoRouteEnd::None;
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
                const TSharedRef<FSCCargoHullRule> Hulls = HullRule(Params, Size / 2 - FVector(6), Pose.GetRotation(), &M);
                if (GetWorld()->SweepTestByObjectType(Start, P, Pose.GetRotation(), Types, FCollisionShape::MakeBox(Size / 2 - FVector(6)), Hulls->Along(Start, P))) return false;
                Out = D;
                return true;
            }
            return false;
        };
        double LiftAt = BaseLift;
        if (!ClearAlong(A->GetActorTransform(), M.FromNormal, LiftAt, LiftAt)) { OutWhy = FString::Printf(TEXT("no clear space along the normal of %s"), *From->GridLabel.ToString()); return false; }
        const FVector Lifted = A->GetActorLocation() + M.FromNormal * LiftAt;
        M.FromRotation = M.PickRotation;
        M.SecondPath.Reset();
        // What was planned, once per finished search of the load's route: from where, to where, how it hangs.
        auto ReportLoad = [&](bool bFound, const FVector& GoalAt, const TCHAR* Turn, const FVector& TurnAt) {
            UE_LOG(LogSCCargo, Display,
                   TEXT("CARGO_LOAD job=%d move=%d box=%d found=%d end=%s lifted=%s lift_m=%.2f goal=%s turn=%s turn_at=%s pick_yaw=%.0f flight_yaw=%.0f hang_m=%.2f drones=%d "
                        "spread_m=%.2f size_m=%s margin_cm=%.0f"),
                   Job.Id, M.MoveId, M.Plan.BoxId, bFound, FSCCargoRoute::EndName(M.RouteEnd), *Lifted.ToCompactString(), LiftAt / 100, *GoalAt.ToCompactString(), Turn,
                   *TurnAt.ToCompactString(), M.PickRotation.Rotator().Yaw, M.FromRotation.Rotator().Yaw, M.HangCm / 100, N,
                   M.Offsets.Num() > 1 ? M.Offsets.Last().Size() / 100 : 0.0, *(Size / 100).ToCompactString(), DroneClearanceCm / 2 + SwayEnvelopeCm());
        };
        // Into a ship's hold from outside it, a long box goes in lengthwise (the hold's entrance passes it only so). It turns
        // onto the hold's lattice where it has room, in free air (swept): by where it was lifted when it can (then it flies
        // the whole way as it will go in, through whatever leads there: a ramp, a channel between tail booms), else
        // outside the hold's entrance (below).
        const bool bIntoHold = !bHold && To->bAccessRule && To->Kind == ESCCargoGridKind::Vehicle && HoldAt(Lifted) != To;
        if (FVector TurnAt; bIntoHold && EarlyTurnPoint(M, A, From, Lifted, Slot.GetRotation(), TurnAt))
        {
            const FQuat InHold = Slot.GetRotation();
            M.ToRotation = InHold;
            FString ApproachWhy;
            if (SlotApproach(M, A, Slot, M.ToNormal, InHold, Def.NormalClearanceM * 50.0, M.Goal, M.Approach, &ApproachWhy))
            {
                const int32 Reported = State->SearchesReported;
                const bool bIn = PlanRoute(Job, M, TEXT("load_turned"), TurnAt, M.Goal, LoadQuery(M, A, false, &InHold), M.SecondPath,
                                              FString::Printf(TEXT("load_turned:%.0f"), M.HangCm), true);
                if (State->SearchesReported != Reported) ReportLoad(bIn, M.Goal, TEXT("by_lift"), TurnAt);
                if (bIn)
                {
                    M.SecondRotation = InHold;
                    M.TransitPath = TurnAt.Equals(Lifted, 1.0) ? TArray<FVector>{Lifted} : FSCCargoRoute::Spline({Lifted, TurnAt}, 50);
                    return true;
                }
                if (M.bRoutePending) return false; // searching: PrepareMove is called again next tick
                M.SecondPath.Reset();
            }
        }
        // Out of a room (the lift room, a hold) lengthwise through its entrance: a long box lying across the way out turns
        // a quarter at the top of its lift, in free air (swept), before it flies.
        if (const int32 Axis = EntranceAxis(From); Axis > 0 && HoldAt(Lifted) == From)
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
            const FQuat InHold = Slot.GetRotation();
            const FSCCargoRouteQuery Aligned = LoadQuery(M, A, false, &InHold), Arriving = LoadQuery(M, A);
            const double Z = Slot.GetLocation().Z + Size.Z / 2 + Def.NormalClearanceM * 50.0;
            const int32 MaxSteps = FMath::FloorToInt32(MaxEntranceOutCm / EntranceStepCm);
            bool bRoom = false, bSide = false;
            for (int32 Side = 0; Side < 4 && !bRoom; ++Side)
            {
                FVector First, Out;
                if (!HoldExit(To, Aligned, Z, Side, First, Out)) continue;
                bSide = true;
                for (int32 Step = 0; Step <= MaxSteps && !bRoom; ++Step)
                {
                    if (EntranceIndex++ < M.EntranceOut) continue;
                    Entrance = First + Out * EntranceStepCm * Step;
                    bRoom = !Arriving.Blocked(Entrance) && TurnClear(M, A, Entrance, M.FromRotation, Entrance, InHold, &OutWhy) && Aligned.Free(Entrance, First);
                }
            }
            --EntranceIndex; // the one that fits
            if (!bRoom)
            {
                OutWhy = !bSide               ? FString::Printf(TEXT("no way into %s lengthwise"), *To->GridLabel.ToString())
                         : M.EntranceOut > 0 ? FString::Printf(TEXT("no point outside %s where the box can turn in is reachable"), *To->GridLabel.ToString())
                                             : FString::Printf(TEXT("no room to turn the box at the entrance of %s: %s"), *To->GridLabel.ToString(), *OutWhy);
                return false;
            }
        }
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
                Corridor = B + B.ShiftBy(M.FromNormal * (B.GetSize().Size() + LiftAt + M.HangCm + DroneHeightCm));
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
        FSCCargoRouteQuery Cached = M.FromRotation.Equals(M.PickRotation) ? Load : LoadQuery(M, A); // at the flight rotation
        if (bIntoHold)
        {
            // In: from the entrance to the hover point, at the hold's rotation.
            const FQuat InHold = Slot.GetRotation();
            FSCCargoRouteQuery In = LoadQuery(M, A, false, &InHold);
            if (!PlanRoute(Job, M, TEXT("load_in"), Entrance, M.Goal, In, M.SecondPath, FString::Printf(TEXT("load_in:%.0f"), M.HangCm), true))
            {
                if (M.bRoutePending) return false;
                OutWhy = FString::Printf(TEXT("no free route into %s from its entrance"), *To->GridLabel.ToString());
                return false;
            }
            M.SecondRotation = InHold;
        }
        const int32 Reported = State->SearchesReported;
        const bool bRoute = PlanRoute(Job, M, TEXT("load"), Lifted, bIntoHold ? Entrance : M.Goal, Cached, M.TransitPath, FString::Printf(TEXT("load:%.0f"), M.HangCm), true);
        if (State->SearchesReported != Reported)
            ReportLoad(bRoute, bIntoHold ? Entrance : M.Goal, bIntoHold ? TEXT("at_entrance") : M.FromRotation.Equals(M.PickRotation) ? TEXT("at_slot") : TEXT("quarter_at_lift"),
                    bIntoHold ? Entrance : M.Goal);
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
            OutWhy = FString::Printf(TEXT("no free route for the %d SCU box from %s to %s (%s)"), Box->Occupancy.StandardCargoUnits, *From->GridLabel.ToString(),
                                  bHold ? TEXT("a holding point") : *To->GridLabel.ToString(), *SCCargoRouteEndText(M.RouteEnd));
            return false;
        }
        return true;
    };
    // The beam at its full hang first, then reeled in (bReeledIn: kept for the ticks a search takes); when no way is found
    // on the full-cell lattice either way, once more with half steps where a full step is blocked (bRefine: a passage
    // narrower than a cell; the costly search, so the last resort).
    const double MinHang = Def.MinDistanceM * 100.0;
    if (M.bReeledIn) M.HangCm = FMath::Min(M.HangCm, MinHang);
    bool bPlanned = PlanLoad(Why);
    if (!bPlanned && !M.bRoutePending && M.HangCm > MinHang + 1)
    {
        M.HangCm = MinHang;
        M.bReeledIn = true;
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_PATH job=%d move=%d box=%d reel_in hang_m=%.2f"), Job.Id, M.MoveId, M.Plan.BoxId, M.HangCm / 100);
        bPlanned = PlanLoad(Why);
    }
    if (!bPlanned)
    {
        if (M.bRoutePending) return false;
        const bool bNoWay = M.RouteEnd == ESCCargoRouteEnd::StartEnclosed || M.RouteEnd == ESCCargoRouteEnd::GoalEnclosed || M.RouteEnd == ESCCargoRouteEnd::Budget ||
                            M.RouteEnd == ESCCargoRouteEnd::Smoothing;
        if (!bNoWay || M.bRefine) return false;
        M.bRefine = true;
        M.EntranceOut = 0;
        M.bRoutePending = true; // planned again from the next tick on
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_PATH job=%d move=%d box=%d refine half_steps=1"), Job.Id, M.MoveId, M.Plan.BoxId);
        return false;
    }
    M.DistanceCm = 0;
    for (int32 I = 1; I < M.TransitPath.Num(); ++I) M.DistanceCm += FVector::Dist(M.TransitPath[I - 1], M.TransitPath[I]);
    for (int32 I = 1; I < M.SecondPath.Num(); ++I) M.DistanceCm += FVector::Dist(M.SecondPath[I - 1], M.SecondPath[I]);
    // Each drone out to its place over the box.
    M.DroneFlights.Reset();
    for (int32 I = 0; I < N; ++I)
    {
        ASCCargoDrone* D = Drones[I];
        const FVector Over = A->GetActorLocation() + M.FromNormal * (Size.Z / 2 + M.HangCm) + M.FromRotation.RotateVector(M.Offsets[I]);
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
        FSCCargoRouteQuery Query = DroneQuery(D);
        Query.CellCache = M.Cells(Query.Identity);
        if (!PlanRoute(Job, M, TEXT("out"), Start, Over, Query, Route, FString::Printf(TEXT("out:%d"), D->DroneId), true))
        {
            if (M.bRoutePending) return false;
            Why = FString::Printf(TEXT("no free route for drone %d to the box on %s"), D->DroneId, *From->GridLabel.ToString());
            return false;
        }
        Path.Append(Route);
        M.DroneFlights.AddDefaulted_GetRef().Start(Path, Def.MaxSpeedMS * 100.0, Def.MaxAccelMS2 * 100.0, Def.MaxJerkMS3 * 100.0);
    }
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_MOVE job=%d move=%d box=%d team=%d load_kg=%.0f loaded_accel_m_s2=%.2f empty_accel_m_s2=%.2f lift_m=%.2f hang_m=%.2f"),
           Job.Id, M.MoveId, M.Plan.BoxId, N, Box->Occupancy.Mass, LoadAccel, Def.MaxAccelMS2, Lift / 100, M.HangCm / 100);
    return true;
}
