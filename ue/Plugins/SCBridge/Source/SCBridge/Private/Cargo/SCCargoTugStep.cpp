// USCCargoSubsystem: a team tugging its box for one tick (Cargo/SCCargoTug.h is the model). The route gives where the
// box should be and how it moves; the force that takes (its weight, the route's acceleration, the body's drag) is
// shared equally among the beams and applied to the box as a whole (round 5: a tractor beam holds to a point, in any
// direction; the box's rotation is the beams' angular hold). Each drone holds from its own place round the box (in front
// of an end, beside it, over it: SCCargoGrip.cpp), fixed in the box's frame on its route: its station, the spring's rest
// length for its share, and the anchor it holds the box to; it flies there as a body, its gusts move it about, and its
// wobble turns and moves its anchor (ASCCargoDrone::WobbledHold). The box is a simulated body with its mass. The physics
// works with each drone's body emitter (ASCCargoDrone::BodyEmitter), not the one its clips and attitude show.
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

double USCCargoSubsystem::DroneHeightCm() const
{
    double Above = -1;
    for (const TWeakObjectPtr<ASCCargoDrone>& D : State->Drones)
        if (D.IsValid()) Above = FMath::Max(Above, D->HullAboveCm());
    return Above >= 0 ? Above : SCCargoFlightTuning::DefaultDroneHeightCm;
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
    const TSharedRef<FSCCargoHullRule> Hulls = HullRule(Params, Drone->StepHalf(), FQuat::Identity);
    TArray<FOverlapResult> Found;
    GetWorld()->OverlapMultiByObjectType(Found, Way[0] + Drone->StepCentre(), FQuat::Identity, SCCargoObstacles(), FCollisionShape::MakeBox(Drone->StepHalf()), Hulls->At(Way[0] + Drone->StepCentre()));
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
        const TSharedRef<FSCCargoHullRule> Hulls = DroneHullRule(Params, Drone);
        FHitResult Hit;
        if (!GetWorld()->SweepSingleByObjectType(Hit, A + Drone->StepCentre(), B + Drone->StepCentre(), FQuat::Identity, SCCargoObstacles(), FCollisionShape::MakeBox(Drone->StepHalf()),
                                                 Hulls->Along(A + Drone->StepCentre(), B + Drone->StepCentre())))
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
                    if (!GetWorld()->OverlapAnyTestByObjectType(B + Drone->StepCentre(), FQuat::Identity, SCCargoObstacles(), FCollisionShape::MakeBox(Drone->StepHalf() - FVector(TouchCm / 2)),
                                                                Hulls->At(B + Drone->StepCentre())))
                        return false;
                    OutFraction = 0;
                    OutNormal = Hit.ImpactNormal;
                    return true;
                }
                const FVector Off = A + Drone->StepCentre() + Hit.ImpactNormal * TouchCm;
                FHitResult Again;
                if (!GetWorld()->SweepSingleByObjectType(Again, Off, B + Drone->StepCentre(), FQuat::Identity, SCCargoObstacles(), FCollisionShape::MakeBox(Drone->StepHalf()),
                                                         Hulls->Along(Off, B + Drone->StepCentre())))
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
                                     Hit.GetComponent()->OverlapComponent(B + Drone->StepCentre(), FQuat::Identity, FCollisionShape::MakeBox(Drone->StepHalf()))))
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

