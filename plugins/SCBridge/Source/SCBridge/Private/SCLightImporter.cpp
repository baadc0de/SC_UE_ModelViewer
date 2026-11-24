// Scene light and projector creation; photometric mapping is shared with the package contract.
#include "SCLightImporter.h"
#include "SCLightJson.h"
#include "EngineUtils.h"
#include "SCViewerSettings.h"
#include "SCStarEngine.h"
#include "SCVehicle.h"
#include "SCMeshes.h"
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
UTexture2D* FSCLightImporter::LoadGobo(const FSCPackageSource& Source, ASCVehicle* Vehicle, const FString& Path)
{
    if (!Vehicle || Path.IsEmpty()) return nullptr;
    if (UTexture2D* Cached = Vehicle->GoboTextures.FindRef(Path)) return Cached;
    UTexture2D* Texture = nullptr;
    if (FPaths::GetExtension(Path).Equals(TEXT("exr"), ESearchCase::IgnoreCase))
    {
        TArray<uint8> Bytes;
        FString Error;
        auto Wrapper = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper")).CreateImageWrapper(EImageFormat::EXR);
        const auto Limits = FSCTextureLimits::FromHost();
        if (Source.ReadBytes(Path, Bytes, Error) && Bytes.Num() <= Limits.MaxTextureBytes && Wrapper &&
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
        if (FSCTextureImporter::Decode(Source, {Path, ESCTextureRole::Linear}, Decoded)) Texture = FSCTextureImporter::Create(MoveTemp(Decoded), Vehicle);
    }
    if (!Texture) return nullptr;
    Texture->AddressX = TA_Clamp;
    Texture->AddressY = TA_Clamp;
    Texture->UpdateResource(); // sampler state must include clamp for PNG and EXR
    Vehicle->Textures.Add(Texture);
    Vehicle->GoboTextures.Add(Path, Texture);
    Vehicle->SourcePaths.Add(Texture, TEXT("gobo:") + Path);
    return Texture;
}
void FSCLightImporter::Build(const FSCPackageSource& Source, const FSCSceneInstance& Instance, ASCVehicle* Vehicle, USceneComponent* Parent,
                             TFunction<USceneComponent*(const FString&)> Helper, TArray<FString>& Warnings, bool bHelperFrame)
{
    auto Basis = [bHelperFrame](FVector V) { return FSCLights::ContainerBasis(V, bHelperFrame); };
    Vehicle->bMegaLightsActive = MegaLightsActive();
    const double ShadowRadius = FSCLights::ShadowRadiusThreshold(Instance.LightJson);
    int32 NonUnitSpecular = 0;
    ON_SCOPE_EXIT
    {
        if (NonUnitSpecular)
            UE_LOG(LogSCImport, Display, TEXT("LIGHT_SPECULAR instance=%s non_unit=%d of %d"), *Instance.EntityName, NonUnitSpecular,
                   Instance.LightJson.Num());
    };
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
        UClass* Class = FSCLights::ComponentClass(J);
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
        // The entity an attached light rides (a lift carriage re-parents it, WP-P5).
        if (const FString Rides = LightString(J, TEXT("parent_entity_id")); !Rides.IsEmpty())
            Light->ComponentTags.Add(FName(*(FSCLights::RidesTagPrefix + Rides)));
        FSCImportedLightState State;
        State.Component = Light;
        State.Name = Name;
        State.OwnerEntity = Instance.EntityName;
        State.Json = J;
        FSCLights::AuthorStates(State); // each authored state as ApplyState puts it on, saved with a prepared level
        State.AnimationSpeed = LightNumber(J, TEXT("animation_speed"), 1);
        State.AnimationPhase = FMath::Clamp(LightNumber(J, TEXT("animation_phase")), 0., 255.);
        bool bRandomPhase = false;
        if (J->TryGetBoolField(TEXT("random_animation_phase"), bRandomPhase) && bRandomPhase)
            State.AnimationPhase = FMath::RandRange(0, 255);
        if (const FLightJson* Attachment; J->TryGetObjectField(TEXT("attachment"), Attachment))
            State.GroupEntityId = LightString(*Attachment, TEXT("group_entity_id"));
        // Hull-attached lights can be emitted under the root or a child for placement.
        // Their group control still belongs to the source container, where ids are scoped.
        const FString SourceContainer = LightString(J, TEXT("source_container"));
        if (!SourceContainer.IsEmpty() && !State.GroupEntityId.IsEmpty())
        {
            const double SourceIndex = LightNumber(J, TEXT("source_container_index"), INDEX_NONE);
            const auto& Interiors = Vehicle->Manifest.Interiors;
            for (int32 I = 0; I < Interiors.Num(); ++I)
            {
                const FSCInterior& Interior = Interiors[I];
                if ((SourceIndex != INDEX_NONE && SourceIndex != Interior.SourceIndex) ||
                    Interior.Container.EntityName != SourceContainer ||
                    !Interior.Entities.ContainsByPredicate([&](const FSCInteriorEntity& Entity) {
                        return Entity.Kind == TEXT("light_group") && Entity.EntityId == State.GroupEntityId;
                    }))
                    continue;
                if (State.Interior != INDEX_NONE)
                {
                    Warnings.AddUnique(SourceContainer + TEXT(": ambiguous source container for light group ") + State.GroupEntityId);
                    State.Interior = INDEX_NONE;
                    break;
                }
                State.Interior = I;
            }
        }
        J->TryGetBoolField(TEXT("affects_this_area_only"), State.bAreaOnly);
        const FString ExportState = LightString(J, TEXT("active_state"));
        State.ExportActiveState = FName(*ExportState);
        State.ActiveState = ExportState.IsEmpty() ? FName(TEXT("defaultState")) : FName(*ExportState);
        FLightJson Selected = J;
        const FLightJson* States;
        if (J->TryGetObjectField(TEXT("states"), States))
        {
            FJsonSerializer::Serialize(States->ToSharedRef(), TJsonWriterFactory<>::Create(&State.StatesJson));
            if (Vehicle->Manifest.LightStyles.IsEmpty())
                for (const auto& [Key, Value] : (*States)->Values)
                    if (Value->Type == EJson::Object && LightNumber(Value->AsObject(), TEXT("light_style")) > 0)
                    {
                        Warnings.AddUnique(TEXT("Package has light style references without waveform definitions; using steady lights"));
                        break;
                    }
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
        State.BaseCandela = FSCLights::StateCandela(J, Selected);
        State.bEnabled = State.ActiveState != TEXT("offState") && State.BaseCandela > 0;
        const FVector Color = LightVector(Selected, TEXT("color"), LightVector(J, TEXT("color"), FVector(1)));
        State.BaseColor = FLinearColor(Color.X, Color.Y, Color.Z);
        State.LightStyleId = int32(FMath::Clamp(LightNumber(Selected, TEXT("light_style")), 0., double(MAX_int32)));
        // miscParams.specularMultiplier scales the light's specular in the game's shaders (0 = diffuse only); packages without
        // the field keep UE's 1.
        const double Specular = FMath::Max(0., LightNumber(J, TEXT("specular_multiplier"), 1.0));
        Light->SetSpecularScale(float(Specular));
        NonUnitSpecular += Specular != 1.0;
        if (auto* Local = Cast<ULocalLightComponent>(Light))
        {
            Local->SetIntensityUnits(ELightUnits::Candelas);
            const double Radius = FMath::Max(0., LightNumber(J, TEXT("radius_m")));
            Local->SetAttenuationRadius(Radius * 100);
            Local->SetCastShadows(State.bFallbackShadow || USCViewerSettings::Get()->bShadowAllLights); // look: every light (D15)
            // GetLightForcedDistanceFade: every light fades over the last forcedFalloffFraction of its radius (the fixture), in
            // place of UE's (1 - (d / R)^4)^2 (engine fork, WP-E1 E1-2).
            Local->SetRangeFade(ELightRangeFade::SquaredLinearTail, float(FSCStarEngine::Get().ForcedFadeFraction));
        }
        // Emitter size from the authored bulb / plane (CryEngine's attenuation bulb is the source radius).
        if (bAuthored)
        {
            if (auto* Point = Cast<UPointLightComponent>(Light)) // also spot lights
            {
                const double Bulb = FMath::Max(0., LightNumber(J, TEXT("bulb_radius_m")));
                Point->SetSourceRadius(Bulb * 100);
                // GetPhysicalLightAttenuation: candela / (max(d, bulb) + offset)^2 with the attenuation tweak's distance offset
                // (FSCStarEngine::AttenuationOffset; negative for tweak > 1), the engine fork's bulb offset law (WP-E1 E1-2).
                // A light without a bulb has no such law (b + c = 0) and keeps UE's inverse square.
                const double BulbCm = Bulb * 100;
                const double OffsetCm = FSCStarEngine::Get().AttenuationOffset(Bulb, LightNumber(J, TEXT("attenuation_tweak"), 1.0)) * 100;
                if (ULocalLightComponent::IsValidBulbOffsetAttenuation(float(BulbCm), float(OffsetCm)))
                    Point->SetDistanceAttenuation(ELightDistanceAttenuation::BulbOffset, float(BulbCm), float(OffsetCm));
            }
            else if (auto* Rect = Cast<URectLightComponent>(Light))
            {
                Rect->SetSourceWidth(FMath::Max(0., LightNumber(J, TEXT("plane_width_m"))) * 100);
                Rect->SetSourceHeight(FMath::Max(0., LightNumber(J, TEXT("plane_height_m"))) * 100);
            }
        }
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
            if (UMaterialInterface* Gobo = FSCLights::GoboLightFunction(Vehicle, LoadGobo(Source, Vehicle, TexturePath)))
            {
                Light->SetLightFunctionMaterial(Gobo);
                // The game's projector is the texture over the square FOV frustum, without a cone falloff (MakeProjector).
                // The exported inner angle is not a game value and is not used for projectors.
                if (auto* Spot = Cast<USpotLightComponent>(Light))
                    FSCLights::MakeProjector(Spot, LightNumber(J, TEXT("outer_angle")));
            }
            else
                Warnings.Add(Name + TEXT(": gobo unavailable: ") + TexturePath);
        }
        // All states stay available even when the default component imports disabled.
        const int32 LightIndex = Vehicle->LightStates.Add(MoveTemp(State));
        FSCLights::ApplyStyle(Vehicle, LightIndex);
        Light->RegisterComponent();
    }
}
