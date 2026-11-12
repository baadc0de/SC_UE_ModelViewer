// Viewer dirt (WP-U2b): exterior marking from the room classification and the settings -> console round trip.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCDirt.h"
#include "SCViewerSettings.h"
#include "SCVehicle.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCDirtExterior, "SCBridge.Dirt.Exterior", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCDirtExterior::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    auto* Vehicle = World->SpawnActor<ASCVehicle>();
    auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    auto Add = [&](bool C0, bool C1, bool C2) {
        auto* C = NewObject<UStaticMeshComponent>(Vehicle);
        C->SetupAttachment(Vehicle->GetRootComponent());
        C->SetStaticMesh(Cube);
        C->RegisterComponent();
        C->SetLightingChannels(C0, C1, C2);
        return C;
    };
    auto* Hull = Add(true, false, false);      // outside every room (lighting channel 0)
    auto* Cockpit = Add(false, true, false);   // a room on channel 1
    auto* Cargo = Add(false, false, true);     // a room on channel 2
    FSCDirt::MarkExterior(Vehicle);
    auto Slot = [](const UPrimitiveComponent* C) {
        const auto& D = C->GetCustomPrimitiveData().Data;
        return D.IsValidIndex(FSCDirt::ExteriorDataIndex) ? D[FSCDirt::ExteriorDataIndex] : -1.f;
    };
    TestEqual(TEXT("outside every room: exterior"), Slot(Hull), 1.f);
    TestEqual(TEXT("room on channel 1: interior"), Slot(Cockpit), 0.f);
    TestEqual(TEXT("room on channel 2: interior"), Slot(Cargo), 0.f);

    // Presets are viewer choices; the settings default is one of them, and the console command sets both values.
    TestTrue(TEXT("presets exist"), FSCDirt::Presets().Num() >= 2);
    USCViewerSettings* Settings = USCViewerSettings::Get();
    const float Amount = Settings->DirtAmount;
    const FLinearColor Colour = Settings->DirtColor;
    const float Underside = Settings->DirtUnderside;
    IConsoleManager::Get().ProcessUserConsoleInput(*FString::Printf(TEXT("sc.Dirt 0.6 %s"), FSCDirt::Presets()[1].Name), *GLog, World);
    TestEqual(TEXT("console sets the amount"), Settings->DirtAmount, 0.6f);
    TestTrue(TEXT("console sets the preset colour"), Settings->DirtColor.Equals(FSCDirt::Presets()[1].Colour));
    IConsoleManager::Get().ProcessUserConsoleInput(TEXT("sc.Dirt 0.4 Dust 0.75"), *GLog, World);
    TestEqual(TEXT("console sets the underside bias"), Settings->DirtUnderside, 0.75f);
    // SH2_SampleValue_Clamped coefficients: w = amount, xyz = the underside bias along object -Z (scaled by amount).
    const FLinearColor SH = FSCDirt::DirectionCoefficients(*Settings);
    TestTrue(TEXT("underside tilts dirt toward -Z"), FMath::IsNearlyEqual(SH.B, -0.3f) && SH.R == 0.f && SH.G == 0.f);
    Settings->DirtAmount = Amount;
    Settings->DirtColor = Colour;
    Settings->DirtUnderside = Underside;
    Vehicle->Destroy();
    World->DestroyWorld(false);
    return true;
}
#endif
