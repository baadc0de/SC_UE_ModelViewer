// FSCMaterialBuilder: master lookup and UMaterialInstanceDynamic creation from an FSCMaterialDesc.
#include "SCMaterialBuilder.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UObjectGlobals.h"

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
    default: return TEXT("SC_Fallback");
    }
}

FString FSCMaterialBuilder::ParentPath(ESCMaster Master, ESCBlend Blend, bool bTwoSided)
{
    // Translucent masters carry their own blend mode and sidedness; opaque families have constant variants.
    const bool bVariant = (Blend == ESCBlend::Masked || bTwoSided) && (Master == ESCMaster::Illum || Master == ESCMaster::HardSurface
        || Master == ESCMaster::Layered || Master == ESCMaster::Fallback);
    FString Name = bVariant ? FString(TEXT("MI_")) + MasterName(Master) : FString(TEXT("M_")) + MasterName(Master);
    if (bVariant && Blend == ESCBlend::Masked) Name += TEXT("_Masked");
    if (bVariant && bTwoSided) Name += TEXT("_TwoSided");
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
    for (const auto& Pair : Desc.Textures)
    {
        if (UTexture2D* const* Texture = Textures.Find(Pair.Value); Texture && *Texture) MID->SetTextureParameterValue(Pair.Key, *Texture);
        else Warnings.Add(FString::Printf(TEXT("%s: texture %s unavailable for %s; neutral default kept"), *Desc.Name, *Pair.Value.Path, *Pair.Key.ToString()));
    }
    return MID;
}
