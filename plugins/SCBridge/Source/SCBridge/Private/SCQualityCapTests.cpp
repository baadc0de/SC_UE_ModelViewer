// SCBridge.Look.QualityCap*: without hardware ray tracing (the automation run has none: NullRHI) GI and reflections never run
// at Cinematic, which hung the GPU on the first frame (SCLook::CapQualityWithoutRayTracing), the levels the game saves on
// exit stay the user's, and a change of the user's levels during the run still takes effect (SCLook::FollowQualityChanges).
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCLook.h"
#include "Scalability.h"
#include "RenderUtils.h"
#include "HAL/IConsoleManager.h"

namespace
{
// Puts the scalability levels back as they were before a test, whatever it left (the run's levels, Cinematic).
struct FRestoreQualityLevels
{
    Scalability::FQualityLevels Before = Scalability::GetQualityLevels();
    ~FRestoreQualityLevels()
    {
        SCLook::EndRunQuality();
        Scalability::SetQualityLevels(Before, true);
    }
};
int32 Effective(const TCHAR* Name)
{
    const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
    return Var ? Var->GetInt() : -1;
}
// The tests need no ray tracing and start from the user's own levels (no temporary levels left by anything else).
bool Applies(FAutomationTestBase& Test)
{
    if (IsRayTracingEnabled())
    {
        Test.AddInfo(TEXT("hardware ray tracing is on: the cap does not apply"));
        return false;
    }
    if (Scalability::IsTemporaryQualityLevelActive()) Test.AddError(TEXT("temporary scalability levels are already active"));
    return !Scalability::IsTemporaryQualityLevelActive();
}
// The user's levels with GI and reflections at the given level, set as a user's levels are (forced: whatever priority the
// sg.* variables have).
Scalability::FQualityLevels SetUserLevels(Scalability::FQualityLevels Levels, int32 GIAndReflections)
{
    Levels.GlobalIlluminationQuality = GIAndReflections;
    Levels.ReflectionQuality = GIAndReflections;
    Scalability::SetQualityLevels(Levels, true);
    return Levels;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCQualityCapWithoutRayTracing, "SCBridge.Look.QualityCapWithoutRayTracing",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCQualityCapWithoutRayTracing::RunTest(const FString&)
{
    if (!Applies(*this)) return !HasAnyErrors();
    FRestoreQualityLevels Restore;
    // The user's saved levels: Cinematic GI and reflections (what -SCMaxQuality used to leave in GameUserSettings.ini).
    const Scalability::FQualityLevels Saved = SetUserLevels(Restore.Before, 4);

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
    SCLook::EndRunQuality();
    TestFalse(TEXT("ended: no temporary levels"), Scalability::IsTemporaryQualityLevelActive());
    TestEqual(TEXT("ended: the user's GI in effect"), Effective(TEXT("sg.GlobalIlluminationQuality")), 4);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCQualityCapFollowsChanges, "SCBridge.Look.QualityCapFollowsChanges",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCQualityCapFollowsChanges::RunTest(const FString&)
{
    if (!Applies(*this)) return !HasAnyErrors();
    FRestoreQualityLevels Restore;
    Scalability::FQualityLevels User = SetUserLevels(Restore.Before, 4);
    SCLook::CapQualityWithoutRayTracing();
    // A change of the levels during the run (console `scalability`, UGameUserSettings::ApplySettings): non-forced.
    User.ShadowQuality = User.ShadowQuality == 1 ? 2 : 1;
    Scalability::SetQualityLevels(User);
    SCLook::FollowQualityChanges();
    TestEqual(TEXT("the changed shadow level takes effect"), Effective(TEXT("sg.ShadowQuality")), User.ShadowQuality);
    TestEqual(TEXT("GI stays capped"), Effective(TEXT("sg.GlobalIlluminationQuality")), 3);
    TestEqual(TEXT("the change is the saved level"), Scalability::GetQualityLevels().ShadowQuality, User.ShadowQuality);
    // The user lowers GI below the cap: no run-only levels are needed any more.
    User.GlobalIlluminationQuality = 2;
    User.ReflectionQuality = 2;
    Scalability::SetQualityLevels(User);
    SCLook::FollowQualityChanges();
    TestFalse(TEXT("at Epic or below: no temporary levels"), Scalability::IsTemporaryQualityLevelActive());
    TestEqual(TEXT("the lowered GI takes effect"), Effective(TEXT("sg.GlobalIlluminationQuality")), 2);
    // ... and raises it to Cinematic again (here set directly, as `sg.GlobalIlluminationQuality 4` would): capped again.
    SetUserLevels(User, 4);
    SCLook::FollowQualityChanges();
    TestEqual(TEXT("raised again: GI capped"), Effective(TEXT("sg.GlobalIlluminationQuality")), 3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCQualityCapAtEpic, "SCBridge.Look.QualityCapAtEpic",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCQualityCapAtEpic::RunTest(const FString&)
{
    if (!Applies(*this)) return !HasAnyErrors();
    FRestoreQualityLevels Restore;
    SetUserLevels(Restore.Before, 3);
    SCLook::CapQualityWithoutRayTracing();
    TestFalse(TEXT("levels at Epic or below are left as they are"), Scalability::IsTemporaryQualityLevelActive());
    // -SCMaxQuality without ray tracing keeps them too (Cinematic is what hangs; its hit lighting needs ray tracing).
    SCLook::ApplyMaxQuality();
    TestEqual(TEXT("-SCMaxQuality GI without ray tracing"), Effective(TEXT("sg.GlobalIlluminationQuality")), 3);
    TestEqual(TEXT("-SCMaxQuality reflections without ray tracing"), Effective(TEXT("sg.ReflectionQuality")), 3);
    return true;
}
#endif
