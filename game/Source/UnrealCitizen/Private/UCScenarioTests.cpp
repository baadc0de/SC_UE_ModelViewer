// WP-O1-4: scenario definitions, the prep step's records and when the game refuses to run one.
#include "Misc/AutomationTest.h"
#include "SCPackageReader.h"
#include "SCViewerSubsystem.h"
#include "UCScenario.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
const TCHAR* Hangar = TEXT("Data/ObjectContainers/PU/loc/mod/common/hangar/hightech_a/hangar_xltop_001.socpak");
FString Definition(const FString& Extra)
{
    return FString::Printf(TEXT("{\"name\": \"T\", \"environment\": \"%s\", \"vehicle\": \"CRUS_Starlifter_C2\"%s}"), Hangar, *Extra);
}
FString Cargo(const FString& Ship, const FString& Environment, bool bSpace = false)
{
    // The least drone a cargo setup reads with (FSCCargoScenario::Parse).
    return FString::Printf(TEXT("{\"name\": \"c\", \"ship\": \"%s\", \"environment\": \"%s\"%s, \"drone\": {\"tractor_beam\": {\"max_force\": 1000, ")
                           TEXT("\"full_strength_distance\": 3}, \"flight\": {\"max_speed_m_s\": 10, \"max_accel_m_s2\": 12, \"max_jerk_m_s3\": 30}}}"), *Ship, *Environment,
                           bSpace ? TEXT(", \"space\": true") : TEXT(""));
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUCScenarioParseTest, "SCBridge.Scenario.Parse",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FUCScenarioParseTest::RunTest(const FString&)
{
    FUCScenario S;
    FString Error;
    TestTrue(TEXT("a full definition reads"),
             FUCScenario::Parse(TEXT("t"), Definition(TEXT(", \"skin\": \"Pirate\", \"player\": {\"camera\": \"first\", \"start\": \"Pilot\"}, \"music\": \"m1\"")),
                                FString(), S, Error));
    TestEqual(TEXT("vehicle"), S.Vehicle, FString(TEXT("CRUS_Starlifter_C2")));
    TestEqual(TEXT("skin"), S.Skin, FString(TEXT("Pirate")));
    TestEqual(TEXT("start"), S.Start, FString(TEXT("Pilot")));
    TestEqual(TEXT("music"), S.Music, FString(TEXT("m1")));
    TestEqual(TEXT("camera defaults to first"), (FUCScenario::Parse(TEXT("t"), Definition(TEXT("")), FString(), S, Error), S.Camera), FString(TEXT("first")));

    TestFalse(TEXT("no vehicle"), FUCScenario::Parse(TEXT("t"), TEXT("{\"name\": \"x\"}"), FString(), S, Error));
    TestFalse(TEXT("an environment that is no object container"),
              FUCScenario::Parse(TEXT("t"), TEXT("{\"vehicle\": \"V\", \"environment\": \"out/hangars/x\"}"), FString(), S, Error));
    TestFalse(TEXT("an unknown camera"), FUCScenario::Parse(TEXT("t"), Definition(TEXT(", \"player\": {\"camera\": \"top\"}")), FString(), S, Error));
    TestFalse(TEXT("a start without first person"),
              FUCScenario::Parse(TEXT("t"), Definition(TEXT(", \"player\": {\"camera\": \"orbit\", \"start\": \"Pilot\"}")), FString(), S, Error));

    // The cargo setup's grids are chosen for its ship and environment.
    const FString WithCargo = Definition(TEXT(", \"cargo\": \"c\""));
    TestTrue(TEXT("a cargo setup for the scenario's ship and hangar"), FUCScenario::Parse(TEXT("t"), WithCargo, Cargo(TEXT("CRUS_Starlifter_C2"), Hangar), S, Error));
    TestEqual(TEXT("the cargo setup is kept as read"), S.CargoJson, Cargo(TEXT("CRUS_Starlifter_C2"), Hangar));
    TestTrue(TEXT("... the path's slashes do not matter (backslashes, JSON-escaped)"),
             FUCScenario::Parse(TEXT("t"), WithCargo, Cargo(TEXT("crus_starlifter_c2"), FString(Hangar).Replace(TEXT("/"), TEXT("\\\\"))), S, Error));
    TestFalse(TEXT("a cargo setup for another ship"), FUCScenario::Parse(TEXT("t"), WithCargo, Cargo(TEXT("DRAK_Golem"), Hangar), S, Error));
    TestTrue(TEXT("... the error names both"), Error.Contains(TEXT("DRAK_Golem")) && Error.Contains(TEXT("CRUS_Starlifter_C2")));
    TestFalse(TEXT("a cargo setup for another hangar"),
              FUCScenario::Parse(TEXT("t"), WithCargo, Cargo(TEXT("CRUS_Starlifter_C2"), TEXT("Data/ObjectContainers/x/y.socpak")), S, Error));
    TestFalse(TEXT("a cargo setup in space with an environment"),
              FUCScenario::Parse(TEXT("t"), WithCargo, Cargo(TEXT("CRUS_Starlifter_C2"), FString(), true), S, Error));
    TestTrue(TEXT("a cargo setup in space without one"),
             FUCScenario::Parse(TEXT("t"), TEXT("{\"vehicle\": \"CRUS_Starlifter_C2\", \"cargo\": \"c\"}"), Cargo(TEXT("CRUS_Starlifter_C2"), FString(), true), S, Error));
    TestFalse(TEXT("a cargo setup that does not read"), FUCScenario::Parse(TEXT("t"), WithCargo, TEXT("{"), S, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUCScenarioFilesTest, "SCBridge.Scenario.ProjectFiles",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FUCScenarioFilesTest::RunTest(const FString&)
{
    // The project's scenarios read, with their cargo setups.
    FUCScenario S;
    FString Error;
    TestTrue(TEXT("c2_hangar: ") + Error, FUCScenario::Load(TEXT("c2_hangar"), S, Error));
    TestFalse(TEXT("c2_hangar has its cargo setup"), S.CargoJson.IsEmpty());
    TestTrue(TEXT("golem_showcase: ") + Error, FUCScenario::Load(TEXT("golem_showcase"), S, Error));
    TestFalse(TEXT("a name with a path is refused"), FUCScenario::Load(TEXT("../Cargo/fleet"), S, Error));
    TestFalse(TEXT("a missing scenario"), FUCScenario::Load(TEXT("no_such_scenario"), S, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUCPreparedTest, "SCBridge.Scenario.PreparedRecord",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FUCPreparedTest::RunTest(const FString&)
{
    FUCScenario Source;
    FString Error;
    const FString CargoJson = Cargo(TEXT("CRUS_Starlifter_C2"), Hangar);
    TestTrue(TEXT("the source"), FUCScenario::Parse(TEXT("t"), Definition(TEXT(", \"cargo\": \"c\"")), CargoJson, Source, Error));
    FUCPrepared R;
    R.Format = FUCPrepared::FormatVersion;
    R.Scenario = TEXT("t");
    R.Definition = Source.Json;
    R.CargoDefinition = Source.CargoJson;
    R.EnvironmentLevel = TEXT("/Game/SC/Hangars/Maps/hightech_a_hangar_xltop_001");
    R.VehicleLevel = TEXT("/Game/SC/Ships/Maps/CRUS_Starlifter_C2");
    R.Archive = TEXT("build");
    R.PreparedAt = TEXT("2026-11-23T00:00:00.000Z");

    FUCPrepared Back;
    TestTrue(TEXT("the record round-trips"), FUCPrepared::Parse(R.ToJson(), Back, Error));
    TestEqual(TEXT("... its definition byte for byte"), Back.Definition, R.Definition);
    TestEqual(TEXT("... its cargo setup"), Back.CargoDefinition, R.CargoDefinition);
    TestEqual(TEXT("... its levels"), Back.VehicleLevel + Back.EnvironmentLevel, R.VehicleLevel + R.EnvironmentLevel);
    TestEqual(TEXT("... its format"), Back.Format, R.Format);
    TestFalse(TEXT("a record without a vehicle level does not read"), FUCPrepared::Parse(TEXT("{\"scenario\": \"t\", \"definition\": \"{}\"}"), Back, Error));

    auto All = [](const FString&) { return true; };
    TestEqual(TEXT("prepared and unchanged: runs"), R.Problem(&Source, All), FString());
    TestEqual(TEXT("no definition on disk (a packaged game): runs"), R.Problem(nullptr, All), FString());
    FUCPrepared Old = R;
    Old.Format = FUCPrepared::FormatVersion - 1;
    TestTrue(TEXT("another format is stale"), Old.Problem(&Source, All).Contains(TEXT("format")));
    TestTrue(TEXT("... and says what to run"), Old.Problem(&Source, All).Contains(TEXT("-run=SCPrepare -Scenario=t")));
    TestTrue(TEXT("a missing vehicle level"),
             R.Problem(&Source, [&](const FString& L) { return L != R.VehicleLevel; }).Contains(R.VehicleLevel));
    TestTrue(TEXT("a missing environment level"),
             R.Problem(&Source, [&](const FString& L) { return L != R.EnvironmentLevel; }).Contains(R.EnvironmentLevel));
    FUCScenario Changed = Source;
    Changed.Json += TEXT(" ");
    TestTrue(TEXT("a changed definition is stale"), R.Problem(&Changed, All).Contains(TEXT("changed")));
    Changed = Source;
    Changed.CargoJson.Reset();
    TestTrue(TEXT("a changed cargo setup is stale"), R.Problem(&Changed, All).Contains(TEXT("cargo setup")));
    FUCPrepared NoEnvironment = R;
    NoEnvironment.EnvironmentLevel.Reset();
    TestEqual(TEXT("no environment: only the vehicle level is needed"),
              NoEnvironment.Problem(nullptr, [&](const FString& L) { return L == R.VehicleLevel; }), FString());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUCPreparedLevelsTest, "SCBridge.Scenario.LevelNamesAndPaints",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FUCPreparedLevelsTest::RunTest(const FString&)
{
    // Where the prep step writes and the game looks (USCViewerSubsystem::EnvironmentLevelPath / ShipLevelPath).
    TestEqual(TEXT("an object container"), USCViewerSubsystem::EnvironmentLevelPath(Hangar), FString(TEXT("/Game/SC/Hangars/Maps/hightech_a_hangar_xltop_001")));
    TestEqual(TEXT("... with backslashes"), USCViewerSubsystem::EnvironmentLevelPath(FString(Hangar).Replace(TEXT("/"), TEXT("\\"))),
              FString(TEXT("/Game/SC/Hangars/Maps/hightech_a_hangar_xltop_001")));
    TestEqual(TEXT("no object container"), USCViewerSubsystem::EnvironmentLevelPath(TEXT("out/hangars/x")), FString());
    TestEqual(TEXT("a ship"), USCViewerSubsystem::ShipLevelPath(TEXT("CRUS_Starlifter_C2")), FString(TEXT("/Game/SC/Ships/Maps/CRUS_Starlifter_C2")));
    TestEqual(TEXT("a ship with a paint"), USCViewerSubsystem::ShipLevelPath(TEXT("CRUS_Starlifter_C2"), TEXT("crus.starlifter-pirate")),
              FString(TEXT("/Game/SC/Ships/Maps/CRUS_Starlifter_C2__crus_starlifter_pirate")));

    TArray<FSCPaintVariant> Paints;
    Paints.AddDefaulted(2);
    Paints[0].DisplayName = TEXT("Starlifter Pirate");
    Paints[0].PaletteId = TEXT("crus_starlifter_pirate");
    Paints[1].DisplayName = TEXT("Starlifter Ghoulish Green");
    Paints[1].PaletteId = TEXT("crus_starlifter_ghoul");
    int32 Index = 7;
    TestTrue(TEXT("default"), USCViewerSubsystem::ResolvePaint(Paints, TEXT("default"), Index) && Index == INDEX_NONE);
    TestTrue(TEXT("empty is the default"), USCViewerSubsystem::ResolvePaint(Paints, TEXT(""), Index) && Index == INDEX_NONE);
    TestTrue(TEXT("an index"), USCViewerSubsystem::ResolvePaint(Paints, TEXT("1"), Index) && Index == 1);
    TestTrue(TEXT("a part of the name"), USCViewerSubsystem::ResolvePaint(Paints, TEXT("Ghoulish"), Index) && Index == 1);
    TestTrue(TEXT("a part of the palette id"), USCViewerSubsystem::ResolvePaint(Paints, TEXT("pirate"), Index) && Index == 0);
    TestFalse(TEXT("an index past the paints"), USCViewerSubsystem::ResolvePaint(Paints, TEXT("2"), Index));
    TestFalse(TEXT("a name of none"), USCViewerSubsystem::ResolvePaint(Paints, TEXT("Wikelo"), Index));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
