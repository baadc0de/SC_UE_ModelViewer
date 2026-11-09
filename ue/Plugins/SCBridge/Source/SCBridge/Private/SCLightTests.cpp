// Regression fixtures for instance-local shadow budgets and authored default light states.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCLightImporter.h"
#include "SCVehicle.h"
#include "Engine/World.h"
#include "Components/LightComponent.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLightDefaults, "SCBridge.Lights.DefaultsAndShadowBudget",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLightDefaults::RunTest(const FString&)
{
    auto* MegaLights = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MegaLights.EnableForProject"));
    const int32 Previous = MegaLights->GetInt();
    MegaLights->Set(0, ECVF_SetByCode);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    auto* Vehicle = World->SpawnActor<ASCVehicle>();
    FSCFolderPackageSource Source(TEXT("/tmp"));
    TArray<FString> Warnings;
    const auto Helper = [](const FString&) -> USceneComponent* { return nullptr; };
    FSCSceneInstance Instance;
    for (int32 Radius = 1; Radius <= 4; ++Radius)
        Instance.LightJson.Add(FString::Printf(
            TEXT(
                R"({"name":"Radius%d","semantic_light_kind":"point","transform_basis":"cryengine_z_up","radius_m":%d,"intensity_candela_proxy":25})"),
            Radius, Radius));
    Instance.LightJson.Add(TEXT(R"({"name":"Ambient","semantic_light_kind":"ambient_proxy","radius_m":100})"));
    FSCLightImporter::Build(Source, Instance, Vehicle, Vehicle->GetRootComponent(), Helper, Warnings);
    TestEqual(TEXT("four local components"), Vehicle->Lights.Num(), 4);
    for (int32 I = 0; I < 4; ++I)
        TestEqual(TEXT("upper quartile selects only radius four"), bool(Vehicle->Lights[I]->CastShadows), I == 3);
    FSCSceneInstance Other;
    Other.LightJson.Add(TEXT(
        R"({"name":"OtherInstance","semantic_light_kind":"point","transform_basis":"cryengine_z_up","radius_m":0.5,"intensity_candela_proxy":25})"));
    FSCLightImporter::Build(Source, Other, Vehicle, Vehicle->GetRootComponent(), Helper, Warnings);
    TestTrue(TEXT("threshold is per instance, not per ship"), Vehicle->Lights.Last()->CastShadows);
    FSCSceneInstance States;
    for (
        const TCHAR* State :
        {TEXT(R"("active_state":"offState","states":{"offState":{"intensity_candela_proxy":99}})"),
         TEXT(
             R"("active_state":"auxiliaryState","states":{"defaultState":{"intensity_candela_proxy":0},"auxiliaryState":{"intensity_candela_proxy":25}})"),
         TEXT(R"("states":{"defaultState":{"intensity_candela_proxy":0}})"),
         TEXT(R"("active_state":"defaultState","states":{"defaultState":{"intensity_candela_proxy":25}})")})
    {
        States.LightJson.Add(FString::Printf(
            TEXT(
                R"({"name":"StateFixture","semantic_light_kind":"point","transform_basis":"cryengine_z_up","radius_m":2,"intensity_candela_proxy":25,%s})"),
            State));
    }
    const int32 Begin = Vehicle->Lights.Num();
    FSCLightImporter::Build(Source, States, Vehicle, Vehicle->GetRootComponent(), Helper, Warnings);
    for (int32 I = 0; I < 4; ++I)
        TestEqual(TEXT("off and zero-default states import disabled"), Vehicle->Lights[Begin + I]->IsVisible(), I == 3);
    TestEqual(TEXT("one state property per component"), Vehicle->LightStates.Num(), Vehicle->Lights.Num());
    TestEqual(TEXT("export fallback preserved for UI"), Vehicle->LightStates[Begin + 1].ExportActiveState, FName(TEXT("auxiliaryState")));
    TestEqual(TEXT("viewer uses authored default"), Vehicle->LightStates[Begin + 1].ActiveState, FName(TEXT("defaultState")));
    TestTrue(TEXT("full state map preserved for UI"), Vehicle->LightStates[Begin + 1].StatesJson.Contains(TEXT("auxiliaryState")));
    // Null-RHI cannot provide MegaLights even when the project preference is on.
    MegaLights->Set(1, ECVF_SetByCode);
    Vehicle->RefreshLightShadowPolicy();
    if (FParse::Param(FCommandLine::Get(), TEXT("nullrhi")))
    {
        TestFalse(TEXT("null-RHI does not masquerade as active MegaLights"), Vehicle->bMegaLightsActive);
        TestFalse(TEXT("runtime refresh retains fallback on null-RHI"), Vehicle->Lights[0]->CastShadows);
    }
    TestEqual(TEXT("valid fixtures have no warnings"), Warnings.Num(), 0);
    Vehicle->Destroy();
    World->DestroyWorld(false);
    MegaLights->Set(Previous, ECVF_SetByCode);
    return true;
}
#endif
