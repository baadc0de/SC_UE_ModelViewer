// USCPlayerBodyComponent: kit attachment, pose transfer leader -> followers, stow/hand re-parenting by port
// alignment, first-person head hiding (shadows kept), eye at the camera joint and the third-person orbit.
#include "SCPlayerBody.h"
#include "HAL/IConsoleManager.h"
#include "SCFirstPersonCharacter.h"
#include "SCRigComponent.h"
#include "SCVehicle.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCPlayer, Log, All);
// Off by default: the LIVE stocked idle/locomotion clips author their left-weapon IK blend channel at 0 (the
// clip places the off hand itself); forcing IK to the grip helper pulls the hand back to the magazine well.
static TAutoConsoleVariable<int32> CVarOffHandIK(TEXT("sc.Player.OffHandIK"), 0,
    TEXT("1: off-hand two-bone IK to the weapon's off-hand grip helper while armed; 0 (default): the clip's own off hand"));

namespace
{
// Share of a rig's skin weight carried by the subtree of joint Root (by name on the rig's own skeleton).
float SubtreeInfluence(const USCRigComponent* Rig, FName Root)
{
    const USkeletalMesh* Mesh = Rig->GetSkeletalMeshAsset();
    const FSkeletalMeshRenderData* Render = Mesh ? Mesh->GetResourceForRendering() : nullptr;
    if (!Render || Render->LODRenderData.IsEmpty()) return 0;
    const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
    const int32 RootIndex = Ref.FindBoneIndex(Root);
    if (RootIndex == INDEX_NONE) return 0;
    TArray<bool> Under;
    Under.SetNumZeroed(Ref.GetNum());
    for (int32 I = 0; I < Ref.GetNum(); ++I)
        Under[I] = I == RootIndex || (Ref.GetParentIndex(I) != INDEX_NONE && Under[Ref.GetParentIndex(I)]);
    const FSkeletalMeshLODRenderData& LOD = Render->LODRenderData[0];
    const FSkinWeightVertexBuffer& Weights = LOD.SkinWeightVertexBuffer;
    if (!Weights.GetNeedsCPUAccess() || Weights.GetNumVertices() == 0) return 0;
    double Inside = 0, Total = 0;
    const uint32 Influences = Weights.GetMaxBoneInfluences();
    for (uint32 V = 0; V < Weights.GetNumVertices(); ++V)
        for (uint32 K = 0; K < Influences; ++K)
        {
            const double W = Weights.GetBoneWeight(V, K);
            const uint32 Bone = Weights.GetBoneIndex(V, K);
            Total += W;
            if (Under.IsValidIndex(int32(Bone)) && Under[Bone]) Inside += W;
        }
    return Total > 0 ? float(Inside / Total) : 0.f;
}
} // namespace

