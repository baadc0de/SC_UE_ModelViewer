// USCCargoSubsystem moves in flight: drones fly their own routes out (empty, nimble), demagnetize the box and draw it
// along its grid's normal, carry it hanging from the lead drone's soft beam constraint along a planned route (heavy:
// acceleration from the beam's force and the load's mass), hold it at a holding point when it is a blocker being dug
// out, catch it over the destination, lower it along that grid's normal, magnetize it onto its slot, and fly home.
// Log: CARGO_MOVE (steps), CARGO_REPLAN (a blocked route planned again), CARGO_CARRY (box samples in flight).
#include "Cargo/SCCargoSubsystem.h"
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

const TCHAR* StepName(EStep S)
{
    static const TCHAR* Names[] = {TEXT("Waiting"), TEXT("Outbound"), TEXT("Attaching"), TEXT("Lifting"), TEXT("Transit"), TEXT("Lowering"),
                                   TEXT("Placing"),  TEXT("Returning"), TEXT("Done"),     TEXT("Failed"),  TEXT("Holding")};
    return Names[uint8(S)];
}
FSCCargoPlacement SlotOf(const FSCCargoMovePlan& P) { return SCCargoSlotOf(P); }
// Direction turning from A (T = 0) to B (T = 1), eased.
FVector Turn(const FVector& A, const FVector& B, double T)
{
    return FQuat::Slerp(FQuat::Identity, FQuat::FindBetweenNormals(A, B), FMath::SmoothStep(0.0, 1.0, FMath::Clamp(T, 0.0, 1.0))).RotateVector(A);
}
constexpr double FormationTurnCm = 400; // the drones swing from over the grid's normal to over the box (up) within this
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
    auto Fail = [&](const FString& Why) {
        M.Failure = Why;
        for (ASCCargoDrone* D : Drones) D->Drop();
        if (A && A->Body && !A->Body->IsSimulatingPhysics()) A->Release(); // left where it is, under gravity
        ReleaseMove(M);
        if (Box && Box->GridId == INDEX_NONE) { Box->State = ESCCargoBoxState::Stored; Box->JobId = INDEX_NONE; }
        UE_LOG(LogSCCargo, Warning, TEXT("CARGO_MOVE job=%d move=%d box=%d failed at %s: %s"), Job.Id, M.MoveId, M.Plan.BoxId, StepName(M.Step), *Why);
        SendHome(M, Now);
    };
    if (M.Step != EStep::Returning && M.Step != EStep::Holding && (!Box || !A || !From || !To))
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
        if (Drones.IsEmpty() && M.Step != EStep::Returning) { Fail(TEXT("the drone team is gone")); return; }
        const int32 ByVolume = Def.MaxVolume > 0 && Box ? FMath::CeilToInt(Box->VolumeSCU() / Def.MaxVolume - 1e-9) : 1;
        if (bLoaded && Box && Drones.Num() < FMath::Max(FSCCargoPlanner::DronesFor(Box->Occupancy.Mass, Gravity(), Def.MaxForceN), ByVolume))
        {
            Fail(FString::Printf(TEXT("%d drone(s) left cannot hold the box"), Drones.Num()));
            return;
        }
        // The lead drone holds the beam's constraint: a new lead takes it over where the box is.
        if ((M.Step == EStep::Transit || M.Step == EStep::Holding) && A && A->Body && !Drones[0]->IsHolding())
            Drones[0]->Grab(A->Body, A->GetActorLocation() + A->GetActorQuat().GetUpVector() * A->SizeCm().Z / 2, A->GetActorRotation());
        if (Box)
        {
            const double LoadAccel = SCCargoLoadedAccel(Drones.Num(), Def.MaxForceN, Box->Occupancy.Mass, Def.DroneMassKg, Gravity(), Def.MaxAccelMS2);
            M.LoadAccel = LoadAccel * 100.0;
            M.LoadJerk = Def.MaxJerkMS3 * 100.0 * LoadAccel / FMath::Max<double>(Def.MaxAccelMS2, 1e-3);
        }
    }
    if (Drones.IsEmpty() && M.Step != EStep::Returning && M.Step != EStep::Done && M.Step != EStep::Failed)
    {
        Fail(TEXT("the move has no drones"));
        return;
    }
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
    auto Formation = [&](const FVector& Q, const FVector& N, const FQuat& Rotation, const FVector& Accel, bool bBeams, float Width) {
        for (int32 I = 0; I < Drones.Num() && I < M.Offsets.Num(); ++I)
        {
            Sway(Drones[I], FormationFade, M.Carry.Speed, LoadMass);
            const FVector P = Q + N * (Size.Z / 2 + M.HangCm) + Rotation.RotateVector(M.Offsets[I]);
            Drones[I]->Pose(P, Accel, float(Rotation.Rotator().Yaw), float(Dt));
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
        // Planned by PrepareMove (routes swept free): the drones leave their bays.
        for (ASCCargoDrone* D : Drones)
        {
            if (D->State == ESCCargoDroneState::Docked) D->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
            D->State = ESCCargoDroneState::Launching;
        }
        Box->State = ESCCargoBoxState::Reserved;
        Enter(EStep::Outbound);
        return;
    }
    case EStep::Outbound:
    case EStep::Returning:
    {
        bool bAll = true;
        for (int32 I = 0; I < Drones.Num() && I < M.DroneFlights.Num(); ++I)
        {
            ASCCargoDrone* D = Drones[I];
            FSCCargoFlight& F = M.DroneFlights[I];
            if (M.Step == EStep::Returning && D->MoveId != INDEX_NONE) continue; // claimed by another move on the way home
            if (M.Step == EStep::Returning && F.Points.Num() < 2)
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
                        D->Place(Dock->Actor->BayTransform(D->Bay));
                        D->AttachToActor(Dock->Actor.Get(), FAttachmentTransformRules::KeepWorldTransform);
                    }
                    D->State = ESCCargoDroneState::Docked;
                    D->JobId = INDEX_NONE;
                }
                continue;
            }
            bAll = false;
            F.Step(Dt);
            Sway(D, FMath::Min(F.Dist, F.Length() - F.Dist) / SwayFadeCm, F.Speed, 0); // calm at the dock and over the box
            const FVector T = F.Tangent();
            D->Pose(F.Position(), T * F.Accel, FVector(T.X, T.Y, 0).SizeSquared() > 0.04 ? float(T.Rotation().Yaw) : float(D->GetActorRotation().Yaw), float(Dt));
            if (M.Step == EStep::Outbound && F.Dist > 150) D->State = ESCCargoDroneState::ToPickup;
            if (M.Step == EStep::Returning && F.Length() - F.Dist < 300) D->State = ESCCargoDroneState::Docking;
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
        From->Placed.RemoveAll([&](const FSCCargoPlacement& P) { return P.ItemId == Box->Id && P.Origin == Box->Slot.Origin; });
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
        Formation(Q, M.FromNormal, R, M.Carry.Tangent() * M.Carry.Accel, true, 30.f);
        Sample(Q);
        if (!M.Carry.Done()) return;
        // Clear of the grid: the planned route begins, the box hanging from the lead drone's beam.
        M.Carry.Start(M.TransitPath, Def.MaxSpeedMS * 100.0, M.LoadAccel, M.LoadJerk);
        A->Release();
        A->Body->SetPhysicsLinearVelocity(M.Carry.Tangent() * M.Carry.Speed);
        Drones[0]->Grab(A->Body, Q + M.FromRotation.GetUpVector() * Size.Z / 2, FRotator(M.FromRotation));
        Box->State = ESCCargoBoxState::InTransit;
        Enter(EStep::Transit);
        return;
    }
    case EStep::Transit:
    case EStep::Holding:
    {
        if (Drones.IsEmpty() || !Drones[0]->IsHolding()) { Fail(TEXT("the tractor beam lost the box")); return; }
        const bool bOut = M.Plan.Kind == ESCCargoMoveKind::Hold && !M.bRestoring;
        FVector Q;
        FQuat Rotation;
        FVector N;
        bool bPrecise = false;
        if (M.Step == EStep::Holding)
        {
            // Station keeping: a slow small drift around the holding point.
            Q = M.HoldPoint + FVector(FMath::Sin(Now * 0.7) * 6, FMath::Cos(Now * 0.5) * 6, FMath::Sin(Now * 0.9) * 4);
            Rotation = M.FromRotation;
            N = Up;
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
            // Guard. Other loads in the air are moving obstacles: the later of two that would meet waits where it is (the
            // earlier one, lower box id, keeps going). Anything else ahead (static) stops the load and it plans again;
            // without a route it holds position and retries, and fails after MaxReplans tries.
            const FVector Here = M.Carry.Position();
            const FVector Ahead = M.Carry.At(M.Carry.Dist + FMath::Max(M.Carry.Speed * Dt * 3, 30.0));
            const bool bYield = !M.bStalled && MustYield(Here, Ahead);
            if (M.bStalled)
            {
                // Holding position (the flight is not over: the load is not where it is going).
                if (Now >= M.NextRetry)
                {
                    M.NextRetry = Now + FMath::Min(1.0 + M.Replans, 5.0); // backs off
                    if (Replan(Here, TEXT("stalled"))) M.bStalled = false;
                    else if (M.Replans > MaxReplans) { Fail(TEXT("no free route from where the load stopped")); return; }
                }
            }
            else if (bYield)
            {
                M.Carry.Speed = M.Carry.Accel = 0;
                if (Now >= M.NextRetry)
                {
                    M.NextRetry = Now + 1;
                    UE_LOG(LogSCCargo, Display, TEXT("CARGO_YIELD job=%d move=%d box=%d waits for a load ahead"), Job.Id, M.MoveId, M.Plan.BoxId);
                }
            }
            else if (Now >= M.NextRetry && !Ahead.Equals(Here, 1.0) && !AheadFree(Here, Ahead))
            {
                M.NextRetry = Now + 1;
                M.Carry.Speed = M.Carry.Accel = 0;
                if (!Replan(Here, TEXT("blocked_ahead")))
                {
                    if (M.Replans > MaxReplans) { Fail(TEXT("no free route from where the load stopped")); return; }
                    M.bStalled = true; // holds position; plans again
                }
            }
            // In the destination's precise zone (and braking into it): slow, so the box follows its beam closely.
            const double BrakeCm = M.Carry.Speed * M.Carry.Speed / FMath::Max(2 * 0.7 * M.LoadAccel, 1.0) + SCCargoFlightTuning::PreciseLeadCm;
            bPrecise = InPreciseZone(M, A, M.Carry.Position()) || InPreciseZone(M, A, M.Carry.At(FMath::Min(M.Carry.Dist + BrakeCm, M.Carry.Length())));
            M.Carry.SpeedLimit = bPrecise ? SCCargoFlightTuning::PreciseSpeedCmS : 0;
            if (!bYield && !M.bStalled) M.Carry.Step(Dt);
            Q = M.Carry.Position();
            Rotation = M.FromRotation; // kept on the way (the route was swept so); turned onto the lattice when caught
            // The drones fly over the box (the column the route was swept with); off a grid and onto one they swing
            // over from its normal, close to the ends of the route.
            const double FromEnd = M.Carry.Length() - M.Carry.Dist;
            N = M.Carry.Dist < FormationTurnCm ? Turn(M.FromNormal, Up, M.Carry.Dist / FormationTurnCm)
                : (!bOut && FromEnd < FormationTurnCm ? Turn(M.ToNormal, Up, FromEnd / FormationTurnCm) : Up);
        }
        // The beam holds the box's top centre; the soft constraint's sag (g / k) is lifted out.
        // The lead's gusts reach the box through the beam (the soft constraint swings it).
        const FVector Grab = Q + Rotation.GetUpVector() * Size.Z / 2 + Up * (Gravity() * 100.0 / ASCCargoDrone::HoldStiffness) + Drones[0]->SwayOffset();
        FormationFade = M.Step == EStep::Holding ? 1.0 : FMath::Min(M.Carry.Dist, M.Carry.Length() - M.Carry.Dist) / SwayFadeCm;
        // The gusts die down as the load slows into the precise zone (the route there is swept without their envelope).
        if (bPrecise) FormationFade *= FMath::Clamp(M.Carry.Speed / SCCargoFlightTuning::PreciseSpeedCmS - 1.0, 0.0, 1.0);
        Drones[0]->Hold(Grab, FRotator(Rotation));
        Formation(Q, N, Rotation, M.Step == EStep::Holding ? FVector::ZeroVector : M.Carry.Tangent() * M.Carry.Accel, true, 30.f);
        Sample(Q);
        if (M.Step == EStep::Holding || M.bStalled || !M.Carry.Done()) return;
        if (!M.SecondPath.IsEmpty())
        {
            // At the hold's entrance: turned onto the hold's lattice (swept when planned), then in.
            if (M.TurnStart < 0) M.TurnStart = Now;
            const double T = FMath::SmoothStep(0.0, 1.0, (Now - M.TurnStart) / FlightTurnSeconds);
            Drones[0]->Hold(Q + FQuat::Slerp(M.FromRotation, M.SecondRotation, T).GetUpVector() * Size.Z / 2 + Up * (Gravity() * 100.0 / ASCCargoDrone::HoldStiffness),
                            FRotator(FQuat::Slerp(M.FromRotation, M.SecondRotation, T)));
            if (T < 1) return;
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
        Enter(EStep::Lowering);
        return;
    }
    case EStep::Lowering:
    {
        // Caught over the slot (aligned to the lattice), then drawn down along the destination grid's normal.
        const double Catch = (Now - M.StepStart) / CatchSeconds;
        FVector Q;
        if (Catch < 1)
        {
            // To the turn point at the arriving rotation (the hanging box's lag taken up), then turned there.
            const double Move = FMath::SmoothStep(0.0, 1.0, Catch / 0.4), Turn = FMath::SmoothStep(0.0, 1.0, (Catch - 0.4) / 0.6);
            const FTransform Pose(FQuat::Slerp(M.CatchFrom.GetRotation(), M.ToRotation, Turn), FMath::Lerp(M.CatchFrom.GetLocation(), M.Carry.At(0), Move));
            A->SetActorTransform(Pose, false, nullptr, ETeleportType::TeleportPhysics);
            Q = Pose.GetLocation();
        }
        else
        {
            M.Carry.Step(Dt);
            Q = M.Carry.Position();
            A->SetActorLocationAndRotation(Q, M.ToRotation, false, nullptr, ETeleportType::TeleportPhysics);
        }
        Formation(Q, M.ToNormal, M.ToRotation, Catch < 1 ? FVector::ZeroVector : M.Carry.Tangent() * M.Carry.Accel, true, 30.f);
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
