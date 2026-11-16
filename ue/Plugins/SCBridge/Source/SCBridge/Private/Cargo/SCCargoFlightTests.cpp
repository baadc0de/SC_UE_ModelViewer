// Automation tests of drone flight (SCBridge.Cargo.Flight*): routes around synthetic obstacles, the speed profile's
// limits, loaded acceleration and team size, and the pick-time box substitution (SCBridge.Cargo.PickFallback).
// All values are synthetic.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Cargo/SCCargoFlightPath.h"
#include "Cargo/SCCargoPlanner.h"
#include "Cargo/SCCargoGridComponent.h"
#include "Cargo/SCCargoDrone.h"

namespace
{
constexpr EAutomationTestFlags FlightTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
// A wall across the straight line (x 450..550, |y| < 1000, z < 800), the flying shape a 60 cm cube.
FSCCargoRouteQuery WallQuery()
{
    const FBox Wall(FVector(450, -1000, -1000), FVector(550, 1000, 800));
    const FBox Grown = Wall.ExpandBy(60);
    FSCCargoRouteQuery Q;
    Q.Blocked = [Grown](const FVector& P) { return Grown.IsInside(P); };
    Q.Free = [Grown](const FVector& A, const FVector& B) {
        const int32 N = FMath::Max(1, FMath::CeilToInt(FVector::Dist(A, B) / 20));
        for (int32 I = 0; I <= N; ++I)
            if (Grown.IsInside(FMath::Lerp(A, B, double(I) / N))) return false;
        return true;
    };
    Q.BlockedBy = [Wall](const FVector& P, double Inflate) { return Wall.ExpandBy(60 + Inflate).IsInside(P); };
    Q.CellCm = 100;
    Q.MarginCm = 1500;
    return Q;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoRouteTest, "SCBridge.Cargo.FlightRoute", FlightTestFlags)
bool FSCCargoRouteTest::RunTest(const FString&)
{
    FSCCargoRouteQuery Q = WallQuery();
    const FSCCargoRoute Open = FSCCargoRoute::Plan(FVector(0, 0, 1500), FVector(1000, 0, 1500), Q);
    TestTrue(TEXT("a free straight line is taken as is"), Open.bDirect && Open.bFound);
    const FSCCargoRoute Around = FSCCargoRoute::Plan(FVector(0, 0, 100), FVector(1000, 0, 100), Q);
    TestTrue(TEXT("a route around the wall is found"), Around.bFound && !Around.bDirect);
    bool bClear = true;
    for (int32 I = 1; I < Around.Samples.Num(); ++I) bClear &= Q.Free(Around.Samples[I - 1], Around.Samples[I]);
    TestTrue(TEXT("every spline segment sweeps free"), bClear);
    int32 Hits = 0;
    const double Clearance = FSCCargoRoute::MinClearance(Around.Samples, Q, Hits, 20);
    TestTrue(FString::Printf(TEXT("the route keeps a clearance from the obstacle (min %.0f cm, hits %d)"), Clearance, Hits), Clearance > 0 && Hits == 0);
    int32 StraightHits = 0;
    FSCCargoRoute::MinClearance(FSCCargoRoute::Spline({FVector(0, 0, 100), FVector(1000, 0, 100)}, 50), Q, StraightHits, 20);
    TestTrue(TEXT("the straight line through the obstacle is measured as hitting it"), StraightHits > 0);
    TestTrue(TEXT("it starts and ends where asked"), Around.Samples.Num() > 2 && Around.Samples[0].Equals(FVector(0, 0, 100)) && Around.Samples.Last().Equals(FVector(1000, 0, 100)));
    TestTrue(TEXT("shortcutting keeps few waypoints"), Around.Waypoints.Num() <= 6);
    TestTrue(TEXT("not much longer than over the wall"), Around.Length() < 3500);
    const FSCCargoRoute Into = FSCCargoRoute::Plan(FVector(0, 0, 100), FVector(500, 0, 100), Q);
    TestTrue(TEXT("a goal inside the obstacle is refused without a search"), !Into.bFound && Into.Expansions == 0);
    // Spread over frames: a search stepped with no time budget at all (one small piece of work a step) ends with the same route,
    // and a shared cell cache gives the same route again while sampling no cell twice.
    FSCCargoRouteSearch Sliced;
    Sliced.Begin(FVector(0, 0, 100), FVector(1000, 0, 100), Q);
    int32 Steps = 0;
    while (!Sliced.Step(0) && Steps < 5000000) ++Steps; // a neighbour, a stretch of a sweep or a sample a step
    TestTrue(FString::Printf(TEXT("sliced over %d steps, to the end"), Steps), Steps > 1 && Sliced.IsDone());
    TestEqual(TEXT("sliced: the same waypoints"), Sliced.Result.Waypoints, Around.Waypoints);
    int32 Samples = 0;
    FSCCargoRouteQuery Cached = Q;
    Cached.CellCache = MakeShared<TMap<FIntVector, bool>>();
    Cached.Blocked = [Inner = Q.Blocked, &Samples](const FVector& P) { ++Samples; return Inner(P); };
    const FSCCargoRoute First = FSCCargoRoute::Plan(FVector(0, 0, 100), FVector(1000, 0, 100), Cached);
    const int32 FirstSamples = Samples;
    const FSCCargoRoute Again = FSCCargoRoute::Plan(FVector(0, 0, 100), FVector(1000, 0, 100), Cached);
    TestTrue(TEXT("cached: same route"), First.Waypoints == Again.Waypoints && First.bFound);
    TestEqual(TEXT("cached: no cell sampled twice (only the goal check)"), Samples - FirstSamples, 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoProfileTest, "SCBridge.Cargo.FlightProfile", FlightTestFlags)
bool FSCCargoProfileTest::RunTest(const FString&)
{
    auto Fly = [&](double Accel, double& OutSeconds, bool& bWithinLimits) {
        FSCCargoFlight F;
        F.Start({FVector::ZeroVector, FVector(1000, 0, 0), FVector(1000, 1000, 0), FVector(2000, 1000, 0)}, 800, Accel, Accel * 4);
        const double Dt = 1.0 / 60;
        double PrevAccel = 0, T = 0;
        bWithinLimits = true;
        while (!F.Done() && T < 120)
        {
            F.Step(Dt);
            T += Dt;
            bWithinLimits &= F.Speed <= 800 + 1 && FMath::Abs(F.Accel) <= Accel + 1 && FMath::Abs(F.Accel - PrevAccel) <= Accel * 4 * Dt + 1;
            PrevAccel = F.Accel;
        }
        OutSeconds = T;
        return F.Done() && FMath::IsNearlyEqual(F.Dist, F.Length());
    };
    double Light = 0, Heavy = 0;
    bool bLightOK = false, bHeavyOK = false;
    TestTrue(TEXT("an empty flight arrives at rest"), Fly(600, Light, bLightOK));
    TestTrue(TEXT("speed, acceleration and jerk stay within limits"), bLightOK);
    TestTrue(TEXT("a heavy flight arrives at rest"), Fly(150, Heavy, bHeavyOK));
    TestTrue(TEXT("heavy limits hold too"), bHeavyOK);
    TestTrue(TEXT("the heavy load takes longer"), Heavy > Light * 1.3);
    TestTrue(TEXT("the trapezoid estimate is a lower bound"), FSCCargoFlight::Seconds(3000, 800, 600) <= Light);
    FSCCargoFlight Hover;
    Hover.Start({FVector(1, 2, 3)}, 800, 600, 2400);
    TestTrue(TEXT("a one-point flight (holding position) is done where it is"), Hover.Done() && Hover.Position().Equals(FVector(1, 2, 3)));
    // A one-point flight has arrived (a route of zero length); "no route yet, plan again" is an unstarted flight. A drone
    // sent home whose home route is still being searched must not count as arrived: it would be put in its bay at once,
    // from wherever it is (seen as drones vanishing when their job ended).
    TestTrue(TEXT("a one-point flight counts as a route (arrived)"), Hover.HasRoute());
    TestFalse(TEXT("an unstarted flight is no route yet"), FSCCargoFlight().HasRoute());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoTeamTest, "SCBridge.Cargo.FlightTeam", FlightTestFlags)
bool FSCCargoTeamTest::RunTest(const FString&)
{
    TestEqual(TEXT("loaded acceleration (N F - m g) / (N m_d + m)"), SCCargoLoadedAccel(1, 15000, 1000, 100, 10, 50), 5000.0 / 1100.0);
    TestEqual(TEXT("capped by the empty drones' limit"), SCCargoLoadedAccel(2, 15000, 100, 100, 10, 3), 3.0);
    TestEqual(TEXT("one drone with a reserve"), FSCCargoPlanner::DronesFor(1000, 10, 15000, 100, 2, 4), 1);
    TestEqual(TEXT("a heavier load needs a second drone for the reserve"), FSCCargoPlanner::DronesFor(1400, 10, 15000, 100, 2, 4), 2);
    TestEqual(TEXT("no team within the limit"), FSCCargoPlanner::DronesFor(10000, 10, 15000, 100, 2, 4), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoPickTest, "SCBridge.Cargo.PickFallback", FlightTestFlags)
bool FSCCargoPickTest::RunTest(const FString&)
{
    // Source: box 1 covered by box 3 (another commodity); box 2 free, the same size. Destination: room for one.
    FSCCargoPlanInput In;
    FSCCargoGridModel Source, Dest;
    Source.GridId = 1;
    Source.Label = TEXT("Source");
    Source.Lattice.Cells = FIntVector(2, 1, 2);
    Source.Lattice.CellM = FVector(1);
    Source.Lattice.InteriorM = FVector(2, 1, 2);
    Dest = Source;
    Dest.GridId = 2;
    Dest.Lattice.Cells = FIntVector(1, 1, 1);
    Dest.Lattice.InteriorM = FVector(1);
    auto Add = [&](int32 Id, FName Commodity, const FIntVector& At) {
        FSCCargoItem I;
        I.Id = Id;
        I.SizeM = FVector(1);
        I.SCU = 1;
        I.MassKg = 100;
        I.Commodity = Commodity;
        In.Items.Add(Id, I);
        FSCCargoPlacement P;
        P.ItemId = Id;
        P.Origin = At;
        P.Cells = FIntVector(1);
        Source.Placed.Add(P);
    };
    Add(1, TEXT("Synthetic"), FIntVector(0, 0, 0));
    Add(2, TEXT("Synthetic"), FIntVector(1, 0, 0));
    Add(3, TEXT("Other"), FIntVector(0, 0, 1));
    In.Grids = {Source, Dest};
    In.FromGrids = {1};
    In.ToGrids = {2};
    In.LiftForceN = 10000;
    In.GravityMS2 = 10;
    In.DronesAvailable = 1;
    FSCCargoMovePlan Failed;
    Failed.BoxId = 1;
    Failed.SCU = 1;
    Failed.FromGrid = 1;
    Failed.ToGrid = 2;
    FSCCargoMovePlan Out;
    TArray<FString> Why;
    TestTrue(TEXT("a free box of the same size replaces the covered one"),
             FSCCargoPlanner::Substitute(In, Failed, {}, [](int32, FString&) { return true; }, Out, Why));
    TestEqual(TEXT("the free box"), Out.BoxId, 2);
    Why.Reset();
    TestFalse(TEXT("nothing else liftable: no substitute"),
              FSCCargoPlanner::Substitute(In, Failed, {}, [](int32, FString& W) { W = TEXT("lift path obstructed"); return false; }, Out, Why));
    TestTrue(TEXT("with the reasons"), Why.Contains(TEXT("lift path obstructed")));
    TestFalse(TEXT("an excluded box is not offered"),
              FSCCargoPlanner::Substitute(In, Failed, {2}, [](int32, FString&) { return true; }, Out, Why));
    // Never more than asked for: a larger box of the commodity is no substitute.
    In.Items[2].SCU = 2;
    TestFalse(TEXT("a larger box is not offered"), FSCCargoPlanner::Substitute(In, Failed, {}, [](int32, FString&) { return true; }, Out, Why));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoTeamVolumeTest, "SCBridge.Cargo.TeamByVolume", FlightTestFlags)
bool FSCCargoTeamVolumeTest::RunTest(const FString&)
{
    // Beams of 20 volume units and plenty of force: a 16-unit box takes one, 24 and 32 take two, 48 takes three; the
    // force term still rules for a heavy small box. Synthetic values.
    FSCCargoPlanInput In;
    In.LiftForceN = 1e6;
    In.GravityMS2 = 10;
    In.BeamMaxVolume = 20;
    FSCCargoItem Box;
    Box.MassKg = 100;
    int32 ByForce = 0, ByVolume = 0;
    for (const auto& [Volume, Team] : TArray<TPair<double, int32>>{{16, 1}, {24, 2}, {32, 2}, {48, 3}})
    {
        Box.Volume = Volume;
        TestEqual(FString::Printf(TEXT("%.0f units"), Volume), FSCCargoPlanner::TeamSize(In, Box, 4, &ByForce, &ByVolume), Team);
    }
    TestEqual(TEXT("more than the fleet: the count (the caller refuses)"), [&] { Box.Volume = 100; return FSCCargoPlanner::TeamSize(In, Box, 4); }(), 5);
    Box.Volume = 4;
    Box.MassKg = 250000; // 2.5e6 N of weight against 1e6 N beams
    TestEqual(TEXT("heavy and small: the force decides"), FSCCargoPlanner::TeamSize(In, Box, 4, &ByForce, &ByVolume), 3);
    TestEqual(TEXT("terms"), FIntPoint(ByForce, ByVolume), FIntPoint(3, 1));
    In.BeamMaxVolume = 0;
    Box.MassKg = 100;
    Box.Volume = 1000;
    TestEqual(TEXT("no volume limit in the data: force only"), FSCCargoPlanner::TeamSize(In, Box, 4), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoSwayTest, "SCBridge.Cargo.Sway", FlightTestFlags)
bool FSCCargoSwayTest::RunTest(const FString&)
{
    // Gusts: bounded by the amplitude (vertical half), smooth (no jumps between frames), different per drone.
    double MaxAxis = 0, MaxStep = 0, MaxTilt = 0, Apart = 0;
    FVector Prev = FSCCargoSway::Offset(1, 0, 4, 0.3);
    for (int32 K = 1; K <= 60 * 60; ++K)
    {
        const double T = K / 60.0;
        const FVector O = FSCCargoSway::Offset(1, T, 4, 0.3);
        MaxAxis = FMath::Max3(MaxAxis, FMath::Abs(O.X), FMath::Max(FMath::Abs(O.Y), 2 * FMath::Abs(O.Z)));
        MaxStep = FMath::Max(MaxStep, FVector::Dist(O, Prev));
        MaxTilt = FMath::Max(MaxTilt, FSCCargoSway::Tilt(1, T, 3, 0.3).GetAbsMax());
        Apart = FMath::Max(Apart, FVector::Dist(O, FSCCargoSway::Offset(2, T, 4, 0.3)));
        Prev = O;
    }
    TestTrue(FString::Printf(TEXT("within the amplitude (%.2f cm)"), MaxAxis), MaxAxis <= 4.0 + 1e-6 && MaxAxis > 1.0);
    TestTrue(FString::Printf(TEXT("within the tilt (%.2f deg)"), MaxTilt), MaxTilt <= 3.0 + 1e-6);
    TestTrue(FString::Printf(TEXT("smooth: at most %.3f cm per frame"), MaxStep), MaxStep < 0.5);
    TestTrue(TEXT("teammates do not move in lockstep"), Apart > 1.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoMoverTest, "SCBridge.Cargo.MoverDirection", FlightTestFlags)
bool FSCCargoMoverTest::RunTest(const FString&)
{
    // The grid's frame at a clip's first and last frame around a hull centred at the origin.
    const FVector Centre = FVector::ZeroVector;
    const FTransform In(FVector(0, 200, 300)), Out(FVector(0, 600, 300));
    bool bForward = false;
    double Travel = 0;
    TestTrue(TEXT("a deploy clip carries the grid"), FSCCargoMover::Classify(In, Out, Centre, bForward, Travel));
    TestTrue(TEXT("deploy: open at its last frame (out of the hull)"), bForward);
    TestEqual(TEXT("travel"), Travel, 400.0);
    TestTrue(TEXT("a retract clip carries it too"), FSCCargoMover::Classify(Out, In, Centre, bForward, Travel));
    TestFalse(TEXT("retract: open at its first frame (played backwards to open)"), bForward);
    const FTransform Turned(FRotator(0, 90, 0), FVector(0, 200, 300));
    TestTrue(TEXT("a clip only turning the grid in place still carries it"), FSCCargoMover::Classify(In, Turned, Centre, bForward, Travel));
    TestFalse(TEXT("an idle that leaves the grid where it is is no mechanism"),
              FSCCargoMover::Classify(In, FTransform(FVector(0, 200.5, 300)), Centre, bForward, Travel));
    // Two mechanisms: a plate unfolding (inner: alone it moves the grid towards the hull, 200 -> 150 from the centre) on
    // a strut swinging it over (outer: half a turn about y = 400, y -> 800 - y). Judged alone the plate looks closing;
    // with the strut out, the unfolded plate is the furthest: both at their last frame.
    const FVector Point(0, 200, 0);
    const TArray<FTransform> First = {FTransform::Identity, FTransform::Identity};
    const FTransform Unfold(FVector(0, -50, 0));
    const FTransform Strut = FTransform(FVector(0, -400, 0)) * FTransform(FRotator(0, 180, 0)) * FTransform(FVector(0, 400, 0));
    TestTrue(TEXT("alone the plate moves the grid inwards"), FMath::IsNearlyEqual(Unfold.TransformPosition(Point).Y, 150.0, 0.01));
    TestTrue(TEXT("the strut swings it out"), FMath::IsNearlyEqual(Strut.TransformPosition(Point).Y, 600.0, 0.01));
    TestEqual(TEXT("together: strut out, plate unfolded"), FSCCargoMover::OpenEnds(Point, First, {Unfold, Strut}, Centre), 3u);
    TestEqual(TEXT("a strut that swings in: open at its first frame"), FSCCargoMover::OpenEnds(Point, First, {FTransform::Identity, FTransform(FVector(0, -150, 0))}, Centre), 1u);
    return true;
}

// A load stopped within a placed box's margin replans from a start out of that margin: straight up under gravity (the
// hangar convention), but in zero g a box can lie above it (an underside plate), so other ways out are tried: out along
// the destination plate's normal, back the way it came, then the remaining axes. (WP-K3)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoEscapeStartTest, "SCBridge.Cargo.EscapeStart", FlightTestFlags)
bool FSCCargoEscapeStartTest::RunTest(const FString&)
{
    // A box margin from z 50 up (a box on an underside plate above the load); the load stopped at z 100 inside it.
    auto Query = [](const FBox& Margin) {
        FSCCargoRouteQuery Q;
        Q.Blocked = [Margin](const FVector& P) { return Margin.IsInside(P); };
        return Q;
    };
    const FSCCargoRouteQuery Above = Query(FBox(FVector(-200, -200, 50), FVector(200, 200, 600)));
    const FVector From(0, 0, 100), Down(0, 0, -1), Back(1, 0, 0);
    FVector Start;
    TestTrue(TEXT("zero g, box above: a way out is found"), FSCCargoRoute::EscapeStart(From, Above, false, Down, Back, Start));
    TestTrue(TEXT("zero g, box above: out along the plate's normal (down)"), Start.Z <= 50 && FMath::IsNearlyEqual(Start.X, 0.0) && !Above.Blocked(Start));
    TestTrue(TEXT("gravity, box above: up is closed, another way is found"), FSCCargoRoute::EscapeStart(From, Above, true, Down, Back, Start) && !Above.Blocked(Start));
    // A box margin below (the hangar case): up first, as before.
    const FSCCargoRouteQuery Below = Query(FBox(FVector(-200, -200, -500), FVector(200, 200, 150)));
    TestTrue(TEXT("gravity, box below: up"), FSCCargoRoute::EscapeStart(From, Below, true, FVector(1, 0, 0), Back, Start) && Start.Z >= 150 && FMath::IsNearlyEqual(Start.X, 0.0));
    TestTrue(TEXT("a free start stays where it is"), FSCCargoRoute::EscapeStart(FVector(0, 0, 1000), Below, false, Down, Back, Start) && Start.Equals(FVector(0, 0, 1000)));
    // Enclosed beyond reach: none, and the start is left as it was.
    const FSCCargoRouteQuery Deep = Query(FBox(FVector(-2000), FVector(2000)));
    TestFalse(TEXT("no way out within reach"), FSCCargoRoute::EscapeStart(From, Deep, false, Down, Back, Start));
    TestTrue(TEXT("no way out: the start as it was"), Start.Equals(From));
    return true;
}

// Every drone that is not holding a load steps out of the way of loads other teams fly, also one hovering over its
// box while the beams lock on (Attaching): the box is still at rest, so the drone is alone in the air. (WP-K3)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDodgeTest, "SCBridge.Cargo.DodgeLoads", FlightTestFlags)
bool FSCCargoDodgeTest::RunTest(const FString&)
{
    using S = ESCCargoDroneState;
    for (const S Free : {S::Launching, S::ToPickup, S::Attaching, S::Returning, S::Docking})
        TestTrue(FString::Printf(TEXT("dodges in state %d"), int32(Free)), ASCCargoDrone::DodgesLoads(Free));
    for (const S Bound : {S::Docked, S::Carrying, S::Releasing, S::Holding})
        TestFalse(FString::Printf(TEXT("stays with its box (or bay) in state %d"), int32(Bound)), ASCCargoDrone::DodgesLoads(Bound));
    return true;
}
#endif
