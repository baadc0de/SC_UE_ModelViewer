// Automation tests of the route search (SCBridge.Cargo.FlightNarrow / FlightEnclosed / FlightClearance / ZoneSplit):
// passages narrower than a lattice cell at every phase of the lattice, ends enclosed by obstacles (reported as such,
// without searching to the cap), the expansion cap, the route's clearance measured by the search itself, a segment split
// at a zone's boundary. All geometry is synthetic.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Cargo/SCCargoFlightPath.h"

namespace
{
constexpr EAutomationTestFlags RouteTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
// A point flying among boxes.
FSCCargoRouteQuery BoxesQuery(const TArray<FBox>& Boxes)
{
    FSCCargoRouteQuery Q;
    Q.Blocked = [Boxes](const FVector& P) {
        for (const FBox& B : Boxes)
            if (B.IsInside(P)) return true;
        return false;
    };
    Q.Free = [Blocked = Q.Blocked](const FVector& A, const FVector& B) {
        const int32 N = FMath::Max(1, FMath::CeilToInt(FVector::Dist(A, B) / 5));
        for (int32 I = 0; I <= N; ++I)
            if (Blocked(FMath::Lerp(A, B, double(I) / N))) return false;
        return true;
    };
    Q.BlockedBy = [Boxes](const FVector& P, double Inflate) {
        for (const FBox& B : Boxes)
            if (B.ExpandBy(Inflate).IsInside(P)) return true;
        return false;
    };
    Q.CellCm = 100;
    Q.MarginCm = 1000;
    return Q;
}
// A wall across x 450..550, far wider and taller than the search box, with one window from (Y, Z) of Size x Size.
TArray<FBox> WindowWall(double Y, double Z, double Size)
{
    const double Far = 1e5;
    return {FBox(FVector(450, -Far, -Far), FVector(550, Y, Far)), FBox(FVector(450, Y + Size, -Far), FVector(550, Far, Far)),
            FBox(FVector(450, Y, -Far), FVector(550, Y + Size, Z)), FBox(FVector(450, Y, Z + Size), FVector(550, Y + Size, Far))};
}
// A closed shell around Centre: a free room of Inner cm inside walls of Wall cm; with bDoor a window of 60 cm in its -x wall.
TArray<FBox> Shell(const FVector& Centre, double Inner, double Wall, bool bDoor)
{
    const double I = Inner / 2, O = Inner / 2 + Wall;
    TArray<FBox> Boxes;
    auto Add = [&](const FVector& Min, const FVector& Max) { Boxes.Add(FBox(Centre + Min, Centre + Max)); };
    Add(FVector(I, -O, -O), FVector(O, O, O));
    Add(FVector(-O, I, -O), FVector(O, O, O));
    Add(FVector(-O, -O, -O), FVector(O, -I, O));
    Add(FVector(-O, -O, I), FVector(O, O, O));
    Add(FVector(-O, -O, -O), FVector(O, O, -I));
    if (!bDoor) Add(FVector(-O, -O, -O), FVector(-I, O, O));
    else
    {
        Add(FVector(-O, -O, -O), FVector(-I, 70, O)); // the window: y 70..130, z -30..30 (off the straight line)
        Add(FVector(-O, 130, -O), FVector(-I, O, O));
        Add(FVector(-O, 70, 30), FVector(-I, 130, O));
        Add(FVector(-O, 70, -O), FVector(-I, 130, -30));
    }
    return Boxes;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoRouteNarrowTest, "SCBridge.Cargo.FlightNarrow", RouteTestFlags)
bool FSCCargoRouteNarrowTest::RunTest(const FString&)
{
    // A 60 cm window in a wall, moved through ten phases of the 100 cm lattice in both directions: found every time, and
    // every sample sweeps free. With full cells only, the window is missed at some phases (what the half steps are for).
    const FVector Start(0, 0, 0), Goal(1000, 0, 0);
    int32 Found = 0, CoarseFound = 0, MaxExpansions = 0;
    bool bClear = true;
    for (int32 K = 0; K < 10; ++K)
    {
        FSCCargoRouteQuery Q = BoxesQuery(WindowWall(13 + 10 * K, 27 + 10 * K, 60));
        const FSCCargoRoute R = FSCCargoRoute::Plan(Start, Goal, Q);
        Found += R.bFound && R.End == ESCCargoRouteEnd::Found;
        MaxExpansions = FMath::Max(MaxExpansions, R.Expansions);
        for (int32 I = 1; R.bFound && I < R.Samples.Num(); ++I) bClear &= Q.Free(R.Samples[I - 1], R.Samples[I]);
        Q.bRefine = false;
        Q.MaxExpansions = 20000;
        CoarseFound += FSCCargoRoute::Plan(Start, Goal, Q).bFound;
    }
    TestEqual(TEXT("a window narrower than a cell is found at every lattice phase"), Found, 10);
    TestTrue(TEXT("the routes through it sweep free"), bClear);
    TestTrue(FString::Printf(TEXT("with a bounded search (most expansions %d)"), MaxExpansions), MaxExpansions < 20000);
    TestTrue(FString::Printf(TEXT("full cells alone miss it at some phases (found %d of 10)"), CoarseFound), CoarseFound < 10);
    // A window narrower than half a cell may be missed (the lattice's resolution); one of at least half a cell never is.
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoRouteEnclosedTest, "SCBridge.Cargo.FlightEnclosed", RouteTestFlags)
bool FSCCargoRouteEnclosedTest::RunTest(const FString&)
{
    const FVector Outside(0, 0, 0), Inside(1000, 0, 0);
    FSCCargoRouteQuery Closed = BoxesQuery(Shell(Inside, 300, 50, false));
    Closed.MarginCm = 3000;
    const FSCCargoRoute In = FSCCargoRoute::Plan(Outside, Inside, Closed);
    TestTrue(TEXT("a goal in a closed room: not found, reported as enclosed"), !In.bFound && In.End == ESCCargoRouteEnd::GoalEnclosed);
    TestTrue(FString::Printf(TEXT("decided by searching the room, not the world (%d expansions)"), In.Expansions), In.Expansions < 3000);
    TestTrue(TEXT("the goal's front stayed in the room"), In.FromGoal.Reached.GetSize().GetMax() <= 300);
    const FSCCargoRoute Out = FSCCargoRoute::Plan(Inside, Outside, Closed);
    TestTrue(TEXT("a start in a closed room: reported as enclosed"), !Out.bFound && Out.End == ESCCargoRouteEnd::StartEnclosed && Out.Expansions < 3000);
    // The same room with a 60 cm door: both ways are found through it.
    FSCCargoRouteQuery Door = BoxesQuery(Shell(Inside, 300, 50, true));
    Door.MarginCm = 3000;
    const FSCCargoRoute Through = FSCCargoRoute::Plan(Outside, Inside, Door);
    const FSCCargoRoute Back = FSCCargoRoute::Plan(Inside, Outside, Door);
    TestTrue(TEXT("a door: found in"), Through.bFound && Through.Samples.Num() > 2 && Through.Samples[0].Equals(Outside) && Through.Samples.Last().Equals(Inside));
    TestTrue(TEXT("a door: found out"), Back.bFound && Back.Samples[0].Equals(Inside) && Back.Samples.Last().Equals(Outside));
    bool bClear = true;
    for (int32 I = 1; I < Through.Samples.Num(); ++I) bClear &= Door.Free(Through.Samples[I - 1], Through.Samples[I]);
    TestTrue(TEXT("the route through the door sweeps free"), bClear);
    // The cap: an undecided search says so.
    Door.MaxExpansions = 10;
    const FSCCargoRoute Capped = FSCCargoRoute::Plan(Outside, Inside, Door);
    TestTrue(TEXT("the expansion cap is reported as undecided"), !Capped.bFound && Capped.End == ESCCargoRouteEnd::Budget && Capped.Expansions == 10);
    // Spread over frames with no budget (a neighbour of a node, a stretch of a sweep or a sample a step): the same route.
    Door.MaxExpansions = 60000;
    FSCCargoRouteSearch Sliced;
    Sliced.Begin(Outside, Inside, Door);
    int32 Steps = 0;
    while (!Sliced.Step(0) && Steps < 5000000) ++Steps;
    TestTrue(TEXT("sliced: the same route"), Sliced.Result.Waypoints == Through.Waypoints && Sliced.Steps == Steps + 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoRouteClearanceTest, "SCBridge.Cargo.FlightClearance", RouteTestFlags)
bool FSCCargoRouteClearanceTest::RunTest(const FString&)
{
    // A search measures its own route (clearance, hits) within its budget: the same numbers as MinClearance over its
    // samples, whether it ran in one go or a sample a step, for a searched route and for a straight one.
    const FVector Outside(0, 0, 0), Inside(1000, 0, 0);
    FSCCargoRouteQuery Door = BoxesQuery(Shell(Inside, 300, 50, true));
    Door.MarginCm = 3000;
    const FSCCargoRoute Through = FSCCargoRoute::Plan(Outside, Inside, Door);
    int32 Hits = -1;
    const double Clearance = FSCCargoRoute::MinClearance(Through.Samples, Door, Hits);
    TestTrue(TEXT("found through the door"), Through.bFound && !Through.bDirect);
    TestEqual(TEXT("the search's clearance is MinClearance of its samples"), Through.Clearance, Clearance);
    TestEqual(TEXT("and its hits"), Through.Hits, Hits);
    TestTrue(FString::Printf(TEXT("no hits on a found route (clearance %.0f cm)"), Clearance), Hits == 0);
    FSCCargoRouteSearch Sliced;
    Sliced.Begin(Outside, Inside, Door);
    int32 Steps = 0;
    while (!Sliced.Step(0) && Steps < 5000000) ++Steps;
    TestTrue(TEXT("stepped with no budget: the same measure"), Sliced.IsDone() && Sliced.Result.Clearance == Clearance && Sliced.Result.Hits == Hits);
    const FSCCargoRoute Straight = FSCCargoRoute::Plan(Outside, FVector(0, 0, 5000), Door);
    TestTrue(TEXT("a straight route far from everything: measured too, at the most"), Straight.bDirect && Straight.Clearance == 200 && Straight.Hits == 0);
    const FSCCargoRoute None = FSCCargoRoute::Plan(Outside, Inside, BoxesQuery(Shell(Inside, 300, 50, false)));
    TestTrue(TEXT("no route: nothing measured"), !None.bFound && None.Clearance == 0 && None.Hits == 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoZoneSplitTest, "SCBridge.Cargo.ZoneSplit", RouteTestFlags)
bool FSCCargoZoneSplitTest::RunTest(const FString&)
{
    // Where a segment is inside a zone (a sweep crossing the boundary is split there, each stretch with its margin).
    const FBox Zone(FVector(0, 0, 0), FVector(1000, 400, 300));
    double Enter = -1, Leave = -1;
    TestTrue(TEXT("entering: inside from the boundary on"), SCCargoSegmentInside(FVector(-500, 200, 100), FVector(500, 200, 100), Zone, Enter, Leave) &&
                                                             FMath::IsNearlyEqual(Enter, 0.5) && FMath::IsNearlyEqual(Leave, 1.0));
    TestTrue(TEXT("leaving: the same stretch the other way"), SCCargoSegmentInside(FVector(500, 200, 100), FVector(-500, 200, 100), Zone, Enter, Leave) &&
                                                               FMath::IsNearlyEqual(Enter, 0.0) && FMath::IsNearlyEqual(Leave, 0.5));
    TestTrue(TEXT("through: in and out"), SCCargoSegmentInside(FVector(-1000, 200, 100), FVector(3000, 200, 100), Zone, Enter, Leave) &&
                                           FMath::IsNearlyEqual(Enter, 0.25) && FMath::IsNearlyEqual(Leave, 0.5));
    TestTrue(TEXT("within: all of it"), SCCargoSegmentInside(FVector(100, 100, 100), FVector(900, 300, 200), Zone, Enter, Leave) && Enter == 0 && Leave == 1);
    TestTrue(TEXT("a diagonal across a corner"), SCCargoSegmentInside(FVector(-100, 300, 100), FVector(100, 500, 100), Zone, Enter, Leave) &&
                                                  FMath::IsNearlyEqual(Enter, 0.5) && FMath::IsNearlyEqual(Leave, 0.5));
    TestFalse(TEXT("past it: none"), SCCargoSegmentInside(FVector(-500, 500, 100), FVector(500, 500, 100), Zone, Enter, Leave));
    TestFalse(TEXT("short of it: none"), SCCargoSegmentInside(FVector(-500, 200, 100), FVector(-100, 200, 100), Zone, Enter, Leave));
    TestTrue(TEXT("a point inside"), SCCargoSegmentInside(FVector(10, 10, 10), FVector(10, 10, 10), Zone, Enter, Leave));
    TestFalse(TEXT("a point outside"), SCCargoSegmentInside(FVector(-10, 10, 10), FVector(-10, 10, 10), Zone, Enter, Leave));
    return true;
}
#endif
