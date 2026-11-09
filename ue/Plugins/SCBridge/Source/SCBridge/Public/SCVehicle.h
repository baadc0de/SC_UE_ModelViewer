#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SCPackageReader.h"
#include "SCVehicle.generated.h"
class UTexture2D;
class UMaterialInstanceDynamic;

UCLASS(BlueprintType)
class SCBRIDGE_API USCClipCatalog : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintReadOnly, Category="SC") TArray<FSCClipRecord> Clips;
};
UCLASS(BlueprintType)
class SCBRIDGE_API ASCVehicle : public AActor
{
    GENERATED_BODY()
public:
    ASCVehicle();
    UPROPERTY(BlueprintReadOnly, Category="SC") TArray<TObjectPtr<UStaticMesh>> Meshes;
    /** Transient textures (deduplicated by source path + role) and material instances owned by this import. */
    UPROPERTY(BlueprintReadOnly, Category="SC") TArray<TObjectPtr<UTexture2D>> Textures;
    UPROPERTY(BlueprintReadOnly, Category="SC") TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;
    /** Instances driven by scene controls.engine_glow (main thrusters). */
    UPROPERTY(BlueprintReadOnly, Category="SC") TArray<TObjectPtr<UMaterialInstanceDynamic>> EngineGlowMaterials;
    /** Sets the main-thruster emission strength, clamped to the manifest's controls.engine_glow range. */
    UFUNCTION(BlueprintCallable, Category="SC") void SetEngineGlow(float Strength);
    UPROPERTY(BlueprintReadOnly, Category="SC") TObjectPtr<USCClipCatalog> ClipCatalog;
    UPROPERTY(BlueprintReadOnly, Category="SC") FSCSceneManifest Manifest;
    UPROPERTY(BlueprintReadOnly, Category="SC") FString SessionPath;
};
