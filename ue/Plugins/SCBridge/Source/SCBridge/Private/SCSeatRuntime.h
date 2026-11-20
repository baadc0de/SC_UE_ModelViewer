// Data-driven initial cockpit seat sequence. The character is the only consumer and owns the session.
#pragma once

#include "SCBridgeClient.h"
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
    bool ApplyLook(ASCFirstPersonCharacter& Character, FVector2D Pixels);

private:
    enum class EPhase : uint8 { Walk, LoadClips, Holster, Enter, Retract, Select, Seated, Deselect, Deploy, Exit };
    struct FRoute
    {
        TWeakObjectPtr<USceneComponent> Access;
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
    struct FClipResult
    {
        FSCPlayerClipMap Clips;
        FString Error;
    };

    int32 Focus(const ASCFirstPersonCharacter& Character) const;
    bool RequestClips(ASCFirstPersonCharacter& Character);
    void Begin(ASCFirstPersonCharacter& Character);
    void StartAction(EPhase NewPhase, FName State);
    bool PlayRigs(const FRoute& Route, ASCVehicle& Vehicle, FName Tag, bool bHold = false);
    FTransform Motion(const ASCFirstPersonCharacter& Character, float Seconds) const;
    FTransform SeatedTransform(const FRoute& Route) const;
    void Restore(ASCFirstPersonCharacter& Character);
    static FClipResult ReadClips(TArray64<uint8>&& Tar, const TArray<FString>& Names, const TMap<FString, int32>& Bones);

    TArray<FRoute> Routes;
    TWeakObjectPtr<ASCVehicle> Host, PlayerKit;
    TSharedPtr<FSCExportProgress, ESPMode::ThreadSafe> Progress;
    UE::Tasks::TTask<FSCExportResult> Export;
    UE::Tasks::TTask<FClipResult> Parse;
    TArray<FString> Requested;
    FSCPlayerClipMap Clips;
    FTransform BodyInActor = FTransform::Identity;
    FTransform StartInShip = FTransform::Identity, ExitInShip = FTransform::Identity;
    EPhase Phase = EPhase::Walk;
    int32 Active = INDEX_NONE;
    FName ActionState;
    float ActionClock = 0.f;
    FRotator RelativeView = FRotator::ZeroRotator;
    ECollisionEnabled::Type SavedCollision = ECollisionEnabled::QueryAndPhysics;
    bool bSavedControllerYaw = false;
};
