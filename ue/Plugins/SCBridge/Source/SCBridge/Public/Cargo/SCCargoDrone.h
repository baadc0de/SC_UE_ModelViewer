// Cargo drone and its dock (WP-K1, WP-K4). A drone flies thrust and speed limited (arrival braking) and has a tractor
// emitter. Carrying, it is a body of its own (FSCCargoDroneBody: thrust less the beam's reaction, jerk limited) that
// flies to the station it chose for itself; its beam is a spring to its attach point on the box, whose rest length the
// drone sets (Cargo/SCCargoTug.h; the subsystem applies the forces, SCCargoTugStep.cpp). Every step it takes, however
// it is flown, is its own (Step): out of the way of other teams' loads, apart from the other drones, swept with its
// hull's bounds and slid along what it meets; what it still ends in is counted (Overlaps).
// Its look (WP-K4a): the art Config/Cargo/drone_art.json names (FSCCargoDroneArt: a skeletal mesh at native size, its
// centre bone on the actor origin, turned by its yaw offset to face +X, the beam from its beam bone, clips by motion),
// else the K1d stand-in (/SCBridge/Cargo meshes by soft path, engine basic shapes when missing; logged once). Its
// heading is its own (FSCCargoHeading): its bay's in its dock's corridor, else where it (or, carrying as a team, its
// load) travels, kept while hovering; turning is visual and moves the hull about the emitter, its collision shape
// covers every heading. Its clips follow its velocity in its own frame (FSCCargoDroneMotion).
// Its beam (WP-K4c): the tractor record's Niagara beam, hit and muzzle (FSCCargoFx) at the beam's strength, blended
// as the record says; the K1d beam mesh when they are not built.
// The dock is a grid item of as many lattice cells as its mesh spans (FSCCargoDockFootprint) that stores drones in its
// bays, facing out of them.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Cargo/SCCargoTypes.h"
#include "Cargo/SCCargoTug.h"
#include "Cargo/SCCargoDroneArt.h"
#include "SCCargoDrone.generated.h"

class UPrimitiveComponent;
class UStaticMeshComponent;
class USkeletalMeshComponent;
class UPointLightComponent;
class UNiagaraComponent;
class UAnimSequence;

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
    /** How close its emitter may hang over the face its beam holds (cm): its hull's reach below the emitter (reference
     *  pose) plus the gap it keeps from its own box (FSCCargoTug::HullHangCm). */
    double HullHangCm() const;
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

    UPROPERTY() TObjectPtr<UStaticMeshComponent> Hull; // the stand-in (null with the art)
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Rotors;
    UPROPERTY() TObjectPtr<USkeletalMeshComponent> Art; // drone_art.json's mesh (null: the stand-in)
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Beam;  // the K1d beam mesh (without the built effects)
    UPROPERTY() TObjectPtr<UNiagaraComponent> BeamFx;
    UPROPERTY() TObjectPtr<UNiagaraComponent> HitFx;
    UPROPERTY() TObjectPtr<UNiagaraComponent> MuzzleFx;
    UPROPERTY() TObjectPtr<UPointLightComponent> Light;
    /** The art's clips, loaded once (by asset path). */
    UPROPERTY() TMap<FString, TObjectPtr<UAnimSequence>> Clips;

    /** Fly to Target (world cm). Arrived() once within Tolerance and slowed down. */
    void FlyTo(const FVector& Target);
    bool Arrived(float ToleranceCm = 8.f) const;
    /** Snap to a pose (docked in a bay): no flight. */
    void Place(const FTransform& World);
    /** Flown by its task: at Location, leaning into Accel (world cm/s^2). It turns by its heading policy. */
    void Pose(const FVector& Location, const FVector& Accel, float DeltaSeconds);
    /** Carrying as a team this tick: it faces its load's direction of travel (Velocity, world cm/s; the route's, not
     *  the formation's wobble), and keeps its heading while the load is at rest. Set before its Pose / Tug. */
    void SetLoadVelocity(const FVector& InVelocity) { LoadVelocity = InVelocity; bByLoad = true; }
    /** The heading it turns towards now (degrees; FSCCargoHeading::Target). */
    float HeadingTarget() const { return WantYaw; }
    /** The clip it shows, as motion/phase (e.g. "Front/Loop"; the art's drones). */
    FString ClipName() const;
    FVector GetVelocity() const override { return Velocity; }
    /** The emitter in world space: the art's beam bone, the stand-in's Tractor socket. */
    FVector Emitter() const;
    /** The direction the beam leaves the emitter (world): the art's beam axis, the stand-in socket's forward. */
    FVector EmitterForward() const;
    /** Beam from the emitter to Point (world); bOn false hides it. */
    void SetBeam(bool bOn, const FVector& Point = FVector::ZeroVector, float WidthCm = 30.f);
    /** The beam takes hold of Box (the subsystem works its spring each tick) / lets go. */
    void Grab(UPrimitiveComponent* Box);
    void Drop();
    bool IsHolding() const { return Held.IsValid(); }
    /** Tugging: one step of the body towards its station (emitter position and velocity, metres), against the beam's
     *  reaction (N); the gusts move it about that. Turns by its heading policy (its load's travel). */
    void Tug(const FSCCargoTug::FStation& Station, const FVector& ReactionN, float DeltaSeconds);

