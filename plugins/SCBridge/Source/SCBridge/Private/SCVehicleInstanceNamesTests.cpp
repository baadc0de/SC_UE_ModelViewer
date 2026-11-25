// SCBridge.Vehicles.EntityNames: a vehicle known by its qualified record name (`EntityClassDefinition.X`, as a package
// manifest or a request spells it) is the bare entity X everywhere the viewer keys, exports or compares it.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCViewerSubsystem.h"
#include "SCVehicle.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCVehicleEntityNamesTest, "SCBridge.Vehicles.EntityNames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCVehicleEntityNamesTest::RunTest(const FString&)
{
    TestEqual(TEXT("the record prefix and spaces go"), USCViewerSubsystem::EntityName(TEXT(" EntityClassDefinition.TEST_Ship ")), FString(TEXT("TEST_Ship")));
    TestEqual(TEXT("a bare name stays"), USCViewerSubsystem::EntityName(TEXT("TEST_Ship")), FString(TEXT("TEST_Ship")));

    USCViewerSubsystem* Viewer = USCViewerSubsystem::CreateHeadless();
    if (!TestNotNull(TEXT("headless viewer"), Viewer)) return false;
    ON_SCOPE_EXIT { USCViewerSubsystem::DestroyHeadless(Viewer); };
    UWorld* World = Viewer->GetGameInstance() ? Viewer->GetGameInstance()->GetWorld() : nullptr;
    if (!TestNotNull(TEXT("viewer world"), World)) return false;

    // A package adopted without an archive (-SCPackage, the folder harness): its manifest names the record.
    ASCVehicle* Adopted = World->SpawnActor<ASCVehicle>();
    if (!TestNotNull(TEXT("vehicle"), Adopted)) return false;
    Adopted->Manifest.Root.EntityName = TEXT("EntityClassDefinition.TEST_Adopted");
    Viewer->AdoptPackageVehicle(Adopted);
    TestEqual(TEXT("the adopted package's loaded entity is the bare name the exporter resolves"), Viewer->GetLoadedEntity(), FString(TEXT("TEST_Adopted")));

    // Another ship asked for by its record name is queued as the bare entity (a copy of the loaded ship starts from the
    // loaded entity, checked above). Queued only: whether it can start depends on the archive, which the editor opens.
    const int32 Named = Viewer->SpawnVehicleInstance(TEXT(" EntityClassDefinition.TEST_Other "));
    bool bFound = false;
    for (const FSCVehicleInstanceInfo& Info : Viewer->GetVehicleInstances())
        if (Info.Id == Named)
        {
            bFound = true;
            TestEqual(TEXT("a ship asked for by its record name is queued under its bare name"), Info.Entity, FString(TEXT("TEST_Other")));
        }
    TestTrue(TEXT("the request is listed"), bFound);
    return !HasAnyErrors();
}

// SCBridge.Garage.StoredShipLevel: a ship stored from a prepared level comes back from that level (a scenario's paint; a
// package may hold no other level of the entity), and stays listed as stored.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCGarageStoredShipLevelTest, "SCBridge.Garage.StoredShipLevel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCGarageStoredShipLevelTest::RunTest(const FString&)
{
    const FString Level = TEXT("/Engine/Maps/Entry"); // stands in for a painted ship level: any existing map package
    if (!FPackageName::DoesPackageExist(Level))
    {
        AddInfo(TEXT("no stand-in level in this build"));
        return true;
    }
    USCViewerSubsystem* Viewer = USCViewerSubsystem::CreateHeadless();
    if (!TestNotNull(TEXT("headless viewer"), Viewer)) return false;
    ON_SCOPE_EXIT { USCViewerSubsystem::DestroyHeadless(Viewer); };
    TestTrue(TEXT("nothing stored: no level"), Viewer->GarageShipLevel(TEXT("TEST_Painted")).IsEmpty());
    Viewer->SetStoredShipLevel(TEXT("EntityClassDefinition.TEST_Painted"), Level);
    TestEqual(TEXT("the Garage retrieves it from the level it was stored from"), Viewer->GarageShipLevel(TEXT("TEST_Painted")), Level);
    TestEqual(TEXT("a copy without the entity's own level streams it too"), Viewer->PreparedShipLevelFor(TEXT("test_painted")), Level);
    TestTrue(TEXT("listed as stored, by its bare name"), Viewer->GetStoredShipEntities().Contains(TEXT("TEST_Painted")));
    Viewer->SetStoredShipLevel(TEXT("TEST_Gone"), TEXT("/Game/SCBridgeTest/DoesNotExist"));
    TestTrue(TEXT("a level that does not exist is no level"), Viewer->GarageShipLevel(TEXT("TEST_Gone")).IsEmpty());
    TestFalse(TEXT("and not listed"), Viewer->GetStoredShipEntities().Contains(TEXT("TEST_Gone")));
    return !HasAnyErrors();
}
#endif
