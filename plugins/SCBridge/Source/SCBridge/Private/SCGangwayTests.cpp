// WP-G1: verify first ground contact along the authored opening path.
#include "SCGangwayGround.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCGangwayContact, "SCBridge.Exploration.GangwayGround",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCGangwayContact::RunTest(const FString&)
{
    TestTrue(TEXT("authored ramp attachment role on an ordinary Door item"),
        SCGangwayGround::IsRampRole(TEXT("Door"), TEXT(""), TEXT("hardpoint_rear_ramp")));
    TestTrue(TEXT("typed boarding gangway"), SCGangwayGround::IsRampRole(TEXT("Door"), TEXT("Gangway"), TEXT("bay")));
    TestFalse(TEXT("a substring in an unrelated port is not a ramp"),
        SCGangwayGround::IsRampRole(TEXT("Door"), TEXT(""), TEXT("hardpoint_crampon")));
    auto Pose = [](double Frame, TArray<FVector>& P) {
        P = {FVector(Frame, -30, 100 - Frame * 10), FVector(Frame, 30, 100 - Frame * 10)};
    };
    auto Sloped = [](FVector A, FVector B) -> TOptional<double> {
        const double Ground = A.Y > 0 ? 35. : 0.;
        if (A.Z >= Ground && B.Z <= Ground && B.Z < A.Z) return (A.Z - Ground) / (A.Z - B.Z);
        return {};
    };
    TOptional<double> Hit = SCGangwayGround::LimitFrame(0, 12, Pose, Sloped);
    TestTrue(TEXT("high side contacts first, even in a tick traversing multiple keys"),
        Hit.IsSet() && FMath::IsNearlyEqual(Hit.GetValue(), 6.5, 0.015));
    TestFalse(TEXT("unobstructed stroke advances to next playing frame"),
        SCGangwayGround::LimitFrame(0, 6, Pose, Sloped).IsSet());
    TestFalse(TEXT("rising probes never stop against ground"),
        SCGangwayGround::LimitFrame(7, 0, Pose, Sloped).IsSet());
    auto Curved = [](double F, TArray<FVector>& P) {
        P = {FVector(0, -30, 40. - 42. * FMath::Sin(UE_DOUBLE_PI * F))};
    };
    Hit = SCGangwayGround::LimitFrame(0, 1, Curved, Sloped);
    TestTrue(TEXT("a rotating tip can contact between two clear key poses"),
        Hit.IsSet() && Hit.GetValue() > .39 && Hit.GetValue() < .41);
    return true;
}
