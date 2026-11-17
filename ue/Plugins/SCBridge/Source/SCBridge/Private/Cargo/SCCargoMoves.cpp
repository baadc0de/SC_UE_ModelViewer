// USCCargoSubsystem moves in flight: drones fly their own routes out (empty, nimble), demagnetize the box and draw it
// along its grid's normal, carry it hanging from the lead drone's soft beam constraint along a planned route (heavy:
// acceleration from the beam's force and the load's mass), hold it at a holding point when it is a blocker being dug
// out, catch it over the destination, lower it along that grid's normal, magnetize it onto its slot, and fly home.
// Flying on its own a drone faces where it flies; a team faces its load's travel (the route's velocity, given to each
// drone with SetLoadVelocity); the heading itself is the drone's (ASCCargoDrone, FSCCargoHeading).
// Log: CARGO_MOVE (steps), CARGO_REPLAN (a blocked route planned again), CARGO_CARRY (box samples in flight).
#include "Cargo/SCCargoSubsystem.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "SCCargoState.h"
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoDrone.h"
#include "Cargo/SCCargoFlightPath.h"
#include "Cargo/SCCargoGridComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"

namespace
{
using EStep = FSCCargoMove::EStep;
constexpr double MagnetSeconds = 0.6;     // demagnetize / magnetize (presentation)
constexpr double CatchSeconds = 1.2;
constexpr double FlightTurnSeconds = 2.0;  // the turn onto a hold's lattice at its entrance      // the beam brings the swinging box to rest over its slot
constexpr double StepTimeoutSeconds = 180; // a step that takes longer fails the move
constexpr int32 MaxReplans = 10;           // blocked-ahead replans before a move fails
constexpr double SwayFadeCm = 300;         // gusts fade in / out over this much of a flight's ends
constexpr double HoldCheckSeconds = 0.25;  // how often a drone on its way out looks at the loads' routes yet to fly
constexpr double FlightStallSeconds = 3;   // a lone drone not getting on for this long plans its route again
constexpr double SwingWaitSeconds = 3;     // a box that tilts as it is caught waits this long at most for its drones to move out
const double SwingCos = FMath::Cos(FMath::DegreesToRadians(1.0)); // a box's up or a grid's normal further than this from upright: it tilts

const TCHAR* StepName(EStep S)
{
    static const TCHAR* Names[] = {TEXT("Waiting"), TEXT("Outbound"), TEXT("Attaching"), TEXT("Lifting"), TEXT("Transit"), TEXT("Lowering"),
                                   TEXT("Placing"),  TEXT("Returning"), TEXT("Done"),     TEXT("Failed"),  TEXT("Holding")};
    return Names[uint8(S)];
}
FSCCargoPlacement SlotOf(const FSCCargoMovePlan& P) { return SCCargoSlotOf(P); }
} // namespace

