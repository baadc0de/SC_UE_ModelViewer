#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SCPackageReader.h"
#include "SCVehicle.generated.h"

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
    UPROPERTY(BlueprintReadOnly, Category="SC") TObjectPtr<USCClipCatalog> ClipCatalog;
    UPROPERTY(BlueprintReadOnly, Category="SC") FSCSceneManifest Manifest;
    UPROPERTY(BlueprintReadOnly, Category="SC") FString SessionPath;
};
