// WP-O1-4c-3a: the runtime half of the lights (SCLights.h), split from SCLightImporter.cpp.
#include "SCLights.h"
#include "SCLightJson.h"
#include "EngineUtils.h"
#include "SCViewerSettings.h"
#include "SCStarEngine.h"
#include "SCVehicle.h"
#include "SCMeshImporter.h"
#include "SCImporter.h"
#include "SCTextureImporter.h"
#include "SCPlayerKit.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture2D.h"
#include "ImageUtils.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"
#include "RenderUtils.h"
#include "RHI.h"
#include "SceneManagement.h"
using namespace SCLightJson;
namespace
{
// One light-function MID per gobo texture: the light function atlas (deferred lighting, MegaLights, Lumen)
// de-duplicates slots by material and holds r.LightFunctionAtlas.Size^2 materials; a MID per light exhausts it.
UMaterialInstanceDynamic* GoboMaterial(ASCVehicle* Vehicle, UMaterialInterface* Master, UTexture2D* Texture)
{
    for (UMaterialInstanceDynamic* Existing : Vehicle->Materials)
    {
        UTexture* Bound = nullptr;
        if (Existing && Existing->Parent == Master &&
            Existing->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Gobo")), Bound, true) && Bound == Texture)
            return Existing;
    }
    auto* MID = UMaterialInstanceDynamic::Create(Master, Vehicle);
    MID->SetTextureParameterValue(TEXT("Gobo"), Texture);
    Vehicle->Materials.Add(MID);
    return MID;
}
} // namespace
UClass* FSCLights::ComponentClass(const TSharedPtr<FJsonObject>& Light)
{
    if (LightString(Light, TEXT("transform_basis")) != TEXT("cryengine_z_up")) return nullptr;
    const FString Kind = LightString(Light, TEXT("semantic_light_kind"));
    if (Kind == TEXT("point")) return UPointLightComponent::StaticClass();
    if (Kind == TEXT("spot")) return USpotLightComponent::StaticClass();
    if (Kind == TEXT("area")) return URectLightComponent::StaticClass();
    if (Kind == TEXT("sun")) return UDirectionalLightComponent::StaticClass();
    return nullptr; // the ambient proxy and kinds without a UE light
}
// Fallback rule: within one scene instance (root entity or interior container) only the local lights whose authored
// attenuation radius is in the upper quartile cast shadows; small-radius fixtures only light their housing.
double FSCLights::ShadowRadiusThreshold(const TArray<FString>& LightJson)
{
    TArray<double> Radii;
    for (const auto& Text : LightJson)
    {
        FLightJson J;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) || !J) continue;
        const UClass* Class = ComponentClass(J);
        if (Class && Class->IsChildOf(ULocalLightComponent::StaticClass())) Radii.Add(FMath::Max(0., LightNumber(J, TEXT("radius_m"))));
    }
    if (Radii.IsEmpty()) return 0;
    Radii.Sort();
    return Radii[(3 * Radii.Num()) / 4];
}
double FSCLights::StateCandela(const TSharedPtr<FJsonObject>& Light, const TSharedPtr<FJsonObject>& State)
{
    if (!HasAuthoredModel(Light))
        return FMath::Max(0., LightNumber(State, TEXT("intensity_candela_proxy"), LightNumber(Light, TEXT("intensity_candela_proxy"))));
    // The game renderer's light model (FSCStarEngine, viewer/verification/WP-U3b.md §13): the authored intensity is the
    // brightness (scene units, Lambert pi folded in) reached 1 m from the bulb surface, falling off as
    // GetPhysicalLightAttenuation; planar lights carry it as the plane's luminance.
    const FSCStarEngine& Game = FSCStarEngine::Get();
    const double Raw = FMath::Max(0., LightNumber(State, TEXT("intensity_raw"), LightNumber(Light, TEXT("intensity_raw"))));
    if (LightString(Light, TEXT("semantic_light_kind")) == TEXT("area"))
        return Game.PlanarCandela(Raw, FMath::Max(0., LightNumber(Light, TEXT("plane_width_m"))),
                                  FMath::Max(0., LightNumber(Light, TEXT("plane_height_m"))));
    return Game.PointCandela(Raw, FMath::Max(0., LightNumber(Light, TEXT("bulb_radius_m"))),
                             LightNumber(Light, TEXT("attenuation_tweak"), 1.0));
}
void FSCLights::MakeProjector(USpotLightComponent* Spot, double HalfFovDeg)
{
    Spot->SetShape(ESpotLightShape::SquareFrustum);
    Spot->SetOuterConeAngle(float(HalfFovDeg));
    Spot->SetInnerConeAngle(float(HalfFovDeg)); // unused by the square frustum; no cone falloff in the game either
    Spot->SetLightFunctionAddress(ELightFunctionAddress::Border);
}
UMaterialInterface* FSCLights::GoboLightFunction(ASCVehicle* Vehicle, UTexture2D* Texture)
{
    auto* Master = LoadObject<UMaterialInterface>(nullptr, TEXT("/SCBridge/Materials/M_SC_LightFunction.M_SC_LightFunction"));
    return Vehicle && Texture && Master ? GoboMaterial(Vehicle, Master, Texture) : nullptr;
}
FSCLights::FFlashlightSettings FSCLights::FlashlightSettings(const FSCFlashlight& Light)
{
    // SCItemFlashlightParams carries no attenuation tweak: CryEngine's neutral 1 (the importer's default for lights without one).
    constexpr double NoTweak = 1.0;
    const FSCStarEngine& Game = FSCStarEngine::Get();
    const double Bulb = FMath::Max(0.0, double(Light.BulbRadiusM));
    FFlashlightSettings S;
    S.Candela = Game.PointCandela(FMath::Max(0.0, double(Light.DiffuseMult)), Bulb, NoTweak);
    S.RadiusCm = FMath::Max(0.0, double(Light.DistanceM)) * 100;
    S.BulbCm = Bulb * 100;
    S.OffsetCm = Game.AttenuationOffset(Bulb, NoTweak) * 100;
    S.bBulbOffset = ULocalLightComponent::IsValidBulbOffsetAttenuation(float(S.BulbCm), float(S.OffsetCm));
    S.HalfFovDeg = FMath::Max(0.0, double(Light.FovDeg)) / 2;
    S.RangeFadeFraction = Game.ForcedFadeFraction;
    S.Color = FLinearColor::FromSRGBColor(Light.Color); // SRGB8: an sRGB-encoded colour (as the exporter decodes palette SRGB8)
    return S;
}
void FSCLights::ApplyFlashlight(USpotLightComponent* Spot, const FSCFlashlight& Light, UMaterialInterface* LightFunction)
{
    const FFlashlightSettings S = FlashlightSettings(Light);
    Spot->SetIntensityUnits(ELightUnits::Candelas);
    Spot->SetIntensity(float(S.Candela));
    Spot->SetLightColor(S.Color);
    Spot->SetAttenuationRadius(float(S.RadiusCm));
    Spot->SetSourceRadius(float(S.BulbCm));
    Spot->SetDistanceAttenuation(S.bBulbOffset ? ELightDistanceAttenuation::BulbOffset : ELightDistanceAttenuation::Default,
                                 S.bBulbOffset ? float(S.BulbCm) : 0.f, S.bBulbOffset ? float(S.OffsetCm) : 0.f);
    Spot->SetRangeFade(ELightRangeFade::SquaredLinearTail, float(S.RangeFadeFraction));
    MakeProjector(Spot, S.HalfFovDeg);
    Spot->SetLightFunctionMaterial(LightFunction);
}

