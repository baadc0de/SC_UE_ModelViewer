// Automation tests of the tug model (SCBridge.Cargo.Tug*): the beam's force law, the team's force split, a drone's own
// station (beam length, rest length, hover point), its look along the route for the room ahead, the drone as a body,
// a box flown by a team at several step lengths, and the independence of teammates' gusts; and of a drone's own
// steering (SCBridge.Cargo.Drone*): slid along what it meets, apart from other drones, out of its bay when its way
// is free, out of the way of a load, the space kept for other teams' drones, teammates' routes apart.
// All values are synthetic.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Cargo/SCCargoTug.h"
#include "Cargo/SCCargoFlightPath.h"

namespace
{
constexpr EAutomationTestFlags TugTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
FSCCargoBeam TestBeam()
{
    FSCCargoBeam B;
    B.MaxForceN = 1000;
    B.MinM = 0.5;
    B.FullM = 3;
    B.MaxM = 13;
    return B;
}

// A point-mass box under gravity flown along x by a team, as the subsystem does it (SCCargoTugStep.cpp): every drone
// takes its own station for its share, flies there as a body with its own gusts, and its beam's spring pulls the
// attach point to the anchor the drone carries. Returns the box's final position; OutWorst is its largest distance
// from the route point on the way.
FVector Fly(double BoxKg, int32 Team, double Dt, double Seconds, double RoomM, double GustM, double& OutWorst, double* OutMeanLength = nullptr)
{
    const double G = 9.81;
    FSCCargoBeam Beam = TestBeam();
    Beam.MaxForceN = 250000; // 500 kN/m: soft under 30 t, very stiff on 40 kg
    TArray<double> Along;
    for (int32 I = 0; I < Team; ++I) Along.Add(Team > 1 ? -1.0 + 2.0 * I / (Team - 1) : 0.0);
    const TArray<double> Shares = FSCCargoTug::Shares(Along);
    // The route: from rest 1 m/s^2 for 2 s, cruise 2 m/s, brake at 1 m/s^2 to a stop at x = 12.
    auto Route = [](double T, double& X, double& V, double& A) {
        if (T < 2) { A = 1; V = T; X = 0.5 * T * T; }
        else if (T < 6) { A = 0; V = 2; X = 2 + 2 * (T - 2); }
        else if (T < 8) { A = -1; V = 2 - (T - 6); X = 10 + 2 * (T - 6) - 0.5 * FMath::Square(T - 6); }
        else { A = 0; V = 0; X = 12; }
    };
    FVector BoxP(0, 0, 0), BoxV(0, 0, 0);
    TArray<FSCCargoDroneBody> Drones;
    TArray<FVector> Reaction;
    for (int32 I = 0; I < Team; ++I)
    {
        FSCCargoDroneBody& D = Drones.AddDefaulted_GetRef();
        D.MassKg = 180;
        D.MaxAccelMS2 = 12;
        D.MaxJerkMS3 = 30;
        D.MaxForceN = Beam.MaxForceN;
        D.GravityMS2 = G;
        // Close over the box, as a team is when it starts to tug: each pays its beam out from there.
        D.P = FVector(Along[I], 0, 0) + FVector(0, 0, Beam.MinM + Beam.StretchM(Shares[I] * BoxKg * G));
        Reaction.Add(FVector::ZeroVector);
    }
    OutWorst = 0;
    double LengthSum = 0;
    int32 Steps = 0;
    for (double T = 0; T < Seconds; T += Dt, ++Steps)
    {
        double X, V, A;
        Route(T + Dt, X, V, A);
        FVector Force(0, 0, -BoxKg * G);
        for (int32 I = 0; I < Team; ++I)
        {
            const FVector Share = FVector(A, 0, G) * Shares[I] * BoxKg;
            const FVector WantAttach = FVector(X + Along[I], 0, 0), Attach = BoxP + FVector(Along[I], 0, 0);
            const FSCCargoTug::FStation Station = FSCCargoTug::Station(Beam, Share, RoomM, WantAttach, FVector(V, 0, 0));
            Drones[I].Step(Station.Hover, Station.HoverV, Reaction[I], Dt);
            // The drone's own gusts move its emitter about the point its thrust holds.
            const FVector Gust = FSCCargoSway::Offset(GetTypeHash(I + 1) ^ 0x5eed, T, GustM, 0.3);
            const FVector Emitter = Drones[I].P + Gust;
            const FVector Pull = Beam.ForceN(Station.Anchor(Beam, Drones[I].P, Emitter) - Attach, FVector(V, 0, 0) - BoxV, FVector::Dist(Emitter, Attach), Shares[I] * BoxKg, Dt, Share);
            Reaction[I] = -Pull;
            Force += Pull;
            LengthSum += FVector::Dist(Emitter, Attach);
        }
        BoxV += Force / BoxKg * Dt;
        BoxP += BoxV * Dt;
        OutWorst = FMath::Max(OutWorst, FVector::Dist(BoxP, FVector(X, 0, 0)));
    }
    if (OutMeanLength) *OutMeanLength = LengthSum / FMath::Max(1, Steps * Team);
    return BoxP;
}

// The largest correlation of two series over any shift of up to MaxLag samples.
double LaggedCorrelation(const TArray<double>& A, const TArray<double>& B, int32 MaxLag)
{
    double Best = 0;
    for (int32 Lag = -MaxLag; Lag <= MaxLag; ++Lag)
    {
        double SA = 0, SB = 0, SAA = 0, SBB = 0, SAB = 0;
        int32 N = 0;
        for (int32 I = 0; I < A.Num(); ++I)
        {
            const int32 J = I + Lag;
            if (!B.IsValidIndex(J)) continue;
            SA += A[I]; SB += B[J]; SAA += A[I] * A[I]; SBB += B[J] * B[J]; SAB += A[I] * B[J];
            ++N;
        }
        const double Var = (SAA - SA * SA / N) * (SBB - SB * SB / N);
        if (Var > 1e-12) Best = FMath::Max(Best, FMath::Abs((SAB - SA * SB / N) / FMath::Sqrt(Var)));
    }
    return Best;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoTugBeamTest, "SCBridge.Cargo.TugBeam", TugTestFlags)
bool FSCCargoTugBeamTest::RunTest(const FString&)
{
    const FSCCargoBeam B = TestBeam();
    TestEqual(TEXT("Full force up to the full-strength distance"), B.CapN(3.0), 1000.0);
    TestEqual(TEXT("Half way to the maximum distance: half the force"), B.CapN(8.0), 500.0, 1e-6);
    TestEqual(TEXT("Nothing beyond the maximum distance"), B.CapN(14.0), 0.0);
    TestEqual(TEXT("The whole force stretched by the minimum distance"), B.StiffnessNPerM() * B.MinM, 1000.0, 1e-6);
    // The force the beam was set for when the attach point is where it was set for, at any step length.
    const FVector Hold(120, 0, 400);
    for (const double Dt : {1.0 / 240, 1.0 / 30, 0.25})
        TestTrue(TEXT("Held force at its stretch"), B.ForceN(Hold / B.StiffnessNPerM(), FVector::ZeroVector, 1.0, 50, Dt, Hold).Equals(Hold, 1e-6));
    // Off its place the attach point is pulled back to it, from any side (the anchor holds it, not a line to the emitter).
    const FVector Low = B.ForceN(Hold / B.StiffnessNPerM() + FVector(0, 0, 0.1), FVector::ZeroVector, 1.0, 50, 1.0 / 60, Hold);
    const FVector Aside = B.ForceN(Hold / B.StiffnessNPerM() + FVector(0, 0.1, 0), FVector::ZeroVector, 1.0, 50, 1.0 / 60, Hold);
    TestTrue(TEXT("Below its place: pulled up harder"), Low.Z > Hold.Z && FMath::IsNearlyEqual(Low.X, Hold.X, 1e-6));
    TestTrue(TEXT("Beside its place: pulled across"), Aside.Y > 0 && FMath::IsNearlyEqual(Aside.Z, Hold.Z, 1e-6));
    TestTrue(TEXT("The anchor moving away pulls harder (damping)"), B.ForceN(Hold / B.StiffnessNPerM(), FVector(0, 0, 0.5), 1.0, 50, 1.0 / 60, Hold).Z > Hold.Z);
    TestEqual(TEXT("Capped by the record"), B.ForceN(FVector(0, 0, 5), FVector::ZeroVector, 2.9, 50, 1.0 / 60, Hold).Size(), 1000.0, 1e-6);
    TestEqual(TEXT("Weaker beyond the full-strength distance"), B.ForceN(FVector(0, 0, 5), FVector::ZeroVector, 8.0, 50, 1.0 / 60, Hold).Size(), 500.0, 1e-6);
    TestEqual(TEXT("Rest length kept inside the record's range"), B.ClampRestM(0.1), 0.5);
    TestEqual(TEXT("Rest length kept inside the record's range (far)"), B.ClampRestM(40.0), 13.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoTugSharesTest, "SCBridge.Cargo.TugShares", TugTestFlags)
bool FSCCargoTugSharesTest::RunTest(const FString&)
{
    for (const TArray<double>& Along : {TArray<double>{0.0}, TArray<double>{-1.5, 1.5}, TArray<double>{-2.0, 0.0, 2.0}, TArray<double>{-1.0, 0.5, 3.0}, TArray<double>{-2.0, 1.0}})
    {
        const TArray<double> W = FSCCargoTug::Shares(Along);
        double Sum = 0, Moment = 0;
        for (int32 I = 0; I < W.Num(); ++I) { Sum += W[I]; Moment += W[I] * Along[I]; }
        TestEqual(TEXT("Shares sum to the whole force"), Sum, 1.0, 1e-9);
        if (Along.Num() > 1) TestEqual(TEXT("No moment about the centre"), Moment, 0.0, 1e-9);
    }
    const TArray<double> Even = FSCCargoTug::Shares({-1.5, 1.5});
    TestEqual(TEXT("Symmetric points share equally"), Even[0], 0.5, 1e-9);
    const TArray<double> Uneven = FSCCargoTug::Shares({-2.0, 1.0});
    TestTrue(TEXT("The nearer point carries more"), Uneven[1] > Uneven[0]);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoTugStationTest, "SCBridge.Cargo.TugStation", TugTestFlags)
bool FSCCargoTugStationTest::RunTest(const FString&)
{
    // Each drone's own decision: the beam's length, the spring's rest length for its share, where it hovers.
    const FSCCargoBeam B = TestBeam();
    const FVector Share(300, 0, 400), Want(10, 20, 30), WantV(1, 2, 0); // 500 N: a stretch of 0.25 m
    const FSCCargoTug::FStation Open = FSCCargoTug::Station(B, Share, 50, Want, WantV);
    TestEqual(TEXT("With room: out to the full-strength distance"), Open.LengthM, 3.0, 1e-9);
    TestEqual(TEXT("Rest length set for the share"), B.StiffnessNPerM() * (Open.LengthM - Open.RestM), 500.0, 1e-6);
    TestTrue(TEXT("The beam points along the share"), Open.Dir.Equals(Share / 500.0, 1e-9));
    TestTrue(TEXT("Hovers the beam's length out from where the attach point belongs"), Open.Hover.Equals(Want + Share / 500.0 * 3.0, 1e-9) && Open.HoverV.Equals(WantV));
    // On station the anchor is the stretch beyond the attach point's place: the spring gives the share there.
    TestTrue(TEXT("On station the spring gives the share"), ((Open.Anchor(B, Open.Hover, Open.Hover) - Want) * B.StiffnessNPerM()).Equals(Share, 1e-6));
    TestEqual(TEXT("On station the rest length is the station's"), Open.RestAt(B, Open.Hover), Open.RestM, 1e-9);
    // Half way out to its station the drone has paid out half as much: a shorter rest length, the same anchor.
    const FVector Midway = Want + Open.Dir * 1.5;
    TestTrue(TEXT("On its way out: a shorter rest length"), Open.RestAt(B, Midway) < Open.RestM - 1.0);
    TestTrue(TEXT("On its way out: the same anchor, the same share"), Open.Anchor(B, Midway, Midway).Equals(Open.AnchorWant, 1e-9));
    // A gust moves the emitter without the drone's knowing: the anchor goes with it.
    TestTrue(TEXT("A gust carries the anchor"), Open.Anchor(B, Open.Hover, Open.Hover + FVector(0, 0.04, 0)).Equals(Open.AnchorWant + FVector(0, 0.04, 0), 1e-9));
    const FSCCargoTug::FStation Low = FSCCargoTug::Station(B, Share, 1.2, Want, WantV);
    TestEqual(TEXT("Under a ceiling: as long as the room"), Low.LengthM, 1.2, 1e-9);
    TestEqual(TEXT("Reeled in: a shorter rest length for the same share"), B.StiffnessNPerM() * (Low.LengthM - Low.RestM), 500.0, 1e-6);
    TestTrue(TEXT("Reeled in: the rest length is shorter"), Low.RestM < Open.RestM);
    const FSCCargoTug::FStation Tight = FSCCargoTug::Station(B, Share, 0.1, Want, WantV);
    TestEqual(TEXT("Never a rest length under the record's minimum"), Tight.RestM, 0.5, 1e-9);
    TestEqual(TEXT("The shortest beam that gives the share"), Tight.LengthM, 0.75, 1e-9);
    TestTrue(TEXT("A heavier share needs a longer shortest beam"), FSCCargoTug::Station(B, Share * 1.5, 0.1, Want, WantV).LengthM > Tight.LengthM);
    // Two drones of a team decide apart: one with room, one under a ceiling, the same share from different lengths.
    TestTrue(TEXT("Teammates' beams differ with their room"), Open.LengthM - Low.LengthM > 1.5 && Open.RestM - Low.RestM > 1.5);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoTugDroneTest, "SCBridge.Cargo.TugDrone", TugTestFlags)
bool FSCCargoTugDroneTest::RunTest(const FString&)
{
    FSCCargoDroneBody D;
    D.MassKg = 180;
    D.MaxAccelMS2 = 12;
    D.MaxJerkMS3 = 30;
    D.MaxForceN = 1000;
    D.GravityMS2 = 9.81;
    TestEqual(TEXT("Unloaded: the whole acceleration, any way"), D.AccelLeft(FVector::ForwardVector, FVector::ZeroVector), 12.0, 1e-6);
    // Holding its beam's whole force straight down, climbing takes thrust it still has; pulled harder than that it has less.
    TestEqual(TEXT("At the beam's limit: still the whole acceleration upwards"), D.AccelLeft(FVector::UpVector, FVector(0, 0, -1000)), 12.0, 1e-6);
    TestTrue(TEXT("Beyond the beam's limit: less"), D.AccelLeft(FVector::UpVector, FVector(0, 0, -2500)) < 12.0);
    // A step to a hover point 3 m off: within the limits all the way, there and still at the end.
    double WorstAccel = 0, WorstJerk = 0, Overshoot = 0;
    const double Dt = 1.0 / 60;
    for (int32 I = 0; I < 600; ++I)
    {
        const FVector Before = D.A;
        D.Step(FVector(3, 0, 0), FVector::ZeroVector, FVector::ZeroVector, Dt);
        WorstAccel = FMath::Max(WorstAccel, D.A.Size());
        WorstJerk = FMath::Max(WorstJerk, (D.A - Before).Size() / Dt);
        Overshoot = FMath::Max(Overshoot, D.P.X - 3.0);
    }
    TestTrue(TEXT("Acceleration within the thrust limit"), WorstAccel <= 12.0 + 1e-6);
    TestTrue(TEXT("Jerk within its limit"), WorstJerk <= 30.0 + 1e-6);
    TestTrue(FString::Printf(TEXT("Arrives and stays (%.3f m off, %.3f m/s)"), FMath::Abs(D.P.X - 3.0), D.V.Size()), FMath::Abs(D.P.X - 3.0) < 0.01 && D.V.Size() < 0.01);
    TestTrue(FString::Printf(TEXT("Little overshoot (%.3f m)"), Overshoot), Overshoot < 0.3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoTugSlideTest, "SCBridge.Cargo.DroneSlide", TugTestFlags)
bool FSCCargoTugSlideTest::RunTest(const FString&)
{
    // A wall: everything with x >= 100 is solid (a sphere of radius 30 swept against it). Then a corner with a second
    // wall at y >= 200.
    const double R = 30;
    auto Walls = [R](bool bCorner) {
        return [R, bCorner](const FVector& A, const FVector& B, double& OutFraction, FVector& OutNormal) {
            double Best = 2;
            auto Plane = [&](double PosA, double PosB, double Limit, const FVector& N) {
                if (PosB <= Limit - R || PosB <= PosA) return;
                const double F = PosA >= Limit - R ? 0.0 : (Limit - R - PosA) / (PosB - PosA);
                if (F < Best) { Best = F; OutNormal = N; }
            };
            Plane(A.X, B.X, 100, FVector(-1, 0, 0));
            if (bCorner) Plane(A.Y, B.Y, 200, FVector(0, -1, 0));
            OutFraction = Best;
            return Best <= 1;
        };
    };
    bool bBlocked = false;
    auto Wall = Walls(false);
    FVector End = FSCCargoDroneSteer::Slide(FVector(0, 0, 0), FVector(50, 40, 0), Wall, bBlocked);
    TestTrue(TEXT("A free step is flown whole"), !bBlocked && End.Equals(FVector(50, 40, 0)));
    End = FSCCargoDroneSteer::Slide(FVector(0, 0, 0), FVector(200, 0, 0), Wall, bBlocked);
    TestTrue(FString::Printf(TEXT("Straight at the wall: stopped short of it (x %.2f)"), End.X), bBlocked && End.X < 70 && End.X > 69 && FMath::IsNearlyZero(End.Y));
    End = FSCCargoDroneSteer::Slide(FVector(0, 0, 0), FVector(200, 80, 0), Wall, bBlocked);
    TestTrue(FString::Printf(TEXT("At an angle: stopped at the wall and slid along it (x %.2f, y %.2f)"), End.X, End.Y), bBlocked && End.X < 70 && End.X > 69 && FMath::IsNearlyEqual(End.Y, 80.0, 0.5));
    auto Corner = Walls(true);
    End = FSCCargoDroneSteer::Slide(FVector(0, 100, 0), FVector(200, 300, 0), Corner, bBlocked);
    TestTrue(FString::Printf(TEXT("Into a corner: never past either wall (x %.2f, y %.2f)"), End.X, End.Y), bBlocked && End.X < 70 && End.Y < 170);
    // Step after step at the wall the drone never gets into it, and creeps along it.
    FVector P(0, 0, 0);
    for (int32 I = 0; I < 200; ++I)
    {
        P = FSCCargoDroneSteer::Slide(P, P + FVector(8, 3, 0), Wall, bBlocked);
        if (P.X > 70 + 1e-6) { AddError(FString::Printf(TEXT("In the wall at step %d (x %.3f)"), I, P.X)); break; }
    }
    TestTrue(FString::Printf(TEXT("Along the wall after 200 steps (y %.1f)"), P.Y), P.Y > 500);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoTugSeparateTest, "SCBridge.Cargo.DroneSeparate", TugTestFlags)
bool FSCCargoTugSeparateTest::RunTest(const FString&)
{
    // Two drones fly at each other's start, each deciding for itself from where the other is and how it moves:
    // head-on, crossing at right angles, and one overtaking a slower one. They never come nearer than they keep,
    // and both arrive.
    const double R = 45, Keep = FSCCargoDroneSteer::Keep(R, R), Dt = 1.0 / 60, Speed = 1000;
    TestEqual(TEXT("Kept distance: the radii and half a radius"), Keep, 112.5, 1e-9);
    struct FCase { FVector A0, A1, B0, B1; double SpeedB; const TCHAR* Name; };
    for (const FCase& Case : {FCase{FVector(0, 0, 300), FVector(2000, 0, 300), FVector(2000, 0, 300), FVector(0, 0, 300), Speed, TEXT("head-on")},
                              FCase{FVector(0, 1000, 300), FVector(2000, 1000, 300), FVector(1000, 0, 300), FVector(1000, 2000, 300), Speed, TEXT("crossing")},
                              FCase{FVector(0, 0, 300), FVector(3000, 0, 300), FVector(400, 0, 300), FVector(2000, 0, 300), Speed * 0.3, TEXT("overtaking")}})
    {
        FVector A = Case.A0, B = Case.B0, VA = FVector::ZeroVector, VB = FVector::ZeroVector;
        double Nearest = 1e9;
        auto Step = [&](const FVector& P, const FVector& Goal, double Max, const FVector& OtherP, const FVector& OtherV) {
            const FVector To = P + (Goal - P).GetClampedToMaxSize(Max * Dt);
            FSCCargoDroneSteer::FOther O;
            O.P = OtherP;
            O.V = OtherV;
            O.Radius = R;
            return FSCCargoDroneSteer::Separate(P, To, R, MakeArrayView(&O, 1), Dt);
        };
        for (int32 I = 0; I < 1200; ++I)
        {
            const FVector NewA = Step(A, Case.A1, Speed, B, VB);
            VA = (NewA - A) / Dt;
            A = NewA;
            const FVector NewB = Step(B, Case.B1, Case.SpeedB, A, VA);
            VB = (NewB - B) / Dt;
            B = NewB;
            Nearest = FMath::Min(Nearest, FVector::Dist(A, B));
        }
        TestTrue(FString::Printf(TEXT("%s: never nearer than the hulls (%.1f, hulls %.0f)"), Case.Name, Nearest, 2 * R), Nearest >= 2 * R);
        TestTrue(FString::Printf(TEXT("%s: the kept distance held but for the other's own turn (%.1f of %.1f)"), Case.Name, Nearest, Keep), Nearest >= Keep - 2 * Speed * Dt);
        TestTrue(FString::Printf(TEXT("%s: both arrive (%.1f, %.1f off)"), Case.Name, FVector::Dist(A, Case.A1), FVector::Dist(B, Case.B1)),
                 FVector::Dist(A, Case.A1) < 1 && FVector::Dist(B, Case.B1) < 1);
    }
    // Standing drones are left where they are; a drone asked to step into one stops at the kept distance.
    FSCCargoDroneSteer::FOther Still;
    Still.P = FVector(100, 0, 0);
    Still.Radius = R;
    const FVector End = FSCCargoDroneSteer::Separate(FVector(-200, 0, 0), FVector(100, 0, 0), R, MakeArrayView(&Still, 1), Dt);
    TestTrue(FString::Printf(TEXT("A step into a standing drone ends the kept distance from it (%.1f)"), FVector::Dist(End, Still.P)), FVector::Dist(End, Still.P) >= Keep - 1e-6);
    // Found inside the kept distance (the other came there), it moves out at its own pace: no step longer than a
    // step may be, and after a few of them the kept distance away.
    const double MaxStep = 2 * Speed * Dt;
    FVector P(40, 10, 0);
    double Longest = 0;
    for (int32 I = 0; I < 20; ++I)
    {
        const FVector Next = FSCCargoDroneSteer::Separate(P, P, R, MakeArrayView(&Still, 1), Dt, MaxStep);
        Longest = FMath::Max(Longest, FVector::Dist(P, Next));
        P = Next;
    }
    TestTrue(FString::Printf(TEXT("Out of the kept distance in steps of no more than a step (%.1f of %.1f cm)"), Longest, MaxStep), Longest <= MaxStep + 1e-6);
    TestTrue(FString::Printf(TEXT("... and then the kept distance away (%.1f)"), FVector::Dist(P, Still.P)), FVector::Dist(P, Still.P) >= Keep - 1e-6);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoTugFlightTest, "SCBridge.Cargo.TugFlight", TugTestFlags)
bool FSCCargoTugFlightTest::RunTest(const FString&)
{
    // A heavy box on two beams and a light one on a single beam (a stiff spring on little mass), at three step lengths
    // (the physics step is at most 1/30 s): they end where the route ends, stay near it on the way while the drones
    // pay their beams out from close over the box, and agree with each other.
    for (const TPair<double, int32>& Case : {TPair<double, int32>(30000.0, 2), TPair<double, int32>(40.0, 1)})
    {
        TArray<FVector> Ends;
        for (const double Dt : {1.0 / 240, 1.0 / 60, 1.0 / 30})
        {
            double Worst = 0;
            const FVector End = Fly(Case.Key, Case.Value, Dt, 14, 50, 0, Worst);
            Ends.Add(End);
            TestTrue(FString::Printf(TEXT("%.0f kg at %.0f Hz ends at the route's end (%.3f m off)"), Case.Key, 1 / Dt, FVector::Dist(End, FVector(12, 0, 0))),
                     FVector::Dist(End, FVector(12, 0, 0)) < 0.02);
            TestTrue(FString::Printf(TEXT("%.0f kg at %.0f Hz stays near the route (%.3f m at worst)"), Case.Key, 1 / Dt, Worst), Worst < 0.25);
        }
        TestTrue(TEXT("The same flight at every step length"), FVector::Dist(Ends[0], Ends[2]) < 0.02);
    }
    // The drones' own distance: out at the full-strength distance in the open, tucked in under a ceiling.
    double Worst = 0, Open = 0, Low = 0;
    Fly(30000, 2, 1.0 / 60, 14, 50, 0, Worst, &Open);
    Fly(30000, 2, 1.0 / 60, 14, 1.0, 0, Worst, &Low);
    TestTrue(FString::Printf(TEXT("Beams %.2f m long in the open, %.2f m under a ceiling"), Open, Low), Open > 2.5 && Low < 1.5 && Low >= 0.5);
    // Gusts: every drone's own (4 cm), all of them felt by the box through the springs, none of them carrying it off.
    double Calm = 0, Gusty = 0;
    Fly(40, 2, 1.0 / 60, 14, 50, 0, Calm);
    Fly(40, 2, 1.0 / 60, 14, 50, 0.04, Gusty);
    TestTrue(FString::Printf(TEXT("The box sways with its drones' gusts (%.3f m off at worst, %.3f m calm)"), Gusty, Calm), Gusty > Calm && Gusty < Calm + 0.08);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoTugSwayTest, "SCBridge.Cargo.TugSway", TugTestFlags)
bool FSCCargoTugSwayTest::RunTest(const FString&)
{
    // Teammates' gusts over ten minutes at 10 Hz: bounded by the scenario's amplitude, and no two of them alike at any
    // shift of up to a period of the slowest component (copies of one process out of step would be).
    const double Amplitude = 4, Frequency = 0.3;
    const int32 Samples = 6000, MaxLag = FMath::CeilToInt(10 / Frequency);
    TArray<TArray<double>> Series;
    for (int32 Drone = 1; Drone <= 4; ++Drone)
    {
        TArray<double>& S = Series.AddDefaulted_GetRef();
        const uint32 Seed = GetTypeHash(Drone) ^ 0x5eed;
        for (int32 I = 0; I < Samples; ++I)
        {
            const FVector O = FSCCargoSway::Offset(Seed, I * 0.1, Amplitude, Frequency);
            S.Add(O.X);
            if (FMath::Abs(O.X) > Amplitude + 1e-9 || FMath::Abs(O.Y) > Amplitude + 1e-9 || FMath::Abs(O.Z) > Amplitude / 2 + 1e-9)
            {
                AddError(TEXT("Sway beyond the scenario's amplitude"));
                return false;
            }
        }
    }
    constexpr double Threshold = 0.4;
    for (int32 A = 0; A < Series.Num(); ++A)
        for (int32 B = A + 1; B < Series.Num(); ++B)
        {
            const double C = LaggedCorrelation(Series[A], Series[B], MaxLag);
            TestTrue(FString::Printf(TEXT("Drones %d and %d sway independently (correlation %.2f at the best shift)"), A + 1, B + 1, C), C < Threshold);
        }
    // The measure tells copies out of step from separate processes: one spectrum with two sets of phases fails it.
    TArray<double> First, Second;
    for (int32 I = 0; I < Samples; ++I)
    {
        const double T = I * 0.1, W = UE_DOUBLE_TWO_PI * Frequency;
        First.Add(0.55 * FMath::Sin(W * T + 0.4) + 0.3 * FMath::Sin(W * 1.73 * T + 2.0) + 0.15 * FMath::Sin(W * 2.91 * T + 5.1));
        Second.Add(0.55 * FMath::Sin(W * T + 3.3) + 0.3 * FMath::Sin(W * 1.73 * T + 1.1) + 0.15 * FMath::Sin(W * 2.91 * T + 0.2));
    }
    TestTrue(TEXT("Phase-shifted copies of one spectrum are told apart"), LaggedCorrelation(First, Second, MaxLag) > Threshold);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoTugOwnBoxTest, "SCBridge.Cargo.TugOwnBox", TugTestFlags)
bool FSCCargoTugOwnBoxTest::RunTest(const FString&)
{
    // A drone's shape (0.68 x 0.69 x 0.38 m half, with its clearance; its centre 0.18 m over the emitter) and a box
    // 5 x 2.5 x 2.5 m. Hanging upright, the attach point is on its top: the station over it is clear of the box at
    // the shortest beam and is left as it is. Carried off an upright plate as it stood, the box hangs on its side:
    // the attach point is on a face that stands upright, and the station straight above it would be half in the
    // box; it moves out from the face until the shape is beside it.
    const FSCCargoBeam Beam = TestBeam();
    const FVector ShapeAt(0, 0, 0.18), ShapeHalf(0.68, 0.69, 0.38), BoxHalf(2.5, 1.25, 1.25), Share(0, 0, 500);
    auto InBox = [&](const FVector& Hover, const FQuat& BoxRotation) {
        // The level shape against the box (axis-aligned in these cases: turned by quarters).
        const FVector Half = BoxRotation.RotateVector(BoxHalf).GetAbs(), C = Hover + ShapeAt;
        return FMath::Abs(C.X) < Half.X + ShapeHalf.X - 1e-6 && FMath::Abs(C.Y) < Half.Y + ShapeHalf.Y - 1e-6 && FMath::Abs(C.Z) < Half.Z + ShapeHalf.Z - 1e-6;
    };
    FSCCargoTug::FStation Upright = FSCCargoTug::Station(Beam, Share, 0, FVector(0, 0, 1.25), FVector::ZeroVector); // no room: the shortest beam
    const FVector Before = Upright.Hover;
    Upright.KeepOffFace(FVector::UpVector, FVector(0, 0, 1.25), ShapeAt, ShapeHalf);
    TestTrue(TEXT("Over an upright box: the station is left as it is"), Upright.Hover.Equals(Before));
    TestFalse(TEXT("... and is clear of the box at the shortest beam"), InBox(Upright.Hover, FQuat::Identity));
    // On its side: the box's top faces +y, the attach point is the middle of that face.
    const FQuat OnSide(FVector::ForwardVector, -UE_DOUBLE_HALF_PI);
    const FVector FaceN = OnSide.GetAxisZ(), Attach = FaceN * 1.25;
    TestTrue(TEXT("On its side the box's top faces sideways"), FaceN.Equals(FVector(0, 1, 0), 1e-6));
    FSCCargoTug::FStation Side = FSCCargoTug::Station(Beam, Share, 0, Attach, FVector::ZeroVector); // tucked in, as beside boxes at rest
    TestTrue(TEXT("Straight over the attach point the drone would be in its box"), InBox(Side.Hover, OnSide));
    const double Height = Side.Hover.Z;
    Side.KeepOffFace(FaceN, Attach, ShapeAt, ShapeHalf);
    TestFalse(FString::Printf(TEXT("Moved out from the face it is beside the box (%s)"), *Side.Hover.ToCompactString()), InBox(Side.Hover, OnSide));
    TestTrue(TEXT("... by its reach across the face, at the height it chose"), FMath::IsNearlyEqual(Side.Hover.Y, 1.25 + ShapeHalf.Y, 1e-6) && FMath::IsNearlyEqual(Side.Hover.Z, Height, 1e-9));
    // The box it carries comes up under it (lagging its route where that drops): 75 cm in half a second towards a
    // drone 5 cm over its top. Each step the drone gives way (cm here): never in the box, and no further off it
    // than the box came.
    const FVector Hull(48, 49, 18), CmHalf(250, 125, 125);
    FVector Drone(100, 0, 125 + 18 + 5);
    bool bIn = false;
    for (int32 I = 0; I <= 30; ++I)
    {
        const FTransform Pose(FVector(0, 0, 75.0 * I / 30));
        Drone = FSCCargoDroneSteer::OffFace(Drone, Hull, Pose, CmHalf);
        bIn |= Drone.Z - Pose.GetLocation().Z < CmHalf.Z + Hull.Z - 1e-6;
    }
    TestFalse(TEXT("A box coming up under its drone: the drone is never in it"), bIn);
    TestTrue(FString::Printf(TEXT("... and rose with it by what was missing (to z %.1f)"), Drone.Z), FMath::IsNearlyEqual(Drone.Z, 75 + 125 + 18.0, 1e-6));
    TestTrue(TEXT("Beside the box, or clear above it: left where it is"), FSCCargoDroneSteer::OffFace(FVector(400, 0, 130), Hull, FTransform::Identity, CmHalf).Equals(FVector(400, 0, 130)) &&
                                                                         FSCCargoDroneSteer::OffFace(FVector(0, 0, 200), Hull, FTransform::Identity, CmHalf).Equals(FVector(0, 0, 200)));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoTugRoomAheadTest, "SCBridge.Cargo.TugRoomAhead", TugTestFlags)
bool FSCCargoTugRoomAheadTest::RunTest(const FString&)
{
    // A route under a lintel: 3 m of room over it but for x in 30 .. 40 m, where there is 1 m. A drone (the body the
    // subsystem flies: 12 m/s^2, 30 m/s^3) rides its beam's end over the route point; each tick it takes the room it
    // allows itself from the room over the route ahead. It must be under the lintel's height wherever its hull (half
    // a metre fore and aft) is under the lintel, at cruise and at a crawl; and it must not tuck in long before.
    const double Step = 0.4, Slack = 0.1, Rate = 30.0 / 12.0, HullHalf = 0.5, Margin = 0.2;
    auto RoomAt = [](double X) { return X >= 30 && X <= 40 ? 1.0 : 3.0; };
    for (const double Speed : {10.0, 4.0, 1.0})
    {
        FSCCargoDroneBody D;
        D.MassKg = 180;
        D.MaxAccelMS2 = 12;
        D.MaxJerkMS3 = 30;
        D.MaxForceN = 250000;
        D.GravityMS2 = 9.81;
        D.P = FVector(0, 0, 3);
        D.V = FVector(Speed, 0, 0);
        const double Dt = 1.0 / 60, Look = FSCCargoTug::LookAheadM(2.0, Speed, 0, Speed, Rate, Slack);
        double Worst = -1e9, OutAt = 0, HighBefore = 0, HighAfter = 0;
        for (double X = 0; X < 70; X += Speed * Dt)
        {
            TArray<double> Ahead;
            // The room where the hull is (half a hull behind the route point) and on to the look's end.
            for (double S = -HullHalf; S <= Look + HullHalf + Step; S += Step) Ahead.Add(FMath::Min(RoomAt(X + S), RoomAt(X + S + Step)));
            const double Room = FSCCargoTug::RoomAhead(Ahead, Step, Speed, 0, Speed, Rate, Slack, 3.0, HullHalf);
            D.Step(FVector(X, 0, Room), FVector(Speed, 0, 0), FVector::ZeroVector, Dt);
            for (const double Edge : {-HullHalf, HullHalf}) Worst = FMath::Max(Worst, D.P.Z - RoomAt(D.P.X + Edge));
            if (D.P.Z > 2.9) (D.P.X < 30 ? OutAt : HighAfter) = D.P.X;
            if (D.P.X < 30 - Look - 1) HighBefore = D.P.Z;
        }
        TestTrue(FString::Printf(TEXT("At %.0f m/s: under the lintel wherever the hull is under it (%.3f m over at worst, margin %.2f)"), Speed, Worst, Margin), Worst <= Margin);
        TestTrue(FString::Printf(TEXT("At %.0f m/s: out at full length until the look reaches the lintel (%.2f m)"), Speed, HighBefore), HighBefore > 2.9);
        TestTrue(FString::Printf(TEXT("At %.0f m/s: out again after it (last at 3 m at x %.1f)"), Speed, HighAfter), HighAfter > 60);
        TestTrue(FString::Printf(TEXT("At %.0f m/s: tucked in no earlier than the look ahead (%.1f m before, look %.1f m)"), Speed, 30 - OutAt, Look), 30 - OutAt <= Look + 2 * HullHalf + 1);
    }
    TArray<double> Here = {3.0};
    TestEqual(TEXT("Room here with nothing ahead"), FSCCargoTug::RoomAhead(Here, Step, 10, 0, 10, Rate, Slack, 3.0), 3.0, 1e-9);
    TArray<double> Low = {3.0, 1.0};
    TestTrue(TEXT("A lintel one step ahead at speed: its room now"), FSCCargoTug::RoomAhead(Low, Step, 10, 0, 10, Rate, Slack, 3.0) < 1.0 + 1e-6);
    TestTrue(TEXT("The same lintel at a crawl: still far in time, the room here"), FSCCargoTug::RoomAhead(Low, Step, 0.01, 0, 10, Rate, Slack, 3.0) > 2.99);
    // A route at rest that may set off at 4 m/s^2 gets to a lintel 3 m ahead in 1.2 s: too soon to shed 2 m, so the
    // drone is tucked in before the route moves; one 40 m ahead leaves it out.
    TArray<double> Near, Far;
    for (int32 K = 0; K <= 100; ++K)
    {
        Near.Add(K * Step >= 3 ? 1.0 : 3.0);
        Far.Add(K * Step >= 40 ? 1.0 : 3.0);
    }
    TestTrue(TEXT("At rest before a lintel 3 m ahead: tucked in"), FSCCargoTug::RoomAhead(Near, Step, 0, 4, 10, Rate, Slack, 3.0) < 1.5);
    TestTrue(TEXT("At rest with the lintel 40 m ahead: out"), FSCCargoTug::RoomAhead(Far, Step, 0, 4, 10, Rate, Slack, 3.0) > 2.99);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDroneKeepOutTest, "SCBridge.Cargo.DroneKeepOut", TugTestFlags)
bool FSCCargoDroneKeepOutTest::RunTest(const FString&)
{
    // A drone hovering over a box at rest (its hull 96 x 98 x 36 cm about (0, 0, 300)) is kept space: a load's shape
    // (a box 10 x 2.5 x 2.5 m) swept past meets it or not by where it flies.
    FSCCargoKeepOut Keep;
    Keep.Boxes.Add(FBox(FVector(-48, -49, 282), FVector(48, 49, 318)));
    const FVector Half(500, 125, 125);
    TestTrue(TEXT("Straight through it"), Keep.Hits(FVector(-2000, 0, 300), FVector(2000, 0, 300), FQuat::Identity, Half));
    TestTrue(TEXT("32 cm over the box it hovers on (through the drone)"), Keep.Hits(FVector(-2000, 0, 300 - 18 + 125), FVector(2000, 0, 300 - 18 + 125), FQuat::Identity, Half));
    TestFalse(TEXT("Over it with a hand's width to spare"), Keep.Hits(FVector(-2000, 0, 318 + 125 + 10), FVector(2000, 0, 318 + 125 + 10), FQuat::Identity, Half));
    TestFalse(TEXT("Past its side"), Keep.Hits(FVector(-2000, 49 + 125 + 10, 300), FVector(2000, 49 + 125 + 10, 300), FQuat::Identity, Half));
    TestFalse(TEXT("Short of it"), Keep.Hits(FVector(-2000, 0, 300), FVector(-48 - 500 - 10, 0, 300), FQuat::Identity, Half));
    // Turned a quarter the long side lies across: the same pass at the side now meets it.
    const FQuat Quarter(FVector::UpVector, UE_DOUBLE_HALF_PI);
    TestTrue(TEXT("Turned a quarter, its long side reaches it"), Keep.Hits(FVector(-2000, 49 + 125 + 10, 300), FVector(2000, 49 + 125 + 10, 300), Quarter, Half));
    TestTrue(TEXT("Standing in it (no sweep)"), Keep.Hits(FVector(400, 0, 300), FVector(400, 0, 300), FQuat::Identity, Half));
    // A load's route yet to fly and a station: the route passes through it or not (its drones' column above counts).
    const TArray<FVector> Route = {FVector(-3000, 0, 100), FVector(0, 0, 100), FVector(0, 3000, 100)};
    TestTrue(TEXT("A station in the column over the route"), FSCCargoKeepOut::RoutePasses(Route, FQuat::Identity, Half, 140, FVector(-1000, 0, 100 + 125 + 100), 50));
    TestFalse(TEXT("A station above the column"), FSCCargoKeepOut::RoutePasses(Route, FQuat::Identity, Half, 140, FVector(-1000, 0, 100 + 125 + 140 + 60), 50));
    TestTrue(TEXT("A station beside the second leg, within the box's long side"), FSCCargoKeepOut::RoutePasses(Route, FQuat::Identity, Half, 140, FVector(400, 2000, 100), 50));
    int32 Last = -1;
    TestTrue(TEXT("The last leg that passes a station on the first leg: the first"), FSCCargoKeepOut::RoutePasses(Route, FQuat::Identity, Half, 140, FVector(-1000, 0, 100), 50, &Last) && Last == 0);
    TestFalse(TEXT("A station beyond the route's end"), FSCCargoKeepOut::RoutePasses(Route, FQuat::Identity, Half, 140, FVector(0, 3000 + 125 + 60, 100), 50));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDroneLaunchTest, "SCBridge.Cargo.DroneLaunch", TugTestFlags)
bool FSCCargoDroneLaunchTest::RunTest(const FString&)
{
    // A dock with two bays 61 cm apart, one over the other; the drones' hulls have a radius of 69 cm, so two leaving
    // together overlap. Each flies bay -> exit (beside it) -> departure (above) -> its place over a box, 3 m apart.
    // Each decides for itself from the other's place and velocity: it leaves its bay when its way out is free.
    const double R = 69, Keep = FSCCargoDroneSteer::Keep(R, R), Dt = 1.0 / 60, Speed = 1000, Accel = 1200;
    struct FDrone
    {
        TArray<FVector> Path;
        double Dist = 0, V = 0;
        bool bDocked = true;
        FVector At(double S) const
        {
            for (int32 I = 0; I + 1 < Path.Num(); ++I)
            {
                const double L = FVector::Dist(Path[I], Path[I + 1]);
                if (S <= L) return FMath::Lerp(Path[I], Path[I + 1], S / L);
                S -= L;
            }
            return Path.Last();
        }
        double Length() const
        {
            double L = 0;
            for (int32 I = 0; I + 1 < Path.Num(); ++I) L += FVector::Dist(Path[I], Path[I + 1]);
            return L;
        }
    };
    FDrone Drones[2];
    Drones[0].Path = {FVector(0, 0, 23), FVector(100, 0, 23), FVector(100, 0, 250), FVector(2000, -300, 400)};
    Drones[1].Path = {FVector(0, 0, 84), FVector(100, 0, 84), FVector(100, 0, 311), FVector(2000, 0, 400)};
    TestTrue(TEXT("The bays are nearer than the hulls allow"), FVector::Dist(Drones[0].Path[0], Drones[1].Path[0]) < 2 * R);
    double Nearest = 1e9, SecondLeft = -1;
    for (int32 Tick = 0; Tick < 1200; ++Tick)
    {
        for (int32 I = 0; I < 2; ++I)
        {
            FDrone& D = Drones[I];
            const FDrone& O = Drones[1 - I];
            TArray<FSCCargoDroneSteer::FOther> Others;
            if (!O.bDocked)
            {
                FSCCargoDroneSteer::FOther& Other = Others.AddDefaulted_GetRef();
                Other.P = O.At(O.Dist);
                Other.V = (O.At(O.Dist + 1) - O.At(O.Dist)) * O.V;
                Other.Radius = R;
            }
            if (D.bDocked)
            {
                // Its way out of the dock: bay, exit, departure.
                const TArray<FVector> Out = {D.Path[0], D.Path[1], D.Path[2]};
                const double Way = FVector::Dist(Out[0], Out[1]) + FVector::Dist(Out[1], Out[2]);
                if (FSCCargoDroneSteer::FreeAlong(Out, R, Others, 0.5) < Way - 1e-6) continue;
                D.bDocked = false;
                if (I == 1) SecondLeft = Tick * Dt;
            }
            const double Left = D.Length() - D.Dist;
            D.V = FMath::Min(FMath::Min(D.V + Accel * Dt, Speed), FMath::Sqrt(2 * Accel * FMath::Max(Left, 0.0)));
            D.Dist = FMath::Min(D.Length(), D.Dist + D.V * Dt);
        }
        if (!Drones[0].bDocked && !Drones[1].bDocked) Nearest = FMath::Min(Nearest, FVector::Dist(Drones[0].At(Drones[0].Dist), Drones[1].At(Drones[1].Dist)));
    }
    TestTrue(TEXT("The first leaves at once"), !Drones[0].bDocked);
    TestTrue(FString::Printf(TEXT("The second waits in its bay for the first to clear its way out (left after %.2f s)"), SecondLeft), SecondLeft > 0.2 && SecondLeft < 5);
    TestTrue(FString::Printf(TEXT("In the air they are never nearer than their hulls (%.0f cm, hulls %.0f)"), Nearest, 2 * R), Nearest >= 2 * R);
    TestTrue(TEXT("Both arrive"), Drones[0].Dist >= Drones[0].Length() - 1 && Drones[1].Dist >= Drones[1].Length() - 1);
    // A way with nobody near is free to its end; with a drone standing on it, free up to the kept distance from it.
    const TArray<FVector> Way = {FVector(0, 0, 0), FVector(1000, 0, 0)};
    TestEqual(TEXT("Free to its end"), FSCCargoDroneSteer::FreeAlong(Way, R, TConstArrayView<FSCCargoDroneSteer::FOther>(), 1), 1000.0, 1e-6);
    FSCCargoDroneSteer::FOther Standing;
    Standing.P = FVector(600, 0, 0);
    Standing.Radius = R;
    const double Free = FSCCargoDroneSteer::FreeAlong(Way, R, MakeArrayView(&Standing, 1), 1);
    TestTrue(FString::Printf(TEXT("Free up to the kept distance from a drone on it (%.0f of %.0f cm)"), Free, 600 - Keep), Free <= 600 - Keep + 1e-6 && Free > 600 - Keep - R / 2);
    // One coming along the path is in the way for as far as it flies in the lead.
    Standing.V = FVector(-400, 0, 0);
    TestTrue(TEXT("A drone coming is in the way sooner"), FSCCargoDroneSteer::FreeAlong(Way, R, MakeArrayView(&Standing, 1), 1) < Free - 300);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDroneHoldTest, "SCBridge.Cargo.DroneHold", TugTestFlags)
bool FSCCargoDroneHoldTest::RunTest(const FString&)
{
    // A drone's flight out held short of its station (a load has yet to fly through there): 19 m, held at 14 m. A
    // flight this short is still gaining speed when it has to brake; it never goes beyond the point it is held at,
    // comes to rest there, and flies on to its end once it is let go. Held at a point it is already past (a load's
    // new route), it stays where it is.
    FSCCargoFlight F;
    F.Start(FSCCargoRoute::Spline({FVector(0, 0, 0), FVector(100, 0, 0), FVector(100, 0, 180), FVector(1900, 0, 300)}, 50), 1000, 1200, 3000);
    const double Dt = 1.0 / 60, Hold = 1400;
    TestTrue(TEXT("The flight is longer than where it is held"), F.Length() > Hold + 400);
    double Furthest = 0;
    F.HoldAt = Hold;
    for (int32 I = 0; I < 600; ++I)
    {
        F.Step(Dt);
        Furthest = FMath::Max(Furthest, F.Dist);
    }
    TestTrue(FString::Printf(TEXT("Never beyond where it is held (%.1f of %.0f cm)"), Furthest, Hold), Furthest <= Hold + 1e-6);
    TestTrue(FString::Printf(TEXT("At rest there (%.1f cm, %.1f cm/s)"), F.Dist, F.Speed), FMath::IsNearlyEqual(F.Dist, Hold, 1.0) && F.Speed < 1 && !F.Done());
    F.HoldAt = 700; // behind it now
    for (int32 I = 0; I < 60; ++I) F.Step(Dt);
    TestTrue(TEXT("Held behind where it is: it stays"), FMath::IsNearlyEqual(F.Dist, Hold, 1.0));
    F.HoldAt = -1;
    for (int32 I = 0; I < 600; ++I) F.Step(Dt);
    TestTrue(TEXT("Let go: on to its end"), F.Done());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDroneEvadeTest, "SCBridge.Cargo.DroneEvade", TugTestFlags)
bool FSCCargoDroneEvadeTest::RunTest(const FString&)
{
    // A 10 x 2.5 x 2.5 m box flies at 10 m/s along x through the place a drone hovers at. The drone steps out of its
    // way by its own decision (at no more than twice its speed limit a step) and is never in it; one beside the
    // box's path stays where it is.
    const FVector Hull(48, 49, 18), BoxHalf(500, 125, 125);
    const double Dt = 1.0 / 60, MaxStep = 2 * 1000 * Dt, Lead = 1200; // the drone's acceleration (cm/s^2)
    auto Overlaps = [&](const FVector& Drone, const FVector& Box) {
        return FMath::Abs(Drone.X - Box.X) < Hull.X + BoxHalf.X && FMath::Abs(Drone.Y - Box.Y) < Hull.Y + BoxHalf.Y && FMath::Abs(Drone.Z - Box.Z) < Hull.Z + BoxHalf.Z;
    };
    for (const FVector& Start : {FVector(0, 30, 300), FVector(0, -100, 320), FVector(0, 0, 400)})
    {
        FVector Drone = Start;
        bool bHit = false;
        double Moved = 0;
        for (int32 I = 0; I < 600; ++I)
        {
            FSCCargoDroneSteer::FLoad Load;
            Load.Pose = FTransform(FVector(-4000 + 1000 * I * Dt, 0, 300));
            Load.Half = BoxHalf;
            Load.V = FVector(1000, 0, 0);
            // It wants to be where it started: each step towards there, as far as it is out of the load's way.
            const FVector Want = FSCCargoDroneSteer::Evade(Drone + (Start - Drone).GetClampedToMaxSize(MaxStep), Hull, MakeArrayView(&Load, 1), Lead);
            Drone += (Want - Drone).GetClampedToMaxSize(MaxStep);
            Moved = FMath::Max(Moved, FVector::Dist(Drone, Start));
            bHit |= Overlaps(Drone, Load.Pose.GetLocation());
        }
        TestFalse(FString::Printf(TEXT("From %s: never in the box"), *Start.ToCompactString()), bHit);
        TestTrue(FString::Printf(TEXT("From %s: it moved out of the way (%.0f cm) and came back"), *Start.ToCompactString(), Moved), Moved > 40 && FVector::Dist(Drone, Start) < 1);
    }
    // A load flying askew to its own sides (its route bends, it keeps its heading) comes across towards the side the
    // drone is on: the drone steps further out on that side, by as much as the load comes across while it passes,
    // and not through the load's path to its far side.
    {
        const FVector Start(0, 180, 300), Velocity(900, 436, 0);
        FVector Drone = Start;
        bool bHit = false;
        double Out = 0, Across = 0;
        for (int32 I = 0; I < 600; ++I)
        {
            FSCCargoDroneSteer::FLoad Load;
            Load.Pose = FTransform(FVector(-2700, -1308, 300) + Velocity * (I * Dt));
            Load.Half = BoxHalf;
            Load.V = Velocity;
            const FVector Want = FSCCargoDroneSteer::Evade(Drone + (Start - Drone).GetClampedToMaxSize(MaxStep), Hull, MakeArrayView(&Load, 1), Lead);
            Drone += (Want - Drone).GetClampedToMaxSize(MaxStep);
            Out = FMath::Max(Out, Drone.Y - Start.Y);
            Across = FMath::Min(Across, Drone.Y - Start.Y);
            bHit |= Overlaps(Drone, Load.Pose.GetLocation());
        }
        TestFalse(TEXT("A load coming across: never in it"), bHit);
        TestTrue(FString::Printf(TEXT("A load coming across: out on its own side (%.0f cm out, %.0f cm the other way)"), Out, -Across), Out > 100 && Out < 800 && Across > -1);
        TestTrue(TEXT("A load coming across: back when it has passed"), FVector::Dist(Drone, Start) < 1);
    }
    FSCCargoDroneSteer::FLoad Passing;
    Passing.Pose = FTransform(FVector(-800, 0, 300));
    Passing.Half = BoxHalf;
    Passing.V = FVector(1000, 0, 0);
    // Still far off (it takes the drone under a second to step aside): left to come nearer.
    FSCCargoDroneSteer::FLoad Far = Passing;
    Far.Pose = FTransform(FVector(-4000, 0, 300));
    TestTrue(TEXT("A load 35 m off: not yet"), FSCCargoDroneSteer::Evade(FVector(0, 30, 300), Hull, MakeArrayView(&Far, 1), Lead).Equals(FVector(0, 30, 300)));
    const FVector Beside(0, 125 + 49 + 20, 300);
    TestTrue(TEXT("Beside its path: left where it is"), FSCCargoDroneSteer::Evade(Beside, Hull, MakeArrayView(&Passing, 1), Lead).Equals(Beside));
    // Out sideways or over, not ahead of the box (where it would be pushed along).
    const FVector Out = FSCCargoDroneSteer::Evade(FVector(0, 30, 300), Hull, MakeArrayView(&Passing, 1), Lead);
    TestTrue(FString::Printf(TEXT("Out across its path, level (%s)"), *Out.ToCompactString()), FMath::IsNearlyZero(Out.X) && FMath::Abs(Out.Y) > 125 + 49 && FMath::IsNearlyEqual(Out.Z, 300.0));
    // Over the box's top, nearer to it than to its sides: still out sideways (its drones fly over it).
    const FVector Over = FSCCargoDroneSteer::Evade(FVector(0, 30, 420), Hull, MakeArrayView(&Passing, 1), Lead);
    TestTrue(FString::Printf(TEXT("From over it: out sideways, not up (%s)"), *Over.ToCompactString()), FMath::IsNearlyEqual(Over.Z, 420.0) && FMath::Abs(Over.Y) > 125 + 49);
    // A box that stands, turned a quarter: out of it by the nearest side.
    Passing.V = FVector::ZeroVector;
    Passing.Pose = FTransform(FQuat(FVector::UpVector, UE_DOUBLE_HALF_PI), FVector(0, 0, 300));
    const FVector Stand = FSCCargoDroneSteer::Evade(FVector(100, 0, 300), Hull, MakeArrayView(&Passing, 1), Lead);
    TestTrue(FString::Printf(TEXT("Out of a standing box turned a quarter (%s)"), *Stand.ToCompactString()), Stand.X > 125 + 48);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDroneApartTest, "SCBridge.Cargo.DroneApart", TugTestFlags)
bool FSCCargoDroneApartTest::RunTest(const FString&)
{
    // Two teammates leave one dock (bays 61 cm apart) for places 3 m apart over a box. The first flies its route (up
    // out of the dock, across, round a bend); the second flies beside it: from its own bay's way out to its own
    // place, the kept distance to the side all the way between, and as smoothly.
    const double R = 69, Keep = FSCCargoDroneSteer::Keep(R, R), Ramp = 5 * Keep;
    const TArray<FVector> First = FSCCargoRoute::Spline({FVector(0, 0, 250), FVector(0, 0, 600), FVector(3000, 0, 600), FVector(6000, 2000, 400)}, 50);
    const FVector From(0, 0, 311), To = First.Last() + FVector(-166, 250, 0); // its place: 3 m along the box, across the way in
    auto Nearest = [&](const TArray<FVector>& Path, double Skip) {
        double Length = 0, Out = 1e9;
        TArray<double> S = {0};
        for (int32 I = 1; I < Path.Num(); ++I) S.Add(Length += FVector::Dist(Path[I - 1], Path[I]));
        for (int32 I = 0; I < Path.Num(); ++I)
        {
            if (S[I] < Skip || Length - S[I] < Skip) continue;
            for (int32 K = 0; K + 1 < First.Num(); ++K) Out = FMath::Min(Out, FMath::PointDistToSegment(Path[I], First[K], First[K + 1]));
        }
        return Out;
    };
    auto Sharpest = [](const TArray<FVector>& Path) {
        double Out = 0;
        for (int32 I = 1; I + 1 < Path.Num(); ++I)
            Out = FMath::Max(Out, FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct((Path[I] - Path[I - 1]).GetSafeNormal(), (Path[I + 1] - Path[I]).GetSafeNormal()), -1.0, 1.0))));
        return Out;
    };
    auto Length = [](const TArray<FVector>& Path) {
        double Out = 0;
        for (int32 I = 1; I < Path.Num(); ++I) Out += FVector::Dist(Path[I - 1], Path[I]);
        return Out;
    };
    const TArray<FVector> Second = FSCCargoDroneSteer::Beside(First, From, To, Keep, Ramp);
    TestTrue(TEXT("From its own way out to its own place"), Second.Num() == First.Num() && Second[0].Equals(From, 1e-6) && Second.Last().Equals(To, 1e-6));
    const double Apart = Nearest(Second, Ramp + 50);
    TestTrue(FString::Printf(TEXT("The kept distance from the first's route between the ends (%.0f of %.0f cm)"), Apart, Keep), Apart >= Keep - 2);
    TestTrue(FString::Printf(TEXT("As smooth as the first's (sharpest turn %.1f against %.1f degrees a sample)"), Sharpest(Second), Sharpest(First)), Sharpest(Second) < Sharpest(First) + 8);
    TestTrue(FString::Printf(TEXT("Hardly longer (%.1f against %.1f m)"), Length(Second) / 100, Length(First) / 100), Length(Second) < Length(First) * 1.05);
    // On the side its place lies on: it never crosses the first's route (a place on the other side: the other side).
    const TArray<FVector> Other = FSCCargoDroneSteer::Beside(First, From, First.Last() + FVector(166, -250, 0), Keep, Ramp);
    const int32 Mid = First.Num() / 2;
    TestTrue(TEXT("To the side of its place"), FVector::DotProduct(Second[Mid] - First[Mid], To - First.Last()) > 0 && FVector::DotProduct(Other[Mid] - First[Mid], To - First.Last()) < 0);
    // A third teammate: twice as far to the side, so the kept distance from the second as well.
    const TArray<FVector> Third = FSCCargoDroneSteer::Beside(First, FVector(0, 0, 372), First.Last() + FVector(-332, 500, 0), 2 * Keep, Ramp);
    double Between = 1e9;
    for (int32 I = 0; I < Third.Num(); ++I)
        if (FVector::Dist(Third[I], Third[0]) > 2 * Ramp && FVector::Dist(Third[I], Third.Last()) > 2 * Ramp)
            for (int32 K = 0; K + 1 < Second.Num(); ++K) Between = FMath::Min(Between, FMath::PointDistToSegment(Third[I], Second[K], Second[K + 1]));
    TestTrue(FString::Printf(TEXT("The third keeps the distance from the second (%.0f cm)"), Between), Between >= Keep - 2);
    TestTrue(TEXT("Nothing to fly beside: no route"), FSCCargoDroneSteer::Beside(TArray<FVector>{From}, From, To, Keep, Ramp).IsEmpty());
    return true;
}
#endif
