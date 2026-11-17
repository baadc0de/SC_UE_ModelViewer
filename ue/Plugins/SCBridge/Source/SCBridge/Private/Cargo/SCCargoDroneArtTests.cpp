// Automation tests of the drone art policies (WP-K4a, SCBridge.Cargo.Drone* / DockFootprint / FxNames): where a drone
// faces, which clip it plays from its motion, how many cells a dock mesh takes and the turns it may try, the
// drone_art.json shape, and the effect -> system naming rule the builder and the runtime share. Every number here is
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
#endif
