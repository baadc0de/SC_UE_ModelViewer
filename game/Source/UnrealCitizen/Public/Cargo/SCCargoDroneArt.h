// Cargo drone art and motion policy (WP-K4a). The drone's look is described in Config/Cargo/drone_art.json, not in
// code: the skeletal mesh, the yaw that turns its face to +X, its bones (centre, beam, light), the material slots it
// hides, the clip for each motion, and the heading / animation thresholds. Without the file, or without the mesh it
// names, the drone is the K1d stand-in (ASCCargoDrone). The pure policies live here so they can be tested:
//  - FSCCargoHeading: where a drone faces (its bay's yaw in its dock's corridor, else the box it holds in front or
//    beside it (round 5), else the direction of travel, else where it faces) and the limited turn towards it;
//  - FSCCargoDroneMotion: which clip plays from the drone's velocity in its own frame (start / loop / end per direction,
//    idle under a speed, turning in place, scanning);
//  - FSCCargoDockFootprint: how many lattice cells a dock mesh takes, its uniform scale and the turns it may try;
//  - FSCCargoAttitude: how the drone's look tilts and wobbles about its body (round 4: each drone its own dynamics;
//    round 5: motion.load_transfer, how much of it the box it holds feels);
//  - the "grip" block (round 5): where a carrying drone holds its box from and how often it re-picks (FSCCargoGripPick).
#pragma once
#include "CoreMinimal.h"
#include "Cargo/SCCargoTug.h"

/** What a drone shows: Idle; moving Front / Back / Up / Down / Left / Right; turning in place; scanning a box. */
enum class ESCDroneMotion : uint8 { Idle, Front, Back, Up, Down, Left, Right, TurnLeft, TurnRight, Scanning };
/** A directional motion's clip: entering it (Start), holding it (Loop), leaving it (End). Other motions play Loop. */
enum class ESCDroneClipPhase : uint8 { Start, Loop, End };

