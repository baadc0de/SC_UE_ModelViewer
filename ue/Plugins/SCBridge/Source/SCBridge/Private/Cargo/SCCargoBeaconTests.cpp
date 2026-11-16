// Automation tests of freestyle cargo beacons (WP-K2, SCBridge.Cargo.Beacon*): which beacon a loose box joins, names,
// the order a load let go of in flight tries destinations in, and beacons as transfer sources (never destinations).
// Every size, mass and force here is synthetic (1 m cells, round numbers), not game data.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Cargo/SCCargoBeacons.h"
#include "Cargo/SCCargoPlanner.h"

namespace
{
constexpr EAutomationTestFlags BeaconTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
FBox BoxAt(const FVector& CentreCm, const FVector& SizeCm) { return FBox(CentreCm - SizeCm / 2, CentreCm + SizeCm / 2); }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoBeaconJoinTest, "SCBridge.Cargo.BeaconJoin", BeaconTestFlags)
bool FSCCargoBeaconJoinTest::RunTest(const FString&)
{
    const FVector Size(500, 250, 250); // a 5 m box
    TArray<FSCCargoBeaconModel> Beacons = {{100, 7, TEXT("you"), {1}}, {101, 8, TEXT("you"), {2}}, {102, 7, TEXT("them"), {3}}};
    const TMap<int32, FBox> Members = {{1, BoxAt(FVector(0, 0, 0), Size)}, {2, BoxAt(FVector(300, 0, 0), Size)}, {3, BoxAt(FVector(0, 400, 0), Size)}};
    TestEqual(TEXT("a box near a beacon of its origin and owner joins it"), FSCCargoBeacons::Join(Beacons, Members, BoxAt(FVector(0, 700, 0), Size), 7, TEXT("you")), 100);
    TestEqual(TEXT("one from another origin does not (it founds its own)"), FSCCargoBeacons::Join(Beacons, Members, BoxAt(FVector(0, -700, 0), Size), 9, TEXT("you")), INDEX_NONE);
    TestEqual(TEXT("nor one of another owner"), FSCCargoBeacons::Join(Beacons, Members, BoxAt(FVector(0, 800, 0), Size), 7, TEXT("other")), INDEX_NONE);
    TestEqual(TEXT("far from every member: none (gap 10 m > the 5 m box)"), FSCCargoBeacons::Join(Beacons, Members, BoxAt(FVector(0, 1250, 0), Size), 7, TEXT("you")), INDEX_NONE);
    TestEqual(TEXT("near takes the larger box's longest side"), FSCCargoBeacons::NearCm(BoxAt(FVector::ZeroVector, Size), BoxAt(FVector::ZeroVector, FVector(100))), 500.0);
    // Two beacons of the same origin and owner: the nearest member wins.
    Beacons.Add({103, 7, TEXT("you"), {4}});
    TMap<int32, FBox> More = Members;
    More.Add(4, BoxAt(FVector(0, -400, 0), Size));
    TestEqual(TEXT("between two candidates the nearer one"), FSCCargoBeacons::Join(Beacons, More, BoxAt(FVector(0, -650, 0), Size), 7, TEXT("you")), 103);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoBeaconNameTest, "SCBridge.Cargo.BeaconName", BeaconTestFlags)
bool FSCCargoBeaconNameTest::RunTest(const FString&)
{
    int32 Calls = 0;
    auto Fixed = [&Calls](int32 Max) { ++Calls; return 3 % Max; }; // always the same word and number
    const FString First = FSCCargoBeacons::Name(Fixed, {});
    TestTrue(TEXT("names read as loose cargo"), First.StartsWith(TEXT("Loose cargo ")));
    const FString Second = FSCCargoBeacons::Name(Fixed, {First});
    TestNotEqual(TEXT("a name in use is never given twice"), Second, First);
    TestTrue(TEXT("the fallback numbers it"), Second.StartsWith(First));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoBeaconRetargetOrderTest, "SCBridge.Cargo.BeaconRetargetOrder", BeaconTestFlags)
