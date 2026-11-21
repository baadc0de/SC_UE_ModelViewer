// First-person ownership of a single authored ladder traversal.
#pragma once

#include "Engine/EngineTypes.h"
#include "SCLadderRecords.h"
#include "SCTraversalClips.h"

class ASCFirstPersonCharacter;
class ASCVehicle;

class FSCLadderRuntime
{
public:
    void Tick(ASCFirstPersonCharacter& Character, float Delta);
    bool Interact(ASCFirstPersonCharacter& Character);
    bool ApplyLook(ASCFirstPersonCharacter& Character, FVector2D Pixels);
    bool OwnsMovement() const;
    void Stop(ASCFirstPersonCharacter& Character);
    FString State() const;

private:
    enum class EPhase : uint8 { Walk, Mount, Climb, Dismount };
    struct FApproach
    {
        int32 Index = INDEX_NONE;
        bool bTop = false;
        float FacingSign = 1.f;
    };

    FApproach FindApproach(const ASCFirstPersonCharacter& Character, bool bAutomatic) const;
    bool TryMount(ASCFirstPersonCharacter& Character, const FApproach& Approach);
    bool HaveClips(ASCFirstPersonCharacter& Character, const FSCLadderDefinition& Data);
    FTransform OnLadder(const FSCLadderAnchor& Ladder, float Height) const;
    FTransform RootMotion(const ASCFirstPersonCharacter& Character, const FSCPlayerClip& Clip, float Seconds) const;
    bool TryDismount(ASCFirstPersonCharacter& Character, const FSCLadderAnchor& Ladder, bool bTop);
    void StartAction(const FSCLadderAnchor& Ladder, FName State, EPhase NewPhase);
    void Restore(ASCFirstPersonCharacter& Character);
    void Recover(ASCFirstPersonCharacter& Character);

    EPhase Phase = EPhase::Walk;
    FName Action;
    FSCTraversalClips Request;
    FSCPlayerClipMap Clips;
    TWeakObjectPtr<ASCVehicle> Host, Kit;
    int32 Active = INDEX_NONE;
    bool bLoading = false, bClipLoadFailed = false, bManualPending = false;
    bool bSavedControllerYaw = false, bNeedFreshInput = false;
    ECollisionEnabled::Type SavedCollision = ECollisionEnabled::QueryAndPhysics;
    float FacingSign = 1.f, Height = 0.f, Velocity = 0.f, ActionClock = 0.f, Cycle = 0.f;
    float RemountDelay = 0.f;
    FRotator RelativeView = FRotator::ZeroRotator;
    FTransform BodyInActor = FTransform::Identity, Start = FTransform::Identity, End = FTransform::Identity;
};
