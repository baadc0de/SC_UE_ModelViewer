// The cargo drone's animation instance (WP-K4a): plays one sequence at a time and crossfades to the next (two slots,
// no anim graph, no Blueprint). A clip played with bLockYaw keeps the yaw-lock bone at its reference yaw: the turn
// clips rotate the body, but the actor does the turning (its heading policy), so the clip adds only its tilt and sway.
// In every clip the place-lock bone keeps its reference place (within MaxBob): the actor carries the motion and the
// clips keep their rotations and moving parts (so the hull built from those poses is what renders).
#pragma once
#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "SCCargoDroneAnim.generated.h"

class UAnimSequence;

UCLASS(Transient, NotBlueprintable)
class USCCargoDroneAnimInstance : public UAnimInstance
{
    GENERATED_BODY()
public:
    /** Crossfade to Sequence (from its start) over BlendS. */
    void Play(UAnimSequence* Sequence, bool bLoop, float BlendS, bool bLockYaw);
    /** The bone bLockYaw holds at its reference yaw. */
    FName YawLockBone;
    /** The bone held at its reference place in every clip (the actor carries the motion), within MaxBob (mesh units). */
    FName PlaceLockBone;
    float MaxBob = 0.f;

    struct FCommand
    {
        UAnimSequence* Sequence = nullptr;
        bool bLoop = false, bLockYaw = false, bPending = false;
        float BlendS = 0.f;
    };
    FCommand Pending; // read by the proxy in PreUpdate (game thread)

protected:
    virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
    virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;

private:
    UPROPERTY(Transient) TArray<TObjectPtr<UAnimSequence>> Held; // the two playing (kept from collection)
};
