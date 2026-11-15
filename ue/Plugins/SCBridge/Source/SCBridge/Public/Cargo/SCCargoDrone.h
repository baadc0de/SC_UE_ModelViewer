// Cargo drone and its dock (WP-K1). A drone is a kinematic flyer (thrust and speed limited, arrival braking) with a
// belly tractor emitter: a physics handle holds the carried box softly (it sways and collides) and a beam mesh is drawn
// from the emitter to the box. The dock is a 1-cell grid item on the ship that stores drones in its bays.
// Meshes: /SCBridge/Cargo (K1d art) by soft path, engine basic shapes when missing (logged once).
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Cargo/SCCargoTypes.h"
#include "SCCargoDrone.generated.h"

class UPhysicsHandleComponent;
class UPrimitiveComponent;
class UStaticMeshComponent;
class UPointLightComponent;

UCLASS()
class SCBRIDGE_API ASCCargoDrone : public AActor
{
    GENERATED_BODY()
public:
    ASCCargoDrone();
    virtual void PostInitializeComponents() override;
    virtual void Tick(float DeltaSeconds) override;

    int32 DroneId = INDEX_NONE;
    int32 DockItem = INDEX_NONE; // the dock's grid item id
    int32 Bay = 0;
    ESCCargoDroneState State = ESCCargoDroneState::Docked;
    int32 JobId = INDEX_NONE;
    int32 MoveId = INDEX_NONE; // the move (task) that has claimed the drone
    int32 CarryingBoxId = INDEX_NONE;
    FName Owner; // its dock's owner; None = the hangar's shared pool
    float MaxSpeed = 0, MaxAccel = 0; // cm/s, cm/s^2
    /** Gusts (FSCCargoSway): amplitude / tilt / frequency from the drone definition; SwayGain is set by the task each tick
     *  (0 in vacuum, while docked and in the precise phases) and eased so it fades rather than snaps. */
    float SwayAmplitudeCm = 0, SwayTiltDeg = 0, SwayFrequencyHz = 0, SwayGain = 0;
    /** The sway offset applied in the last Pose (the lead's goes to the box through the beam). */
    FVector SwayOffset() const { return SwayNow; }
    /** The beam's hold stiffness (1/s^2, acceleration mode): the hanging box sags g/k below its target. */
    static constexpr float HoldStiffness = 40.f;

    UPROPERTY() TObjectPtr<UStaticMeshComponent> Hull;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Rotors;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Beam;
    UPROPERTY() TObjectPtr<UPointLightComponent> Light;
    UPROPERTY() TObjectPtr<UPhysicsHandleComponent> Handle;

    /** Fly to Target (world cm), facing Yaw (degrees) when bFace. Arrived() once within Tolerance and slowed down. */
    void FlyTo(const FVector& Target, float Yaw, bool bFace = true);
    bool Arrived(float ToleranceCm = 8.f) const;
    /** Snap to a pose (docked in a bay): no flight. */
    void Place(const FTransform& World);
    /** Flown by its task: at Location, leaning into Accel (world cm/s^2), turning towards Yaw (degrees). */
    void Pose(const FVector& Location, const FVector& Accel, float Yaw, float DeltaSeconds);
    FVector GetVelocity() const override { return Velocity; }
    /** The emitter in world space. */
    FVector Emitter() const;
    /** Beam from the emitter to Point (world); bOn false hides it. */
    void SetBeam(bool bOn, const FVector& Point = FVector::ZeroVector, float WidthCm = 30.f);
    /** Grab Box at its top centre and hold it (soft); Hold moves the grab target. */
    void Grab(UPrimitiveComponent* Box, const FVector& At, const FRotator& Rotation);
    void Hold(const FVector& At, const FRotator& Rotation);
    void Drop();
    bool IsHolding() const;

private:
    FVector Velocity = FVector::ZeroVector;
    FVector Target = FVector::ZeroVector;
    float TargetYaw = 0;
    bool bFlying = false, bFace = true;
    float RotorAngle = 0;
    float SwayGainNow = 0;
    FVector SwayNow = FVector::ZeroVector;
};

UCLASS()
class SCBRIDGE_API ASCCargoDroneDock : public AActor
{
    GENERATED_BODY()
public:
    ASCCargoDroneDock();
    int32 ItemId = INDEX_NONE;
    int32 GridId = INDEX_NONE;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Mesh;
    /** Footprint edge (cm) the dock was scaled to (one lattice cell). */
    void SetFootprint(const FVector& CellCm);
    /** Bay pose (a drone at rest) and the launch point beside it, world space. */
    FTransform BayTransform(int32 Bay) const;
    FVector BayExit(int32 Bay) const;
    int32 NumBays() const;
};
