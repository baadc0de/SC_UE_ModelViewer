// USCPlayerKitData (SCPlayerKitData.h): a prepared player body's kit and clips, written and read field by field.
#include "SCPlayerKitData.h"
#include "Serialization/Archive.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCPlayerKitData, Log, All);

FArchive& operator<<(FArchive& Ar, FSCItemPort& P)
{
    return Ar << P.Name << P.OwnerEntity << P.Attachment << P.AttachBone << P.ItemOffsetHelper << P.SelectTag << P.Flags << P.Types << P.Offset;
}
FArchive& operator<<(FArchive& Ar, FSCFireAction& A)
{
    return Ar << A.Kind << A.FireRate << A.FireFragment << A.StopFragment << A.OneShotFragment << A.FireHelper << A.MuzzleHelper << A.EffectHelpers;
}
FArchive& operator<<(FArchive& Ar, FSCWeaponFragment::FLayer& L)
{
    return Ar << L.Clip << L.bLoop << L.Speed;
}
FArchive& operator<<(FArchive& Ar, FSCWeaponFragment& F)
{
    return Ar << F.Fragment << F.Tags << F.FragTags << F.Layers;
}
FArchive& operator<<(FArchive& Ar, FSCPlayerWeapon& W)
{
    return Ar << W.Entity << W.ChildId << W.Subtype << W.ItemType << W.Size << W.StowPort << W.MannequinTags << W.HandGripHelper
              << W.OffHandGripHelper << W.FireActions << W.FireFragments << W.ClipSidecars << W.FireClipJson;
}
FArchive& operator<<(FArchive& Ar, FSCMotionLayer& L)
{
    return Ar << L.Clip << L.BlendSpace << L.bAdditive << L.bMissing << L.bLoop << L.Speed << L.BlendIn << L.BlendOut << L.Weight << L.StartTime
              << L.ExitTime << L.Layer << L.HasJointMask << L.bUnsupported;
}
FArchive& operator<<(FArchive& Ar, FSCMotionFragment& F)
{
    return Ar << F.Fragment << F.Tags << F.FragTags << F.QueryFragTags << F.Layers;
}
FArchive& operator<<(FArchive& Ar, FSCMotionState& S)
{
    return Ar << S.WeaponChildId << S.Stance << S.MoveSpeedTag << S.bFirstPerson << S.Tags << S.Fragments;
}
FArchive& operator<<(FArchive& Ar, FSCBlendDimension& D)
{
    return Ar << D.Name << D.Min << D.Max << D.AuthoredMin << D.AuthoredMax;
}
FArchive& operator<<(FArchive& Ar, FSCBlendExample& E)
{
    return Ar << E.Clip << E.Params << E.PlaybackScale << E.bMissing << E.Component;
}
FArchive& operator<<(FArchive& Ar, FSCBlendSelector& S)
{
    return Ar << S.Name << S.Dimension << S.bChooseBlendSpace << S.bLocked;
}
FArchive& operator<<(FArchive& Ar, FSCBlendSpace& B)
{
    return Ar << B.Id << B.Dimensions << B.Examples << B.Faces << B.Components << B.Selectors << B.Unsupported;
}
FArchive& operator<<(FArchive& Ar, FSCFlashlight& L)
{
    return Ar << L.Entity << L.Port << L.Color << L.DiffuseMult << L.BulbRadiusM << L.DistanceM << L.FovDeg << L.Cookie << L.ProjectorTexture;
}
FArchive& operator<<(FArchive& Ar, FSCPlayerKit::FPart& P)
{
    return Ar << P.EntityName << P.MeshAsset << P.Id << P.Port;
}
FArchive& operator<<(FArchive& Ar, FSCPlayerKit& K)
{
    return Ar << K.Gender << K.Loadout << K.Skeleton << K.CameraBone << K.RootMotionBone << K.HandPorts << K.Weapons << K.Flashlights << K.States
              << K.BlendSpaces << K.ClipSidecars << K.RootPort << K.Parts << K.Diagnostics;
}
FArchive& operator<<(FArchive& Ar, FSCPlayerClip::FTrack& T)
{
    return Ar << T.Bone << T.RotTimes << T.PosTimes << T.Rot << T.Pos;
}
FArchive& operator<<(FArchive& Ar, FSCPlayerClip& C)
{
    return Ar << C.Name << C.Fps << C.Frames << C.Duration << C.bAdditive << C.bCyclic << C.Tracks;
}

