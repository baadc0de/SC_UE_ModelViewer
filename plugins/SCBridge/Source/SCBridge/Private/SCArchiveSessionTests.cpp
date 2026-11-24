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


/** WP-O1-4c-3b: the export options mirror (FSCExportOptions, SCArchive.h) defaults to the core's (sc_default_options), so
 *  the view options' defaults need no archive library. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCArchiveExportOptionsDefaultsTest, "SCBridge.Archive.ExportOptionsDefaults",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCArchiveExportOptionsDefaultsTest::RunTest(const FString&)
{
    const sc_options Core = FSCBridgeClient::DefaultOptions();
    const sc_options Mirror = FSCBridgeClient::ToFfi(FSCExportOptions());
    TestEqual(TEXT("lod"), Mirror.lod, Core.lod);
    TestEqual(TEXT("mip"), Mirror.mip, Core.mip);
    TestEqual(TEXT("material_mode"), Mirror.material_mode, Core.material_mode);
    TestEqual(TEXT("attachments"), Mirror.attachments, Core.attachments);
    TestEqual(TEXT("interior"), Mirror.interior, Core.interior);
    TestEqual(TEXT("lights"), Mirror.lights, Core.lights);
    TestEqual(TEXT("shields"), Mirror.shields, Core.shields);
    TestEqual(TEXT("animations"), Mirror.animations, Core.animations);
    TestEqual(TEXT("default_pose"), Mirror.default_pose, Core.default_pose);
    TestEqual(TEXT("collision_reserved"), Mirror.collision_reserved, Core.collision_reserved);
    return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
