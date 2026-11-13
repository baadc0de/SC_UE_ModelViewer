// Scene light and projector creation; photometric mapping is shared with the package contract.
#include "SCLightImporter.h"
#include "EngineUtils.h"
#include "SCViewerSettings.h"
#include "SCStarEngine.h"
#include "SCVehicle.h"
#include "SCMeshImporter.h"
#include "SCImporter.h"
#include "SCTextureImporter.h"
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
#include "RenderUtils.h"
#include "RHI.h"
#include "SceneManagement.h"
namespace
{
using FLightJson = TSharedPtr<FJsonObject>;
FString LightString(const FLightJson& J, const TCHAR* K)
{
    FString S;
    J->TryGetStringField(K, S);
    return S;
}
double LightNumber(const FLightJson& J, const TCHAR* K, double Default = 0)
{
    double V;
    return J->TryGetNumberField(K, V) && FMath::IsFinite(V) ? V : Default;
}
FVector LightVector(const FLightJson& J, const TCHAR* K, FVector Default)
{
    const TArray<TSharedPtr<FJsonValue>>* A;
    if (!J->TryGetArrayField(K, A) || A->Num() != 3) return Default;
    return FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber());
}
FQuat LightRotation(const FLightJson& J, const TCHAR* K)
{
    const TArray<TSharedPtr<FJsonValue>>* A;
    if (!J->TryGetArrayField(K, A) || A->Num() != 4) return FQuat::Identity;
    return FQuat((*A)[1]->AsNumber(), (*A)[2]->AsNumber(), (*A)[3]->AsNumber(), (*A)[0]->AsNumber()).GetNormalized();
}
FVector LightBasis(FVector V)
{
    return FVector(V.Y, V.X, V.Z);
}
// Shadows follow the game: only lights authored `shadowParams.shadowCasting` cast them (2-4 per ship), with or without
// MegaLights. WP-U3b shadowed every light under MegaLights as a stand-in for the game's per-room light confinement;
// WP-L1 confines lights to their rooms with lighting channels instead (SCLightAreas), so the stand-in is gone.
// Packages without the authored field keep the radius-quartile rule.
bool ConsoleEnabled(const TCHAR* Name)
{
    const auto* CVar = IConsoleManager::Get().FindConsoleVariable(Name);
    return CVar && CVar->GetInt() != 0;
}
bool MegaLightsActive()
{
    // Project-level Viewer policy; the Viewer PPV does not override MegaLights.
    // Match scalability, shader-platform and tracing gates, not just the project preference.
    if (GUsingNullRHI || !ConsoleEnabled(TEXT("r.MegaLights.EnableForProject")) || !ConsoleEnabled(TEXT("r.MegaLights.Allowed")) ||
        !ShouldCompileMegaLightsShaders(GMaxRHIShaderPlatform))
        return false;
    const bool Hardware =
        IsRayTracingEnabled() && ConsoleEnabled(TEXT("r.MegaLights.HardwareRayTracing")) &&
        (GRHISupportsRayTracingShaders || (GRHISupportsInlineRayTracing && ConsoleEnabled(TEXT("r.MegaLights.HardwareRayTracing.Inline"))));
    const bool Software = ConsoleEnabled(TEXT("r.MegaLights.SoftwareRayTracing.Allow")) && DoesProjectSupportDistanceFields();
    return Hardware || Software;
}
// The authored light model is present when the exporter wrote the WP-R5b fields.
bool HasAuthoredModel(const FLightJson& J)
{
    return J->HasField(TEXT("bulb_radius_m"));
}
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
UClass* FSCLightImporter::ComponentClass(const TSharedPtr<FJsonObject>& Light)
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
double FSCLightImporter::ShadowRadiusThreshold(const TArray<FString>& LightJson)
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
double FSCLightImporter::StateCandela(const TSharedPtr<FJsonObject>& Light, const TSharedPtr<FJsonObject>& State)
{
    if (!HasAuthoredModel(Light))
        return FMath::Max(0., LightNumber(State, TEXT("intensity_candela_proxy"), LightNumber(Light, TEXT("intensity_candela_proxy"))));
    // The game renderer's light model (FSCStarEngine, ue/verification/WP-U3b.md §13): the authored intensity is the
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
FVector FSCLightImporter::ContainerBasis(const FVector& Cry, bool bHelperFrame)
{
    return bHelperFrame ? FSCMeshImporter::Position(Cry) / 100. : LightBasis(Cry);
}

void FSCLightImporter::Build(const FSCPackageSource& Source, const FSCSceneInstance& Instance, ASCVehicle* Vehicle, USceneComponent* Parent,
                             TFunction<USceneComponent*(const FString&)> Helper, TArray<FString>& Warnings, bool bHelperFrame)
{
    auto Basis = [bHelperFrame](FVector V) { return ContainerBasis(V, bHelperFrame); };
    Vehicle->bMegaLightsActive = MegaLightsActive();
    const double ShadowRadius = ShadowRadiusThreshold(Instance.LightJson);
    for (const auto& Text : Instance.LightJson)
    {
        FLightJson J;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) || !J) continue;
        const FString Kind = LightString(J, TEXT("semantic_light_kind")), Name = LightString(J, TEXT("name"));
        if (Kind == TEXT("ambient_proxy"))
        {
            UE_LOG(LogSCImport, Display, TEXT("LIGHT_SKIPPED ambient_proxy %s"), *Name);
            continue;
        }
        if (LightString(J, TEXT("transform_basis")) != TEXT("cryengine_z_up"))
        {
            Warnings.Add(Name + TEXT(": unsupported light basis"));
            continue;
        }
        UClass* Class = ComponentClass(J);
        if (!Class)
        {
            Warnings.Add(Name + TEXT(": unsupported light kind ") + Kind);
            continue;
        }
        ULightComponent* Light = NewObject<ULightComponent>(Vehicle, Class, NAME_None, RF_Transient);
        Vehicle->AddInstanceComponent(Light);
        Vehicle->Lights.Add(Light);
        Light->SetMobility(EComponentMobility::Movable);
        Light->ComponentTags.Add(FName(*Name));
        FSCImportedLightState State;
        State.Component = Light;
        State.Name = Name;
        State.OwnerEntity = Instance.EntityName;
        J->TryGetBoolField(TEXT("affects_this_area_only"), State.bAreaOnly);
        const FString ExportState = LightString(J, TEXT("active_state"));
        State.ExportActiveState = FName(*ExportState);
        State.ActiveState = ExportState.IsEmpty() ? FName(TEXT("defaultState")) : FName(*ExportState);
        FLightJson Selected = J;
        const FLightJson* States;
        if (J->TryGetObjectField(TEXT("states"), States))
        {
            FJsonSerializer::Serialize(States->ToSharedRef(), TJsonWriterFactory<>::Create(&State.StatesJson));
            const FLightJson* Authored;
            // The exporter falls through zero defaults to auxiliary/emergency. The viewer starts
            // in the authored default instead, while an explicit offState always stays off.
            if (State.ActiveState != TEXT("offState") && (*States)->TryGetObjectField(TEXT("defaultState"), Authored))
            {
                State.ActiveState = TEXT("defaultState");
                Selected = *Authored;
            }
            else if ((*States)->TryGetObjectField(State.ActiveState.ToString(), Authored))
                Selected = *Authored;
        }
        State.RadiusM = FMath::Max(0., LightNumber(J, TEXT("radius_m")));
        State.ShadowThresholdM = ShadowRadius;
        // The authored shadowParams.shadowCasting flag when exported, else the radius quartile.
        const bool bAuthored = HasAuthoredModel(J);
        bool bAuthoredShadow = false;
        State.bFallbackShadow = !Cast<ULocalLightComponent>(Light) ||
                                (bAuthored && J->TryGetBoolField(TEXT("shadow_casting"), bAuthoredShadow) ? bAuthoredShadow
                                                                                                          : State.RadiusM >= ShadowRadius);
        const double Intensity = StateCandela(J, Selected);
        State.bEnabled = State.ActiveState != TEXT("offState") && Intensity > 0;
        Light->SetVisibility(State.bEnabled);
        const FVector Color = LightVector(Selected, TEXT("color"), LightVector(J, TEXT("color"), FVector(1)));
        Light->SetLightColor(FLinearColor(Color.X, Color.Y, Color.Z));
        if (auto* Local = Cast<ULocalLightComponent>(Light))
        {
            Local->SetIntensityUnits(ELightUnits::Candelas);
            const double Radius = FMath::Max(0., LightNumber(J, TEXT("radius_m")));
            Local->SetAttenuationRadius(Radius * 100);
            Local->SetCastShadows(State.bFallbackShadow || USCViewerSettings::Get()->bShadowAllLights); // look: every light (D15)
        }
        // Emitter size from the authored bulb / plane (CryEngine's attenuation bulb is the source radius).
        if (bAuthored)
        {
            if (auto* Point = Cast<UPointLightComponent>(Light)) // also spot lights
                Point->SetSourceRadius(FMath::Max(0., LightNumber(J, TEXT("bulb_radius_m"))) * 100);
            else if (auto* Rect = Cast<URectLightComponent>(Light))
            {
                Rect->SetSourceWidth(FMath::Max(0., LightNumber(J, TEXT("plane_width_m"))) * 100);
                Rect->SetSourceHeight(FMath::Max(0., LightNumber(J, TEXT("plane_height_m"))) * 100);
            }
        }
        Light->SetIntensity(State.bEnabled ? Intensity : 0.);
        if (auto* Spot = Cast<USpotLightComponent>(Light))
        {
            Spot->SetOuterConeAngle(LightNumber(J, TEXT("outer_angle")));
            Spot->SetInnerConeAngle(LightNumber(J, TEXT("inner_angle")));
        }
        FVector P = LightVector(J, TEXT("position"), FVector::ZeroVector);
        FVector Direction = Basis(LightVector(J, TEXT("direction_sc"), FVector(1, 0, 0))).GetSafeNormal();
        const FVector Up = Basis(LightRotation(J, TEXT("rotation")).RotateVector(FVector::UpVector));
        FQuat Orientation = FRotationMatrix::MakeFromXZ(Direction, Up).ToQuat();
        USceneComponent* Attach = Parent;
        const FString ParentName = LightString(J, TEXT("parent_node_name"));
        if (!ParentName.IsEmpty())
            if (auto* H = Helper(ParentName))
            {
                Attach = H;
                // Convert the already-correct entity-local position/orientation into the helper frame.
                // This also preserves the authored direction_sc and never applies both placements.
                const FTransform EntityLocal(Orientation, Basis(P) * 100);
                const FTransform Relative =
                    (EntityLocal * Parent->GetComponentTransform()).GetRelativeTransform(H->GetComponentTransform());
                Light->SetupAttachment(H);
                Light->SetRelativeTransform(Relative);
            }
        if (Attach == Parent)
        {
            Light->SetupAttachment(Parent);
            Light->SetRelativeTransform(FTransform(Orientation, Basis(P) * 100));
        }
        const FString TexturePath = LightString(J, TEXT("projector_texture"));
        if (!TexturePath.IsEmpty())
        {
            auto* Master = LoadObject<UMaterialInterface>(nullptr, TEXT("/SCBridge/Materials/M_SC_LightFunction.M_SC_LightFunction"));
            UTexture2D* Texture = Vehicle->GoboTextures.FindRef(TexturePath);
            if (Master && !Texture)
            {
                if (FPaths::GetExtension(TexturePath).Equals(TEXT("exr"), ESearchCase::IgnoreCase))
                {
                    TArray<uint8> Bytes;
                    FString Error;
                    auto Wrapper =
                        FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper")).CreateImageWrapper(EImageFormat::EXR);
                    const auto Limits = FSCTextureLimits::FromHost();
                    if (Source.ReadBytes(TexturePath, Bytes, Error) && Bytes.Num() <= Limits.MaxTextureBytes && Wrapper &&
                        Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()) && Wrapper->GetWidth() > 0 && Wrapper->GetHeight() > 0 &&
                        Wrapper->GetWidth() <= Limits.MaxDimension && Wrapper->GetHeight() <= Limits.MaxDimension &&
                        int64(Wrapper->GetWidth()) * Wrapper->GetHeight() * 16 <= Limits.MaxTextureBytes)
                    {
                        Texture = FImageUtils::ImportBufferAsTexture2D(Bytes); // preserves HDR platform pixels, no editor factory
                        if (Texture)
                        {
                            Texture->Rename(nullptr, Vehicle, REN_DontCreateRedirectors | REN_NonTransactional);
                            Texture->SRGB = false;
                        }
                    }
                }
                else
                {
                    FSCDecodedTexture Decoded;
                    if (FSCTextureImporter::Decode(Source, {TexturePath, ESCTextureRole::Linear}, Decoded))
                        Texture = FSCTextureImporter::Create(MoveTemp(Decoded), Vehicle);
                }
                if (Texture)
                {
                    Texture->AddressX = TA_Clamp;
                    Texture->AddressY = TA_Clamp;
                    Texture->UpdateResource(); // sampler state must include clamp for PNG and EXR
                    Vehicle->Textures.Add(Texture);
                    Vehicle->GoboTextures.Add(TexturePath, Texture);
                    Vehicle->SourcePaths.Add(Texture, TEXT("gobo:") + TexturePath);
                }
            }
            if (Master && Texture)
            {
                Light->SetLightFunctionMaterial(GoboMaterial(Vehicle, Master, Texture));
            }
            else
                Warnings.Add(Name + TEXT(": gobo unavailable: ") + TexturePath);
        }
        // TODO(WP-V2): select/reapply named states and style waveforms through the light panel.
        // All states stay available even when the default component imports disabled.
        Vehicle->LightStates.Add(MoveTemp(State));
        Light->RegisterComponent();
    }
}

void FSCLightImporter::RefreshShadows(ASCVehicle* Vehicle)
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
        for (TActorIterator<ASCVehicle> It(World); It; ++It, ++Vehicles) FSCLightImporter::RefreshShadows(*It);
        UE_LOG(LogTemp, Display, TEXT("LOOK shadow_all=%d vehicles=%d"), USCViewerSettings::Get()->bShadowAllLights, Vehicles);
    }));
