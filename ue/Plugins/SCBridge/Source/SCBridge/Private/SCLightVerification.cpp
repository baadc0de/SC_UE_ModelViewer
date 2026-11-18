// Package-level light invariants: counts are derived from the manifest, never ship-name expectations.
#include "SCLightImporter.h"
#include "SCStarEngine.h"
#include "SCViewerSettings.h"
#include "SCVehicle.h"
#include "SCImporter.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Components/LightComponent.h"
#include "Components/LocalLightComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "HAL/IConsoleManager.h"
#include "LightDefinitions.h" // light function atlas capacities (engine fork, WP-E1 E1-1)
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
        if (Spot && Spot->Shape == ESpotLightShape::SquareFrustum)
        {
            // The engine fork's square frustum (WP-E1 E1-3, LocalLightFalloff.ush): hard-edged square of half-FOV OuterConeAngle
            // in light space, no cone falloff, nothing in front of the projector near clip.
            const FVector Local = L->GetComponentTransform().InverseTransformVectorNoScale(Point - Origin);
            const double Tan = FMath::Tan(USpotLightComponent::GetClampedSquareFrustumHalfAngle(Spot->OuterConeAngle));
            Cone = Local.X > 0 && FMath::Max(FMath::Abs(Local.Y), FMath::Abs(Local.Z)) <= Local.X * Tan && Local.X >= Spot->ProjectorNearClip ? 1 : 0;
        }
        else if (Spot)
        {
            const double CosOuter = FMath::Cos(FMath::DegreesToRadians(Spot->OuterConeAngle));
            const double CosInner = FMath::Cos(FMath::DegreesToRadians(FMath::Min(Spot->InnerConeAngle, Spot->OuterConeAngle)));
            const double CosPoint = FVector::DotProduct(L->GetForwardVector(), (Point - Origin).GetSafeNormal());
            Cone = FMath::Square(FMath::Clamp((CosPoint - CosOuter) / FMath::Max(1e-4, CosInner - CosOuter), 0., 1.));
        }
        const double Candela = L->Intensity * ULocalLightComponent::GetUnitsConversionFactor(
            L->IntensityUnits, ELightUnits::Candelas, Spot ? FMath::Cos(FMath::DegreesToRadians(Spot->OuterConeAngle)) : -1.f);
        // The light's own falloff models (engine fork, WP-E1 E1-2), as the renderer evaluates them (LocalLightFalloff.ush):
        // GetPhysicalLightAttenuation is the bulb offset law candela / (max(d, b) + c)^2 on point and spot lights, UE's
        // inverse square otherwise; GetLightForcedDistanceFade is the squared linear range fade.
        const bool bBulbOffset = Bulb && L->DistanceAttenuation == ELightDistanceAttenuation::BulbOffset &&
                                 ULocalLightComponent::IsValidBulbOffsetAttenuation(L->AttenuationBulbRadius, L->AttenuationDistanceOffset);
        const double M = (bBulbOffset ? FMath::Max(Cm, double(L->AttenuationBulbRadius)) + double(L->AttenuationDistanceOffset) : FMath::Max(1., Cm)) / 100.;
        const double Fade = L->RangeFade == ELightRangeFade::SquaredLinearTail
                                ? FMath::Square(FMath::Clamp((1.0 - Cm / Radius) / double(L->RangeFadeFraction), 0.0, 1.0))
                                : FMath::Square(FMath::Clamp(1.0 - FMath::Square(FMath::Square(Cm / Radius)), 0.0, 1.0));
        const double Lux = Candela / (M * M) * Fade * Cone;
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
        bool Local;
        double Candela;
    };
    TArray<FExpected> Expected;
    TSet<FString> Textures;
    auto Collect = [&](const FSCSceneInstance& Instance) {
        // The importer's own rules decide which records become components and where the shadow quartile lies.
        const double Threshold = FSCLightImporter::ShadowRadiusThreshold(Instance.LightJson);
        for (const auto& Text : Instance.LightJson)
        {
            TSharedPtr<FJsonObject> J;
            if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) || !J) continue;
            const UClass* Class = FSCLightImporter::ComponentClass(J);
            if (!Class) continue;
            const bool bLocal = Class->IsChildOf(ULocalLightComponent::StaticClass());
            FString State;
            J->TryGetStringField(TEXT("active_state"), State);
            if (State.IsEmpty()) State = TEXT("defaultState");
            TSharedPtr<FJsonObject> Selected = J;
            const TSharedPtr<FJsonObject>* States;
            const TSharedPtr<FJsonObject>* Authored;
            if (J->TryGetObjectField(TEXT("states"), States))
            {
                if (State != TEXT("offState") && (*States)->TryGetObjectField(TEXT("defaultState"), Authored)) State = TEXT("defaultState");
                if ((*States)->TryGetObjectField(State, Authored)) Selected = *Authored;
            }
            // The importer's intensity rule (authored light model, else the exporter's proxy) decides what is lit.
            const double Intensity = FSCLightImporter::StateCandela(J, Selected);
            FString Gobo;
            J->TryGetStringField(TEXT("projector_texture"), Gobo);
            if (!Gobo.IsEmpty()) Textures.Add(Gobo);
            // Same fallback rule as the importer: the authored shadowParams.shadowCasting flag when the record carries
            // the authored light model, else the radius quartile; lights that are not local (the sun) always cast.
            bool bAuthoredShadow = false;
            double RadiusM = 0;
            J->TryGetNumberField(TEXT("radius_m"), RadiusM);
            const bool bShadow = J->HasField(TEXT("bulb_radius_m")) && J->TryGetBoolField(TEXT("shadow_casting"), bAuthoredShadow)
                                     ? bAuthoredShadow
                                     : FMath::Max(0., RadiusM) >= Threshold;
            FString Name;
            J->TryGetStringField(TEXT("name"), Name);
            const bool bEnabled = State != TEXT("offState") && Intensity > 0;
            Expected.Add({Name, Gobo, FName(*State), bEnabled, !bLocal || bShadow, bLocal, bEnabled ? Intensity : 0.});
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
        // The component carries the rule's candela (float precision), in candela.
        OK &= FMath::IsNearlyEqual(double(Light->Intensity), Want.Candela, 1e-5 * FMath::Max(1.0, Want.Candela));
        const auto* Local = Cast<ULocalLightComponent>(Light);
        OK &= (Local != nullptr) == Want.Local && (!Local || Local->IntensityUnits == ELightUnits::Candelas);
        OK &= State.bFallbackShadow == Want.Shadow;
        OK &= bool(Light->CastShadows) == (Want.Shadow || USCViewerSettings::Get()->bShadowAllLights); // authored rule (WP-L1), or every light (look, D15)
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
    // The light function atlas capacities as the engine clamps them (LightDefinitions.h; LightFunctionAtlas.cpp): material tiles
    // (Size^2, Size in [2, LIGHT_FUNCTION_ATLAS_MAX_EDGE_SIZE]) and light records (MaxLightCount, -1 = every addressable record;
    // record 0 is reserved). The engine logs what it allocated ("LightFunctionAtlas: ...").
    const auto* Atlas = IConsoleManager::Get().FindConsoleVariable(TEXT("r.LightFunctionAtlas.Size"));
    const auto* AtlasLights = IConsoleManager::Get().FindConsoleVariable(TEXT("r.LightFunctionAtlas.MaxLightCount"));
    const int32 Edge = FMath::Clamp(Atlas ? Atlas->GetInt() : 0, 2, int32(LIGHT_FUNCTION_ATLAS_MAX_EDGE_SIZE));
    const int32 MaxRecords = int32(LIGHT_FUNCTION_ATLAS_MAX_LIGHT_RECORD_COUNT);
    const int32 MaxLights = AtlasLights ? AtlasLights->GetInt() : -1;
    const int32 Records = FMath::Max((MaxLights < 0 ? MaxRecords : FMath::Min(MaxLights, MaxRecords)) - 1, 0);
    UE_LOG(LogSCImport, Display,
           TEXT("LIGHT_VERIFY passed=%d components=%d enabled=%d disabled=%d projectors=%d gobo_textures=%d gobo_materials=%d "
                "fallback_selected=%d shadow_flags=%d visible_shadowed=%d megalights_active=%d atlas_slots=%d atlas_light_records=%d"),
           OK, Vehicle->Lights.Num(), Vehicle->Lights.Num() - Disabled, Disabled, Projectors, Textures.Num(), Materials.Num(), Fallback,
           Shadowed, VisibleShadowed, Vehicle->bMegaLightsActive, Edge * Edge, Records);
    return OK;
}

