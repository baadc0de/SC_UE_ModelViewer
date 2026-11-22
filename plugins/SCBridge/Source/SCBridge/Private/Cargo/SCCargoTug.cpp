// The tractor-beam spring, the team's force split, a drone's own station and the drone as a body, holding a box from its
// ends, and where a drone holds from and re-picks (Cargo/SCCargoTug.h). Pure arithmetic in SI units (grips in cm).
#include "Cargo/SCCargoTug.h"

double FSCCargoBeam::CapN(double LengthM) const
{
    if (LengthM <= FullM || MaxM <= FullM) return LengthM <= FMath::Max(FullM, MaxM) ? MaxForceN : 0.0;
    return MaxForceN * FMath::Clamp((MaxM - LengthM) / (MaxM - FullM), 0.0, 1.0);
}

double FSCCargoBeam::StiffnessNPerM() const { return MinM > 0 ? MaxForceN / MinM : 0.0; }

FVector FSCCargoBeam::ForceN(const FVector& ToAnchor, const FVector& Rate, double LengthM, double MassKg, double DtS, const FVector& HoldN) const
{
    // The spring and its damper around the force the drone set its beam for, taken implicitly over the step:
    // (k dx + (c + k dt) v) / (1 + c dt / m + k dt^2 / m). A stiff beam on a light box cannot overshoot at any step, and
    // with the attach point where the beam was set for the force is HoldN exactly, at any frame rate.
    const double K = StiffnessNPerM(), M = FMath::Max(MassKg, 1e-6), Dt = FMath::Max(DtS, 0.0);
    const double C = 2.0 * FMath::Sqrt(K * M);
    const FVector Off = ToAnchor * K - HoldN;
    return (HoldN + (Off + Rate * (C + K * Dt)) / (1.0 + C * Dt / M + K * Dt * Dt / M)).GetClampedToMaxSize(CapN(LengthM));
}

TArray<double> FSCCargoTug::Shares(const TArray<double>& Along)
{
    TArray<double> Out;
    const int32 N = Along.Num();
    if (N == 0) return Out;
    double Mean = 0, Spread = 0;
    for (const double S : Along) Mean += S / N;
    for (const double S : Along) Spread += FMath::Square(S - Mean);
    for (const double S : Along) Out.Add(1.0 / N - (Spread > 1e-9 ? Mean * (S - Mean) / Spread : 0.0));
    return Out;
}

FSCCargoTug::FStation FSCCargoTug::Station(const FSCCargoBeam& Beam, const FVector& ShareN, double RoomM, const FVector& WantAttach, const FVector& WantV)
{
    FStation Out;
    const double Force = FMath::Min(ShareN.Size(), Beam.MaxForceN);
    if (ShareN.Size() > 1e-6) Out.Dir = ShareN / ShareN.Size();
    const double Stretch = Beam.StretchM(Force);
    Out.LengthM = FMath::Max(Beam.MinM + Stretch, FMath::Min(Beam.FullM, RoomM));
    Out.RestM = Beam.ClampRestM(Out.LengthM - Stretch);
    Out.Hover = WantAttach + Out.Dir * Out.LengthM;
    Out.HoverV = WantV;
    Out.AnchorWant = WantAttach + Out.Dir * Stretch;
    return Out;
}

double FSCCargoTug::FStation::RestAt(const FSCCargoBeam& Beam, const FVector& Body) const { return Beam.ClampRestM(FVector::Dist(Body, AnchorWant)); }

FVector FSCCargoTug::FStation::Anchor(const FSCCargoBeam& Beam, const FVector& Body, const FVector& Emitter) const
{
    return Emitter + (AnchorWant - Body).GetSafeNormal(1e-9, -Dir) * RestAt(Beam, Body);
}

void FSCCargoTug::FStation::KeepOffFace(const FVector& FaceN, const FVector& Attach, const FVector& ShapeAt, const FVector& ShapeHalf)
{
    const double Reach = FMath::Abs(FaceN.X) * ShapeHalf.X + FMath::Abs(FaceN.Y) * ShapeHalf.Y + FMath::Abs(FaceN.Z) * ShapeHalf.Z;
    const double Off = FVector::DotProduct(Hover + ShapeAt - Attach, FaceN);
    if (Off < Reach) Hover += FaceN * (Reach - Off);
}

