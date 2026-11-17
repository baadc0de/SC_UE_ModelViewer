// USCCargoSubsystem: a team tugging its box for one tick (Cargo/SCCargoTug.h is the model). The route gives where the
// box should be and how it moves; the force that takes (its weight, the route's acceleration, the body's drag) is
// shared among the attach points. Every drone then decides for itself: how long its beam is (out to the tractor's
// full-strength distance where its shape swept along the beam finds room, shorter under a ceiling or a door, and by
// its look along the route it flies shorter before one: BeamRoom, FSCCargoTug::RoomAhead), the rest length of its
// spring, and its station; it flies there as a body, and its gusts move it about. Each beam's spring pulls its
// attach point to the anchor the drone holds; the box is a simulated body with its mass.
// Also here: the sweep and the overlap test every step of a drone is held to (DroneSweep, DroneOverlap).
// Log: CARGO_TUG (once a second per move: the beams' lengths, rest lengths and forces).
#include "Cargo/SCCargoSubsystem.h"
#include "SCCargoState.h"
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoDrone.h"
#include "Cargo/SCCargoTug.h"
#include "Cargo/SCCargoPlanner.h"
#include "Cargo/SCCargoFlightPath.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Engine/OverlapResult.h"

FSCCargoBeam USCCargoSubsystem::TractorBeam() const
{
    const FSCCargoDroneDefinition& Def = State->Scenario.Drone;
    FSCCargoBeam Beam;
    Beam.MaxForceN = Def.MaxForceN;
    Beam.MinM = Def.MinDistanceM;
    Beam.FullM = Def.FullStrengthDistanceM;
    Beam.MaxM = Def.MaxDistanceM;
    return Beam;
}

double USCCargoSubsystem::MinHangCm(double LoadKg, int32 Team) const
{
    // The shortest beam that holds its share of the weight: the record's minimum rest length, or the drones' hull hang
    // when that is longer (no drone hangs closer to its box than its own reach below its emitter), and the spring's
    // stretch.
    const FSCCargoBeam Beam = TractorBeam();
    double Hang = 0;
    for (const TWeakObjectPtr<ASCCargoDrone>& D : State->Drones)
        if (D.IsValid()) Hang = FMath::Max(Hang, D->HullHangCm());
    return FSCCargoTug::ShortestCm(Beam.MinM * 100.0, Hang, Beam.StretchM(FMath::Min(LoadKg * Gravity() / FMath::Max(Team, 1), Beam.MaxForceN)) * 100.0);
}

double USCCargoSubsystem::RouteAccel(int32 Team, double LoadKg) const
{
    const FSCCargoDroneDefinition& Def = State->Scenario.Drone;
    const double Up = SCCargoLoadedAccel(Team, Def.MaxForceN, LoadKg, Def.DroneMassKg, Gravity(), Def.MaxAccelMS2);
    // A team that has no more than the minimum keeps it all (it was sized to have that much).
    return FMath::Max(Up - Def.MinLoadAccelMS2, FMath::Min<double>(Up, Def.MinLoadAccelMS2)) / UE_DOUBLE_SQRT_2;
}

double USCCargoSubsystem::MinHangCm(const ASCCargoContainer* A) const
{
    const FSCCargoDroneDefinition& Def = State->Scenario.Drone;
    for (const auto& Pair : State->Boxes)
        if (Pair.Value.Actor.Get() == A)
        {
            const int32 ByVolume = Def.MaxVolume > 0 ? FMath::CeilToInt(Pair.Value.VolumeSCU() / Def.MaxVolume - 1e-9) : 1;
            return MinHangCm(Pair.Value.Occupancy.Mass, FMath::Max(FSCCargoPlanner::DronesFor(Pair.Value.Occupancy.Mass, Gravity(), Def.MaxForceN), ByVolume));
        }
    return Def.MinDistanceM * 100.0;
}

