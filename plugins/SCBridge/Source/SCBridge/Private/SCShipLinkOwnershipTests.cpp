// SCBridge.ShipLink.Unowned: an instance marked as nobody's ship (a cargo obstacle) has no owner and is not paired with
// the player, who otherwise owns every vehicle without an explicit owner.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCShipLinkSubsystem.h"
#include "SCViewerSubsystem.h"
#include "SCVehicle.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCShipLinkUnownedTest, "SCBridge.ShipLink.Unowned",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCShipLinkUnownedTest::RunTest(const FString&)
{
    USCViewerSubsystem* Viewer = USCViewerSubsystem::CreateHeadless();
    if (!TestNotNull(TEXT("headless viewer"), Viewer)) return false;
    ON_SCOPE_EXIT { USCViewerSubsystem::DestroyHeadless(Viewer); };
    UWorld* World = Viewer->GetGameInstance() ? Viewer->GetGameInstance()->GetWorld() : nullptr;
    USCShipLinkSubsystem* Link = World ? World->GetSubsystem<USCShipLinkSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("ship link"), Link)) return false;
    Link->OnWorldBeginPlay(*World); // binds to the viewer (a headless world does not begin play)
    ASCVehicle* Ship = World->SpawnActor<ASCVehicle>();
    if (!TestNotNull(TEXT("vehicle"), Ship)) return false;
    Ship->Manifest.Root.EntityName = TEXT("TEST_Rock");
    Viewer->AdoptPackageVehicle(Ship); // instance 0
    const FName Player = Link->GetPlayer();
    TestEqual(TEXT("a vehicle without an explicit owner is the player's"), Link->GetOwner(Ship), Player);
    Link->SetUnowned(0);
    TestTrue(TEXT("an unowned instance has no owner"), Link->GetOwner(Ship).IsNone());
    TestFalse(TEXT("and is not among the player's paired ships"), Link->PairedShips(Player).Contains(Ship));
    Link->SetOwner(0, Player);
    TestEqual(TEXT("an explicit owner replaces it"), Link->GetOwner(Ship), Player);
    return !HasAnyErrors();
}
#endif
