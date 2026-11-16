// FSCCargoRoute (spline, clearance measurement) and FSCCargoFlight (curvature- and braking-capped, jerk-limited speed
// profile along the route), gusts. The route search itself is SCCargoRouteSearch.cpp.
#include "Cargo/SCCargoFlightPath.h"
#include "HAL/PlatformTime.h"

double FSCCargoRoute::Length() const
{
    double L = 0;
    for (int32 I = 1; I < Samples.Num(); ++I) L += FVector::Dist(Samples[I - 1], Samples[I]);
    return L;
}

TArray<FVector> FSCCargoRoute::Spline(const TArray<FVector>& P, double StepCm)
{
    TArray<FVector> Out;
    if (P.Num() < 2) return P;
    for (int32 I = 0; I + 1 < P.Num(); ++I)
    {
        // Centripetal Catmull-Rom (alpha 0.5) between P1 and P2; the ends mirror their neighbour (no overshoot there).
        const FVector P1 = P[I], P2 = P[I + 1];
        const FVector P0 = I > 0 ? P[I - 1] : 2 * P1 - P2;
        const FVector P3 = I + 2 < P.Num() ? P[I + 2] : 2 * P2 - P1;
        auto Knot = [](double T, const FVector& A, const FVector& B) { return T + FMath::Max(FMath::Sqrt(FVector::Dist(A, B)), 1e-3); };
        const double T0 = 0, T1 = Knot(T0, P0, P1), T2 = Knot(T1, P1, P2), T3 = Knot(T2, P2, P3);
        const int32 Steps = FMath::Max(1, FMath::CeilToInt(FVector::Dist(P1, P2) / StepCm));
        for (int32 K = 0; K < Steps; ++K)
        {
            const double T = FMath::Lerp(T1, T2, double(K) / Steps);
            const FVector A1 = (T1 - T) / (T1 - T0) * P0 + (T - T0) / (T1 - T0) * P1;
            const FVector A2 = (T2 - T) / (T2 - T1) * P1 + (T - T1) / (T2 - T1) * P2;
            const FVector A3 = (T3 - T) / (T3 - T2) * P2 + (T - T2) / (T3 - T2) * P3;
            const FVector B1 = (T2 - T) / (T2 - T0) * A1 + (T - T0) / (T2 - T0) * A2;
            const FVector B2 = (T3 - T) / (T3 - T1) * A2 + (T - T1) / (T3 - T1) * A3;
            Out.Add((T2 - T) / (T2 - T1) * B1 + (T - T1) / (T2 - T1) * B2);
        }
    }
    Out.Add(P.Last());
    return Out;
}

void FSCCargoClearance::Begin(double StepCm)
{
    Hits = 0;
    Min = 200;
    Since = StepCm;
}

void FSCCargoClearance::Sample(const TArray<FVector>& Samples, int32 I, const FSCCargoRouteQuery& Q, double StepCm)
{
    static const double Inflations[] = {0, 10, 25, 50, 100, 200};
    if (I > 0)
    {
        if (Q.Free && !Q.Free(Samples[I - 1], Samples[I])) ++Hits;
        Since += FVector::Dist(Samples[I - 1], Samples[I]);
    }
    if (Since < StepCm && I + 1 < Samples.Num()) return;
    Since = 0;
    double Clear = -1;
    for (const double Inflate : Inflations)
    {
        if (Q.BlockedBy && Q.BlockedBy(Samples[I], Inflate)) break;
        Clear = Inflate;
    }
    if (Clear < 0)
    {
        ++Hits;
        Clear = 0;
    }
    Min = FMath::Min(Min, Clear);
}

double FSCCargoRoute::MinClearance(const TArray<FVector>& Samples, const FSCCargoRouteQuery& Q, int32& OutHits, double StepCm)
{
    FSCCargoClearance C;
    C.Begin(StepCm);
    for (int32 I = 0; I < Samples.Num(); ++I) C.Sample(Samples, I, Q, StepCm);
    OutHits = C.Hits;
    return C.Min;
}

