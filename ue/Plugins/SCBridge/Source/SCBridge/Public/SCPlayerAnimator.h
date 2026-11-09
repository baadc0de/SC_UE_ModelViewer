// WP-P2b: plain, unit-testable player animator (no UObjects). Samples body clip sidecars, 2-D Mannequin blend
// spaces with phase-synchronised examples, cross-fades fragments with their exported blend times, layers additive
// and one-shot (select/deselect) fragments, pins the root-motion joint, distributes view pitch over the spine and
// solves the off-hand two-bone IK. Output: a local pose for the body skeleton (USCRigComponent::LocalPose).
#pragma once
#include "CoreMinimal.h"
#include "SCPlayerKit.h"

struct FReferenceSkeleton;

/** One body clip, keys converted to UE local space: absolute locals, or deltas for additive clips. */
struct SCBRIDGE_API FSCPlayerClip
{
    struct FTrack
    {
        int32 Bone = INDEX_NONE;
        TArray<float> RotTimes, PosTimes; // frames
        TArray<FQuat4f> Rot;
        TArray<FVector3f> Pos;
    };
    FName Name;
    float Fps = 30, Frames = 0, Duration = 0;
    bool bAdditive = false, bCyclic = false;
    TArray<FTrack> Tracks;

    /** BoneIndex maps a sidecar joint name to the body skeleton (INDEX_NONE: not routed). */
    static bool Parse(const FString& Json, TFunctionRef<int32(const FString&)> BoneIndex, FSCPlayerClip& Out, FString& Error);
    /** Absolute clip: overwrite the tracked channels at Seconds (clamped to the clip). */
    void Sample(float Seconds, TArray<FTransform>& Pose) const;
    /** Override the tracked channels with weight Alpha (sparse layer). */
    void Overlay(float Seconds, float Alpha, TArray<FTransform>& Pose) const;
    /** Additive clip: local = delta (parent space) composed with the base; translation adds. */
    void ApplyAdditive(float Seconds, float Alpha, TArray<FTransform>& Pose) const;
};
using FSCPlayerClipMap = TMap<FName, TSharedPtr<const FSCPlayerClip>>;

class FSCPackageSource;
/** Worker-safe: parse the player block of ScenePath and every clip its playable layers reference, routed to the
 *  body skeleton by joint name (Bones: leader rig bone name -> index). Unreadable clips are counted in Failed and
 *  left out (the animator skips layers without a clip). */
SCBRIDGE_API bool SCLoadPlayerKit(const FSCPackageSource& Source, const FString& ScenePath, const TMap<FString, int32>& Bones,
                                  TSharedPtr<FSCPlayerKit>& Kit, TSharedPtr<FSCPlayerClipMap>& Clips, int32& Failed, FString& Error);

/** Blend-space weights: closest point of the authored faces in dimension-normalised parameter space. */
struct SCBRIDGE_API FSCBlendSampler
{
    /** Params indexed like Space.Dimensions. Weights are non-negative and sum to 1 (empty if no usable face). */
    static void Weights(const FSCBlendSpace& Space, TConstArrayView<float> Params, TArray<TPair<int32, float>>& Out);
    /** Input vector for a 2-D locomotion space: MoveSpeed (m/s) and TravelAngle (rad, left positive); other dims 0. */
    static TArray<float> LocomotionParams(const FSCBlendSpace& Space, float Speed, float TravelAngle);
    /** Highest forward (TravelAngle ~ 0) example speed inside the authored MoveSpeed grid; 0 if none. */
    static float NominalForwardSpeed(const FSCBlendSpace& Space);
    /** Whether TravelAngle lies within the angles the space's examples cover. */
    static bool CoversAngle(const FSCBlendSpace& Space, float TravelAngle);
};

enum class ESCWeaponState : uint8
{
    Unarmed,
    Selecting,
    Armed,
    Deselecting
};
enum class ESCWeaponAction : uint8
{
    None,
    Select,
    Deselect
};
/** Unarmed -> Selecting -> Armed -> Deselecting -> Unarmed. Requests are ignored mid-transition. */
class SCBRIDGE_API FSCWeaponStateMachine
{
public:
    /** What a request for weapon Index (INDEX_NONE: holster) does now. Armed + another index: deselect first,
     *  the other weapon is selected when the deselect ends (Queued). Never changes state. */
    ESCWeaponAction Request(int32 Index) const;
    void BeginSelect(int32 Index, float Seconds);
    void BeginDeselect(float Seconds, int32 Then = INDEX_NONE);
    enum class EEvent : uint8 { None, Selected, Holstered };
    EEvent Tick(float DeltaSeconds);
    ESCWeaponState GetState() const { return State; }
    int32 GetActive() const { return Active; }
    int32 GetQueued() const { return Queued; }
    float GetElapsed() const { return Elapsed; }
    float GetDuration() const { return Duration; }
    static const TCHAR* Name(ESCWeaponState State);
private:
    ESCWeaponState State = ESCWeaponState::Unarmed;
    int32 Active = INDEX_NONE, Queued = INDEX_NONE;
    float Elapsed = 0, Duration = 0;
};

