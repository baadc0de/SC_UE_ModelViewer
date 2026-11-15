// Automation tests of dig-out planning (SCBridge.Cargo.Dig*): a box under one blocker with two drones (hold + carry +
// put back), under two blockers with two drones (relocate one, hold one), and an impossible dig (refusal with reason).
// Synthetic 1 m cells, 100 kg boxes, one drone per box.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Cargo/SCCargoPlanner.h"

namespace
{
constexpr EAutomationTestFlags DigTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
// Source grid Width x 1 x 3 with a column at x = 0: the target (commodity "Synthetic") at the bottom, Blockers ("Other")
// stacked on it; destination 1 x 1 x 1.
FSCCargoPlanInput Column(int32 Width, int32 Blockers, int32 Drones)
{
    FSCCargoPlanInput In;
    FSCCargoGridModel Source;
    Source.GridId = 1;
    Source.Label = TEXT("Source");
    Source.Lattice.Cells = FIntVector(Width, 1, 3);
    Source.Lattice.CellM = FVector(1);
    Source.Lattice.InteriorM = FVector(Width, 1, 3);
    FSCCargoGridModel Dest = Source;
    Dest.GridId = 2;
    Dest.Label = TEXT("Dest");
    Dest.Lattice.Cells = FIntVector(1, 1, 1);
    Dest.Lattice.InteriorM = FVector(1);
    for (int32 Z = 0; Z <= Blockers; ++Z)
    {
        FSCCargoItem I;
        I.Id = 10 + Z;
        I.SizeM = FVector(1);
        I.SCU = 1;
        I.MassKg = 100;
        I.Commodity = Z == 0 ? FName(TEXT("Synthetic")) : FName(TEXT("Other"));
        In.Items.Add(I.Id, I);
        FSCCargoPlacement P;
        P.ItemId = I.Id;
        P.Origin = FIntVector(0, 0, Z);
        P.Cells = FIntVector(1);
        Source.Placed.Add(P);
    }
    In.Grids = {Source, Dest};
    In.FromGrids = {1};
    In.ToGrids = {2};
    In.Commodity = TEXT("Synthetic");
    In.AmountSCU = 1;
    In.LiftForceN = 10000;
    In.GravityMS2 = 10;
    In.DronesAvailable = Drones;
    return In;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDigHoldTest, "SCBridge.Cargo.DigHold", DigTestFlags)
bool FSCCargoDigHoldTest::RunTest(const FString&)
{
    const FSCCargoTransferPreview P = FSCCargoPlanner::Plan(Column(1, 1, 2));
    TestTrue(TEXT("a box under one blocker is reachable with two drones"), P.bValid && P.PlannedSCU == 1);
    TestEqual(TEXT("hold, then carry (the hold puts its box back afterwards)"), P.Moves.Num(), 2);
    if (P.Moves.Num() == 2)
    {
        TestTrue(TEXT("the blocker is held, then put back where the dug-out box was"), P.Moves[0].Kind == ESCCargoMoveKind::Hold && P.Moves[0].BoxId == 11 &&
                                                                  P.Moves[0].ToGrid == 1 && P.Moves[0].Origin == FIntVector(0, 0, 0));
        TestTrue(TEXT("for the target"), P.Moves[0].ForBoxId == 10);
        TestTrue(TEXT("the target is carried"), P.Moves[1].Kind == ESCCargoMoveKind::Carry && P.Moves[1].BoxId == 10 && P.Moves[1].ToGrid == 2);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDigRelocateTest, "SCBridge.Cargo.DigRelocate", DigTestFlags)
bool FSCCargoDigRelocateTest::RunTest(const FString&)
{
    const FSCCargoTransferPreview P = FSCCargoPlanner::Plan(Column(2, 2, 2));
    TestTrue(TEXT("a box under two blockers is reachable with two drones and a free column"), P.bValid);
    TestEqual(TEXT("relocate, hold, carry"), P.Moves.Num(), 3);
    if (P.Moves.Num() == 3)
    {
        TestTrue(TEXT("the top blocker goes to the free column for good"), P.Moves[0].Kind == ESCCargoMoveKind::Relocate && P.Moves[0].BoxId == 12 &&
                                                                         P.Moves[0].Origin.X == 1);
        TestTrue(TEXT("the lower blocker is held"), P.Moves[1].Kind == ESCCargoMoveKind::Hold && P.Moves[1].BoxId == 11);
        TestTrue(TEXT("then the target"), P.Moves[2].Kind == ESCCargoMoveKind::Carry && P.Moves[2].BoxId == 10);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDigCancelTest, "SCBridge.Cargo.DigCancelReturn", DigTestFlags)
bool FSCCargoDigCancelTest::RunTest(const FString&)
{
    // The hold's return slot was planned for after the target leaves (the floor cell under it). The dig is cancelled
    // with the target still in place: the held box must not go back into the target's cells.
    const FSCCargoPlanInput In = Column(1, 1, 2);
    const FSCCargoTransferPreview P = FSCCargoPlanner::Plan(In);
    const FSCCargoMovePlan* Hold = P.Moves.FindByPredicate([](const FSCCargoMovePlan& M) { return M.Kind == ESCCargoMoveKind::Hold; });
    TestTrue(TEXT("a hold is planned"), Hold != nullptr);
    if (!Hold) return true;
    TestEqual(TEXT("planned back onto the floor once the target is out"), Hold->Origin, FIntVector(0, 0, 0));
    FSCCargoGridModel Source = In.Grids[0]; // as it is now: target at (0,0,0), the held box off the grid
    Source.Placed.RemoveAll([&](const FSCCargoPlacement& X) { return X.ItemId == Hold->BoxId; });
    FSCCargoPlacement Planned;
    Planned.Origin = Hold->Origin;
    Planned.Cells = Hold->Cells;
    FSCCargoPlacement Out;
    TestTrue(TEXT("a slot is found"), FSCCargoPlanner::ReturnSlot(Source, In.Items[Hold->BoxId], Planned, Out));
    TestEqual(TEXT("back on top of the target, not inside it"), Out.Origin, FIntVector(0, 0, 1));
    FSCCargoGridModel Emptied = Source;
    Emptied.Placed.RemoveAll([](const FSCCargoPlacement& X) { return X.ItemId == 10; });
    TestTrue(TEXT("target gone: the planned slot"), FSCCargoPlanner::ReturnSlot(Emptied, In.Items[Hold->BoxId], Planned, Out) && Out.Origin == Planned.Origin);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoQueueTest, "SCBridge.Cargo.QueueNoDeadlock", DigTestFlags)
bool FSCCargoQueueTest::RunTest(const FString&)
{
    // Two jobs, each needing what the other frees: job 1 moves box 20 (Y) from the full grid B to A's free cell; job 2
    // moves box 10 (X) from A to B, which has room only once box 20 is gone. Plans are all or nothing: job 2 reserves
    // nothing while it cannot plan (it queues), so job 1 is never blocked by it, and job 2 plans once job 1 is done.
    FSCCargoPlanInput In;
    FSCCargoGridModel A, B;
    A.GridId = 1; A.Label = TEXT("A"); A.Lattice.Cells = FIntVector(2, 1, 1); A.Lattice.CellM = FVector(1); A.Lattice.InteriorM = FVector(2, 1, 1);
    B.GridId = 2; B.Label = TEXT("B"); B.Lattice.Cells = FIntVector(1, 1, 1); B.Lattice.CellM = FVector(1); B.Lattice.InteriorM = FVector(1, 1, 1);
    auto Box = [&](FSCCargoGridModel& G, int32 Id, FName Commodity, int32 X) {
        FSCCargoItem I;
        I.Id = Id; I.SizeM = FVector(1); I.SCU = 1; I.MassKg = 100; I.Commodity = Commodity;
        In.Items.Add(Id, I);
        FSCCargoPlacement P;
        P.ItemId = Id; P.Origin = FIntVector(X, 0, 0); P.Cells = FIntVector(1);
        G.Placed.Add(P);
    };
    Box(A, 10, TEXT("X"), 0);
    Box(B, 20, TEXT("Y"), 0);
    In.LiftForceN = 10000; In.GravityMS2 = 10; In.DronesAvailable = 2; In.AmountSCU = 1;
    auto Job = [&](int32 From, int32 To, FName Commodity) {
        FSCCargoPlanInput J = In;
        J.Grids = {A, B};
        J.FromGrids = {From}; J.ToGrids = {To}; J.Commodity = Commodity;
        return FSCCargoPlanner::Plan(J);
    };
    const FSCCargoTransferPreview One = Job(2, 1, TEXT("Y"));
    TestTrue(TEXT("job 1 plans (A has a free cell)"), One.bValid && One.Moves.Num() == 1);
    // Job 1 holds its reservation: box 20 busy, A's cell 1 reserved.
    In.Busy.Add(20);
    FSCCargoPlacement Reserved;
    Reserved.ItemId = 20; Reserved.Origin = One.Moves[0].Origin; Reserved.Cells = FIntVector(1);
    A.Placed.Add(Reserved);
    TestFalse(TEXT("job 2 cannot plan yet: it queues, holding nothing"), Job(1, 2, TEXT("X")).bValid);
    // Job 1 done: box 20 lies in A, B is free.
    B.Placed.Reset();
    In.Busy.Remove(20);
    const FSCCargoTransferPreview Two = Job(1, 2, TEXT("X"));
    TestTrue(TEXT("job 2 plans once job 1 is done"), Two.bValid && Two.Moves.Num() == 1 && Two.Moves[0].ToGrid == 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoDigImpossibleTest, "SCBridge.Cargo.DigImpossible", DigTestFlags)
bool FSCCargoDigImpossibleTest::RunTest(const FString&)
{
    const FSCCargoTransferPreview P = FSCCargoPlanner::Plan(Column(1, 2, 2));
    TestFalse(TEXT("two blockers, two drones and no free cell: nothing to plan"), P.bValid);
    const FSCCargoRefusal* R = P.Refusals.FindByPredicate([](const FSCCargoRefusal& X) { return X.BoxId == 10; });
    TestTrue(TEXT("the target is refused with the reason"), R && R->Reason.ToString().Contains(TEXT("covered by 2 boxes")));
    return true;
}
#endif