namespace
{
// The imported falloff models of one light, kept while sc.Lights.FalloffModels 0 shows UE's falloff in their place.
struct FSCImportedFalloff
{
    ELightRangeFade RangeFade;
    float RangeFadeFraction;
    ELightDistanceAttenuation DistanceAttenuation;
    float BulbRadius;
    float DistanceOffset;
};
TMap<TWeakObjectPtr<ULocalLightComponent>, FSCImportedFalloff> GSCImportedFalloffs;

// A/B of the engine fork's falloff models (WP-E1 E1-2): 0 puts every local light of the world on UE's range window and inverse
// square law, 1 restores what the importer set. Movable lights only (the importer's are).
void SetFalloffModels(UWorld* World, bool bOn)
{
    int32 Changed = 0;
    for (TObjectIterator<ULocalLightComponent> It; It; ++It)
    {
        ULocalLightComponent* L = *It;
        if (L->GetWorld() != World || !L->IsRegistered()) continue;
        if (!bOn)
        {
            if (GSCImportedFalloffs.Contains(L)) continue;
            GSCImportedFalloffs.Add(L, {L->RangeFade, L->RangeFadeFraction, L->DistanceAttenuation, L->AttenuationBulbRadius, L->AttenuationDistanceOffset});
            L->SetRangeFade(ELightRangeFade::Default, L->RangeFadeFraction);
            L->SetDistanceAttenuation(ELightDistanceAttenuation::Default, L->AttenuationBulbRadius, L->AttenuationDistanceOffset);
            ++Changed;
        }
        else if (const FSCImportedFalloff* Saved = GSCImportedFalloffs.Find(L))
        {
            L->SetRangeFade(Saved->RangeFade, Saved->RangeFadeFraction);
            L->SetDistanceAttenuation(Saved->DistanceAttenuation, Saved->BulbRadius, Saved->DistanceOffset);
            ++Changed;
        }
    }
    if (bOn) GSCImportedFalloffs.Reset();
    UE_LOG(LogSCImport, Display, TEXT("LIGHT_FALLOFF_MODELS %s lights=%d"), bOn ? TEXT("imported") : TEXT("ue_default"), Changed);
}

// The imported projector of one spot light, kept while sc.Lights.ProjectorShape 0 or 2 shows a UE cone in its place.
struct FSCImportedProjector
{
    ESpotLightShape Shape;
    ELightFunctionAddress Address;
    float InnerConeAngle;
    float OuterConeAngle;
    FVector LightFunctionScale;
};
TMap<TWeakObjectPtr<USpotLightComponent>, FSCImportedProjector> GSCImportedProjectors;

// A/B of the engine fork's spot light shape (WP-E1 E1-3) on every square frustum projector the importer made:
//  1  the import: SquareFrustum + Border (the game's projector).
//  0  UE's cone with UE's light function wrap at the same angles (OuterConeAngle = half-FOV: the square's inscribed circle).
//  2  the E1-2 importer's emulation on a UE cone: the cone through the square's corners (inner = outer), the light function
//     scaled back laterally to the half-FOV, UE's wrap (so the gobo repeats between the square and the cone).
void SetProjectorShape(UWorld* World, int32 Mode)
{
    int32 Changed = 0;
    for (TObjectIterator<USpotLightComponent> It; It; ++It)
    {
        USpotLightComponent* S = *It;
        if (S->GetWorld() != World || !S->IsRegistered()) continue;
        const FSCImportedProjector* Saved = GSCImportedProjectors.Find(S);
        if (!Saved)
        {
            if (Mode == 1 || S->Shape != ESpotLightShape::SquareFrustum) continue;
            Saved = &GSCImportedProjectors.Add(S, {S->Shape, S->LightFunctionAddress, S->InnerConeAngle, S->OuterConeAngle, S->LightFunctionScale});
        }
        S->SetShape(Mode == 1 ? Saved->Shape : ESpotLightShape::Cone);
        S->SetLightFunctionAddress(Mode == 1 ? Saved->Address : ELightFunctionAddress::Wrap);
        if (Mode == 2)
        {
            const double HalfFov = FMath::DegreesToRadians(double(Saved->OuterConeAngle));
            const float Corner = float(FMath::Min(FMath::RadiansToDegrees(FMath::Atan(UE_DOUBLE_SQRT_2 * FMath::Tan(HalfFov))), 88.9));
            S->SetOuterConeAngle(Corner);
            S->SetInnerConeAngle(Corner);
            const double Rendered = FMath::Tan(double(S->GetClampedConeAngles().Y));
            const double Axial = Saved->LightFunctionScale.Z, Lateral = Axial * FMath::Tan(HalfFov) / Rendered;
            S->SetLightFunctionScale(FVector(Lateral, Lateral, Axial));
        }
        else
        {
            S->SetOuterConeAngle(Saved->OuterConeAngle);
            S->SetInnerConeAngle(Saved->InnerConeAngle);
            S->SetLightFunctionScale(Saved->LightFunctionScale);
        }
        ++Changed;
    }
    if (Mode == 1) GSCImportedProjectors.Reset();
    static const TCHAR* Names[] = {TEXT("cone_wrap"), TEXT("square_border"), TEXT("e12_corner_cone_wrap")};
    UE_LOG(LogSCImport, Display, TEXT("LIGHT_PROJECTOR_SHAPE %s lights=%d"), Names[FMath::Clamp(Mode, 0, 2)], Changed);
}

FAutoConsoleCommandWithWorldAndArgs GSCLightsProjectorShape(
    TEXT("sc.Lights.ProjectorShape"),
    TEXT("sc.Lights.ProjectorShape <0|1|2>: 1 the imported SquareFrustum + Border projectors; 0 UE's cone + wrap at the same angles; 2 the E1-2 importer's corner cone + lateral light function scale + wrap"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        SetProjectorShape(World, Args.Num() == 0 ? 1 : FMath::Clamp(FCString::Atoi(*Args[0]), 0, 2));
    }));

FAutoConsoleCommandWithWorldAndArgs GSCLightsFalloffModels(
    TEXT("sc.Lights.FalloffModels"),
    TEXT("sc.Lights.FalloffModels <0|1>: 0 renders every local light with UE's range window and inverse square law, 1 restores the imported (game) falloff"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        SetFalloffModels(World, Args.Num() == 0 || FCString::Atoi(*Args[0]) != 0);
    }));
} // namespace
