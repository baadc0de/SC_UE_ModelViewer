// WP-A1: the package audio manifest (`Packages/<name>/audio.json`, contract: vendor/StarBreaker/docs/
// decomposed-export-contract.md §Audio) and the game's playback rules it encodes, free of any world or audio device:
// - FSCAudioCurve: a Wwise curve (points with per-segment interpolation) and its dB scaling;
// - FSCAudioManifest: triggers, the shared play-node table, attenuations, buses, media, RTPC defaults, environments
//   (convolution impulse responses), rooms and entity-class sound effects, parsed from the JSON;
// - FSCAudioContainerState / SCAudioPick: random (weights, avoid-repeat, shuffle) and sequence selection;
// - FSCAudioGain: the gain, pitch and aux-send composition of one voice (props, random draws, RTPC curves, bus chain);
// - FSCAudioBusEffect: a bus effect slot (Parametric EQ, Meter, Peak Limiter, Gain) and its parameters at RTPC values;
//   FSCAudioBus::RtpcGainDeltaDb: a bus's level change from RTPCs that move away from their defaults (side-chains).
// Nothing here names a trigger, media or asset: every value comes from the manifest.
#pragma once
#include "CoreMinimal.h"
#include "SCAudioFilters.h"

/** A Wwise curve: (x, y, interpolation) points; `Scaling` 2 (dB) stores 10^(dB/20) - 1. */
struct SCBRIDGE_API FSCAudioCurve
{
    uint8 Scaling = 0;
    TArray<FVector3f> Points; // X, Y, interpolation (AkCurveInterpolation) of the segment starting at the point
    bool IsEmpty() const { return Points.IsEmpty(); }
    /** The curve at X, clamped to its ends (the exporter's mix::evaluate). */
    float Eval(float X) const;
    /** The value as dB when this is a gain curve (dB-scaled: 20 log10(1 + y); else y). */
    float EvalDb(float X) const { return ToDb(Scaling, Eval(X)); }
    static float ToDb(uint8 Scaling, float Y);
    static float ShapeAt(int32 Interp, float T);
};

/** An RTPC curve on a node, a bus or an action context. */
struct SCBRIDGE_API FSCAudioRtpcCurve
{
    FString Rtpc;  // ATL name
    uint8 Type = 0; // 0 game parameter (the only kind a player drives)
    FString Param; // volume, pitch, lpf, hpf, make_up_gain, game_aux_send_volume, output_bus_volume, ...; fx_param_<id>
    uint8 Accum = 0; // AkRtpcAccum: 0 none / 1 exclusive (replaces), 2 additive, 3 multiply, 4 boolean, 5 maximum
    int32 FxParam = INDEX_NONE; // an effect's own parameter id (Param "fx_param_<id>")
    FSCAudioCurve Curve;
    /** The curve's value at X in the parameter's units: dB-scaled (2) as dB, log-scaled (3) as 10^y, dB-to-linear (4)
     *  as 10^(y/20), unscaled (0) as y. */
    float ValueAt(float X) const;
    /** The parameter at X combined with its authored value Base by the curve's accumulation. */
    float Apply(float Base, float X) const;
    /** Scaling and accumulation this player implements (others are reported, not applied). */
    bool IsSupported() const { return (Curve.Scaling == 0 || (Curve.Scaling >= 2 && Curve.Scaling <= 4)) && Accum <= 5; }
};

/** A node's own properties (Wwise sums them down the tree). */
struct SCBRIDGE_API FSCAudioProps
{
    float VolumeDb = 0, PitchCents = 0, Lpf = 0, Hpf = 0, MakeUpDb = 0, GameAuxDb = 0, OutputBusDb = 0, InitialDelayS = 0;
    struct FRange { FString Param; float Min = 0, Max = 0; };
    TArray<FRange> Random;
    TArray<FSCAudioRtpcCurve> Rtpc;
    void Parse(const TSharedPtr<class FJsonObject>& O);
};

enum class ESCAudioNodeKind : uint8 { Sound, Random, Sequence, Blend, Switch, Mixer };

