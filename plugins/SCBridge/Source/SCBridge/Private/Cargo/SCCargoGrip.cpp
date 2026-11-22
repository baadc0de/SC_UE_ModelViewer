// USCCargoSubsystem: where each carrying drone holds its box from (WP-K4 round 5). A drone holds from a place of its own
// round the box (FSCCargoGripSpot: in front of an end, beside a side, over the top) and every so often re-picks it, as if
// searching for a better grip (FSCCargoGripPick, drone_art.json "grip"): mostly the ends, never where a teammate is, only
// where its hull fits along the route ahead and on its way there and back (SpotFits), and no further round the box than a
// grip adjustment (MoveCap, along its way) once it has its first grip; when its own place stops fitting, the nearest
// that does, however far. The route's shape covers each
// drone's planned place (PlannedSpot: its end of the ends grip, or over its formation offset); every other place is
// tried against the world as the box flies, and a drone goes back to its planned place before the route's piece ends
// (a turn, a swap, the descent) or in the precise zone, one drone of the team at a time (ReturnAheadS), or when its own
// place stops fitting. Log: CARGO_GRIP_PICK.
// Also the load's route margin with what the drones' wobble passes on to it (LoadEnvelopeCm).
#include "Cargo/SCCargoSubsystem.h"
#include "HAL/PlatformTime.h"
#include "SCCargoState.h"
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoDrone.h"
#include "Cargo/SCCargoDroneArt.h"
#include "Cargo/SCCargoFlightPath.h"

using namespace SCCargoFlightTuning;

namespace
{
const TCHAR* KindName(FSCCargoGripSpot::EKind Kind)
{
    switch (Kind)
    {
    case FSCCargoGripSpot::EKind::End: return TEXT("end");
    case FSCCargoGripSpot::EKind::Side: return TEXT("side");
    default: return TEXT("over");
    }
}
// A drone's own stream for its pick K in a move.
FRandomStream PickStream(const FSCCargoMove& M, const ASCCargoDrone* D, int32 K)
{
    return FRandomStream(int32(HashCombineFast(HashCombineFast(GetTypeHash(D ? D->DroneId : 0), GetTypeHash(M.MoveId)), GetTypeHash(K * 7919 + 13))));
}
// The hull's reach about a place's station (box frame), with a margin.
FBox HullBox(const FSCCargoEndGrip& G, const FVector& Station, double MarginCm) { return G.HullAt(Station).ExpandBy(MarginCm); }
} // namespace

TArray<int32> USCCargoSubsystem::TierOrder(const FSCCargoMove& M, const ASCCargoContainer* A, int32 Team) const
{
    // The planned place is the fallback the route's shape covers, and every move ends over a lattice where boxes stand
    // flush beside its slot: there a drone beyond a box's end stands where its neighbour is or goes, and a light drone on a
    // heavy box lags its station by up to a metre in the route's last turns (round 5's runs: the C2's hold, the fleet's
    // Hull A). So over with room first, then from the ends where that finds no way, then over as tight as it fits; on the
    // way each drone holds from where it picks (SCCargoGrip.cpp), the ends about 3 of 4 times, where they fit. A hold
    // (dug out and put back) and a team of more than two: over only.
    if (M.Plan.Kind == ESCCargoMoveKind::Hold || Team > 2) return {1, 2};
    return {1, 0, 2};
}

