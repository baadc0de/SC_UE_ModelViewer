// Data and runtime bindings for the Ship Link thruster visual preview.
#pragma once

#include "CoreMinimal.h"
#include "SCThrusterPreview.generated.h"

class UNiagaraComponent;
class USceneComponent;
class USCRigComponent;
class UMaterialInstanceDynamic;

UENUM(BlueprintType)
enum class ESCThrusterPreviewMode : uint8
{
    Off,
    Minimum,
    Full,
    Boosted
};

USTRUCT()
struct FSCThrusterEffect
{
    GENERATED_BODY()

    UPROPERTY() FString Reference;
    UPROPERTY() FString Bone;
    UPROPERTY() FTransform Offset;
    UPROPERTY() FVector2D Strength = FVector2D(0, 1);
    UPROPERTY() bool bBoost = false;
    UPROPERTY() bool bParent = false;
};

USTRUCT()
struct FSCThrusterEmitter
{
    GENERATED_BODY()

    UPROPERTY() FSCThrusterEffect Source;
    UPROPERTY() TObjectPtr<USceneComponent> Mount;
    UPROPERTY() FName Socket;
    UPROPERTY(Transient) TObjectPtr<UNiagaraComponent> Component;
};

USTRUCT()
struct FSCThrusterBinding
{
    GENERATED_BODY()

    UPROPERTY() TArray<FSCThrusterEmitter> Emitters;
    UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;
    UPROPERTY() TObjectPtr<USCRigComponent> Rig;
    UPROPERTY() FString NozzleAnimation;
    UPROPERTY() FVector Glow = FVector::ZeroVector;
    UPROPERTY() bool bHasGlow = false;
    UPROPERTY() bool bManeuvering = false;
};
