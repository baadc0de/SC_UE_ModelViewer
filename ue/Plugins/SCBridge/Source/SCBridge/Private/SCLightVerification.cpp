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
#include "Components/SpotLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/World.h"
#include "UObject/UObjectIterator.h"

TArray<FString> FSCLightImporter::Probe(const UWorld* World, const FVector& Point, const AActor* Ignore, int32 Top, uint8 ReceiverChannels)
{
    struct FRow { double Lux; FString Text; };
    TArray<FRow> Rows;
    double Total = 0, Visible = 0, GatedLux = 0;
    int32 Gated = 0;
    for (TObjectIterator<ULocalLightComponent> It; It; ++It)
    {
        const ULocalLightComponent* L = *It;
        if (L->GetWorld() != World || !L->IsRegistered() || !L->IsVisible() || L->Intensity <= 0) continue;
        const FVector Origin = L->GetComponentLocation();
        const double Cm = FVector::Dist(Origin, Point), Radius = L->AttenuationRadius;
        if (Cm >= Radius) continue;
        const auto* Spot = Cast<USpotLightComponent>(L);
        const auto* Bulb = Cast<UPointLightComponent>(L);
        double Cone = 1;
        if (Spot)
        {
            const double CosOuter = FMath::Cos(FMath::DegreesToRadians(Spot->OuterConeAngle));
            const double CosInner = FMath::Cos(FMath::DegreesToRadians(FMath::Min(Spot->InnerConeAngle, Spot->OuterConeAngle)));
            const double CosPoint = FVector::DotProduct(L->GetForwardVector(), (Point - Origin).GetSafeNormal());
            Cone = FMath::Square(FMath::Clamp((CosPoint - CosOuter) / FMath::Max(1e-4, CosInner - CosOuter), 0., 1.));
        }
        const double Candela = L->Intensity * ULocalLightComponent::GetUnitsConversionFactor(
            L->IntensityUnits, ELightUnits::Candelas, Spot ? FMath::Cos(FMath::DegreesToRadians(Spot->OuterConeAngle)) : -1.f);
        const double M = FMath::Max3(Cm, Bulb ? double(Bulb->SourceRadius) : 0., 1.) / 100.;
        const double Lux = Candela / (M * M) * FMath::Square(1 - FMath::Pow(Cm / Radius, 4.)) * Cone;
        if (Lux <= 0) continue;
        const uint8 Channels = L->LightingChannels.bChannel0 | (L->LightingChannels.bChannel1 << 1) | (L->LightingChannels.bChannel2 << 2);
        if (ReceiverChannels && !(Channels & ReceiverChannels)) { ++Gated; GatedLux += Lux; continue; }
        FHitResult Hit;
        FCollisionQueryParams Q(SCENE_QUERY_STAT(SCLightProbe), true, Ignore);
        const bool bBlocked = World->LineTraceSingleByChannel(Hit, Origin, Point, ECC_Visibility, Q) && Hit.Distance < Cm - 2;
        Total += Lux;
        if (!bBlocked) Visible += Lux;
        Rows.Add({Lux, FString::Printf(TEXT("lux=%.1f cd=%.0f d=%.2fm r=%.1fm cone=%.2f blocked=%d%s %s/%s"), Lux, Candela, Cm / 100., Radius / 100.,
                                       Cone, bBlocked, bBlocked ? *FString::Printf(TEXT("@%.2fm"), Hit.Distance / 100.) : TEXT(""),
                                       L->GetOwner() ? *L->GetOwner()->GetName() : TEXT("?"),
                                       L->ComponentTags.Num() ? *L->ComponentTags[0].ToString() : *L->GetName())});
    }
    Rows.Sort([](const FRow& A, const FRow& B) { return A.Lux > B.Lux; });
    TArray<FString> Out;
    Out.Add(FString::Printf(TEXT("point=%s channels=%d lights=%d lux_unshadowed=%.1f lux_visible=%.1f gated=%d lux_gated=%.1f"),
                            *Point.ToCompactString(), ReceiverChannels, Rows.Num(), Total, Visible, Gated, GatedLux));
    for (int32 I = 0; I < FMath::Min(Top, Rows.Num()); ++I) Out.Add(Rows[I].Text);
    return Out;
}

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
