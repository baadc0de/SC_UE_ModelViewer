// Cargo drone and its dock (WP-K1). A drone flies thrust and speed limited (arrival braking) and has a belly tractor
// emitter. Carrying, it is a body of its own (FSCCargoDroneBody: thrust less the beam's reaction, jerk limited) that
// flies to the station it chose for itself; its beam is a spring to its attach point on the box, whose rest length the
// drone sets (Cargo/SCCargoTug.h; the subsystem applies the forces, SCCargoTugStep.cpp). Every step it takes, however
// it is flown, is its own (Step): out of the way of other teams' loads, apart from the other drones, swept with its
// hull's bounds and slid along what it meets; what it still ends in is counted (Overlaps). A beam mesh is drawn from
// the emitter to the box. The dock is a 1-cell grid item on the ship that stores drones in its bays.
// Meshes: /SCBridge/Cargo (K1d art) by soft path, engine basic shapes when missing (logged once).
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Cargo/SCCargoTypes.h"
#include "Cargo/SCCargoTug.h"
#include "SCCargoDrone.generated.h"

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
    /** The sway offset applied in the last Pose / Tug: the drone's own gusts (its seed's process). They move its
     *  emitter, and through its beam's spring the box. */
    FVector SwayOffset() const { return SwayNow; }
    /** The drone's collision shape: the bounds of its hull and rotors about the actor's origin (cm, level), and their
     *  bounding radius in plan. Every step a drone takes is swept with it (Step). */
    FVector HullCentre = FVector::ZeroVector, HullHalf = FVector(45, 45, 20);
    double HullRadius() const { return HullHalf.Size2D(); }
    /** Its last step met something (the surroundings, a box) and was slid or cut short. */
    bool bBlocked = false;
    /** Times any drone was found overlapping something (the surroundings, a box, another drone) after a step: 0 in a
     *  sound run. Logged as CARGO_DRONE_OVERLAP and on every CARGO_JOB line. */
    static int32 Overlaps;
    /** Where its hull is kept space for other teams (route queries): its place, followed only once it has moved
     *  KeptSlackCm from it, so a drone hovering in its gusts keeps one box. */
    static constexpr double KeptSlackCm = 10;
    FVector GetKeptAt() const { return KeptAt; }
    /** What its hull meets standing in its bay (the structure of the grid cell its dock stands on: a plate's rails):
     *  the bay's own, as a box's slot is the grid's. Left out of its sweeps while it is in its dock's corridor
     *  (docked, launching, docking); set when it leaves its bay and when it comes in to dock. */
    TArray<TWeakObjectPtr<const UPrimitiveComponent>> BayOwn;
    /** The end of its dock's corridor as it was last planned (USCCargoSubsystem::Departure): the point off the dock's
     *  grid where its routing shape is clear. */
    FVector DockClear = FVector::ZeroVector;
    bool bDockClear = false;
    /** On the stretch between its bay and the bay's exit (set by its flight): the dock moves it there, as it holds it
     *  in the bay; its steps there are not swept. */
    bool bDockLeg = false;
    /** Whether it could leave its bay when last looked at (USCCargoSubsystem::CanLeaveBay), and when that was. */
    bool bCanLeave = true;
    double LeaveChecked = -1000;
    bool InDockWay() const { return State == ESCCargoDroneState::Docked || State == ESCCargoDroneState::Launching || State == ESCCargoDroneState::Docking; }
    /** The states in which a drone holds no load in the air and steps out of the way of loads other teams fly: in
     *  flight on its own, and hovering over its box while the beams lock on (Attaching: the box is still at rest). */
    static bool DodgesLoads(ESCCargoDroneState S)
    {
        return S == ESCCargoDroneState::Launching || S == ESCCargoDroneState::ToPickup || S == ESCCargoDroneState::Attaching ||
               S == ESCCargoDroneState::Returning || S == ESCCargoDroneState::Docking;
    }
    /** The way it came: its places every TrailStepCm, the last TrailPoints of them (not in its dock). From a place
     *  it cannot plan a route out of (a pocket beside a box it has set down), it goes back along this. */
    static constexpr double TrailStepCm = 50;
    static constexpr int32 TrailPoints = 60;
    TArray<FVector> Trail;
    /** Held up (its step met something): until then it keeps to its shortest beam (seconds of world time). */
    double TuckUntil = 0;
    /** The drone as a body while it tugs (metres; the emitter's position), and its beam as it set it last: the rest
     *  length, the length and the pull on the box (N, world). */
    FSCCargoDroneBody Flight;
    double BeamRestM = 0, BeamLengthM = 0;
    FVector BeamForceN = FVector::ZeroVector;

    UPROPERTY() TObjectPtr<UStaticMeshComponent> Hull;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Rotors;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Beam;
    UPROPERTY() TObjectPtr<UPointLightComponent> Light;

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
    /** The beam takes hold of Box (the subsystem works its spring each tick) / lets go. */
    void Grab(UPrimitiveComponent* Box);
    void Drop();
    bool IsHolding() const { return Held.IsValid(); }
    /** Tugging: one step of the body towards its station (emitter position and velocity, metres), against the beam's
     *  reaction (N); the gusts move it about that. Turns towards Yaw. */
    void Tug(const FSCCargoTug::FStation& Station, const FVector& ReactionN, float Yaw, float DeltaSeconds);

private:
    FVector Velocity = FVector::ZeroVector;
    FVector Target = FVector::ZeroVector;
    float TargetYaw = 0;
    bool bFlying = false, bFace = true;
    float RotorAngle = 0;
    float SwayGainNow = 0;
    FVector SwayNow = FVector::ZeroVector;
    TWeakObjectPtr<UPrimitiveComponent> Held;
    bool bBody = false;                       // Flight is live (set by Tug; Pose flies it back onto its formation point)
    FVector PoseTarget = FVector::ZeroVector; // the last Pose location, for its velocity
    bool bOverlapping = false;
    double BlockedLogged = -1000;
    FVector KeptAt = FVector::ZeroVector;
    /** One step to Want, the drone's own: out of the way of the loads other teams fly (FSCCargoDroneSteer::Evade), clear
     *  of the other drones (Separate, from where they are and how they move), swept with its shape against the surroundings and every box and slid along what it meets
     *  (USCCargoSubsystem::DroneSweep), no faster than twice its speed limit. Counts an overlap it ends in. */
    void Step(const FVector& Want, const FRotator& Rotation, float Dt);
    void Gusts(float Dt, FVector2D& OutWobble);
    void Lean(const FVector& Accel, const FVector2D& Wobble, float Yaw, float Dt, const FVector& At);
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
