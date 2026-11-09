// Package-level light invariants: counts are derived from the manifest, never ship-name expectations.
#include "SCLightImporter.h"
#include "SCVehicle.h"
#include "SCImporter.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Components/LightComponent.h"
#include "Components/LocalLightComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "HAL/IConsoleManager.h"

bool FSCLightImporter::Verify(ASCVehicle* Vehicle)
{
    struct FExpected
    {
        FString Name;
        FString Gobo;
        FName State;
        bool Enabled;
        bool Shadow;
    };
    TArray<FExpected> Expected;
    TSet<FString> Textures;
    auto Collect = [&](const FSCSceneInstance& Instance) {
        TArray<TSharedPtr<FJsonObject>> Records;
        TArray<double> Radii;
        for (const auto& Text : Instance.LightJson)
        {
            TSharedPtr<FJsonObject> J;
            if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) || !J) continue;
            const FString Kind = J->GetStringField(TEXT("semantic_light_kind"));
            if (Kind == TEXT("ambient_proxy")) continue;
            Records.Add(J);
            if (Kind != TEXT("sun")) Radii.Add(J->GetNumberField(TEXT("radius_m")));
        }
        Radii.Sort();
        const double Threshold = Radii.IsEmpty() ? 0 : Radii[Radii.Num() * 3 / 4];
        for (const auto& J : Records)
        {
            FString State;
            J->TryGetStringField(TEXT("active_state"), State);
            if (State.IsEmpty()) State = TEXT("defaultState");
            double Intensity = J->GetNumberField(TEXT("intensity_candela_proxy"));
            const TSharedPtr<FJsonObject>* States;
            const TSharedPtr<FJsonObject>* Authored;
            if (J->TryGetObjectField(TEXT("states"), States))
            {
                if (State != TEXT("offState") && (*States)->TryGetObjectField(TEXT("defaultState"), Authored)) State = TEXT("defaultState");
                if ((*States)->TryGetObjectField(State, Authored))
                    (*Authored)->TryGetNumberField(TEXT("intensity_candela_proxy"), Intensity);
            }
            FString Gobo;
            J->TryGetStringField(TEXT("projector_texture"), Gobo);
            if (!Gobo.IsEmpty()) Textures.Add(Gobo);
            Expected.Add(
                {J->GetStringField(TEXT("name")), Gobo, FName(*State), State != TEXT("offState") && Intensity > 0,
                 J->GetStringField(TEXT("semantic_light_kind")) == TEXT("sun") || J->GetNumberField(TEXT("radius_m")) >= Threshold});
        }
    };
    Collect(Vehicle->Manifest.Root);
    for (const auto& I : Vehicle->Manifest.Children)
        Collect(I);
    for (const auto& I : Vehicle->Manifest.Interiors)
    {
        Collect(I.Container);
        for (const auto& Placement : I.Placements)
            Collect(Placement);
    }
    bool OK = Expected.Num() == Vehicle->Lights.Num() && Vehicle->LightStates.Num() == Vehicle->Lights.Num();
    TMap<UTexture*, UMaterialInstanceDynamic*> Shared;
    TSet<UMaterialInterface*> Materials;
    int32 Disabled = 0, Fallback = 0, Shadowed = 0, VisibleShadowed = 0, Projectors = 0;
    for (int32 I = 0; I < Vehicle->Lights.Num(); ++I)
    {
        ULightComponent* Light = Vehicle->Lights[I];
        if (!Expected.IsValidIndex(I) || !Vehicle->LightStates.IsValidIndex(I))
        {
            OK = false;
            continue;
        }
        const auto& Want = Expected[I];
        const auto& State = Vehicle->LightStates[I];
        OK &= State.Component == Light && State.Name == Want.Name && State.ActiveState == Want.State;
        OK &= State.bEnabled == Want.Enabled && Light->IsVisible() == Want.Enabled;
        OK &= Want.Enabled ? Light->Intensity > 0 : Light->Intensity == 0;
        OK &= State.bFallbackShadow == Want.Shadow;
        OK &= bool(Light->CastShadows) == (Vehicle->bMegaLightsActive || Want.Shadow);
        Disabled += !Light->IsVisible();
        Fallback += Want.Shadow;
        Shadowed += bool(Light->CastShadows);
        VisibleShadowed += Light->IsVisible() && Light->CastShadows;
        if (!Want.Gobo.IsEmpty())
        {
            ++Projectors;
            auto* MID = Cast<UMaterialInstanceDynamic>(Light->LightFunctionMaterial);
            UTexture* Texture = nullptr;
            if (!MID || !MID->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Gobo")), Texture, true))
            {
                OK = false;
                continue;
            }
            OK &= Texture == Vehicle->GoboTextures.FindRef(Want.Gobo);
            if (auto* Existing = Shared.Find(Texture))
                OK &= *Existing == MID;
            else
                Shared.Add(Texture, MID);
            Materials.Add(MID);
        }
    }
    OK &= Materials.Num() == Textures.Num() && Vehicle->GoboTextures.Num() == Textures.Num();
    const auto* Atlas = IConsoleManager::Get().FindConsoleVariable(TEXT("r.LightFunctionAtlas.Size"));
    const int32 Edge = Atlas ? Atlas->GetInt() : 0;
    UE_LOG(LogSCImport, Display,
           TEXT("LIGHT_VERIFY passed=%d components=%d enabled=%d disabled=%d projectors=%d gobo_textures=%d gobo_materials=%d "
                "fallback_selected=%d shadow_flags=%d visible_shadowed=%d megalights_active=%d atlas_slots=%d"),
           OK, Vehicle->Lights.Num(), Vehicle->Lights.Num() - Disabled, Disabled, Projectors, Textures.Num(), Materials.Num(), Fallback,
           Shadowed, VisibleShadowed, Vehicle->bMegaLightsActive, Edge * Edge);
    return OK;
}
