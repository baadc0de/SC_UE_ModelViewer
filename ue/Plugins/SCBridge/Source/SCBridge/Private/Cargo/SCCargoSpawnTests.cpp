// Automation tests of live cargo spawning (SCBridge.Cargo.Spawn*): box records from DataCore JSON, the catalogue by
// commodity, spawn plans on an empty and a filled grid (counts, valid slots, SCU), refusals, clears, and a transfer of
// spawned boxes. Every record, size and GUID here is synthetic (1 m cells, round numbers), not game data.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Cargo/SCCargoSpawn.h"

namespace
{
constexpr EAutomationTestFlags SpawnTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
// A lattice of 1 m cells, sizes in lattice axes.
FSCCargoGridModel Grid(int32 Id, const FIntVector& Cells, const FVector& MaxItem = FVector::ZeroVector)
{
    FSCCargoGridModel G;
    G.GridId = Id;
    G.Lattice.Cells = Cells;
    G.Lattice.CellM = FVector(1);
    G.Lattice.InteriorM = FVector(Cells);
    G.Lattice.MaxItemM = MaxItem;
    G.Label = TEXT("Grid");
    G.GroupLabel = TEXT("Synthetic");
    return G;
}
// A box record; DimensionsSC in SC axes (lattice X = SC y).
FSCCargoBoxRecord Record(const FString& Entity, FName Commodity, int32 SCU, const FVector& DimensionsSC, const FString& Display = FString())
{
    FSCCargoBoxRecord R;
    R.Entity = Entity;
    R.Commodity = Commodity;
    R.CommodityLabel = FText::FromName(Commodity);
    R.DisplayName = Display;
    R.SCU = SCU;
    R.DimensionsSC = DimensionsSC;
    R.SubType = TEXT("Cargo");
    return R;
}
/** 1, 8, 16 and 32 SCU boxes of one commodity: 1x1x1, 2x2x2, 2x4x2 and 2x8x2 m (SC axes). */
TArray<FSCCargoBoxRecord> Sizes(FName Commodity = TEXT("Synthetic"))
{
    return {Record(TEXT("Box32"), Commodity, 32, FVector(2, 8, 2)), Record(TEXT("Box16"), Commodity, 16, FVector(2, 4, 2)),
            Record(TEXT("Box8"), Commodity, 8, FVector(2, 2, 2)), Record(TEXT("Box1"), Commodity, 1, FVector(1, 1, 1))};
}
TArray<const FSCCargoBoxRecord*> Ptrs(const TArray<FSCCargoBoxRecord>& Records)
{
    TArray<const FSCCargoBoxRecord*> Out;
    for (const FSCCargoBoxRecord& R : Records) Out.Add(&R);
    return Out;
}
FSCCargoSpawnRequest Request(int32 GridId, int32 Count, int32 AmountSCU = 0, int32 SizeSCU = 0)
{
    FSCCargoSpawnRequest R;
    R.Grids = {GridId};
    R.What = TEXT("Synthetic");
    R.Count = Count;
    R.AmountSCU = AmountSCU;
    R.SizeSCU = SizeSCU;
    return R;
}
void Put(FSCCargoGridModel& G, int32 Id, const FIntVector& Origin, const FIntVector& Cells)
{
    FSCCargoPlacement P;
    P.ItemId = Id;
    P.Origin = Origin;
    P.Cells = Cells;
    G.Placed.Add(P);
}
/** Puts the plan's boxes on G in their order, each checked free and supported as SpawnBox checks it; ids from FirstId. */
bool PutPlan(FAutomationTestBase& T, FSCCargoGridModel& G, const FSCCargoSpawnPreview& P, int32 FirstId)
{
    bool bOK = true;
    for (int32 I = 0; I < P.Boxes.Num(); ++I)
    {
        const FSCCargoSpawnBoxPlan& B = P.Boxes[I];
        const bool bFree = G.IsFree(B.Origin, B.Cells), bSupported = G.IsSupported(B.Origin, B.Cells);
        T.TestTrue(FString::Printf(TEXT("box %d (%d SCU at %s) is on a free cell"), I, B.SCU, *B.Origin.ToString()), bFree);
        T.TestTrue(FString::Printf(TEXT("box %d (%d SCU at %s) rests on the floor or on boxes"), I, B.SCU, *B.Origin.ToString()), bSupported);
        bOK &= bFree && bSupported;
        Put(G, FirstId + I, B.Origin, B.Cells);
    }
    return bOK;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoSpawnRecordTest, "SCBridge.Cargo.SpawnRecord", SpawnTestFlags)
bool FSCCargoSpawnRecordTest::RunTest(const FString&)
{
    // The FFI's shape: `__type`, references as GUID strings.
    const FString Box = TEXT(R"({"__type":"EntityClassDefinition","__record_name":"EntityClassDefinition.Box16","Components":[
        {"__type":"SAttachableComponentParams","AttachDef":{"__type":"SItemDefinition","Type":"Cargo","SubType":"Cargo",
          "inventoryOccupancyVolume":{"__type":"SStandardCargoUnit","standardCargoUnits":16.0},
          "inventoryOccupancyDimensions":{"__type":"Vec3","x":2.5,"y":5.0,"z":2.5}}},
        {"__type":"ResourceContainer","inclusiveGroups":["00000000-0000-0000-0000-00000000000a"],
          "defaultComposition":[{"__type":"ResourceContainerDefaultCompositionEntry","entry":"00000000-0000-0000-0000-00000000000b","weight":1.0}]}]})");
    FSCCargoBoxRecord R;
    FString Ref, Why;
    bool bGroup = true;
    TestTrue(TEXT("a whole-SCU box holding a resource is a box record"), SCCargoSpawn::ParseBoxRecord(Box, R, Ref, bGroup, Why));
    TestEqual(TEXT("SCU from the occupancy volume"), R.SCU, 16);
    TestEqual(TEXT("dimensions as authored (SC axes)"), R.DimensionsSC, FVector(2.5, 5, 2.5));
    TestEqual(TEXT("lattice axes swap x and y"), R.SizeM(), FVector(5, 2.5, 2.5));
    TestEqual(TEXT("sub type"), R.SubType, FString(TEXT("Cargo")));
    TestEqual(TEXT("the commodity is the default composition's resource"), Ref, FString(TEXT("00000000-0000-0000-0000-00000000000b")));
    TestFalse(TEXT("a resource, not a group"), bGroup);
    // A mixed box: no composition, its group.
    const FString Mixed = Box.Replace(TEXT(R"([{"__type":"ResourceContainerDefaultCompositionEntry","entry":"00000000-0000-0000-0000-00000000000b","weight":1.0}])"), TEXT("[]"));
    TestTrue(TEXT("a mixed box is a box record"), SCCargoSpawn::ParseBoxRecord(Mixed, R, Ref, bGroup, Why));
    TestEqual(TEXT("its commodity is its group"), Ref, FString(TEXT("00000000-0000-0000-0000-00000000000a")));
    TestTrue(TEXT("a group"), bGroup);
    // The CLI's shape: `_Type_`, reference objects carrying the record name.
    const FString Cli = TEXT(R"({"_RecordValue_":{"_Type_":"EntityClassDefinition","Components":[
        {"_Type_":"SAttachableComponentParams","AttachDef":{"_Type_":"SItemDefinition","SubType":"Cargo",
          "inventoryOccupancyVolume":{"_Type_":"SStandardCargoUnit","standardCargoUnits":2.0},
          "inventoryOccupancyDimensions":{"_Type_":"Vec3","x":1.25,"y":2.5,"z":1.25}}},
        {"_Type_":"ResourceContainer","inclusiveGroups":[],"defaultComposition":[{"_Type_":"ResourceContainerDefaultCompositionEntry",
          "entry":{"_RecordName_":"ResourceType.Synthetic","_RecordId_":"00000000-0000-0000-0000-00000000000c"}}]}]}})");
    TestTrue(TEXT("the CLI's shape reads too"), SCCargoSpawn::ParseBoxRecord(Cli, R, Ref, bGroup, Why));
    TestEqual(TEXT("a reference object gives its name"), Ref, FString(TEXT("ResourceType.Synthetic")));
    TestEqual(TEXT("2 SCU"), R.SCU, 2);
    // Not boxes: an eighth-SCU hand box, a box holding no resource.
    const FString Eighth = Box.Replace(TEXT(R"({"__type":"SStandardCargoUnit","standardCargoUnits":16.0})"), TEXT(R"({"__type":"SMicroCargoUnit","microSCU":125000})"));
    TestFalse(TEXT("an occupancy in another unit is no whole-SCU box"), SCCargoSpawn::ParseBoxRecord(Eighth, R, Ref, bGroup, Why));
    const FString Fraction = Box.Replace(TEXT(R"("standardCargoUnits":16.0)"), TEXT(R"("standardCargoUnits":0.125)"));
    TestFalse(TEXT("a fraction of an SCU is no whole-SCU box"), SCCargoSpawn::ParseBoxRecord(Fraction, R, Ref, bGroup, Why));
    const FString Empty = Box.Replace(TEXT(R"("__type":"ResourceContainer")"), TEXT(R"("__type":"SomethingElse")"));
    TestFalse(TEXT("no resource container: no commodity"), SCCargoSpawn::ParseBoxRecord(Empty, R, Ref, bGroup, Why));
    TestEqual(TEXT("a record's short name"), SCCargoSpawn::RecordShortName(TEXT(R"({"__record_name":"ResourceType.Synthetic"})")), FString(TEXT("Synthetic")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoSpawnCatalogTest, "SCBridge.Cargo.SpawnCatalog", SpawnTestFlags)
bool FSCCargoSpawnCatalogTest::RunTest(const FString&)
{
    FSCCargoBoxCatalog C;
    C.Add(Record(TEXT("Ore8"), TEXT("OreRefined"), 8, FVector(2, 2, 2), TEXT("Ore (Refined)")));
    C.Add(Record(TEXT("Ore1"), TEXT("OreRefined"), 1, FVector(1, 1, 1), TEXT("Ore (Refined)")));
    C.Add(Record(TEXT("Ore1Alt"), TEXT("OreRefined"), 1, FVector(1, 1, 1), TEXT("Ore (Refined) Alt")));
    C.Add(Record(TEXT("Raw8"), TEXT("OreRaw"), 8, FVector(2, 2, 2), TEXT("Ore (Raw)")));
    C.Add(Record(TEXT("Food4"), TEXT("Food"), 4, FVector(2, 2, 1)));
    C.Add(Record(TEXT("MixA"), TEXT("MixA"), 8, FVector(2, 2, 2), TEXT("<placeholder>")));
    C.Add(Record(TEXT("MixB"), TEXT("MixB"), 8, FVector(2, 2, 2), TEXT("<placeholder>")));
    C.Finish();
    TestEqual(TEXT("records"), C.Num(), 7);
    TestEqual(TEXT("a name boxes of two commodities share names neither"), C.LabelOf(TEXT("MixA")).ToString(), FString(TEXT("MixA")));
    TestEqual(TEXT("label: the name most of its boxes share"), C.LabelOf(TEXT("OreRefined")).ToString(), FString(TEXT("Ore (Refined)")));
    TestEqual(TEXT("label without names: its key"), C.LabelOf(TEXT("Food")).ToString(), FString(TEXT("Food")));
    FString Why;
    TestEqual(TEXT("by key, any case"), C.FindCommodity(TEXT("orerefined"), Why), FName(TEXT("OreRefined")));
    TestEqual(TEXT("by label"), C.FindCommodity(TEXT("ore (raw)"), Why), FName(TEXT("OreRaw")));
    TestEqual(TEXT("by text unique to one"), C.FindCommodity(TEXT("Foo"), Why), FName(TEXT("Food")));
    TestTrue(TEXT("text in two is ambiguous"), C.FindCommodity(TEXT("Ore"), Why).IsNone() && Why.Contains(TEXT("matches 2")));
    TestTrue(TEXT("unknown"), C.FindCommodity(TEXT("Nothing"), Why).IsNone());
    const TArray<const FSCCargoBoxRecord*> Boxes = C.BoxesOf(TEXT("OreRefined"));
    TestEqual(TEXT("every record of the commodity"), Boxes.Num(), 3);
    TestTrue(TEXT("largest first, then by entity"), Boxes.Num() == 3 && Boxes[0]->SCU == 8 && Boxes[1]->Entity == TEXT("Ore1"));
    TestNotNull(TEXT("by entity, any case"), C.FindEntity(TEXT("raw8")));
    const TArray<FSCCargoSpawnCommodity> List = C.Commodities();
    TestEqual(TEXT("five commodities"), List.Num(), 5);
    const FSCCargoSpawnCommodity* Ore = List.FindByPredicate([](const FSCCargoSpawnCommodity& X) { return X.Commodity == TEXT("OreRefined"); });
    TestTrue(TEXT("sizes ascending with their dimensions"), Ore && Ore->SizesSCU == TArray<int32>{1, 8} && Ore->SizesM[1] == FVector(2, 2, 2));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoSpawnEmptyGridTest, "SCBridge.Cargo.SpawnEmptyGrid", SpawnTestFlags)
bool FSCCargoSpawnEmptyGridTest::RunTest(const FString&)
{
    const TArray<FSCCargoBoxRecord> Records = Sizes();
    const FSCCargoGridModel Empty = Grid(1, FIntVector(4, 8, 2), FVector(4, 4, 2)); // a 32 SCU box (8 m) is longer than it takes
    // A count, the largest size that fits.
    FSCCargoSpawnPreview P = SCCargoSpawn::Plan({Empty}, Ptrs(Records), Request(1, 3), 100);
    TestTrue(TEXT("three boxes fit"), P.bValid);
    TestEqual(TEXT("three boxes"), P.Boxes.Num(), 3);
    TestEqual(TEXT("of the largest size that fits (16, not 32)"), P.TotalSCU, 48);
    TestTrue(TEXT("every one 16 SCU on grid 1"), !P.Boxes.ContainsByPredicate([](const FSCCargoSpawnBoxPlan& B) { return B.SCU != 16 || B.GridId != 1 || B.Entity != TEXT("Box16"); }));
    FSCCargoGridModel G = Empty;
    PutPlan(*this, G, P, 100);
    // An amount: whole boxes, largest first (16 + 16 + 8).
    P = SCCargoSpawn::Plan({Empty}, Ptrs(Records), Request(1, 0, 40), 100);
    TestTrue(TEXT("40 SCU fits"), P.bValid);
    TestEqual(TEXT("40 SCU"), P.TotalSCU, 40);
    TestEqual(TEXT("in three boxes"), P.Boxes.Num(), 3);
    G = Empty;
    PutPlan(*this, G, P, 100);
    // A size and a count: stacked when the floor is full (8 SCU boxes of 2 m are as tall as the grid: one layer).
    const FSCCargoGridModel Tall = Grid(2, FIntVector(4, 8, 4), FVector(4, 4, 4));
    P = SCCargoSpawn::Plan({Tall}, Ptrs(Records), Request(2, 10, 0, 8), 100);
    TestTrue(TEXT("ten 8 SCU boxes fit a 4 x 8 x 4 grid"), P.bValid);
    TestEqual(TEXT("80 SCU"), P.TotalSCU, 80);
    TestTrue(TEXT("some on the second layer"), P.Boxes.ContainsByPredicate([](const FSCCargoSpawnBoxPlan& B) { return B.Origin.Z == 2; }));
    G = Tall;
    PutPlan(*this, G, P, 100);
    // Next to what is there: the free cells only.
    FSCCargoGridModel Used = Empty;
    Put(Used, 7, FIntVector(0, 0, 0), FIntVector(4, 4, 2));
    P = SCCargoSpawn::Plan({Used}, Ptrs(Records), Request(1, 2), 100);
    TestTrue(TEXT("two 16 SCU boxes beside a box taking half the grid"), P.bValid && P.TotalSCU == 32);
    PutPlan(*this, Used, P, 100);
    // One box of a named entity: its record alone.
    P = SCCargoSpawn::Plan({Empty}, {&Records[3]}, Request(1, 0), 100);
    TestTrue(TEXT("one 1 SCU box"), P.bValid && P.Boxes.Num() == 1 && P.TotalSCU == 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoSpawnRefusalTest, "SCBridge.Cargo.SpawnRefusals", SpawnTestFlags)
bool FSCCargoSpawnRefusalTest::RunTest(const FString&)
{
    const TArray<FSCCargoBoxRecord> Records = Sizes();
    const FSCCargoGridModel Empty = Grid(1, FIntVector(4, 8, 2), FVector(4, 4, 2));
    FSCCargoSpawnPreview P = SCCargoSpawn::Plan({Empty}, Ptrs(Records), Request(1, 1, 0, 3), 100);
    TestTrue(TEXT("a size the data has no box of"), !P.bValid && P.Refused == TEXT("size") && P.Reason.ToString().Contains(TEXT("1, 8, 16, 32")));
    P = SCCargoSpawn::Plan({Empty}, Ptrs(Records), Request(1, 1, 0, 32), 100);
    TestTrue(TEXT("a box longer than the grid takes, with the planner's reason"), !P.bValid && P.Refused == TEXT("size") && P.Reason.ToString().Contains(TEXT("8.0 m long")));
    P = SCCargoSpawn::Plan({Empty}, Ptrs(Records), Request(1, 5, 0, 16), 100);
    TestTrue(TEXT("five 16 SCU boxes on a grid that holds four"), !P.bValid && P.Refused == TEXT("full"));
    TestEqual(TEXT("four of them fit"), P.FitCount, 4);
    TestTrue(TEXT("says so"), P.Reason.ToString().Contains(TEXT("only 4 of 5")));
    FSCCargoGridModel Full = Empty;
    Put(Full, 7, FIntVector(0, 0, 0), FIntVector(4, 8, 2));
    P = SCCargoSpawn::Plan({Full}, Ptrs(Records), Request(1, 1), 100);
    TestTrue(TEXT("a full grid takes nothing"), !P.bValid && P.Refused == TEXT("full") && P.FitCount == 0);
    P = SCCargoSpawn::Plan({Empty}, Ptrs(Records), Request(1, 0, 20, 16), 100);
    TestTrue(TEXT("20 SCU is no whole number of 16 SCU boxes"), !P.bValid && P.Refused == TEXT("amount"));
    TArray<FSCCargoBoxRecord> Big = {Records[0], Records[1], Records[2]}; // no 1 SCU box
    P = SCCargoSpawn::Plan({Empty}, Ptrs(Big), Request(1, 0, 7), 100);
    TestTrue(TEXT("7 SCU cannot be made of 8 and 16 SCU boxes"), !P.bValid && P.Refused == TEXT("amount"));
    P = SCCargoSpawn::Plan({Empty}, Ptrs(Records), Request(1, SCCargoSpawn::MaxBoxes + 1, 0, 1), 100);
    TestTrue(TEXT("too many boxes for one spawn"), !P.bValid && P.Refused == TEXT("amount"));
    P = SCCargoSpawn::Plan({Empty}, {}, Request(1, 1), 100);
    TestTrue(TEXT("no box of the commodity"), !P.bValid && P.Refused == TEXT("commodity"));
    FSCCargoGridModel Excludes = Empty;
    Excludes.Lattice.ExcludedSubTypes = {TEXT("cargo")};
    P = SCCargoSpawn::Plan({Excludes}, Ptrs(Records), Request(1, 1), 100);
    TestTrue(TEXT("a grid that excludes the boxes' sub type"), !P.bValid && P.Refused == TEXT("size"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoSpawnClearTest, "SCBridge.Cargo.SpawnClear", SpawnTestFlags)
bool FSCCargoSpawnClearTest::RunTest(const FString&)
{
    FSCCargoGridModel G = Grid(1, FIntVector(8, 2, 3));
    Put(G, 1, FIntVector(0, 0, 0), FIntVector(1, 1, 1)); // A
    Put(G, 2, FIntVector(0, 0, 1), FIntVector(1, 1, 1)); // B on A
    Put(G, 3, FIntVector(2, 0, 0), FIntVector(1, 1, 1)); // C alone
    Put(G, 4, FIntVector(4, 0, 0), FIntVector(1, 1, 1)); // E
    Put(G, 5, FIntVector(4, 0, 1), FIntVector(1, 1, 1)); // F on E
    Put(G, 6, FIntVector(4, 0, 2), FIntVector(1, 1, 1)); // D on F: a box of a transfer
    Put(G, 7, FIntVector(6, 0, 0), FIntVector(1, 1, 1)); // H
    Put(G, 8, FIntVector(6, 0, 1), FIntVector(1, 1, 1)); // a slot reserved for an incoming box, on H
    Put(G, 9, FIntVector(7, 0, 0), FIntVector(1, 2, 1)); // a dock
    const TSet<int32> Candidates = {1, 2, 3, 4, 5, 7};
    TArray<int32> Out = SCCargoSpawn::Clearable(G, Candidates);
    Out.Sort();
    TestEqual(TEXT("everything free goes; what a transfer's box or an incoming slot rests on stays, however deep"), Out, TArray<int32>{1, 2, 3});
    // Removed together: whatever rests only on removed boxes goes with them.
    TestEqual(TEXT("a stack goes whole"), SCCargoSpawn::Clearable(G, {1, 2}).Num(), 2);
    TestEqual(TEXT("nothing to clear"), SCCargoSpawn::Clearable(G, {}).Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoSpawnTransferTest, "SCBridge.Cargo.SpawnThenTransfer", SpawnTestFlags)
bool FSCCargoSpawnTransferTest::RunTest(const FString&)
{
    // Spawned boxes are stock like any: planned on an empty grid, put there, then a transfer of them to another grid.
    const TArray<FSCCargoBoxRecord> Records = Sizes();
    FSCCargoPlanInput In;
    In.Grids = {Grid(1, FIntVector(4, 8, 2), FVector(4, 4, 2)), Grid(2, FIntVector(4, 8, 2), FVector(4, 4, 2))};
    const FSCCargoSpawnPreview P = SCCargoSpawn::Plan({In.Grids[0]}, Ptrs(Records), Request(1, 0, 40), 100);
    TestTrue(TEXT("40 SCU spawned"), P.bValid && P.TotalSCU == 40);
    TestTrue(TEXT("on valid slots"), PutPlan(*this, In.Grids[0], P, 100));
    for (int32 I = 0; I < P.Boxes.Num(); ++I)
    {
        const FSCCargoBoxRecord* R = Records.FindByPredicate([&](const FSCCargoBoxRecord& X) { return X.Entity == P.Boxes[I].Entity; });
        FSCCargoItem Item;
        Item.Id = 100 + I;
        Item.SizeM = R->SizeM();
        Item.SCU = R->SCU;
        Item.SubType = R->SubType;
        Item.Commodity = R->Commodity;
        Item.MassKg = 100;
        In.Items.Add(Item.Id, Item);
    }
    In.FromGrids = {1};
    In.ToGrids = {2};
    In.Commodity = TEXT("Synthetic");
    In.LiftForceN = 10000;
    In.GravityMS2 = 10;
    In.DronesAvailable = 2;
    In.AmountSCU = 40;
    const FSCCargoTransferPreview T = FSCCargoPlanner::Plan(In);
    TestTrue(TEXT("the transfer of the spawned boxes plans"), T.bValid);
    TestEqual(TEXT("all 40 SCU"), T.PlannedSCU, 40);
    TestEqual(TEXT("a move per box"), T.Moves.Num(), P.Boxes.Num());
    TestTrue(TEXT("16 SCU alone is achievable too"), T.AchievableSCU.Contains(16));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoSpawnNoSupportTest, "SCBridge.Cargo.SpawnNoSupport", SpawnTestFlags)
bool FSCCargoSpawnNoSupportTest::RunTest(const FString&)
{
    // A slot reserved for an incoming box (or a box about to leave) occupies its cells but bears nothing.
    const TArray<FSCCargoBoxRecord> Records = Sizes();
    FSCCargoGridModel G = Grid(1, FIntVector(2, 1, 2));
    Put(G, 50, FIntVector(0, 0, 0), FIntVector(1, 1, 1)); // reserved for an incoming box
    Put(G, 51, FIntVector(1, 0, 0), FIntVector(1, 1, 1)); // a box at rest
    const TArray<const FSCCargoBoxRecord*> One = {&Records[3]};
    FSCCargoSpawnPreview P = SCCargoSpawn::Plan({G}, One, Request(1, 1), 100, {50});
    TestTrue(TEXT("a 1 SCU box fits"), P.bValid && P.Boxes.Num() == 1);
    TestTrue(TEXT("on the box at rest, not on the reserved slot"), P.Boxes.Num() == 1 && P.Boxes[0].Origin == FIntVector(1, 0, 1));
    P = SCCargoSpawn::Plan({G}, One, Request(1, 2), 100, {50});
    TestTrue(TEXT("two do not: nothing rests on the reserved slot"), !P.bValid && P.Refused == TEXT("full") && P.FitCount == 1);
    P = SCCargoSpawn::Plan({G}, One, Request(1, 2), 100);
    TestTrue(TEXT("without it, the slot bears like any box"), P.bValid);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoSpawnExactMixTest, "SCBridge.Cargo.SpawnExactMix", SpawnTestFlags)
bool FSCCargoSpawnExactMixTest::RunTest(const FString&)
{
    // Room for one 32 SCU box (8 m) and four 24 SCU boxes (6 m): 128 SCU is 32 + 4 x 24, not 4 x 32.
    const TArray<FSCCargoBoxRecord> Records = {Record(TEXT("Box32"), TEXT("Synthetic"), 32, FVector(2, 8, 2)), Record(TEXT("Box24"), TEXT("Synthetic"), 24, FVector(2, 6, 2))};
    TArray<FSCCargoGridModel> Grids = {Grid(1, FIntVector(8, 2, 2), FVector(8, 8, 2))};
    for (int32 I = 0; I < 4; ++I) Grids.Add(Grid(2 + I, FIntVector(6, 2, 2), FVector(6, 6, 2)));
    FSCCargoSpawnRequest R = Request(1, 0, 128);
    R.Grids = {1, 2, 3, 4, 5};
    const FSCCargoSpawnPreview P = SCCargoSpawn::Plan(Grids, Ptrs(Records), R, 100);
    TestTrue(TEXT("128 SCU fits as a mix"), P.bValid && P.TotalSCU == 128);
    TestEqual(TEXT("five boxes"), P.Boxes.Num(), 5);
    TestEqual(TEXT("one of them 32 SCU"), P.Boxes.FilterByPredicate([](const FSCCargoSpawnBoxPlan& B) { return B.SCU == 32; }).Num(), 1);
    // A variant of a size that a grid excludes gives way to one it takes.
    FSCCargoBoxRecord Excluded = Record(TEXT("Box8Hazard"), TEXT("Synthetic"), 8, FVector(2, 2, 2));
    Excluded.SubType = TEXT("Hazard");
    const FSCCargoBoxRecord Plain = Record(TEXT("Box8Plain"), TEXT("Synthetic"), 8, FVector(2, 2, 2));
    FSCCargoGridModel NoHazard = Grid(9, FIntVector(4, 4, 2));
    NoHazard.Lattice.ExcludedSubTypes = {TEXT("Hazard")};
    const FSCCargoSpawnPreview V = SCCargoSpawn::Plan({NoHazard}, {&Excluded, &Plain}, Request(9, 1), 100);
    TestTrue(TEXT("the variant the grid takes"), V.bValid && V.Boxes.Num() == 1 && V.Boxes[0].Entity == TEXT("Box8Plain"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoSpawnWordsTest, "SCBridge.Cargo.SpawnWords", SpawnTestFlags)
bool FSCCargoSpawnWordsTest::RunTest(const FString&)
{
    using SCCargoSpawn::EAmountWord;
    int32 Scu = 0, Count = 0;
    TestTrue(TEXT("48 is 48 SCU"), SCCargoSpawn::ParseAmountWord(TEXT("48"), Scu, Count) == EAmountWord::Amount && Scu == 48 && Count == 0);
    TestTrue(TEXT("x3 is three boxes"), SCCargoSpawn::ParseAmountWord(TEXT("x3"), Scu, Count) == EAmountWord::Amount && Count == 3 && Scu == 0);
    for (const TCHAR* Bad : {TEXT("0"), TEXT("x0"), TEXT("2.5"), TEXT("-3"), TEXT("x2.5"), TEXT("1234567")})
        TestTrue(FString::Printf(TEXT("%s is no amount, and says so"), Bad), SCCargoSpawn::ParseAmountWord(Bad, Scu, Count) == EAmountWord::Invalid);
    for (const TCHAR* Word : {TEXT("Organic"), TEXT("xenon"), TEXT("x")})
        TestTrue(FString::Printf(TEXT("%s is a word of the name"), Word), SCCargoSpawn::ParseAmountWord(Word, Scu, Count) == EAmountWord::NotAmount);
    int32 Size = 0;
    TestTrue(TEXT("--size=16"), SCCargoSpawn::ParseWhole(TEXT("16"), Size) && Size == 16);
    TestFalse(TEXT("--size=8junk"), SCCargoSpawn::ParseWhole(TEXT("8junk"), Size));
    TestFalse(TEXT("--size=0"), SCCargoSpawn::ParseWhole(TEXT("0"), Size));
    return true;
}
#endif
