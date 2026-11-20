#if WITH_DEV_AUTOMATION_TESTS
#include "SCGarageSubsystem.h"
#include "SCPackageReader.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCGarageStorePolicyTest, "SCBridge.Garage.StorePolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCGarageStorePolicyTest::RunTest(const FString&)
{
    TestTrue(TEXT("own ship clear to store"), FSCGaragePolicy::StoreRefusal(true, false, false).IsEmpty());
    TestFalse(TEXT("someone else's ship"), FSCGaragePolicy::StoreRefusal(false, false, false).IsEmpty());
    TestFalse(TEXT("player still aboard"), FSCGaragePolicy::StoreRefusal(true, true, false).IsEmpty());
    TestFalse(TEXT("a transfer uses the ship's grids"), FSCGaragePolicy::StoreRefusal(true, false, true).IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCGarageKioskRoleTest, "SCBridge.Garage.KioskRole",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCGarageKioskRoleTest::RunTest(const FString&)
{
    FSCUiBinding Binding;
    Binding.bClassified = Binding.bOwnerResolved = Binding.bOperated = true;
    Binding.Kind = TEXT("physical");
    Binding.SourceItemType = TEXT("Display");
    FSCUiStateType& Main = Binding.StateTypes.AddDefaulted_GetRef();
    Main.Name = TEXT("Main");
    Main.States = {TEXT("Attract"), TEXT("VehicleList"), TEXT("RetrievingVehicle")};
    Binding.Buttons.Add({TEXT("TouchHere"), TEXT("UseConsole"), FBox2D(ForceInit)});
    TestTrue(TEXT("a vehicle retrieval panel by authored states"), FSCGaragePolicy::IsVehicleKiosk(Binding));
    Main.States = {TEXT("Attract"), TEXT("FreightList")};
    TestFalse(TEXT("a freight terminal belongs to another interaction"), FSCGaragePolicy::IsVehicleKiosk(Binding));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCGarageBayPolicyTest, "SCBridge.Garage.BayPolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCGarageBayPolicyTest::RunTest(const FString&)
{
    bool bNeedsConfirmation = false;
    TestFalse(TEXT("another owner's vehicle cannot be automatically stored"),
        FSCGaragePolicy::BayRefusal(false, true, bNeedsConfirmation).IsEmpty());
    TestFalse(TEXT("an unowned vehicle never requests confirmation"), bNeedsConfirmation);
    TestFalse(TEXT("replacing your ship needs confirmation"), FSCGaragePolicy::BayRefusal(true, false, bNeedsConfirmation).IsEmpty());
    TestTrue(TEXT("own ship is replaceable when confirmed"), bNeedsConfirmation);
    TestTrue(TEXT("confirmed replacement may proceed"), FSCGaragePolicy::BayRefusal(true, true, bNeedsConfirmation).IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCGarageFleetCatalogTest, "SCBridge.Garage.FleetCatalog",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCGarageFleetCatalogTest::RunTest(const FString&)
{
    FSCEntityHit Entry;
    Entry.Category = TEXT("Ships");
    TestFalse(TEXT("a record with no geometry is not retrievable"), FSCGaragePolicy::IsFleetCandidate(Entry));
    Entry.GeometryPath = TEXT("synthetic.cgf");
    TestTrue(TEXT("the vehicle browser category determines eligibility"), FSCGaragePolicy::IsFleetCandidate(Entry));
    Entry.Category = TEXT("Other");
    TestFalse(TEXT("props are not offered by the garage"), FSCGaragePolicy::IsFleetCandidate(Entry));
    return true;
}
#endif
