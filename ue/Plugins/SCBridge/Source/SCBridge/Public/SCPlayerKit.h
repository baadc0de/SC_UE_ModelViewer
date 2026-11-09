// WP-P2b: the `player` block of a character package's scene.json (contract "Player kits"), parsed into plain
// structs: ports, stowed weapons, resolved Mannequin motion states and blend spaces. No UObjects, no game names.
#pragma once
#include "CoreMinimal.h"

class FJsonObject;

/** An authored item port (`port` / `hand_ports[]` / `weapons[].stow_port`). Offsets are native SC (metres, degrees). */
struct SCBRIDGE_API FSCItemPort
{
    FString Name, OwnerEntity, Attachment, AttachBone, ItemOffsetHelper, SelectTag;
    TArray<FString> Flags;
    /** (Type, SubTypes) pairs as authored. */
    TArray<TPair<FString, TArray<FString>>> Types;
    FMatrix Offset = FMatrix::Identity; // row-vector, native SC axes
    bool IsValid() const { return !Name.IsEmpty(); }
    bool HasFlag(const TCHAR* Flag) const { return Flags.ContainsByPredicate([&](const FString& F) { return F.Equals(Flag, ESearchCase::IgnoreCase); }); }
    /** 2: lists ItemType with SubType explicitly, 1: lists ItemType with no subtype restriction, 0: does not accept it. */
    int32 Accepts(const FString& ItemType, const FString& SubType) const;
};

struct SCBRIDGE_API FSCPlayerWeapon
{
    FString Entity, ChildId, Subtype, ItemType;
    int32 Size = 0;
    FSCItemPort StowPort;
    TArray<FString> MannequinTags;
    /** First authored grip: helper on the weapon that meets the hand port joint ("" = source frame) and the
     *  off-hand grip helper ("" = no off-hand grip). */
    FString HandGripHelper, OffHandGripHelper;
};

struct SCBRIDGE_API FSCMotionLayer
{
    FName Clip, BlendSpace;
    bool bAdditive = false, bMissing = false, bLoop = false;
    float Speed = 1, BlendIn = 0, BlendOut = 0, Weight = 1, StartTime = 0;
    int32 Layer = 0;
    bool HasJointMask = false;
};
struct SCBRIDGE_API FSCMotionFragment
{
    FName Fragment;
    TArray<FString> Tags, FragTags, QueryFragTags;
    TArray<FSCMotionLayer> Layers;
    bool IsPlayable() const;
};
struct SCBRIDGE_API FSCMotionState
{
    FString WeaponChildId; // empty: unarmed
    FString Stance, MoveSpeedTag;
    bool bFirstPerson = false;
    TArray<FString> Tags;
    TArray<FSCMotionFragment> Fragments;
    /** The fragment option with the fewest query fragment tags (the plain request), skipping unplayable ones. */
    const FSCMotionFragment* Default(FName Fragment) const;
};

struct SCBRIDGE_API FSCBlendDimension
{
    FString Name;
    float Min = 0, Max = 0, AuthoredMin = 0, AuthoredMax = 0;
};
struct SCBRIDGE_API FSCBlendExample
{
    FName Clip;
    TArray<float> Params; // NaN where the exporter could not resolve one (missing example)
    float PlaybackScale = 1;
    bool bMissing = false;
};
struct SCBRIDGE_API FSCBlendSpace
{
    FName Id;
    TArray<FSCBlendDimension> Dimensions;
    TArray<FSCBlendExample> Examples;
    TArray<TArray<int32>> Faces;
    int32 Dimension(const TCHAR* Name) const;
};

struct SCBRIDGE_API FSCPlayerKit
{
    FString Gender, Loadout, Skeleton, CameraBone, RootMotionBone;
    TArray<FSCItemPort> HandPorts;
    TArray<FSCPlayerWeapon> Weapons;
    TArray<FSCMotionState> States;
    TMap<FName, FSCBlendSpace> BlendSpaces;
    /** Body clips by name -> sidecar path (export-root relative), from root_entity.animations. */
    TMap<FName, FString> ClipSidecars;
    /** Root part port and every child part port, by (entity name, mesh asset). */
    FSCItemPort RootPort;
    struct FPart { FString EntityName, MeshAsset, Id; FSCItemPort Port; };
    TArray<FPart> Parts;

    static bool Parse(const FString& SceneJson, const FString& PackageDir, FSCPlayerKit& Out, FString& Error);
    /** The kit's own manifest: nested item manifests live below it (Packages/PlayerItems/<id>/...), so it is the
     *  single shallowest scene.json. Empty if there is none or the shallowest is ambiguous. */
    static FString FindRootScene(const class FSCPackageSource& Source);
    static FSCItemPort ParsePort(const TSharedPtr<FJsonObject>& Port);

    const FSCPlayerWeapon* FindWeapon(const FString& ChildId) const;
    /** State for (weapon, stance, speed tag, view); empty speed tag selects the untagged state. */
    const FSCMotionState* FindState(const FString& WeaponChildId, const FString& Stance, const FString& MoveSpeedTag, bool bFirstPerson) const;
    /** Hand port that takes the weapon: explicit subtype match beats an unrestricted type; ties keep port order. */
    const FSCItemPort* HandPortFor(const FSCPlayerWeapon& Weapon) const;
    /** Every clip a playable layer of these states references (blend-space examples included). */
    TSet<FName> ReferencedClips() const;
};