bool USCCargoSubsystem::DroneOverlap(const ASCCargoDrone* Drone, const FVector& P, FString& OutWhat) const
{
    const FCollisionQueryParams Params = DroneParams(GetWorld(), *State, Drone);
    const TSharedRef<FSCCargoHullRule> Hulls = DroneHullRule(Params, Drone);
    TArray<FOverlapResult> Found;
    // A hair inside its bounds: resting against a surface it slid along is not an overlap.
    if (!GetWorld()->OverlapMultiByObjectType(Found, P + Drone->StepCentre(), FQuat::Identity, SCCargoObstacles(), FCollisionShape::MakeBox(Drone->StepHalf() - FVector(1.0)),
                                              Hulls->At(P + Drone->StepCentre())))
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
    const FVector Up = FVector::UpVector;
    const FTransform Box = A->GetActorTransform();
    // The force the route asks of the team (N): the weight, the route's acceleration and the body's own drag. Each beam
    // gives an equal share of it, at the box as a whole (a tractor beam holds what it holds at a point, in any direction;
    // the box's rotation is the beams' angular hold below).
    const FVector Net = (RouteA / 100.0 + Up * G + Body->GetPhysicsLinearVelocity() / 100.0 * Body->GetLinearDamping()) * Mass;
    const FVector Share = Net / N;
    const double Stretch = Beam.StretchM(FMath::Min(Share.Size(), Beam.MaxForceN));

    M.bNoStation = M.bDroneBehind = false;
    TArray<FSCCargoTug::FStation> Stations;
    TArray<FVector> Attach;
    for (int32 I = 0; I < N; ++I)
    {
        ASCCargoDrone* D = Drones[I];
        if (!D->IsHolding()) D->Grab(Body);
        // Where it holds from (round 5: its own place round the box, fixed in the box's frame on the route, or on its way
        // to the next; SCCargoGrip.cpp). Its station follows the route, not the box's sway: no feedback from the box's
        // motion into where the drones fly.
        bool bMoving = false;
        const FSCCargoGripSpot Spot = GripNow(M, A, I, Rotation, &bMoving);
        const FVector WantAttach = Q + Rotation.RotateVector(Spot.Attach);
        Attach.Add(Box.TransformPositionNoScale(Spot.Attach));
        FSCCargoTug::FStation S;
        S.Hover = (Q + Rotation.RotateVector(Spot.Station)) / 100.0;
        S.HoverV = RouteV / 100.0;
        S.HoverA = RouteA / 100.0;
        // Turning (at the top of its lift, at a hold's entrance): the station swings round with the box.
        if (M.TugAt >= 0 && Now - M.TugAt < 0.5 && !Rotation.Equals(M.TugRotation, 1e-6))
            S.HoverV += (Rotation.RotateVector(Spot.Station) - M.TugRotation.RotateVector(Spot.Station)) / FMath::Max(Now - M.TugAt, Dt) / 100.0;
        if (bMoving && M.SpotFlights.IsValidIndex(I))
        {
            // On its way round the box: its way's own speed and acceleration on top of the route's.
            const FSCCargoFlight& F = M.SpotFlights[I];
            S.HoverV += Rotation.RotateVector(F.Tangent() * F.Speed) / 100.0;
            S.HoverA += Rotation.RotateVector(F.Tangent() * F.Accel) / 100.0;
        }
        S.Dir = (S.Hover - WantAttach / 100.0).GetSafeNormal(1e-6, Up);
        S.LengthM = FVector::Dist(S.Hover, WantAttach / 100.0);
        S.RestM = Beam.ClampRestM(S.LengthM - Stretch);
        // Hold to a point: the anchor where the attach point is to be, out by the spring's stretch for its share.
        S.AnchorWant = WantAttach / 100.0 + Share.GetSafeNormal(1e-6, S.Dir) * Stretch;
        // Off the face it holds where the box really is (it sways and lags its route): beyond it by the hull's reach. On
        // its way round the box, the face it is beyond now (over the top, then out past an end or a side).
        FSCCargoEndGrip Shape = M.Places;
        Shape.BoxHalf = A->SizeCm() / 2;
        const FVector Face = bMoving ? Shape.FaceNear(Spot.Station) : Spot.FaceNormal(M.Places.Axis);
        const FVector ShapeAt = (D->GetActorLocation() - D->BodyEmitter() + D->HullCentre) / 100.0;
        const FVector FaceAt = Box.TransformPositionNoScale(bMoving ? Shape.SurfaceNear(Spot.Station) : Spot.Attach);
        S.KeepOffFace(Box.GetRotation().RotateVector(Face), FaceAt / 100.0, ShapeAt, (D->HullHalf + FVector(ASCCargoDrone::OwnBoxGapCm)) / 100.0);
        Stations.Add(S);
        D->HeldFace = Face; // its steps keep off that face too
        // It faces the box it holds from in front or beside it (it pushes, pulls in reverse, strafes), and on its way
        // round it; over it, the load's travel. From its station, not from where it is: its emitter is off its centre,
        // so a heading taken from where it is would move it, and that the heading again (it circled its station).
        if (bMoving || Spot.Kind != FSCCargoGripSpot::EKind::Over) D->SetHold(Rotation.RotateVector(Spot.Attach - Spot.Station));
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
        if (FVector::Dist(D->BodyEmitter() / 100.0, Stations[I].Hover) > Beam.FullM) M.bDroneBehind = true;
        if (const double Off = FVector::Dist(D->BodyEmitter() / 100.0, Stations[I].Hover); Off > 0.4 && bSample)
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_TUG_OFF move=%d drone=%d off_m=%.2f at=%s station=%s (box frame, route) route_a=%s"), M.MoveId, D->DroneId, Off,
                   *Rotation.UnrotateVector(D->BodyEmitter() - Q).ToCompactString(), *Rotation.UnrotateVector(Stations[I].Hover * 100.0 - Q).ToCompactString(), *(RouteA / 100.0).ToCompactString());
        D->SetLoadVelocity(RouteV); // the team faces its load's travel (the route's, not the box's sway), unless it holds in front
        D->Tug(Stations[I], -D->BeamForceN, float(Dt));
        // The beam as the drone holds it now: aimed and its rest length set from where its thrust holds it
        // (Flight.P), out from where its emitter really is (its gusts carry the anchor). Its wobble turns and moves that
        // anchor (WobbledHold: a share of its attitude, faded as its gusts are). The spring pulls the box to the anchor;
        // its damper works on the box's velocity against the route's.
        const FVector Emitter = D->BodyEmitter() / 100.0;
        const FVector Anchor = D->WobbledHold(Stations[I].Anchor(Beam, D->Flight.P, Emitter) * 100.0) / 100.0;
        const double Length = FVector::Dist(Emitter, Attach[I] / 100.0);
        D->BeamRestM = Stations[I].RestAt(Beam, D->Flight.P);
        D->BeamLengthM = Length;
        D->BeamForceN = Beam.ForceN(Anchor - Attach[I] / 100.0, (RouteV - Body->GetPhysicsLinearVelocity()) / 100.0, Length, Mass / N, Dt, Share);
        if (Body->IsSimulatingPhysics()) Body->AddForce(D->BeamForceN * 100.0); // N to kg cm / s^2, at the box as a whole
        D->SetBeam(true, Attach[I], 30.f);
        if (bSample) Sample += FString::Printf(TEXT(" %d:len=%.2f rest=%.2f station=%.2f kN=%.1f"), D->DroneId, Length, D->BeamRestM, Stations[I].LengthM, D->BeamForceN.Size() / 1000.0);
    }
    M.TugRotation = Rotation;
    M.TugAt = Now;
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
