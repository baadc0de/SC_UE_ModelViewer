// FSCMaterialBuilder: master lookup and UMaterialInstanceDynamic creation from an FSCMaterialDesc.
#include "SCMaterialBuilder.h"
#include "SCViewerSettings.h"
#include "SCLook.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UObjectGlobals.h"

const FName FSCMaterialBuilder::GlassReflectanceScaleParam(TEXT("GlassReflectanceScale"));
const FName FSCMaterialBuilder::GlassReflectanceFloorParam(TEXT("GlassReflectanceFloor"));
const FName FSCMaterialBuilder::GlassRefractionLookParam(TEXT("GlassRefractionLook"));
const FName FSCMaterialBuilder::ScreenImageParam(TEXT("ScreenImage"));

bool FSCMaterialBuilder::IsGlassMaster(const UMaterialInterface* Material)
{
    const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
    return Base && Base->GetPathName() == ParentPath(ESCMaster::Glass, ESCBlend::Translucent, false);
}

bool FSCMaterialBuilder::IsScreenMaster(const UMaterialInterface* Material)
{
    const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
    if (!Base) return false;
    const FString Path = Base->GetPathName();
    return Path == ParentPath(ESCMaster::Screen, ESCBlend::Opaque, false) || Path == ParentPath(ESCMaster::Screen, ESCBlend::Translucent, false);
}

void FSCMaterialBuilder::ApplyGlassLook(UMaterialInstanceDynamic* MID)
{
    const USCViewerSettings* Settings = USCViewerSettings::Get();
    MID->SetScalarParameterValue(GlassReflectanceScaleParam, Settings->GlassReflectanceScale);
    MID->SetScalarParameterValue(GlassReflectanceFloorParam, Settings->GlassReflectanceFloor);
    if (IsGlassMaster(MID)) MID->SetScalarParameterValue(GlassRefractionLookParam, Settings->bGlassRefraction ? 1.f : 0.f);
}

const TCHAR* FSCMaterialBuilder::MasterName(ESCMaster Master)
{
    switch (Master)
    {
    case ESCMaster::Illum: return TEXT("SC_Illum");
    case ESCMaster::HardSurface: return TEXT("SC_HardSurface");
    case ESCMaster::Layered: return TEXT("SC_Layered");
    case ESCMaster::Glass: return TEXT("SC_Glass");
    case ESCMaster::MeshDecal: return TEXT("SC_MeshDecal");
    case ESCMaster::MeshDecalRelief: return TEXT("SC_MeshDecal_Relief");
    case ESCMaster::Translucent: return TEXT("SC_Translucent");
    case ESCMaster::EmissiveUnlit: return TEXT("SC_Emissive_Unlit");
    case ESCMaster::Screen: return TEXT("SC_Screen");
    default: return TEXT("SC_Fallback");
    }
}

void FSCMaterialBuilder::ApplyTextureMeans(FSCMaterialDesc& Desc, const TMap<FSCTextureKey, FLinearColor>& Means)
{
    for (const auto& Pair : Desc.VectorMeans)
    {
        FLinearColor* Value = Desc.Vectors.Find(Pair.Key);
        const FLinearColor* Mean = Means.Find(Pair.Value);
        if (Value && Mean) { Value->R *= Mean->R; Value->G *= Mean->G; Value->B *= Mean->B; }
        else Desc.Warnings.Add(FString::Printf(TEXT("%s: the mean colour of %s is not available; the value is not scaled by it"),
            *Pair.Key.ToString(), *Pair.Value.Path));
    }
    Desc.VectorMeans.Reset();
}

FString FSCMaterialBuilder::ParentPath(ESCMaster Master, ESCBlend Blend, bool bTwoSided)
{
    // Opaque families have masked / two-sided constant variants; decal and lit-translucent masters have a
    // two-sided variant (is_double_sided); glass and screens are single-sided by their data. A screen with authored
    // partial opacity has its own translucent master (WP-I1 review).
    if (Master == ESCMaster::Screen && Blend == ESCBlend::Translucent) return TEXT("/SCBridge/Materials/M_SC_Screen_Translucent.M_SC_Screen_Translucent");
    const bool bOpaque = Master == ESCMaster::Illum || Master == ESCMaster::HardSurface || Master == ESCMaster::Layered || Master == ESCMaster::Fallback;
    const bool bSided = Master == ESCMaster::MeshDecal || Master == ESCMaster::MeshDecalRelief || Master == ESCMaster::Translucent;
    const bool bMasked = bOpaque && Blend == ESCBlend::Masked, bTwo = (bOpaque || bSided) && bTwoSided;
    FString Name = (bMasked || bTwo ? FString(TEXT("MI_")) : FString(TEXT("M_"))) + MasterName(Master);
    if (bMasked) Name += TEXT("_Masked");
    if (bTwo) Name += TEXT("_TwoSided");
    return FString::Printf(TEXT("/SCBridge/Materials/%s.%s"), *Name, *Name);
}

UMaterialInstanceDynamic* FSCMaterialBuilder::Create(const FSCMaterialDesc& Desc, const TMap<FSCTextureKey, UTexture2D*>& Textures,
    UObject* Outer, TArray<FString>& Warnings)
{
    check(IsInGameThread());
    const FString Path = ParentPath(Desc.Master, Desc.Blend, Desc.bTwoSided);
    UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *Path);
    if (!Parent) { Warnings.Add(TEXT("Missing master material ") + Path); return nullptr; }
    FString Name = TEXT("MID_") + Desc.Name;
    for (TCHAR& C : Name) if (!FChar::IsAlnum(C) && C != TEXT('_')) C = TEXT('_');
    UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Parent, Outer, MakeUniqueObjectName(Outer, UMaterialInstanceDynamic::StaticClass(), FName(*Name)));
    MID->SetFlags(RF_Transient);
    for (const auto& Pair : Desc.Scalars) MID->SetScalarParameterValue(Pair.Key, Pair.Value);
    for (const auto& Pair : Desc.Vectors) MID->SetVectorParameterValue(Pair.Key, Pair.Value);
    if (Desc.Master == ESCMaster::Glass || Desc.Master == ESCMaster::Screen) ApplyGlassLook(MID); // a screen's face is glass too (no refraction)
    // The refraction profile from the instance's own data (canopy or default; SCLook.h), so glass built outside a viewer load
    // (sc.Import, the editor's folder import) refracts with it too. A player kit's glass becomes Kit when the kit attaches
    // (SCLook::RefreshGlassRefraction).
    if (IsGlassMaster(MID)) SCLook::ApplyGlassProfile(MID, SCLook::SelectGlassProfile(MID, false));
    for (const auto& Pair : Desc.Textures)
    {
        if (UTexture2D* const* Texture = Textures.Find(Pair.Value); Texture && *Texture) MID->SetTextureParameterValue(Pair.Key, *Texture);
        else Warnings.Add(FString::Printf(TEXT("%s: texture %s unavailable for %s; neutral default kept"), *Desc.Name, *Pair.Value.Path, *Pair.Key.ToString()));
    }
    return MID;
}
