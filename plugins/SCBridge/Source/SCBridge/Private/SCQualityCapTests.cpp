// SCBridge.Look.QualityCap*: without hardware ray tracing (the automation run has none: NullRHI) GI and reflections never run
// at Cinematic, which hung the GPU on the first frame (SCLook::CapQualityWithoutRayTracing), and the levels the game saves on
// exit stay the user's.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCLook.h"
#include "Scalability.h"
#include "RenderUtils.h"
#include "HAL/IConsoleManager.h"

namespace
{
// Puts the scalability levels back as they were before a test, whatever it left (temporary levels, Cinematic).
struct FRestoreQualityLevels
{
    Scalability::FQualityLevels Before = Scalability::GetQualityLevels();
    ~FRestoreQualityLevels()
    {
        Scalability::ToggleTemporaryQualityLevels(false);
        Scalability::SetQualityLevels(Before, true);
    }
};
int32 Effective(const TCHAR* Name)
{
    const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
    return Var ? Var->GetInt() : -1;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCQualityCapWithoutRayTracing, "SCBridge.Look.QualityCapWithoutRayTracing",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCQualityCapWithoutRayTracing::RunTest(const FString&)
{
    if (IsRayTracingEnabled())
    {
        AddInfo(TEXT("hardware ray tracing is on: the cap does not apply"));
        return true;
    }
    if (Scalability::IsTemporaryQualityLevelActive())
    {
        AddError(TEXT("temporary scalability levels are already active"));
        return false;
    }
    FRestoreQualityLevels Restore;
    // The user's saved levels: Cinematic GI and reflections (what -SCMaxQuality used to leave in GameUserSettings.ini).
    Scalability::FQualityLevels Saved = Restore.Before;
    Saved.GlobalIlluminationQuality = 4;
    Saved.ReflectionQuality = 4;
    Scalability::SetQualityLevels(Saved);

    SCLook::CapQualityWithoutRayTracing();
    // In effect: the sg.* variables the renderer's scalability reads.
    TestEqual(TEXT("GI at Epic"), Effective(TEXT("sg.GlobalIlluminationQuality")), 3);
    TestEqual(TEXT("reflections at Epic"), Effective(TEXT("sg.ReflectionQuality")), 3);
    TestEqual(TEXT("shadows untouched"), Effective(TEXT("sg.ShadowQuality")), Saved.ShadowQuality);
    TestEqual(TEXT("post-process untouched"), Effective(TEXT("sg.PostProcessQuality")), Saved.PostProcessQuality);
    // Saved: while temporary levels are active, GetQualityLevels and Scalability::SaveState (UGameEngine::PreExit) give the
    // levels from before them, the user's.
    TestTrue(TEXT("the cap is temporary"), Scalability::IsTemporaryQualityLevelActive());
    TestEqual(TEXT("saved GI stays the user's"), Scalability::GetQualityLevels().GlobalIlluminationQuality, 4);
    TestEqual(TEXT("saved reflections stay the user's"), Scalability::GetQualityLevels().ReflectionQuality, 4);
    Scalability::ToggleTemporaryQualityLevels(false);
    TestEqual(TEXT("the user's GI comes back"), Scalability::GetQualityLevels().GlobalIlluminationQuality, 4);
    TestEqual(TEXT("the user's reflections come back"), Scalability::GetQualityLevels().ReflectionQuality, 4);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCQualityCapAtEpic, "SCBridge.Look.QualityCapAtEpic",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCQualityCapAtEpic::RunTest(const FString&)
{
    if (IsRayTracingEnabled() || Scalability::IsTemporaryQualityLevelActive())
    {
        AddInfo(TEXT("ray tracing on or temporary levels active: nothing to check"));
        return true;
    }
    FRestoreQualityLevels Restore;
    Scalability::FQualityLevels Epic = Restore.Before;
    Epic.GlobalIlluminationQuality = 3;
    Epic.ReflectionQuality = 3;
    Scalability::SetQualityLevels(Epic);
    SCLook::CapQualityWithoutRayTracing();
    TestFalse(TEXT("levels at Epic or below are left as they are"), Scalability::IsTemporaryQualityLevelActive());
    // -SCMaxQuality without ray tracing keeps them too (Cinematic is what hangs; its hit lighting needs ray tracing).
    SCLook::ApplyMaxQuality();
    TestEqual(TEXT("-SCMaxQuality GI without ray tracing"), Effective(TEXT("sg.GlobalIlluminationQuality")), 3);
    TestEqual(TEXT("-SCMaxQuality reflections without ray tracing"), Effective(TEXT("sg.ReflectionQuality")), 3);
    return true;
}
#endif