FSCCargoGripSpot USCCargoSubsystem::PlannedSpot(const FSCCargoMove& M, const ASCCargoContainer* A, int32 I, const FQuat& Rot) const
{
    FSCCargoGripSpot S;
    S.bPlanned = true;
    if (M.Grip == FSCCargoMove::EGrip::Ends && M.Ends.bValid)
    {
        const int32 End = M.EndOf.IsValidIndex(I) ? M.EndOf[I] : (I & 1);
        S.Kind = FSCCargoGripSpot::EKind::End;
        S.Face = End;
        S.Station = M.Ends.Station(End);
        S.Attach = M.Ends.Attach(End);
        return S;
    }
    // Over: its emitter the hang over the box's top as the box hangs (the column the route reserves), over its formation
    // offset; its hold on the box's surface nearest to it.
    S.Kind = FSCCargoGripSpot::EKind::Over;
    const FVector Half = A ? A->SizeCm() / 2 : FVector::ZeroVector;
    const double TopZ = FMath::Abs(Rot.GetAxisX().Z) * Half.X + FMath::Abs(Rot.GetAxisY().Z) * Half.Y + FMath::Abs(Rot.GetAxisZ().Z) * Half.Z;
    FVector World = Rot.RotateVector(M.Offsets.IsValidIndex(I) ? M.Offsets[I] : FVector::ZeroVector);
    World.Z = TopZ + M.HangCm;
    S.Station = Rot.UnrotateVector(World);
    FSCCargoEndGrip Box = M.Places;
    Box.BoxHalf = Half;
    S.Attach = Box.SurfaceNear(S.Station);
    return S;
}

FSCCargoGripSpot USCCargoSubsystem::GripNow(const FSCCargoMove& M, const ASCCargoContainer* A, int32 I, const FQuat& Rot, bool* bOutMoving) const
{
    if (bOutMoving) *bOutMoving = false;
    if (!M.Spots.IsValidIndex(I)) return PlannedSpot(M, A, I, Rot);
    if (M.SpotFlights.IsValidIndex(I) && M.SpotFlights[I].HasRoute() && M.SpotTargets.IsValidIndex(I))
    {
        // On its way round the box: its hold on the surface nearest to it.
        FSCCargoGripSpot S = M.SpotTargets[I];
        S.Station = M.SpotFlights[I].Position();
        FSCCargoEndGrip Box = M.Places;
        Box.BoxHalf = A ? A->SizeCm() / 2 : Box.BoxHalf;
        S.Attach = Box.SurfaceNear(S.Station);
        if (bOutMoving) *bOutMoving = true;
        return S;
    }
    // The planned place follows the box as it hangs now (its turn at a hold's entrance).
    return M.Spots[I].bPlanned ? PlannedSpot(M, A, I, Rot) : M.Spots[I];
}

void USCCargoSubsystem::ResetSpots(FSCCargoMove& M, const ASCCargoContainer* A, const TArray<ASCCargoDrone*>& Drones, const FQuat& Rot, double Now)
{
    const int32 N = FMath::Min(Drones.Num(), M.Offsets.Num());
    const FSCCargoGripPick::FCfg& Cfg = FSCCargoDroneArt::Get().Grip;
    M.Spots.SetNum(N);
    M.SpotTargets.SetNum(N);
    M.SpotFlights.Reset();
    M.SpotFlights.SetNum(N);
    M.NextPick.SetNum(N);
    M.NextCheck.SetNum(N);
    if (M.Picks.Num() != N) M.Picks.Init(0, N);
    M.GotGrip.Init(false, N);
    for (int32 I = 0; I < N; ++I)
    {
        M.Spots[I] = M.SpotTargets[I] = PlannedSpot(M, A, I, Rot);
        // Its first look at its own moment: a share of its own first interval (the team staggered).
        FRandomStream Rng = PickStream(M, Drones[I], -1 - M.Picks[I]);
        M.NextPick[I] = Now + FSCCargoGripPick::Interval(Cfg, Rng) * Rng.FRandRange(0.3f, 1.f);
        M.NextCheck[I] = Now;
    }
}

FString USCCargoSubsystem::OtherLoadNear(const FSCCargoMove& M, const ASCCargoContainer* A, const FBox& Region, double Seconds) const
{
    const FSCCargoGripPick::FCfg& Cfg = FSCCargoDroneArt::Get().Grip;
    for (const auto& Pair : State->Boxes)
    {
        const FSCCargoBox& B = Pair.Value;
        const ASCCargoContainer* Other = B.Actor.Get();
        if (!Other || Other == A || (!SCCargoIsFlying(B.State) && B.State != ESCCargoBoxState::Held)) continue;
        const FVector Half = Other->SizeCm() / 2;
        FBox Local = FBox::BuildAABB(FVector::ZeroVector, Half);
        double Speed = 0;
        if (const FSCCargoMove* O = MoveOf(Other))
        {
            FSCCargoEndGrip G = O->Places;
            G.BoxHalf = Half;
            Local += O->Places.bValid ? FSCCargoGripPick::Reach(G, O->HangCm, Cfg) : FBox::BuildAABB(FVector(0, 0, Half.Z + (O->HangCm + DroneHeightCm()) / 2),
                                                                                                    FVector(Half.X, Half.Y, (O->HangCm + DroneHeightCm()) / 2));
            Speed = O->Carry.Speed;
        }
        const FBox Theirs = Local.TransformBy(Other->GetActorTransform()).ExpandBy(Speed * Seconds);
        if (Theirs.Intersect(Region)) return FString::Printf(TEXT("another load near (box %d)"), B.Id);
    }
    return FString();
}

