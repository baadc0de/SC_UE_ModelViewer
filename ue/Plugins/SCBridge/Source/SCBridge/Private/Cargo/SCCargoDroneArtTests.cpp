// Automation tests of the drone art policies (WP-K4a, SCBridge.Cargo.Drone* / DockFootprint / FxNames): where a drone
// faces, which clip it plays from its motion, how many cells a dock mesh takes and the turns it may try, the
// drone_art.json shape, the effect -> system naming rule the builder and the runtime share, and (round 4) a drone's
// attitude: tilting to move, overshooting and rocking back, each drone its own (its onsets and kicks too), within its
// envelope; (round 5) facing the box it holds, and the grip block. Every number here is
// synthetic (round sizes, speeds and angles), not game data.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Cargo/SCCargoDroneArt.h"
#include "SCFxNames.h"
#include "Cargo/SCCargoTug.h"

namespace
{
constexpr EAutomationTestFlags ArtTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

FSCCargoDroneArt TestArt()
{
    FSCCargoDroneArt A;
    A.HoverSpeedCmS = 20;
    A.TurnClipRateDegS = 20;
    A.BlendS = 0.25f;
    A.Mesh = TEXT("/Game/T/M.M");
    return A;
}
/** Every clip 1 s long, idle clips 10 s; End clips exist. */
float Lengths(ESCDroneMotion M, ESCDroneClipPhase)
{
    return M == ESCDroneMotion::Idle ? 10.f : 1.f;
}
FSCCargoDroneMotion::FInput Moving(const FVector& V, float YawRate = 0, bool bScan = false)
{
    FSCCargoDroneMotion::FInput In;
    In.LocalVelocity = V;
    In.YawRateDegS = YawRate;
    In.bScanning = bScan;
    return In;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDroneHeadingTest, "SCBridge.Cargo.DroneHeading", ArtTestFlags)
bool FSCCargoDroneHeadingTest::RunTest(const FString&)
{
    // Flying: it faces its horizontal travel.
    TestEqual(TEXT("faces travel along +Y"), FSCCargoHeading::Target(0, FVector(0, 300, 0), 20, {}), 90.f);
    TestEqual(TEXT("faces travel along -X"), FMath::Abs(FSCCargoHeading::Target(10, FVector(-300, 0, 50), 20, {})), 180.f);
    // Hovering (or going straight up): it keeps its heading.
    TestEqual(TEXT("hovering keeps its heading"), FSCCargoHeading::Target(37, FVector(5, 5, 0), 20, {}), 37.f);
    TestEqual(TEXT("rising keeps its heading"), FSCCargoHeading::Target(-60, FVector(0, 0, 400), 20, {}), -60.f);
    // In its dock's corridor: its bay's yaw, whichever way it moves (out forwards, in backwards).
    TestEqual(TEXT("launching faces out of its bay"), FSCCargoHeading::Target(0, FVector(300, 0, 0), 20, 45.f), 45.f);
    TestEqual(TEXT("docking backs in"), FSCCargoHeading::Target(0, FVector(-300, 0, 0), 20, 45.f), 45.f);
    // Its load's travel when it carries as a team: the caller passes the load's velocity, the rule is the same.
    TestEqual(TEXT("a team faces its load's travel"), FSCCargoHeading::Target(0, FVector(0, -150, 0), 20, {}), -90.f);
    TestEqual(TEXT("a load at rest: kept"), FSCCargoHeading::Target(12, FVector::ZeroVector, 20, {}), 12.f);
    // Holding its box in front of it or beside it (round 5): it faces the box whichever way it flies: pushing it ahead,
    // pulling it in reverse, strafing; its bay's yaw still first in its dock's corridor.
    TestEqual(TEXT("pushing: faces the box ahead of it"), FSCCargoHeading::Target(0, FVector(300, 0, 0), 20, {}, 0.f), 0.f);
    TestEqual(TEXT("pulling in reverse: faces the box behind it"), FMath::Abs(FSCCargoHeading::Target(0, FVector(300, 0, 0), 20, {}, 180.f)), 180.f);
    TestEqual(TEXT("strafing: faces the box beside it"), FSCCargoHeading::Target(0, FVector(300, 0, 0), 20, {}, -90.f), -90.f);
    TestEqual(TEXT("at rest too"), FSCCargoHeading::Target(30, FVector::ZeroVector, 20, {}, 120.f), 120.f);
    TestEqual(TEXT("its bay's yaw first"), FSCCargoHeading::Target(0, FVector(300, 0, 0), 20, 45.f, 180.f), 45.f);
    // The turn: limited rate, the short way round.
    TestEqual(TEXT("limited rate"), FSCCargoHeading::Turn(0, 90, 90, 0.5f), 45.f);
    TestEqual(TEXT("reaches and holds"), FSCCargoHeading::Turn(80, 90, 90, 0.5f), 90.f);
    TestEqual(TEXT("short way across 180"), FSCCargoHeading::Turn(170, -170, 90, 0.1f), 179.f, 1e-3f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDroneMotionTest, "SCBridge.Cargo.DroneMotion", ArtTestFlags)
bool FSCCargoDroneMotionTest::RunTest(const FString&)
{
    const FSCCargoDroneArt Art = TestArt();
    using M = ESCDroneMotion;
    using P = ESCDroneClipPhase;
    // What the motion asks for, in the drone's frame.
    TestEqual(TEXT("forward"), int32(FSCCargoDroneMotion::Want(Moving(FVector(200, 30, 0)), M::Idle, Art)), int32(M::Front));
    TestEqual(TEXT("back"), int32(FSCCargoDroneMotion::Want(Moving(FVector(-200, 30, 0)), M::Idle, Art)), int32(M::Back));
    TestEqual(TEXT("right is +Y"), int32(FSCCargoDroneMotion::Want(Moving(FVector(10, 200, 0)), M::Idle, Art)), int32(M::Right));
    TestEqual(TEXT("left"), int32(FSCCargoDroneMotion::Want(Moving(FVector(10, -200, 0)), M::Idle, Art)), int32(M::Left));
    TestEqual(TEXT("up"), int32(FSCCargoDroneMotion::Want(Moving(FVector(10, 0, 200)), M::Idle, Art)), int32(M::Up));
    TestEqual(TEXT("down"), int32(FSCCargoDroneMotion::Want(Moving(FVector(10, 0, -200)), M::Idle, Art)), int32(M::Down));
    TestEqual(TEXT("hovering idles"), int32(FSCCargoDroneMotion::Want(Moving(FVector(5, 5, 0)), M::Idle, Art)), int32(M::Idle));
    TestEqual(TEXT("turning in place"), int32(FSCCargoDroneMotion::Want(Moving(FVector(5, 0, 0), 60), M::Idle, Art)), int32(M::TurnRight));
    TestEqual(TEXT("turning left in place"), int32(FSCCargoDroneMotion::Want(Moving(FVector::ZeroVector, -60), M::Idle, Art)), int32(M::TurnLeft));
    TestEqual(TEXT("scanning wins"), int32(FSCCargoDroneMotion::Want(Moving(FVector(200, 0, 0), 0, true), M::Front, Art)), int32(M::Scanning));
    // Hysteresis: the axis held until another is 25 % larger; moving from 125 % of the hover speed, until 75 %.
    TestEqual(TEXT("axis held"), int32(FSCCargoDroneMotion::Want(Moving(FVector(100, 110, 0)), M::Front, Art)), int32(M::Front));
    TestEqual(TEXT("axis taken over"), int32(FSCCargoDroneMotion::Want(Moving(FVector(100, 130, 0)), M::Front, Art)), int32(M::Right));
    TestEqual(TEXT("still moving at 80 %"), int32(FSCCargoDroneMotion::Want(Moving(FVector(16, 0, 0)), M::Front, Art)), int32(M::Front));
    TestEqual(TEXT("not yet moving at 120 %"), int32(FSCCargoDroneMotion::Want(Moving(FVector(24, 0, 0)), M::Idle, Art)), int32(M::Idle));
    TestEqual(TEXT("stopped at 70 %"), int32(FSCCargoDroneMotion::Want(Moving(FVector(14, 0, 0)), M::Front, Art)), int32(M::Idle));

    // The sequence: start -> loop -> end -> idle.
    FSCCargoDroneMotion S;
    const auto Fwd = Moving(FVector(200, 0, 0));
    TestTrue(TEXT("entering a direction changes the clip"), S.Step(Fwd, 0.1f, Art, Lengths));
    TestTrue(TEXT("its start clip"), S.Motion == M::Front && S.Phase == P::Start && !S.Loops());
    TestFalse(TEXT("the start plays out"), S.Step(Fwd, 0.5f, Art, Lengths));
    TestTrue(TEXT("then its loop"), S.Step(Fwd, 0.6f, Art, Lengths) && S.Phase == P::Loop && S.Loops());
    TestFalse(TEXT("the loop holds"), S.Step(Fwd, 3.f, Art, Lengths));
    const auto Hover = Moving(FVector::ZeroVector);
    TestTrue(TEXT("leaving it: its end clip"), S.Step(Hover, 0.1f, Art, Lengths) && S.Motion == M::Front && S.Phase == P::End);
    TestFalse(TEXT("the end plays out"), S.Step(Hover, 0.5f, Art, Lengths));
    const int32 IdleBefore = S.IdleIndex;
    TestTrue(TEXT("then idle"), S.Step(Hover, 0.6f, Art, Lengths) && S.Motion == M::Idle && S.IdleIndex == IdleBefore + 1);
    TestTrue(TEXT("an idle clip plays out, then the next"), S.Step(Hover, 10.5f, Art, Lengths) && S.Motion == M::Idle && S.IdleIndex == IdleBefore + 2);
    // Another direction while moving: straight into its start (no end clip in between).
    FSCCargoDroneMotion T;
    T.Step(Fwd, 0.1f, Art, Lengths);
    TestFalse(TEXT("a new direction waits for the axis dwell"), T.Step(Moving(FVector(0, 0, 300)), 0.1f, Art, Lengths));
    TestTrue(TEXT("a new direction crossfades into its start"), T.Step(Moving(FVector(0, 0, 300)), 0.15f, Art, Lengths) && T.Motion == M::Up && T.Phase == P::Start);
    // Coming back while leaving: its start again.
    FSCCargoDroneMotion U;
    U.Step(Fwd, 0.1f, Art, Lengths);
    U.Step(Hover, 0.1f, Art, Lengths);
    TestTrue(TEXT("back to it while it ends: start again"), U.Step(Fwd, 0.1f, Art, Lengths) && U.Motion == M::Front && U.Phase == P::Start);
    // Scanning from idle: at once, looping.
    FSCCargoDroneMotion V;
    TestTrue(TEXT("scanning at once"), V.Step(Moving(FVector::ZeroVector, 0, true), 0.05f, Art, Lengths) && V.Motion == M::Scanning && V.Loops());
    // A turn clip holds a moment (no flicker) before idling.
    FSCCargoDroneMotion W;
    W.Step(Moving(FVector::ZeroVector, 60), 0.05f, Art, Lengths);
    TestTrue(TEXT("turning in place: the turn clip"), W.Motion == M::TurnRight);
    TestFalse(TEXT("held a moment"), W.Step(Hover, 0.05f, Art, Lengths));
    TestTrue(TEXT("then idle"), W.Step(Hover, 0.5f, Art, Lengths) && W.Motion == M::Idle);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDockFootprintTest, "SCBridge.Cargo.DockFootprint", ArtTestFlags)
bool FSCCargoDockFootprintTest::RunTest(const FString&)
{
    const FVector Cell(125, 125, 125);
    // A two-cell dock: 250 x 125 x 125 cm.
    const FIntVector Two = FSCCargoDockFootprint::Cells(FVector(250, 125, 125), Cell);
    TestTrue(TEXT("two cells long"), Two == FIntVector(2, 1, 1));
    TestEqual(TEXT("fills them at native size"), FSCCargoDockFootprint::Scale(FVector(250, 125, 125), Two, Cell), 1.0);
    TestTrue(TEXT("a long dock turns by halves"), FSCCargoDockFootprint::Turns(Two) == TArray<int32>({0, 2}));
    // A one-cell dock (and the engine cube stand-in, 100 cm): one cell, quarter turns, scaled up to fill it.
    const FIntVector One = FSCCargoDockFootprint::Cells(FVector(100, 100, 100), Cell);
    TestTrue(TEXT("at least one cell"), One == FIntVector(1, 1, 1));
    TestEqual(TEXT("scaled to the cell"), FSCCargoDockFootprint::Scale(FVector(100, 100, 100), One, Cell), 1.25);
    TestEqual(TEXT("a square dock turns by quarters"), FSCCargoDockFootprint::Turns(One).Num(), 4);
    // Rounding: a mesh a little off the cell multiple; a tiny one still takes a cell.
    TestTrue(TEXT("rounds to the nearest cell count"), FSCCargoDockFootprint::Cells(FVector(240, 130, 60), Cell) == FIntVector(2, 1, 1));
    TestTrue(TEXT("tiny: one cell"), FSCCargoDockFootprint::Cells(FVector(10, 10, 10), Cell) == FIntVector(1, 1, 1));
    // The uniform scale keeps the plan inside its cells (the smaller ratio).
    TestEqual(TEXT("uniform, fits both plan axes"), FSCCargoDockFootprint::Scale(FVector(240, 130, 60), FIntVector(2, 1, 1), Cell), 125.0 / 130.0, 1e-9);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDroneArtParseTest, "SCBridge.Cargo.DroneArtParse", ArtTestFlags)
bool FSCCargoDroneArtParseTest::RunTest(const FString&)
{
    FSCCargoDroneArt A;
    FString Error;
    TestTrue(TEXT("parses"), FSCCargoDroneArt::Parse(TEXT(R"({"heading":{"turn_rate_deg_s":45,"hover_speed_cm_s":30},
        "art":{"mesh":"/Game/X/Mesh/SK_X","yaw_offset_deg":-90,"centre_bone":"b","beam_bone":"c","beam_axis":[0,0,-2],"hidden_slots":["S"],
        "clips":{"idle":["/Game/X/A/I1","/Game/X/A/I2.I2"],"front":{"start":"/Game/X/A/FS","loop":"/Game/X/A/FL"},"turn_left":"/Game/X/A/TL"}}})"), A, Error));
    TestTrue(TEXT("heading"), A.TurnRateDegS == 45.f && A.HoverSpeedCmS == 30.f);
    TestEqual(TEXT("object path from a package path"), A.Mesh, FString(TEXT("/Game/X/Mesh/SK_X.SK_X")));
    TestEqual(TEXT("bones"), A.CentreBone, FName(TEXT("b")));
    TestEqual(TEXT("beam axis normalised"), A.BeamAxis, FVector(0, 0, -1));
    TestEqual(TEXT("idle wraps"), A.Clip(ESCDroneMotion::Idle, ESCDroneClipPhase::Loop, 3), FString(TEXT("/Game/X/A/I2.I2")));
    TestEqual(TEXT("a direction's phase"), A.Clip(ESCDroneMotion::Front, ESCDroneClipPhase::Start), FString(TEXT("/Game/X/A/FS.FS")));
    TestTrue(TEXT("a phase it lacks is empty"), A.Clip(ESCDroneMotion::Front, ESCDroneClipPhase::End).IsEmpty());
    TestEqual(TEXT("assets to cook"), A.AssetPaths().Num(), 6);
    FSCCargoDroneArt B;
    TestTrue(TEXT("no art block: the stand-in"), FSCCargoDroneArt::Parse(TEXT(R"({"heading":{"turn_rate_deg_s":60}})"), B, Error) && !B.HasArt());
    FSCCargoDroneArt C;
    TestFalse(TEXT("an art block needs its mesh"), FSCCargoDroneArt::Parse(TEXT(R"({"art":{}})"), C, Error));
    // The motion block applies without the art too; ranges are kept ordered, the clips' tilt scale is the art's.
    FSCCargoDroneArt D;
    TestTrue(TEXT("motion parses"), FSCCargoDroneArt::Parse(TEXT(R"({"motion":{"freq_min_hz":1.2,"freq_max_hz":0.8,"damp_min":0.3,"damp_max":0.5,"max_tilt_deg":7,"kick_rate_hz":2,"yaw_jitter_deg":3},
        "art":{"mesh":"/Game/X/SK","clip_tilt_scale":0.4}})"), D, Error));
    TestTrue(TEXT("its numbers"), D.Dynamics.FreqMinHz == 1.2f && D.Dynamics.FreqMaxHz == 1.2f && D.Dynamics.DampMin == 0.3f && D.Dynamics.DampMax == 0.5f && D.Dynamics.MaxTiltDeg == 7.f &&
                                      D.Dynamics.KickRateHz == 2.f && D.Dynamics.YawJitterDeg == 3.f && D.ClipTiltScale == 0.4f);
    TestTrue(TEXT("defaults without it"), B.Dynamics.MaxTiltDeg == FSCCargoDroneArt::FMotion().MaxTiltDeg && B.ClipTiltScale == 1.f);
    // The grip block and the load transfer (round 5): ranges ordered, weights kept non-negative.
    FSCCargoDroneArt E;
    TestTrue(TEXT("grip parses"), FSCCargoDroneArt::Parse(TEXT(R"({"motion":{"load_transfer":0.2},"grip":{"end_weight":3,"side_weight":-1,"over_weight":1,"interval_min_s":6,"interval_max_s":4,"spread":0.5,"max_repick_move_frac":0.25,"max_repick_move_min_cm":200,"max_repick_move_max_cm":100}})"), E, Error));
    TestTrue(TEXT("its numbers"), E.Dynamics.LoadTransfer == 0.2f && E.Grip.EndWeight == 3.f && E.Grip.SideWeight == 0.f && E.Grip.OverWeight == 1.f &&
                                      E.Grip.IntervalMinS == 6.f && E.Grip.IntervalMaxS == 6.f && E.Grip.Spread == 0.5f && E.Grip.MaxRepickMoveFrac == 0.25f &&
                                      E.Grip.MaxRepickMoveMinCm == 200.f && E.Grip.MaxRepickMoveMaxCm == 200.f);
    TestTrue(TEXT("grip defaults without it"), B.Grip.EndWeight == FSCCargoGripPick::FCfg().EndWeight && B.Dynamics.LoadTransfer == FSCCargoDroneArt::FMotion().LoadTransfer);
    return true;
}

namespace
{
// The dynamics under test, with a config of their own (the look's tuning is drone_art.json's): a clear lean.
FSCCargoDroneArt::FMotion TestMotion()
{
    FSCCargoDroneArt::FMotion M;
    M.FreqMinHz = 0.9f;
    M.FreqMaxHz = 1.5f;
    M.DampMin = 0.22f;
    M.DampMax = 0.4f;
    M.TiltGain = 0.35f;
    M.GainSpread = 0.25f;
    M.TiltLead = 1.5f;
    M.MaxTiltDeg = 9.f;
    M.KickRateHz = 0.6f;
    M.KickDegS = 12.f;
    M.OnsetMinS = M.OnsetMaxS = 0.f; // the bare spring (the onsets: DroneOnset)
    return M;
}
// A drone's pitch through a start, a cruise and a stop: 3 m/s^2 forward for 1 s, 2 s at speed, 3 m/s^2 back for 1 s,
// then 4 s at rest (its own corrections off: the bare response).
TArray<double> StartStop(uint32 Seed, const FSCCargoDroneArt::FMotion& Cfg, double Dt, FSCCargoAttitude* Out = nullptr)
{
    FSCCargoAttitude A;
    A.Init(Seed, Cfg);
    TArray<double> Pitch;
    for (double T = 0; T < 8; T += Dt)
    {
        const double Ax = T < 1 ? 300 : T < 3 ? 0 : T < 4 ? -300 : 0;
        A.Step(FVector(Ax, 0, 0), FVector2D::ZeroVector, Dt, Cfg, /*bKicks=*/false);
        Pitch.Add(A.Tilt.X);
    }
    if (Out) *Out = A;
    return Pitch;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDroneAttitudeTest, "SCBridge.Cargo.DroneAttitude", ArtTestFlags)
bool FSCCargoDroneAttitudeTest::RunTest(const FString&)
{
    // Moving is tilting: to go forward it pitches nose down, past the command at the start; braking, nose up; stopped,
    // it rocks back past level and settles in decaying swings.
    const FSCCargoDroneArt::FMotion Cfg = TestMotion();
    const double Dt = 1.0 / 60;
    FSCCargoAttitude End;
    const TArray<double> P = StartStop(1, Cfg, Dt, &End);
    const double Cmd = FSCCargoAttitude::CommandFor(FVector(300, 0, 0), Cfg).X * End.Gain; // its own share of the lean
    auto At = [&](double T) { return P[FMath::Clamp(FMath::RoundToInt32(T / Dt), 0, P.Num() - 1)]; };
    double Lowest = 0, Highest = 0, AfterStop = 0;
    for (int32 I = 0; I < P.Num(); ++I)
    {
        const double T = I * Dt;
        if (T < 1) Lowest = FMath::Min(Lowest, P[I]);
        if (T >= 3 && T < 4) Highest = FMath::Max(Highest, P[I]);
        if (T >= 4) AfterStop = FMath::Min(AfterStop, P[I]);
    }
    TestTrue(FString::Printf(TEXT("Accelerating forward it pitches nose down (command %.1f deg)"), Cmd), Cmd < 0 && At(0.9) < 0);
    TestTrue(FString::Printf(TEXT("... overshooting the command at the start (to %.1f deg)"), Lowest), Lowest < Cmd * 1.1);
    TestTrue(FString::Printf(TEXT("Braking it pitches nose up (to %.1f deg)"), Highest), Highest > -Cmd * 0.8);
    TestTrue(FString::Printf(TEXT("Stopped, it rocks back past level (to %.1f deg)"), AfterStop), AfterStop < -0.2);
    // Its swings after the stop decay: each extreme smaller than the one before.
    TArray<double> Peaks;
    for (int32 I = FMath::RoundToInt32(4 / Dt) + 1; I + 1 < P.Num(); ++I)
        if ((P[I] - P[I - 1]) * (P[I + 1] - P[I]) < 0) Peaks.Add(FMath::Abs(P[I]));
    bool bDecays = Peaks.Num() >= 2;
    for (int32 I = 1; I < Peaks.Num(); ++I) bDecays &= Peaks[I] < Peaks[I - 1] + 1e-9;
    TestTrue(FString::Printf(TEXT("... in decaying swings (%d of them)"), Peaks.Num()), bDecays);
    TestTrue(FString::Printf(TEXT("... and settles (%.2f deg after 4 s)"), P.Last()), FMath::Abs(P.Last()) < 0.3);
    // Any frame rate gives the same motion (sub-stepped).
    const TArray<double> Slow = StartStop(1, Cfg, 1.0 / 20);
    double Worst = 0;
    for (int32 I = 0; I < Slow.Num(); ++I) Worst = FMath::Max(Worst, FMath::Abs(Slow[I] - At(I / 20.0)));
    TestTrue(FString::Printf(TEXT("At 20 fps as at 60 (within %.2f deg)"), Worst), Worst < 2.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDroneIndividualTest, "SCBridge.Cargo.DroneIndividual", ArtTestFlags)
bool FSCCargoDroneIndividualTest::RunTest(const FString&)
{
    // Each drone its own: frequency and damping from its seed within the ranges; teammates through the same start and
    // stop do not move in lockstep.
    const FSCCargoDroneArt::FMotion Cfg = TestMotion();
    TSet<int32> Frequencies;
    bool bInRange = true;
    for (uint32 Seed = 1; Seed <= 50; ++Seed)
    {
        FSCCargoAttitude A;
        A.Init(Seed, Cfg);
        const double Hz = A.Omega / UE_DOUBLE_TWO_PI;
        bInRange &= Hz >= Cfg.FreqMinHz - 1e-6 && Hz <= Cfg.FreqMaxHz + 1e-6 && A.Zeta >= Cfg.DampMin - 1e-6 && A.Zeta <= Cfg.DampMax + 1e-6 &&
                    FMath::Abs(A.Gain - 1.0) <= Cfg.GainSpread + 1e-6;
        Frequencies.Add(FMath::RoundToInt32(Hz * 100));
    }
    TestTrue(TEXT("Frequencies, damping and lean shares within the ranges"), bInRange);
    TestTrue(FString::Printf(TEXT("... and spread over them (%d distinct of 50)"), Frequencies.Num()), Frequencies.Num() >= 25);
    const double Dt = 1.0 / 60;
    const TArray<double> A = StartStop(1, Cfg, Dt), B = StartStop(2, Cfg, Dt);
    double Apart = 0;
    for (int32 I = FMath::RoundToInt32(4 / Dt); I < A.Num(); ++I) Apart = FMath::Max(Apart, FMath::Abs(A[I] - B[I]));
    TestTrue(FString::Printf(TEXT("Two teammates after the same stop differ by up to %.2f deg"), Apart), Apart > 0.5);
    // Their own corrections: hovering still, a drone keeps moving a little, and differently from another.
    FSCCargoAttitude X, Y;
    X.Init(3, Cfg);
    Y.Init(4, Cfg);
    double Activity = 0, Differ = 0, Hunt = 0;
    for (double T = 0; T < 30; T += Dt)
    {
        X.Step(FVector::ZeroVector, FVector2D::ZeroVector, Dt, Cfg);
        Y.Step(FVector::ZeroVector, FVector2D::ZeroVector, Dt, Cfg);
        Activity = FMath::Max(Activity, X.Tilt.Size());
        Differ = FMath::Max(Differ, (X.Tilt - Y.Tilt).Size());
        Hunt = FMath::Max(Hunt, FMath::Abs(X.Offset.Z - Y.Offset.Z));
    }
    TestTrue(FString::Printf(TEXT("Hovering it corrects itself (up to %.1f deg)"), Activity), Activity > 0.5);
    TestTrue(FString::Printf(TEXT("... unlike its neighbour (up to %.1f deg apart, heights %.1f cm apart)"), Differ, Hunt), Differ > 0.5 && Hunt > 0.3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDroneAttitudeBoundsTest, "SCBridge.Cargo.DroneAttitudeBounds", ArtTestFlags)
bool FSCCargoDroneAttitudeBoundsTest::RunTest(const FString&)
{
    // Whatever it is asked (hard accelerations every way, gusts, its kicks), its look stays in the envelope the hull
    // covers: the tilt, the drift, the hunt; its yaw wanders within twice its jitter.
    const FSCCargoDroneArt::FMotion Cfg;
    const double Dt = 1.0 / 30;
    double Tilt = 0, Drift = 0, Hunt = 0, Yaw = 0;
    for (uint32 Seed = 1; Seed <= 8; ++Seed)
    {
        FSCCargoAttitude A;
        A.Init(Seed, Cfg);
        FRandomStream Rng(static_cast<int32>(Seed));
        FVector Accel = FVector::ZeroVector;
        for (double T = 0; T < 40; T += Dt)
        {
            if (Rng.FRand() < 0.05) Accel = FVector(Rng.FRandRange(-1, 1), Rng.FRandRange(-1, 1), Rng.FRandRange(-1, 1)) * 2000.0;
            A.Step(Accel, FVector2D(Rng.FRandRange(-3, 3), Rng.FRandRange(-3, 3)), Dt, Cfg);
            Tilt = FMath::Max(Tilt, A.Tilt.Size());
            Drift = FMath::Max(Drift, FVector2D(A.Offset.X, A.Offset.Y).Size());
            Hunt = FMath::Max(Hunt, FMath::Abs(A.Offset.Z));
            Yaw = FMath::Max(Yaw, FMath::Abs(A.Yaw));
        }
    }
    TestTrue(FString::Printf(TEXT("Tilt within %.0f deg (%.2f)"), Cfg.MaxTiltDeg, Tilt), Tilt <= Cfg.MaxTiltDeg + 1e-6);
    TestTrue(FString::Printf(TEXT("Drift within %.1f cm (%.2f)"), Cfg.DriftCm, Drift), Drift <= Cfg.DriftCm + 1e-6);
    TestTrue(FString::Printf(TEXT("Hunt within %.1f cm (%.2f)"), Cfg.HuntCm, Hunt), Hunt <= Cfg.HuntCm + 1e-6);
    TestTrue(FString::Printf(TEXT("Yaw within %.1f deg (%.2f)"), 2 * Cfg.YawJitterDeg, Yaw), Yaw <= 2 * Cfg.YawJitterDeg + 1e-6);
    // Docked (reset): level and still.
    FSCCargoAttitude R;
    R.Init(5, Cfg);
    R.Step(FVector(500, 0, 0), FVector2D::ZeroVector, 0.5, Cfg);
    R.Reset();
    TestTrue(TEXT("Reset: level and still"), R.Tilt.IsZero() && R.Rate.IsZero() && R.Rotator().IsNearlyZero());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoHullHangTest, "SCBridge.Cargo.HullHang", ArtTestFlags)
bool FSCCargoHullHangTest::RunTest(const FString&)
{
    // The stand-in shape: emitter 19 cm under its origin, hull bottom 20.15 cm: the record's 50 cm minimum stands.
    const double StandIn = FSCCargoTug::HullHangCm(-19, -20.15, 2);
    TestEqual(TEXT("stand-in hangs by its 1.15 cm reach and the gap"), StandIn, 3.15, 1e-9);
    TestEqual(TEXT("stand-in: the record's minimum, unchanged"), FSCCargoTug::ShortestCm(50, StandIn, 10), 60.0);
    // A drone whose hull reaches further below its emitter than the record's minimum: its hull decides.
    const double Deep = FSCCargoTug::HullHangCm(-29, -90, 2);
    TestEqual(TEXT("reach below the emitter plus the gap"), Deep, 63.0);
    TestEqual(TEXT("its hull hang plus the stretch"), FSCCargoTug::ShortestCm(50, Deep, 10), 73.0);
    // An emitter at or below the hull's bottom: only the gap.
    TestEqual(TEXT("emitter lowest: the gap"), FSCCargoTug::HullHangCm(-30, -20, 2), 2.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoHullAboveTest, "SCBridge.Cargo.HullAbove", ArtTestFlags)
bool FSCCargoHullAboveTest::RunTest(const FString&)
{
    // What a load's route keeps over its beams' hang (it was a fixed 60 cm: a taller drone met a hold's ceiling).
    TestEqual(TEXT("hull top over the emitter"), FSCCargoTug::HullAboveCm(-29, 45.15), 74.15, 1e-9);
    TestEqual(TEXT("an emitter on top: nothing above"), FSCCargoTug::HullAboveCm(30, 20), 0.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCFxNamesTest, "SCBridge.Cargo.FxNames", ArtTestFlags)
bool FSCFxNamesTest::RunTest(const FString&)
{
    // The builder names a system from the effect JSON's library and effect, the runtime from the reference.
    TestEqual(TEXT("package"), SCFxNames::PackageFor(TEXT("Lib/sub_dir"), TEXT("group.effect-1")), FString(TEXT("/Game/SC/FX/Lib/sub_dir/group_effect_1")));
    FString L, E;
    TestTrue(TEXT("splits at the first dot after the last slash"), SCFxNames::SplitReference(TEXT("abc-123|Lib/sub.dir/x.y.z"), L, E));
    TestEqual(TEXT("library"), L, FString(TEXT("Lib/sub.dir/x")));
    TestEqual(TEXT("effect"), E, FString(TEXT("y.z")));
    TestEqual(TEXT("system path"), SCFxNames::SystemPathForReference(TEXT("g|A/b.c.d")), FString(TEXT("/Game/SC/FX/A/b/c_d.c_d")));
    TestTrue(TEXT("no effect: not a reference"), SCFxNames::SystemPathForReference(TEXT("g|A/b")).IsEmpty());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDroneOnsetTest, "SCBridge.Cargo.DroneOnset", ArtTestFlags)
bool FSCCargoDroneOnsetTest::RunTest(const FString&)
{
    // Each drone answers its command its own moment late, spread over the drone ids: any two of eight answer more than a
    // 60 Hz frame apart; through the same start, two teammates leave level at their own moments.
    const FSCCargoDroneArt::FMotion Cfg;
    double Closest = 1e9;
    for (int32 A = 1; A <= 8; ++A)
        for (int32 B = A + 1; B <= 8; ++B) Closest = FMath::Min(Closest, FMath::Abs(FSCCargoAttitude::OnsetFor(A, Cfg) - FSCCargoAttitude::OnsetFor(B, Cfg)));
    TestTrue(FString::Printf(TEXT("Onsets of drones 1..8 at least a frame apart (%.3f s)"), Closest), Closest > 1.0 / 60);
    double Lo = 1e9, Hi = -1e9;
    for (int32 A = 0; A < 50; ++A)
    {
        Lo = FMath::Min(Lo, FSCCargoAttitude::OnsetFor(A, Cfg));
        Hi = FMath::Max(Hi, FSCCargoAttitude::OnsetFor(A, Cfg));
    }
    TestTrue(TEXT("... within the range"), Lo >= Cfg.OnsetMinS - 1e-9 && Hi <= Cfg.OnsetMaxS + 1e-9);
    const double Dt = 1.0 / 60;
    double Left[2] = {-1, -1};
    for (int32 D = 0; D < 2; ++D)
    {
        FSCCargoAttitude A;
        A.Init(7, Cfg, 3 + D); // the same seed: only the onset differs
        for (double T = 0; T < 1 && Left[D] < 0; T += Dt)
        {
            A.Step(FVector(300, 0, 0), FVector2D::ZeroVector, Dt, Cfg, /*bKicks=*/false);
            if (FMath::Abs(A.Tilt.X) > 0.05) Left[D] = T;
        }
        TestTrue(FString::Printf(TEXT("Drone %d leaves level no sooner than its onset (%.3f s, onset %.3f s)"), 3 + D, Left[D], A.Onset), Left[D] >= A.Onset - Dt);
    }
    TestTrue(FString::Printf(TEXT("Two teammates leave level at their own moments (%.3f s, %.3f s)"), Left[0], Left[1]), FMath::Abs(Left[0] - Left[1]) > Dt);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDroneKickDrawTest, "SCBridge.Cargo.DroneKickDraw", ArtTestFlags)
bool FSCCargoDroneKickDrawTest::RunTest(const FString&)
{
    // A kick's direction: mostly about the expected one (scatter), a share off to a side, a few the other way; its size
    // log-normal about its median. From each drone's own stream: two streams give two sequences.
    const FSCCargoDroneArt::FMotion Cfg;
    FRandomStream Rng(11);
    const int32 N = 20000;
    int32 Near = 0, Opposite = 0;
    TArray<double> Sizes;
    for (int32 I = 0; I < N; ++I)
    {
        const FVector2D K = FSCCargoAttitude::DrawKick(Rng, FVector2D(1, 0), 10.0, Cfg);
        const double Deg = FMath::Abs(FMath::RadiansToDegrees(FMath::Atan2(K.Y, K.X)));
        Near += Deg <= 60;
        Opposite += Deg >= 135;
        Sizes.Add(K.Size());
    }
    Sizes.Sort();
    const double NearShare = double(Near) / N, OppositeShare = double(Opposite) / N, Median = Sizes[N / 2];
    const double Expect = (1 - Cfg.OffAxisShare - Cfg.OppositeShare) * 0.95; // within two sigma of 30 degrees
    TestTrue(FString::Printf(TEXT("Most about the expected direction (%.2f within 60 deg)"), NearShare), NearShare > Expect - 0.03 && NearShare < 1 - Cfg.OffAxisShare + 0.02);
    TestTrue(FString::Printf(TEXT("... a few the other way (%.3f, configured %.2f)"), OppositeShare, Cfg.OppositeShare), OppositeShare > Cfg.OppositeShare * 0.6 && OppositeShare < Cfg.OppositeShare * 1.6);
    TestTrue(FString::Printf(TEXT("Sizes about their median (%.2f of 10), spread (%.1f .. %.1f)"), Median, Sizes[N / 10], Sizes[N * 9 / 10]),
             FMath::IsNearlyEqual(Median, 10.0, 0.5) && Sizes[N / 10] < 7 && Sizes[N * 9 / 10] > 14 && Sizes.Last() <= 30 + 1e-9);
    FRandomStream A(1), B(2);
    int32 Same = 0;
    for (int32 I = 0; I < 50; ++I) Same += FSCCargoAttitude::DrawKick(A, FVector2D(1, 0), 10, Cfg).Equals(FSCCargoAttitude::DrawKick(B, FVector2D(1, 0), 10, Cfg), 0.5);
    TestTrue(FString::Printf(TEXT("Two drones' streams draw their own (%d of 50 alike)"), Same), Same < 5);
    FRandomStream C(3);
    double Spread = 0;
    for (int32 I = 0; I < 200; ++I) Spread += FMath::Abs(FSCCargoAttitude::DrawKick(C, FVector2D::ZeroVector, 10, Cfg).GetSafeNormal().X);
    TestTrue(TEXT("No expected direction: any way"), Spread / 200 > 0.5 && Spread / 200 < 0.8);
    return true;
}

#endif