FVector FSCCargoDroneSteer::Separate(const FVector& From, const FVector& To, double Radius, TConstArrayView<FOther> Others, double DtS, double MaxStep)
{
    FVector End = To;
    // A few passes: a push away from one drone may bring another nearer.
    for (int32 Pass = 0; Pass < 4; ++Pass)
    {
        bool bMoved = false;
        for (const FOther& O : Others)
        {
            const double Keep = FSCCargoDroneSteer::Keep(Radius, O.Radius);
            const FVector Ahead = O.P + O.V * DtS;
            // The nearest point of the other's stretch (now .. after the step) to where this step would end.
            const FVector Near = FMath::ClosestPointOnSegment(End, O.P, Ahead);
            FVector Away = End - Near;
            const double Dist = Away.Size();
            if (Dist >= Keep) continue;
            // Closing on it: the way round is to the right of the way towards it (level), so two drones meeting
            // head-on each turn to their own right. Dead level and dead centre has no other way out.
            const FVector Towards = (Near - From).GetSafeNormal2D();
            const FVector Right = FVector(Towards.Y, -Towards.X, 0);
            if (Dist < 1e-6) Away = Right.IsNearlyZero() ? FVector::UpVector : Right;
            else Away /= Dist;
            const FVector Closing = (To - From) / FMath::Max(DtS, 1e-6) - O.V;
            if (FVector::DotProduct(Closing, Near - From) > 0 && !Right.IsNearlyZero()) Away = (Away + Right * 0.5).GetSafeNormal();
            End = Near + Away * Keep;
            bMoved = true;
        }
        if (!bMoved) break;
    }
    return From + (End - From).GetClampedToMaxSize(MaxStep);
}

FVector FSCCargoDroneSteer::Slide(const FVector& From, const FVector& To, FSweep Sweep, bool& bOutBlocked)
{
    bOutBlocked = false;
    double Fraction = 1;
    FVector Normal = FVector::ZeroVector;
    if (!Sweep(From, To, Fraction, Normal)) return To;
    bOutBlocked = true;
    // Short of the surface by a hair, so the next sweep does not start in it.
    const FVector Step = To - From;
    const double Back = Step.Size() > 1e-9 ? FMath::Min(Fraction, 0.1 / Step.Size()) : 0.0;
    const FVector Stop = From + Step * FMath::Clamp(Fraction - Back, 0.0, 1.0);
    const FVector Left = To - Stop;
    const FVector Along = Left - Normal * FVector::DotProduct(Left, Normal);
    if (Along.SizeSquared() < 1e-6) return Stop;
    double SlideFraction = 1;
    FVector SlideNormal = FVector::ZeroVector;
    if (!Sweep(Stop, Stop + Along, SlideFraction, SlideNormal)) return Stop + Along;
    const double SlideBack = FMath::Min(SlideFraction, 0.1 / Along.Size());
    return Stop + Along * FMath::Clamp(SlideFraction - SlideBack, 0.0, 1.0);
}

double FSCCargoDroneBody::AccelLeft(const FVector& Dir, const FVector& ReactionN) const
{
    // Thrust T = m a + m g up - reaction, |T| <= m (g + a_max) + F_max: the largest a along Dir.
    const double Limit = MassKg * (GravityMS2 + MaxAccelMS2) + MaxForceN;
    const FVector Base = FVector::UpVector * MassKg * GravityMS2 - ReactionN;
    const double B = FVector::DotProduct(Dir, Base), Under = B * B + Limit * Limit - Base.SizeSquared();
    if (Under <= 0) return 0;
    return FMath::Clamp((FMath::Sqrt(Under) - B) / FMath::Max(MassKg, 1e-6), 0.0, MaxAccelMS2);
}

void FSCCargoDroneBody::Step(const FVector& Hover, const FVector& HoverV, const FVector& ReactionN, double DtS, const FVector& HoverA)
{
    if (DtS <= 0 || MaxAccelMS2 <= 0) return;
    // Critically damped onto the moving hover point, at the rate the drone can swing its thrust round (jerk / accel).
    const double W = MaxJerkMS3 > 0 ? MaxJerkMS3 / MaxAccelMS2 : 1.0 / DtS;
    FVector Want = HoverA + (Hover - P) * W * W + (HoverV - V) * 2.0 * W;
    if (const double Size = Want.Size(); Size > 1e-9) Want *= FMath::Min(1.0, AccelLeft(Want / Size, ReactionN) / Size);
    const FVector Change = Want - A;
    A += MaxJerkMS3 > 0 ? Change.GetClampedToMaxSize(MaxJerkMS3 * DtS) : Change;
    V += A * DtS;
    P += V * DtS;
}

double FSCCargoTug::ComeInS(double ExcessM, double RateHz, double SlackM)
{
    if (ExcessM <= SlackM || RateHz <= 0) return 0;
    double X = 0;
    while (X < 40 && (1.0 + X) * FMath::Exp(-X) * ExcessM > SlackM) X += 0.05;
    return (X + 1.0) / RateHz;
}