struct SCBRIDGE_API FSCAudioNode
{
    uint32 Id = 0;
    ESCAudioNodeKind Kind = ESCAudioNodeKind::Sound;
    TArray<int32> Children;
    FSCAudioProps Props;
    // Inherited by override (resolved by the exporter).
    uint32 Bus = 0, Attenuation = 0;
    bool bSpatial = false, bUseGameAux = false;
    // Sound
    uint32 Media = 0;
    FString SourcePlugin;
    int32 LoopCount = 1; // 0 = forever
    // Random / sequence
    TArray<float> Weights;
    int32 AvoidRepeat = 0;
    bool bShuffle = false, bContinuous = false;
    FString Transition; // disabled, crossfade_amp, crossfade_power, delay, sample_accurate, trigger_rate
    float TransitionS = 0;
    FVector2f TransitionRandomS = FVector2f::ZeroVector; // a random offset in [min, max] s added to each transition
    bool bGlobal = false; // one selection history for every emitter (else one per emitter)
    // Blend: per layer, the RTPC and each child's linear-gain curve over the RTPC value (children by index).
    struct FLayer
    {
        FString Rtpc;
        TArray<TPair<int32, FSCAudioCurve>> Children;
    };
    TArray<FLayer> Layers;
};

struct SCBRIDGE_API FSCAudioAction
{
    int32 Root = INDEX_NONE;
    float DelayS = 0, FadeInS = 0;
    FSCAudioProps Context; // the target's ancestors, summed
};

struct SCBRIDGE_API FSCAudioTrigger
{
    FString Status, Reason, DurationType;
    float RadiusMax = 0;
    TArray<FSCAudioAction> Play;
    struct FOther { int32 ActionType = 0; uint32 Target = 0; float DelayS = 0, FadeS = 0; };
    TArray<FOther> Other;
    bool IsPlayable() const { return Status == TEXT("playable"); }
};

struct SCBRIDGE_API FSCAudioAttenuation
{
    float MaxDistance = 0;
    FSCAudioCurve VolumeDry, VolumeAuxGame, Lpf, Hpf;
};

/** A Wwise Peak Limiter's parameters (`CAkPeakLimiterFXParams`). */
struct SCBRIDGE_API FSCAudioLimiter
{
    bool bOn = false, bChannelLink = false, bProcessLfe = true;
    float ThresholdDb = 0, Ratio = 0, LookAheadS = 0, ReleaseS = 0, OutputDb = 0;
};

/** A bus effect slot (not bypassed). Kind: parametric_eq, meter, peak_limiter, gain, convolution_reverb, others. */
struct SCBRIDGE_API FSCAudioBusEffect
{
    FString Kind;
    int32 Slot = 0;
    uint32 Id = 0;
    /** State groups that bypass this slot in some of their states (group -> those states): on or off by the game's
     *  state, which is not modelled. */
    TArray<uint32> StateGroups;
    TMap<uint32, TArray<uint32>> StateBypass;
    bool bEqProcessLfe = true;
    FSCAudioLimiter Limiter;
    /** Wwise Parametric EQ band (AkFilterType 0 low pass .. 6 peaking). */
    struct FBand { int32 Type = 0; float GainDb = 0, FrequencyHz = 1000, Q = 1; bool bOn = false; };
    FBand Bands[3];
    float OutputDb = 0;
    /** Wwise Meter: ballistics and the game parameter it writes. */
    struct FMeter
    {
        float AttackS = 0, ReleaseS = 0, HoldS = 0, MinDb = -60, MaxDb = 0;
        bool bRms = false, bDownstream = false, bInfiniteHold = false, bGlobal = true;
        FString GameParameter;
    } Meter;
    float GainDb = 0; // Wwise Gain (full band)
    TArray<FSCAudioRtpcCurve> Rtpc; // curves on the effect's own parameters
    bool IsStateDependent() const { return !StateGroups.IsEmpty(); }
    /** EQ band B at the RTPC values. Wwise Parametric EQ parameter ids (from the C2 / hangar banks: log-scaled curves of
     *  frequencies on 5b+2, on/off steps on 5b+4, dB curves on 5b+1 and 15): band b's type 5b, gain 5b+1, frequency
     *  5b+2, Q 5b+3, on 5b+4; the output level 15. */
    FBand BandAt(int32 B, const TFunctionRef<float(const FString&)>& Rtpc) const;
    float OutputAt(const TFunctionRef<float(const FString&)>& Rtpc) const;
    /** A Gain effect's full-band gain (parameter 0) at the RTPC values. */
    float GainAt(const TFunctionRef<float(const FString&)>& Rtpc) const;
};

