// Data-driven initial cockpit seat sequence. The character is the only consumer and owns the session.
#pragma once

#include "SCTraversalClips.h"
#include "SCInteractionRecords.h"
#include "SCPlayerAnimator.h"
#include "SCSeatRecords.h"
#include "Engine/EngineTypes.h"

class ASCFirstPersonCharacter;
class ASCVehicle;
class USCRigComponent;
class USceneComponent;

class FSCSeatRuntime
{
public:
    void Setup(ASCVehicle& Vehicle);
    bool Interact(ASCFirstPersonCharacter& Character);
    void Tick(ASCFirstPersonCharacter& Character, float DeltaSeconds);
    void Stop(ASCFirstPersonCharacter& Character);
    FString Prompt(const ASCFirstPersonCharacter& Character) const;
    ESCSeatRole OccupiedRole() const;
    bool OwnsMovement() const;
    bool IsActive() const { return Phase != EPhase::Walk; }
    bool ApplyLook(ASCFirstPersonCharacter& Character, FVector2D Pixels);

private:
    enum class EPhase : uint8
    {
        Walk, LoadClips, Holster, OpenForEnter, Enter, CloseOnEnter, Retract, Select, Seated,
        Deselect, Deploy, OpenForExit, Exit, CloseOnExit
    };
    struct FRoute
    {
        TWeakObjectPtr<USceneComponent> Access, SeatAnchor;
        FTransform StaticSocket = FTransform::Identity;
        bool bStaticSocket = false, bAnimateSeat = false, bAnimateDash = false;
        TArray<int32> GatewayDoors;
        TWeakObjectPtr<USCRigComponent> SeatRig, DashRig;
        FSCSeatDefinition Definition;
        FSCSeatEntrance Entrance;
        FSCInteractionPoint Point;
        FSCPointInteraction Press;
        FTransform PointFrame = FTransform::Identity;
        TMap<FName, FSCSeatAction> Actions;
        bool IsValid() const;
        FTransform Socket() const;
    };
    int32 Focus(const ASCFirstPersonCharacter& Character) const;
    bool RequestClips(ASCFirstPersonCharacter& Character);
    void Begin(ASCFirstPersonCharacter& Character);
    void StartAction(EPhase NewPhase, FName State);
    void StartGateways(EPhase NewPhase);
    bool UpdateGateways(ASCFirstPersonCharacter& Character, const FRoute& Route, bool bEnter, bool bOpen);
    void StartExit(ASCFirstPersonCharacter& Character, const FRoute& Route);
    void FinishExit(ASCFirstPersonCharacter& Character, const FRoute& Route);
    bool CanUse(const FRoute& Route, const ASCFirstPersonCharacter& Character) const;
    bool PlayRigs(const FRoute& Route, ASCVehicle& Vehicle, FName Tag, bool bHold = false);
    FTransform Motion(const ASCFirstPersonCharacter& Character, float Seconds) const;
    FTransform SeatedTransform(const FRoute& Route) const;
    bool ReturnToSeat(ASCFirstPersonCharacter& Character, const FRoute& Route);
    static bool StandingClear(const ASCFirstPersonCharacter& Character, FVector Centre);
    void Restore(ASCFirstPersonCharacter& Character);
    TArray<FRoute> Routes;
    TWeakObjectPtr<ASCVehicle> Host, PlayerKit;
    FSCTraversalClips Request;
    FSCPlayerClipMap Clips;
    FTransform BodyInActor = FTransform::Identity;
    FTransform StartInShip = FTransform::Identity, ExitInShip = FTransform::Identity;
    EPhase Phase = EPhase::Walk;
    int32 Active = INDEX_NONE;
    FName ActionState;
    float ActionClock = 0.f;
    TBitArray<> GatewayIssued;
    FRotator RelativeView = FRotator::ZeroRotator;
    ECollisionEnabled::Type SavedCollision = ECollisionEnabled::QueryAndPhysics;
    bool bSavedControllerYaw = false;
};