double FSCCargoTug::RoomAhead(TConstArrayView<double> RoomM, double StepM, double SpeedMS, double AccelMS2, double MaxSpeedMS, double RateHz, double SlackM, double LongestM, double BehindM)
{
    const double V = FMath::Max(SpeedMS, 1e-3), Top = FMath::Max(MaxSpeedMS, V);
    double Out = TNumericLimits<double>::Max();
    for (int32 K = 0; K < RoomM.Num(); ++K)
    {
        if (RoomM[K] >= Out) continue;
        // The soonest the route gets there: speeding up at its acceleration to its top speed.
        const double D = FMath::Max(K * StepM - 2 * BehindM, 0.0);
        double T = D / V;
        if (AccelMS2 > 1e-6)
        {
            const double Ramp = (Top * Top - V * V) / (2 * AccelMS2);
            T = D <= Ramp ? (FMath::Sqrt(V * V + 2 * AccelMS2 * D) - V) / AccelMS2 : (Top - V) / AccelMS2 + (D - Ramp) / Top;
        }
        // Too near to leave for later: its room counts now.
        if (D <= 0 || T <= ComeInS(LongestM - RoomM[K], RateHz, SlackM)) Out = RoomM[K];
    }
    return Out;
}

double FSCCargoTug::LookAheadM(double RangeM, double SpeedMS, double AccelMS2, double MaxSpeedMS, double RateHz, double SlackM)
{
    // As far as the route flies in the time the longest way in takes, speeding up as it may.
    const double T = ComeInS(RangeM, RateHz, SlackM), Top = FMath::Max(MaxSpeedMS, SpeedMS);
    return FMath::Min(SpeedMS * T + 0.5 * AccelMS2 * T * T, Top * T);
}

namespace
{
// The segment P .. P + D against the box of half size H about Centre (slabs).
bool SegmentMeetsBox(const FVector& P, const FVector& D, const FVector& Centre, const FVector& H)
{
    double T0 = 0, T1 = 1;
    for (int32 I = 0; I < 3; ++I)
    {
        const double At = P[I] - Centre[I];
        if (FMath::Abs(D[I]) < 1e-9)
        {
            if (FMath::Abs(At) > H[I]) return false;
            continue;
        }
        double Ta = (-H[I] - At) / D[I], Tb = (H[I] - At) / D[I];
        if (Ta > Tb) Swap(Ta, Tb);
        T0 = FMath::Max(T0, Ta);
        T1 = FMath::Min(T1, Tb);
        if (T0 > T1) return false;
    }
    return true;
}
// The bounds of a level box of half size E in the frame of Rotation.
FVector BoundsIn(const FQuat& Rotation, const FVector& E)
{
    const FVector X = Rotation.GetAxisX(), Y = Rotation.GetAxisY(), Z = Rotation.GetAxisZ();
    return FVector(FMath::Abs(X.X) * E.X + FMath::Abs(X.Y) * E.Y + FMath::Abs(X.Z) * E.Z, FMath::Abs(Y.X) * E.X + FMath::Abs(Y.Y) * E.Y + FMath::Abs(Y.Z) * E.Z,
                   FMath::Abs(Z.X) * E.X + FMath::Abs(Z.Y) * E.Y + FMath::Abs(Z.Z) * E.Z);
}
} // namespace

bool FSCCargoKeepOut::Hits(const FVector& A, const FVector& B, const FQuat& Rotation, const FVector& Half) const
{
    for (const FBox& Kept : Boxes)
        if (SegmentMeetsBox(Rotation.UnrotateVector(A - Kept.GetCenter()), Rotation.UnrotateVector(B - A), FVector::ZeroVector, Half + BoundsIn(Rotation, Kept.GetExtent())))
            return true;
    return false;
}

