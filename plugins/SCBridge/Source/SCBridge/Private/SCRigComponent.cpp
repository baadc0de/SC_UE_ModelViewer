// Sparse local-pose evaluation and socket-child propagation for imported rigs.
#include "SCRigComponent.h"
#include "Engine/SkeletalMesh.h"
USCRigComponent::USCRigComponent()
{
    SetAnimationMode(EAnimationMode::AnimationCustomMode);
    PrimaryComponentTick.bCanEverTick = false; // ASCVehicle evaluates once, then updates socket children.
    bEnableUpdateRateOptimizations = false;
    VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
}
void USCRigComponent::ResetPose()
{
    if (GetSkeletalMeshAsset()) LocalPose = GetSkeletalMeshAsset()->GetRefSkeleton().GetRefBonePose();
    RefreshBoneTransforms();
}
bool USCRigComponent::ComposeLocalPose(TArray<FTransform>& Out) const
{
    const USkeletalMesh* Mesh = GetSkeletalMeshAsset();
    if (!Mesh || LocalPose.Num() != Mesh->GetRefSkeleton().GetNum()) return false;
    const auto& Ref = Mesh->GetRefSkeleton();
    Out.SetNum(LocalPose.Num());
    for (int32 I = 0; I < Out.Num(); ++I)
    {
        const int32 Parent = Ref.GetParentIndex(I);
        Out[I] = Parent == INDEX_NONE ? LocalPose[I] : LocalPose[I] * Out[Parent];
        Out[I].NormalizeRotation();
    }
    return true;
}
void USCRigComponent::RefreshBoneTransforms(FActorComponentTickFunction*)
{
    const USkeletalMesh* Mesh = GetSkeletalMeshAsset();
    if (!Mesh) return;
    const auto& Ref = Mesh->GetRefSkeleton();
    if (LocalPose.Num() != Ref.GetNum()) LocalPose = Ref.GetRefBonePose();
    auto& Space = GetEditableComponentSpaceTransforms();
    if (Space.Num() != Ref.GetNum() || Space.IsEmpty()) return;
    for (int32 I = 0; I < Space.Num(); ++I)
    {
        const int32 Parent = Ref.GetParentIndex(I);
        Space[I] = Parent == INDEX_NONE ? LocalPose[I] : LocalPose[I] * Space[Parent];
        Space[I].NormalizeRotation();
    }
    bNeedToFlipSpaceBaseBuffers = true;
    USkinnedMeshComponent::FinalizeBoneTransform();
    RefitCollision(); // our own CPU skin refit; never the engine's MeshObject-gated per-poly path
    UpdateChildTransforms();
    UpdateBounds();
    // Followers take these bounds (CalcBounds); the engine's follower refresh does not recompute theirs.
    for (const TWeakObjectPtr<USkinnedMeshComponent>& Follower : GetFollowerPoseComponents())
        if (USkinnedMeshComponent* F = Follower.Get()) F->UpdateBounds();
    MarkRenderTransformDirty();
    MarkRenderDynamicDataDirty();
}
const TArray<FTransform>& USCRigComponent::InverseBindPose() const
{
    const USkeletalMesh* Mesh = GetSkeletalMeshAsset();
    if (BindMesh == Mesh) return InverseBind;
    BindMesh = Mesh;
    InverseBind.Reset();
    if (!Mesh) return InverseBind;
    const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
    const TArray<FTransform>& Local = Ref.GetRefBonePose();
    InverseBind.SetNum(Local.Num());
    for (int32 I = 0; I < Local.Num(); ++I)
    {
        const int32 Parent = Ref.GetParentIndex(I);
        InverseBind[I] = Parent == INDEX_NONE ? Local[I] : Local[I] * InverseBind[Parent]; // bind pose, component space
    }
    for (FTransform& Bind : InverseBind) Bind = Bind.Inverse();
    return InverseBind;
}
FBoxSphereBounds USCRigComponent::CalcBounds(const FTransform& LocalToWorld) const
{
    const FBoxSphereBounds Engine = Super::CalcBounds(LocalToWorld);
    // A vertex skinned to bone b moves by b's bind -> posed transform; a blended vertex lies within the hull of its bones'
    // moved positions. So the mesh's bind-pose box moved by every bone's transform, joined, holds the posed mesh: no
    // vertex can leave it however far it sits from its bones (a door panel swung on its hinge). A leader-pose follower is
    // skinned by its leader's component-space bones in its own component space: its own box, moved by the leader's bones.
    const USCRigComponent* Leader = Cast<USCRigComponent>(LeaderPoseComponent.Get());
    const USCRigComponent* Posing = Leader ? Leader : this;
    const USkeletalMesh* Mesh = GetSkeletalMeshAsset();
    const TArray<FTransform>& Space = Posing->GetComponentSpaceTransforms();
    const TArray<FTransform>& Unbind = Posing->InverseBindPose();
    if (!Mesh || Space.IsEmpty() || Space.Num() != Unbind.Num()) return Engine;
    const FBox MeshBox = Mesh->GetImportedBounds().GetBox();
    if (!MeshBox.IsValid) return Engine;
    FBox Posed(ForceInit);
    for (int32 Bone = 0; Bone < Space.Num(); ++Bone) Posed += MeshBox.TransformBy(Unbind[Bone] * Space[Bone]);
    return Engine + FBoxSphereBounds(Posed.TransformBy(LocalToWorld));
}
