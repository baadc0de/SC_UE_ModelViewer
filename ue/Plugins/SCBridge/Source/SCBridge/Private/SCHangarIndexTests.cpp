// USCHangarIndex fit rule (WP-C1): a ship fits a hangar when its solid footprint, centred on the ship elevator, stays on
// the platform and its height stays under the clear height (an open roof has none).
#include "SCHangarIndex.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCHangarCapacityHolds, "SCBridge.Hangars.CapacityHolds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCHangarCapacityHolds::RunTest(const FString&)
{
    FSCHangarCapacity Small;
    Small.BayCm = FVector2D(4800, 4800);
    Small.ClearanceCm = 3700;
    const FBox Cutlass(FVector(-2090, -1310, 0), FVector(1480, 1310, 1030));
    const FBox Galaxy(FVector(-6680, -3120, 0), FVector(6740, 3120, 2310));
    TestTrue(TEXT("a ship on the platform under the ceiling fits"), Small.Holds(Cutlass));
    TestFalse(TEXT("a ship longer than the platform does not"), Small.Holds(Galaxy));
    TestFalse(TEXT("a ship off the platform centre does not"), Small.Holds(Cutlass.ShiftBy(FVector(1000, 0, 0))));
    FSCHangarCapacity Low = Small;
    Low.ClearanceCm = 900;
    TestFalse(TEXT("a ship taller than the clear height does not"), Low.Holds(Cutlass));
    Low.ClearanceCm = -1;
    TestTrue(TEXT("an open roof has no height limit"), Low.Holds(Cutlass));
    TestFalse(TEXT("no measurement holds nothing"), FSCHangarCapacity().Holds(Cutlass));
    TestFalse(TEXT("no ship bounds fit nowhere"), Small.Holds(FBox(ForceInit)));
    return true;
}