bool SCCargoSegmentInside(const FVector& A, const FVector& B, const FBox& Box, double& OutEnter, double& OutLeave)
{
    // Slabs: the stretch of A..B (as fractions of it) inside Box on every axis.
    double Enter = 0, Leave = 1;
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        const double From = A[Axis], Delta = B[Axis] - A[Axis];
        if (FMath::Abs(Delta) < 1e-9)
        {
            if (From < Box.Min[Axis] || From > Box.Max[Axis]) return false;
            continue;
        }
        double T0 = (Box.Min[Axis] - From) / Delta, T1 = (Box.Max[Axis] - From) / Delta;
        if (T0 > T1) Swap(T0, T1);
        Enter = FMath::Max(Enter, T0);
        Leave = FMath::Min(Leave, T1);
        if (Enter > Leave) return false;
    }
    OutEnter = Enter;
    OutLeave = Leave;
    return true;
}

// ------------------------------------------------------------------------------------------------------------ flight

void FSCCargoFlight::Start(const TArray<FVector>& Path, double InMaxSpeed, double InMaxAccel, double InMaxJerk)
{
    Points.Reset();
    for (const FVector& P : Path)
        if (Points.IsEmpty() || FVector::DistSquared(Points.Last(), P) > 1e-4) Points.Add(P);
    if (Points.Num() == 1)
    {
        const FVector Only = Points[0]; // a copy: adding an element of the array to itself may reallocate under it
        Points.Add(Only);
    }
    MaxSpeed = InMaxSpeed;
    MaxAccel = InMaxAccel;
    MaxJerk = InMaxJerk;
    Dist = Speed = Accel = 0;
    StopAt = HoldAt = -1;
    S.SetNum(Points.Num());
    VCap.SetNum(Points.Num());
    S[0] = 0;
    for (int32 I = 1; I < Points.Num(); ++I) S[I] = S[I - 1] + FVector::Dist(Points[I - 1], Points[I]);
    // Lateral acceleration in turns: v^2 / R <= a, R the circumradius of three consecutive samples.
    for (int32 I = 0; I < Points.Num(); ++I)
    {
        VCap[I] = MaxSpeed;
        if (I == 0 || I + 1 == Points.Num()) continue;
        const FVector A = Points[I] - Points[I - 1], B = Points[I + 1] - Points[I], C = Points[I + 1] - Points[I - 1];
        const double Cross = FVector::CrossProduct(A, B).Size();
        if (Cross > 1e-6) VCap[I] = FMath::Min(MaxSpeed, FMath::Sqrt(MaxAccel * A.Size() * B.Size() * C.Size() / (2 * Cross)));
    }
    // Braking: from every point the rest of the path can be stopped on (a margin left for the jerk ramp).
    VCap.Last() = 0;
    const double Brake = MaxAccel * 0.7;
    for (int32 I = Points.Num() - 2; I >= 0; --I)
        VCap[I] = FMath::Min(VCap[I], FMath::Sqrt(VCap[I + 1] * VCap[I + 1] + 2 * Brake * (S[I + 1] - S[I])));
}

double FSCCargoFlight::CapAt(double D) const
{
    if (S.Num() < 2) return 0;
    const int32 I = FMath::Clamp(Algo::UpperBound(S, D) - 1, 0, S.Num() - 2);
    const double T = FMath::Clamp((D - S[I]) / FMath::Max(S[I + 1] - S[I], 1e-6), 0.0, 1.0);
    return FMath::Lerp(VCap[I], VCap[I + 1], T);
}

FVector FSCCargoFlight::At(double D) const
{
    if (Points.Num() < 2) return Points.IsEmpty() ? FVector::ZeroVector : Points[0];
    const int32 I = FMath::Clamp(Algo::UpperBound(S, D) - 1, 0, S.Num() - 2);
    const double T = FMath::Clamp((D - S[I]) / FMath::Max(S[I + 1] - S[I], 1e-6), 0.0, 1.0);
    return FMath::Lerp(Points[I], Points[I + 1], T);
}

FVector FSCCargoFlight::Tangent() const
{
    const FVector D = At(FMath::Min(Dist + 25, Length())) - At(FMath::Max(Dist - 25, 0.0));
    return D.GetSafeNormal();
}

