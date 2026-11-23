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
    /** The collision body from serialized Chaos trimesh bytes (FSCSkeletalMeshImporter::DeriveCollision's, in LOD 0's
     *  render vertex order). An editor host keeps them in SavedCollision, so a saved rig restores its body when it
     *  registers after a load (no derivation at load). bConfigure: also the import's collision policy (BlockAll, query
     *  only, per-poly); false keeps the component's profile and channel responses (a restored or replaced body). */
    bool InitializeCollision(const TArray<uint8>& Bytes, const FCustomVersionContainer& Versions, bool bConfigure = true,
                             bool bFindSkinBones = true);
    /** The body from SavedCollision (OnRegister calls it); false when there is none or it does not read. */
    bool RestoreSavedCollision();
    bool HasCollisionBody() const { return CollisionBody != nullptr; }
    /** The bones that weight a soft vertex (the ones that can deform the body). */
    const TArray<int32>& GetSkinBones() const { return SkinBones; }
    /** CPU-skins LOD0 with the current pose and refits the per-instance triangle body. Independent of
     *  the render mesh object, so null-RHI and GPU hosts run the same path. Skips unchanged skin bones. */
    bool RefitCollision();
    /** Particle positions of the live trimesh body (component space, external vertex order). */
    bool ReadCollisionVertices(TArray<FVector3f>& Out) const;
    int32 GetCollisionRefits() const { return CollisionRefits; }
    /** The component-space pose the collision bytes are in (WP-O1-4b: the prep bakes them in the rig's saved pose, so a
     *  load refits nothing); empty: the bind pose (the import's bytes). */
    UPROPERTY() TArray<FTransform> CollisionPose;
    /** LocalPose composed to component space, as RefreshBoneTransforms poses the bones; false without a mesh or with a
     *  pose of another size. */
    bool ComposeLocalPose(TArray<FTransform>& Out) const;
    virtual UBodySetup* GetBodySetup() override;
    virtual FBodyInstance* GetBodyInstance(FName BoneName = NAME_None, bool bGetWelded = true, int32 Index = INDEX_NONE) const override;
    virtual bool HasValidPhysicsState() const override;
    /** Never for a rig with its own body (saved or built): the engine's asynchronous path would create the stock body
     *  before OnRegister restores this one; OnCreatePhysicsState installs it. */
    virtual bool AllowsAsyncPhysicsStateCreation() const override;
    /** The engine's anim init may set the reference pose (registration, InitializeComponent): the rig's own pose
     *  (LocalPose, e.g. a hangar saved open) is applied again after it. */
    virtual void InitAnim(bool bForceReinit) override;
    /** The collision bytes with their custom versions (WP-O1-4b: kept on editor hosts, saved with a prepared level). */
    UPROPERTY() TArray<uint8> SavedCollision;
protected:
    virtual void OnRegister() override;
    virtual void OnCreatePhysicsState() override;
    virtual void OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport) override;
    /** Built from bytes (the Chaos trimesh is not saved with a body setup): never saved; a saved rig restores it from
     *  SavedCollision. */
    UPROPERTY(Transient) TObjectPtr<UBodySetup> CollisionBody;
private:
    bool SkinPoseChanged() const;
    void ResetRefitPose(); // RefitPose: CollisionPose, or the bind pose
    UPROPERTY() TArray<int32> SkinBones;  // bones with non-zero influence on the soft vertices (saved: a load does not scan)
    TArray<FTransform> RefitPose;         // component-space pose the body currently represents
    int32 CollisionRefits = 0;
    bool bRefitUnavailable = false;
};
