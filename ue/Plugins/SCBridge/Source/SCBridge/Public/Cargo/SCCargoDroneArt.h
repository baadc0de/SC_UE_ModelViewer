// Cargo drone art and motion policy (WP-K4a). The drone's look is described in Config/Cargo/drone_art.json, not in
// code: the skeletal mesh, the yaw that turns its face to +X, its bones (centre, beam, light), the material slots it
// hides, the clip for each motion, and the heading / animation thresholds. Without the file, or without the mesh it
// names, the drone is the K1d stand-in (ASCCargoDrone). The pure policies live here so they can be tested:
//  - FSCCargoHeading: where a drone faces (its bay's yaw in its dock's corridor, else the direction of travel, else
//    where it faces) and the limited turn towards it;
//  - FSCCargoDroneMotion: which clip plays from the drone's velocity in its own frame (start / loop / end per direction,
//    idle under a speed, turning in place, scanning);
//  - FSCCargoDockFootprint: how many lattice cells a dock mesh takes, its uniform scale and the turns it may try.
#pragma once
#include "CoreMinimal.h"

/** What a drone shows: Idle; moving Front / Back / Up / Down / Left / Right; turning in place; scanning a box. */
enum class ESCDroneMotion : uint8 { Idle, Front, Back, Up, Down, Left, Right, TurnLeft, TurnRight, Scanning };
/** A directional motion's clip: entering it (Start), holding it (Loop), leaving it (End). Other motions play Loop. */
enum class ESCDroneClipPhase : uint8 { Start, Loop, End };

struct SCBRIDGE_API FSCCargoDroneArt
{
    // Heading and animation (apply to the stand-in too).
    /** Yaw rate towards the heading (degrees / s). */
    float TurnRateDegS = 120.f;
    /** Below this horizontal speed a drone hovers: it keeps its heading and idles (cm / s). */
    float HoverSpeedCmS = 20.f;
    /** Turning at least this fast while hovering plays the turn clip (degrees / s). */
    float TurnClipRateDegS = 20.f;
    /** Crossfade between clips (s); time constant of the travel velocity the heading and clips follow (s); how long a
     *  directional clip plays before another direction may take over (s). */
    float BlendS = 0.25f, SmoothingS = 0.2f, AxisDwellS = 0.2f;

    // The art (Mesh empty: the stand-in).
    FString Mesh;
    double Scale = 1.0, YawOffsetDeg = 0.0;
    /** CentreBone sits on the actor origin (its reference-pose place); the beam leaves BeamBone along BeamAxis (actor
     *  frame); the status light rides LightBone; YawLockBone keeps its reference yaw in the turn clips (the actor turns). */
    FName CentreBone, BeamBone, LightBone, YawLockBone;
    FVector BeamAxis = FVector(0, 0, -1);
    /** The actor's lean into its acceleration (8 degrees at full thrust) is multiplied by this with the art: its clips
     *  lean the body themselves (Move / Slide), and the two compound. 1 for the stand-in. */
    float ActorLeanScale = 1.f;
    /** How far (cm, actor scale) the clips may still move the centre bone: the actor carries the motion (0: none). */
    float BodyBobCm = 0.f;
    TArray<FName> HiddenSlots;
    TArray<FString> Idle;
    /** Start / Loop / End sequences per directional motion (indexed by ESCDroneMotion Front..Right). */
    FString Directional[6][3];
    FString TurnLeft, TurnRight, Scanning;
    /** The file it came from, or "defaults". */
    FString Source;

    bool HasArt() const { return !Mesh.IsEmpty(); }
    /** The sequence for a motion's phase (Idle: IdleIndex into Idle, wrapping); empty when the art has none. */
    FString Clip(ESCDroneMotion Motion, ESCDroneClipPhase Phase, int32 IdleIndex = 0) const;
    /** Every asset the art names (mesh and sequences): what a cook must include. */
    TArray<FString> AssetPaths() const;

    static FString ConfigFile();
    /** Config/Cargo/drone_art.json, read once (logged: CARGO_DRONE_ART). Missing or invalid: the defaults (stand-in). */
    static const FSCCargoDroneArt& Get();
    static bool Parse(const FString& Json, FSCCargoDroneArt& Out, FString& Error);
};

struct SCBRIDGE_API FSCCargoHeading
{
    /** The yaw a drone turns towards (degrees): its bay's while it is in its dock's corridor (it leaves forwards and
     *  backs in); else the direction of Travel (its own velocity, or its load's when it carries as a team) when that is
     *  at least MinSpeedCmS horizontally; else where it faces (hovering, it keeps its heading). */
    static float Target(float Yaw, const FVector& Travel, float MinSpeedCmS, const TOptional<float>& BayYaw);
    /** Yaw turned towards Target by at most RateDegS x Dt, the short way round. */
    static float Turn(float Yaw, float Target, float RateDegS, float Dt);
};

struct SCBRIDGE_API FSCCargoDroneMotion
{
    struct FInput
    {
        FVector LocalVelocity = FVector::ZeroVector; // cm / s, the drone's frame (X forward, Y right, Z up)
        float YawRateDegS = 0.f;
        bool bScanning = false;
    };
    ESCDroneMotion Motion = ESCDroneMotion::Idle;
    ESCDroneClipPhase Phase = ESCDroneClipPhase::Loop;
    float Time = 0.f;   // in the current clip
    int32 IdleIndex = 0; // the idle clip playing (each idle clip plays once, then the next)

    static bool IsDirectional(ESCDroneMotion M) { return M >= ESCDroneMotion::Front && M <= ESCDroneMotion::Right; }
    /** What the input asks for: scanning; else hovering (under 1.25 x HoverSpeedCmS to start moving, 0.75 x to stop) idles
     *  or turns in place (yaw rate at least TurnClipRateDegS); else the velocity's dominant axis in the drone's frame
     *  (another axis takes over once it is 25 % larger). */
    static ESCDroneMotion Want(const FInput& In, ESCDroneMotion Current, const FSCCargoDroneArt& Art);
    /** Advance by Dt; true when the clip changed (Motion / Phase / IdleIndex name the new one, Time is 0). A direction
     *  entered plays its Start then its Loop; one left plays its End before anything but another direction (which
     *  crossfades straight into its Start, once the current clip has played AxisDwellS). Length(motion, phase) is the clip's length in seconds (0: none). */
    bool Step(const FInput& In, float Dt, const FSCCargoDroneArt& Art, TFunctionRef<float(ESCDroneMotion, ESCDroneClipPhase)> Length);
    /** The clip loops (a Loop phase of a direction, turning, scanning); Start, End and idle clips play once. */
    bool Loops() const { return Motion != ESCDroneMotion::Idle && Phase == ESCDroneClipPhase::Loop; }
};

struct SCBRIDGE_API FSCCargoDockFootprint
{
    /** Lattice cells a dock mesh of MeshSizeCm takes: round(size / cell) per axis, at least 1. */
    static FIntVector Cells(const FVector& MeshSizeCm, const FVector& CellCm);
    /** The uniform scale at which the mesh's footprint fills those cells (the smaller of the plan axes' ratios). */
    static double Scale(const FVector& MeshSizeCm, const FIntVector& Cells, const FVector& CellCm);
    /** The quarter turns a dock may take looking for clear bay exits: all four when its footprint is square, else only
     *  half turns (its long axis stays along its slot's). */
    static TArray<int32> Turns(const FIntVector& Cells);
};