struct SCBRIDGE_API FSCAudioBus
{
    uint32 Id = 0, Parent = 0;
    FString Name;
    bool bAux = false;
    float VoiceDb = 0, BusDb = 0, MakeUpDb = 0, PitchCents = 0;
    FSCAudioFilterLevels OutputFilters;
    /** The bus's output-stage gain curves (bus volume, output-bus volume, make-up) at their RTPCs' defaults (the
     *  manifest's `rtpcs`); curves whose RTPC has no default are left out (inactive). */
    float CurveDefaultDb = 0;
    // A Convolution Reverb slot (an environment's aux bus).
    bool bConvolution = false;
    uint32 IrMedia = 0;
    float WetDb = 0, DryDb = 0, PreDelayMs = 0;
    /** The bus's first Peak Limiter (for logs; each slot keeps its own parameters). */
    using FLimiter = FSCAudioLimiter;
    FSCAudioLimiter Limiter;
    /** Applied effect slots in slot order (bypassed slots are left out). Gain slots run in the bus's chain. */
    TArray<FSCAudioBusEffect> Effects;
    /** The bus's own RTPC curves (gains, filters). */
    TArray<FSCAudioRtpcCurve> Rtpc;
    /** The bus's own level on its mix (bus volume, make-up, output-stage gain curves at their RTPC defaults), dB. */
    float MixDb() const { return BusDb + MakeUpDb + CurveDefaultDb; }
    /** Is P a gain property of the voice stage (`volume`) or of the output stage (bus volume, output-bus volume,
     *  make-up)? */
    static bool IsVoiceGain(const FString& P);
    static bool IsOutputGain(const FString& P);
    /** The output-stage gain curves at Value relative to their values at Defaults (dB): the side-chain change. Curves
     *  whose RTPC has no default are inactive. */
    float OutputCurveDeltaDb(const TFunctionRef<float(const FString&)>& Value, const TMap<FString, float>& Defaults) const;
};

struct SCBRIDGE_API FSCAudioMedia
{
    FString File, Codec, Error;
    int32 Channels = 0, Rate = 0;
    float Seconds = 0;
};

struct SCBRIDGE_API FSCAudioEnvironment
{
    FString Name, Record, IrFile, Error;
    uint32 AuxBus = 0, IrMedia = 0;
    int32 IrChannels = 0, IrRate = 0, IrFrames = 0;
    float IrPeakDb = 0, T20 = 0, T30 = 0;
    FVector2f Size = FVector2f::ZeroVector, Interiority = FVector2f::ZeroVector;
};

struct SCBRIDGE_API FSCAudioRoom
{
    FString Name, Environment, Rule, Selection; // Selection: explicit, inferred_in_range, inferred_nearest_range
    float VolumeM3 = 0, SizeM = 0, Interiority = 0;
};

/** One entity-class sound effect (`on` trigger: fired by its tag; `on` tag: plays while the tag is set). */
struct SCBRIDGE_API FSCAudioEffect
{
    FString On, Tag, TagPath, Label, Trigger, Bone;
    FVector Offset = FVector::ZeroVector; // CryEngine axes, metres
};

struct SCBRIDGE_API FSCAudioClass
{
    FString Name;
    TArray<FSCAudioEffect> Effects;
    TArray<TPair<FString, FString>> Actions; // (property path, trigger)
    /** Effects whose tag (its own name, the last element of its path) is Tag, of kind On ("" = any). */
    TArray<const FSCAudioEffect*> EffectsFor(const FString& Tag, const FString& On = FString()) const;
};