namespace
{
constexpr double TuckSeconds = 1.0; // a drone held up keeps to its shortest beam this long after
constexpr double TouchCm = 0.5;     // a sweep that meets something within this of its start rests against it
FCollisionQueryParams DroneParams(const UWorld* World, const FSCCargoState& State, const ASCCargoDrone* Drone)
{
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCCargoDroneStep), /*bTraceComplex=*/true, Drone);
    for (const TWeakObjectPtr<ASCCargoDrone>& D : State.Drones) Params.AddIgnoredActor(D.Get());
    for (const FSCCargoDock& D : State.Docks) Params.AddIgnoredActor(D.Actor.Get()); // a drone rests in its dock's bay
    // ... and in its dock's corridor what its hull meets standing in that bay is the bay's own.
    if (Drone && Drone->InDockWay())
        for (const TWeakObjectPtr<const UPrimitiveComponent>& Own : Drone->BayOwn)
            if (Own.IsValid()) Params.AddIgnoredComponent(Own.Get());
    if (const APlayerController* PC = World->GetFirstPlayerController(); PC && PC->GetPawn()) Params.AddIgnoredActor(PC->GetPawn());
    return Params;
}
} // namespace

void USCCargoSubsystem::FindBayOwn(ASCCargoDrone* Drone) const
{
    const TArray<FVector> Way = DockWay(Drone);
    if (Way.IsEmpty()) return;
    const bool bFirst = Drone->BayOwn.IsEmpty();
    Drone->BayOwn.Reset();
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCCargoDroneBay), /*bTraceComplex=*/true, Drone);
    for (const TWeakObjectPtr<ASCCargoDrone>& D : State->Drones) Params.AddIgnoredActor(D.Get());
    for (const FSCCargoDock& D : State->Docks) Params.AddIgnoredActor(D.Actor.Get());
    for (const auto& Pair : State->Boxes) Params.AddIgnoredActor(Pair.Value.Actor.Get()); // a box is never the bay's
    const TSharedRef<FSCCargoHullRule> Hulls = HullRule(Params, Drone->HullHalf, FQuat::Identity);
    TArray<FOverlapResult> Found;
    GetWorld()->OverlapMultiByObjectType(Found, Way[0] + Drone->HullCentre, FQuat::Identity, SCCargoObstacles(), FCollisionShape::MakeBox(Drone->HullHalf), Hulls->At(Way[0] + Drone->HullCentre));
    FString Names;
    for (const FOverlapResult& O : Found)
        if (const UPrimitiveComponent* C = O.GetComponent(); C && !Drone->BayOwn.Contains(C))
        {
            Drone->BayOwn.Add(C);
            Names += FString::Printf(TEXT(" %s/%s"), O.GetActor() ? *O.GetActor()->GetName() : TEXT("?"), *C->GetName());
        }
    if (bFirst && !Names.IsEmpty())
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_DRONE_BAY drone=%d dock=%d bay=%d at=%s hull_meets=[%s ] (the bay's own while the drone is in its dock's corridor)"), Drone->DroneId,
               Drone->DockItem, Drone->Bay, *Way[0].ToCompactString(), *Names);
}

bool USCCargoSubsystem::CanLeaveBay(ASCCargoDrone* D) const
{
    const double Now = GetWorld()->GetTimeSeconds();
    if (Now - D->LeaveChecked < 1.0) return D->bCanLeave;
    D->LeaveChecked = Now;
    const bool bBefore = D->bCanLeave;
    D->bCanLeave = true;
    FString What;
    if (const TArray<FVector> Way = DockWay(D); Way.Num() >= 2)
    {
        FindBayOwn(D);
        const FVector Clear = Departure(D, Way[1]);
        double Fraction = 1;
        FVector Normal;
        D->bCanLeave = !DroneOverlap(D, Way[1], What) && !DroneSweep(D, Way[1], Clear, Fraction, Normal, &What);
    }
    if (D->bCanLeave != bBefore)
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_DRONE_BAY_BLOCKED drone=%d dock=%d bay=%d blocked=%d by=%s"), D->DroneId, D->DockItem, D->Bay, !D->bCanLeave, D->bCanLeave ? TEXT("-") : *What);
    return D->bCanLeave;
}

