// Weapon fire for the player body (left mouse): the drawn weapon's first authored fire action (the game's default
// fire mode: rate, Mannequin fragments, launch helper) plays the weapon's own fire fragment on its rig (trigger and
// bolt clips from its enslavement ADB) while the trigger is held, and every shot puts a muzzle flash at the launch
// helper, a tracer from it to what the view axis hits, and a kick on the view. No damage, no ammunition.
// The game's particle effects are not imported: flash and tracer are viewer stand-ins (constants below).
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SCPlayerAnimator.h"
#include "SCPlayerFire.generated.h"

class ASCVehicle;
class USCRigComponent;
class UPointLightComponent;
class UStaticMeshComponent;

/** One shot's visuals: a short flash at the muzzle, a tracer flying to the target, a short flash where it lands. */
UCLASS()
class ASCShotFx : public AActor
{
    GENERATED_BODY()
public:
    ASCShotFx();
    void Launch(const FVector& Muzzle, const FVector& Target, bool bHit);
    virtual void Tick(float DeltaSeconds) override;

    // Viewer look constants for a ballistic weapon (the game's effects are particles the viewer does not import; the
    // round's own speed is not in the player package): a one-frame muzzle flash, a short thin streak at a rifle
    // bullet's speed (~900 m/s), a small spark where it lands.
    static constexpr float FlashSeconds = 0.03f;
    static constexpr float FlashCandelas = 4000;
    static constexpr float FlashRadius = 400;
    static constexpr float FlashGlow = 60000;
    static constexpr float TracerSpeed = 90000;
    static constexpr float TracerLength = 80;
    static constexpr float TracerRadius = 0.35f;
    static constexpr float TracerGlow = 25000;
    static constexpr float ImpactSeconds = 0.05f;
    static constexpr float ImpactCandelas = 2000;

private:
    UPROPERTY() TObjectPtr<UPointLightComponent> Flash;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> FlashGlowMesh;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Tracer;
    UPROPERTY() TObjectPtr<UPointLightComponent> Impact;
    FVector From = FVector::ZeroVector, To = FVector::ZeroVector;
    float Age = 0, Flight = 0;
    bool bLanded = false, bHits = false;
};

class FSCPlayerFire
{
public:
    /** Per weapon of Data: its rig (the kit rig of the weapon entity), its fire clips parsed against that rig, and
     *  its launch helper on its anchor. */
    void Attach(ASCVehicle* Kit, const FSCPlayerKit& Data);
    void Reset();
    void SetTrigger(bool bDown) { bTrigger = bDown; }
    bool IsTriggerDown() const { return bTrigger; }
    /** Advance with the drawn weapon (index into Data.Weapons; INDEX_NONE: nothing drawn, the trigger does nothing).
     *  AimOrigin/AimDirection: the view axis the shots go along. Returns the view kick (pitch, yaw degrees). */
    FVector2D Step(float DeltaSeconds, int32 Weapon, bool bAiming, const FVector& AimOrigin, const FVector& AimDirection, UWorld* World,
                   const TArray<const AActor*>& Ignore);
    int32 GetShots() const { return Shots; }
    FString Describe() const;

    /** View kick per shot (degrees) and tracer reach (cm): viewer constants; the game's procedural recoil is not in
     *  the player package. */
    static constexpr float KickPitch = 0.3f;
    static constexpr float KickYaw = 0.12f;
    static constexpr float Range = 50000;
    /** The kick returns at this rate (degrees/s) once the trigger is released (viewer constant). */
    static constexpr float KickRecovery = 12;
    /** A long frame (a hitch, a screenshot) fires at most this many shots: no burst of catch-up shots. */
    static constexpr int32 MaxShotsPerStep = 2;

private:
    struct FWeaponFire
    {
        const FSCPlayerWeapon* Weapon = nullptr;
        TWeakObjectPtr<USCRigComponent> Rig;
        TArray<FTransform> BindPose;
        TMap<FName, TSharedPtr<FSCPlayerClip>> Clips;
        TWeakObjectPtr<USceneComponent> Anchor;
        FTransform Launch = FTransform::Identity; // the fire helper in its anchor's frame (X along the barrel)
        bool bLaunch = false;
    };
    struct FPlaying
    {
        const FSCWeaponFragment* Fragment = nullptr;
        float Time = 0;
    };
    void Pose(FWeaponFire& W, const FPlaying& Main, const FPlaying& Tail);
    void Shoot(FWeaponFire& W, const FVector& AimOrigin, const FVector& AimDirection, UWorld* World, const TArray<const AActor*>& Ignore);

    TArray<FWeaponFire> Weapons; // parallel to the kit's weapons
    FPlaying Firing, Stopping;
    int32 Posed = INDEX_NONE; // weapon whose rig carries a fire pose
    bool bTrigger = false, bWasFiring = false;
    float ShotClock = 0;
    FVector2D Recoil = FVector2D::ZeroVector; // kick not yet recovered
    int32 Shots = 0;
    TWeakObjectPtr<ASCVehicle> Kit; // WP-A1: whose audio manifest holds the weapons' fire sounds
};