TArray<FVector> FSCCargoDroneSteer::Beside(TConstArrayView<FVector> Route, const FVector& From, const FVector& To, double Apart, double RampCm)
{
    TArray<FVector> Out;
    const int32 N = Route.Num();
    if (N < 2) return Out;
    TArray<double> S;
    S.SetNumZeroed(N);
    for (int32 I = 1; I < N; ++I) S[I] = S[I - 1] + FVector::Dist(Route[I - 1], Route[I]);
    const double Length = S.Last();
    if (Length < 1.0) return Out;
    // The route's level normal as it goes: turning with the route, kept through a stretch straight up or down.
    TArray<FVector> Across;
    Across.SetNumZeroed(N);
    FVector Kept = FVector::ZeroVector;
    for (int32 I = 0; I < N; ++I)
    {
        const FVector Tangent = Route[FMath::Min(I + 1, N - 1)] - Route[FMath::Max(I - 1, 0)];
        FVector Side(Tangent.Y, -Tangent.X, 0);
        if (Side.SizeSquared() > 0.04 * Tangent.SizeSquared())
        {
            Side.Normalize();
            if (!Kept.IsZero() && FVector::DotProduct(Side, Kept) < 0) Side = -Side;
            Kept = Side;
        }
        Across[I] = Kept;
    }
    if (Kept.IsZero()) Kept = FVector::ForwardVector; // straight up all the way: any side
    for (int32 I = N - 1; I >= 0; --I)
    {
        if (Across[I].IsZero()) Across[I] = Kept; // before the first level stretch: as that one
        Kept = Across[I];
    }
    // The ends are the drone's own (its bay's way out, its place over the box); between them, Apart to the side its
    // place lies on.
    const FVector AtStart = From - Route[0], AtEnd = To - Route.Last();
    const double Sign = FVector::DotProduct(Across.Last(), AtEnd) >= 0 ? 1.0 : -1.0, Ramp = FMath::Max(RampCm, 1.0);
    Out.Reserve(N);
    for (int32 I = 0; I < N; ++I)
    {
        const double Bump = FMath::SmoothStep(0.0, 1.0, S[I] / Ramp) * FMath::SmoothStep(0.0, 1.0, (Length - S[I]) / Ramp);
        Out.Add(Route[I] + FMath::Lerp(AtStart, AtEnd, FMath::SmoothStep(0.0, 1.0, S[I] / Length)) + Across[I] * (Sign * Apart * Bump));
    }
    return Out;
}

bool FSCCargoKeepOut::RoutePasses(TConstArrayView<FVector> Route, const FQuat& Rotation, const FVector& Half, double Above, const FVector& Point, double PadCm, int32* OutLast)
{
    // The point as the load sees it while its centre flies each leg: against the box and the column over it.
    const FVector Centre(0, 0, Above / 2), H = Half + FVector(PadCm, PadCm, PadCm + Above / 2);
    bool bPasses = false;
    for (int32 I = 0; I < Route.Num(); ++I)
    {
        const FVector Next = Route[FMath::Min(I + 1, Route.Num() - 1)];
        if (!SegmentMeetsBox(Rotation.UnrotateVector(Point - Route[I]), Rotation.UnrotateVector(Route[I] - Next), Centre, H)) continue;
        if (!OutLast) return true;
        bPasses = true;
        *OutLast = I;
    }
    return bPasses;
}

void FSCCargoKeepOut::LegShape(const FBox& Reach, FVector& OutHalf, double& OutAbove)
{
    const FVector Far = Reach.Max.ComponentMax(-Reach.Min);
    OutHalf = FVector(Far.X, Far.Y, FMath::Max(-Reach.Min.Z, 0.0));
    OutAbove = FMath::Max(Reach.Max.Z - OutHalf.Z, 0.0);
}

double FSCCargoDroneSteer::FreeAlong(TConstArrayView<FVector> Path, double Radius, TConstArrayView<FOther> Others, double LeadS)
{
    double Along = 0, Clear = 0; // Clear: the last point found clear
    for (int32 I = 0; I < Path.Num(); ++I)
    {
        const bool bLeg = I + 1 < Path.Num();
        const FVector A = Path[I], B = bLeg ? Path[I + 1] : Path[I];
        const double Length = FVector::Dist(A, B);
        const int32 Steps = bLeg ? FMath::Max(1, FMath::CeilToInt32(Length / FMath::Max(Radius / 4, 1.0))) : 1;
        for (int32 K = 0; K < Steps; ++K)
        {
            const FVector P = FMath::Lerp(A, B, double(K) / Steps);
            for (const FOther& O : Others)
                if (FMath::PointDistToSegment(P, O.P, O.P + O.V * LeadS) < Keep(Radius, O.Radius)) return Clear;
            Clear = Along + Length * K / Steps;
        }
        Along += Length;
    }
    return Along;
}

FVector FSCCargoDroneSteer::OffFace(const FVector& To, const FVector& HullHalf, const FTransform& Pose, const FVector& Half, const FVector& Face)
{
    if (Face.IsNearlyZero()) return To;
    const FQuat R = Pose.GetRotation();
    const FVector Reach = BoundsIn(R, HullHalf), C = R.UnrotateVector(To - Pose.GetLocation());
    // The face's axis and side.
    const int32 K = FMath::Abs(Face.X) >= FMath::Abs(Face.Y) && FMath::Abs(Face.X) >= FMath::Abs(Face.Z) ? 0 : FMath::Abs(Face.Y) >= FMath::Abs(Face.Z) ? 1 : 2;
    const double Side = Face[K] >= 0 ? 1.0 : -1.0;
    // Over the face (within the box's other sides grown by the hull) and not beyond it by the hull's reach: out along it.
    for (int32 J = 0; J < 3; ++J)
        if (J != K && FMath::Abs(C[J]) >= Half[J] + Reach[J]) return To;
    const double Out = Side * C[K];
    if (Out >= Half[K] + Reach[K] || Out < 0) return To;
    const FVector Axes[] = {R.GetAxisX(), R.GetAxisY(), R.GetAxisZ()};
    return To + Axes[K] * (Side * (Half[K] + Reach[K] - Out));
}