bool USCCargoSubsystem::DroneSweep(const ASCCargoDrone* Drone, const FVector& A, const FVector& B, double& OutFraction, FVector& OutNormal, FString* OutWhat) const
{
    if (!Drone || A.Equals(B, 1e-3)) return false;
    FCollisionQueryParams Params = DroneParams(GetWorld(), *State, Drone);
    // Something that came onto the drone (a box swinging, a door) lets it step only out of it; that step is still
    // swept against everything else (a few such things at most).
    for (int32 Try = 0; Try < 4; ++Try)
    {
        const TSharedRef<FSCCargoHullRule> Hulls = HullRule(Params, Drone->HullHalf, FQuat::Identity);
        FHitResult Hit;
        if (!GetWorld()->SweepSingleByObjectType(Hit, A + Drone->HullCentre, B + Drone->HullCentre, FQuat::Identity, SCCargoObstacles(), FCollisionShape::MakeBox(Drone->HullHalf),
                                                 Hulls->Along(A + Drone->HullCentre, B + Drone->HullCentre)))
            return false;
        if (OutWhat) *OutWhat = FString::Printf(TEXT("%s/%s%s"), Hit.GetActor() ? *Hit.GetActor()->GetName() : TEXT("?"), Hit.GetComponent() ? *Hit.GetComponent()->GetName() : TEXT("?"), Hit.bStartPenetrating ? TEXT("(start inside)") : TEXT(""));
        if (!Hit.bStartPenetrating)
        {
            // Resting against it (it slid there, a hair off) and stepping away: the sweep names the surface it touches
            // at time 0 whichever way the step goes, and a drone that came to rest under a ceiling stayed there.
            // Swept again from a touch's width off that surface: what is met then is in the way. (A step shorter
            // than that, a body starting from rest: free when the hull, a hair smaller, overlaps nothing where it ends.)
            if (Hit.Distance <= TouchCm && FVector::DotProduct(B - A, Hit.ImpactNormal) > 0)
            {
                if (FVector::DistSquared(A, B) <= FMath::Square(2 * TouchCm))
                {
                    if (!GetWorld()->OverlapAnyTestByObjectType(B + Drone->HullCentre, FQuat::Identity, SCCargoObstacles(), FCollisionShape::MakeBox(Drone->HullHalf - FVector(TouchCm / 2)),
                                                                Hulls->At(B + Drone->HullCentre)))
                        return false;
                    OutFraction = 0;
                    OutNormal = Hit.ImpactNormal;
                    return true;
                }
                const FVector Off = A + Drone->HullCentre + Hit.ImpactNormal * TouchCm;
                FHitResult Again;
                if (!GetWorld()->SweepSingleByObjectType(Again, Off, B + Drone->HullCentre, FQuat::Identity, SCCargoObstacles(), FCollisionShape::MakeBox(Drone->HullHalf),
                                                         Hulls->Along(Off, B + Drone->HullCentre)))
                    return false;
                if (Again.bStartPenetrating || Again.Distance <= TouchCm)
                {
                    OutFraction = 0;
                    OutNormal = Again.bStartPenetrating ? Again.Normal : Again.ImpactNormal;
                    return true;
                }
                if (OutWhat) *OutWhat = FString::Printf(TEXT("%s/%s"), Again.GetActor() ? *Again.GetActor()->GetName() : TEXT("?"), Again.GetComponent() ? *Again.GetComponent()->GetName() : TEXT("?"));
                OutFraction = Again.Time;
                OutNormal = Again.ImpactNormal;
                return true;
            }
            OutFraction = Hit.Time;
            OutNormal = Hit.ImpactNormal;
            return true;
        }
        // Out of it: along the way out the engine names, or to where the hull is clear of it.
        if (!Hit.GetComponent() || (FVector::DotProduct(B - A, Hit.Normal) <= 0 &&
                                     Hit.GetComponent()->OverlapComponent(B + Drone->HullCentre, FQuat::Identity, FCollisionShape::MakeBox(Drone->HullHalf))))
        {
            OutFraction = 0;
            OutNormal = Hit.Normal;
            return true;
        }
        Params.AddIgnoredComponent(Hit.GetComponent());
    }
    OutFraction = 0;
    OutNormal = FVector::UpVector;
    return true;
}

