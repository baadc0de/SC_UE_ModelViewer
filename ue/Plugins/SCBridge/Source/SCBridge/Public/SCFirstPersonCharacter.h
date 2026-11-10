// Collision-aware first-person exploration, shared by Slate, native input, console automation and
// headless verification.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Camera/CameraComponent.h"
#include "SCFirstPersonCharacter.generated.h"
class ASCVehicle;
class USpotLightComponent;
class USCPlayerBodyComponent;

/** The exploration camera; NearClip > 0 overrides the global near plane (arms and weapon at the eye, WP-P2). */
UCLASS()
class SCBRIDGE_API USCEyeCameraComponent : public UCameraComponent
{
    GENERATED_BODY()
public:
    float NearClip = 0;
    virtual void GetCameraView(float DeltaTime, FMinimalViewInfo& DesiredView) override;
};

/** Body and locomotion dimensions (cm, cm/s). Viewer defaults until a real player body supplies its own (WP-P2). */
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCCharacterProfile
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadWrite) float CapsuleRadius = 34;
    UPROPERTY(BlueprintReadWrite) float StandingHalfHeight = 88;
    UPROPERTY(BlueprintReadWrite) float CrouchedHalfHeight = 44;
    UPROPERTY(BlueprintReadWrite) float StepHeight = 45;
    UPROPERTY(BlueprintReadWrite) float WalkSpeed = 400;
    UPROPERTY(BlueprintReadWrite) float RunSpeed = 650;
    UPROPERTY(BlueprintReadWrite) float NoclipSpeed = 650;
    /** Noclip stops when input is released instead of drifting (flying braking defaults to zero). */
    UPROPERTY(BlueprintReadWrite) float NoclipBraking = 4000;
    /** Eye above the capsule centre, standing and crouched (UE pawn/character defaults). */
    UPROPERTY(BlueprintReadWrite) float EyeHeight = 64;
    UPROPERTY(BlueprintReadWrite) float CrouchedEyeHeight = 32;
};

/** Every input the character understands; keys, console automation and UI map onto these. */
UENUM()
enum class ESCFirstPersonAction : uint8
{
    Forward, Back, Left, Right, Down, Up, Run, Crouch, // held
    Interact, Torch, Noclip, Teleport, Close,          // pressed
    Holster, View,                                     // pressed, player body (WP-P2)
    Weapon1, Weapon2, Weapon3, Weapon4, Weapon5, Weapon6, Weapon7, Weapon8, Weapon9
};

/** A seat's collidable geometry: a box in a rigid frame (rotation and translation; lengths are world lengths),
 *  so a turned vehicle's seat keeps its own box. An invalid box is a seat known only by its hint. */
struct FSCSeatBox
{
    FBox Box = FBox(ForceInit);
    FTransform Frame = FTransform::Identity;
};
UCLASS()
class SCBRIDGE_API ASCFirstPersonCharacter : public ACharacter
{
    GENERATED_BODY()
public:
    ASCFirstPersonCharacter();
    virtual void Tick(float DeltaSeconds) override;
    virtual void SetupPlayerInputComponent(UInputComponent* Input) override;
    virtual void RecalculateBaseEyeHeight() override;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UCameraComponent> Camera;
    UPROPERTY(VisibleAnywhere) TObjectPtr<USpotLightComponent> Flashlight;
    /** The real player body (WP-P2); inactive until the viewer attaches a player kit. */
    UPROPERTY(VisibleAnywhere) TObjectPtr<USCPlayerBodyComponent> Body;
    /** Player-kit status for the HUD while no body is attached (loading / failure message). */
    FString BodyStatus;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<ASCVehicle> Vehicle;
    UPROPERTY(BlueprintReadOnly) bool bNoclip = false;
    UPROPERTY(BlueprintReadOnly) bool bStuck = false;
    UPROPERTY(BlueprintReadOnly) bool bTeleportMenuOpen = false;
    UPROPERTY(BlueprintReadOnly) int32 RecoveryCount = 0;
    UPROPERTY(BlueprintReadOnly) int32 CapsuleHits = 0;
    /** Spawn index used by the last successful teleport (INDEX_NONE before the first). */
    UPROPERTY(BlueprintReadOnly) int32 CurrentSpawn = INDEX_NONE;