bool FSCCargoBeaconRetargetOrderTest::RunTest(const FString&)
{
    const TArray<TPair<int32, FBox>> Grids = {{1, BoxAt(FVector(5000, 0, 0), FVector(400))}, {2, BoxAt(FVector(1000, 0, 0), FVector(400))}, {3, BoxAt(FVector(3000, 0, 0), FVector(400))}};
    TestEqual(TEXT("nearest first"), FSCCargoBeacons::NearestFirst(Grids, FVector::ZeroVector, {}), TArray<int32>{2, 3, 1});
    TestEqual(TEXT("ones tried are left out (the one it could not reach)"), FSCCargoBeacons::NearestFirst(Grids, FVector::ZeroVector, {2}), TArray<int32>{3, 1});
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoBeaconSourceTest, "SCBridge.Cargo.BeaconSource", BeaconTestFlags)
bool FSCCargoBeaconSourceTest::RunTest(const FString&)
{
    FSCCargoPlanInput In;
    FSCCargoGridModel Dest;
    Dest.GridId = 2;
    Dest.Lattice.Cells = FIntVector(4, 2, 1);
    Dest.Lattice.CellM = FVector(1);
    Dest.Lattice.InteriorM = FVector(4, 2, 1);
    Dest.GroupLabel = TEXT("Dest");
    In.Grids = {Dest};
    auto Item = [](int32 Id, int32 SCU, FName Commodity) {
        FSCCargoItem I;
        I.Id = Id;
        I.SizeM = FVector(SCU, 1, 1);
        I.SCU = SCU;
        I.Commodity = Commodity;
        I.MassKg = 100;
        return I;
    };
    In.Items.Add(11, Item(11, 2, TEXT("Synthetic")));
    In.Items.Add(12, Item(12, 1, TEXT("Synthetic")));
    In.Items.Add(13, Item(13, 1, TEXT("Other")));
    In.Items.Add(14, Item(14, 9, TEXT("Synthetic"))); // longer than the destination
    In.Beacons.Add(500, {11, 12, 13, 14});
    In.BeaconLabels.Add(500, TEXT("Loose cargo Test-01"));
    In.FromGrids = {500};
    In.ToGrids = {2};
    In.Commodity = TEXT("Synthetic");
    In.LiftForceN = 10000;
    In.GravityMS2 = 10;
    In.DronesAvailable = 2;
    In.AmountSCU = 3;
    FSCCargoTransferPreview P = FSCCargoPlanner::Plan(In);
    TestTrue(TEXT("a beacon is a source: 3 SCU of its boxes"), P.bValid && P.PlannedSCU == 3);
    TestTrue(TEXT("moves come from the beacon (its location id), no dig-out"),
             !P.Moves.IsEmpty() && !P.Moves.ContainsByPredicate([](const FSCCargoMovePlan& M) { return M.FromGrid != 500 || M.Kind != ESCCargoMoveKind::Carry; }));
    TestTrue(TEXT("its members are checked like any box (the long one refused)"),
             P.Refusals.ContainsByPredicate([](const FSCCargoRefusal& R) { return R.BoxId == 14; }));
    TestEqual(TEXT("achievable amounts of the commodity"), P.AchievableSCU, TArray<int32>{1, 2, 3});
    // As a destination: refused with the reason (loose cargo is a source only).
    In.FromGrids = {2};
    In.ToGrids = {500};
    P = FSCCargoPlanner::Plan(In);
    TestFalse(TEXT("a beacon as destination plans nothing"), P.bValid);
    TestTrue(TEXT("and says why"), P.Refusals.ContainsByPredicate([](const FSCCargoRefusal& R) { return R.Reason.ToString().Contains(TEXT("source only")); }));
    // A grid and a beacon as sources together.
    FSCCargoGridModel Src;
    Src.GridId = 3;
    Src.Lattice = Dest.Lattice;
    Src.GroupLabel = TEXT("Src");
    FSCCargoPlacement Placed;
    Placed.ItemId = 15;
    Placed.Cells = FIntVector(1, 1, 1);
    Src.Placed.Add(Placed);
    In.Grids.Add(Src);
    In.Items.Add(15, Item(15, 1, TEXT("Synthetic")));
    In.FromGrids = {3, 500};
    In.ToGrids = {2};
    In.AmountSCU = 4;
    P = FSCCargoPlanner::Plan(In);
    TestTrue(TEXT("grid and beacon sources together"), P.bValid && P.PlannedSCU == 4 && P.Moves.ContainsByPredicate([](const FSCCargoMovePlan& M) { return M.FromGrid == 3; }) &&
                                                           P.Moves.ContainsByPredicate([](const FSCCargoMovePlan& M) { return M.FromGrid == 500; }));
    return true;
}
#endif