// --------------------------------------------------------------------------------------------- holding from the ends

FBox FSCCargoEndGrip::HullAt(const FVector& StationCm) const
{
    // About the emitter: its hull's centre lies within EmitterPlanCm of it in plan (any heading), Above / Below of it
    // upright.
    const double Plan = FMath::Max(Drone.HullHalf.X, Drone.HullHalf.Y) + Drone.EmitterPlanCm;
    const FVector Centre = StationCm + FVector(0, 0, (Drone.AboveCm - Drone.BelowCm) / 2);
    return FBox::BuildAABB(Centre, FVector(Plan, Plan, (Drone.AboveCm + Drone.BelowCm) / 2));
}

FBox FSCCargoEndGrip::Shape() const
{
    FBox Out = FBox::BuildAABB(FVector::ZeroVector, BoxHalf);
    Out += HullAt(Station(0));
    Out += HullAt(Station(1));
    return Out;
}

double FSCCargoEndGrip::AccelLimit(const FSCCargoBeam& Beam, double MassKg, double GravityMS2) const
{
    // Each beam's share of m (g + a), any direction a: the worst is a straight up, |share| = m (g + a) / N.
    const double Share = MassKg / FMath::Max(Team, 1);
    if (Share <= 0) return 100;
    return FMath::Clamp(Beam.MaxForceN / Share - FMath::Max(GravityMS2, 0.0), 0.0, 100.0);
}

FSCCargoEndGrip FSCCargoEndGrip::Plan(const FIn& In)
{
    FSCCargoEndGrip Out;
    Out.BoxHalf = In.BoxHalfCm;
    Out.Drone = In.Drone;
    Out.Team = In.Team;
    Out.Axis = In.BoxHalfCm.X >= In.BoxHalfCm.Y ? 0 : 1;
    Out.InsetCm = In.InsetCm;
    Out.GapCm = In.GapCm;
    Out.MaxAngleDeg = In.MaxAngleDeg;
    const FSCCargoBeam& Beam = In.Beam;
    Out.MinCm = Beam.MinM * 100.0;
    Out.FullCm = FMath::Max(Beam.FullM, Beam.MinM) * 100.0;
    if (In.Team < 1 || In.Team > 2)
    {
        Out.Why = TEXT("a box is held from its ends by one or two drones (larger teams hold it over)");
        return Out;
    }
    Out.ShareN = In.MassKg * FMath::Max(In.GravityMS2, 0.0) / In.Team;
    Out.ForceN = Out.ShareN;
    if (Out.ShareN >= Beam.MaxForceN)
    {
        Out.Why = TEXT("its share of the weight is the beam's whole force");
        return Out;
    }
    const double Hz = In.BoxHalfCm.Z;
    // Out from the end face far enough for the hull to clear it at any heading; its top at the box's top; the hold level
    // or down onto the face (the attach point at the emitter's height, kept on the face).
    Out.ClearCm = FMath::Max(In.Drone.HullHalf.X, In.Drone.HullHalf.Y) + In.Drone.EmitterPlanCm + In.GapCm;
    const double Inset = FMath::Min(In.InsetCm, Hz);
    const double StationZ = Hz - In.Drone.AboveCm;
    Out.AttachZCm = FMath::Clamp(StationZ, -Hz + Inset, FMath::Max(Hz - Inset, -Hz + Inset));
    Out.OutCm = Out.ClearCm;
    Out.RiseCm = StationZ - Out.AttachZCm;
    // Not shorter than the record's minimum distance: further out along the face's normal.
    if (FMath::Sqrt(FMath::Square(Out.OutCm) + FMath::Square(Out.RiseCm)) < Out.MinCm)
        Out.OutCm = FMath::Sqrt(FMath::Max(FMath::Square(Out.MinCm) - FMath::Square(Out.RiseCm), 0.0));
    Out.BeamCm = FMath::Sqrt(FMath::Square(Out.OutCm) + FMath::Square(Out.RiseCm));
    Out.AngleRad = FMath::Atan2(FMath::Max(-Out.RiseCm, 0.0), Out.OutCm); // below level (the hold points down onto the face)
    if (Out.BeamCm > Out.FullCm)
    {
        Out.Why = FString::Printf(TEXT("the hold (%.0f cm) is beyond the beam's full-strength distance"), Out.BeamCm);
        return Out;
    }
    if (FMath::RadiansToDegrees(FMath::Atan2(FMath::Abs(Out.RiseCm), Out.OutCm)) > In.MaxAngleDeg)
    {
        Out.Why = TEXT("the hold is outside the beam's cone (maxAngle)");
        return Out;
    }
    Out.MaxAccelMS2 = Out.AccelLimit(Beam, In.MassKg, In.GravityMS2);
    if (Out.MaxAccelMS2 < In.MinAccelMS2)
    {
        Out.Why = FString::Printf(TEXT("the beams have under %.1f m/s^2 to spare"), In.MinAccelMS2);
        return Out;
    }
    Out.bValid = true;
    return Out;
}

