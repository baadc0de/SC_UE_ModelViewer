// WP-O1-4c-3a: the clips' run-time sampling (SCClipCatalog.h FSCClipSampling), split from SCAnimImporter.cpp.
#include "SCClipCatalog.h"
uint32 FSCClipSampling::BoneHash(const FString& Name)
{
    // Standard CRC32 of case-sensitive UTF-8, not UE's TCHAR-based StrCrc32.
    FTCHARToUTF8 Bytes(*Name);
    uint32 CRC = ~0u;
    for (int32 I = 0; I < Bytes.Length(); ++I)
    {
        CRC ^= uint8(Bytes.Get()[I]);
        for (int32 Bit = 0; Bit < 8; ++Bit)
            CRC = (CRC >> 1) ^ ((CRC & 1) ? 0xedb88320u : 0);
    }
    return ~CRC;
}
FVector FSCClipSampling::SidecarPosition(const FVector& V)
{
    // Undo exporter (x,y,z)->(x,-z,y), then GLB G=(-z,x,y), metres to cm.
    return FVector(V.Y, V.X, V.Z) * 100.;
}
FQuat FSCClipSampling::SidecarRotation(const FQuat& Q)
{
    return FQuat(-Q.Y, -Q.X, -Q.Z, Q.W).GetNormalized();
}

FQuat FSCClipSampling::LinearRotation(FQuat A, FQuat B, double Alpha)
{
    if ((A | B) < 0) B = B * -1.;
    return (A * (1 - Alpha) + B * Alpha).GetNormalized();
}
FTransform FSCClipSampling::Sample(const FSCBoneTrack& Track, double Frame)
{
    if (Track.Keys.IsEmpty()) return FTransform::Identity;
    Frame = FMath::Clamp(Frame, 0., double(Track.Keys.Num() - 1));
    const int32 A = FMath::FloorToInt(Frame), B = FMath::Min(A + 1, Track.Keys.Num() - 1);
    const double T = Frame - A;
    return FTransform(LinearRotation(Track.Keys[A].GetRotation(), Track.Keys[B].GetRotation(), T),
                      FMath::Lerp(Track.Keys[A].GetTranslation(), Track.Keys[B].GetTranslation(), T),
                      FMath::Lerp(Track.Keys[A].GetScale3D(), Track.Keys[B].GetScale3D(), T));
}
double FSCClipSampling::TrackFrame(const FSCImportedClip& Clip, const FSCBoneTrack& Track, double ClipFrame, bool bFromEnd)
{
    checkf(Clip.Fps > 0 && Track.Fps > 0, TEXT("clip and track rates are set by the importer"));
    const double Last = FMath::Max(0, Track.Keys.Num() - 1);
    const double Scale = Track.Fps / Clip.Fps;
    return bFromEnd ? FMath::Max(0., Last - (Clip.LastFrame - ClipFrame) * Scale) : FMath::Min(Last, ClipFrame * Scale);
}