bool FSCCargoFlight::Step(double Dt)
{
    if (Points.Num() < 2 || Dt <= 0) return Done();
    // Held: the path ends there for now (a flight already beyond it stays where it is).
    const double End = HoldAt >= 0 ? FMath::Max(FMath::Min(Length(), HoldAt), Dist) : Length();
    const double Remaining = (StopAt >= 0 ? FMath::Min(End, StopAt) : End) - Dist;
    // Speed the path allows here (curvature, braking to the end); acceleration follows it, its change limited by jerk.
    double Target = FMath::Min(CapAt(Dist), FMath::Sqrt(2 * MaxAccel * 0.7 * FMath::Max(Remaining, 0.0)));
    if (SpeedLimit > 0) Target = FMath::Min(Target, SpeedLimit);
    const double Want = FMath::Clamp((Target - Speed) / 0.15, -MaxAccel, MaxAccel);
    Accel += FMath::Clamp(Want - Accel, -MaxJerk * Dt, MaxJerk * Dt);
    Speed = FMath::Max(0.0, Speed + Accel * Dt);
    if (Speed == 0 && Accel < 0) Accel = 0;
    Dist = FMath::Min(End, Dist + Speed * Dt);
    if (End - Dist < 1.0 && Speed < 30)
    {
        Dist = End;
        Speed = Accel = 0;
    }
    return Done();
}

double FSCCargoFlight::Seconds(double Length, double MaxSpeed, double MaxAccel)
{
    if (MaxSpeed <= 0 || MaxAccel <= 0) return 0;
    return Length < MaxSpeed * MaxSpeed / MaxAccel ? 2 * FMath::Sqrt(Length / MaxAccel) : Length / MaxSpeed + MaxSpeed / MaxAccel;
}

double SCCargoLoadedAccel(int32 Drones, double LiftForceN, double LoadKg, double DroneKg, double GravityMS2, double EmptyAccelMS2)
{
    const double Mass = Drones * DroneKg + LoadKg;
    if (Mass <= 0) return EmptyAccelMS2;
    return FMath::Min(EmptyAccelMS2, (Drones * LiftForceN - LoadKg * GravityMS2) / Mass);
}

// ------------------------------------------------------------------------------------------------------------- sway

namespace
{
// A number in [0, 1) from (Seed, Channel, K, Salt).
double Unit(uint32 Seed, uint32 Channel, int32 K, uint32 Salt)
{
    const uint32 H = HashCombineFast(HashCombineFast(HashCombineFast(GetTypeHash(Seed), GetTypeHash(Channel)), GetTypeHash(K)), GetTypeHash(Salt));
    return (H % 10000) / 10000.0;
}
// Sum of three sines (weights 0.55 / 0.3 / 0.15: peak 1). Each seed (a drone) and channel (an axis) has its own
// spectrum: every component's frequency is FrequencyHz x (1, 1.73, 2.91) shifted by up to half an octave either way,
// and its own phase, so two drones' gusts are separate processes, not copies of one out of step.
double Noise(uint32 Seed, uint32 Channel, double T, double FrequencyHz)
{
    static const double Mult[3] = {1.0, 1.73, 2.91}, Weight[3] = {0.55, 0.3, 0.15};
    double V = 0;
    for (int32 K = 0; K < 3; ++K)
    {
        const double Phase = Unit(Seed, Channel, K, 0) * UE_DOUBLE_TWO_PI;
        const double Shift = FMath::Pow(2.0, Unit(Seed, Channel, K, 1) - 0.5);
        V += Weight[K] * FMath::Sin(UE_DOUBLE_TWO_PI * FrequencyHz * Mult[K] * Shift * T + Phase);
    }
    return V;
}
// A drone's own share of the scenario's amplitude: 0.7 to 1 of it (never more than the scenario allows).
double Strength(uint32 Seed) { return 0.7 + 0.3 * Unit(Seed, 99, 0, 2); }
} // namespace

FVector FSCCargoSway::Offset(uint32 Seed, double T, double AmplitudeCm, double FrequencyHz)
{
    return FVector(Noise(Seed, 0, T, FrequencyHz), Noise(Seed, 1, T, FrequencyHz), 0.5 * Noise(Seed, 2, T, FrequencyHz)) * AmplitudeCm * Strength(Seed);
}

FVector2D FSCCargoSway::Tilt(uint32 Seed, double T, double TiltDeg, double FrequencyHz)
{
    return FVector2D(Noise(Seed, 3, T, FrequencyHz), Noise(Seed, 4, T, FrequencyHz)) * TiltDeg * Strength(Seed);
}