const FSCCargoMove* USCCargoSubsystem::MoveOfDrone(const ASCCargoDrone* D) const
{
    if (!D || D->MoveId == INDEX_NONE) return nullptr;
    for (const FSCCargoJob& Job : State->Jobs)
        for (const FSCCargoMove& O : Job.Moves)
            if (O.MoveId == D->MoveId) return &O;
    return nullptr;
}

bool USCCargoSubsystem::InZoneOf(const FSCCargoMove& M, const ASCCargoContainer* A, const ASCCargoDrone* D) const
{
    FTransform ToLattice;
    FBox Zone;
    if (!A || !D || !PreciseZone(M, A, ToLattice, Zone)) return false;
    // Grown by everything round the box its own drones can reach, and the distance drones keep from each other.
    double Grow = A->SizeCm().GetMax() / 2;
    if (M.Places.bValid)
    {
        FSCCargoEndGrip G = M.Places;
        G.BoxHalf = A->SizeCm() / 2;
        const FBox R = FSCCargoGripPick::Reach(G, M.HangCm, FSCCargoDroneArt::Get().Grip);
        Grow = R.Max.ComponentMax(-R.Min).GetMax();
    }
    const double Keep = FSCCargoDroneSteer::Keep(D->HullRadius(), D->HullRadius());
    if (Zone.ExpandBy(Grow + Keep).IsInsideOrOn(ToLattice.TransformPosition(D->GetActorLocation()))) return true;
    // ... or in the load's way into it from where it is, within the gate's reach (the C2's ramp): its box and drones
    // anywhere round it along that way, the keep distance more.
    if (M.Step != FSCCargoMove::EStep::Transit || M.Carry.Points.Num() < 2 || !M.Places.bValid) return false;
    TArray<FVector> Way;
    bool bReaches = false; // the zone within the gate's reach (else the load does not wait at the gate, nor is this its way in)
    for (double S = M.Carry.Dist; S <= FMath::Min(M.Carry.Length(), M.Carry.Dist + SCCargoFlightTuning::ZoneGateCm + 100.0); S += 100.0)
    {
        Way.Add(M.Carry.At(S));
        if (InPreciseZone(M, A, Way.Last())) { bReaches = true; break; }
    }
    if (!bReaches) return false;
    FSCCargoEndGrip G = M.Places;
    G.BoxHalf = A->SizeCm() / 2;
    FVector Half;
    double Above = 0;
    FSCCargoKeepOut::LegShape(FSCCargoGripPick::Reach(G, M.HangCm, FSCCargoDroneArt::Get().Grip), Half, Above);
    return FSCCargoKeepOut::RoutePasses(Way, M.FromRotation, Half, Above, D->GetActorLocation() + D->HullCentre, Keep);
}

int32 USCCargoSubsystem::ZoneOccupant(const FSCCargoMove& M, const ASCCargoContainer* A) const
{
    for (const TWeakObjectPtr<ASCCargoDrone>& W : State->Drones)
    {
        const ASCCargoDrone* D = W.Get();
        if (!D || D->State == ESCCargoDroneState::Docked || M.Drones.Contains(D->DroneId)) continue;
        // Holding a load in the air: the right of way between loads decides (their guards and yields).
        if (const FSCCargoMove* O = D->CarryingBoxId != INDEX_NONE ? MoveOfDrone(D) : nullptr;
            O && (O->Step == FSCCargoMove::EStep::Lifting || O->Step == FSCCargoMove::EStep::Transit || O->Step == FSCCargoMove::EStep::Lowering))
            continue;
        if (InZoneOf(M, A, D)) return D->DroneId;
    }
    return INDEX_NONE;
}