void USCCargoSubsystem::TickMove(FSCCargoJob& Job, FSCCargoMove& M, double Now, double Dt)
{
    const FSCCargoDroneDefinition& Def = State->Scenario.Drone;
    FSCCargoBox* Box = State->Boxes.Find(M.Plan.BoxId);
    ASCCargoContainer* A = Box ? Box->Actor.Get() : nullptr;
    USCCargoGridComponent* From = FindGrid(M.Plan.FromGrid);
    USCCargoGridComponent* To = FindGrid(M.Plan.ToGrid);
    TArray<ASCCargoDrone*> Drones;
    for (const int32 Id : M.Drones)
        if (ASCCargoDrone* D = FindDrone(*State, Id)) Drones.Add(D);
    auto Enter = [&](EStep Step) {
        M.Step = Step;
        M.StepStart = Now;
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_MOVE job=%d move=%d box=%d kind=%s step=%s drones=%d t=%.1f"), Job.Id, M.MoveId, M.Plan.BoxId,
               *StaticEnum<ESCCargoMoveKind>()->GetNameStringByValue(int64(M.Plan.Kind)), StepName(Step), M.Drones.Num(), Now - Job.Created);
        ++Revision;
    };
    // A move that cannot go on (WP-K2): a load in flight its team still holds is set down on the nearest free slot it can
    // reach (RetargetLoad: another grid, a new route); one let go of is let down where it is, loose (a beacon takes it once
    // at rest). A box not yet lifted keeps its place (grid or beacon: ReleaseMove).
    auto Fail = [&](const FString& Why, bool bTeamHolds = true) {
        const bool bLifted = Box && A && Box->GridId == INDEX_NONE && Box->BeaconId == INDEX_NONE && Box->State != ESCCargoBoxState::Loose &&
                             Box->State != ESCCargoBoxState::Reserved;
        if (bLifted && bTeamHolds && M.Step == EStep::Transit && M.Plan.Kind != ESCCargoMoveKind::Hold && RetargetLoad(Job, M, Box, A, Why, Now)) return;
        M.Failure = Why;
        for (ASCCargoDrone* D : Drones) D->Drop();
        ReleaseMove(M);
        if (bLifted) LetDown(*Box, M.Plan.FromGrid); // under gravity, where it is
        UE_LOG(LogSCCargo, Warning, TEXT("CARGO_MOVE job=%d move=%d box=%d failed at %s: %s%s"), Job.Id, M.MoveId, M.Plan.BoxId, StepName(M.Step), *Why,
               bLifted ? TEXT(" (let down loose)") : TEXT(""));
        SendHome(M, Now);
    };
    if (M.bInterrupt)
    {
        M.bInterrupt = false;
        Fail(TEXT("interrupted (verification)"));
        return;
    }
    if (M.Step != EStep::Returning && M.Step != EStep::Holding && (!Box || !A || (!From && !M.bLooseSource) || !To))
    {
        Fail(TEXT("the box or one of its grids is gone"));
        return;
    }
    if (M.Step != EStep::Waiting && M.Step != EStep::Returning && M.Step != EStep::Holding && Now - M.StepStart > StepTimeoutSeconds)
    {
        Fail(FString::Printf(TEXT("%s took longer than %.0f s"), StepName(M.Step), StepTimeoutSeconds));
        return;
    }
    // The team as it is: a drone destroyed mid-move leaves the team. With fewer drones than it takes to hold the box
    // (tractor force against its weight) the move fails and the box is let down under gravity; enough drones go on with
    // the loaded acceleration of the smaller team.
    if (Drones.Num() != M.Drones.Num())
    {
        // Each drone's flight and formation slot go with it (the arrays run parallel to M.Drones).
        for (int32 I = M.Drones.Num() - 1; I >= 0; --I)
            if (!FindDrone(*State, M.Drones[I]))
            {
                M.Drones.RemoveAt(I);
                if (M.DroneFlights.IsValidIndex(I)) M.DroneFlights.RemoveAt(I);
                if (M.Offsets.IsValidIndex(I)) M.Offsets.RemoveAt(I);
            }
        UE_LOG(LogSCCargo, Warning, TEXT("CARGO_TEAM job=%d move=%d box=%d lost_drones team=%d"), Job.Id, M.MoveId, M.Plan.BoxId, Drones.Num());
        const bool bLoaded = M.Step != EStep::Waiting && M.Step != EStep::Outbound && M.Step != EStep::Returning;
        if (Drones.IsEmpty() && M.Step != EStep::Returning) { Fail(TEXT("the drone team is gone"), false); return; }
        const int32 ByVolume = Def.MaxVolume > 0 && Box ? FMath::CeilToInt(Box->VolumeSCU() / Def.MaxVolume - 1e-9) : 1;
        if (bLoaded && Box && Drones.Num() < FMath::Max(FSCCargoPlanner::DronesFor(Box->Occupancy.Mass, Gravity(), Def.MaxForceN), ByVolume))
        {
            Fail(FString::Printf(TEXT("%d drone(s) left cannot hold the box"), Drones.Num()), false);
            return;
        }
        if (Box)
        {
            const double LoadAccel = RouteAccel(Drones.Num(), Box->Occupancy.Mass);
            M.LoadAccel = LoadAccel * 100.0;
            M.LoadJerk = Def.MaxJerkMS3 * 100.0 * LoadAccel / FMath::Max<double>(Def.MaxAccelMS2, 1e-3);
        }
    }
    if (Drones.IsEmpty() && M.Step != EStep::Returning && M.Step != EStep::Done && M.Step != EStep::Failed)
    {
        Fail(TEXT("the move has no drones"));
        return;
    }
    M.bTurning = M.bTilting = false; // set by the step that turns the box this tick
    const FVector Up = FVector::UpVector;
    const FVector Size = A ? A->SizeCm() : FVector::ZeroVector;
    const double LoadMass = Box ? Box->Occupancy.Mass : 0;
    // Gusts: full in free flight, faded out towards the precise phases (lift-off, the turn and descent onto a slot,
    // docking); less under load (heavier is steadier), a little more at speed; none in vacuum.
    const bool bAir = InAtmosphere();
    auto Sway = [&](ASCCargoDrone* D, double Fade, double SpeedCmS, double LoadKg) {
        D->SwayAmplitudeCm = Def.SwayAmplitudeCm;
        D->SwayTiltDeg = Def.SwayTiltDeg;
        D->SwayFrequencyHz = Def.SwayFrequencyHz;
        const double Team = FMath::Max(1, Drones.Num()) * FMath::Max<double>(Def.DroneMassKg, 1.0);
        D->SwayGain = bAir ? float(FMath::Clamp(Fade, 0.0, 1.0) / (1.0 + LoadKg / (Team * 10.0)) * (1.0 + 0.05 * SpeedCmS / 100.0)) : 0.f;
    };
    double FormationFade = 0; // set by the step before Formation
    // Where the drones fly around the box centre Q: Along the beam direction N, over the box's top by the hang length.
    // They carry as a team: each faces the load's travel, LoadV (the route's velocity; zero while it is at rest).
    auto Formation = [&](const FVector& Q, const FVector& N, const FQuat& Rotation, const FVector& Accel, bool bBeams, float Width, double Hang = -1,
                         const FVector& LoadV = FVector::ZeroVector) {
        for (int32 I = 0; I < Drones.Num() && I < M.Offsets.Num(); ++I)
        {
            Sway(Drones[I], FormationFade, M.Carry.Speed, LoadMass);
            // The hang is the beam's length: from the box's top to the emitter under the drone.
            const FVector P = Q + N * (Size.Z / 2 + (Hang < 0 ? M.HangCm : Hang) + FVector::Dist(Drones[I]->GetActorLocation(), Drones[I]->Emitter())) + Rotation.RotateVector(M.Offsets[I]);
            Drones[I]->SetLoadVelocity(LoadV);
            Drones[I]->Pose(P, Accel, float(Dt));
            if (bBeams && A) Drones[I]->SetBeam(true, A->GetActorLocation() + A->GetActorQuat().RotateVector(M.Offsets[I] + FVector(0, 0, Size.Z / 2)), Width);
        }
    };
    auto Sample = [&](const FVector& Target) {
        if (!A || Now - M.LastSample < 1.0) return;
        M.LastSample = Now;
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_CARRY job=%d move=%d box=%d step=%s box_at=%s target=%s lag_cm=%.0f speed_m_s=%.2f accel_m_s2=%.2f s=%.0f/%.0f sim=%d"),
               Job.Id, M.MoveId, M.Plan.BoxId, StepName(M.Step), *A->GetActorLocation().ToCompactString(), *Target.ToCompactString(),
               FVector::Dist(A->GetActorLocation(), Target), M.Carry.Speed / 100, M.Carry.Accel / 100, M.Carry.Dist, M.Carry.Length(), A->Body->IsSimulatingPhysics());
    };
    // Right of way, re-slotting and replanning (SCCargoRetarget.cpp), bound to this move.
    auto MustYield = [&](const FVector& PA, const FVector& PB, bool bAnyLoad = false) { return RightOfWayYield(M, Box->Id, A, PA, PB, bAnyLoad); };
    auto InsideWaitingLoad = [&](const FVector& P) { return InsideWaitingLoadAt(M, A, P); };
    auto AheadFree = [&](const FVector& Here, const FVector& Ahead) { return StretchAheadFree(M, A, Here, Ahead); };
    auto ReSlot = [&](const TCHAR* Why) { return ReSlotMove(Job, M, Box, To, Why); };
    auto Replan = [&](const FVector& From, const TCHAR* Why) { return ReplanLoad(Job, M, Box, A, To, From, Why, Now); };
    switch (M.Step)
    {
    case EStep::Waiting:
    {
        // Planned by PrepareMove (routes swept free). A docked drone leaves its bay when its way out is free
        // (Outbound, below); one claimed in the air flies on from where it is.
        for (ASCCargoDrone* D : Drones)
            if (D->State != ESCCargoDroneState::Docked) D->State = ESCCargoDroneState::ToPickup;
        Box->State = ESCCargoBoxState::Reserved;
        M.FlightAt.Reset();
        Enter(EStep::Outbound);
        return;
    }
    case EStep::Outbound:
    case EStep::Returning:
    {
        bool bAll = true;
        // Out to its box, a drone holds short of its station while a load planned before its own move has yet to fly
        // through there (looked at a few times a second: the loads' routes yet to fly are long).
        if (M.Step == EStep::Outbound && Now - M.HoldChecked >= HoldCheckSeconds)
        {
            M.HoldChecked = Now;
            const TArray<double> Before = M.HoldAt;
            M.HoldAt.Reset();
            for (int32 I = 0; I < Drones.Num() && I < M.DroneFlights.Num(); ++I)
            {
                M.HoldAt.Add(HoldShort(M, Drones[I], M.DroneFlights[I]));
                if (const bool bWas = Before.IsValidIndex(I) && Before[I] >= 0; bWas != (M.HoldAt[I] >= 0) || (bWas && FMath::Abs(Before[I] - M.HoldAt[I]) > 10))
                    UE_LOG(LogSCCargo, Display, TEXT("CARGO_HOLD_SHORT job=%d move=%d drone=%d %s at=%s s=%.0f/%.0f flown=%.0f"), Job.Id, M.MoveId, Drones[I]->DroneId,
                           M.HoldAt[I] < 0 ? TEXT("goes on to its station") : TEXT("holds short of its station (a load has yet to fly through it, or a drone is in its place)"),
                           *Drones[I]->GetActorLocation().ToCompactString(), M.HoldAt[I], M.DroneFlights[I].Length(), M.DroneFlights[I].Dist);
            }
        }
        for (int32 I = 0; I < Drones.Num() && I < M.DroneFlights.Num(); ++I)
        {
            ASCCargoDrone* D = Drones[I];
            FSCCargoFlight& F = M.DroneFlights[I];
            if (M.Step == EStep::Returning && D->MoveId != INDEX_NONE)
            {
                // Claimed by another move on the way home: it is that move's once that move flies it. While that move is
                // still planning its routes the drone hovers and stays this flight's: a claim let go before it flies
                // (cancelled, no route, another box) brings it back here instead of leaving it in no move at all.
                const FSCCargoMove* Claim = nullptr;
                for (const FSCCargoJob& O : State->Jobs)
                    if (const FSCCargoMove* C = O.Moves.FindByPredicate([&](const FSCCargoMove& X) { return X.MoveId == D->MoveId; })) { Claim = C; break; }
                if (Claim && Claim->Step == EStep::Waiting) bAll = false;
                continue;
            }
            if (M.Step == EStep::Returning) D->JobId = Job.Id; // a claim let go: it flies home for this job again
            if (M.Step == EStep::Returning && !F.HasRoute())
            {
                // No free route home yet (a door, a box in the way): hover and plan again.
                bAll = false;
                if (Now >= M.NextRetry)
                {
                    M.NextRetry = Now + 2;
                    HomeFlight(Job, M, D, F);
                }
                continue;
            }
            if (F.Done())
            {
                if (M.Step == EStep::Returning && D->State != ESCCargoDroneState::Docked)
                {
                    const FSCCargoDock* Dock = State->Docks.FindByPredicate([&](const FSCCargoDock& K) { return K.ItemId == D->DockItem; });
                    if (Dock && Dock->Actor.IsValid())
                    {
                        // The last snap into the bay is short; a long one is a drone put away from afar (it vanished).
                        const FTransform Bay = Dock->Actor->BayTransform(D->Bay);
                        UE_LOG(LogSCCargo, Display, TEXT("CARGO_DOCKED job=%d move=%d drone=%d snap_cm=%.0f"), Job.Id, M.MoveId, D->DroneId,
                               FVector::Dist(D->GetActorLocation(), Bay.GetLocation()));
                        D->Place(Bay);
                        D->AttachToActor(Dock->Actor.Get(), FAttachmentTransformRules::KeepWorldTransform);
                    }
                    D->State = ESCCargoDroneState::Docked;
                    D->JobId = INDEX_NONE;
                }
                continue;
            }
            bAll = false;
            // How far it may fly now: not into the dock corridor another drone is using, not onto a station a load
            // has yet to fly through. In its bay it stays docked until its way out is free (bays nearer to each other
            // than the drones keep apart so launch one at a time).
            const double Stop = FlightFree(D, F), Hold = M.Step == EStep::Outbound && M.HoldAt.IsValidIndex(I) ? M.HoldAt[I] : -1.0;
            if (D->State == ESCCargoDroneState::Docked)
            {
                if (M.Step != EStep::Outbound || (Stop >= 0 && Stop <= F.Dist + 1.0) || (Hold >= 0 && Hold <= F.Dist + 1.0)) continue;
                D->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
                FindBayOwn(D);
                D->State = ESCCargoDroneState::Launching;
            }
            // A dock corridor in use: it brakes for it. A station not free yet: the flight ends short of it for now.
            F.StopAt = Stop;
            F.HoldAt = Hold;
            // Not getting on though no rule holds it (its route ends up within the distance it keeps from a drone
            // that came to stand there after it was planned): a new route from where it is, round what stands now.
            if (M.FlightAt.Num() != Drones.Num())
            {
                M.FlightAt.Init(0, Drones.Num());
                M.FlightSince.Init(Now, Drones.Num());
            }
            if (Stop >= 0 || Hold >= 0 || F.Dist > M.FlightAt[I] + 1.0)
            {
                M.FlightAt[I] = F.Dist;
                M.FlightSince[I] = Now;
            }
            else if (Now - M.FlightSince[I] > FlightStallSeconds)
            {
                bool bNew = false;
                if (M.Step == EStep::Returning) bNew = HomeFlight(Job, M, D, F);
                else
                {
                    TArray<FVector> Path;
                    const FVector Ends[] = {D->GetActorLocation(), F.Points.Last()};
                    FSCCargoRouteQuery Query = DroneQuery(D, &M, Ends);
                    if (Query.Blocked(Ends[0]) || Query.Blocked(Ends[1])) Query = DroneQuery(D, &M, Ends, /*bTight=*/true);
                    FVector Start = Ends[0];
                    TArray<FVector> Via;
                    bNew = ClearStart(D, Query, Start, &Via) && PlanRoute(Job, M, TEXT("out_again"), Start, Ends[1], Query, Path, FString::Printf(TEXT("out_again:%d"), D->DroneId), false);
                    if (bNew && !Start.Equals(Ends[0], 1.0))
                    {
                        Path.Insert(Via, 0);
                        Path.Insert(Ends[0], 0);
                    }
                    if (bNew) F.Start(Path, Def.MaxSpeedMS * 100.0, Def.MaxAccelMS2 * 100.0, Def.MaxJerkMS3 * 100.0);
                }
                if (bNew || !M.bRoutePending)
                {
                    UE_LOG(LogSCCargo, Display, TEXT("CARGO_FLIGHT_AGAIN job=%d move=%d drone=%d step=%s at=%s found=%d"), Job.Id, M.MoveId, D->DroneId, StepName(M.Step),
                           *D->GetActorLocation().ToCompactString(), bNew);
                    M.FlightAt[I] = 0;
                    M.FlightSince[I] = Now;
                }
            }
            // The flight waits for a drone held up or turned aside (its own steps keep it clear of the other drones
            // and of what it would fly into); it comes back to its route at no more than twice its speed.
            if (FVector::Dist(D->GetActorLocation(), F.Position()) <= D->HullRadius() + Def.SwayAmplitudeCm) F.Step(Dt);
            Sway(D, FMath::Min(F.Dist, F.Length() - F.Dist) / SwayFadeCm, F.Speed, 0); // calm at the dock and over the box
            // Between its bay and the bay's exit the dock moves it (the flight's first leg out, its last leg home).
            D->bDockLeg = F.S.Num() > 2 && (M.Step == EStep::Outbound ? D->State == ESCCargoDroneState::Launching && F.Dist < F.S[1] : F.Dist > F.S[F.S.Num() - 2]);
            const FVector T = F.Tangent();
            D->Pose(F.Position(), T * F.Accel, float(Dt)); // on its own: it faces where it flies (its heading policy)
            // Out of its dock's corridor (bay, exit, clear of the dock's grid: the flight's first legs): on its way.
            if (M.Step == EStep::Outbound && D->State == ESCCargoDroneState::Launching && F.Dist >= (F.S.Num() > 2 ? F.S[2] : 0.0)) D->State = ESCCargoDroneState::ToPickup;
            if (M.Step == EStep::Returning && F.Length() - F.Dist < 300 && D->State != ESCCargoDroneState::Docking)
            {
                FindBayOwn(D);
                D->State = ESCCargoDroneState::Docking;
            }
        }
        if (!bAll) return;
        if (M.Step == EStep::Returning) { FinishMove(Job, M, M.Failure.IsEmpty()); return; }
        for (ASCCargoDrone* D : Drones) D->State = ESCCargoDroneState::Attaching;
        Enter(EStep::Attaching);
        return;
    }
    case EStep::Attaching:
    {
        // Demagnetize: the beams lock on, wide; then the box comes off the grid, drawn along the grid's normal.
        Formation(A->GetActorLocation(), M.FromNormal, M.PickRotation, FVector::ZeroVector, true, 70.f);
        if (Now - M.StepStart < MagnetSeconds) return;
        if (MustYield(A->GetActorLocation(), M.TransitPath[0], true))
        {
            if (Now >= M.NextRetry)
            {
                M.NextRetry = Now + 1;
                UE_LOG(LogSCCargo, Display, TEXT("CARGO_YIELD job=%d move=%d box=%d step=Attaching waits for a load lifting nearby"), Job.Id, M.MoveId, M.Plan.BoxId);
            }
            return;
        }
        if (From) From->Placed.RemoveAll([&](const FSCCargoPlacement& P) { return P.ItemId == Box->Id && P.Origin == Box->Slot.Origin; });
        else LeaveBeacon(*Box); // loose cargo: off its beacon as it leaves the floor
        Box->GridId = INDEX_NONE;
        Box->State = ESCCargoBoxState::Lifting;
        A->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
        A->Body->SetSimulatePhysics(false);
        const FVector Q0 = A->GetActorLocation();
        M.Carry.Start({Q0, M.TransitPath[0]}, Def.MaxSpeedMS * 100.0, M.LoadAccel, M.LoadJerk); // along the normal, as planned
        for (ASCCargoDrone* D : Drones) { D->State = ESCCargoDroneState::Carrying; D->CarryingBoxId = Box->Id; }
        Enter(EStep::Lifting);
        return;
    }
    case EStep::Lifting:
    {
        M.Carry.Step(Dt);
        const FVector Q = M.Carry.Position();
        // Up at the pickup rotation; at the top, turned onto the flight rotation when that differs (out of a room lengthwise).
        FQuat R = M.PickRotation;
        if (M.Carry.Done() && !M.PickRotation.Equals(M.FromRotation))
        {
            if (M.TurnStart < 0) M.TurnStart = Now;
            M.bTurning = true;
            const double T = FMath::SmoothStep(0.0, 1.0, (Now - M.TurnStart) / FlightTurnSeconds);
            R = FQuat::Slerp(M.PickRotation, M.FromRotation, T);
            if (T < 1)
            {
                A->SetActorLocationAndRotation(Q, R, false, nullptr, ETeleportType::TeleportPhysics);
                Formation(Q, M.FromNormal, R, FVector::ZeroVector, true, 30.f);
                return;
            }
            M.TurnStart = -1;
        }
        A->SetActorLocationAndRotation(Q, R, false, nullptr, ETeleportType::TeleportPhysics);
        Formation(Q, M.FromNormal, R, M.Carry.Tangent() * M.Carry.Accel, true, 30.f, -1, M.Carry.Tangent() * M.Carry.Speed);
        Sample(Q);
        if (!M.Carry.Done()) return;
        // Clear of the grid: the planned route begins, the box hanging from the lead drone's beam.
        M.Carry.Start(M.TransitPath, Def.MaxSpeedMS * 100.0, M.LoadAccel, M.LoadJerk);
        A->Release();
        A->Body->SetPhysicsLinearVelocity(M.Carry.Tangent() * M.Carry.Speed);
        for (ASCCargoDrone* D : Drones) D->Grab(A->Body); // every drone's beam holds its own attach point (TugStep)
        M.bRouteMoving = false;
        M.bRigid = false;
        M.ClearTo = 0;
        M.bBlockedAhead = false;
        Box->State = ESCCargoBoxState::InTransit;
        Enter(EStep::Transit);
        return;
    }
    case EStep::Transit:
    case EStep::Holding:
    {
        if (Drones.IsEmpty()) { Fail(TEXT("the tractor beam lost the box"), false); return; }
        const bool bOut = M.Plan.Kind == ESCCargoMoveKind::Hold && !M.bRestoring;
        FVector Q;
        FQuat Rotation;
        bool bPrecise = false;
        if (M.Step == EStep::Holding)
        {
            // Station keeping: a slow small drift around the holding point.
            Q = M.HoldPoint + FVector(FMath::Sin(Now * 0.7) * 6, FMath::Cos(Now * 0.5) * 6, FMath::Sin(Now * 0.9) * 4);
            Rotation = M.FromRotation;
            const FSCCargoMove* Target = Job.Moves.FindByPredicate([&](const FSCCargoMove& O) { return O.Plan.BoxId == M.Plan.ForBoxId && O.Plan.Kind != ESCCargoMoveKind::Hold; });
            bool bRelease = !Target || (Target->Step != EStep::Waiting && Target->Step != EStep::Outbound && Target->Step != EStep::Attaching && Target->Step != EStep::Lifting);
            // Lower blockers (later in the dig) go back first.
            bool bAfter = false;
            for (const FSCCargoMove& O : Job.Moves)
            {
                if (&O == &M) { bAfter = true; continue; }
                if (bAfter && O.Plan.ForBoxId == M.Plan.ForBoxId && O.Plan.Kind == ESCCargoMoveKind::Hold &&
                    O.Step != EStep::Returning && O.Step != EStep::Done && O.Step != EStep::Failed)
                    bRelease = false;
            }
            if (bRelease && Now >= M.NextRetry)
            {
                // The slot is decided now, against the grid as it is: a cancelled or failed dig leaves the target where
                // it was, and the slot planned for after its removal may be inside it.
                if (!ReSlot(TEXT("put_back"))) { Fail(TEXT("no free slot to put the held box back")); return; }
                const FTransform Slot = To->SlotToWorld(SlotOf(M.Plan), Box->SizeM());
                double Lift = 0, Hang = 0;
                LiftPlan(A, Slot, M.ToNormal, Lift, Hang);
                // A hover point over the slot where the load fits, turns onto the lattice and goes down free.
                M.NextRetry = Now + FMath::Min(2.0 + M.Replans, 6.0);
                if (M.Replans >= MaxReplans) { Fail(TEXT("no free route to put the held box back")); return; }
                FString ApproachWhy;
                if (!SlotApproach(M, A, Slot, M.ToNormal, A->GetActorQuat(), FMath::Max(Lift, Def.NormalClearanceM * 100.0), M.Goal, M.Approach, &ApproachWhy))
                {
                    ++M.Replans;
                    UE_LOG(LogSCCargo, Display, TEXT("CARGO_HOLD job=%d move=%d box=%d put_back_wait=\"%s\""), Job.Id, M.MoveId, Box->Id, *ApproachWhy);
                    return;
                }
                if (Replan(A->GetActorLocation(), TEXT("put_back")))
                {
                    M.bRestoring = true;
                    M.FromNormal = Up;
                    M.ToRotation = Slot.GetRotation();
                    Box->State = ESCCargoBoxState::InTransit;
                    for (ASCCargoDrone* D : Drones) D->State = ESCCargoDroneState::Carrying;
                    Enter(EStep::Transit);
                }
            }
        }
        else
        {
            // Guard. The route is swept again as it is flown, along itself in short stretches (never a chord across its
            // bends), as far ahead as the load needs to brake to rest: what is not free stops it where the free stretch
            // ends, at its own deceleration (a load stopped dead swings on into what stopped it). Other loads in the
            // air are moving obstacles: the later of two that would meet brakes and waits (the earlier one, lower box
            // id, keeps going). Stopped by anything else, the load plans again from there; without a route it holds
            // position and retries, and fails after MaxReplans tries.
            const FVector Here = M.Carry.Position();
            const double BrakeCm = M.Carry.Speed * M.Carry.Speed / FMath::Max(2 * 0.7 * M.LoadAccel, 1.0) + FMath::Max(M.Carry.Speed * Dt * 3, 30.0);
            const double Need = FMath::Min(M.Carry.Length(), M.Carry.Dist + BrakeCm);
            RefreshMove(M);
            if (M.GuardStamp != M.WorldStamp || (M.bBlockedAhead && Now >= M.NextRetry && M.Carry.Speed >= 1))
            {
                // The world moved (a ramp, a box set down), or what blocked may have gone: swept again from here.
                M.GuardStamp = M.WorldStamp;
                M.ClearTo = M.Carry.Dist;
                M.bBlockedAhead = false;
            }
            M.ClearTo = FMath::Max(M.ClearTo, M.Carry.Dist);
            while (!M.bStalled && !M.bBlockedAhead && M.ClearTo < Need)
            {
                const double Next = FMath::Min(Need, M.ClearTo + SCCargoFlightTuning::GuardStretchCm);
                if (AheadFree(M.Carry.At(M.ClearTo), M.Carry.At(Next))) M.ClearTo = Next;
                else M.bBlockedAhead = true;
            }
            const bool bYield = !M.bStalled && MustYield(Here, M.Carry.At(Need));
            // The route also waits for a box left behind: beyond the beam's full-strength distance the team could not
            // bring it along (a box held up by something the sweeps did not see).
            // It waits as well while a drone of the team finds no room for its station there (it may clear: a load
            // passing, a door opening); if it does not, the same new route.
            // A route planned because a drone had no station is flown even so: the load leads its drone out of the pocket
            // it was caught in (the station frees as the load moves on; the guard still sweeps the load's way).
            const bool bBoxBehind = (M.bNoStation && !M.bLeaveNoStation) || (!M.bRigid && FVector::Dist(A->GetActorLocation(), Here) > Def.FullStrengthDistanceM * 100.0);
            // And for a drone of the team held up behind something (it tucks in and comes after).
            const bool bBehind = bBoxBehind || M.bDroneBehind;
            M.Carry.StopAt = bYield || bBehind ? M.Carry.Dist : (M.bBlockedAhead ? M.ClearTo : -1.0);
            if (M.bStalled)
            {
                // Holding position (the flight is not over: the load is not where it is going).
                if (Now >= M.NextRetry)
                {
                    M.NextRetry = Now + FMath::Min(1.0 + M.Replans, 5.0); // backs off
                    if (Replan(Here, TEXT("stalled")))
                    {
                        M.bStalled = false;
                        M.ClearTo = 0;
                        M.bBlockedAhead = false;
                    }
                    else if (M.Replans > MaxReplans) { Fail(TEXT("no free route from where the load stopped")); return; }
                }
            }
            else if (bYield)
            {
                if (Now >= M.NextRetry)
                {
                    M.NextRetry = Now + 1;
                    UE_LOG(LogSCCargo, Display, TEXT("CARGO_YIELD job=%d move=%d box=%d waits for a load ahead"), Job.Id, M.MoveId, M.Plan.BoxId);
                }
            }
            else if (bBoxBehind && M.Carry.Speed < 1 && Now >= M.NextRetry && A->Body->GetPhysicsLinearVelocity().Size() < 10)
            {
                // Waited for and not coming (it rests against something): a route from where the box is.
                M.NextRetry = Now + FMath::Min(1.0 + M.Replans, 5.0);
                const bool bNoStation = M.bNoStation;
                const bool bFound = Replan(A->GetActorLocation(), bNoStation ? TEXT("no_station") : TEXT("left_behind"));
                if (bFound && bNoStation) M.bLeaveNoStation = true;
                M.ClearTo = 0;
                M.bBlockedAhead = false;
                if (!bFound && M.Replans > MaxReplans) { Fail(TEXT("no free route from where the load was held up")); return; }
            }
            else if (M.bBlockedAhead && M.Carry.Speed < 1 && Now >= M.NextRetry)
            {
                // At rest where the free stretch ends: a new route from here.
                M.NextRetry = Now + 1;
                const bool bFound = Replan(Here, TEXT("blocked_ahead"));
                M.ClearTo = 0;
                M.bBlockedAhead = false;
                if (!bFound)
                {
                    if (M.Replans > MaxReplans) { Fail(TEXT("no free route from where the load stopped")); return; }
                    M.bStalled = true; // holds position; plans again
                }
            }
            // In the destination's precise zone (and braking into it): slow, so the box follows its beam closely.
            const double SlowCm = M.Carry.Speed * M.Carry.Speed / FMath::Max(2 * 0.7 * M.LoadAccel, 1.0) + SCCargoFlightTuning::PreciseLeadCm;
            bPrecise = InPreciseZone(M, A, M.Carry.Position()) || InPreciseZone(M, A, M.Carry.At(FMath::Min(M.Carry.Dist + SlowCm, M.Carry.Length())));
            M.Carry.SpeedLimit = bPrecise ? SCCargoFlightTuning::PreciseSpeedCmS : 0;
            // The route advances with the physics that carries the box: a long frame simulates at most one physics step.
            if (!M.bStalled) M.Carry.Step(FMath::Min<double>(Dt, UPhysicsSettings::Get()->MaxPhysicsDeltaTime));
            Q = M.Carry.Position();
            Rotation = M.FromRotation; // kept on the way (the route was swept so); turned onto the lattice when caught
        }
        FormationFade = M.Step == EStep::Holding ? 1.0 : FMath::Min(M.Carry.Dist, M.Carry.Length() - M.Carry.Dist) / SwayFadeCm;
        // The gusts die down as the load slows into the precise zone (the route there is swept without their envelope).
        if (bPrecise) FormationFade *= FMath::Clamp(M.Carry.Speed / SCCargoFlightTuning::PreciseSpeedCmS - 1.0, 0.0, 1.0);
        // At the hold's entrance (the first leg flown): turned onto the hold's lattice (swept when planned), then in.
        const bool bTurning = M.Step == EStep::Transit && !M.bStalled && M.Carry.Done() && !M.SecondPath.IsEmpty();
        if (bTurning && M.TurnStart < 0) M.TurnStart = Now;
        M.bTurning = bTurning;
        const double TurnT = bTurning ? FMath::SmoothStep(0.0, 1.0, (Now - M.TurnStart) / FlightTurnSeconds) : 0.0;
        if (bTurning) Rotation = FQuat::Slerp(M.FromRotation, M.SecondRotation, TurnT);
        // The team tugs: every drone on its own station, its beam's spring on its attach point (TugStep). Each drone's
        // gusts are its own; the box feels them all through the springs.
        for (ASCCargoDrone* D : Drones) Sway(D, FormationFade, M.Carry.Speed, LoadMass);
        // In the precise zone the beams hold the box rigidly on its route (as on the lift and the descent): it passes
        // boxes at rest 2 cm from them, closer than a spring settles. What it was off the route by when it came in
        // dies away at the beams' own rate. The drones keep flying their own stations.
        if (bPrecise && !M.bRigid && A->Body->IsSimulatingPhysics())
        {
            M.bRigid = true;
            M.RigidOffset = A->GetActorLocation() - Q;
            M.RigidRotation = A->GetActorQuat();
            A->Body->SetSimulatePhysics(false);
        }
        if (M.bRigid)
        {
            const double Keep = FMath::Exp(-FMath::Sqrt(FMath::Max(1, Drones.Num()) * TractorBeam().StiffnessNPerM() / FMath::Max(LoadMass, 1.0)) * Dt);
            M.RigidOffset *= Keep;
            M.RigidRotation = FQuat::Slerp(Rotation, M.RigidRotation, Keep);
            A->SetActorLocationAndRotation(Q + M.RigidOffset, M.RigidRotation, false, nullptr, ETeleportType::TeleportPhysics);
        }
        // The route's velocity and its whole acceleration (along it and round its bends: the change of that velocity).
        const double TugDt = FMath::Min<double>(Dt, UPhysicsSettings::Get()->MaxPhysicsDeltaTime);
        const FVector RouteV = M.Step != EStep::Holding && !M.bStalled ? M.Carry.Tangent() * M.Carry.Speed : FVector::ZeroVector;
        const FVector RouteA = M.bRouteMoving && TugDt > 0 ? ((RouteV - M.RouteV) / TugDt).GetClampedToMaxSize(M.LoadAccel * UE_DOUBLE_SQRT_2) : FVector::ZeroVector;
        M.RouteV = RouteV;
        M.bRouteMoving = true;
        TugStep(M, A, Drones, Q, Rotation, RouteV, RouteA, Now, TugDt);
        Sample(Q);
        if (M.Step == EStep::Holding || M.bStalled || !M.Carry.Done()) return;
        if (!M.SecondPath.IsEmpty())
        {
            if (TurnT < 1) return;
            M.FromRotation = M.SecondRotation;
            M.Carry.Start(M.SecondPath, Def.MaxSpeedMS * 100.0, M.LoadAccel, M.LoadJerk);
            M.SecondPath.Reset();
            M.TurnStart = -1;
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_MOVE job=%d move=%d box=%d turned at the hold entrance"), Job.Id, M.MoveId, M.Plan.BoxId);
            return;
        }
        if (bOut)
        {
            Box->State = ESCCargoBoxState::Held;
            for (ASCCargoDrone* D : Drones) D->State = ESCCargoDroneState::Holding;
            Enter(EStep::Holding);
            return;
        }
        // Over the destination: wait for the slot's support (a box below it still on its way), then the approach.
        TSet<int32> Pending(To->Reserved);
        Pending.Remove(Box->Id);
        if (!To->Model().IsSupported(M.Plan.Origin, M.Plan.Cells, Box->Id, &Pending))
        {
            // Still on its way: wait. Gone (the move bringing it failed or was cancelled): another slot, a new approach.
            if (To->Model().IsSupported(M.Plan.Origin, M.Plan.Cells, Box->Id) || Now < M.NextRetry) return;
            M.NextRetry = Now + 1;
            if (!ReSlot(TEXT("support_gone"))) { Fail(TEXT("the box it was to rest on did not arrive and no other slot fits")); return; }
            const FTransform NewSlot = To->SlotToWorld(SlotOf(M.Plan), Box->SizeM());
            FString ApproachWhy;
            if (!SlotApproach(M, A, NewSlot, M.ToNormal, A->GetActorQuat(), Def.NormalClearanceM * 100.0, M.Goal, M.Approach, &ApproachWhy))
            {
                if (++M.Replans > MaxReplans) Fail(TEXT("no room over the new slot: ") + ApproachWhy);
                return;
            }
            M.ToRotation = NewSlot.GetRotation();
            if (!Replan(A->GetActorLocation(), TEXT("support_gone"))) M.bStalled = true;
            return;
        }
        // The turn onto the lattice (from where the box hangs to the hover point at the slot's rotation) and the way down
        // are swept as the world is now (a neighbour may have landed since): blocked, a hover point further out, or wait.
        const FTransform Slot = To->SlotToWorld(SlotOf(M.Plan), Box->SizeM());
        FString TurnWhy;
        if (!TurnClear(M, A, M.Goal, A->GetActorQuat(), M.Goal, Slot.GetRotation(), &TurnWhy) ||
            !DescentClear(A, Slot.GetRotation(), M.Approach, Slot.GetLocation(), &TurnWhy) ||
            (!M.Goal.Equals(M.Approach, 1.0) && !DescentClear(A, Slot.GetRotation(), M.Goal, M.Approach, &TurnWhy)))
        {
            if (Now < M.NextRetry) return;
            M.NextRetry = Now + 1;
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_TURN job=%d move=%d box=%d blocked=\"%s\""), Job.Id, M.MoveId, Box->Id, *TurnWhy);
            if (M.Replans > MaxReplans) { Fail(TEXT("no room to turn and set the box down: ") + TurnWhy); return; }
            FVector NewGoal;
            const double Out = FVector::DotProduct(M.Approach - Slot.GetLocation(), M.ToNormal) + 25;
            FVector NewHover;
            if (SlotApproach(M, A, Slot, M.ToNormal, A->GetActorQuat(), Out, NewGoal, NewHover, &TurnWhy))
            {
                M.Goal = NewGoal;
                M.Approach = NewHover;
                if (!Replan(A->GetActorLocation(), TEXT("turn_room"))) M.bStalled = true;
            }
            else ++M.Replans;
            return;
        }
        for (ASCCargoDrone* D : Drones) D->Drop();
        M.CatchFrom = A->GetActorTransform();
        A->Body->SetSimulatePhysics(false);
        TArray<FVector> Down = {M.Goal};
        if (!M.Goal.Equals(M.Approach, 1.0)) Down.Add(M.Approach); // turned above the hover point: down to it first
        Down.Add(Slot.GetLocation());
        M.Carry.Start(Down, Def.MaxSpeedMS * 100.0, M.LoadAccel, M.LoadJerk);
        Box->State = ESCCargoBoxState::Placing;
        M.CatchStart = -1;
        Enter(EStep::Lowering);
        return;
    }
    case EStep::Lowering:
    {
        // Caught over the slot (aligned to the lattice), then drawn down along the destination grid's normal.
        // A box that tilts as it is caught (onto a plate standing upright, or back off one) sweeps the sphere about
        // its centre, and its drones swing from over it to the side the plate's normal points to. They first move
        // out of that sphere (their beams reach that far: the box hangs still meanwhile), fly round it as it turns,
        // and come in again along the normal as it goes down. A box that only turns level keeps them over its top.
        const bool bSwing = FVector::DotProduct(M.CatchFrom.GetRotation().GetUpVector(), M.ToRotation.GetUpVector()) < SwingCos || FVector::DotProduct(M.ToNormal, Up) < SwingCos;
        double Out = M.HangCm;
        if (bSwing)
            for (const ASCCargoDrone* D : Drones) Out = FMath::Max(Out, Size.Size() / 2 + D->HullHalf.Size() + SCCargoFlightTuning::DroneClearanceCm / 2 - Size.Z / 2);
        if (M.CatchStart < 0)
        {
            const FVector Q0 = M.CatchFrom.GetLocation();
            Formation(Q0, Up, M.CatchFrom.GetRotation(), FVector::ZeroVector, true, 30.f, Out);
            bool bClear = true;
            for (const ASCCargoDrone* D : Drones) bClear &= FVector::Dist(D->GetActorLocation(), Q0) >= Size.Z / 2 + Out - SCCargoFlightTuning::DroneClearanceCm / 2;
            if (bSwing && !bClear && Now - M.StepStart < SwingWaitSeconds) return;
            M.CatchStart = Now;
        }
        const double Catch = (Now - M.CatchStart) / CatchSeconds;
        FVector Q;
        if (Catch < 1)
        {
            M.bTurning = true;
            M.bTilting = bSwing;
            // To the turn point at the arriving rotation (the hanging box's lag taken up), then turned there.
            const double Move = FMath::SmoothStep(0.0, 1.0, Catch / 0.4), Turn = FMath::SmoothStep(0.0, 1.0, (Catch - 0.4) / 0.6);
            const FTransform Pose(FQuat::Slerp(M.CatchFrom.GetRotation(), M.ToRotation, Turn), FMath::Lerp(M.CatchFrom.GetLocation(), M.Carry.At(0), Move));
            A->SetActorTransform(Pose, false, nullptr, ETeleportType::TeleportPhysics);
            Q = Pose.GetLocation();
            // The drones round the turning box: from over it to the plate's normal, at the distance that clears it.
            const FVector Side = FQuat::Slerp(FQuat::Identity, FQuat::FindBetweenNormals(Up, M.ToNormal), Turn).RotateVector(Up);
            Formation(Q, Side, Pose.GetRotation(), FVector::ZeroVector, true, 30.f, Out);
        }
        else
        {
            M.Carry.Step(Dt);
            Q = M.Carry.Position();
            A->SetActorLocationAndRotation(Q, M.ToRotation, false, nullptr, ETeleportType::TeleportPhysics);
            Formation(Q, M.ToNormal, M.ToRotation, M.Carry.Tangent() * M.Carry.Accel, true, 30.f, -1, M.Carry.Tangent() * M.Carry.Speed);
        }
        Sample(Q);
        if (Catch < 1 || !M.Carry.Done()) return;
        for (ASCCargoDrone* D : Drones) D->State = ESCCargoDroneState::Releasing;
        Enter(EStep::Placing);
        return;
    }
    case EStep::Placing:
    {
        // Magnetize: the beams flare while the grid takes the box, then it is locked on its cell.
        Formation(A->GetActorLocation(), M.ToNormal, M.ToRotation, FVector::ZeroVector, true, 70.f);
        if (Now - M.StepStart < MagnetSeconds) return;
        const FTransform Slot = To->SlotToWorld(SlotOf(M.Plan), Box->SizeM());
        A->Lock(To, Slot);
        To->Reserved.Remove(Box->Id);
        if (!To->Placed.ContainsByPredicate([&](const FSCCargoPlacement& P) { return P.ItemId == Box->Id; })) To->Placed.Add(SlotOf(M.Plan));
        Box->GridId = To->GridId;
        Box->Slot = SlotOf(M.Plan);
        Box->State = ESCCargoBoxState::Stored;
        Box->JobId = INDEX_NONE;
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_MOVE job=%d move=%d box=%d placed grid=%d origin=%s rotated=%d"), Job.Id, M.MoveId, Box->Id, To->GridId,
               *M.Plan.Origin.ToString(), M.Plan.bRotated);
        SendHome(M, Now);
        Enter(EStep::Returning);
        return;
    }
    default: return;
    }
}