FVector FSCLights::ContainerBasis(const FVector& Cry, bool bHelperFrame)
{
    return bHelperFrame ? FSCMeshImporter::Position(Cry) / 100. : LightBasis(Cry);
}

void FSCLights::AuthorStates(FSCImportedLightState& State)
{
    State.AuthoredStates.Reset();
    const TSharedPtr<FJsonObject>& J = State.Json;
    const FLightJson* States = nullptr;
    if (!J || !J->TryGetObjectField(TEXT("states"), States)) return;
    for (const auto& [Key, Value] : (*States)->Values)
        if (const TSharedPtr<FJsonObject> Selected = Value.IsValid() ? Value->AsObject() : nullptr)
        {
            FSCLightStateValues& V = State.AuthoredStates.Add(FName(*Key));
            V.Candela = StateCandela(J, Selected);
            const FVector Color = LightVector(Selected, TEXT("color"), LightVector(J, TEXT("color"), FVector(1)));
            V.Color = FLinearColor(Color.X, Color.Y, Color.Z);
            V.LightStyleId = int32(FMath::Clamp(LightNumber(Selected, TEXT("light_style")), 0., double(MAX_int32)));
        }
}
bool FSCLights::ApplyState(ASCVehicle* Vehicle, int32 Index, FName StateKey)
{
    // Another of the light's authored states (`states.<key>`): its intensity by the same model as at import (StateCandela)
    // and its colour and style. A state the light does not author leaves it as it is.
    if (!Vehicle || !Vehicle->LightStates.IsValidIndex(Index)) return false;
    FSCImportedLightState& S = Vehicle->LightStates[Index];
    ULightComponent* Light = S.Component;
    const FSCLightStateValues* Selected = S.AuthoredStates.Find(StateKey); // computed at import (saved with a prepared level)
    if (!Light || !Selected) return false;
    S.BaseCandela = Selected->Candela;
    S.ActiveState = StateKey;
    S.bEnabled = StateKey != FName(TEXT("offState")) && S.BaseCandela > 0;
    S.BaseColor = Selected->Color;
    S.LightStyleId = Selected->LightStyleId;
    ApplyStyle(Vehicle, Index);
    return true;
}
void FSCLights::RefreshShadows(ASCVehicle* Vehicle)
{
    Vehicle->bMegaLightsActive = MegaLightsActive();
    for (const auto& State : Vehicle->LightStates)
        if (auto* Local = Cast<ULocalLightComponent>(State.Component))
            Local->SetCastShadows(State.bFallbackShadow || USCViewerSettings::Get()->bShadowAllLights);
}

static FAutoConsoleCommandWithWorldAndArgs GSCLookShadowAll(
    TEXT("sc.Look.ShadowAll"), TEXT("sc.Look.ShadowAll <0|1>: every ship light casts shadows (look) or only the authored casters"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCViewerSettings::Get()->bShadowAllLights = !Args.IsValidIndex(0) || FCString::Atoi(*Args[0]) != 0;
        int32 Vehicles = 0;
        for (TActorIterator<ASCVehicle> It(World); It; ++It, ++Vehicles) FSCLights::RefreshShadows(*It);
        UE_LOG(LogTemp, Display, TEXT("LOOK shadow_all=%d vehicles=%d"), USCViewerSettings::Get()->bShadowAllLights, Vehicles);
    }));