double USCCargoSubsystem::LoadEnvelopeCm(const FSCCargoMove& M) const
{
    // A drone's hold point turns with its attitude about its emitter (at most max_tilt_deg) and moves with its drift and
    // hunt, by load_transfer; the box's spring (critically damped) passes on no more than its anchor moves, and the
    // team's mean no more than the worst. The longest hold: a place's reach out from a face or over the top, at most the
    // beam's full-strength distance.
    const FSCCargoDroneArt& Art = FSCCargoDroneArt::Get();
    const double Gain = Art.Dynamics.LoadTransfer;
    double Lever = TractorBeam().FullM * 100.0;
    if (M.Places.bValid)
    {
        const double Out = M.Places.ClearCm * (1.0 + Art.Grip.ExtraReach), Rise = M.Places.Drone.AboveCm + Art.Grip.ExtraHeightCm;
        Lever = FMath::Min(Lever, FMath::Max(FMath::Sqrt(Out * Out + Rise * Rise), M.HangCm + Art.Grip.ExtraHeightCm));
    }
    const double Transfer = Gain * (Lever * FMath::Sin(FMath::DegreesToRadians(double(Art.Dynamics.MaxTiltDeg))) + Art.Dynamics.DriftCm + Art.Dynamics.HuntCm);
    return SwayEnvelopeCm() + Transfer;
}

bool USCCargoSubsystem::SpotFits(const FSCCargoMove& M, const ASCCargoContainer* A, const FSCCargoGripSpot& From, const FSCCargoGripSpot& Spot,
                                 const FSCCargoGripSpot& Back, const FQuat& Rot, FString* OutWhy) const
{
    if (!A || !M.Places.bValid || M.Carry.Points.Num() < 2) return false;
    const FSCCargoDroneDefinition& Def = State->Scenario.Drone;
    const FSCCargoGripPick::FCfg& Cfg = FSCCargoDroneArt::Get().Grip;
    FSCCargoEndGrip G = M.Places;
    G.BoxHalf = A->SizeCm() / 2;
    // Back to its planned place in time: within the lookahead, less one check.
    const double MoveSpeed = Def.MaxSpeedMS * 100.0 * Cfg.MoveSpeedScale;
    const TArray<FVector> There = G.WayRound(From.Station, Spot.Station), Home = G.WayRound(Spot.Station, Back.Station);
    if (FSCCargoFlight::Seconds(FSCCargoGripPick::WayLength(Home), MoveSpeed, Def.MaxAccelMS2 * 100.0) > Cfg.LookaheadS - GripCheckSeconds)
    {
        if (OutWhy) *OutWhy = TEXT("too far from its planned place to be back in time");
        return false;
    }
    // Its hull there and along both ways (box frame).
    TArray<FBox> Regions = {G.HullAt(Spot.Station)};
    for (const TArray<FVector>* Way : {&There, &Home})
        for (int32 K = 1; K < Way->Num(); ++K) Regions.Add(G.HullAt((*Way)[K - 1]) + G.HullAt((*Way)[K]));
    // The route ahead: as far as the load can fly within the lookahead (from its speed, at its acceleration, to the top
    // speed), the route's own corners on the way.
    const FSCCargoFlight& C = M.Carry;
    const double T = Cfg.LookaheadS;
    const double Ahead = FMath::Min(Def.MaxSpeedMS * 100.0, C.Speed + M.LoadAccel * T) * T;
    const double End = FMath::Min(C.Dist + Ahead, C.Length());
    TArray<FVector> Along = {C.At(C.Dist)};
    for (int32 K = 0; K < C.Points.Num(); ++K)
        if (C.S[K] > C.Dist + 1 && C.S[K] < End - 1) Along.Add(C.Points[K]);
    Along.Add(C.At(End));
    const double Margin = DroneClearanceCm / 2 + LoadEnvelopeCm(M);
    // Other loads in the air: a place round this box is no route either of them was planned with (their right of way
    // and guards know only each other's planned shapes). Kept clear of where each, with its drones anywhere round it
    // (FSCCargoGripPick::Reach), can be within the lookahead.
    FBox Ours(ForceInit);
    for (const FBox& R : Regions)
        for (const FVector& P : Along) Ours += R.TransformBy(FTransform(Rot, P));
    if (const FString Near = OtherLoadNear(M, A, Ours.ExpandBy(Margin), T); !Near.IsEmpty())
    {
        if (OutWhy) *OutWhy = Near;
        return false;
    }
    for (const FBox& R : Regions)
    {
        const FSCCargoRouteQuery Q = RouteQuery(R.GetExtent() + FVector(Margin), Rot.RotateVector(R.GetCenter()), A, -1e9, 1e9, false, Rot, false, nullptr, &M);
        for (int32 K = 1; K < Along.Num(); ++K)
            if (!Q.Free(Along[K - 1], Along[K]))
            {
                if (OutWhy) *OutWhy = Q.Describe(Along[K - 1], Along[K]);
                return false;
            }
    }
    return true;
}

