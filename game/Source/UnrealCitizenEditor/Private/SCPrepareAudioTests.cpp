// WP-O1-4: a package's audio staged beside its prepared level (SCPrepareAudio.h).
#include "Misc/AutomationTest.h"
#include "SCPackageSource.h"
#include "SCPrepareAudio.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCPrepareAudioTest, "SCBridge.Prepare.AudioStaging",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCPrepareAudioTest::RunTest(const FString&)
{
    const FString Root = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / TEXT("SCPrepareAudio") / FGuid::NewGuid().ToString());
    const FString Export = Root / TEXT("export"), Target = Root / TEXT("Content/SCAudio/Level");
    auto Put = [&](const FString& Rel, const FString& Text) { FFileHelper::SaveStringToFile(Text, *(Export / Rel)); };
    const FString Scene = TEXT("Packages/P/scene.json");
    Put(Scene, TEXT("{\"interiors\": [{\"name\": \"room\", \"audio_spots\": [{\"id\": 1}], \"room_volumes\": [], \"placements\": [1, 2]}, {\"name\": \"bare\"}]}"));
    Put(TEXT("Packages/P/audio.json"), TEXT("{\"media\": {\"1\": {\"file\": \"Packages/P/Audio/1.ogg\"}, \"2\": {\"file\": \"\"}}, ")
                                       TEXT("\"environments\": {\"e\": {\"impulse_response\": {\"file\": \"Packages/P/Audio/ir/e.f32\"}}, \"dry\": {}}}"));
    Put(TEXT("Packages/P/Audio/1.ogg"), TEXT("ogg"));
    Put(TEXT("Packages/P/Audio/ir/e.f32"), TEXT("ir"));
    FSCFolderPackageSource Source(Export);

    int32 Files = 0;
    FString Error;
    TestTrue(TEXT("staged: ") + Error, SCPrepareAudio::Stage(Source, Scene, Target, Files, Error));
    TestEqual(TEXT("the media and the impulse response"), Files, 2);
    FString Text;
    TestTrue(TEXT("audio.json beside the package path"), FFileHelper::LoadFileToString(Text, *(Target / TEXT("Packages/P/audio.json"))) && Text.Contains(TEXT("1.ogg")));
    TestTrue(TEXT("the media at its export path"), FFileHelper::LoadFileToString(Text, *(Target / TEXT("Packages/P/Audio/1.ogg"))) && Text == TEXT("ogg"));
    TestTrue(TEXT("the impulse response"), FPaths::FileExists(Target / TEXT("Packages/P/Audio/ir/e.f32")));
    TestTrue(TEXT("the interiors' records"), FFileHelper::LoadFileToString(Text, *(Target / TEXT("Packages/P/audio_interiors.json"))));
    TestTrue(TEXT("... per interior its audio records"), Text.Contains(TEXT("\"name\":\"room\"")) && Text.Contains(TEXT("\"audio_spots\":[{\"id\":1}]")));
    TestTrue(TEXT("... an interior without them has empty lists"), Text.Contains(TEXT("\"name\":\"bare\",\"audio_spots\":[],\"audio_areas\":[],\"room_volumes\":[]")));
    TestFalse(TEXT("... nothing else of the scene"), Text.Contains(TEXT("placements")));
    TestFalse(TEXT("no staging folder is left"), IFileManager::Get().DirectoryExists(*(Target + TEXT(".staging"))));

    // A payload the manifest lists is missing: refused, and the previous staging stays.
    IFileManager::Get().Delete(*(Export / TEXT("Packages/P/Audio/ir/e.f32")));
    TestFalse(TEXT("a missing payload is refused"), SCPrepareAudio::Stage(Source, Scene, Target, Files, Error));
    TestTrue(TEXT("... named"), Error.Contains(TEXT("e.f32")));
    TestTrue(TEXT("... the previous staging stays"), FPaths::FileExists(Target / TEXT("Packages/P/Audio/ir/e.f32")));
    // No audio.json (an export before cache format 36).
    IFileManager::Get().Delete(*(Export / TEXT("Packages/P/audio.json")));
    TestFalse(TEXT("no audio.json is refused"), SCPrepareAudio::Stage(Source, Scene, Target, Files, Error));
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