TArray<FVector> FSCCargoEndGrip::SwapWay(const FVector& From, const FVector& To, double MinZ)
{
    // Up where it is to the higher height (at least MinZ), across, down: from over the box out past its end, or back.
    const double Z = FMath::Max3(From.Z, To.Z, MinZ);
    TArray<FVector> Way = {From};
    for (const FVector& P : {FVector(From.X, From.Y, Z), FVector(To.X, To.Y, Z), To})
        if (!P.Equals(Way.Last(), 0.5)) Way.Add(P);
    return Way;
}

TArray<FVector> FSCCargoEndGrip::WayRound(const FVector& From, const FVector& To) const
{
    // Across at the higher height where that clears the box: its plan grown by the hull's reach (any heading) and the gap
    // passed by the straight way across, or the hull's bottom over its top there.
    // (A place on a face is out by just that: within a centimetre of it counts as clear, its gap still kept but that.)
    const double Reach = FMath::Max(Drone.HullHalf.X, Drone.HullHalf.Y) + Drone.EmitterPlanCm + GapCm - 1.0;
    const FBox Plan(FVector(-BoxHalf.X - Reach, -BoxHalf.Y - Reach, -1e6), FVector(BoxHalf.X + Reach, BoxHalf.Y + Reach, 1e6));
    const FVector A(From.X, From.Y, 0), B(To.X, To.Y, 0);
    const bool bCrosses = !(A - B).IsNearlyZero(0.5) ? FMath::LineBoxIntersection(Plan, A, B, B - A) : Plan.IsInside(A);
    const bool bOver = bCrosses && FMath::Max(From.Z, To.Z) < OverZ();
    return SwapWay(From, To, bOver ? OverZ() : -1e9);
}

FVector FSCCargoEndGrip::FaceNear(const FVector& P) const
{
    int32 Face = 2;
    double Most = -1e9;
    for (int32 K = 0; K < 3; ++K)
        if (const double Beyond = FMath::Abs(P[K]) - BoxHalf[K]; Beyond > Most) { Most = Beyond; Face = K; }
    FVector N = FVector::ZeroVector;
    N[Face] = P[Face] >= 0 ? 1.0 : -1.0;
    return N;
}

FVector FSCCargoEndGrip::SurfaceNear(const FVector& P) const
{
    FVector Out = P.BoundToBox(-BoxHalf, BoxHalf);
    // On the face it reaches (the axis it is furthest beyond), within the inset of that face's edges on the others.
    int32 Face = 0;
    double Most = -1e9;
    for (int32 K = 0; K < 3; ++K)
        if (const double Beyond = FMath::Abs(P[K]) - BoxHalf[K]; Beyond > Most) { Most = Beyond; Face = K; }
    for (int32 K = 0; K < 3; ++K)
        if (K != Face)
        {
            const double In = FMath::Max(BoxHalf[K] - FMath::Min(InsetCm, BoxHalf[K]), 0.0);
            Out[K] = FMath::Clamp(Out[K], -In, In);
        }
    return Out;
}

// ---------------------------------------------------------------------------------------------- where a drone holds

FVector FSCCargoGripSpot::FaceNormal(int32 LongAxis) const
{
    const double S = Face ? 1.0 : -1.0;
    switch (Kind)
    {
    case EKind::End: return (LongAxis ? FVector::RightVector : FVector::ForwardVector) * S;
    case EKind::Side: return (LongAxis ? FVector::ForwardVector : FVector::RightVector) * S;
    default: return FVector::UpVector;
    }
}