namespace
{
/** A clip map as its count and (name, clip) pairs; a null entry (none are made) is left out. */
void WriteClips(FArchive& Ar, const FSCPlayerClipMap& Clips)
{
    int32 Count = 0;
    for (const auto& [Key, Clip] : Clips) Count += Clip.IsValid();
    Ar << Count;
    for (const auto& [Key, Clip] : Clips)
        if (Clip)
        {
            FName Name = Key;
            // Written from the shared, read-only data: the operators take references but do not change what they save.
            Ar << Name << const_cast<FSCPlayerClip&>(*Clip);
        }
}
TSharedRef<FSCPlayerClipMap> ReadClipMap(FArchive& Ar)
{
    int32 Count = 0;
    Ar << Count;
    const TSharedRef<FSCPlayerClipMap> Clips = MakeShared<FSCPlayerClipMap>();
    if (Count < 0) Ar.SetError();
    for (int32 I = 0; I < Count && !Ar.IsError(); ++I)
    {
        FName Key;
        const TSharedRef<FSCPlayerClip> Clip = MakeShared<FSCPlayerClip>();
        Ar << Key << *Clip;
        Clips->Add(Key, Clip);
    }
    return Clips;
}
} // namespace

void USCPlayerKitData::WritePayload(const FSCPlayerKit& Kit, const FSCPlayerClipMap& Clips, TArray<uint8>& Out)
{
    Out.Reset();
    FMemoryWriter Ar(Out);
    Ar << const_cast<FSCPlayerKit&>(Kit);
    WriteClips(Ar, Clips);
}
bool USCPlayerKitData::ReadPayload(int32 Layout, const TArray<uint8>& Bytes, TSharedPtr<const FSCPlayerKit>& OutKit, TSharedPtr<const FSCPlayerClipMap>& OutClips)
{
    OutKit.Reset();
    OutClips.Reset();
    if (Layout != Version) return false; // another layout cannot be read field by field
    FMemoryReader Ar(Bytes);
    const TSharedRef<FSCPlayerKit> Kit = MakeShared<FSCPlayerKit>();
    Ar << *Kit;
    const TSharedRef<FSCPlayerClipMap> Clips = ReadClipMap(Ar);
    // All of it, exactly: a short or a long payload is not this layout either.
    if (Ar.IsError() || Ar.Tell() != Bytes.Num()) return false;
    OutKit = Kit;
    OutClips = Clips;
    return true;
}

void USCPlayerKitData::Serialize(FArchive& Ar)
{
    Super::Serialize(Ar);
    // Only a save or a load carries the data (it holds no object references for other archives to visit).
    if (!Ar.IsSaving() && !Ar.IsLoading()) return;
    // The payload is one byte array after its layout: a load reads all of it whatever the layout (a package's export is read
    // to its end), and only a payload of this layout is parsed.
    int32 Layout = Version;
    TArray<uint8> Bytes;
    if (Ar.IsSaving() && HasData()) WritePayload(*Kit, *Clips, Bytes);
    Ar << Layout << Bytes;
    if (!Ar.IsLoading()) return;
    if (Bytes.IsEmpty())
    {
        Kit.Reset();
        Clips.Reset();
    }
    else if (!ReadPayload(Layout, Bytes, Kit, Clips))
    {
        UE_LOG(LogSCPlayerKitData, Warning, TEXT("PLAYER_KIT_DATA %s holds layout %d (%d bytes) this build (layout %d) does not read: no player body (prepare it again)"),
               *GetPathName(), Layout, Bytes.Num(), Version);
    }
}

void USCTraversalClipsData::Serialize(FArchive& Ar)
{
    Super::Serialize(Ar);
    if (!Ar.IsSaving() && !Ar.IsLoading()) return;
    // As USCPlayerKitData: the clips as one payload after the layout, read whole whatever the layout.
    int32 Layout = USCPlayerKitData::Version;
    TArray<uint8> Bytes;
    if (Ar.IsSaving() && Clips)
    {
        FMemoryWriter Writer(Bytes);
        WriteClips(Writer, *Clips);
    }
    Ar << Layout << Bytes;
    if (!Ar.IsLoading()) return;
    Clips.Reset();
    if (Layout != USCPlayerKitData::Version)
    {
        UE_LOG(LogSCPlayerKitData, Warning, TEXT("TRAVERSAL_CLIPS_DATA %s holds layout %d, this build reads %d: none (prepare it again)"), *GetPathName(),
               Layout, USCPlayerKitData::Version);
        return;
    }
    FMemoryReader Reader(Bytes);
    const TSharedRef<FSCPlayerClipMap> Loaded = ReadClipMap(Reader);
    if (Reader.IsError() || Reader.Tell() != Bytes.Num())
    {
        UE_LOG(LogSCPlayerKitData, Warning, TEXT("TRAVERSAL_CLIPS_DATA %s does not read (%d bytes): none (prepare it again)"), *GetPathName(), Bytes.Num());
        return;
    }
    Clips = Loaded;
}
