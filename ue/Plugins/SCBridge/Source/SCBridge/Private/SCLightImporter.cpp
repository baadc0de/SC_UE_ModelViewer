#include "SCLightImporter.h"
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
namespace
{
using FLightJson = TSharedPtr<FJsonObject>;
FString LightString(const FLightJson& J, const TCHAR* K) { FString S; J->TryGetStringField(K,S); return S; }
double LightNumber(const FLightJson& J, const TCHAR* K, double Default = 0) { double V; return J->TryGetNumberField(K,V) && FMath::IsFinite(V) ? V : Default; }
FVector LightVector(const FLightJson& J, const TCHAR* K, FVector Default)
{
    const TArray<TSharedPtr<FJsonValue>>* A;
    if (!J->TryGetArrayField(K,A) || A->Num()!=3) return Default;
    return FVector((*A)[0]->AsNumber(),(*A)[1]->AsNumber(),(*A)[2]->AsNumber());
}
FQuat LightRotation(const FLightJson& J, const TCHAR* K)
{
    const TArray<TSharedPtr<FJsonValue>>* A;
    if (!J->TryGetArrayField(K,A) || A->Num()!=4) return FQuat::Identity;
    return FQuat((*A)[1]->AsNumber(),(*A)[2]->AsNumber(),(*A)[3]->AsNumber(),(*A)[0]->AsNumber()).GetNormalized();
}
FVector LightBasis(FVector V) { return FVector(V.Y,V.X,V.Z); }
}
void FSCLightImporter::Build(const FSCPackageSource& Source, const FSCSceneInstance& Instance, ASCVehicle* Vehicle,
    USceneComponent* Parent, TFunction<USceneComponent*(const FString&)> Helper, TArray<FString>& Warnings, bool bHelperFrame)
{
    auto Basis = [bHelperFrame](FVector V) { return bHelperFrame ? FSCMeshImporter::Position(V)/100. : LightBasis(V); };
    for (const auto& Text : Instance.LightJson)
    {
        FLightJson J; if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) || !J) continue;
        const FString Kind = LightString(J,TEXT("semantic_light_kind")), Name = LightString(J,TEXT("name"));
        if (Kind == TEXT("ambient_proxy")) { UE_LOG(LogSCImport, Display, TEXT("LIGHT_SKIPPED ambient_proxy %s"), *Name); continue; }
        if (LightString(J,TEXT("transform_basis")) != TEXT("cryengine_z_up")) { Warnings.Add(Name + TEXT(": unsupported light basis")); continue; }
        ULightComponent* Light = nullptr;
        if (Kind == TEXT("point")) Light = NewObject<UPointLightComponent>(Vehicle, NAME_None, RF_Transient);
        else if (Kind == TEXT("spot")) Light = NewObject<USpotLightComponent>(Vehicle, NAME_None, RF_Transient);
        else if (Kind == TEXT("area")) Light = NewObject<URectLightComponent>(Vehicle, NAME_None, RF_Transient);
        else if (Kind == TEXT("sun")) Light = NewObject<UDirectionalLightComponent>(Vehicle, NAME_None, RF_Transient);
        if (!Light) { Warnings.Add(Name + TEXT(": unsupported light kind ") + Kind); continue; }
        Vehicle->AddInstanceComponent(Light); Vehicle->Lights.Add(Light); Light->SetMobility(EComponentMobility::Movable);
        Light->ComponentTags.Add(FName(*Name));
        const FVector Color = LightVector(J,TEXT("color"),FVector(1));
        Light->SetLightColor(FLinearColor(Color.X,Color.Y,Color.Z));
        if (auto* Local = Cast<ULocalLightComponent>(Light))
        {
            Local->SetIntensityUnits(ELightUnits::Candelas);
            Local->SetAttenuationRadius(FMath::Max(0.,LightNumber(J,TEXT("radius_m"))) * 100);
        }
        Light->SetIntensity(FMath::Max(0.,LightNumber(J,TEXT("intensity_candela_proxy"))));
        if (auto* Spot = Cast<USpotLightComponent>(Light))
        {
            Spot->SetOuterConeAngle(LightNumber(J,TEXT("outer_angle")));
            Spot->SetInnerConeAngle(LightNumber(J,TEXT("inner_angle")));
        }
        FVector P = LightVector(J,TEXT("position"),FVector::ZeroVector);
        FVector Direction = Basis(LightVector(J,TEXT("direction_sc"),FVector(1,0,0))).GetSafeNormal();
        const FVector Up = Basis(LightRotation(J,TEXT("rotation")).RotateVector(FVector::UpVector));
        FQuat Orientation = FRotationMatrix::MakeFromXZ(Direction, Up).ToQuat();
        USceneComponent* Attach = Parent;
        const FString ParentName = LightString(J,TEXT("parent_node_name"));
        if (!ParentName.IsEmpty()) if (auto* H = Helper(ParentName))
        {
            Attach = H;
            // Convert the already-correct entity-local position/orientation into the helper frame.
            // This also preserves the authored direction_sc and never applies both placements.
            const FTransform EntityLocal(Orientation, Basis(P)*100);
            const FTransform Relative = (EntityLocal * Parent->GetComponentTransform()).GetRelativeTransform(H->GetComponentTransform());
            Light->SetupAttachment(H); Light->SetRelativeTransform(Relative);
        }
        if (Attach == Parent)
        { Light->SetupAttachment(Parent); Light->SetRelativeTransform(FTransform(Orientation, Basis(P)*100)); }
        const FString TexturePath = LightString(J,TEXT("projector_texture"));
        if (!TexturePath.IsEmpty())
        {
            auto* Master = LoadObject<UMaterialInterface>(nullptr, TEXT("/SCBridge/Materials/M_SC_LightFunction.M_SC_LightFunction"));
            UTexture2D* Texture = Vehicle->GoboTextures.FindRef(TexturePath);
            if (Master && !Texture)
            {
                if (FPaths::GetExtension(TexturePath).Equals(TEXT("exr"), ESearchCase::IgnoreCase))
                {
                    TArray<uint8> Bytes; FString Error;
                    auto Wrapper = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper")).CreateImageWrapper(EImageFormat::EXR);
                    const auto Limits = FSCTextureLimits::FromHost();
                    if (Source.ReadBytes(TexturePath,Bytes,Error) && Bytes.Num() <= Limits.MaxTextureBytes && Wrapper
                        && Wrapper->SetCompressed(Bytes.GetData(),Bytes.Num()) && Wrapper->GetWidth()>0 && Wrapper->GetHeight()>0
                        && Wrapper->GetWidth()<=Limits.MaxDimension && Wrapper->GetHeight()<=Limits.MaxDimension
                        && int64(Wrapper->GetWidth())*Wrapper->GetHeight()*16 <= Limits.MaxTextureBytes)
                    {
                        Texture = FImageUtils::ImportBufferAsTexture2D(Bytes); // preserves HDR platform pixels, no editor factory
                        if (Texture) { Texture->Rename(nullptr,Vehicle,REN_DontCreateRedirectors|REN_NonTransactional); Texture->SRGB=false; Texture->UpdateResource(); }
                    }
                }
                else
                {
                    FSCDecodedTexture Decoded;
                    if (FSCTextureImporter::Decode(Source,{TexturePath,ESCTextureRole::Linear},Decoded))
                        Texture = FSCTextureImporter::Create(MoveTemp(Decoded),Vehicle);
                }
                if (Texture)
                {
                    Texture->AddressX=Texture->AddressY=TA_Clamp;
                    Vehicle->Textures.Add(Texture); Vehicle->GoboTextures.Add(TexturePath,Texture);
                }
            }
            if (Master && Texture)
            {
                auto* MID = UMaterialInstanceDynamic::Create(Master,Vehicle);
                MID->SetTextureParameterValue(TEXT("Gobo"),Texture); Vehicle->Materials.Add(MID); Light->SetLightFunctionMaterial(MID);
            }
            else Warnings.Add(Name + TEXT(": gobo unavailable: ") + TexturePath);
        }
        // State/style metadata stays lossless on Manifest. Style waveform definitions are not in this contract.
        Light->RegisterComponent();
    }
}
