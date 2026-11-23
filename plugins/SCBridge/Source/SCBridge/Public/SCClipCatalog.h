// Transient clip metadata and sparse authored channels shared by editor and game playback.
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
    UPROPERTY(BlueprintReadOnly) FString OptionId;
    UPROPERTY(BlueprintReadOnly) float OptionWeight = 1;
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
    UPROPERTY() TArray<FTransform> Keys; // uniform frames of the animation this track came from, including its final frame
    UPROPERTY() float Fps = 0;           // that animation's own rate, always set by the importer; a merged clip's
                                         // tracks can differ in rate and length
};
/** WP-A1: an audio event of the animation a binding plays (clip source `audio_events[]`), at a clip frame. */
USTRUCT()
struct SCBRIDGE_API FSCClipAudioEvent
{
    GENERATED_BODY()
    UPROPERTY() double Frame = 0;   // clip frame (the clip's rate) of the event's normalised time in its own animation
    /** The clip frame of the same moment when the clip plays backward: reverse playback aligns a shorter source's end
     *  with the clip's end (FSCAnimImporter::TrackFrame); -1 = the same as Frame. */
    UPROPERTY() double FrameReverse = -1;
    UPROPERTY() int8 Direction = 1; // 1 forward, -1 reverse, 0 both
    UPROPERTY() FString Trigger;
    UPROPERTY() FString Bone;
    UPROPERTY() FVector Offset = FVector::ZeroVector; // CryEngine axes, metres
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCClipBinding
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) TObjectPtr<USkeletalMesh> Mesh;
    // One per animation rate and length among the tracks: a sequence has one timeline, a merged clip several.
    UPROPERTY(BlueprintReadOnly) TArray<TObjectPtr<UAnimSequence>> Sequences;
    UPROPERTY() TArray<FSCBoneTrack> Tracks; // also provides a non-editor runtime evaluation path
    UPROPERTY() TArray<FSCClipAudioEvent> AudioEvents; // WP-A1: of the source animations this binding's tracks play
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
    UPROPERTY(BlueprintReadOnly) bool bCyclic = false;
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
    UPROPERTY(BlueprintReadOnly, Category = "SC") TArray<FSCImportedClip> Clips;
    /** The vehicle's root rig mesh (the manifest root's, built whether or not it was placed): the skeleton of its clips
     *  without receiving bones (FSCAnimImporter::Finalize); Keep as assets builds them on it. Null without a root rig. */
    UPROPERTY(BlueprintReadOnly, Category = "SC") TObjectPtr<USkeletalMesh> RootMesh;
};
