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
float USCRigComponent::BonePadding() const
{
    const USkeletalMesh* Mesh = GetSkeletalMeshAsset();
    if (!Mesh) return 0.f;
    if (PaddingMesh == Mesh && CachedBonePadding >= 0.f) return CachedBonePadding;
    const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
    const TArray<FTransform>& Local = Ref.GetRefBonePose();
    TArray<FTransform> Space;
    Space.SetNum(Local.Num());
    FBox Bones(ForceInit);
    for (int32 I = 0; I < Local.Num(); ++I)
    {
        const int32 Parent = Ref.GetParentIndex(I);
        Space[I] = Parent == INDEX_NONE ? Local[I] : Local[I] * Space[Parent];
        Bones += Space[I].GetLocation();
    }
    const FBox MeshBox = Mesh->GetImportedBounds().GetBox();
    float Padding = 0.f;
    if (Bones.IsValid && MeshBox.IsValid)
    {
        const FVector Out = FVector::Max(MeshBox.Max - Bones.Max, Bones.Min - MeshBox.Min);
        Padding = float(FMath::Max(0.0, Out.GetMax()));
    }
    PaddingMesh = Mesh;
    CachedBonePadding = Padding;
    return Padding;
}
FBoxSphereBounds USCRigComponent::CalcBounds(const FTransform& LocalToWorld) const
{
    const FBoxSphereBounds Engine = Super::CalcBounds(LocalToWorld);
    if (const USCRigComponent* Leader = Cast<USCRigComponent>(LeaderPoseComponent.Get()))
        return Leader->IsRegistered() && Leader->Bounds.SphereRadius > 0 ? Engine + Leader->Bounds : Engine;
    const TArray<FTransform>& Space = GetComponentSpaceTransforms();
    const USkeletalMesh* Mesh = GetSkeletalMeshAsset();
    if (!Mesh || Space.Num() != Mesh->GetRefSkeleton().GetNum() || Space.IsEmpty()) return Engine;
    FBox Posed(ForceInit);
    for (const FTransform& Bone : Space) Posed += Bone.GetLocation();
    return Engine + FBoxSphereBounds(Posed.ExpandBy(BonePadding()).TransformBy(LocalToWorld));
}
