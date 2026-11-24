// USCViewOptions: core defaults, the archive's export options, hashing.
#include "SCViewOptions.h"
#include "SCArchive.h"
#include "Misc/SecureHash.h"

USCViewOptions::USCViewOptions()
{
    ResetToDefaults(); // Config values, when saved, are applied to the CDO after construction and win.
}
void USCViewOptions::ResetToDefaults()
{
    const FSCExportOptions D; // the core's defaults
    Lod = int32(D.Lod);
    Mip = int32(D.Mip);
    MaterialMode = D.MaterialMode == FSCExportOptions::MaterialColors ? ESCMaterialMode::Colors
                   : D.MaterialMode == FSCExportOptions::MaterialNone ? ESCMaterialMode::None
                                                                      : ESCMaterialMode::Textures;
    bAttachments = D.Attachments != 0;
    bInterior = D.Interior != 0;
    bLights = D.Lights != 0;
    bShields = D.Shields != 0;
    bAnimations = D.Animations != 0;
    bDefaultPose = D.DefaultPose != 0;
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
void USCViewOptions::ToExportOptions(FSCExportOptions& Out) const
{
    Out = FSCExportOptions();
    Out.Lod = uint32(FMath::Max(0, Lod));
    Out.Mip = uint32(FMath::Max(0, Mip));
    Out.MaterialMode = MaterialMode == ESCMaterialMode::Colors ? FSCExportOptions::MaterialColors
                       : MaterialMode == ESCMaterialMode::None ? FSCExportOptions::MaterialNone
                                                               : FSCExportOptions::MaterialTextures;
    Out.Attachments = bAttachments;
    Out.Interior = bInterior;
    Out.Lights = bLights;
    Out.Shields = bShields;
    Out.Animations = bAnimations;
    Out.DefaultPose = bDefaultPose;
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
