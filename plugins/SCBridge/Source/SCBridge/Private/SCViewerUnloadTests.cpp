// SCBridge.Viewer.UnloadWithEnvironmentBusy: unloading the vehicle drains only a vehicle load; environment work (a hangar
// streaming, exporting or waiting for the archive) carries on and does not hold the game thread.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCViewerSubsystem.h"
#include "SCVehicleInstances.h"
#include "HAL/PlatformTime.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerUnloadEnvironmentBusyTest, "SCBridge.Viewer.UnloadWithEnvironmentBusy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerUnloadEnvironmentBusyTest::RunTest(const FString&)
{
    USCViewerSubsystem* Viewer = USCViewerSubsystem::CreateHeadless();
    if (!TestNotNull(TEXT("headless viewer"), Viewer)) return false;
    ON_SCOPE_EXIT { USCViewerSubsystem::DestroyHeadless(Viewer); };
    // An environment no archive holds: pending while the archive opens, then its export runs (and fails) — environment
    // work either way, with no vehicle load.
    Viewer->SetEnvironment(TEXT("Data/ObjectContainers/SCBridgeTest/does_not_exist.socpak"));
    if (!Viewer->IsBusy())
    {
        AddInfo(TEXT("the environment is not busy in this configuration (no archive reader): nothing to drain"));
        return true;
    }
    TestFalse(TEXT("no vehicle load"), Viewer->GetLoadPhase() == ESCLoadPhase::Exporting || Viewer->GetLoadPhase() == ESCLoadPhase::Parsing ||
                                       Viewer->GetLoadPhase() == ESCLoadPhase::Importing);
    const double Start = FPlatformTime::Seconds();
    Viewer->Unload();
    const double Seconds = FPlatformTime::Seconds() - Start;
    TestTrue(FString::Printf(TEXT("unload returns at once while only the environment works (%.2f s)"), Seconds), Seconds < 5.0);
    return !HasAnyErrors();
}

// SCBridge.Vehicles.QueuedSurviveReset: a copy requested for the ship being loaded outlives the old ship's teardown (an
// archive load frees the previous ship after parsing the next); with no ship arriving it fails, never disappears.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCVehicleQueuedSurviveResetTest, "SCBridge.Vehicles.QueuedSurviveReset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCVehicleQueuedSurviveResetTest::RunTest(const FString&)
{
    USCViewerSubsystem* Viewer = USCViewerSubsystem::CreateHeadless();
    if (!TestNotNull(TEXT("headless viewer"), Viewer)) return false;
    ON_SCOPE_EXIT { USCViewerSubsystem::DestroyHeadless(Viewer); };
    FSCVehicleInstances* Set = Viewer->GetVehicleInstanceSet();
    if (!TestNotNull(TEXT("instance set"), Set)) return false;
    Viewer->SetEnvironment(TEXT("none")); // no hangar work: instances step only while the viewer is idle
    TestFalse(TEXT("idle"), Viewer->IsBusy());
    // No ship yet: "loaded" waits for the one being loaded.
    const int32 Id = Viewer->SpawnVehicleInstance(TEXT("loaded"));
    auto Find = [&]() -> TOptional<FSCVehicleInstanceInfo> {
        for (const FSCVehicleInstanceInfo& Info : Viewer->GetVehicleInstances())
            if (Info.Id == Id) return Info;
        return {};
    };
    if (!TestTrue(TEXT("queued"), Find().IsSet() && Find()->State == ESCVehicleInstanceState::Queued)) return false;
    // The previous ship goes for the load the request waits for (StepLoad -> UnloadInternal(bForPendingLoad)).
    Set->ResetForLoad(nullptr, true, Viewer->GetLoadSerial());
    TestTrue(TEXT("the request outlives the previous ship's teardown"), Find().IsSet() && Find()->State == ESCVehicleInstanceState::Queued);
    for (int32 Step = 0; Step < 4; ++Step) Set->Tick();
    TestTrue(TEXT("no ship arrives: the request fails instead of vanishing"), Find().IsSet() && Find()->State == ESCVehicleInstanceState::Failed);

    // Another load (or an unload) is not the one it waited for: it fails at the reset, it does not wait for that ship.
    const int32 Other = Viewer->SpawnVehicleInstance(TEXT("loaded"));
    Set->ResetForLoad(nullptr, true, Viewer->GetLoadSerial() + 1);
    bool bFailed = false;
    for (const FSCVehicleInstanceInfo& Info : Viewer->GetVehicleInstances())
        if (Info.Id == Other) bFailed = Info.State == ESCVehicleInstanceState::Failed;
    TestTrue(TEXT("a request for an earlier load fails at a later load's reset"), bFailed);
    const int32 Third = Viewer->SpawnVehicleInstance(TEXT("loaded"));
    Set->Reset(nullptr, true); // an unload
    bFailed = false;
    for (const FSCVehicleInstanceInfo& Info : Viewer->GetVehicleInstances())
        if (Info.Id == Third) bFailed = Info.State == ESCVehicleInstanceState::Failed;
    TestTrue(TEXT("an unload fails a waiting request"), bFailed);
    return !HasAnyErrors();
}
#endif
