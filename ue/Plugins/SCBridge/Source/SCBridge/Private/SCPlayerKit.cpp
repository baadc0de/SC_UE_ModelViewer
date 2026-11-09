// FSCPlayerKit: lossless-enough parse of the player block; fragment/option choice rules live here so the
// animator and tests share them. Rules are structural (fewest query tags, explicit port type) - no names.
#include "SCPlayerKit.h"
#include "SCPackageReader.h"
#include "SCPackageSource.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Paths.h"
#include <limits>

namespace
{
using FObj = TSharedPtr<FJsonObject>;
const TArray<TSharedPtr<FJsonValue>>& Arr(const FObj& O, const TCHAR* Key)
{
    static const TArray<TSharedPtr<FJsonValue>> Empty;
    const TArray<TSharedPtr<FJsonValue>>* A;
    return O && O->TryGetArrayField(Key, A) ? *A : Empty;
}
FString Str(const FObj& O, const TCHAR* Key)
{
    FString S;
    if (O) O->TryGetStringField(Key, S);
    return S;
}
float Num(const FObj& O, const TCHAR* Key, float Default)
{
    double V;
    return O && O->TryGetNumberField(Key, V) && FMath::IsFinite(V) ? float(V) : Default;
}
bool Bool(const FObj& O, const TCHAR* Key)
{
    bool B = false;
    if (O) O->TryGetBoolField(Key, B);
    return B;
}
FObj Obj(const FObj& O, const TCHAR* Key)
{
    const FObj* Out;
    return O && O->TryGetObjectField(Key, Out) ? *Out : nullptr;
}
TArray<FString> Strings(const FObj& O, const TCHAR* Key)
{
    TArray<FString> Out;
    for (const auto& V : Arr(O, Key))
        if (V->Type == EJson::String) Out.Add(V->AsString());
    return Out;
}
FString GripHelper(const FObj& Grip)
{
    return Str(Grip, TEXT("optionalHelper"));
}
} // namespace