USCPlayerBodyComponent::USCPlayerBodyComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
    // After the character and its movement: the pose and the eye follow this frame's capsule.
    PrimaryComponentTick.TickGroup = TG_PostPhysics;
}
ASCFirstPersonCharacter* USCPlayerBodyComponent::Character() const
{
    return Cast<ASCFirstPersonCharacter>(GetOwner());
}
USCRigComponent* USCPlayerBodyComponent::FindLeader(const ASCVehicle* Kit)
{
    if (!Kit) return nullptr;
    for (USCRigComponent* Rig : Kit->Rigs)
        if (Rig && Rig->OwnerEntity == Kit->Manifest.Root.EntityName && Rig->MeshAsset == Kit->Manifest.Root.MeshAsset) return Rig;
    return nullptr;
}
FSCImportOptions USCPlayerBodyComponent::KitImportOptions(UWorld* World, const FString& ScenePath)
{
    FSCImportOptions Options;
    Options.World = World;
    Options.ScenePath = ScenePath;
    Options.bBuildCollision = false;
    Options.bPublish = false;
    Options.bReplacePrevious = false;
    Options.bRigSkinnedMeshes = true;
    Options.bImportClips = false;
    return Options;
}
TMap<FString, int32> USCPlayerBodyComponent::BoneMap(const USCRigComponent* Leader)
{
    TMap<FString, int32> Bones;
    const FReferenceSkeleton& Ref = Leader->GetSkeletalMeshAsset()->GetRefSkeleton();
    for (int32 I = 0; I < Ref.GetNum(); ++I) Bones.Add(Ref.GetBoneName(I).ToString(), I);
    return Bones;
}
bool USCPlayerBodyComponent::Attach(ASCVehicle* InKit, TSharedPtr<const FSCPlayerKit> InData, TSharedPtr<const FSCPlayerClipMap> InClips,
                                    FString& Error)
{
    Detach();
    ASCFirstPersonCharacter* C = Character();
    USCRigComponent* Root = FindLeader(InKit);
    if (!C || !Root || !InData || !InClips)
    {
        Error = TEXT("Player kit has no root rig");
        return false;
    }
    Data = InData;
    ClipStore = InClips;
    if (!Animator.Init(*Data, Root->GetSkeletalMeshAsset()->GetRefSkeleton(), *ClipStore, Error)) return false;
    Kit = InKit;
    Leader = Root;
    CameraBone = Animator.Bone(Data->CameraBone);
    // Followers: every other rig on the body skeleton (it carries the camera and root-motion joints) renders with
    // the leader's bone buffer, matched by joint name (leader pose: no per-part evaluation); weapon and item rigs
    // keep their own bind pose.
    Followers.Reset();
    const FReferenceSkeleton& LeaderRef = Leader->GetSkeletalMeshAsset()->GetRefSkeleton();
    for (USCRigComponent* Rig : Kit->Rigs)
    {
        if (!Rig || Rig == Leader) continue;
        const FReferenceSkeleton& Ref = Rig->GetSkeletalMeshAsset()->GetRefSkeleton();
        if (Ref.FindBoneIndex(FName(*Data->CameraBone)) == INDEX_NONE || Ref.FindBoneIndex(FName(*Data->RootMotionBone)) == INDEX_NONE) continue;
        TArray<int32> Map;
        Map.SetNum(Ref.GetNum());
        bool bComplete = true;
        for (int32 B = 0; B < Ref.GetNum(); ++B)
        {
            Map[B] = B == 0 ? INDEX_NONE : LeaderRef.FindBoneIndex(Ref.GetBoneName(B)); // bone 0 is the host wrapper
            bComplete &= B == 0 || Map[B] != INDEX_NONE;
        }
        // A part whose joints all exist on the leader skins with the leader's buffer (leader pose). A part with
        // joints of its own (hair, cloth) keeps them at their bind relative to their parents: it copies the pose.
        if (bComplete) Rig->SetLeaderPoseComponent(Leader, true);
        else CopyMaps.Add(Rig, MoveTemp(Map));
        Followers.Add(Rig);
    }
    // First-person hidden parts: ports flagged invisible in first person or head wear, the root part's own port
    // (the head socket), and parts skinned mostly to the joint that carries the camera.
    HeadParts.Reset();
    const int32 Head = LeaderRef.GetParentIndex(LeaderRef.FindBoneIndex(FName(*Data->CameraBone)));
    const FName HeadName = Head == INDEX_NONE ? NAME_None : LeaderRef.GetBoneName(Head);
    TArray<USCRigComponent*> Body = {Leader};
    for (USCRigComponent* F : Followers) Body.Add(F);
    auto PortOf = [&](const USCRigComponent* Rig) {
        const FSCPlayerKit::FPart* Part = Data->Parts.FindByPredicate([&](const FSCPlayerKit::FPart& P) {
            return P.EntityName == Rig->OwnerEntity && P.MeshAsset == Rig->MeshAsset;
        });
        return Part ? Part->Port : FSCItemPort();
    };
    // Worn head wear encloses the rest of the head: then only it casts the first-person (hidden) head shadow.
    const bool bHeadWear = Body.ContainsByPredicate([&](const USCRigComponent* Rig) { return PortOf(Rig).HasFlag(TEXT("HeadWear")); });
    for (USCRigComponent* Rig : Body)
    {
        const FSCItemPort Port = PortOf(Rig);
        const float Influence = SubtreeInfluence(Rig, HeadName);
        const bool bHide = Port.HasFlag(TEXT("invis_p1")) || Port.HasFlag(TEXT("HeadWear")) ||
                           (Port.IsValid() && Port.Name == Data->RootPort.Name && Port.OwnerEntity == Data->RootPort.OwnerEntity) || Influence > 0.5f;
        UE_LOG(LogSCPlayer, Display, TEXT("PLAYER_PART entity=%s port=%s flags=%s head_influence=%.2f hidden_fp=%d fp_shadow=%d leader_pose=%d"),
               *Rig->OwnerEntity, *Port.Name, *FString::Join(Port.Flags, TEXT(",")), Influence, bHide,
               !bHide || !bHeadWear || Port.HasFlag(TEXT("HeadWear")), Rig != Leader && !CopyMaps.Contains(Rig));
        Rig->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        const bool bShadow = !bHeadWear || Port.HasFlag(TEXT("HeadWear"));
        Rig->bCastHiddenShadow = bShadow;
        if (!bHide) continue;
        HeadParts.Add(Rig);
        TArray<USceneComponent*> Children;
        Rig->GetChildrenComponents(false, Children);
        for (USceneComponent* Child : Children) // rigid sections of the same mesh asset
            if (auto* Mesh = Cast<UStaticMeshComponent>(Child); Mesh && Mesh->ComponentTags.Contains(FName(*Rig->MeshAsset)))
            {
                Mesh->bCastHiddenShadow = bShadow;
                HeadParts.Add(Mesh);
            }
    }
    // The body moves every frame: keep it out of the distance-field and Lumen scene representations (a moving
    // caster there forces global distance-field and surface-cache updates each frame); it still renders, casts
    // shadows and is lit. No collision on body or weapon parts.
    TArray<UPrimitiveComponent*> Primitives;
    Kit->GetComponents<UPrimitiveComponent>(Primitives);
    for (UPrimitiveComponent* P : Primitives)
    {
        P->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        P->bAffectDistanceFieldLighting = false;
        P->bAffectDynamicIndirectLighting = false;
        P->MarkRenderStateDirty();
    }
    // Weapon slots: stowed weapons the body can draw (an anchor, a hand port, playable select and deselect),
    // ordered by size (primary first), then stow port name (port numbering), then authored order.
    Slots.Reset();
    for (int32 W = 0; W < Data->Weapons.Num(); ++W)
    {
        const FSCPlayerWeapon& Weapon = Data->Weapons[W];
        const FSCMotionState* State = Data->FindState(Weapon.ChildId, FSCPlayerAnimator::StandTag, FString(), true);
        if (!Kit->InstanceAnchors.Contains(Weapon.ChildId) || !Data->HandPortFor(Weapon) || !State ||
            !State->Default(FSCPlayerAnimator::SelectFragment) || !State->Default(FSCPlayerAnimator::DeselectFragment))
            continue;
        Slots.Add(W);
    }
    Slots.StableSort([&](int32 A, int32 B) {
        const auto &WA = Data->Weapons[A], &WB = Data->Weapons[B];
        return WA.Size != WB.Size ? WA.Size > WB.Size : WA.StowPort.Name < WB.StowPort.Name;
    });
    for (int32 I = 0; I < Slots.Num(); ++I)
    {
        PlaceWeapon(Slots[I], false);
        UE_LOG(LogSCPlayer, Display, TEXT("PLAYER_SLOT key=%d weapon=%s child=%s stow=%s/%s hand=%s"), I + 1, *Data->Weapons[Slots[I]].Entity,
               *Data->Weapons[Slots[I]].ChildId, *Data->Weapons[Slots[I]].StowPort.Name, *Data->Weapons[Slots[I]].StowPort.AttachBone,
               *Data->HandPortFor(Data->Weapons[Slots[I]])->AttachBone);
    }
    Weapons = FSCWeaponStateMachine();
    // Feet on the capsule floor, facing the character's forward (+X).
    const FVector F = Animator.Forward();
    Kit->AttachToComponent(C->GetCapsuleComponent(), FAttachmentTransformRules::KeepRelativeTransform);
    Kit->SetActorRelativeTransform(FTransform(FRotator(0, -FMath::RadiansToDegrees(FMath::Atan2(F.Y, F.X)), 0),
                                              FVector(0, 0, -C->GetCapsuleComponent()->GetScaledCapsuleHalfHeight())));
    Kit->SetActorHiddenInGame(false);
    C->SetEyeSource([this] { return Eye; });
    if (USCEyeCameraComponent* Camera = Cast<USCEyeCameraComponent>(C->Camera)) Camera->NearClip = NearClip;
    ProfileWeapon = TEXT("<none>");
    UpdateProfile();
    AddTickPrerequisiteComponent(C->GetCharacterMovement());
    SetComponentTickEnabled(true);
    Step(0); // pose and eye before the first rendered frame
    UE_LOG(LogSCPlayer, Display, TEXT("PLAYER_BODY attached rigs=%d followers=%d (pose copies %d) head_parts=%d slots=%d clips=%d spine=%d camera=%s forward=%s"),
           Kit->Rigs.Num(), Followers.Num(), CopyMaps.Num(), HeadParts.Num(), Slots.Num(), ClipStore->Num(), Animator.PitchChain().Num(), *Data->CameraBone,
           *F.ToCompactString());
    return true;
}
void USCPlayerBodyComponent::Detach()
{
    SetComponentTickEnabled(false);
    if (ASCFirstPersonCharacter* C = Character())
    {
        C->SetEyeSource(nullptr);
        if (USCEyeCameraComponent* Camera = Cast<USCEyeCameraComponent>(C->Camera)) Camera->NearClip = 0;
        if (C->Camera) C->Camera->bUsePawnControlRotation = true;
    }
    if (Kit && IsValid(Kit) && !Kit->IsActorBeingDestroyed())
    {
        // Leave the kit as it was imported: weapons stowed, every part visible, hidden until the next attach.
        for (int32 W : Slots) PlaceWeapon(W, false);
        for (UPrimitiveComponent* P : HeadParts)
            if (P)
            {
                P->SetHiddenInGame(false);
                P->SetVisibility(true);
            }
        Kit->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
        Kit->SetActorHiddenInGame(true);
    }
    for (USCRigComponent* F : Followers)
        if (F && IsValid(F))
        {
            F->SetLeaderPoseComponent(nullptr);
            F->ResetPose();
        }
    Kit = nullptr;
    Leader = nullptr;
    Followers.Reset();
    CopyMaps.Reset();
    HeadParts.Reset();
    Slots.Reset();
    Weapons = FSCWeaponStateMachine();
    bThirdPerson = false;
}
void USCPlayerBodyComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    Detach();
    Super::EndPlay(Reason);
}
const FSCPlayerWeapon* USCPlayerBodyComponent::SlotWeapon(int32 Slot) const
{
    return Slots.IsValidIndex(Slot) ? &Data->Weapons[Slots[Slot]] : nullptr;
}
FName USCPlayerBodyComponent::WeaponParentBone(int32 Slot) const
{
    return Slots.IsValidIndex(Slot) ? WeaponAttach.FindRef(Slots[Slot]) : NAME_None;
}
void USCPlayerBodyComponent::ResolvePortFrame(const FSCItemPort& Port, USceneComponent*& Parent, FName& Socket, FTransform& Frame) const
{
    for (const FSCPlayerKit::FPart& Part : Data->Parts)
    {
        if (Part.Id.IsEmpty() || Part.EntityName != Port.OwnerEntity) continue;
        const FSCInstanceHelpers* Helpers = Kit->InstanceHelpers.Find(Part.Id);
        const FTransform* Helper = Helpers ? Helpers->Frames.Find(Port.AttachBone) : nullptr;
        USceneComponent* Owner = Kit->InstanceAnchors.FindRef(Part.Id);
        if (Helper && Owner)
        {
            Parent = Owner;
            Socket = NAME_None;
            Frame = *Helper;
            return;
        }
    }
    Parent = Leader;
    Socket = FName(*Port.AttachBone);
    Frame = FTransform::Identity;
}
void USCPlayerBodyComponent::PlaceWeapon(int32 WeaponIndex, bool bInHand)
{
    const FSCPlayerWeapon& W = Data->Weapons[WeaponIndex];
    USceneComponent* Anchor = Kit->InstanceAnchors.FindRef(W.ChildId);
    const FSCItemPort* Port = bInHand ? Data->HandPortFor(W) : &W.StowPort;
    if (!Anchor || !Port) return;
    // In hand the grip's hand helper meets the hand joint; stowed, the port's item offset helper meets the stow joint.
    FString Helper = bInHand ? W.HandGripHelper : Port->ItemOffsetHelper;
    const FSCInstanceHelpers* Helpers = Kit->InstanceHelpers.Find(W.ChildId);
    if (Helpers && !Helpers->Frames.Contains(Helper)) Helper.Reset(); // grip helper absent from the mesh: its source frame
    FTransform Relative;
    if (!Kit->PortRelative(W.ChildId, Helper, Port->Offset, Relative))
    {
        UE_LOG(LogSCPlayer, Warning, TEXT("PLAYER_WEAPON_PLACE weapon=%s helper=%s unresolved"), *W.Entity, *Helper);
        return;
    }
    USceneComponent* Parent = nullptr;
    FName Socket;
    FTransform Frame;
    ResolvePortFrame(*Port, Parent, Socket, Frame);
    Anchor->AttachToComponent(Parent, FAttachmentTransformRules::KeepRelativeTransform, Socket);
    Anchor->SetRelativeTransform(Relative * Frame);
    WeaponAttach.Add(WeaponIndex, FName(*Port->AttachBone));
    UE_LOG(LogSCPlayer, Display, TEXT("PLAYER_WEAPON_PLACE weapon=%s in_hand=%d port=%s frame=%s helper=%s"), *W.Entity, bInHand, *Port->AttachBone,
           Socket.IsNone() ? *(Port->OwnerEntity + TEXT(" helper")) : TEXT("body joint"), Helper.IsEmpty() ? TEXT("<source frame>") : *Helper);
}
FSCAnimatorInput USCPlayerBodyComponent::MakeInput(float DeltaSeconds) const
{
    FSCAnimatorInput In;
    ASCFirstPersonCharacter* C = Character();
    In.DeltaSeconds = DeltaSeconds;
    In.bFirstPerson = !bThirdPerson;
    In.bCrouched = C->bIsCrouched;
    In.bFast = C->IsRunning();
    const FVector Local = C->GetActorRotation().UnrotateVector(C->GetVelocity());
    In.Speed = C->bNoclip ? 0.f : float(FVector2D(Local).Size() / 100.0);
    In.TravelAngle = In.Speed >= FSCPlayerAnimator::MoveThreshold ? float(FMath::Atan2(-Local.Y, Local.X)) : 0.f;
    In.PitchDegrees = float(FRotator::NormalizeAxis(C->GetViewRotation().Pitch));
    if (Weapons.GetState() != ESCWeaponState::Unarmed && Slots.IsValidIndex(Weapons.GetActive()))
    {
        const FSCPlayerWeapon& W = Data->Weapons[Slots[Weapons.GetActive()]];
        In.WeaponChildId = W.ChildId;
        const FSCItemPort* Hand = Data->HandPortFor(W);
        const FSCItemPort* Off = Data->HandPorts.FindByPredicate([&](const FSCItemPort& P) { return &P != Hand; });
        const FSCInstanceHelpers* Helpers = Kit->InstanceHelpers.Find(W.ChildId);
        const FTransform* Grip = Helpers && !W.OffHandGripHelper.IsEmpty() ? Helpers->Frames.Find(W.OffHandGripHelper) : nullptr;
        USceneComponent* Anchor = Kit->InstanceAnchors.FindRef(W.ChildId);
        if (Hand && Off && Grip && Anchor)
        {
            In.HandBone = Animator.Bone(Hand->AttachBone);
            In.OffHandBone = Animator.Bone(Off->AttachBone);
            In.OffHandTargetInHand = *Grip * Anchor->GetRelativeTransform(); // helper -> anchor -> hand joint
            In.OffHandWeight = Weapons.GetState() == ESCWeaponState::Armed && CVarOffHandIK.GetValueOnGameThread() ? 1.f : 0.f;
        }
    }
    return In;
}
void USCPlayerBodyComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    Step(DeltaTime);
}
void USCPlayerBodyComponent::Step(float DeltaSeconds)
{
    ASCFirstPersonCharacter* C = Character();
    if (!Kit || !Leader || !C || !IsValid(Kit)) return;
    LastDelta = DeltaSeconds;
    switch (Weapons.Tick(DeltaSeconds))
    {
    case FSCWeaponStateMachine::EEvent::Selected:
        UE_LOG(LogSCPlayer, Display, TEXT("PLAYER_WEAPON state=Armed weapon=%s"), *SlotWeapon(Weapons.GetActive())->Entity);
        break;
    case FSCWeaponStateMachine::EEvent::Holstered:
    {
        // The deselect has ended: the weapon goes back to its stow port (aligned by its item offset helper).
        int32 Previous = INDEX_NONE;
        for (int32 S = 0; S < Slots.Num(); ++S)
            if (WeaponParentBone(S) == FName(*Data->HandPortFor(Data->Weapons[Slots[S]])->AttachBone)) Previous = S;
        if (Previous != INDEX_NONE) PlaceWeapon(Slots[Previous], false);
        UE_LOG(LogSCPlayer, Display, TEXT("PLAYER_WEAPON state=Unarmed stowed=%s"), Previous != INDEX_NONE ? *SlotWeapon(Previous)->Entity : TEXT("-"));
        UpdateProfile();
        if (Weapons.GetQueued() != INDEX_NONE) BeginSelect(Weapons.GetQueued());
        break;
    }
    default: break;
    }
    Animator.Tick(MakeInput(DeltaSeconds));
    Leader->LocalPose = Animator.GetPose();
    Leader->RefreshBoneTransforms();
    Leader->RefreshFollowerComponents(); // leader-pose followers skin with the leader's buffer: mark them dirty
    for (auto& Pair : CopyMaps)
    {
        USCRigComponent* Rig = Pair.Key;
        if (Rig->LocalPose.Num() != Pair.Value.Num()) Rig->LocalPose = Rig->GetSkeletalMeshAsset()->GetRefSkeleton().GetRefBonePose();
        for (int32 B = 0; B < Pair.Value.Num(); ++B)
            if (Pair.Value[B] != INDEX_NONE) Rig->LocalPose[B] = Leader->LocalPose[Pair.Value[B]];
        Rig->RefreshBoneTransforms();
    }
    // Crouching shrinks the capsule about its bottom: keep the feet on the floor.
    Kit->GetRootComponent()->SetRelativeLocation(FVector(0, 0, -C->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()));
    ApplyView();
}
void USCPlayerBodyComponent::ApplyView()
{
    ASCFirstPersonCharacter* C = Character();
    const FTransform Capsule = C->GetCapsuleComponent()->GetComponentTransform();
    const FVector HeadWorld = Leader->GetSocketLocation(FName(*Data->CameraBone));
    // First person: head parts are hidden; those that cast no shadow leave the scene altogether (no bone upload).
    for (UPrimitiveComponent* P : HeadParts)
        if (P)
        {
            P->SetHiddenInGame(!bThirdPerson);
            P->SetVisibility(bThirdPerson || P->bCastHiddenShadow);
        }
    if (!bThirdPerson)
    {
        // The camera joint leans ahead of the capsule (armed stance, spine pitch): never let it enter geometry the
        // capsule is touching. Sweep from the capsule axis at eye height to the joint, keeping the near plane clear.
        const FVector Axis = Capsule.TransformPosition(FVector(0, 0, Capsule.InverseTransformPosition(HeadWorld).Z));
        FHitResult Hit;
        FCollisionQueryParams Params(SCENE_QUERY_STAT(SCEyeClamp), false, C);
        Params.AddIgnoredActor(Kit);
        const bool bBlocked = GetWorld() && GetWorld()->SweepSingleByChannel(Hit, Axis, HeadWorld, FQuat::Identity, ECC_Camera,
                                                                             FCollisionShape::MakeSphere(NearClip * 2), Params);
        Eye = Capsule.InverseTransformPosition(bBlocked ? Hit.Location : HeadWorld);
        C->Camera->bUsePawnControlRotation = true;
        C->RefreshEye();
        return;
    }
    // Third person: orbit the camera joint at a presentation distance, pulled in by the level's camera channel.
    const FRotator View = C->GetViewRotation() + FRotator(0, OrbitYaw, 0);
    const FVector Desired = HeadWorld - View.Vector() * OrbitDistance;
    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCThirdPerson), false, C);
    Params.AddIgnoredActor(Kit);
    const FVector Location = GetWorld() && GetWorld()->SweepSingleByChannel(Hit, HeadWorld, Desired, FQuat::Identity, ECC_Camera,
                                                                            FCollisionShape::MakeSphere(10), Params)
                                 ? Hit.Location
                                 : Desired;
    Eye = Capsule.InverseTransformPosition(Location);
    C->Camera->bUsePawnControlRotation = false;
    C->RefreshEye();
    C->Camera->SetWorldRotation(View);
}
void USCPlayerBodyComponent::SetThirdPerson(bool bOn, float InOrbitYaw, float InOrbitDistance)
{
    bThirdPerson = bOn;
    OrbitYaw = InOrbitYaw;
    if (InOrbitDistance > 0) OrbitDistance = InOrbitDistance;
    UE_LOG(LogSCPlayer, Display, TEXT("PLAYER_VIEW mode=%s orbit_yaw=%.0f distance=%.0f"), bOn ? TEXT("tp") : TEXT("fp"), OrbitYaw, OrbitDistance);
    if (Kit) ApplyView();
}
void USCPlayerBodyComponent::BeginSelect(int32 Slot)
{
    FSCAnimatorInput In = MakeInput(0);
    In.WeaponChildId = Data->Weapons[Slots[Slot]].ChildId;
    const float Seconds = Animator.PlayOneShot(FSCPlayerAnimator::SelectFragment, In);
    // The weapon is on the hand port for the whole select (the clip carries the hand joint to it).
    PlaceWeapon(Slots[Slot], true);
    Weapons.BeginSelect(Slot, Seconds);
    UpdateProfile();
    UE_LOG(LogSCPlayer, Display, TEXT("PLAYER_WEAPON state=Selecting weapon=%s seconds=%.3f view=%s"), *Data->Weapons[Slots[Slot]].Entity, Seconds,
           bThirdPerson ? TEXT("tp") : TEXT("fp"));
}
void USCPlayerBodyComponent::BeginDeselect(int32 Then)
{
    const float Seconds = Animator.PlayOneShot(FSCPlayerAnimator::DeselectFragment, MakeInput(0));
    Weapons.BeginDeselect(Seconds, Then);
    UE_LOG(LogSCPlayer, Display, TEXT("PLAYER_WEAPON state=Deselecting weapon=%s seconds=%.3f then=%d"), *SlotWeapon(Weapons.GetActive())->Entity,
           Seconds, Weapons.GetQueued() + 1);
}
bool USCPlayerBodyComponent::RequestSlot(int32 Slot)
{
    if (!Kit || (Slot != INDEX_NONE && !Slots.IsValidIndex(Slot))) return false;
    switch (Weapons.Request(Slot))
    {
    case ESCWeaponAction::Select: BeginSelect(Slot); return true;
    case ESCWeaponAction::Deselect: BeginDeselect(Slot == Weapons.GetActive() ? INDEX_NONE : Slot); return true;
    default:
        UE_LOG(LogSCPlayer, Display, TEXT("PLAYER_WEAPON ignored request=%d state=%s"), Slot + 1, FSCWeaponStateMachine::Name(Weapons.GetState()));
        return false;
    }
}
void USCPlayerBodyComponent::UpdateProfile()
{
    ASCFirstPersonCharacter* C = Character();
    const FString Weapon = Weapons.GetState() == ESCWeaponState::Unarmed ? FString() : Data->Weapons[Slots[Weapons.GetActive()]].ChildId;
    if (!C || Weapon == ProfileWeapon) return;
    ProfileWeapon = Weapon;
    float Slow = 0, Fast = 0;
    Animator.LocomotionSpeeds(Weapon, true, Slow, Fast);
    if (Slow <= 0 || Fast <= 0) return;
    // Walk / run speeds come from the blend-space examples of the slowest / fastest move-speed tag.
    FSCCharacterProfile Profile = C->GetProfile();
    Profile.WalkSpeed = Slow * 100;
    Profile.RunSpeed = Fast * 100;
    C->SetProfile(Profile);
    UE_LOG(LogSCPlayer, Display, TEXT("PLAYER_PROFILE weapon=%s walk_cm_s=%.0f run_cm_s=%.0f"), Weapon.IsEmpty() ? TEXT("-") : *Weapon,
           Profile.WalkSpeed, Profile.RunSpeed);
}
FString USCPlayerBodyComponent::HUDLine() const
{
    if (!Kit) return FString();
    const FSCPlayerWeapon* W = SlotWeapon(Weapons.GetActive());
    const FString Keys = Slots.IsEmpty() ? TEXT("no stowed weapons") : FString::Printf(TEXT("1-%d draw · H holster"), FMath::Min(Slots.Num(), 9));
    return FString::Printf(TEXT("%s%s · %s · C %s view"), FSCWeaponStateMachine::Name(Weapons.GetState()),
                           W ? *(TEXT(" ") + W->Entity) : TEXT(""), *Keys, bThirdPerson ? TEXT("first-person") : TEXT("third-person"));
}
FString USCPlayerBodyComponent::Describe() const
{
    if (!Kit) return TEXT("body=none");
    const FSCPlayerWeapon* W = SlotWeapon(Weapons.GetActive());
    return FString::Printf(TEXT("weapon_state=%s weapon=%s parent=%s view=%s oneshot=%.2f offhand_error_cm=%.1f eye=%s %s"),
                           FSCWeaponStateMachine::Name(Weapons.GetState()), W ? *W->Entity : TEXT("-"),
                           *WeaponParentBone(Weapons.GetActive()).ToString(), bThirdPerson ? TEXT("tp") : TEXT("fp"), Animator.OneShotProgress(),
                           Animator.LastOffHandError(), *Eye.ToCompactString(), *Animator.Describe());
}
