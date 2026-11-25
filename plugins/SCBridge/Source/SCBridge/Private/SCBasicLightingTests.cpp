// SCBridge.Look.BasicLighting: without hardware ray tracing (the automation run has none: NullRHI) the startup look keeps
// every light's virtual shadow map filtered (32 lights per pixel) and holds 60 fps with dynamic resolution (SCLook).
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCLook.h"
#include "HAL/IConsoleManager.h"
#include "RenderUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCBasicLighting, "SCBridge.Look.BasicLighting",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCBasicLighting::RunTest(const FString&)
{
    if (IsRayTracingEnabled())
    {
        AddInfo(TEXT("hardware ray tracing is on: Basic lighting does not apply"));
        return true;
    }
    const TCHAR* Names[] = {TEXT("r.Shadow.Virtual.OnePassProjection.MaxLightsPerPixel"), TEXT("r.DynamicRes.OperationMode"),
                            TEXT("r.DynamicRes.MinScreenPercentage"), TEXT("r.DynamicRes.MaxScreenPercentage"), TEXT("r.DynamicRes.FrameTimeBudget")};
    // Restored afterwards at the priority they had (code priority would pin them for the rest of the session).
    TMap<FString, TPair<FString, EConsoleVariableFlags>> Before;
    for (const TCHAR* Name : Names)
        if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
            Before.Add(Name, {Var->GetString(), EConsoleVariableFlags(Var->GetFlags() & ECVF_SetByMask)});
    SCLook::ApplyBasicLighting();
    auto Int = [](const TCHAR* Name) { const IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name); return V ? V->GetInt() : -1; };
    auto Float = [](const TCHAR* Name) { const IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name); return V ? V->GetFloat() : -1.f; };
    TestEqual(TEXT("32 filtered virtual shadow maps per pixel"), Int(Names[0]), 32);
    TestEqual(TEXT("dynamic resolution on"), Int(Names[1]), 2);
    TestEqual(TEXT("down to half resolution"), Float(Names[2]), 50.f);
    TestEqual(TEXT("up to full resolution"), Float(Names[3]), 100.f);
    TestEqual(TEXT("held at 60 fps"), Float(Names[4]), 1000.f / 60.f, 1e-3f);
    for (const auto& [Name, Value] : Before)
        if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(*Name))
        {
            Var->Unset(ECVF_SetByCode); // drop the test's code-priority value, back to what was set below it
            if (Var->GetString() != Value.Key) Var->Set(*Value.Key, Value.Value);
        }
    return true;
}
#endif
