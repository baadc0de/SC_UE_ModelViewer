// Automation tests of the cargo planner (SCBridge.Cargo.*): lattice from a container record, yaw 0/90 placement,
// gravity support, max item size, best fit, drones per load, transfer planning and refusals, achievable amounts.
// Every size, mass and force here is synthetic (1 m cells, round numbers), not game data.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Cargo/SCCargoPlanner.h"

namespace
{
constexpr EAutomationTestFlags CargoTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
// A lattice of 1 m cells, sizes in lattice axes.
FSCCargoGridModel Grid(int32 Id, const FIntVector& Cells, const FVector& MaxItem = FVector::ZeroVector, const FString& Group = TEXT("Synthetic"))
{
    FSCCargoGridModel G;
    G.GridId = Id;
    G.Lattice.Cells = Cells;
    G.Lattice.CellM = FVector(1);
    G.Lattice.InteriorM = FVector(Cells);
    G.Lattice.MaxItemM = MaxItem;
    G.GroupLabel = Group;
    return G;
}
FSCCargoItem Item(int32 Id, const FVector& Size, int32 SCU, FName Commodity = TEXT("Synthetic"), double MassKg = 100)
{
    FSCCargoItem I;
    I.Id = Id;
    I.SizeM = Size;
    I.SCU = SCU;
    I.Commodity = Commodity;
    I.MassKg = MassKg;
    return I;
}
void Put(FSCCargoGridModel& G, int32 Id, const FIntVector& Origin, const FIntVector& Cells)
{
    FSCCargoPlacement P;
    P.ItemId = Id;
    P.Origin = Origin;
    P.Cells = Cells;
    G.Placed.Add(P);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoLatticeTest, "SCBridge.Cargo.LatticeFromContainer", CargoTestFlags)
bool FSCCargoLatticeTest::RunTest(const FString&)
{
    FSCCargoContainerParams C;
    C.InteriorDimensions = FVector(3, 5, 2); // SC x, y, z
    C.MinPermittedItemSize = FVector(1, 1, 0.5);
    C.MaxPermittedItemSize = FVector(2, 4, 2);
    FSCCargoLattice L;
    FString Error;
    TestTrue(TEXT("a container with interior and minimum item size makes a lattice"), FSCCargoLattice::FromContainer(C, L, Error));
    TestEqual(TEXT("lattice axes are SC (y, x, z); cells = interior / minimum item size"), L.Cells, FIntVector(5, 3, 4));
    TestEqual(TEXT("max item size in lattice axes"), L.MaxItemM, FVector(4, 2, 2));
    TestEqual(TEXT("an item spans whole cells, rounded up"), L.CellsFor(FVector(2.2, 1, 1)), FIntVector(3, 1, 2));
    C.MinPermittedItemSize = FVector::ZeroVector;
    TestFalse(TEXT("no minimum item size: no lattice step, no lattice"), FSCCargoLattice::FromContainer(C, L, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoYawTest, "SCBridge.Cargo.PlacementYaw", CargoTestFlags)
bool FSCCargoYawTest::RunTest(const FString&)
{
    FSCCargoGridModel G = Grid(1, FIntVector(4, 2, 1));
    FSCCargoPlacement P;
    TestTrue(TEXT("an item along the grid fits at yaw 0"), FSCCargoPlanner::FindSlot(G, Item(1, FVector(4, 2, 1), 8), P));
    TestFalse(TEXT("yaw 0 needs no turn"), P.bRotated);
    TestTrue(TEXT("an item across the grid fits turned 90 degrees"), FSCCargoPlanner::FindSlot(G, Item(2, FVector(2, 4, 1), 8), P));
    TestTrue(TEXT("it is placed turned"), P.bRotated);
    TestEqual(TEXT("turned footprint"), P.Cells, FIntVector(4, 2, 1));
    TestFalse(TEXT("no tipping: a tall item does not lie down"), FSCCargoPlanner::FindSlot(G, Item(3, FVector(1, 1, 2), 2), P));
    // A hold entered along lattice Y stows boxes lengthwise along Y (in and out through its entrance), else along X.
    FSCCargoGridModel Hold = Grid(5, FIntVector(4, 4, 1));
    Hold.PreferredLongAxis = 2;
    TestTrue(TEXT("hold: placed"), FSCCargoPlanner::FindSlot(Hold, Item(6, FVector(2, 1, 1), 2), P));
    TestTrue(TEXT("hold: long side along the entrance (y)"), P.Cells.Y > P.Cells.X);
    Hold.PreferredLongAxis = 1;
    TestTrue(TEXT("hold: placed (x)"), FSCCargoPlanner::FindSlot(Hold, Item(6, FVector(1, 2, 1), 2), P));
    TestTrue(TEXT("hold: long side along the entrance (x)"), P.Cells.X > P.Cells.Y);
    // Unequal cells (1 m along X, 0.5 m along Y): the turned footprint is the turned physical size in cells, not the
    // yaw-0 cell counts swapped. A 2 x 1 m item only fits a 1 x 4 m grid turned: 1 cell along X, 4 along Y.
    FSCCargoGridModel Narrow = Grid(4, FIntVector(1, 8, 1));
    Narrow.Lattice.CellM = FVector(1, 0.5, 1);
    Narrow.Lattice.InteriorM = FVector(1, 4, 1);
    TestTrue(TEXT("unequal cells: the item fits turned"), FSCCargoPlanner::FindSlot(Narrow, Item(4, FVector(2, 1, 1), 2), P));
    TestTrue(TEXT("unequal cells: placed turned"), P.bRotated);
    TestEqual(TEXT("unequal cells: turned footprint from the turned size"), P.Cells, FIntVector(1, 4, 1));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoGravityTest, "SCBridge.Cargo.GravitySupport", CargoTestFlags)
bool FSCCargoGravityTest::RunTest(const FString&)
{
    FSCCargoGridModel G = Grid(1, FIntVector(2, 2, 2));
    Put(G, 10, FIntVector(0, 0, 0), FIntVector(1, 1, 1));
    FSCCargoPlacement P;
    TestFalse(TEXT("a 2x2 slab cannot overhang a single box"), FSCCargoPlanner::FindSlot(G, Item(1, FVector(2, 2, 1), 4), P));
    Put(G, 11, FIntVector(1, 0, 0), FIntVector(1, 1, 1));
    Put(G, 12, FIntVector(0, 1, 0), FIntVector(1, 1, 1));
    Put(G, 13, FIntVector(1, 1, 0), FIntVector(1, 1, 1));
    TestTrue(TEXT("fully on boxes it rests"), FSCCargoPlanner::FindSlot(G, Item(1, FVector(2, 2, 1), 4), P));
    TestEqual(TEXT("on the second layer"), P.Origin, FIntVector(0, 0, 1));
    FSCCargoGridModel Free = Grid(2, FIntVector(2, 1, 2));
    Put(Free, 20, FIntVector(0, 0, 0), FIntVector(1, 1, 1));
    TestTrue(TEXT("a cube fits"), FSCCargoPlanner::FindSlot(Free, Item(2, FVector(1, 1, 1), 1), P));
    TestEqual(TEXT("lowest layer first, beside the box rather than on it"), P.Origin, FIntVector(1, 0, 0));
    TestEqual(TEXT("items resting on a box"), G.ItemsOn(10).Num(), 0);
    // A dock keeps the cells around it free: a cube goes beyond the ring, not against the dock.
    FSCCargoGridModel Docked = Grid(3, FIntVector(4, 1, 1));
    Put(Docked, 30, FIntVector(0, 0, 0), FIntVector(1, 1, 1));
    Docked.Placed.Last().KeepClear = 1;
    TestTrue(TEXT("dock ring: a slot is found"), FSCCargoPlanner::FindSlot(Docked, Item(3, FVector(1, 1, 1), 1), P));
    TestEqual(TEXT("dock ring: not next to the dock"), P.Origin, FIntVector(3, 0, 0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoMaxItemTest, "SCBridge.Cargo.MaxItemSize", CargoTestFlags)
bool FSCCargoMaxItemTest::RunTest(const FString&)
{
    FSCCargoLattice L = Grid(1, FIntVector(4, 4, 2), FVector(2, 2, 2)).Lattice;
    bool bTurn = false;
    FString Why;
    TestFalse(TEXT("an item longer than the max item size is refused"), FSCCargoPlanner::FitsMaxItem(L, FVector(1, 3, 1), bTurn, &Why, TEXT("Synthetic"), 3));
    TestEqual(TEXT("the reason names length and limit"), Why, FString(TEXT("3 SCU box is 3.0 m long; Synthetic grids take up to 2.0 m")));
    TestFalse(TEXT("a tall item is refused"), FSCCargoPlanner::FitsMaxItem(L, FVector(1, 1, 3), bTurn, &Why, TEXT("Synthetic"), 3));
    TestTrue(TEXT("height reason"), Why.Contains(TEXT("tall")));
    L.MaxItemM = FVector::ZeroVector;
    TestTrue(TEXT("no authored max: the interior bounds"), FSCCargoPlanner::FitsMaxItem(L, FVector(1, 4, 1), bTurn));
    // The max item size holds no orientation: a grid 4 x 4 x 2 m whose max is authored long in z (2 x 2 x 4) takes a box
    // lying 4 m long (placed upright, yaw 0/90), not one 4 x 4 m wide.
    FSCCargoGridModel ZLong = Grid(2, FIntVector(4, 4, 2), FVector(2, 2, 4));
    TestTrue(TEXT("z-long max: a long box fits lying"), FSCCargoPlanner::FitsMaxItem(ZLong.Lattice, FVector(4, 2, 2), bTurn));
    FSCCargoPlacement P;
    TestTrue(TEXT("z-long max: and is placed"), FSCCargoPlanner::FindSlot(ZLong, Item(5, FVector(4, 2, 2), 16), P));
    TestFalse(TEXT("z-long max: too wide in every orientation"), FSCCargoPlanner::FitsMaxItem(ZLong.Lattice, FVector(4, 4, 1), bTurn, &Why, TEXT("Synthetic"), 16));
    TestEqual(TEXT("z-long max: the reason"), Why, FString(TEXT("16 SCU box is 4.0 m wide; Synthetic grids take up to 2.0 m wide")));
    TestFalse(TEXT("z-long max: taller than the interior (boxes stay upright)"), FSCCargoPlanner::FitsMaxItem(ZLong.Lattice, FVector(2, 2, 4), bTurn, &Why));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoPackTest, "SCBridge.Cargo.BestFitPack", CargoTestFlags)
bool FSCCargoPackTest::RunTest(const FString&)
{
    TArray<FSCCargoGridModel> Grids = {Grid(1, FIntVector(3, 2, 1))};
    const TArray<FSCCargoItem> Items = {Item(1, FVector(1, 1, 1), 1), Item(2, FVector(1, 1, 1), 1), Item(3, FVector(2, 2, 1), 4)};
    TArray<FSCCargoPlacement> Slots;
    TArray<int32> GridOf;
    TestTrue(TEXT("largest first packs a 2x2 and two cubes in 3x2"), FSCCargoPlanner::Pack(Grids, Items, Slots, GridOf));
    for (int32 A = 0; A < Slots.Num(); ++A)
        for (int32 B = A + 1; B < Slots.Num(); ++B) TestFalse(TEXT("no overlaps"), Slots[A].Overlaps(Slots[B]));
    TArray<FSCCargoGridModel> Small = {Grid(1, FIntVector(2, 2, 1))};
    TestFalse(TEXT("more volume than the grid does not pack"), FSCCargoPlanner::Pack(Small, Items, Slots, GridOf));
    TestEqual(TEXT("drones: ceil(m g / F)"), FSCCargoPlanner::DronesFor(1000, 10, 4000), 3);
    TestEqual(TEXT("one drone lifts what it can"), FSCCargoPlanner::DronesFor(1000, 10, 10000), 1);
    TestEqual(TEXT("unknown lift: none"), FSCCargoPlanner::DronesFor(1000, 10, 0), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoTransferTest, "SCBridge.Cargo.TransferPlan", CargoTestFlags)
bool FSCCargoTransferTest::RunTest(const FString&)
{
    FSCCargoPlanInput In;
    In.Grids = {Grid(1, FIntVector(8, 2, 2), FVector::ZeroVector, TEXT("Source")), Grid(2, FIntVector(2, 2, 1), FVector(2, 2, 1), TEXT("Dest"))};
    In.Items.Add(1, Item(1, FVector(1, 1, 1), 1));
    In.Items.Add(2, Item(2, FVector(1, 2, 1), 2));
    In.Items.Add(3, Item(3, FVector(3, 1, 1), 3));       // longer than the destination takes
    In.Items.Add(4, Item(4, FVector(1, 1, 1), 1, TEXT("Other")));
    In.Items.Add(5, Item(5, FVector(1, 1, 1), 1));       // under the other commodity's box
    In.Items.Add(6, Item(6, FVector(1, 1, 1), 1, TEXT("Synthetic"), 5000)); // too heavy for two drones
    Put(In.Grids[0], 1, FIntVector(0, 0, 0), FIntVector(1, 1, 1));
    Put(In.Grids[0], 2, FIntVector(1, 0, 0), FIntVector(1, 2, 1));
    Put(In.Grids[0], 3, FIntVector(2, 0, 0), FIntVector(3, 1, 1));
    Put(In.Grids[0], 5, FIntVector(6, 0, 0), FIntVector(1, 1, 1));
    Put(In.Grids[0], 4, FIntVector(6, 0, 1), FIntVector(1, 1, 1));
    Put(In.Grids[0], 6, FIntVector(7, 0, 0), FIntVector(1, 1, 1));
    In.FromGrids = {1};
    In.ToGrids = {2};
    In.Commodity = TEXT("Synthetic");
    In.LiftForceN = 10000;
    In.GravityMS2 = 10;
    In.DronesAvailable = 2;
    In.AmountSCU = 3;
    FSCCargoTransferPreview P = FSCCargoPlanner::Plan(In);
    TestTrue(TEXT("3 SCU of whole boxes that fit is valid"), P.bValid);
    TestEqual(TEXT("planned 3"), P.PlannedSCU, 3);
    TestEqual(TEXT("two moves"), P.Moves.Num(), 2);
    TestEqual(TEXT("achievable stops (the covered box can be dug out)"), P.AchievableSCU, TArray<int32>{1, 2, 3, 4});
    auto Refused = [&](int32 Box) { return P.Refusals.FindByPredicate([&](const FSCCargoRefusal& R) { return R.BoxId == Box; }); };
    TestTrue(TEXT("the long box is refused with its length"), Refused(3) && Refused(3)->Reason.ToString().Contains(TEXT("3.0 m long; Dest grids take up to 2.0 m")));
    TestNull(TEXT("the covered box is not refused: a free drone can hold its cover"), Refused(5));
    TestFalse(TEXT("no digging when free boxes make the amount"), P.Moves.ContainsByPredicate([](const FSCCargoMovePlan& M) { return M.Kind != ESCCargoMoveKind::Carry; }));
    TestTrue(TEXT("the heavy box needs more drones"), Refused(6) && Refused(6)->Reason.ToString().Contains(TEXT("5 drones, 2 available")));
    TestNull(TEXT("another commodity is not part of the plan"), Refused(4));
    In.AmountSCU = 4;
    P = FSCCargoPlanner::Plan(In);
    TestTrue(TEXT("4 SCU digs out the covered box: hold its cover, then carry it"),
             P.Moves.Num() == 4 && P.Moves[2].Kind == ESCCargoMoveKind::Hold && P.Moves[2].BoxId == 4 && P.Moves[3].BoxId == 5 && P.PlannedSCU == 4);
    In.AmountSCU = 5;
    P = FSCCargoPlanner::Plan(In);
    TestEqual(TEXT("an amount past the stops plans the nearest below"), P.PlannedSCU, 4);
    TestNotNull(TEXT("and says so"), P.Refusals.FindByPredicate([](const FSCCargoRefusal& R) { return R.BoxId == INDEX_NONE; }));
    In.Busy.Add(2);
    In.AmountSCU = 3;
    P = FSCCargoPlanner::Plan(In);
    TestEqual(TEXT("a box another job carries is not offered"), P.AchievableSCU, TArray<int32>{1, 2});
    In.Busy.Reset();
    In.Grids[1].Lattice.ExcludedSubTypes = {TEXT("Excluded")};
    In.Items[1].SubType = TEXT("Excluded");
    P = FSCCargoPlanner::Plan(In);
    TestTrue(TEXT("an excluded sub type is refused"), Refused(1) && Refused(1)->Reason.ToString().Contains(TEXT("not accepted")));
    In.ToGrids = {1};
    P = FSCCargoPlanner::Plan(In);
    TestFalse(TEXT("source and destination the same grid: nothing to plan"), P.bValid);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoStackedTest, "SCBridge.Cargo.StackedRemoval", CargoTestFlags)
bool FSCCargoStackedTest::RunTest(const FString&)
{
    // Two stacked boxes of one commodity: the bottom one moves only with the top one, top first.
    FSCCargoPlanInput In;
    In.Grids = {Grid(1, FIntVector(1, 1, 2)), Grid(2, FIntVector(2, 1, 1))};
    In.Items.Add(1, Item(1, FVector(1, 1, 1), 2));
    In.Items.Add(2, Item(2, FVector(1, 1, 1), 1));
    Put(In.Grids[0], 1, FIntVector(0, 0, 0), FIntVector(1, 1, 1));
    Put(In.Grids[0], 2, FIntVector(0, 0, 1), FIntVector(1, 1, 1));
    In.FromGrids = {1};
    In.ToGrids = {2};
    In.Commodity = TEXT("Synthetic");
    In.LiftForceN = 10000;
    In.GravityMS2 = 10;
    In.DronesAvailable = 1;
    In.AmountSCU = 3;
    const FSCCargoTransferPreview P = FSCCargoPlanner::Plan(In);
    TestEqual(TEXT("the bottom box alone is not achievable"), P.AchievableSCU, TArray<int32>{1, 3});
    TestTrue(TEXT("both move"), P.bValid && P.Moves.Num() == 2);
    if (P.Moves.Num() == 2) TestEqual(TEXT("top box first"), P.Moves[0].BoxId, 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoZeroGravityTeamTest, "SCBridge.Cargo.ZeroGravityTeam", CargoTestFlags)
bool FSCCargoZeroGravityTeamTest::RunTest(const FString&)
{
    // Space (WP-K3): nothing to hold up; a team is as large as the acceleration it must keep needs.
    TestEqual(TEXT("weightless: one beam holds any box"), FSCCargoPlanner::DronesFor(30000, 0, 10000), 1);
    TestEqual(TEXT("the reserve still sizes the team: 2 x 10 kN on 30 t + 2 x 100 kg keeps 0.66 m/s^2, 3 keep 0.99, 4 keep 1.31"),
              FSCCargoPlanner::DronesFor(30000, 0, 10000, 100, 1.0, 8), 4);
    TestEqual(TEXT("under gravity the same box needs more"), FSCCargoPlanner::DronesFor(30000, 10, 100000, 100, 1.0, 8), 4);
    TestEqual(TEXT("negative gravity is no input"), FSCCargoPlanner::DronesFor(30000, -1, 10000, 100, 1.0, 8), 0);
    return true;
}
#endif
