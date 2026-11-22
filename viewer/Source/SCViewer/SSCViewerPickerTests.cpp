// SCBridge.Viewer.PickerQueue: the viewer menu's click decisions (SSCViewerPicker::DecideVehiclePick /
// DecideEnvironmentPick) through the click sequences of the WP-O1-3 reviews. Pure: no world, no archive.
#include "SSCViewerPicker.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerPickerQueueTest, "SCBridge.Viewer.PickerQueue",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSCViewerPickerQueueTest::RunTest(const FString&)
{
    using P = SSCViewerPicker;
    // Vehicles. A in the scene, B loading (LoadEntity refuses while it runs).
    P::FVehiclePickState Loading;
    Loading.bLoading = true;
    Loading.Pending = TEXT("B");
    TestTrue(TEXT("C while B loads waits"), P::DecideVehiclePick(TEXT("C"), Loading) == P::EVehiclePick::Queue);
    TestTrue(TEXT("B again keeps B (and drops C)"), P::DecideVehiclePick(TEXT("b"), Loading) == P::EVehiclePick::Keep);
    TestTrue(TEXT("A (replaced by B) comes back after"), P::DecideVehiclePick(TEXT("A"), Loading) == P::EVehiclePick::Queue);
    P::FVehiclePickState Cancelling = Loading;
    Cancelling.bCancelling = true;
    TestTrue(TEXT("B while its load is cancelled is asked again"), P::DecideVehiclePick(TEXT("B"), Cancelling) == P::EVehiclePick::Queue);
    P::FVehiclePickState Idle;
    Idle.bCanLoad = true;
    TestTrue(TEXT("idle: load (LoadEntity reuses an identical one)"), P::DecideVehiclePick(TEXT("A"), Idle) == P::EVehiclePick::Load);
    P::FVehiclePickState Opening; // the archive opens: nothing loads yet
    TestTrue(TEXT("archive opening: waits"), P::DecideVehiclePick(TEXT("A"), Opening) == P::EVehiclePick::Queue);

    // Environments.
    const FString H = TEXT("Data/ObjectContainers/x/h.socpak"), K = TEXT("Data/ObjectContainers/x/k.socpak");
    P::FEnvironmentPickState Installed;
    Installed.Request = H;
    Installed.bInstalled = true;
    TestTrue(TEXT("the one in the scene"), P::DecideEnvironmentPick(TEXT("data\\objectcontainers\\x\\h.socpak"), Installed) == P::EEnvironmentPick::There);
    TestTrue(TEXT("another, idle: now"), P::DecideEnvironmentPick(K, Installed) == P::EEnvironmentPick::Request);
    TestTrue(TEXT("the procedural hangar, idle: now"), P::DecideEnvironmentPick(TEXT("none"), Installed) == P::EEnvironmentPick::Request);
    P::FEnvironmentPickState OtherArchive = Installed;
    OtherArchive.bInstalled = false; // the actor was built before the archive was reopened
    TestTrue(TEXT("an earlier archive's: loaded again"), P::DecideEnvironmentPick(H, OtherArchive) == P::EEnvironmentPick::Request);
    P::FEnvironmentPickState Failed;
    Failed.Request = H; // requested, not installed, nothing runs
    TestTrue(TEXT("a failed one: requested again"), P::DecideEnvironmentPick(H, Failed) == P::EEnvironmentPick::Request);
    P::FEnvironmentPickState Exporting;
    Exporting.Request = H;
    Exporting.bLoading = Exporting.bWorking = true;
    TestTrue(TEXT("the one on its way"), P::DecideEnvironmentPick(H, Exporting) == P::EEnvironmentPick::There);
    TestTrue(TEXT("another while one works: waits"), P::DecideEnvironmentPick(K, Exporting) == P::EEnvironmentPick::Queue);
    TestTrue(TEXT("the procedural hangar while one works: waits"), P::DecideEnvironmentPick(TEXT("none"), Exporting) == P::EEnvironmentPick::Queue);
    P::FEnvironmentPickState Stopping = Exporting;
    Stopping.bCancelling = true;
    TestTrue(TEXT("the one being stopped: asked again after"), P::DecideEnvironmentPick(H, Stopping) == P::EEnvironmentPick::Queue);
    P::FEnvironmentPickState Procedural;
    TestTrue(TEXT("the procedural hangar in the scene"), P::DecideEnvironmentPick(TEXT("none"), Procedural) == P::EEnvironmentPick::There);

    // A row of archive A's lists clicked after archive B was opened (its list not yet refreshed): nothing happens.
    P::FVehiclePickState Reopened = Idle;
    Reopened.RowSession = 1;
    Reopened.Session = 2;
    TestTrue(TEXT("a vehicle row of an earlier archive is dropped"), P::DecideVehiclePick(TEXT("A"), Reopened) == P::EVehiclePick::Drop);
    P::FVehiclePickState ReopenedLoading = Loading;
    ReopenedLoading.RowSession = 1;
    ReopenedLoading.Session = 2;
    TestTrue(TEXT("... also while a load runs"), P::DecideVehiclePick(TEXT("B"), ReopenedLoading) == P::EVehiclePick::Drop);
    P::FEnvironmentPickState EnvReopened = Failed;
    EnvReopened.RowSession = 1;
    EnvReopened.Session = 2;
    TestTrue(TEXT("an environment row of an earlier archive is dropped"), P::DecideEnvironmentPick(K, EnvReopened) == P::EEnvironmentPick::Drop);
    return true;
}

#endif