struct UNREALCITIZEN_API FSCCargoDroneArt
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

    /** How a drone's look moves about its body (WP-K4 round 4, every drone, the stand-in too; FSCCargoAttitude): as a
     *  multirotor's, its attitude an underdamped spring that tilts its thrust to accelerate, overshoots on starts and
     *  stops and rocks back past level, with its own frequency, damping and small corrections. Visual only: the hull
     *  covers what it adds. */
    struct FMotion
    {
        float FreqMinHz = 0.7f, FreqMaxHz = 1.7f; // the attitude spring's natural frequency, each drone its own in this range
        float DampMin = 0.18f, DampMax = 0.3f;    // its damping ratio, each drone its own (under 1: it overshoots)
        float TiltGain = 0.08f;    // the share of the thrust's tilt atan(a / g) it shows ...
        float GainSpread = 0.5f;   // ... each drone its own, within +- this share of it (teammates lean into a bend unalike)
        float TiltLead = 3.f;     // how much of the commanded tilt's rate it leads with: a start or stop overshoots ...
        float TiltLeadCarrying = 1.5f; // ... and carrying (its team's shared starts and stops softer, its own motion the clearer)
        float MaxTiltDeg = 9.f;   // its attitude's limit
        float KickRateHz = 1.f;   // its own corrections: random kicks to its attitude, how often (per second) ...
        float KickDegS = 26.f;    // ... and how hard (degrees / s)
        float HuntCm = 1.5f;      // a slight altitude hunt (its height wanders by this) ...
        float HuntHz = 0.35f;     // ... at about this rate
        float DriftCm = 2.f;      // where its tilt runs ahead of its thrust it drifts that way, at most this far
        float YawJitterDeg = 2.5f; // its heading wanders by this
        // Each drone its own onsets and directions (owner, round 4): drawn from its own stream, per event.
        float OnsetMinS = 0.04f, OnsetMaxS = 0.26f; // it answers its command this late, each drone its own (spread over the ids)
        float OnsetJitterS = 0.12f; // ... and each start's kick a further 0 .. this late
        float EventDeg = 0.6f;      // a change of its command by this much is a motion starting or changing (an event) ...
        float EventMinS = 0.35f;    // ... at most one per this
        float EventKickScale = 0.6f; // an event's kick: this x kick_deg_s (median)
        float KickLogSigma = 0.45f; // every kick's size: log-normal about its median, this sigma
        float ScatterDeg = 30.f;    // every kick's direction: wrapped normal about the expected one (an event: the command's
                                    // change; a correction: back towards level), this sigma ...
        float OffAxisShare = 0.15f; // ... a share of them off to either side (90 degrees) ...
        float OppositeShare = 0.07f; // ... and a share the opposite way
        // Its wobble moves the box it holds (owner, round 5): the hold point turns with its attitude and moves with its
        // drift, by this share (the hold is stiff, critically damped near the wobble's band: alone it would pass the
        // wobble on whole); faded as the gusts are (none in the precise zone). Its worst reach is in the route's margin.
        float LoadTransfer = 0.35f;
    } Dynamics;
    /** Where a carrying drone holds its box from and how it re-picks (round 5; FSCCargoGripPick). */
    FSCCargoGripPick::FCfg Grip;

    // The art (Mesh empty: the stand-in).
    FString Mesh;
    double Scale = 1.0, YawOffsetDeg = 0.0;
    /** CentreBone sits on the actor origin (its reference-pose place); the beam leaves BeamBone along BeamAxis (actor
     *  frame); the status light rides LightBone; YawLockBone keeps its reference yaw in the turn clips (the actor turns). */
    FName CentreBone, BeamBone, LightBone, YawLockBone;
    FVector BeamAxis = FVector(0, 0, -1);
    /** With the art, the share of its acceleration its attitude (Dynamics) leans into (its clips lean the body too, by
     *  ClipTiltScale of theirs). 1 for the stand-in. */
    float ActorLeanScale = 1.f;
    /** How far (cm, actor scale) the clips may still move the centre bone: the actor carries the motion (0: none). */
    float BodyBobCm = 0.f;
    /** The share of the clips' own tilt of the centre bone (their lean) that is kept: the attitude (Dynamics) leans the
     *  drone now (1: all of it). */
    float ClipTiltScale = 1.f;
    TArray<FName> HiddenSlots;
    /** The glowing parts: per material slot, the scalar parameters that set its emission (the art's own values are
     *  multiplied by EmissiveScale while powered, 0 while powered down: docked in its bay, it powers down completely). */
    TMap<FName, TArray<FName>> EmissiveParams;
    float EmissiveScale = 1.f;
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

struct UNREALCITIZEN_API FSCCargoHeading
{
    /** The yaw a drone turns towards (degrees): its bay's while it is in its dock's corridor (it leaves forwards and
     *  backs in); else the direction of Travel (its own velocity, or its load's when it carries as a team) when that is
     *  at least MinSpeedCmS horizontally; else where it faces (hovering, it keeps its heading). Holding a box in front or
     *  beside it (HoldYaw: the hold's direction in plan), it faces the box, whichever way it flies (round 5). */
    static float Target(float Yaw, const FVector& Travel, float MinSpeedCmS, const TOptional<float>& BayYaw, const TOptional<float>& HoldYaw = {});
    /** Yaw turned towards Target by at most RateDegS x Dt, the short way round. */
    static float Turn(float Yaw, float Target, float RateDegS, float Dt);
};

struct UNREALCITIZEN_API FSCCargoDroneMotion
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

/** A drone's look about its body (FSCCargoDroneArt::Dynamics; WP-K4 round 4). Moving is tilting: to accelerate a
 *  multirotor tilts its thrust, its attitude commanded to atan(a / g) (a share of it), and its attitude controller is an
 *  underdamped spring that leads with the command's rate: it kicks into a start, overshoots, rocks back past level when
 *  it stops and settles in a few decaying swings. Each drone has its own frequency and damping (from its seed, within
 *  the configured ranges) and makes its own small corrections: random kicks, a slight altitude hunt, a wandering yaw;
 *  where its tilt runs ahead of the command it drifts that way. Teammates therefore never move in lockstep. Units:
 *  degrees, cm, seconds; the drone's frame (X forward, Y right, Z up). Pure: the drone applies it to its look. */
