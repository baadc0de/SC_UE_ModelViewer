// SCBridge.Archive.PanicNoticeRetiredByOpen (WP-O1-3 review): a panic notice of the session an open replaces is
// retired by that open, also when the open fails, so the failure reported is the new archive's own. No archive needed.
#include "SCBridgeClient.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCArchivePanicNoticeTest, "SCBridge.Archive.PanicNoticeRetiredByOpen",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSCArchivePanicNoticeTest::RunTest(const FString&)
{
    const TSharedRef<FSCBridgeClient, ESPMode::ThreadSafe> Client = MakeShared<FSCBridgeClient, ESPMode::ThreadSafe>();
    Client->NotePanicForTest(INVTEXT("the replaced session crashed"));
    const FString Missing = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("no-such-archive/Data.p4k"));
    UE::Tasks::TTask<FSCOpenResult> Open = Client->Open(Missing, FString());
    Open.Wait();
    TestFalse(TEXT("the open of a missing archive fails"), Open.GetResult().bOK);
    FText Error;
    TestFalse(TEXT("the replaced session's notice is retired by the (failed) open"), Client->TakePanic(Error));
    Client->NotePanicForTest(INVTEXT("this session crashed"));
    TestTrue(TEXT("a notice after the open is reported"), Client->TakePanic(Error));
    TestEqual(TEXT("as itself"), Error.ToString(), FString(TEXT("this session crashed")));
    return true;
}

#endif
