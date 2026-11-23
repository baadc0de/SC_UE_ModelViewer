// WP-O1-4: scenario definitions, the prep step's records and when the game refuses to run one.
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SCBridgeClient.h"
#include "SCPackageReader.h"
#include "SCViewerSubsystem.h"
#include "UCScenario.h"
#include "UCScenarioSubsystem.h"

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
    TestTrue(TEXT("... the body defaults to the exporter's, all weapons, spares"),
             S.PlayerGender.IsEmpty() && S.PlayerLoadout.IsEmpty() && S.PlayerWeapons.IsEmpty() && S.bPlayerSpares);
    TestTrue(TEXT("a body"), FUCScenario::Parse(TEXT("t"), Definition(TEXT(", \"player\": {\"gender\": \"female\", \"loadout\": \"Data\\\\Scripts\\\\L.xml\", ")
                                                                    TEXT("\"weapons\": [\"behr_rifle_ballistic_01\", \" gmni_lmg \"], \"spares\": false}")),
                                                 FString(), S, Error));
    TestEqual(TEXT("... gender"), S.PlayerGender, FString(TEXT("female")));
    TestEqual(TEXT("... loadout, slashes normalised"), S.PlayerLoadout, FString(TEXT("Data/Scripts/L.xml")));
    TestEqual(TEXT("... weapons"), FString::Join(S.PlayerWeapons, TEXT(",")), FString(TEXT("behr_rifle_ballistic_01,gmni_lmg")));
    TestFalse(TEXT("... spares"), S.bPlayerSpares);
    TestFalse(TEXT("weapons that are no list"), FUCScenario::Parse(TEXT("t"), Definition(TEXT(", \"player\": {\"weapons\": \"x\"}")), FString(), S, Error));
    TestFalse(TEXT("two weapons in one entry"),
              FUCScenario::Parse(TEXT("t"), Definition(TEXT(", \"player\": {\"weapons\": [\"behr_smg_ballistic_01,behr_sniper_ballistic_01\"]}")), FString(), S, Error));
    TestFalse(TEXT("a weapon that is no name"), FUCScenario::Parse(TEXT("t"), Definition(TEXT(", \"player\": {\"weapons\": [1]}")), FString(), S, Error));
    TestFalse(TEXT("a player block that is no object"), FUCScenario::Parse(TEXT("t"), Definition(TEXT(", \"player\": [{\"gender\": \"female\"}]")), FString(), S, Error));
    TestFalse(TEXT("a loadout that is no string"), FUCScenario::Parse(TEXT("t"), Definition(TEXT(", \"player\": {\"loadout\": 3}")), FString(), S, Error));
    TestFalse(TEXT("a gender that is no string"), FUCScenario::Parse(TEXT("t"), Definition(TEXT(", \"player\": {\"gender\": true}")), FString(), S, Error));
    TestFalse(TEXT("spares that are no boolean"), FUCScenario::Parse(TEXT("t"), Definition(TEXT(", \"player\": {\"spares\": \"no\"}")), FString(), S, Error));

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
    R.EnvironmentLevel = USCViewerSubsystem::EnvironmentLevelPath(Hangar);
    R.VehicleLevel = TEXT("/Game/SC/Ships/Maps/CRUS_Starlifter_C2");
    R.PlayerLevel = Source.PlayerLevel();
    R.TraversalClips = USCViewerSubsystem::TraversalClipsPath(R.PlayerLevel, R.VehicleLevel, R.EnvironmentLevel);
    R.CargoLevels.Add(TEXT("Carryable_TBO_FL_16SCU"), FUCPrepared::CargoLevelPath(TEXT("Carryable_TBO_FL_16SCU")));
    R.TractorRecordJson = TEXT("{\"record\": \"tractor\"}");
    R.Archive = TEXT("build");
    R.PreparedAt = TEXT("2026-11-23T00:00:00.000Z");

    FUCPrepared Back;
    TestTrue(TEXT("the record round-trips"), FUCPrepared::Parse(R.ToJson(), Back, Error));
    TestTrue(TEXT("... its definition byte for byte"), Back.Definition.Equals(R.Definition, ESearchCase::CaseSensitive));
    TestTrue(TEXT("... its cargo setup"), Back.CargoDefinition.Equals(R.CargoDefinition, ESearchCase::CaseSensitive));
    TestEqual(TEXT("... its levels"), Back.VehicleLevel + Back.EnvironmentLevel + Back.PlayerLevel + Back.TraversalClips,
              R.VehicleLevel + R.EnvironmentLevel + R.PlayerLevel + R.TraversalClips);
    TestEqual(TEXT("... its format"), Back.Format, R.Format);
    TestTrue(TEXT("... its cargo box levels"), Back.CargoLevels.Num() == 1 && Back.CargoLevels.FindRef(TEXT("Carryable_TBO_FL_16SCU")) == R.CargoLevels[TEXT("Carryable_TBO_FL_16SCU")]);
    TestTrue(TEXT("... its tractor record as kept"), Back.TractorRecordJson.Equals(R.TractorRecordJson, ESearchCase::CaseSensitive));
    TestTrue(TEXT("a box entity's level"), FUCPrepared::CargoLevelPath(TEXT("Carryable_TBO_FL_16SCU")).StartsWith(TEXT("/Game/SC/Cargo/Maps/Carryable_TBO_FL_16SCU_")));
    TestNotEqual(TEXT("... another entity whose name only differs in a character a level name drops"),
                 FUCPrepared::CargoLevelPath(TEXT("Carryable-TBO_FL_16SCU")), FUCPrepared::CargoLevelPath(TEXT("Carryable_TBO_FL_16SCU")));
    TestNotEqual(TEXT("... or only in a trailing space (the export is asked for the name as written)"),
                 FUCPrepared::CargoLevelPath(TEXT("Carryable_TBO_FL_16SCU ")), FUCPrepared::CargoLevelPath(TEXT("Carryable_TBO_FL_16SCU")));
    // Every box entity of the prepared setup has its level in the record.
    {
        FUCPrepared Stocked = R;
        Stocked.CargoDefinition = R.CargoDefinition.LeftChop(1) +
            TEXT(", \"stock\": [{\"grid\": {\"owner\": \"environment\", \"index\": 0}, \"boxes\": [{\"entity\": \"Box_A\", \"commodity\": \"organic\"}]}]}");
        auto Every = [](const FString&) { return true; };
        TestTrue(TEXT("a box entity the record has no level for"), Stocked.Problem(nullptr, Every).Contains(TEXT("Box_A")));
        Stocked.CargoLevels.Add(TEXT("Box_A"), FUCPrepared::CargoLevelPath(TEXT("Box_A")));
        TestEqual(TEXT("... with its level: runs"), Stocked.Problem(nullptr, Every), FString());
    }
    TestTrue(TEXT("a missing cargo box level"),
             R.Problem(&Source, [&](const FString& L) { return L != R.CargoLevels[TEXT("Carryable_TBO_FL_16SCU")]; }).Contains(TEXT("cargo box")));
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
    TestTrue(TEXT("a missing player body level"), R.Problem(&Source, [&](const FString& L) { return L != R.PlayerLevel; }).Contains(R.PlayerLevel));
    TestTrue(TEXT("missing traversal clips"), R.Problem(&Source, [&](const FString& L) { return L != R.TraversalClips; }).Contains(R.TraversalClips));
    FUCPrepared NoTraversal = R;
    NoTraversal.TraversalClips.Reset();
    TestTrue(TEXT("a record that names no traversal clips"), NoTraversal.Problem(&Source, All).Contains(TEXT("traversal clips")));
    // The clips follow the levels they were read from: a level saved again since (another id in its stamp) makes them stale.
    {
        // A folder of this run's own, removed afterwards with only what it wrote.
        const FString Folder = TEXT("SCTest_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString Body = TEXT("/Game/") + Folder + TEXT("/Body_ids"), Clips = TEXT("/Game/") + Folder + TEXT("/Traversal_ids");
        const FString BodyStamp = FUCPrepared::StampFile(Body), ClipsStamp = FUCPrepared::StampFile(Clips);
        ON_SCOPE_EXIT
        {
            IFileManager::Get().Delete(*BodyStamp, false, true, true);
            IFileManager::Get().Delete(*ClipsStamp, false, true, true);
            IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPaths::ProjectContentDir(), Folder), false, false); // empty now
        };
        FUCPrepared Ids;
        Ids.PlayerLevel = Body;
        Ids.VehicleLevel = Body; // one stamp stands for both named levels here
        TestEqual(TEXT("no ids, no sources"), Ids.TraversalSources(), FString());
        FFileHelper::SaveStringToFile(FString::Printf(TEXT("%d\nb\n"), FUCPrepared::FormatVersion), *BodyStamp);
        TestEqual(TEXT("a level stamped without an id: no sources"), Ids.TraversalSources(), FString());
        FFileHelper::SaveStringToFile(FString::Printf(TEXT("%d\nb\nid-1\n"), FUCPrepared::FormatVersion), *BodyStamp);
        TestEqual(TEXT("ids for the named levels, the absent environment empty"), Ids.TraversalSources(), FString(TEXT("id-1|id-1|")));
        FFileHelper::SaveStringToFile(FString::Printf(TEXT("%d\nb\nid-c\n%s\n"), FUCPrepared::FormatVersion, *Ids.TraversalSources()), *ClipsStamp);
        TestEqual(TEXT("a level's id"), FUCPrepared::LevelId(Body), FString(TEXT("id-1")));
        TestEqual(TEXT("the clips read from these levels"), FUCPrepared::RecordedSources(Clips), Ids.TraversalSources());
        FFileHelper::SaveStringToFile(FString::Printf(TEXT("%d\nb\nid-2\n"), FUCPrepared::FormatVersion), *BodyStamp);
        TestNotEqual(TEXT("... no longer once the body saved again"), FUCPrepared::RecordedSources(Clips), Ids.TraversalSources());
    }
    TestTrue(TEXT("the traversal clips are one asset per level set"),
             R.TraversalClips.StartsWith(TEXT("/Game/SC/Players/Traversal/Traversal_")) &&
                 USCViewerSubsystem::TraversalClipsPath(R.PlayerLevel, R.VehicleLevel, FString()) != R.TraversalClips &&
                 USCViewerSubsystem::TraversalClipsPath(R.PlayerLevel, R.VehicleLevel, R.EnvironmentLevel) == R.TraversalClips);
    FUCPrepared NoPlayer = R;
    NoPlayer.PlayerLevel.Reset();
    TestTrue(TEXT("a record that names no player body level"), NoPlayer.Problem(&Source, All).Contains(TEXT("player body")));
    FUCScenario Changed = Source;
    Changed.Json += TEXT(" ");
    TestTrue(TEXT("a changed definition is stale"), R.Problem(&Changed, All).Contains(TEXT("changed")));
    Changed = Source;
    Changed.Json = Changed.Json.Replace(TEXT("\"T\""), TEXT("\"t\""), ESearchCase::CaseSensitive);
    TestTrue(TEXT("a definition changed only in case is stale"), R.Problem(&Changed, All).Contains(TEXT("changed")));
    // A checkout that turns LF line ends into CRLF (git's autocrlf) changes no definition.
    FUCPrepared Lf = R;
    Lf.Definition = R.Definition + TEXT("\n\"\":\n1");
    Lf.CargoDefinition = R.CargoDefinition + TEXT("\n");
    Changed = Source;
    Changed.Json = Lf.Definition.Replace(TEXT("\n"), TEXT("\r\n"));
    Changed.CargoJson = Lf.CargoDefinition.Replace(TEXT("\n"), TEXT("\r\n"));
    TestEqual(TEXT("CRLF for LF is no change"), Lf.Problem(&Changed, All), FString());
    Changed = Source;
    Changed.CargoJson.Reset();
    TestTrue(TEXT("a changed cargo setup is stale"), R.Problem(&Changed, All).Contains(TEXT("cargo setup")));
    Changed = Source;
    Changed.CargoJson = Changed.CargoJson.Replace(TEXT("\"c\""), TEXT("\"C\""), ESearchCase::CaseSensitive);
    TestTrue(TEXT("a cargo setup changed only in case is stale"), R.Problem(&Changed, All).Contains(TEXT("cargo setup")));
    FUCPrepared NoEnvironment = R;
    NoEnvironment.EnvironmentLevel.Reset();
    TestEqual(TEXT("no environment: only the vehicle level is needed"),
              NoEnvironment.Problem(nullptr, [&](const FString& L) {
                  return L == R.VehicleLevel || L == R.PlayerLevel || L == R.TraversalClips || L == R.CargoLevels[TEXT("Carryable_TBO_FL_16SCU")];
              }), FString());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUCPreparedLevelsTest, "SCBridge.Scenario.LevelNamesAndPaints",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FUCPreparedLevelsTest::RunTest(const FString&)
{
    // Where the prep step writes and the game looks (USCViewerSubsystem::EnvironmentLevelPath / ShipLevelPath).
    const FString Level = USCViewerSubsystem::EnvironmentLevelPath(Hangar);
    TestTrue(TEXT("an object container: ") + Level, Level.StartsWith(TEXT("/Game/SC/Hangars/Maps/hightech_a_hangar_xltop_001_")) && Level.Len() == 
             FString(TEXT("/Game/SC/Hangars/Maps/hightech_a_hangar_xltop_001_")).Len() + 8);
    TestEqual(TEXT("... with backslashes"), USCViewerSubsystem::EnvironmentLevelPath(FString(Hangar).Replace(TEXT("/"), TEXT("\\"))), Level);
    TestEqual(TEXT("... in other case (archive paths ignore it)"), USCViewerSubsystem::EnvironmentLevelPath(FString(Hangar).ToUpper()), Level);
    // Containers whose last two parts make the same name are other levels.
    TestNotEqual(TEXT("a_b/c and a/b_c"), USCViewerSubsystem::EnvironmentLevelPath(TEXT("Data/x/a_b/c.socpak")),
                 USCViewerSubsystem::EnvironmentLevelPath(TEXT("Data/x/a/b_c.socpak")));
    TestNotEqual(TEXT("the same last two parts in another tree"), USCViewerSubsystem::EnvironmentLevelPath(TEXT("Data/x/a/b.socpak")),
                 USCViewerSubsystem::EnvironmentLevelPath(TEXT("Data/y/a/b.socpak")));
    TestEqual(TEXT("no object container"), USCViewerSubsystem::EnvironmentLevelPath(TEXT("out/hangars/x")), FString());
    // A player body's level: one per body, whatever writes it the same way.
    const FString Body = USCViewerSubsystem::PlayerLevelPath(TEXT("female"), TEXT("Data/Scripts/L.xml"), {TEXT("a"), TEXT("b")}, true);
    TestTrue(TEXT("a player body: ") + Body, Body.StartsWith(TEXT("/Game/SC/Players/Maps/Player_female_")));
    TestEqual(TEXT("... the same body in other case, slashes and weapon order"),
              USCViewerSubsystem::PlayerLevelPath(TEXT("Female"), TEXT("data\\scripts\\l.xml"), {TEXT("B"), TEXT(" a")}, true), Body);
    TestEqual(TEXT("... the loadout without its Data/ prefix (the exporter reads it there)"),
              USCViewerSubsystem::PlayerLevelPath(TEXT("female"), TEXT("Scripts/L.xml"), {TEXT("a"), TEXT("b")}, true), Body);
    TestNotEqual(TEXT("... another loadout"), USCViewerSubsystem::PlayerLevelPath(TEXT("female"), TEXT("Data/Scripts/M.xml"), {TEXT("a"), TEXT("b")}, true), Body);
    TestNotEqual(TEXT("... other weapons"), USCViewerSubsystem::PlayerLevelPath(TEXT("female"), TEXT("Data/Scripts/L.xml"), {TEXT("a")}, true), Body);
    TestNotEqual(TEXT("... without spares"), USCViewerSubsystem::PlayerLevelPath(TEXT("female"), TEXT("Data/Scripts/L.xml"), {TEXT("a"), TEXT("b")}, false), Body);
    TestEqual(TEXT("... no gender: the one the export asks for"), USCViewerSubsystem::PlayerLevelPath(FString(), FString(), {}, true),
              USCViewerSubsystem::PlayerLevelPath(FSCBridgeClient::DefaultPlayerGender, FString(), {}, true));
    TestEqual(TEXT("... a weapon named twice"), USCViewerSubsystem::PlayerLevelPath(TEXT("female"), TEXT("Data/Scripts/L.xml"), {TEXT("a"), TEXT("b"), TEXT("A")}, true), Body);
    TestEqual(TEXT("a ship"), USCViewerSubsystem::ShipLevelPath(TEXT("CRUS_Starlifter_C2")), FString(TEXT("/Game/SC/Ships/Maps/CRUS_Starlifter_C2")));
    TestEqual(TEXT("a ship with a paint"), USCViewerSubsystem::ShipLevelPath(TEXT("CRUS_Starlifter_C2"), TEXT("crus.starlifter-pirate")),
              FString(TEXT("/Game/SC/Ships/Maps/CRUS_Starlifter_C2__crus_starlifter_pirate")));

    // "__" marks a paint variant only: an entity's own part never holds it (the Garage lists entities from level names).
    TestFalse(TEXT("an entity's own level"), USCViewerSubsystem::IsShipPaintLevel(USCViewerSubsystem::ShipLevelPath(TEXT("CRUS_Starlifter_C2"))));
    TestTrue(TEXT("a paint level"), USCViewerSubsystem::IsShipPaintLevel(USCViewerSubsystem::ShipLevelPath(TEXT("CRUS_Starlifter_C2"), TEXT("pirate_1234abcd"))));
    TestFalse(TEXT("an entity name with a double underscore is no paint level"), USCViewerSubsystem::IsShipPaintLevel(USCViewerSubsystem::ShipLevelPath(TEXT("A__B"))));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUCScenarioSwitchesTest, "SCBridge.Scenario.ConflictingSwitches",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FUCScenarioSwitchesTest::RunTest(const FString&)
{
    using S = UUCScenarioSubsystem;
    TestEqual(TEXT("a scenario alone"), S::ConflictingSwitch(TEXT("-Scenario=golem_showcase -SCShowcase -SCMaxQuality")), FString());
    TestEqual(TEXT("-SCLoad"), S::ConflictingSwitch(TEXT("-Scenario=x -SCLoad=DRAK_Golem")), FString(TEXT("-SCLoad")));
    TestEqual(TEXT("-SCEnvironment"), S::ConflictingSwitch(TEXT("-Scenario=x -SCEnvironment=a.socpak")), FString(TEXT("-SCEnvironment")));
    TestEqual(TEXT("-SCPackage"), S::ConflictingSwitch(TEXT("-Scenario=x -SCPackage=out/p")), FString(TEXT("-SCPackage")));
    TestEqual(TEXT("-SCCargoScenario"), S::ConflictingSwitch(TEXT("-Scenario=x -SCCargoScenario=c2_hangar")), FString(TEXT("-SCCargoScenario")));
    TestEqual(TEXT("-SCPlayerDemo=<entity> loads a vehicle"), S::ConflictingSwitch(TEXT("-Scenario=x -SCPlayerDemo=DRAK_Golem")), FString(TEXT("-SCPlayerDemo")));
    TestEqual(TEXT("-SCPlayerDemo alone loads none"), S::ConflictingSwitch(TEXT("-Scenario=x -SCPlayerDemo")), FString());
    TestEqual(TEXT("-SCNoCooked"), S::ConflictingSwitch(TEXT("-Scenario=x -SCNoCooked")), FString(TEXT("-SCNoCooked")));
    TestEqual(TEXT("-SCPlayerLoadout: the body is the scenario's"), S::ConflictingSwitch(TEXT("-Scenario=x -SCPlayerLoadout=Data/L.xml")), FString(TEXT("-SCPlayerLoadout")));
    TestEqual(TEXT("-SCPlayerWeapons"), S::ConflictingSwitch(TEXT("-Scenario=x -SCPlayerWeapons=a,b")), FString(TEXT("-SCPlayerWeapons")));
    TestEqual(TEXT("-SCPlayerPackage"), S::ConflictingSwitch(TEXT("-Scenario=x -SCPlayerPackage=kit.tar")), FString(TEXT("-SCPlayerPackage")));
    // Automation that puts the camera: the scenario leaves the camera and the start to it.
    TestFalse(TEXT("a scenario alone puts its camera"), S::AutomationOwnsCamera(TEXT("-Scenario=x -SCShowcase")));
    TestTrue(TEXT("-SCCamera"), S::AutomationOwnsCamera(TEXT("-Scenario=x -SCCamera=orbit")));
    TestTrue(TEXT("-SCScript"), S::AutomationOwnsCamera(TEXT("-Scenario=x -SCScript=route.txt")));
    TestTrue(TEXT("-SCPlayerDemo"), S::AutomationOwnsCamera(TEXT("-Scenario=x -SCPlayerDemo")));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
