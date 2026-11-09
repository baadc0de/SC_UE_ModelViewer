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
    MarkRenderTransformDirty();
    MarkRenderDynamicDataDirty();
}