/** An Init.bnk game parameter's authored ramp from its current to target value. */
struct SCBRIDGE_API FSCAudioRamp
{
    float Up = 0.f, Down = 0.f;
    uint8 Type = 0;
    float Advance(float Current, float Target, float Seconds) const;
};

struct FSCAudioGameContext
{
    TMap<FString, float> Rtpcs;
    TMap<FString, FString> States;
};

SCBRIDGE_API uint32 SCAudioWwiseId(const FString& Name);

struct SCBRIDGE_API FSCAudioManifest
{
    TMap<FString, FSCAudioTrigger> Triggers;
    TArray<FSCAudioNode> Nodes;
    TMap<uint32, FSCAudioAttenuation> Attenuations;
    TMap<uint32, FSCAudioBus> Buses;
    TMap<uint32, FSCAudioMedia> Media;
    TMap<uint32, FSCAudioRamp> GameParameters;
    TMap<FString, FSCAudioGameContext> GameContexts;
    TMap<FString, uint32> RtpcIds;
    TMap<FString, float> RtpcDefaults; // ATL default, else the Wwise game-parameter default (Init.bnk STMG)
    TMap<FString, FSCAudioEnvironment> Environments;
    TMap<FString, FSCAudioRoom> Rooms;
    TMap<FString, FSCAudioClass> Classes; // by class GUID (lower case)
    FString RootClass;
    int32 SpotCount = 0, AreaCount = 0;

    static bool Parse(const FString& Json, FSCAudioManifest& Out, FString& Error);
    /** The bus chain of Bus, nearest first (parents resolved within the manifest). */
    TArray<const FSCAudioBus*> BusChain(uint32 Bus) const;
    /** The environment a room uses ("" for none). */
    FString RoomEnvironment(const FString& Room) const;
};

/** Per-container selection state (Wwise keeps it per container; the exporter's flag says "global" for the C2's). */
struct SCBRIDGE_API FSCAudioContainerState
{
    TArray<int32> History; // last picks, most recent last
    TArray<int32> Bag;     // shuffle: indices not yet played in this pass
    int32 Next = 0;        // sequence: next child
};

/** Random container: weighted pick among children not in the last AvoidRepeat picks (at most Num - 1 avoided);
 *  shuffle draws without replacement. Sequence: the next child, wrapping. Returns a child index (not a node index). */
SCBRIDGE_API int32 SCAudioPick(const FSCAudioNode& Node, FSCAudioContainerState& State, FRandomStream& Random);

/** Same-group area priority from the CryEngine area-manager lineage, as documented in WP-A2. */
struct SCBRIDGE_API FSCAudioAreaOverlap
{
    bool bLowerInside = false, bHigherInside = false;
    bool bHigherHullInLower = false, bLowerHullInHigher = false;
    double LowerDistance = 0., HigherDistance = 0., HullSeparation = 0., HigherFadeDistance = 0.;
    TOptional<float> ExclusiveFade() const;
};

/** The gain, pitch and aux-send composition of one voice at fixed RTPC values (dB / cents). */
struct SCBRIDGE_API FSCAudioGain
{
    float VolumeDb = 0, PitchCents = 0, GameAuxDb = 0;
    FSCAudioFilterLevels Filters;
    /** Add one props block: its own values, one draw of its random ranges and its gain/pitch RTPC curves at Rtpc. */
    void Add(const FSCAudioProps& Props, const TFunctionRef<float(const FString&)>& Rtpc, FRandomStream* Random);
    /** Add a bus chain's voice volumes and voice-stage curves at their RTPC defaults (the bus mix levels go to the
     *  submixes); curves on an RTPC that Skip names are left to the caller (they change while playing). */
    void AddBusVoice(const TArray<const FSCAudioBus*>& Chain, const TFunctionRef<bool(const FString&)>& Skip, const TMap<FString, float>& Defaults);
    static float Linear(float Db) { return Db <= -199.f ? 0.f : FMath::Pow(10.f, Db / 20.f); }
};