double USCCargoSubsystem::BeamRoom(const FSCCargoMove& M, const ASCCargoContainer* A, const ASCCargoDrone* D, const FVector& Q, const FVector& Attach, const FVector& Dir,
                                   double ShortestCm, FVector* OutTucked) const
{
    // What a drone keeps clear of along its beam: the surroundings and the boxes at rest. Its own box, the loads in
    // the air and the drones are left out (the beam's minimum keeps it off its box; its steps keep it clear of the
    // rest as they come).
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCCargoTug), /*bTraceComplex=*/true, A);
    for (const TWeakObjectPtr<ASCCargoDrone>& Other : State->Drones) Params.AddIgnoredActor(Other.Get());
    if (const APlayerController* PC = GetWorld()->GetFirstPlayerController(); PC && PC->GetPawn()) Params.AddIgnoredActor(PC->GetPawn());
    for (const auto& Pair : State->Boxes)
        if (SCCargoIsFlying(Pair.Value.State)) Params.AddIgnoredActor(Pair.Value.Actor.Get());
    // The drone's shape (its hull's bounds, with half the clearance a load's route keeps), where it is when its
    // emitter is on the beam: swept from the shortest beam out to the full-strength distance.
    const double Full = State->Scenario.Drone.FullStrengthDistanceM * 100.0;
    const FVector Half = D->HullHalf + FVector(SCCargoFlightTuning::DroneClearanceCm / 2);
    const FVector ShapeAt = D->GetActorLocation() - D->Emitter() + D->HullCentre; // the shape's centre from the emitter
    const FVector From = Q + Attach + Dir * ShortestCm + ShapeAt, To = Q + Attach + Dir * Full + ShapeAt;
    if (OutTucked) *OutTucked = From;
    if (FVector::DistSquared(From, To) <= 1.0) return FMath::Max(Full, ShortestCm);
    FHitResult Hit;
    const TSharedRef<FSCCargoHullRule> Hulls = HullRule(Params, Half, FQuat::Identity);
    if (!GetWorld()->SweepSingleByObjectType(Hit, From, To, FQuat::Identity, SCCargoObstacles(), FCollisionShape::MakeBox(Half), Hulls->Along(From, To))) return Full;
    return ShortestCm + (Hit.bStartPenetrating ? 0.0 : Hit.Distance);
}

bool USCCargoSubsystem::DroneOverlap(const ASCCargoDrone* Drone, const FVector& P, FString& OutWhat) const
{
    const FCollisionQueryParams Params = DroneParams(GetWorld(), *State, Drone);
    const TSharedRef<FSCCargoHullRule> Hulls = HullRule(Params, Drone->HullHalf, FQuat::Identity);
    TArray<FOverlapResult> Found;
    // A hair inside its bounds: resting against a surface it slid along is not an overlap.
    if (!GetWorld()->OverlapMultiByObjectType(Found, P + Drone->HullCentre, FQuat::Identity, SCCargoObstacles(), FCollisionShape::MakeBox(Drone->HullHalf - FVector(1.0)),
                                              Hulls->At(P + Drone->HullCentre)))
        return false;
    const UPrimitiveComponent* C = Found[0].GetComponent();
    OutWhat = FString::Printf(TEXT("%s/%s"), Found[0].GetActor() ? *Found[0].GetActor()->GetName() : TEXT("?"), C ? *C->GetName() : TEXT("?"));
    return true;
}

