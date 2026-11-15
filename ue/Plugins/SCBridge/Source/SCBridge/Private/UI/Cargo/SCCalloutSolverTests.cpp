// SCBridge.UI.Callouts.*: the callout placement rules on synthetic screen layouts (no world, no Slate) — labels never
// overlap, never cover their object, leader lines use 0°/45°/90° segments of the token lengths and do not cross,
// previous sides are kept, the label budget goes to the highest priorities.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCCalloutSolver.h"

namespace
{
const FBox2D Screen(FVector2D(0, 0), FVector2D(1920, 1080));

FSCCalloutTarget Target(int32 Key, FVector2D Centre, FVector2D HalfSize, int32 Priority = 0, double Depth = 0)
{
    FSCCalloutTarget T;
    T.Key = Key;
    T.Anchor = Centre;
    T.Object = FBox2D(Centre - HalfSize, Centre + HalfSize);
    T.LabelSize = FVector2D(180, 40);
    T.Priority = Priority;
    T.Depth = Depth;
    return T;
}

/** Synthetic layouts: scattered objects, a tight cluster, a column of objects at one x, a large object near an edge. */
TArray<TArray<FSCCalloutTarget>> Layouts()
{
    TArray<TArray<FSCCalloutTarget>> L;
    L.Add({Target(1, {500, 500}, {60, 40}), Target(2, {900, 420}, {80, 50}), Target(3, {1300, 600}, {50, 50}), Target(4, {700, 760}, {40, 30}),
           Target(5, {1500, 300}, {70, 40})});
    // Four grids stacked two by two (a ship's cargo plates) with a drone above them.
    L.Add({Target(1, {900, 500}, {40, 30}), Target(2, {900, 600}, {40, 30}), Target(3, {1010, 500}, {40, 30}), Target(4, {1010, 600}, {40, 30}),
           Target(5, {955, 440}, {10, 10}, 3)});
    L.Add({Target(1, {960, 300}, {40, 30}), Target(2, {960, 450}, {40, 30}), Target(3, {960, 600}, {40, 30}), Target(4, {960, 750}, {40, 30})});
    L.Add({Target(1, {1600, 540}, {300, 320}), Target(2, {600, 500}, {50, 40})});
    return L;
}

bool IsAllowedSegment(const FVector2D& A, const FVector2D& B)
{
    const FVector2D D = B - A;
    const bool bHorizontal = FMath::Abs(D.Y) < 1e-6, bVertical = FMath::Abs(D.X) < 1e-6;
    const bool bDiagonal = FMath::Abs(FMath::Abs(D.X) - FMath::Abs(D.Y)) < 1e-6;
    return bHorizontal || bVertical || bDiagonal;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCalloutsNoOverlap, "SCBridge.UI.Callouts.NoOverlap", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCCalloutsNoOverlap::RunTest(const FString&)
{
    for (const TArray<FSCCalloutTarget>& Layout : Layouts())
    {
        const TArray<FSCCalloutPlacement> Out = FSCCalloutSolver::Solve(Layout, Screen);
        for (int32 I = 0; I < Out.Num(); ++I)
        {
            if (!Out[I].bLabel) continue;
            TestTrue(TEXT("label inside the screen"), Out[I].Label.Min.X >= 0 && Out[I].Label.Min.Y >= 0 && Out[I].Label.Max.X <= 1920 && Out[I].Label.Max.Y <= 1080);
            for (int32 J = I + 1; J < Out.Num(); ++J)
                if (Out[J].bLabel)
                    TestFalse(FString::Printf(TEXT("labels %d and %d keep the 6 px gap"), Out[I].Key, Out[J].Key),
                              FSCCalloutSolver::BoxesOverlap(Out[I].Label, Out[J].Label, 6 - 1e-3));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCalloutsNoCover, "SCBridge.UI.Callouts.NoCover", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCCalloutsNoCover::RunTest(const FString&)
{
    for (const TArray<FSCCalloutTarget>& Layout : Layouts())
    {
        const TArray<FSCCalloutPlacement> Out = FSCCalloutSolver::Solve(Layout, Screen);
        int32 Labels = 0;
        for (int32 I = 0; I < Out.Num(); ++I)
        {
            if (!Out[I].bLabel) continue;
            ++Labels;
            TestFalse(FString::Printf(TEXT("label %d clears its object by 16 px"), Out[I].Key),
                      FSCCalloutSolver::BoxesOverlap(Out[I].Label, Layout[I].Object.ExpandBy(16 - 1e-3)));
            for (int32 J = 0; J < Out.Num(); ++J)
                TestFalse(FString::Printf(TEXT("label %d does not cover object %d"), Out[I].Key, Layout[J].Key),
                          FSCCalloutSolver::BoxesOverlap(Out[I].Label, Layout[J].Object));
        }
        TestEqual(TEXT("every target of a solvable layout gets a label"), Labels, Layout.Num());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCalloutsNoCrossing, "SCBridge.UI.Callouts.NoCrossing", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCCalloutsNoCrossing::RunTest(const FString&)
{
    for (const TArray<FSCCalloutTarget>& Layout : Layouts())
    {
        const TArray<FSCCalloutPlacement> Out = FSCCalloutSolver::Solve(Layout, Screen);
        for (int32 I = 0; I < Out.Num(); ++I)
        {
            if (!Out[I].bLabel) continue;
            for (int32 J = 0; J < Out.Num(); ++J)
            {
                if (J == I || !Out[J].bLabel) continue;
                if (J > I)
                    TestFalse(FString::Printf(TEXT("lines %d and %d do not cross"), Out[I].Key, Out[J].Key),
                              FSCCalloutSolver::PolylinesCross(Out[I].Line, Out[J].Line));
                TestFalse(FString::Printf(TEXT("line %d does not run through label %d"), Out[I].Key, Out[J].Key),
                          FSCCalloutSolver::PolylineHitsBox(Out[I].Line, Out[J].Label));
            }
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCalloutsLeaderShape, "SCBridge.UI.Callouts.LeaderShape", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCCalloutsLeaderShape::RunTest(const FString&)
{
    for (const TArray<FSCCalloutTarget>& Layout : Layouts())
    {
        const TArray<FSCCalloutPlacement> Out = FSCCalloutSolver::Solve(Layout, Screen);
        for (int32 I = 0; I < Out.Num(); ++I)
        {
            const FSCCalloutPlacement& P = Out[I];
            if (!P.bLabel) continue;
            TestTrue(TEXT("line starts at the anchor"), P.Line.Num() >= 3 && P.Line[0].Equals(Layout[I].Anchor));
            for (int32 S = 1; S < P.Line.Num(); ++S) TestTrue(TEXT("0/45/90 degree segments"), IsAllowedSegment(P.Line[S - 1], P.Line[S]));
            const FVector2D Elbow = P.Line[P.Line.Num() - 2], Before = P.Line[P.Line.Num() - 3], Edge = P.Line.Last();
            const double Run = FMath::Abs(Elbow.X - Before.X);
            TestTrue(FString::Printf(TEXT("45 degree run %.1f within 24..64 px"), Run), Run >= 24 - 1e-6 && Run <= 64 + 1e-6);
            TestTrue(TEXT("shelf is horizontal and at least 16 px"), FMath::Abs(Edge.Y - Elbow.Y) < 1e-6 && FMath::Abs(Edge.X - Elbow.X) >= 16 - 1e-6);
            TestTrue(TEXT("shelf meets the label edge at mid height"),
                     FMath::Abs(Edge.Y - P.Label.GetCenter().Y) < 1e-6 && (FMath::Abs(Edge.X - P.Label.Min.X) < 1e-6 || FMath::Abs(Edge.X - P.Label.Max.X) < 1e-6));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCalloutsCrowded, "SCBridge.UI.Callouts.Crowded", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCCalloutsCrowded::RunTest(const FString&)
{
    // Six objects within 150 x 80 px: not all can be labelled under the rules; those that are keep every rule and the
    // rest are dots.
    const TArray<FSCCalloutTarget> Layout = {Target(1, {900, 540}, {30, 20}), Target(2, {960, 560}, {30, 20}), Target(3, {1020, 530}, {30, 20}),
                                             Target(4, {940, 600}, {30, 20}), Target(5, {1000, 610}, {30, 20}), Target(6, {880, 590}, {25, 20})};
    const TArray<FSCCalloutPlacement> Out = FSCCalloutSolver::Solve(Layout, Screen);
    int32 Labels = 0;
    for (int32 I = 0; I < Out.Num(); ++I)
    {
        if (!Out[I].bLabel)
        {
            TestTrue(TEXT("a dot keeps its anchor"), Out[I].Line.Num() == 1);
            continue;
        }
        ++Labels;
        for (int32 J = 0; J < Out.Num(); ++J)
        {
            TestFalse(TEXT("no object covered"), FSCCalloutSolver::BoxesOverlap(Out[I].Label, Layout[J].Object));
            if (J == I || !Out[J].bLabel) continue;
            TestFalse(TEXT("no overlap"), FSCCalloutSolver::BoxesOverlap(Out[I].Label, Out[J].Label, 6 - 1e-3));
            TestFalse(TEXT("no crossing"), FSCCalloutSolver::PolylinesCross(Out[I].Line, Out[J].Line));
            TestFalse(TEXT("no line through a label"), FSCCalloutSolver::PolylineHitsBox(Out[I].Line, Out[J].Label));
        }
    }
    TestTrue(FString::Printf(TEXT("most of the cluster is labelled (%d of 6)"), Labels), Labels >= 5);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCalloutsMotion, "SCBridge.UI.Callouts.Motion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCCalloutsMotion::RunTest(const FString&)
{
    // The layer re-solves every frame with the previous sides: as the view pans across a 2 x 2 cluster with a drone,
    // every painted frame keeps the rules and labels do not flip sides while their side stays free.
    TArray<FSCCalloutTarget> Layout = {Target(1, {900, 500}, {40, 30}), Target(2, {900, 600}, {40, 30}), Target(3, {1010, 500}, {40, 30}),
                                       Target(4, {1010, 600}, {40, 30}), Target(5, {955, 440}, {10, 10}, 3)};
    int32 Flips = 0;
    for (int32 Frame = 0; Frame < 60; ++Frame)
    {
        const TArray<FSCCalloutPlacement> Out = FSCCalloutSolver::Solve(Layout, Screen);
        for (int32 I = 0; I < Out.Num(); ++I)
        {
            if (!Out[I].bLabel) continue;
            Flips += Layout[I].PreviousSide != ESCCalloutSide::None && Layout[I].PreviousSide != Out[I].Side;
            for (int32 J = I + 1; J < Out.Num(); ++J)
                if (Out[J].bLabel && (FSCCalloutSolver::BoxesOverlap(Out[I].Label, Out[J].Label, 6 - 1e-3) || FSCCalloutSolver::PolylinesCross(Out[I].Line, Out[J].Line)))
                    AddError(FString::Printf(TEXT("frame %d: labels %d and %d overlap or cross"), Frame, Out[I].Key, Out[J].Key));
            Layout[I].PreviousSide = Out[I].Side;
        }
        for (FSCCalloutTarget& T : Layout) // pan right and slightly down
        {
            T.Anchor += FVector2D(6, 1);
            T.Object = FBox2D(T.Object.Min + FVector2D(6, 1), T.Object.Max + FVector2D(6, 1));
        }
    }
    TestTrue(FString::Printf(TEXT("labels keep their sides while panning (%d flips)"), Flips), Flips <= 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCalloutsHysteresis, "SCBridge.UI.Callouts.Hysteresis", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCCalloutsHysteresis::RunTest(const FString&)
{
    TArray<FSCCalloutTarget> Layout = {Target(1, {900, 500}, {50, 40})};
    TestEqual(TEXT("first choice is NE"), FSCCalloutSolver::Solve(Layout, Screen)[0].Side, ESCCalloutSide::NE);
    Layout[0].PreviousSide = ESCCalloutSide::SW;
    TestEqual(TEXT("a free previous side is kept"), FSCCalloutSolver::Solve(Layout, Screen)[0].Side, ESCCalloutSide::SW);
    // Near the bottom-left corner SW has no room: the label moves to the next free side.
    Layout[0] = Target(1, {120, 1020}, {40, 30});
    Layout[0].PreviousSide = ESCCalloutSide::SW;
    const FSCCalloutPlacement P = FSCCalloutSolver::Solve(Layout, Screen)[0];
    TestTrue(TEXT("a colliding previous side is given up"), P.bLabel && P.Side == ESCCalloutSide::NE);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCalloutsBudget, "SCBridge.UI.Callouts.Budget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCCalloutsBudget::RunTest(const FString&)
{
    TArray<FSCCalloutTarget> Layout;
    for (int32 I = 0; I < 12; ++I)
        Layout.Add(Target(I, FVector2D(200 + (I % 4) * 460, 220 + (I / 4) * 320), FVector2D(30, 20), I == 11 ? 10 : 0, I));
    const TArray<FSCCalloutPlacement> Out = FSCCalloutSolver::Solve(Layout, Screen);
    int32 Labels = 0;
    for (const FSCCalloutPlacement& P : Out) Labels += P.bLabel;
    TestEqual(TEXT("at most 8 labels"), Labels, 8);
    TestTrue(TEXT("the high-priority target is labelled"), Out[11].bLabel);
    TestTrue(TEXT("the nearest targets win the remaining budget"), Out[0].bLabel && Out[6].bLabel && !Out[7].bLabel && !Out[10].bLabel);
    TestTrue(TEXT("dot-only targets keep their anchor"), Out[10].Line.Num() == 1 && Out[10].Line[0].Equals(Layout[10].Anchor));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCalloutsStacking, "SCBridge.UI.Callouts.Stacking", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCCalloutsStacking::RunTest(const FString&)
{
    // Three anchors close together on one side stack in a column ordered like their anchors (top label = top anchor).
    TArray<FSCCalloutTarget> Layout = {Target(1, {800, 520}, {12, 12}), Target(2, {820, 540}, {12, 12}), Target(3, {840, 560}, {12, 12})};
    for (FSCCalloutTarget& T : Layout) T.PreviousSide = ESCCalloutSide::NE;
    const TArray<FSCCalloutPlacement> Out = FSCCalloutSolver::Solve(Layout, Screen);
    for (const FSCCalloutPlacement& P : Out) TestEqual(TEXT("all on NE"), P.Side, ESCCalloutSide::NE);
    TestTrue(TEXT("ordered by anchor y"), Out[0].Label.Min.Y < Out[1].Label.Min.Y && Out[1].Label.Min.Y < Out[2].Label.Min.Y);
    TestFalse(TEXT("no crossing in the stack"), FSCCalloutSolver::PolylinesCross(Out[0].Line, Out[1].Line) ||
                                                FSCCalloutSolver::PolylinesCross(Out[1].Line, Out[2].Line) ||
                                                FSCCalloutSolver::PolylinesCross(Out[0].Line, Out[2].Line));
    return true;
}
#endif
