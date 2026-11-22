#include "SCRenderPath.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCRenderPathTest, "SCBridge.RpcRender.Path",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSCRenderPathTest::RunTest(const FString&)
{
    using namespace SCRenderPath;
    TestEqual(TEXT("before start"), Progress(-1, 2, false), 0.0);
    TestEqual(TEXT("after end"), Progress(3, 2, false), 1.0);
    TestEqual(TEXT("eased quarter"), Progress(0.5, 2, true), 0.15625);
    const FVector Center(100, 200, 300);
    const FPose Quarter = Orbit(Center, 50, 12, 0, 360, 0.25);
    TestTrue(TEXT("quarter circle preserves height"), Quarter.Position.Equals(Center + FVector(0, 50, 12)));
    TestTrue(TEXT("orbit faces the target"), Quarter.Rotation.Vector().Equals((Center - Quarter.Position).GetSafeNormal()));
    const FPose Half = Move({FVector::ZeroVector, FRotator(0, 170, 0)}, {Center, FRotator(0, -170, 0)}, 0.5);
    TestTrue(TEXT("move follows the authored Euler arc"), Half.Rotation.Vector().Equals(FVector::ForwardVector));
    TestTrue(TEXT("halfway translation"), Half.Position.Equals(Center * 0.5));
    const FPose Tracked = MoveLookingAt(FVector::ZeroVector, Center, FVector(0, 10, 0), Center + FVector(0, 10, 0), 0.5);
    TestTrue(TEXT("moving target tracked"), Tracked.Rotation.Vector().Equals(FVector::RightVector));
    return true;
}
#endif
