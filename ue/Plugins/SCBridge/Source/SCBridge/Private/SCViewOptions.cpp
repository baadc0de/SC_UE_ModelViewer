// USCViewOptions: core defaults, FFI mapping, hashing.
#include "SCViewOptions.h"
#include "SCBridgeClient.h"
#include "Misc/SecureHash.h"

USCViewOptions::USCViewOptions()
{
    ResetToDefaults(); // Config values, when saved, are applied to the CDO after construction and win.
}
void USCViewOptions::ResetToDefaults()
{
    const sc_options D = FSCBridgeClient::DefaultOptions();
    Lod = int32(D.lod);
    Mip = int32(D.mip);
    MaterialMode = D.material_mode == SC_MATERIAL_COLORS ? ESCMaterialMode::Colors
                   : D.material_mode == SC_MATERIAL_NONE ? ESCMaterialMode::None
                                                         : ESCMaterialMode::Textures;
    bAttachments = D.attachments != 0;
    bInterior = D.interior != 0;
    bLights = D.lights != 0;
    bShields = D.shields != 0;
    bAnimations = D.animations != 0;
    bDefaultPose = D.default_pose != 0;
    bCollision = true;
}
void USCViewOptions::CopyFrom(const USCViewOptions* Other)
{
    if (!Other) return;
    Lod = Other->Lod;
    Mip = Other->Mip;
    MaterialMode = Other->MaterialMode;
    bAttachments = Other->bAttachments;
    bInterior = Other->bInterior;
    bLights = Other->bLights;
    bShields = Other->bShields;
    bAnimations = Other->bAnimations;
    bDefaultPose = Other->bDefaultPose;
    bCollision = Other->bCollision;
}
void USCViewOptions::ToFfi(sc_options& Out) const
{
    Out = FSCBridgeClient::DefaultOptions();
    Out.lod = uint32(FMath::Max(0, Lod));
    Out.mip = uint32(FMath::Max(0, Mip));
    Out.material_mode = MaterialMode == ESCMaterialMode::Colors ? SC_MATERIAL_COLORS
                        : MaterialMode == ESCMaterialMode::None ? SC_MATERIAL_NONE
                                                                : SC_MATERIAL_TEXTURES;
    Out.attachments = bAttachments;
    Out.interior = bInterior;
    Out.lights = bLights;
    Out.shields = bShields;
    Out.animations = bAnimations;
    Out.default_pose = bDefaultPose;
    Out.collision_reserved = 0;
}
FString USCViewOptions::GetHash() const
{
    const FString Key = FString::Printf(TEXT("lod=%d;mip=%d;mat=%d;att=%d;int=%d;lit=%d;shd=%d;anim=%d;pose=%d;col=%d"), Lod, Mip,
                                        int32(MaterialMode), bAttachments, bInterior, bLights, bShields, bAnimations, bDefaultPose, bCollision);
    return FMD5::HashAnsiString(*Key);
}
FString USCViewOptions::Describe() const
{
    static const TCHAR* Modes[] = {TEXT("textures"), TEXT("colours"), TEXT("no materials")};
    FString Text = FString::Printf(TEXT("LOD %d · mip %d · %s"), Lod, Mip, Modes[FMath::Clamp(int32(MaterialMode), 0, 2)]);
    auto Add = [&](bool bOn, const TCHAR* Name) { if (bOn) Text += FString(TEXT(" · ")) + Name; };
    Add(bAttachments, TEXT("attachments"));
    Add(bInterior, TEXT("interior"));
    Add(bLights, TEXT("lights"));
    Add(bShields, TEXT("shields"));
    Add(bAnimations, TEXT("anim"));
    Add(bDefaultPose, TEXT("default pose"));
    Add(bCollision, TEXT("collision"));
    return Text;
}