int32 FSCItemPort::Accepts(const FString& ItemType, const FString& SubType) const
{
    int32 Best = 0;
    for (const auto& T : Types)
        if (T.Key.Equals(ItemType, ESearchCase::IgnoreCase))
            Best = FMath::Max(Best, T.Value.IsEmpty() ? 1 : T.Value.ContainsByPredicate([&](const FString& S) { return S.Equals(SubType, ESearchCase::IgnoreCase); }) ? 2 : 0);
    return Best;
}
bool FSCMotionFragment::IsPlayable() const
{
    if (Layers.IsEmpty()) return false;
    for (const auto& L : Layers)
        if (L.bMissing) return false;
    return true;
}
const FSCMotionFragment* FSCMotionState::Default(FName Fragment) const
{
    const FSCMotionFragment* Best = nullptr;
    for (const auto& F : Fragments)
        if (F.Fragment == Fragment && F.IsPlayable() && (!Best || F.QueryFragTags.Num() < Best->QueryFragTags.Num())) Best = &F;
    return Best;
}
int32 FSCBlendSpace::Dimension(const TCHAR* Name) const
{
    return Dimensions.IndexOfByPredicate([&](const FSCBlendDimension& D) { return D.Name.Equals(Name, ESearchCase::IgnoreCase); });
}
FSCItemPort FSCPlayerKit::ParsePort(const FObj& O)
{
    FSCItemPort P;
    if (!O) return P;
    P.Name = Str(O, TEXT("name"));
    P.OwnerEntity = Str(O, TEXT("owner_entity"));
    P.Attachment = Str(O, TEXT("attachment"));
    P.AttachBone = Str(O, TEXT("attach_bone"));
    P.ItemOffsetHelper = Str(O, TEXT("item_offset_helper"));
    P.SelectTag = Str(O, TEXT("select_tag"));
    P.Flags = Strings(O, TEXT("flags"));
    for (const auto& V : Arr(O, TEXT("types")))
        if (V->Type == EJson::Object)
            P.Types.Emplace(Str(V->AsObject(), TEXT("Type")), Strings(V->AsObject(), TEXT("SubTypes")));
    P.Offset = FSCPackageReader::PortOffset(O);
    return P;
}
bool FSCPlayerKit::Parse(const FString& SceneJson, const FString& PackageDir, FSCPlayerKit& Out, FString& Error)
{
    Out = FSCPlayerKit();
    FObj Scene;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SceneJson), Scene) || !Scene)
    {
        Error = TEXT("Invalid scene JSON");
        return false;
    }
    const FObj Player = Obj(Scene, TEXT("player"));
    if (!Player)
    {
        Error = TEXT("Scene has no player block (not a player kit)");
        return false;
    }
    Out.Gender = Str(Player, TEXT("gender"));
    Out.Loadout = Str(Player, TEXT("loadout"));
    Out.Skeleton = Str(Player, TEXT("skeleton"));
    Out.CameraBone = Str(Player, TEXT("camera_bone"));
    Out.RootMotionBone = Str(Player, TEXT("root_motion_bone"));
    if (Out.CameraBone.IsEmpty() || Out.RootMotionBone.IsEmpty())
    {
        Error = TEXT("player block lacks camera_bone or root_motion_bone");
        return false;
    }
    for (const auto& V : Arr(Player, TEXT("hand_ports")))
        if (V->Type == EJson::Object) Out.HandPorts.Add(ParsePort(V->AsObject()));
    // Parts: root and children (only top level carries body parts and loadout items).
    const FObj Root = Obj(Scene, TEXT("root_entity"));
    Out.RootPort = ParsePort(Obj(Root, TEXT("port")));
    Out.Parts.Add({Str(Root, TEXT("entity_name")), Str(Root, TEXT("mesh_asset")), Str(Root, TEXT("id")), Out.RootPort});
    TMap<FString, FString> ItemTypes;
    for (const auto& V : Arr(Scene, TEXT("children")))
        if (V->Type == EJson::Object)
        {
            const FObj C = V->AsObject();
            Out.Parts.Add({Str(C, TEXT("entity_name")), Str(C, TEXT("mesh_asset")), Str(C, TEXT("id")), ParsePort(Obj(C, TEXT("port")))});
            if (!Str(C, TEXT("id")).IsEmpty()) ItemTypes.Add(Str(C, TEXT("id")), Str(C, TEXT("item_type")));
        }
    for (const auto& V : Arr(Root, TEXT("animations")))
        if (V->Type == EJson::Object)
        {
            FString Path;
            if (FSCPackageSource::Normalize(PackageDir / Str(V->AsObject(), TEXT("sidecar")), Path))
                Out.ClipSidecars.Add(FName(*Str(V->AsObject(), TEXT("name"))), Path);
        }
    for (const auto& V : Arr(Player, TEXT("weapons")))
    {
        if (V->Type != EJson::Object) continue;
        const FObj W = V->AsObject();
        FSCPlayerWeapon Weapon;
        Weapon.Entity = Str(W, TEXT("entity"));
        Weapon.ChildId = Str(W, TEXT("child_id"));
        Weapon.Subtype = Str(W, TEXT("subtype"));
        Weapon.ItemType = ItemTypes.FindRef(Weapon.ChildId);
        Weapon.Size = int32(Num(W, TEXT("size"), 0));
        Weapon.StowPort = ParsePort(Obj(W, TEXT("stow_port")));
        Weapon.MannequinTags = Strings(W, TEXT("mannequin_tags"));
        // The first authored grip is the default carry: hand helper meets the hand port, off-hand helper takes
        // the other hand (gripData.grips[0].handGrip / offHandGrip optionalHelper).
        const auto& Grips = Arr(Obj(W, TEXT("grip_data")), TEXT("grips"));
        if (!Grips.IsEmpty() && Grips[0]->Type == EJson::Object)
        {
            Weapon.HandGripHelper = GripHelper(Obj(Grips[0]->AsObject(), TEXT("handGrip")));
            Weapon.OffHandGripHelper = GripHelper(Obj(Grips[0]->AsObject(), TEXT("offHandGrip")));
        }
        Out.Weapons.Add(MoveTemp(Weapon));
    }
    const FObj Motion = Obj(Player, TEXT("motion"));
    for (const auto& V : Arr(Motion, TEXT("states")))
    {
        if (V->Type != EJson::Object) continue;
        const FObj S = V->AsObject();
        FSCMotionState State;
        State.WeaponChildId = Str(S, TEXT("weapon_child_id"));
        State.Stance = Str(S, TEXT("stance"));
        State.MoveSpeedTag = Str(S, TEXT("move_speed_tag"));
        State.bFirstPerson = Bool(S, TEXT("first_person"));
        State.Tags = Strings(S, TEXT("tags"));
        for (const auto& FV : Arr(S, TEXT("fragments")))
        {
            if (FV->Type != EJson::Object) continue;
            const FObj F = FV->AsObject();
            FSCMotionFragment Fragment;
            Fragment.Fragment = FName(*Str(F, TEXT("fragment")));
            Fragment.Tags = Strings(F, TEXT("tags"));
            Fragment.FragTags = Strings(F, TEXT("frag_tags"));
            Fragment.QueryFragTags = Strings(F, TEXT("query_frag_tags"));
            for (const auto& LV : Arr(F, TEXT("layers")))
            {
                if (LV->Type != EJson::Object) continue;
                const FObj L = LV->AsObject();
                FSCMotionLayer Layer;
                Layer.Clip = FName(*Str(L, TEXT("clip")));
                Layer.BlendSpace = FName(*Str(L, TEXT("blend_space")));
                Layer.bAdditive = Bool(L, TEXT("additive"));
                Layer.bMissing = Bool(L, TEXT("missing"));
                Layer.bLoop = Bool(L, TEXT("loop"));
                Layer.Speed = Num(L, TEXT("speed"), 1);
                Layer.BlendIn = Num(L, TEXT("blend_in"), 0);
                Layer.BlendOut = Num(L, TEXT("blend_out"), 0);
                Layer.Weight = Num(L, TEXT("weight"), 1);
                Layer.StartTime = Num(L, TEXT("start_time"), 0);
                Layer.Layer = int32(Num(L, TEXT("layer"), 0));
                const TSharedPtr<FJsonValue> Mask = L->TryGetField(TEXT("joint_mask"));
                Layer.HasJointMask = Mask.IsValid() && !Mask->IsNull();
                Fragment.Layers.Add(Layer);
            }
            State.Fragments.Add(MoveTemp(Fragment));
        }
        Out.States.Add(MoveTemp(State));
    }
    for (const auto& V : Arr(Player, TEXT("blend_spaces")))
    {
        if (V->Type != EJson::Object) continue;
        const FObj B = V->AsObject();
        FSCBlendSpace Space;
        Space.Id = FName(*Str(B, TEXT("id")));
        for (const auto& DV : Arr(B, TEXT("dimensions")))
            if (DV->Type == EJson::Object)
            {
                const FObj D = DV->AsObject();
                Space.Dimensions.Add({Str(D, TEXT("name")), Num(D, TEXT("min"), 0), Num(D, TEXT("max"), 0), Num(D, TEXT("authored_min"), 0),
                                      Num(D, TEXT("authored_max"), 0)});
            }
        for (const auto& EV : Arr(B, TEXT("examples")))
        {
            if (EV->Type != EJson::Object) continue;
            const FObj E = EV->AsObject();
            FSCBlendExample Example;
            Example.Clip = FName(*Str(E, TEXT("clip")));
            Example.PlaybackScale = Num(E, TEXT("playback_scale"), 1);
            Example.bMissing = Bool(E, TEXT("missing"));
            for (const auto& P : Arr(E, TEXT("params")))
            {
                double X;
                Example.Params.Add(P->TryGetNumber(X) ? float(X) : std::numeric_limits<float>::quiet_NaN());
            }
            Space.Examples.Add(MoveTemp(Example));
        }
        for (const auto& FV : Arr(B, TEXT("faces")))
        {
            const TArray<TSharedPtr<FJsonValue>>* Face;
            if (!FV->TryGetArray(Face)) continue;
            TArray<int32> Indices;
            for (const auto& I : *Face)
                Indices.Add(int32(I->AsNumber()));
            Space.Faces.Add(MoveTemp(Indices));
        }
        Out.BlendSpaces.Add(Space.Id, MoveTemp(Space));
    }
    return true;
}
FString FSCPlayerKit::FindRootScene(const FSCPackageSource& Source)
{
    FString Best;
    int32 BestDepth = MAX_int32, AtBest = 0;
    for (const FString& File : Source.ListFiles())
    {
        if (FPaths::GetCleanFilename(File) != TEXT("scene.json")) continue;
        int32 Depth = 0;
        for (TCHAR C : File) Depth += C == TEXT('/');
        if (Depth < BestDepth) { BestDepth = Depth; Best = File; AtBest = 1; }
        else if (Depth == BestDepth) ++AtBest;
    }
    return AtBest == 1 ? Best : FString();
}
const FSCPlayerWeapon* FSCPlayerKit::FindWeapon(const FString& ChildId) const
{
    return Weapons.FindByPredicate([&](const FSCPlayerWeapon& W) { return W.ChildId == ChildId; });
}
const FSCMotionState* FSCPlayerKit::FindState(const FString& WeaponChildId, const FString& Stance, const FString& MoveSpeedTag, bool bFirstPerson) const
{
    return States.FindByPredicate([&](const FSCMotionState& S) {
        return S.WeaponChildId == WeaponChildId && S.Stance == Stance && S.MoveSpeedTag == MoveSpeedTag && S.bFirstPerson == bFirstPerson;
    });
}
const FSCItemPort* FSCPlayerKit::HandPortFor(const FSCPlayerWeapon& Weapon) const
{
    const FSCItemPort* Best = nullptr;
    int32 BestScore = 0;
    for (const auto& Port : HandPorts)
    {
        const int32 Score = Port.Accepts(Weapon.ItemType, Weapon.Subtype);
        if (Score > BestScore)
        {
            Best = &Port;
            BestScore = Score;
        }
    }
    return Best;
}
TSet<FName> FSCPlayerKit::ReferencedClips() const
{
    TSet<FName> Out;
    for (const auto& S : States)
        for (const auto& F : S.Fragments)
            for (const auto& L : F.Layers)
            {
                if (L.bMissing) continue;
                if (!L.Clip.IsNone()) Out.Add(L.Clip);
                if (const FSCBlendSpace* B = BlendSpaces.Find(L.BlendSpace))
                    for (const auto& E : B->Examples)
                        if (!E.bMissing) Out.Add(E.Clip);
            }
    return Out;
}