TArray<FSCCargoGripSpot> FSCCargoGripPick::Candidates(const FSCCargoEndGrip& G, double HangCm, const FCfg& Cfg, FRandomStream& Rng, int32 PerKind)
{
    TArray<FSCCargoGripSpot> Out;
    const FVector H = G.BoxHalf;
    const int32 A = G.Axis, L = 1 - G.Axis; // the long and the other plan axis
    const double Inset = FMath::Min(G.InsetCm, H.Z);
    const FBox Box = FBox::BuildAABB(FVector::ZeroVector, H + FVector(G.GapCm - 0.5));
    auto Fits = [&](const FSCCargoGripSpot& S) {
        const double Beam = FVector::Dist(S.Station, S.Attach);
        if (G.HullAt(S.Station).Intersect(Box) || Beam < G.MinCm - 0.5 || Beam > G.FullCm + 0.5) return false;
        // In front of a face or beside it: inside the beam's cone about the drone's level facing (it faces the box; it
        // holds level or down). Over the top it holds straight down from its belly, as every grip over the box always has.
        if (S.Kind == FSCCargoGripSpot::EKind::Over) return true;
        const FVector D = S.Attach - S.Station;
        return FMath::RadiansToDegrees(FMath::Atan2(FMath::Max(-D.Z, 0.0), D.Size2D())) <= G.MaxAngleDeg + 1e-6;
    };
    auto Across = [&](double Half) { return (2.0 * Rng.FRand() - 1.0) * Cfg.Spread * FMath::Max(Half - Inset, 0.0); };
    // In front of a face (an end or a side): out by the clearance and up to ExtraReach more, its top at the box's top and
    // up to ExtraHeight higher, across the face by Spread; the hold at the emitter's height kept on the face.
    auto FaceSpot = [&](FSCCargoGripSpot::EKind Kind, int32 Face, int32 Normal, int32 AcrossAxis) {
        FSCCargoGripSpot S;
        S.Kind = Kind;
        S.Face = Face;
        const double Sign = Face ? 1.0 : -1.0;
        const double Out = G.ClearCm * (1.0 + Cfg.ExtraReach * Rng.FRand());
        const double Z = H.Z - G.Drone.AboveCm + Cfg.ExtraHeightCm * Rng.FRand();
        S.Attach[Normal] = Sign * H[Normal];
        S.Attach[AcrossAxis] = Across(H[AcrossAxis]);
        S.Attach.Z = FMath::Clamp(Z, -H.Z + Inset, FMath::Max(H.Z - Inset, -H.Z + Inset));
        S.Station = S.Attach;
        S.Station[Normal] = Sign * (H[Normal] + Out);
        S.Station.Z = Z;
        return S;
    };
    for (int32 K = 0; K < PerKind; ++K)
    {
        const int32 Face = Rng.FRand() < 0.5 ? 0 : 1;
        if (FSCCargoGripSpot S = FaceSpot(FSCCargoGripSpot::EKind::End, Face, A, L); Fits(S)) Out.Add(S);
        const int32 SideFace = Rng.FRand() < 0.5 ? 0 : 1;
        if (FSCCargoGripSpot S = FaceSpot(FSCCargoGripSpot::EKind::Side, SideFace, L, A); Fits(S)) Out.Add(S);
        // Over the top: anywhere along it and across it (spread), at the shortest hang and up to ExtraHeight more.
        FSCCargoGripSpot O;
        O.Kind = FSCCargoGripSpot::EKind::Over;
        O.Attach = FVector(0, 0, H.Z);
        O.Attach[A] = Across(H[A]);
        O.Attach[L] = 0.5 * Across(H[L]);
        O.Station = O.Attach + FVector(0, 0, HangCm + Cfg.ExtraHeightCm * Rng.FRand());
        if (Fits(O)) Out.Add(O);
    }
    return Out;
}

FBox FSCCargoGripPick::Reach(const FSCCargoEndGrip& G, double HangCm, const FCfg& Cfg)
{
    // The farthest places out from each face (at its corners: a side's spread reaches no further), low and as high as the
    // highest over the top; the ways between them go no further out nor higher.
    const FVector H = G.BoxHalf;
    const double Out = G.ClearCm * (1.0 + Cfg.ExtraReach);
    const double Low = H.Z - G.Drone.AboveCm, High = FMath::Max(H.Z + HangCm + Cfg.ExtraHeightCm, Low + Cfg.ExtraHeightCm);
    FBox R = FBox::BuildAABB(FVector::ZeroVector, H);
    for (const double SX : {-1.0, 1.0})
        for (const double SY : {-1.0, 1.0})
            for (const double Z : {Low, High}) R += G.HullAt(FVector(SX * (H.X + Out), SY * (H.Y + Out), Z));
    return R;
}

