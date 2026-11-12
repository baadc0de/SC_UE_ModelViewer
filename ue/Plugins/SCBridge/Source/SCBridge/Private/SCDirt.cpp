// FSCDirt: dirt presets, the MPC_SCDirt push, exterior marking and the sc.Dirt console command (see SCDirt.h).
#include "SCDirt.h"
#include "SCImporter.h"
#include "SCVehicle.h"
#include "SCViewerSettings.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "HAL/IConsoleManager.h"

TConstArrayView<FSCDirt::FPreset> FSCDirt::Presets()
{
    // Chosen by eye against in-game footage of flown ships (owner allowance, 2026-11-12); sRGB -> linear.
    static const FPreset Values[] = {
        {TEXT("Dust"), FLinearColor::FromSRGBColor(FColor(118, 108, 92))},
        {TEXT("Sand"), FLinearColor::FromSRGBColor(FColor(166, 140, 100))},
        {TEXT("Mud"), FLinearColor::FromSRGBColor(FColor(78, 62, 46))},
        {TEXT("Ash"), FLinearColor::FromSRGBColor(FColor(72, 72, 74))},
    };
    return Values;
}

FLinearColor FSCDirt::DirectionCoefficients(const USCViewerSettings& Settings)
{
    const float Amount = FMath::Clamp(Settings.DirtAmount, 0.f, 1.f);
    return FLinearColor(0.f, 0.f, -FMath::Clamp(Settings.DirtUnderside, 0.f, 1.f) * Amount, 0.f);
}

void FSCDirt::Push(UWorld* World)
{
    static TWeakObjectPtr<UMaterialParameterCollection> Cached;
    if (!Cached.IsValid()) Cached = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/SCBridge/Materials/MPC_SCDirt.MPC_SCDirt"));
    UMaterialParameterCollectionInstance* Instance = World && Cached.IsValid() ? World->GetParameterCollectionInstance(Cached.Get()) : nullptr;
    if (!Instance) return;
    const USCViewerSettings* Settings = USCViewerSettings::Get();
    Instance->SetScalarParameterValue(TEXT("DirtAmount"), FMath::Clamp(Settings->DirtAmount, 0.f, 1.f));
    Instance->SetVectorParameterValue(TEXT("DirtColor"), Settings->DirtColor);
    Instance->SetVectorParameterValue(TEXT("DirtSH"), DirectionCoefficients(*Settings));
}

void FSCDirt::MarkExterior(ASCVehicle* Vehicle)
{
    if (!Vehicle) return;
    int32 Exterior = 0, Interior = 0;
    TInlineComponentArray<UPrimitiveComponent*> Primitives(Vehicle);
    for (UPrimitiveComponent* Component : Primitives)
    {
        const FLightingChannels& C = Component->LightingChannels;
        const bool bExterior = C.bChannel0 && !C.bChannel1 && !C.bChannel2;
        Component->SetCustomPrimitiveDataFloat(ExteriorDataIndex, bExterior ? 1.f : 0.f);
        ++(bExterior ? Exterior : Interior);
    }
    UE_LOG(LogSCImport, Display, TEXT("DIRT_EXTERIOR exterior=%d interior=%d"), Exterior, Interior);
}

namespace
{
// sc.Dirt <amount 0..1> [preset]: set the viewer dirt (for routes and the console); saved like the UI does.
FAutoConsoleCommandWithWorldAndArgs GSCDirtCommand(
    TEXT("sc.Dirt"), TEXT("sc.Dirt <amount 0..1> [Dust|Sand|Mud|Ash] [underside 0..1]: viewer dirt on exterior surfaces (WP-U2b)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCViewerSettings* Settings = USCViewerSettings::Get();
        if (Args.IsValidIndex(0)) Settings->DirtAmount = FMath::Clamp(FCString::Atof(*Args[0]), 0.f, 1.f);
        if (Args.IsValidIndex(1))
            for (const FSCDirt::FPreset& Preset : FSCDirt::Presets())
                if (Args[1].Equals(Preset.Name, ESearchCase::IgnoreCase)) Settings->DirtColor = Preset.Colour;
        if (Args.IsValidIndex(2)) Settings->DirtUnderside = FMath::Clamp(FCString::Atof(*Args[2]), 0.f, 1.f);
        FSCDirt::Push(World);
        UE_LOG(LogSCImport, Display, TEXT("DIRT amount=%.2f underside=%.2f colour=%s"), Settings->DirtAmount, Settings->DirtUnderside,
               *Settings->DirtColor.ToString());
    }));
}