void USCCargoSubsystem::GripTick(FSCCargoMove& M, ASCCargoContainer* A, const TArray<ASCCargoDrone*>& Drones, const FVector& Q, const FQuat& Rot, bool bSearch,
                                 double Now, double Dt)
{
    const int32 N = FMath::Min(Drones.Num(), M.Offsets.Num());
    if (!A || N == 0) return;
    if (M.Spots.Num() != N) ResetSpots(M, A, Drones, Rot, Now);
    const FSCCargoDroneDefinition& Def = State->Scenario.Drone;
    const FSCCargoGripPick::FCfg& Cfg = FSCCargoDroneArt::Get().Grip;
    const double MoveSpeed = Def.MaxSpeedMS * 100.0 * Cfg.MoveSpeedScale;
    FSCCargoEndGrip G = M.Places;
    G.BoxHalf = A->SizeCm() / 2;
    auto MoveTo = [&](int32 I, const FSCCargoGripSpot& To, const FVector& FromStation) {
        M.SpotTargets[I] = To;
        TArray<FVector> Way = G.WayRound(FromStation, To.Station);
        if (Way.Num() < 2)
        {
            M.Spots[I] = To;
            M.SpotFlights[I] = FSCCargoFlight();
            return;
        }
        M.SpotFlights[I].Start(Way, MoveSpeed, Def.MaxAccelMS2 * 100.0, Def.MaxJerkMS3 * 100.0);
    };
    // On along its way round the box (the way waits for a drone held up); there once its way is flown.
    bool bAnyMoving = false;
    for (int32 I = 0; I < N; ++I)
    {
        FSCCargoFlight& F = M.SpotFlights[I];
        if (!F.HasRoute()) continue;
        if (FVector::Dist(Drones[I]->BodyEmitter(), Q + Rot.RotateVector(F.Position())) <= Drones[I]->HullRadius()) F.Step(Dt);
        else F.Speed = F.Accel = 0; // it waits for its drone, at rest (its speed is fed forward to the drone: TugStep)
        if (F.Done())
        {
            M.Spots[I] = M.SpotTargets[I];
            F = FSCCargoFlight();
        }
        else bAnyMoving = true;
    }
    // Searching only while the box hangs upright on a route piece that goes on beyond the lookahead, out of the precise
    // zone: otherwise every drone goes back to its planned place (the route's shape covers it there).
    const FSCCargoFlight& C = M.Carry;
    // Its drones go back one at a time before the route piece ends or a precise zone begins: it looks that much further
    // ahead for each drone still away from its planned place (or on its way to another).
    int32 Away = 0;
    for (int32 I = 0; I < N; ++I)
        Away += M.SpotFlights[I].HasRoute() ? !M.SpotTargets[I].bPlanned : !GripNow(M, A, I, Rot).bPlanned;
    const double AheadS = FSCCargoGripPick::ReturnAheadS(Cfg, Away);
    const double Ahead = FMath::Min(Def.MaxSpeedMS * 100.0, C.Speed + M.LoadAccel * AheadS) * AheadS;
    FString Calm;
    // Calm at once (every drone goes back now): no route to search on, no places, the box on its side. Calm ahead (the
    // route's end, a precise zone): one at a time.
    bool bAtOnce = true;
    if (!bSearch) Calm = TEXT("not flying its route");
    else if (!G.bValid) Calm = TEXT("no places");
    else if (FMath::Abs(Rot.GetAxisZ().Z) < 0.99) Calm = TEXT("the box hangs on its side");
    else if (bAtOnce = false; C.Points.Num() < 2 || C.Dist + Ahead >= C.Length()) Calm = TEXT("its route piece ends ahead");
    else
        for (double S = C.Dist; S <= C.Dist + Ahead && Calm.IsEmpty(); S += 50.0)
            if (InPreciseZone(M, A, C.At(S))) Calm = TEXT("the precise zone ahead");
    // Teammates keep the distance drones keep from each other (FSCCargoDroneSteer::Keep, between centres) and the
    // emitter's reach about its centre: nearer, their steps push each other about (round 5: two drones of a pair a
    // metre apart swung each other round at 20 m/s).
    double Radius = 0;
    for (int32 J = 0; J < N; ++J) Radius = FMath::Max(Radius, Drones[J]->HullRadius());
    const double KeepCm = FSCCargoDroneSteer::Keep(Radius, Radius) + 2.0 * G.Drone.EmitterPlanCm;
    const double CapCm = FSCCargoGripPick::MoveCap(Cfg, A->SizeCm().GetMax());
    // Where its teammates hold, go and go back to (their planned places: they return there at any time).
    auto MatesOf = [&](int32 I) {
        TArray<FSCCargoGripSpot> Out;
        for (int32 J = 0; J < N; ++J)
            if (J != I)
            {
                Out.Add(GripNow(M, A, J, Rot));
                if (M.SpotFlights[J].HasRoute()) Out.Add(M.SpotTargets[J]);
                if (!Out.Last().bPlanned) Out.Add(PlannedSpot(M, A, J, Rot));
            }
        return Out;
    };
    // Clear of its teammates' hulls there and on its way there.
    auto ClearOfMates = [&](const FSCCargoGripSpot& From, const FSCCargoGripSpot& S, TConstArrayView<FSCCargoGripSpot> Mates) {
        const TArray<FVector> Way = G.WayRound(From.Station, S.Station);
        for (const FSCCargoGripSpot& T : Mates)
        {
            const FBox Theirs = HullBox(G, T.Station, DroneClearanceCm / 4);
            for (int32 K = 1; K < Way.Num(); ++K)
                if ((G.HullAt(Way[K - 1]) + G.HullAt(Way[K])).Intersect(Theirs)) return false;
            if (G.HullAt(S.Station).Intersect(Theirs)) return false;
        }
        return true;
    };
    for (int32 I = 0; I < N; ++I)
    {
        if (M.SpotFlights[I].HasRoute()) continue;
        ASCCargoDrone* D = Drones[I];
        const FSCCargoGripSpot Planned = PlannedSpot(M, A, I, Rot);
        const FSCCargoGripSpot Here = GripNow(M, A, I, Rot);
        if (!Calm.IsEmpty())
        {
            // One at a time when the end is still ahead: a teammate on its way round, it holds where it is.
            if (!Here.bPlanned && (bAtOnce || !bAnyMoving))
            {
                MoveTo(I, Planned, Here.Station);
                bAnyMoving = !bAtOnce;
                UE_LOG(LogSCCargo, Display, TEXT("CARGO_GRIP_PICK move=%d box=%d drone=%d back=planned why=\"%s\""), M.MoveId, M.Plan.BoxId, D->DroneId, *Calm);
            }
            continue;
        }
        // Its place no longer fits ahead: to the nearest viable place, however far (its planned one at the latest).
        if (Now >= M.NextCheck[I])
        {
            M.NextCheck[I] = Now + GripCheckSeconds;
            FString Why;
            if (!Here.bPlanned && !SpotFits(M, A, Here, Here, Planned, Rot, &Why))
            {
                FRandomStream Near = PickStream(M, D, 100000 + M.Picks[I]);
                FSCCargoGripSpot Best = Planned;
                double BestCm = FSCCargoGripPick::WayLength(G.WayRound(Here.Station, Planned.Station));
                const TArray<FSCCargoGripSpot> Mates = MatesOf(I);
                for (const FSCCargoGripSpot& S : FSCCargoGripPick::Candidates(G, M.HangCm, Cfg, Near, 3))
                    if (const double Cm = FSCCargoGripPick::WayLength(G.WayRound(Here.Station, S.Station)); Cm < BestCm && ClearOfMates(Here, S, Mates) &&
                                                                                                            SpotFits(M, A, Here, S, Planned, Rot))
                    {
                        Best = S;
                        BestCm = Cm;
                    }
                MoveTo(I, Best, Here.Station);
                bAnyMoving = true;
                UE_LOG(LogSCCargo, Display, TEXT("CARGO_GRIP_PICK move=%d box=%d drone=%d back=%s move_cm=%.0f why=\"%s\""), M.MoveId, M.Plan.BoxId, D->DroneId,
                       Best.bPlanned ? TEXT("planned") : KindName(Best.Kind), BestCm, *Why);
                continue;
            }
        }
        // Its time to look again (one drone of the team on its way at a time).
        if (Now < M.NextPick[I] || bAnyMoving) continue;
        const double T0 = FPlatformTime::Seconds();
        FRandomStream Rng = PickStream(M, D, M.Picks[I]++);
        M.NextPick[I] = Now + FSCCargoGripPick::Interval(Cfg, Rng);
        const TArray<FSCCargoGripSpot> Mates = MatesOf(I);
        TArray<FSCCargoGripSpot> Cands = FSCCargoGripPick::Candidates(G, M.HangCm, Cfg, Rng, 3);
        Cands.Add(Planned);
        TArray<FSCCargoGripSpot> Viable;
        TArray<double> MoveCm;
        int32 Tried = 0, Far = 0;
        FString LastWhy;
        for (const FSCCargoGripSpot& S : Cands)
        {
            if (FVector::Dist(S.Station, Here.Station) < 30.0) continue; // where it is
            ++Tried;
            // A grip adjustment, not a shift: no further round the box than the cap (along its way there); its first grip
            // from its planned place (the owner: "aside from initial") is its own pick of the whole box.
            const double Cm = FSCCargoGripPick::WayLength(G.WayRound(Here.Station, S.Station));
            if (Cm > CapCm && M.GotGrip[I]) { ++Far; continue; }
            if (!ClearOfMates(Here, S, Mates)) { LastWhy = TEXT("a teammate there or on the way"); continue; }
            if (S.bPlanned || SpotFits(M, A, Here, S, Planned, Rot, &LastWhy))
            {
                Viable.Add(S);
                MoveCm.Add(Cm);
            }
        }
        const int32 K = FSCCargoGripPick::Pick(Viable, Mates, KeepCm, Cfg, Rng);
        if (K != INDEX_NONE)
        {
            MoveTo(I, Viable[K], Here.Station);
            bAnyMoving = true;
        }
        const double CapNow = M.GotGrip[I] ? CapCm : -1.0;
        if (K != INDEX_NONE && !Viable[K].bPlanned) M.GotGrip[I] = true;
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_GRIP_PICK move=%d box=%d drone=%d pick=%d to=%s face=%d planned=%d move_cm=%.0f cap_cm=%.0f viable=%d of %d far=%d ms=%.1f%s%s"),
               M.MoveId, M.Plan.BoxId, D->DroneId, M.Picks[I], K != INDEX_NONE ? KindName(Viable[K].Kind) : TEXT("stay"), K != INDEX_NONE ? Viable[K].Face : -1,
               K != INDEX_NONE && Viable[K].bPlanned, K != INDEX_NONE ? MoveCm[K] : 0.0, CapNow, Viable.Num(), Tried, Far, (FPlatformTime::Seconds() - T0) * 1000.0,
               Viable.IsEmpty() ? TEXT(" last=") : TEXT(""), Viable.IsEmpty() ? *FString::Printf(TEXT("\"%s\""), *LastWhy) : TEXT(""));
    }
}