int32 FSCCargoGripPick::Pick(TConstArrayView<FSCCargoGripSpot> Viable, TConstArrayView<FSCCargoGripSpot> Teammates, double KeepCm, const FCfg& Cfg, FRandomStream& Rng)
{
    // The kind's weight, renormalised over the kinds among Viable (so ends stay about 3 of 4 when all are viable), a
    // teammate's face lowered, a place within KeepCm of a teammate out.
    bool bKind[3] = {false, false, false};
    for (const FSCCargoGripSpot& S : Viable) bKind[uint8(S.Kind)] = true;
    const float KindWeight[3] = {Cfg.EndWeight, Cfg.SideWeight, Cfg.OverWeight};
    int32 KindCount[3] = {0, 0, 0};
    for (const FSCCargoGripSpot& S : Viable) ++KindCount[uint8(S.Kind)];
    TArray<double> W;
    double Sum = 0;
    for (const FSCCargoGripSpot& S : Viable)
    {
        double Weight = KindWeight[uint8(S.Kind)] / FMath::Max(KindCount[uint8(S.Kind)], 1); // a kind's weight over its places
        for (const FSCCargoGripSpot& T : Teammates)
        {
            if (FVector::Dist(S.Station, T.Station) < KeepCm) Weight = 0;
            else if (S.SameFace(T) && S.Kind != FSCCargoGripSpot::EKind::Over) Weight *= Cfg.SameFaceWeight;
        }
        W.Add(Weight);
        Sum += Weight;
    }
    if (Sum <= 0) return INDEX_NONE;
    double R = Rng.FRand() * Sum;
    for (int32 I = 0; I < W.Num(); ++I)
    {
        if (R < W[I]) return I;
        R -= W[I];
    }
    for (int32 I = W.Num() - 1; I >= 0; --I)
        if (W[I] > 0) return I;
    return INDEX_NONE;
}

FVector FSCCargoDroneSteer::Evade(const FVector& To, const FVector& HullHalf, TConstArrayView<FLoad> Loads, double Accel)
{
    FVector End = To;
    // Twice: the way out of one load's path may lie in another's.
    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        bool bMoved = false;
        for (const FLoad& L : Loads)
        {
            // In the load's frame: the load grown by the hull's bounds is where the drone's centre must not be; as the
            // load flies on, the centre as the load sees it goes from C back along the load's velocity.
            const FQuat R = L.Pose.GetRotation();
            const FVector H = L.Half + BoundsIn(R, HullHalf);
            const FVector C = R.UnrotateVector(End - L.Pose.GetLocation()), V = R.UnrotateVector(L.V);
            auto Time = [Accel](double Way) { return 2.0 * FMath::Sqrt(Way / FMath::Max(Accel, 1.0)) + 0.25; };
            // Looked at first as far ahead as the longest way out of it could take.
            const double Longest = Time(2 * H.GetMax());
            if (!SegmentMeetsBox(C, -V * Longest, FVector::ZeroVector, H)) continue;
            // Out across the way it flies (not ahead of it, where it comes, nor behind), level (over a load its own
            // drones fly, under it there is seldom room): along such an axis of the load, by as much as takes the
            // centre past the load's side for as long as the load is passing it. A load that stands: its nearest
            // level side.
            const FVector Axes[] = {R.GetAxisX(), R.GetAxisY(), R.GetAxisZ()};
            const double Speed = V.Size();
            int32 Axis = INDEX_NONE;
            double Shift = TNumericLimits<double>::Max();
            for (int32 I = 0; I < 3; ++I)
            {
                if (FMath::Abs(Axes[I].Z) > 0.7 || (Speed > 1.0 && FMath::Abs(V[I]) > 0.8 * Speed)) continue;
                // While the load passes: the times within the look at which the centre is within its other two sides.
                double From = 0, Until = Longest;
                bool bPasses = true;
                for (int32 J = 0; J < 3 && bPasses; ++J)
                {
                    if (J == I) continue;
                    if (FMath::Abs(V[J]) < 1e-6) bPasses = FMath::Abs(C[J]) <= H[J];
                    else
                    {
                        double A = (C[J] - H[J]) / V[J], B = (C[J] + H[J]) / V[J]; // C[J] - V[J] t within -H .. H
                        if (A > B) Swap(A, B);
                        From = FMath::Max(From, A);
                        Until = FMath::Min(Until, B);
                        bPasses = From <= Until;
                    }
                }
                if (!bPasses) continue;
                // Along this axis the centre is then between Lo and Hi: past the + side by H - Lo, the - side by -H - Hi.
                const double AtFrom = C[I] - V[I] * From, AtUntil = C[I] - V[I] * Until;
                const double Lo = FMath::Min(AtFrom, AtUntil), Hi = FMath::Max(AtFrom, AtUntil);
                for (const double Way : {H[I] - Lo + 0.5, -H[I] - Hi - 0.5})
                    if (FMath::Abs(Way) < FMath::Abs(Shift))
                    {
                        Shift = Way;
                        Axis = I;
                    }
            }
            // Not yet: it is still further off than it flies while the drone takes that way out.
            if (Axis == INDEX_NONE || !SegmentMeetsBox(C, -V * Time(FMath::Abs(Shift)), FVector::ZeroVector, H)) continue;
            FVector Out = C;
            Out[Axis] += Shift;
            End = L.Pose.GetLocation() + R.RotateVector(Out);
            bMoved = true;
        }
        if (!bMoved) break;
    }
    return End;
}