void USCCargoSubsystem::TugStep(FSCCargoMove& M, ASCCargoContainer* A, const TArray<ASCCargoDrone*>& Drones, const FVector& Q, const FQuat& Rotation, const FVector& RouteV,
                                const FVector& RouteA, double Now, double Dt)
{
    UBoxComponent* Body = A ? A->Body.Get() : nullptr;
    const int32 N = FMath::Min(Drones.Num(), M.Offsets.Num());
    if (!Body || N == 0 || Dt <= 0) return;
    const FSCCargoDroneDefinition& Def = State->Scenario.Drone;
    const FSCCargoBeam Beam = TractorBeam();
    const double G = Gravity(), Mass = FMath::Max<double>(Body->GetMass(), 1e-3);
    const FVector Up = FVector::UpVector, Size = A->SizeCm();
    const FTransform Box = A->GetActorTransform();
    // The force the route asks of the team (N): the weight, the route's acceleration and the body's own drag.
    const FVector Net = (RouteA / 100.0 + Up * G + Body->GetPhysicsLinearVelocity() / 100.0 * Body->GetLinearDamping()) * Mass;
    // The attach points lie along the team's spread (the box's long side); the split leaves no moment about the centre.
    const FVector Spread = N > 1 ? (M.Offsets[N - 1] - M.Offsets[0]).GetSafeNormal() : FVector::ForwardVector;
    TArray<double> Along;
    for (int32 I = 0; I < N; ++I) Along.Add(FVector::DotProduct(M.Offsets[I], Spread) / 100.0);
    const TArray<double> Shares = FSCCargoTug::Shares(Along);

    M.bNoStation = M.bDroneBehind = false;
    // The route being flown (not a hold's hover, not a load standing where it stalled): each drone looks along it.
    const bool bRoute = M.Step == FSCCargoMove::EStep::Transit && !M.bStalled && M.Carry.Points.Num() >= 2;
    const bool bTurning = bRoute && !M.SecondPath.IsEmpty() && M.Carry.Done(); // at a hold's entrance, onto its lattice
    const double StepCm = SCCargoFlightTuning::DroneClearanceCm; // a stretch: the sweep's margin (half of it) reaches half a stretch either way
    const double Rate = Def.MaxAccelMS2 > 0 ? Def.MaxJerkMS3 / Def.MaxAccelMS2 : 1.0, SlackM = SCCargoFlightTuning::DroneClearanceCm / 400.0; // half the margin
    // The rooms kept per stretch belong to this route in this world.
    if (bRoute)
    {
        const uint32 Stamp = HashCombineFast(HashCombineFast(M.WorldStamp, GetTypeHash(M.Carry.Points.Num())), HashCombineFast(GetTypeHash(M.Carry.Points[0]), GetTypeHash(M.Carry.Points.Last())));
        if (Stamp != M.RoomStamp)
        {
            M.RoomStamp = Stamp;
            M.Rooms.Empty();
        }
    }
    TArray<FSCCargoTug::FStation> Stations;
    TArray<FVector> Attach, Share;
    for (int32 I = 0; I < N; ++I)
    {
        ASCCargoDrone* D = Drones[I];
        if (!D->IsHolding()) D->Grab(Body);
        const FVector Local = FVector(M.Offsets[I].X, M.Offsets[I].Y, Size.Z / 2);
        const FVector WantAttach = Q + Rotation.RotateVector(Local);
        Attach.Add(Box.TransformPositionNoScale(Local));
        Share.Add(Net * Shares[I]);
        // Along its share of the force; with next to none (zero g, the load at rest) on the side it holds (WP-K3).
        const FVector Dir = FSCCargoTug::PullDirection(Share[I], D->GetActorLocation() - Attach[I]);
        // Room along the beam: the drone's shape swept out from where it would be on the shortest beam it could fly
        // (BeamRoom), here and, by the drone's own look along the route it flies, ahead: it comes in before a lintel
        // as far as its body needs to (FSCCargoTug::RoomAhead), and goes out again behind it.
        // Never shorter than its hull lets it hang over its box (its reach below the emitter and the gap it keeps).
        const double Shortest = FSCCargoTug::ShortestCm(Beam.MinM * 100.0, D->HullHangCm(), Beam.StretchM(FMath::Min(Share[I].Size(), Beam.MaxForceN)) * 100.0);
        FVector Tucked;
        double Room = BeamRoom(M, A, D, Q, Rotation.RotateVector(Local), Dir, Shortest, &Tucked);
        // Not even the bare hull fits on the shortest beam here: the team's route waits (TickMove).
        if (FString What; Room <= Shortest + 1.0 && DroneOverlap(D, Tucked - D->HullCentre, What)) M.bNoStation = true;
        if (bTurning || (bRoute && !Rotation.Equals(M.FromRotation, 1e-4)))
        {
            // Turning at the hold's entrance: the room through the rest of the turn.
            for (int32 K = 1; K <= 4; ++K) Room = FMath::Min(Room, BeamRoom(M, A, D, Q, FQuat::Slerp(Rotation, M.SecondRotation, K / 4.0).RotateVector(Local), Dir, Shortest));
        }
        else if (bRoute)
        {
            const double Back = FMath::CeilToDouble(D->HullRadius() / StepCm) * StepCm;
            const double Look = FSCCargoTug::LookAheadM((Beam.FullM * 100.0 - Shortest) / 100.0, RouteV.Size() / 100.0, M.LoadAccel / 100.0, Def.MaxSpeedMS, Rate, SlackM) * 100.0;
            const int32 First = FMath::FloorToInt32((M.Carry.Dist - Back) / StepCm), Last = FMath::CeilToInt32((M.Carry.Dist + Look + Back) / StepCm);
            const int32 End = FMath::CeilToInt32(M.Carry.Length() / StepCm);
            TArray<double, TInlineAllocator<128>> Ahead;
            for (int32 K = First; K <= Last; ++K)
            {
                // Per stretch of the route, swept once. At its end, where the load turns onto a hold's lattice: the
                // room through that turn.
                const int32 At = FMath::Clamp(K, 0, End);
                const int64 Key = (int64(I) << 32) | uint32(At);
                const double* Known = M.Rooms.Find(Key);
                double R = Known ? *Known : 0;
                if (!Known)
                {
                    const FVector P = M.Carry.At(FMath::Min(At * StepCm, M.Carry.Length()));
                    R = BeamRoom(M, A, D, P, Rotation.RotateVector(Local), Dir, Shortest);
                    if (At == End && !M.SecondPath.IsEmpty())
                        for (int32 T = 1; T <= 4; ++T) R = FMath::Min(R, BeamRoom(M, A, D, P, FQuat::Slerp(Rotation, M.SecondRotation, T / 4.0).RotateVector(Local), Dir, Shortest));
                    M.Rooms.Add(Key, R);
                }
                Ahead.Add(R / 100.0);
            }
            Room = FMath::Min(Room, FSCCargoTug::RoomAhead(Ahead, StepCm / 100.0, RouteV.Size() / 100.0, M.LoadAccel / 100.0, Def.MaxSpeedMS, Rate, SlackM, Beam.FullM, Back / 100.0) * 100.0);
        }
        Room = FMath::Max(Room, Shortest);
        // Held up (its step met something): it tucks in to its shortest beam, where the load's route has room for
        // it, and stays so for a moment after.
        if (D->bBlocked) D->TuckUntil = Now + TuckSeconds;
        if (Now < D->TuckUntil) Room = Shortest;
        Stations.Add(FSCCargoTug::Station(Beam, Share[I], Room / 100.0, WantAttach / 100.0, RouteV / 100.0));
        // Off its own box: a box carried off an upright plate hangs on its side, and its attach point is on a face
        // that stands upright; the drone hovers beside that face, not in the box.
        const FVector ShapeAt = (D->GetActorLocation() - D->Emitter() + D->HullCentre) / 100.0, ShapeHalf = (D->HullHalf + FVector(SCCargoFlightTuning::DroneClearanceCm / 2)) / 100.0;
        Stations.Last().KeepOffFace(Rotation.GetAxisZ(), WantAttach / 100.0, ShapeAt, ShapeHalf);
        // ... and off the box where it really is: it sways and lags its route, up to a drone tucked in on a short beam
        // (at worst as far as that beam is long).
        Stations.Last().KeepOffFace(Box.GetRotation().GetAxisZ(), Attach[I] / 100.0, ShapeAt, ShapeHalf);
    }
    // Teammates keep the formation spacing between their stations: two that would come closer move apart, level, by
    // half the shortfall each (along the spread when they would sit on top of one another).
    const double Spacing = Def.FormationSpacingM;
    for (int32 I = 0; I < N; ++I)
        for (int32 J = I + 1; J < N; ++J)
        {
            FVector Between = Stations[J].Hover - Stations[I].Hover;
            const double Short = Spacing - Between.Size();
            if (Short <= 0) continue;
            Between.Z = 0;
            const FVector Apart = Between.GetSafeNormal(1e-6, Rotation.RotateVector(Spread).GetSafeNormal2D(1e-6, FVector::ForwardVector));
            Stations[I].Hover -= Apart * Short / 2;
            Stations[J].Hover += Apart * Short / 2;
        }

    FString Sample;
    const bool bSample = Now - M.LastTugSample >= 1.0;
    for (int32 I = 0; I < N; ++I)
    {
        ASCCargoDrone* D = Drones[I];
        D->Flight.MassKg = FMath::Max<double>(Def.DroneMassKg, 1.0);
        D->Flight.MaxAccelMS2 = Def.MaxAccelMS2;
        D->Flight.MaxJerkMS3 = Def.MaxJerkMS3;
        D->Flight.MaxForceN = Def.MaxForceN;
        D->Flight.GravityMS2 = G;
        // Further from its station than its beam keeps full strength (held up behind something): the route waits for it.
        if (FVector::Dist(D->Emitter() / 100.0, Stations[I].Hover) > Beam.FullM) M.bDroneBehind = true;
        D->SetLoadVelocity(RouteV); // the team faces its load's travel (the route's, not the box's sway)
        D->Tug(Stations[I], -D->BeamForceN, float(Dt));
        // The beam as the drone holds it now: aimed and its rest length set from where its thrust holds it
        // (Flight.P), out from where its emitter really is (its gusts carry the anchor). The spring pulls the attach
        // point to that anchor; its damper works on the attach point's velocity against the route's.
        const FVector Emitter = D->Emitter() / 100.0;
        const FVector Anchor = Stations[I].Anchor(Beam, D->Flight.P, Emitter);
        const double Length = FVector::Dist(Emitter, Attach[I] / 100.0);
        D->BeamRestM = Stations[I].RestAt(Beam, D->Flight.P);
        D->BeamLengthM = Length;
        D->BeamForceN = Beam.ForceN(Anchor - Attach[I] / 100.0, (RouteV - Body->GetPhysicsLinearVelocityAtPoint(Attach[I])) / 100.0, Length, Shares[I] * Mass, Dt, Share[I]);
        if (Body->IsSimulatingPhysics()) Body->AddForceAtLocation(D->BeamForceN * 100.0, Attach[I]); // N to kg cm / s^2
        D->SetBeam(true, Attach[I], 30.f);
        if (bSample) Sample += FString::Printf(TEXT(" %d:len=%.2f rest=%.2f station=%.2f kN=%.1f"), D->DroneId, Length, D->BeamRestM, Stations[I].LengthM, D->BeamForceN.Size() / 1000.0);
    }
    // The beams hold the box's attitude as well (a tractor beam turns what it holds): a critically damped spring at
    // the beams' own frequency on the box's mass, as an angular acceleration, implicit over the step.
    const FQuat Error = (Rotation * Box.GetRotation().Inverse()).GetNormalized();
    FVector Axis;
    double Angle;
    Error.ToAxisAndAngle(Axis, Angle);
    if (Angle > UE_DOUBLE_PI) Angle -= UE_DOUBLE_TWO_PI;
    const double W = FMath::Sqrt(N * Beam.StiffnessNPerM() / Mass);
    const FVector Alpha = (Axis * Angle * W * W - Body->GetPhysicsAngularVelocityInRadians() * 2.0 * W) / (1.0 + 2.0 * W * Dt + W * W * Dt * Dt);
    if (Body->IsSimulatingPhysics()) Body->AddTorqueInRadians(Alpha, NAME_None, /*bAccelChange=*/true);
    if (!M.bNoStation) M.bLeaveNoStation = false; // every drone has its station again: the route waits for one as before
    if (bSample)
    {
        M.LastTugSample = Now;
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_TUG move=%d box=%d team=%d net_kN=%.1f beams=[%s ]"), M.MoveId, M.Plan.BoxId, N, Net.Size() / 1000.0, *Sample);
    }
}
