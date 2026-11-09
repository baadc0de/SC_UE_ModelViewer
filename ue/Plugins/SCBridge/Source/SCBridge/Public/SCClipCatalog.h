#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SCClipCatalog.generated.h"
class UAnimSequence;
class USkeletalMesh;

USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCClipFragment
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FName Fragment;
    UPROPERTY(BlueprintReadOnly) TArray<FName> Tags;
    UPROPERTY(BlueprintReadOnly) TArray<FName> FragmentTags;
    UPROPERTY(BlueprintReadOnly) TArray<FName> Scopes;
    UPROPERTY(BlueprintReadOnly) TArray<FName> AnimationNames;
    UPROPERTY(BlueprintReadOnly) float Speed = 1;
    UPROPERTY(BlueprintReadOnly) FString Json;
};
USTRUCT()
struct SCBRIDGE_API FSCBoneTrack
{
    GENERATED_BODY()
    UPROPERTY() int32 Bone = INDEX_NONE;
    UPROPERTY() bool bRotation = false;
    UPROPERTY() bool bPosition = false;
    UPROPERTY() TArray<FTransform> Keys; // uniform source frames, including the literal final frame
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCClipBinding
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) TObjectPtr<USkeletalMesh> Mesh;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<UAnimSequence> Sequence;
    UPROPERTY() TArray<FSCBoneTrack> Tracks; // also provides a non-editor runtime evaluation path
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCImportedClip
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FName Id;
    UPROPERTY(BlueprintReadOnly) FString Name;
    UPROPERTY(BlueprintReadOnly) FString DisplayName;
    UPROPERTY(BlueprintReadOnly) FString Owner;
    UPROPERTY(BlueprintReadOnly) FString Source;
    UPROPERTY(BlueprintReadOnly) TArray<FString> Contributors;
    UPROPERTY(BlueprintReadOnly) TArray<FSCClipFragment> Fragments;
    UPROPERTY(BlueprintReadOnly) bool bBoomerang = false;
    UPROPERTY(BlueprintReadOnly) float Fps = 30;
    UPROPERTY(BlueprintReadOnly) int32 LastFrame = 0;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<UAnimSequence> Sequence; // primary rig, or empty metadata-only sequence if no receiver
    UPROPERTY(BlueprintReadOnly) TArray<FSCClipBinding> Bindings;
};
UCLASS(BlueprintType)
class SCBRIDGE_API USCClipCatalog : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintReadOnly, Category="SC") TArray<FSCImportedClip> Clips;
};