private:
    FVector Velocity = FVector::ZeroVector;
    FVector Target = FVector::ZeroVector;
    bool bFlying = false;
    // Heading and clips (WP-K4a).
    FVector Travel = FVector::ZeroVector;       // its velocity, smoothed (FSCCargoDroneArt::SmoothingS)
    FVector LoadVelocity = FVector::ZeroVector; // its team's load's (SetLoadVelocity), this tick
    bool bByLoad = false;
    float WantYaw = 0, YawRate = 0, LastYaw = 0;
    FSCCargoDroneMotion Motion;
    bool bScanning = false;                      // its beam is locking on (shown by the scanning clip)
    // The beam's shown strength and the one its state asks for (FSCCargoFx strengths, blended).
    float BeamStrength = 0, HitStrength = 0;
    bool bBeamOn = false, bFxLogged = false;
    FVector BeamPoint = FVector::ZeroVector;
    void SetupArt();
    void SetupStandIn();
    /** The heading for this tick: the policy's target, turned towards at the limited rate (degrees). */
    float Heading(float Yaw, float Dt);
    void Animate(float Dt);
    bool bAnimStarted = false;
    UAnimSequence* ClipFor(ESCDroneMotion M, ESCDroneClipPhase P, int32 IdleIndex);
    /** The beam as the game's effects (beam, hit, muzzle); false when they are not available (the beam mesh then). */
    bool SetBeamFx(bool bOn, const FVector& Point);
    float RotorAngle = 0;
    float LeanScale = 1.f; // the actor's lean into its acceleration (FSCCargoDroneArt::ActorLeanScale with the art)
    FVector EmitterLocal = FVector(0, 0, -20); // the emitter in the actor's frame, reference pose
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
    void Lean(const FVector& Accel, const FVector2D& Wobble, float Dt, const FVector& At);
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
    UPROPERTY() TArray<TObjectPtr<UPointLightComponent>> BayLights; // a neutral fill at its BayLight_i sockets
    /** The lattice cells the dock mesh spans (FSCCargoDockFootprint::Cells; at least one per axis). */
    static FIntVector MeshCells(const FVector& CellCm);
    /** Scale the mesh uniformly to fill Cells lattice cells of CellCm; its bounds' bottom centre on the actor origin
     *  (the footprint's floor centre). */
    void SetFootprint(const FIntVector& Cells, const FVector& CellCm);
    /** Bay pose (a drone at rest) and the launch point beside it, world space. */
    FTransform BayTransform(int32 Bay) const;
    FVector BayExit(int32 Bay) const;
    int32 NumBays() const;
};
