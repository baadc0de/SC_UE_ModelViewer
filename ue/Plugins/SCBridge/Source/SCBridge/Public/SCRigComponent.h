// Runtime rig component with explicit sparse local-pose evaluation.
#pragma once
#include "CoreMinimal.h"
#include "Components/SkeletalMeshComponent.h"
#include "SCRigComponent.generated.h"

struct FSCDerivedMesh;

/** Sparse clip composition drives this skeletal component in both game and editor worlds. */
UCLASS()
class SCBRIDGE_API USCRigComponent : public USkeletalMeshComponent
{
    GENERATED_BODY()
public:
    USCRigComponent();
    UPROPERTY() TArray<FTransform> LocalPose;
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString MeshAsset;
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString OwnerEntity;
    virtual void RefreshBoneTransforms(FActorComponentTickFunction* TickFunction = nullptr) override;
    void ResetPose();
    bool InitializeCollision(const FSCDerivedMesh& Data);
    /** CPU-skins LOD0 with the current pose and refits the per-instance triangle body. Independent of
     *  the render mesh object, so null-RHI and GPU hosts run the same path. Skips unchanged skin bones. */
    bool RefitCollision();
    /** Particle positions of the live trimesh body (component space, external vertex order). */
    bool ReadCollisionVertices(TArray<FVector3f>& Out) const;
    int32 GetCollisionRefits() const { return CollisionRefits; }
    virtual UBodySetup* GetBodySetup() override;
    virtual FBodyInstance* GetBodyInstance(FName BoneName = NAME_None, bool bGetWelded = true, int32 Index = INDEX_NONE) const override;
    virtual bool HasValidPhysicsState() const override;
protected:
    virtual void OnCreatePhysicsState() override;
    virtual void OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport) override;
    UPROPERTY() TObjectPtr<UBodySetup> CollisionBody;
private:
    bool SkinPoseChanged() const;
    TArray<int32> SkinBones;              // bones with non-zero influence on the soft vertices
    TArray<FTransform> RefitPose;         // component-space pose the body currently represents
    int32 CollisionRefits = 0;
    bool bRefitUnavailable = false;
};
