#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SCPackageReader.h"
#include "SCClipCatalog.h"
#include "SCVehicle.generated.h"
class UTexture2D;
class UMaterialInstanceDynamic;

class USCRigComponent;
class USkeletalMesh;
class ULightComponent;
struct FSCClipPlayback
{
    int32 Clip = INDEX_NONE;
    double Frame = 0;
    float Speed = 1;
    bool bPlaying = true;
};
UCLASS(BlueprintType)
class SCBRIDGE_API ASCVehicle : public AActor
{
    GENERATED_BODY()
public:
    ASCVehicle();
    virtual void Tick(float DeltaSeconds) override;
    UPROPERTY(BlueprintReadOnly, Category="SC") TArray<TObjectPtr<USkeletalMesh>> SkeletalMeshes;
    UPROPERTY(BlueprintReadOnly, Category="SC") TArray<TObjectPtr<USCRigComponent>> Rigs;
    UPROPERTY(BlueprintReadOnly, Category="SC") TArray<TObjectPtr<ULightComponent>> Lights;
    UPROPERTY() TMap<FString, TObjectPtr<UTexture2D>> GoboTextures;
    UFUNCTION(BlueprintCallable, Category="SC|Animation") bool PlayClip(FName Clip);
    UFUNCTION(BlueprintCallable, Category="SC|Animation") bool SnapClip(FName Clip, bool bLast);
    UFUNCTION(BlueprintCallable, Category="SC|Animation") bool PlayFragment(FName Fragment, FName Tag);
    UFUNCTION(BlueprintCallable, Category="SC|Animation") void ResetPose();
    void EvaluateClips();
    TArray<FSCClipPlayback> Playback;

    UPROPERTY(BlueprintReadOnly, Category="SC") TArray<TObjectPtr<UStaticMesh>> Meshes;
    /** Transient textures (deduplicated by source path + role) and material instances owned by this import. */
    UPROPERTY(BlueprintReadOnly, Category="SC") TArray<TObjectPtr<UTexture2D>> Textures;
    UPROPERTY(BlueprintReadOnly, Category="SC") TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;
    /** Instances driven by scene controls.engine_glow (main thrusters). */
    UPROPERTY(BlueprintReadOnly, Category="SC") TArray<TObjectPtr<UMaterialInstanceDynamic>> EngineGlowMaterials;
    /** Sets the engine-glow control value (package units, clamped to its range); see EngineGlowToEmissive. */
    UFUNCTION(BlueprintCallable, Category="SC") void SetEngineGlow(float Strength);
    UPROPERTY(BlueprintReadOnly, Category="SC") TObjectPtr<USCClipCatalog> ClipCatalog;
    UPROPERTY(BlueprintReadOnly, Category="SC") FSCSceneManifest Manifest;
    UPROPERTY(BlueprintReadOnly, Category="SC") FString SessionPath;
};
