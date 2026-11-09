// Runtime rig component with explicit sparse local-pose evaluation.
#pragma once
#include "CoreMinimal.h"
#include "Components/SkeletalMeshComponent.h"
#include "SCRigComponent.generated.h"

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
};