    /** The single setter for body/locomotion dimensions; nothing else writes these values. */
    void SetProfile(const FSCCharacterProfile& InProfile);
    const FSCCharacterProfile& GetProfile() const { return Profile; }
    /** Eye position relative to the capsule. Default: profile eye height (crouch aware). A body
     *  component (WP-P2) overrides the source; a camera attached elsewhere is never moved by Tick. */
    void SetEyeSource(TFunction<FVector()> Source) { EyeSource = MoveTemp(Source); }
    FVector EyeOffset() const;
    /** Re-apply EyeOffset to a capsule-attached camera now (a body evaluated after Tick calls it). */
    void RefreshEye();

    UFUNCTION(BlueprintCallable, Category = "SC|Exploration") bool Explore(ASCVehicle* InVehicle);
    UFUNCTION(BlueprintCallable, Category = "SC|Exploration") bool TeleportToSpawn(int32 Index);
    /** bSeated: a seat hint — crouched (sitting) capsule on the surface right under the hint, no
     *  walkable-neighbourhood requirement and no search beyond it (never the floor below the ship). */
    UFUNCTION(BlueprintCallable, Category = "SC|Exploration") bool TeleportToFloor(FVector Floor, bool bSeated = false);
    /** Seated teleport. Seat is the box of the seat's collidable geometry in Frame (FSCSeatBox): the capsule
     *  must touch it, so a clear pocket beside the seat (behind its back, past a bulkhead) is refused. */
    bool TeleportToSeat(FVector Floor, const FBox& Seat, const FTransform& Frame = FTransform::Identity);
    UFUNCTION(BlueprintCallable, Category = "SC|Exploration") bool Interact();
    /** Interaction E would toggle now: under the crosshair within reach, else the nearest joint in reach. */
    int32 FocusedInteraction() const;
    /** Where E reaches from: the eye camera in first person, the body's camera joint in third person. */
    FVector InteractionOrigin() const;
    UFUNCTION(BlueprintCallable, Category = "SC|Exploration") void ToggleNoclip();
    UFUNCTION(BlueprintCallable, Category = "SC|Exploration") void SetMoveInput(FVector Input);
    UFUNCTION(BlueprintPure, Category = "SC|Exploration") FString HUDText() const;
    void Look(FVector2D Pixels);
    /** Absolute view relative to the vehicle (degrees). */
    void SetVehicleRelativeView(float Yaw, float Pitch);
    FRotator GetVehicleRelativeView() const;
    FVector GetVehicleRelativeLocation() const;
    /** Hold a movement input for a duration of game time (automation); bRun selects run speed. */
    void HoldMoveInput(FVector Input, float Seconds, bool bRun);
    bool IsHoldingMove() const { return HoldSeconds > 0; }
    void SetCrouching(bool bCrouch);
    void SetTorch(bool bOn);
    bool SetNoclip(bool bOn);
    bool IsRunning() const { return bRunning; }

    static const TArray<TPair<FKey, ESCFirstPersonAction>>& KeyTable();
    bool HandleKey(FKey Key, bool bDown);
    bool HandleAction(ESCFirstPersonAction Action, bool bDown);
    void ClearInput();
    /** Find a walkable surface and unoccupied capsule around a semantic spawn hint; standing first,
     *  crouched when only a crouched capsule fits (seats under canopies). */
    bool ResolveSpawn(FVector Hint, FVector& CapsuleCentre, bool& bCrouched, const FSCSeatBox* Seat = nullptr) const;
    bool ResolveSpawn(FVector Hint, FVector& CapsuleCentre) const { bool C; return ResolveSpawn(Hint, CapsuleCentre, C); }
private:
    FSCCharacterProfile Profile;
    TFunction<FVector()> EyeSource;
    FVector Move = FVector::ZeroVector, LastPosition = FVector::ZeroVector, HoldInput = FVector::ZeroVector;
    FTransform LastSpawn;
    TSet<ESCFirstPersonAction> Held;
    float BlockedSeconds = 0, HoldSeconds = 0, NoclipRefusedSeconds = 0;
    /** Last applied light side (WP-U3b §12): -1 none, 0 outside, 1 inside a room volume; and the kit it covered. */
    int8 LightAreaState = -1;
    TWeakObjectPtr<ASCVehicle> LightAreaKit;
    double RecoveryZ = -UE_BIG_NUMBER;
    bool bRunning = false;
    void SetRunning(bool bRun);
    void ApplyHeldInput();
    bool FindCapsuleSpot(FVector Hint, double HalfHeight, FVector& Centre, const FSCSeatBox* Seat = nullptr) const;
    bool Place(FVector Floor, const FSCSeatBox* Seat);
    UFUNCTION() void CapsuleHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
                               FVector NormalImpulse, const FHitResult& Hit);
};
