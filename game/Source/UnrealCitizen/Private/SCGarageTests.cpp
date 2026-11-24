#if WITH_DEV_AUTOMATION_TESTS
#include "SCGarageSubsystem.h"
#include "SCSceneManifest.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "SCVehicleInstances.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

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
    Entry.DisplayName = TEXT("<= PLACEHOLDER =>");
    TestFalse(TEXT("localization placeholders are excluded despite usable geometry"), FSCGaragePolicy::IsFleetCandidate(Entry));
    Entry.DisplayName.Reset();
    Entry.Category = TEXT("Other");
    TestFalse(TEXT("props are not offered by the garage"), FSCGaragePolicy::IsFleetCandidate(Entry));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCGarageFloorSearchTest, "SCBridge.Garage.FloorSearch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCGarageFloorSearchTest::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    AActor* Floor = World->SpawnActor<AActor>();
    UBoxComponent* Slab = NewObject<UBoxComponent>(Floor);
    Floor->SetRootComponent(Slab);
    Slab->SetBoxExtent(FVector(4000, 4000, 50));
    Slab->SetRelativeLocation(FVector(0, 0, -50));
    Slab->SetCollisionProfileName(TEXT("BlockAll"));
    Slab->RegisterComponent();
    FSCVehiclePlacementQuery Query;
    Query.LocalEnvelope = FBox(FVector(-730, -800, -230), FVector(730, 800, 292));
    Query.SearchArea = FBox(FVector(-4000, -4000, 0), FVector(4000, 4000, 0));
    Query.KeepOut.Add(FBox(FVector(-2400, -1600, 0), FVector(1800, 1600, 1100)));
    const FSCVehiclePlacementResult Result = FSCVehiclePlacement::FindFreeFloor(World, Query);
    TestTrue(TEXT("a supported floor placement beside a larger vessel: ") + Result.Reason, Result.bFound);
    if (Result.bFound)
    {
        TestTrue(TEXT("does not occupy the larger ship's keep-out"),
            !Query.KeepOut[0].Intersect(FSCVehiclePlacement::WorldBox(Query.LocalEnvelope, Result.Transform)));
        TestTrue(TEXT("bounded search"), Result.Tested < 10000);
    }
    AActor* Obstruction = World->SpawnActor<AActor>();
    UBoxComponent* Overhead = NewObject<UBoxComponent>(Obstruction);
    Obstruction->SetRootComponent(Overhead);
    Overhead->SetBoxExtent(FVector(100, 100, 20));
    Overhead->SetRelativeLocation(FVector(0, 0, 180));
    Overhead->SetCollisionProfileName(TEXT("BlockAll"));
    Overhead->RegisterComponent();
    Query.SearchArea = FBox(FVector::ZeroVector, FVector::ZeroVector);
    Query.KeepOut.Reset();
    Query.Yaws = {0};
    const FSCVehiclePlacementResult Blocked = FSCVehiclePlacement::FindFreeFloor(World, Query);
    TestFalse(TEXT("the ship may not occupy the overhead obstruction"), Blocked.bFound);
    TestTrue(TEXT("an obstruction is distinguished from missing floor"), Blocked.Reason.Contains(TEXT("0 without flat floor")));
    TestTrue(TEXT("the clearance check reports the blocked spot"), Blocked.Reason.Contains(TEXT("1 blocked")));
    return !HasAnyErrors();
}
#endif