struct SCBRIDGE_API FSCAnimatorInput
{
    FString WeaponChildId; // weapon in hand (selecting/armed/deselecting); empty: unarmed
    bool bFirstPerson = true, bCrouched = false, bFast = false;
    float Speed = 0;       // ground speed, m/s
    float TravelAngle = 0; // rad relative to facing, left positive (Mannequin TravelAngle)
    float PitchDegrees = 0; // view pitch, up positive
    float DeltaSeconds = 0;
    /** Off-hand IK: weapon helper relative to the hand-port joint; Weight 0 disables. */
    float OffHandWeight = 0;
    int32 HandBone = INDEX_NONE, OffHandBone = INDEX_NONE;
    FTransform OffHandTargetInHand = FTransform::Identity;
};

class SCBRIDGE_API FSCPlayerAnimator
{
public:
    /** Stance tags the two capsule states request (Mannequin PlayerTags vocabulary). */
    static const FString StandTag, CrouchTag;
    /** Fragment ids the animator requests (Mannequin PlayerActions vocabulary). */
    static const FName IdleFragment, MoveFragment, SelectFragment, DeselectFragment;
    /** Below this ground speed (m/s) the idle fragment plays (viewer threshold, not game data). */
    static constexpr float MoveThreshold = 0.05f;

    bool Init(const FSCPlayerKit& Kit, const FReferenceSkeleton& Ref, FSCPlayerClipMap Clips, FString& Error);
    bool IsReady() const { return !Pose.IsEmpty(); }
    void Tick(const FSCAnimatorInput& Input);
    const TArray<FTransform>& GetPose() const { return Pose; }
    /** Component-space transform of Bone in the current pose. */
    FTransform ComponentSpace(int32 Bone) const;
    /** Start a one-shot fragment (select/deselect) of the state Input resolves to; returns its length in seconds
     *  (0: the state has no playable fragment). */
    float PlayOneShot(FName Fragment, const FSCAnimatorInput& Input);
    float OneShotProgress() const;
    /** Walk/run speeds (m/s) for a weapon: nominal forward speeds of the slowest / fastest speed tag. */
    void LocomotionSpeeds(const FString& WeaponChildId, bool bFirstPerson, float& Slow, float& Fast) const;
    /** Character facing in component space (the root-motion joint's native +Y). */
    FVector Forward() const { return ForwardCS; }
    int32 Bone(const FString& Name) const;
    const TArray<int32>& PitchChain() const { return SpineChain; }
    FString Describe() const { return Debug; }
    float LastOffHandError() const { return OffHandError; }

private:
    struct FSource
    {
        const FSCMotionFragment* Fragment = nullptr;
        FName Key;          // clip or blend space of the base layer
        float Time = 0;     // clip seconds, or blend-space phase 0..1
        float Weight = 0, FadeRate = 0;
        bool bFadingIn = false;
    };
    struct FOneShot
    {
        const FSCMotionFragment* Fragment = nullptr;
        float Time = 0, Duration = 0;
    };
    const FSCMotionState* ResolveState(const FSCAnimatorInput& Input, bool* bFastOut = nullptr) const;
    void SpeedTags(const FString& Weapon, const FString& Stance, bool bFirstPerson, FString& Slow, FString& Fast) const;
    const FSCMotionLayer* BaseLayer(const FSCMotionFragment& F) const;
    void EvaluateSource(FSource& S, const FSCAnimatorInput& Input, TArray<FTransform>& Out, bool bAdvance);
    void ApplyPitch(float Degrees);
    void SolveOffHand(const FSCAnimatorInput& Input);
    float Length(const FSCMotionFragment& F) const;
    const FSCPlayerClip* Clip(FName Name) const;

    const FSCPlayerKit* Kit = nullptr;
    FSCPlayerClipMap Clips;
    TArray<FTransform> RefPose, Pose, Scratch, Scratch2;
    TArray<int32> Parents, SpineChain;
    TMap<FString, int32> BoneIndex;
    int32 RootMotion = INDEX_NONE;
    FVector ForwardCS = FVector::ForwardVector;
    TArray<FSource> Sources;
    FOneShot OneShot;
    float OffHandBlend = 0, OffHandError = -1;
    FString Debug;
};
