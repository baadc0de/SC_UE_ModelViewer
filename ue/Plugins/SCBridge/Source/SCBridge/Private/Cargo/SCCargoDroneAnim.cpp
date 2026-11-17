// USCCargoDroneAnimInstance: two-slot crossfade of sequences, optional yaw lock (header: SCCargoDroneAnim.h).
#include "SCCargoDroneAnim.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/AnimSequence.h"
#include "AnimationRuntime.h"
#include "BonePose.h"

namespace
{
struct FSCCargoDroneAnimProxy : public FAnimInstanceProxy
{
    struct FSlot
    {
        UAnimSequence* Sequence = nullptr;
        float Time = 0.f;
        bool bLoop = false, bLockYaw = false;
    };
    FSlot Current, Previous;
    float Alpha = 1.f, BlendS = 0.f; // Current's weight, rising to 1 over BlendS
    FName LockBone, PlaceBone;
    float MaxBob = 0.f;

    explicit FSCCargoDroneAnimProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance) {}

    virtual void PreUpdate(UAnimInstance* Instance, float DeltaSeconds) override
    {
        FAnimInstanceProxy::PreUpdate(Instance, DeltaSeconds);
        USCCargoDroneAnimInstance* Drone = CastChecked<USCCargoDroneAnimInstance>(Instance);
        LockBone = Drone->YawLockBone;
        PlaceBone = Drone->PlaceLockBone;
        MaxBob = Drone->MaxBob;
        if (!Drone->Pending.bPending) return;
        Drone->Pending.bPending = false;
        Previous = Current;
        Current = {Drone->Pending.Sequence, 0.f, Drone->Pending.bLoop, Drone->Pending.bLockYaw};
        BlendS = Drone->Pending.BlendS;
        Alpha = Previous.Sequence && BlendS > 0 ? 0.f : 1.f;
    }

    static void Advance(FSlot& S, float Dt)
    {
        if (!S.Sequence) return;
        const float Length = S.Sequence->GetPlayLength();
        S.Time += Dt;
        if (Length <= 0) S.Time = 0;
        else if (S.bLoop) S.Time = FMath::Fmod(S.Time, Length);
        else S.Time = FMath::Min(S.Time, Length);
    }

    virtual void Update(float DeltaSeconds) override
    {
        Advance(Current, DeltaSeconds);
        if (Alpha < 1.f)
        {
            Advance(Previous, DeltaSeconds);
            Alpha = BlendS > 0 ? FMath::Min(1.f, Alpha + DeltaSeconds / BlendS) : 1.f;
        }
        if (Alpha >= 1.f) Previous = {};
    }

    static void Extract(const FSlot& S, FPoseContext& Pose)
    {
        FAnimationPoseData Data(Pose);
        S.Sequence->GetAnimationPose(Data, FAnimExtractContext(double(S.Time), false, FDeltaTimeRecord(), S.bLoop));
    }

    virtual bool Evaluate(FPoseContext& Output) override
    {
        if (!Current.Sequence)
        {
            Output.ResetToRefPose();
            return true;
        }
        Extract(Current, Output);
        if (Previous.Sequence && Alpha < 1.f)
        {
            FPoseContext Other(Output);
            Extract(Previous, Other);
            FAnimationPoseData Mine(Output);
            const FAnimationPoseData Theirs(Other);
            FAnimationRuntime::BlendTwoPosesTogetherInPlace(Mine, Theirs, Alpha);
        }
        // The yaw lock: the bone's rotation less its twist about the vertical from its reference (bone's parent space;
        // the drone's root carries no rotation), weighted by how much of the pose comes from a locking clip.
        const float Lock = (Current.bLockYaw ? Alpha : 0.f) + (Previous.Sequence && Previous.bLockYaw ? 1.f - Alpha : 0.f);
        if (Lock > 0 && !LockBone.IsNone())
        {
            const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
            const int32 MeshIndex = Bones.GetPoseBoneIndexForBoneName(LockBone);
            if (MeshIndex != INDEX_NONE)
            {
                const FCompactPoseBoneIndex Index = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshIndex));
                if (Index.IsValid())
                {
                    const FQuat Ref = Bones.GetRefPoseTransform(Index).GetRotation();
                    FTransform& T = Output.Pose[Index];
                    const FQuat Delta = T.GetRotation() * Ref.Inverse();
                    FQuat Swing, Twist;
                    Delta.ToSwingTwist(FVector::UpVector, Swing, Twist);
                    T.SetRotation(FQuat::Slerp(T.GetRotation(), (Swing * Ref).GetNormalized(), Lock).GetNormalized());
                }
            }
        }
        // The place lock: the bone's translation back to its reference, within MaxBob.
        if (!PlaceBone.IsNone())
        {
            const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
            const int32 MeshIndex = Bones.GetPoseBoneIndexForBoneName(PlaceBone);
            if (MeshIndex != INDEX_NONE)
            {
                const FCompactPoseBoneIndex Index = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshIndex));
                if (Index.IsValid())
                {
                    const FVector Ref = Bones.GetRefPoseTransform(Index).GetTranslation();
                    FTransform& T = Output.Pose[Index];
                    T.SetTranslation(Ref + (T.GetTranslation() - Ref).GetClampedToMaxSize(MaxBob));
                }
            }
        }
        return true;
    }
};
} // namespace

void USCCargoDroneAnimInstance::Play(UAnimSequence* Sequence, bool bLoop, float BlendS, bool bLockYaw)
{
    Pending = {Sequence, bLoop, bLockYaw, true, BlendS};
    // Keep the playing pair alive: the new one and the one it fades from.
    if (Held.Num() >= 2) Held.RemoveAt(0);
    Held.Add(Sequence);
}

FAnimInstanceProxy* USCCargoDroneAnimInstance::CreateAnimInstanceProxy()
{
    return new FSCCargoDroneAnimProxy(this);
}

void USCCargoDroneAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy)
{
    delete static_cast<FSCCargoDroneAnimProxy*>(InProxy);
}