struct UNREALCITIZEN_API FSCCargoAttitude
{
    double Omega = 0, Zeta = 0;          // its tilt spring (rad/s, ratio)
    double Gain = 1;                     // its own share of the lean (1 +- GainSpread)
    double HuntHz = 0, HuntPhase[2] = {0, 0};
    FRandomStream Rng;
    FVector2D Tilt = FVector2D::ZeroVector, Rate = FVector2D::ZeroVector; // pitch, roll (degrees, degrees / s)
    FVector2D Command = FVector2D::ZeroVector;
    FVector Offset = FVector::ZeroVector, OffsetRate = FVector::ZeroVector; // cm, cm / s
    double Yaw = 0, YawRate = 0, YawTarget = 0, Time = 0;
    bool bStarted = false;
    double Onset = 0;                    // how late it answers its command (s)
    TArray<TPair<double, FVector2D>> History; // its commands of the last moments (time, degrees), for that delay
    FVector2D LastEvent = FVector2D::ZeroVector; // its command at its last event
    double LastEventTime = -1e9;
    struct FKick
    {
        double At = 0;
        FVector2D Rate = FVector2D::ZeroVector;
    };
    TArray<FKick> Pending;               // kicks drawn, not yet given (their onset)

    /** Its own constants from Seed, within the ranges of Cfg. Index (a drone's id; < 0: from Seed) spreads the onsets
     *  over their range by the golden ratio, so any two of a few drones answer at least a frame apart. */
    void Init(uint32 Seed, const FSCCargoDroneArt::FMotion& Cfg, int32 Index = -1);
    /** A kick drawn from Rng (degrees / s, pitch and roll): its direction a wrapped normal of ScatterDeg about Expected
     *  (uniform when that is zero), OffAxisShare of them turned a quarter either way and OppositeShare a half; its size
     *  log-normal (KickLogSigma) about MedianDegS. */
    static FVector2D DrawKick(FRandomStream& Rng, const FVector2D& Expected, double MedianDegS, const FSCCargoDroneArt::FMotion& Cfg);
    /** The onset of drone Index's command within the range (golden-ratio spread). */
    static double OnsetFor(int32 Index, const FSCCargoDroneArt::FMotion& Cfg);
    /** One step (any length: sub-stepped). AccelLocal: the body's acceleration in its own frame (cm / s^2); Gust: the
     *  gusts' wobble (degrees, pitch and roll), added to the command; bKicks: its own corrections on (off in tests that
     *  look at the bare response). */
    void Step(const FVector& AccelLocal, const FVector2D& Gust, double Dt, const FSCCargoDroneArt::FMotion& Cfg, bool bKicks = true);
    /** Back to level and still (docked: powered down). */
    void Reset();
    /** The look's turn about the body (pitch, yaw, roll) and its offset. */
    FRotator Rotator() const { return FRotator(Tilt.X, Yaw, Tilt.Y); }
    /** The command for an acceleration (degrees): pitch nose-down to go forward, roll into a turn to the right. */
    static FVector2D CommandFor(const FVector& AccelLocal, const FSCCargoDroneArt::FMotion& Cfg);
};

struct UNREALCITIZEN_API FSCCargoDockFootprint
{
    /** Lattice cells a dock mesh of MeshSizeCm takes: round(size / cell) per axis, at least 1. */
    static FIntVector Cells(const FVector& MeshSizeCm, const FVector& CellCm);
    /** The uniform scale at which the mesh's footprint fills those cells (the smaller of the plan axes' ratios). */
    static double Scale(const FVector& MeshSizeCm, const FIntVector& Cells, const FVector& CellCm);
    /** The quarter turns a dock may take looking for clear bay exits: all four when its footprint is square, else only
     *  half turns (its long axis stays along its slot's). */
    static TArray<int32> Turns(const FIntVector& Cells);
};
