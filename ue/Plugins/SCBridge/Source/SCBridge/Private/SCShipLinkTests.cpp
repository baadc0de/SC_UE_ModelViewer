#if WITH_DEV_AUTOMATION_TESTS
#include "SCShipLinkSubsystem.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCShipLinkAccessTest, "SCBridge.ShipLink.Access",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCShipLinkAccessTest::RunTest(const FString&)
{
    using ERole = ESCShipRole;
    using EAction = ESCShipDoorAction;
    using EGroup = ESCShipDoorGroup;
    for (ERole Role : {ERole::None, ERole::Guest, ERole::Crew, ERole::Owner})
    {
        TestTrue(TEXT("unlocked open"), FSCShipDoorPolicy::Refusal(Role, EAction::Open, false, EGroup::Exterior, false).IsEmpty());
        TestTrue(TEXT("anyone can close"), FSCShipDoorPolicy::Refusal(Role, EAction::Close, true, EGroup::Exterior, false).IsEmpty());
        TestFalse(TEXT("locked entry"), FSCShipDoorPolicy::Refusal(Role, EAction::Open, true, EGroup::Exterior, false).IsEmpty());
        TestEqual(TEXT("only the owner can exit through a locked exterior door"),
            FSCShipDoorPolicy::Refusal(Role, EAction::Open, true, EGroup::Exterior, true).IsEmpty(), Role == ERole::Owner);
        TestFalse(TEXT("locked interior door"), FSCShipDoorPolicy::Refusal(Role, EAction::Open, true, EGroup::Interior, true).IsEmpty());
        for (EAction Action : {EAction::Lock, EAction::Unlock})
            TestEqual(TEXT("only the owner controls locks"),
                FSCShipDoorPolicy::Refusal(Role, Action, true, EGroup::Exterior, false).IsEmpty(), Role == ERole::Owner);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCShipLinkRoomTest, "SCBridge.ShipLink.RoomClassification",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCShipLinkRoomTest::RunTest(const FString&)
{
    using EGroup = ESCShipDoorGroup;
    TestTrue(TEXT("one room and outside"), FSCShipDoorPolicy::Classify(true, TEXT("room"), FString()) == EGroup::Exterior);
    TestTrue(TEXT("two rooms"), FSCShipDoorPolicy::Classify(true, TEXT("room a"), TEXT("room b")) == EGroup::Interior);
    TestTrue(TEXT("within room"), FSCShipDoorPolicy::Classify(true, TEXT("room"), TEXT("room")) == EGroup::Interior);
    TestTrue(TEXT("both outside"), FSCShipDoorPolicy::Classify(true, FString(), FString()) == EGroup::Other);
    TestTrue(TEXT("no room data"), FSCShipDoorPolicy::Classify(false, TEXT("room"), FString()) == EGroup::Other);
    return true;
}
#endif
