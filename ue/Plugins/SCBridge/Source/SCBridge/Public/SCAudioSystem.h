// WP-A1: the viewer's audio player. Plays the game's sounds from a package's audio manifest (SCAudio.h) through UE audio:
// - USCAudioWave: a procedural sound wave that decodes a package's Ogg media on the audio render thread as the mixer
//   pulls it (scbridge sc_audio_*: pull-driven, so a game-thread hitch cannot starve it), looping over the media's loop
//   region as Wwise does, and reporting its own end and render health (FSCAudioWaveHealth);
// - FSCAudioScene: one imported vehicle or environment's audio — manifest, media bytes, impulse responses, the placed
//   spots and areas, and the entities whose class has sound effects;
// - USCAudioSubsystem: posts triggers (the game's container logic over time: random / sequence, step / continuous with
//   crossfades, blends, loops), drives each voice's gain, pitch and reverb send every frame (props, random draws, RTPC
//   curves at the listener-relative and area values, the attenuation curves at the emitter distance), runs the areas and
//   spots, mirrors the game's bus hierarchy as submixes and each room environment's Convolution Reverb as a submix effect
//   over the game's impulse response, and each bus's own effects (EQ, meters writing game parameters, the Peak Limiter,
//   side-chain gains: SCAudioBusFx.h). Console: sc.Audio.List, sc.Audio.Play, sc.Audio.Stop, sc.Audio.Mute, sc.Audio.Log,
//   sc.Audio.Rooms, sc.Audio.Record, sc.Audio.Sends, sc.Audio.Meter, sc.Audio.Health, sc.Audio.Hitch, sc.Audio.MixTrace,
//   sc.Audio.Rtpc. Logs: AUDIO_SCENE, AUDIO_BUSFX, AUDIO_MIX_TRACE, AUDIO_RTPC,
//   AUDIO_EMITTERS, AUDIO_PLAY, AUDIO_ROOM, AUDIO_REVERB, AUDIO_LIMITER, AUDIO_METER, AUDIO_HEALTH, AUDIO_VOICE_GAP,
//   AUDIO_VOICE_RESOURCED, AUDIO_VOICE_CUT.
#pragma once
#include <atomic>
#include "CoreMinimal.h"
#include "Sound/SoundWaveProcedural.h"
#include "Subsystems/WorldSubsystem.h"
#include "SCAudio.h"
#include "SCLightAreas.h"
#include "SCAudioSystem.generated.h"

class ASCVehicle;
class UAudioComponent;
class USoundSubmix;
class USCRigComponent;
class FSCPackageSource;
class FSCBusMeter;
class FSCLimiterTrace;
class FSCRtpcBank;
class FAudioDevice;
class FSCBusDsp;
struct sc_audio_stream;

/** A voice's render-side health, written on the audio render thread as the mixer pulls the wave and read on the game
 *  thread (AUDIO_HEALTH): a gap is a pull later than the audio it was last given lasts (the voice starved or paused);
 *  a source start after the first is the voice losing its mixer source and getting a new one (stolen, culled when
 *  silent, restarted). */
struct FSCAudioWaveHealth
{
    std::atomic<int32> Calls{0}, Sources{0}, Gaps{0}, MaxGapUs{0}, MaxDecodeUs{0};
    std::atomic<int32> MaxResourceGapUs{0}; // longest silence a source change left (old source dry -> new source start)
    std::atomic<int64> Frames{0}, DecodeNs{0};
    std::atomic<uint64> FirstCycles{0}, LastCycles{0}; // FPlatformTime::Cycles64 of the first / latest pull
    std::atomic<int32> LastFrames{0};                    // frames the latest pull returned
    std::atomic<bool> bFinished{false};                  // every pass played and the mixer has consumed the last of it
    // Where the voice is in its play time: at audio clock PullClock (s) its next frame is PullFrame (negative while a
    // scheduled start is still ahead). Two voices of one media compare their positions at a common clock.
    std::atomic<double> PullClock{-1.0};
    std::atomic<int64> PullFrame{0};
};

UCLASS()
class SCBRIDGE_API USCAudioWave : public USoundWaveProcedural
{
    GENERATED_BODY()
public:
    /** Stream Ogg (shared, not copied per voice beyond the decoder's own copy) for `Loops` passes (0 = forever) over its
     *  loop region. `bWholeFileLoops` replays the whole media from its start instead (the pre-fix behaviour, kept for
     *  A/B measurement: sc.Audio.Legacy). */
    bool Init(const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>& Ogg, int32 Loops, bool bWholeFileLoops = false);
    virtual int32 OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples) override;
    virtual Audio::EAudioMixerStreamDataFormat::Type GetGeneratedPCMDataFormat() const override
    {
        return Audio::EAudioMixerStreamDataFormat::Float;
    }
    virtual void OnBeginGenerate() override;
    virtual void BeginDestroy() override;
    int32 Channels = 0, Rate = 0;
    /** The media's frames and loop region [LoopStart, LoopEnd). */
    uint64 MediaFrames = 0, LoopStart = 0, LoopEnd = 0;
    /** Seconds all passes last at the media rate (< 0: forever). */
    double PlaySeconds = -1;
    /** The voice's pitch ratio (set before it plays): how fast the mixer consumes the wave. */
    float Pitch = 1.f;
    /** Sample-aligned timing (set before it plays): the audio-clock time (s, the mixer's render clock) at which the
     *  voice's play time 0 falls. The first pull starts the audio there: silence until then, and, when bSeek, a seek
     *  past whatever the voice is late (a looping voice resumed "from elapsed time", as Wwise's virtual voices do), so
     *  voices of one media posted together play sample for sample in step. < 0: start at the first pull. */
    double ScheduledClock = -1.0;
    bool bSeek = false;
    FAudioDevice* ClockDevice = nullptr;
    FSCAudioWaveHealth Health;

private:
    FCriticalSection Lock;
    sc_audio_stream* Stream = nullptr;
    TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Bytes; // reopened per pass in the whole-file mode
    int32 LoopsLeft = 0;        // whole-file mode: remaining plays after the current one; INDEX_NONE = forever
    bool bWholeFile = false;
    bool bEnded = false;
    bool bAligned = false;     // the first pull placed the stream on the audio clock
    int64 LeadSilence = 0;     // frames of silence before the audio (a start scheduled after the first pull)
    int64 PlayFrame = 0;       // the play-time frame of the next frame pulled
};

/** A placed spot or area (scene `interiors[].audio_spots[]` / `audio_areas[]`), positions container-local metres. */
struct SCBRIDGE_API FSCAudioPlaced
{
    FString EntityId, Name, PlayTrigger, StopTrigger, Room;
    TArray<FVector> Positions; // the first is the entity's own
    TSharedPtr<class FJsonObject> Params;
    TArray<FString> Links;
    TArray<FSCRoomVolume> Shapes; // areas
    int32 Interior = INDEX_NONE;
    int32 Area = INDEX_NONE;      // spots: the governing area (index into Areas)
    double NextPlay = 0;          // playRandom spots
    double RetryAt = 0;           // cooldown after a placed trigger produced no voices
    bool bFired = false;          // a one-shot spot played since the listener came within reach
    int32 Instance = INDEX_NONE;  // the playing instance
    float Fade = 0;               // areas: area-fade value this frame
    bool bInside = false;         // areas: listener inside
    float LoggedFade = -1.f;      // areas: the fade last logged (AUDIO_AREA)
};

/** An entity whose class has sound effects, as placed. */
struct SCBRIDGE_API FSCAudioEntity
{
    FString ClassGuid;
    TWeakObjectPtr<USceneComponent> Anchor;
    TWeakObjectPtr<USCRigComponent> Rig;
    int32 Interior = INDEX_NONE;
    TMap<FString, TArray<int32>> TagInstances; // state tag -> the posts playing for it (one per effect)
};

struct SCBRIDGE_API FSCAudioScene
{
    TWeakObjectPtr<ASCVehicle> Vehicle;
    FString Name;
    FSCAudioManifest Manifest;
    TMap<uint32, TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>> MediaBytes;
    TMap<FString, TArray<float>> Ir; // environment -> interleaved samples
    TArray<TWeakObjectPtr<USceneComponent>> InteriorAnchors;
    TArray<FMatrix> AnchorToContainer; // per interior: anchor-local cm -> container-local metres
    TArray<FSCAudioPlaced> Spots, Areas;
    TMap<FString, FSCAudioEntity> Entities; // "<interior>/<entity id>"
    TMap<uint64, FSCAudioContainerState> Containers; // by node index (high word) and emitter (low word; 0 = global)
    int32 MediaMissing = 0;
    /** The audio-clock time the scene's placed loops began (all at once, as the game posts a zone's spots on entering
     *  it); < 0 until the subsystem sets it. */
    double EpochClock = -1.0;
    /** Read audio.json beside ScenePath and every media file and impulse response it lists. */
    static TSharedPtr<FSCAudioScene> Load(const FSCPackageSource& Source, const FString& ScenePath, FString& Error);
    /** A cooked level's audio (ASCVehicle::CookedAudio): load it from its loose files and register the scene. */
    static void LoadCooked(ASCVehicle* Vehicle);
    /** Add one interior's spots and areas (scene JSON arrays as text). */
    void AddInterior(int32 Interior, USceneComponent* Anchor, bool bHelperFrame, const TArray<FString>& SpotJson,
                     const TArray<FString>& AreaJson, TArray<FString>& Warnings);
    FVector ContainerToWorld(int32 Interior, const FVector& ContainerM) const;
    FVector WorldToContainer(int32 Interior, const FVector& World) const;
};

/** Where a posted trigger plays. */
struct SCBRIDGE_API FSCAudioPlace
{
    TWeakObjectPtr<USceneComponent> Attach; // follows this component (bone Socket if set) with Offset (UE cm, its frame)
    FName Socket;
    FVector Offset = FVector::ZeroVector;
    FVector World = FVector::ZeroVector;    // when not attached and not in an interior
    int32 Interior = INDEX_NONE;            // placed audio: follows its interior's anchor at ContainerPos (metres)
    FVector ContainerPos = FVector::ZeroVector;
    FString Room;                           // room GUID for the reverb send ("" = from the position)
    float VolumeDb = 0, PitchCents = 0;     // the emitter's own (a spot's volume and pitch, with their random draws)
    float AttenuationScale = 1;
    int32 Spot = INDEX_NONE, Area = INDEX_NONE; // area-fade RTPC source
    FString AreaFadeRtpc;                   // the RTPC the emitter drives with its area's fade
    bool bListener = false;                 // an area bed at the listener
    double EpochClock = -1.0;               // a placed loop: its looping voices keep the scene's timeline
    FString Label;                          // for logs: who played it
};

UCLASS()
class SCBRIDGE_API USCAudioSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    static USCAudioSubsystem* Get(const UObject* WorldContext);
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool IsTickableInEditor() const override { return false; }

    void AddScene(const TSharedRef<FSCAudioScene>& Scene);
    void RemoveScene(const ASCVehicle* Vehicle);
    TSharedPtr<FSCAudioScene> SceneOf(const ASCVehicle* Vehicle) const;

    /** Post Trigger of Scene at Where; returns the post's handle (one instance per play action, all stopped and queried
     *  through the handle; INDEX_NONE when nothing plays) and logs AUDIO_PLAY. */
    int32 Post(FSCAudioScene& Scene, const FString& Trigger, const FSCAudioPlace& Where, int32* CreatedVoices = nullptr);
    /** Stop every instance of a post (after DelayS, fading over FadeS). */
    void Stop(int32 Post, float FadeS = 0.f, float DelayS = 0.f);
    bool IsPlaying(int32 Instance) const;

    // ---------------------------------------------------------------- viewer events (no trigger names: effects by tag)
    /** Fire the trigger effects tagged Tag of entity EntityId in Interior of Vehicle (e.g. a lift phase, a press). */
    int32 FireEntityTag(ASCVehicle* Vehicle, int32 Interior, const FString& EntityId, const FString& Tag);
    /** Start / stop the state-tag effects (tag effects) tagged Tag of an entity. */
    void SetEntityTag(ASCVehicle* Vehicle, int32 Interior, const FString& EntityId, const FString& Tag, bool bSet);
    /** An item's own action (WP-A1 hooks: flashlight, weapon): the trigger of the item class's audio field Field (a
     *  DataCore field name, the last element of the action's property path), played at Attach. */
    int32 PlayItemAction(ASCVehicle* Kit, const FString& ItemEntity, const FString& Field, USceneComponent* Attach);
    /** A clip audio event crossed on Rig (route C). */
    int32 PlayClipEvent(ASCVehicle* Vehicle, USCRigComponent* Rig, const FString& Trigger, const FString& Bone, const FVector& OffsetCry);

    bool bMuted = false;
    bool bLog = true;
    bool bSends = true; // aux (reverb) sends on; sc.Audio.Sends 0 isolates the dry path for measurements
    bool bAmbience = true; // placed spots and areas run (sc.Audio.Ambience 0 stops them, for isolated measurements)
    /** Scenes whose voices are silenced (sc.Audio.MuteScene, A/B measurements): a substring of the scene name. */
    TArray<FString> MutedScenes;
    /** The master output trace (sc.Audio.LimiterTrace): the master bus's output (after its limiter) and the engine's
     *  main output per 10 ms block, peaks over the limiter threshold and samples over full scale (clipped at the
     *  device); Stop logs AUDIO_LIMITER_TRACE and writes the blocks as CSV. */
    void StartLimiterTrace();
    void StopLimiterTrace(const FString& CsvPath);
    /** Every bus limiter bypassed (0) or on (1) (sc.Audio.Limiter, A/B measurements). */
    void SetLimiterMode(int32 Mode);
    /** Trace every bus effect chain per render buffer (limiter gain and peaks, meter values, side-chain gains, EQ bands
     *  in force); Stop logs AUDIO_MIX_TRACE per bus and writes the rows as CSV. */
    void StartMixTrace();
    void StopMixTrace(const FString& CsvPath);
    /** The live game parameters (meter-written) and their values (sc.Audio.Rtpc). */
    FString DescribeRtpc() const;
    /** A bus of any scene's manifest. */
    const FSCAudioBus* BusOf(uint32 Bus) const;
    /** Listener this frame (world cm, forward / right unit vectors). */
    FVector ListenerPos = FVector::ZeroVector, ListenerFwd = FVector::ForwardVector, ListenerRight = FVector::RightVector;
    FString ListenerRoom, ListenerEnvironment;
    int32 VoiceCount() const;
    FString Describe() const;
    void StopAll();
    void LogRooms() const;
    /** Measure every bus submix's output (peak and RMS over the window) until StopMeter logs AUDIO_METER per bus. */
    void StartMeter();
    void StopMeter();

private:
    TArray<TPair<uint32, TSharedRef<FSCBusMeter, ESPMode::ThreadSafe>>> Meters;
    TSharedPtr<FSCLimiterTrace, ESPMode::ThreadSafe> LimiterTrace;
    TArray<TPair<TWeakObjectPtr<USoundSubmix>, TSharedRef<class FSCLimiterTap, ESPMode::ThreadSafe>>> LimiterTaps;
    TMap<uint32, TSharedPtr<FSCBusDsp, ESPMode::ThreadSafe>> BusDsps; // bus -> its effect chain (render thread)
    TSharedPtr<FSCRtpcBank> RtpcBank;                                // the game parameters meters write
    bool bMixTracing = false;                                        // sc.Audio.MixTrace running
    double MixTraceStart = 0;
    struct FVoice
    {
        TObjectPtr<UAudioComponent> Component;
        TObjectPtr<USCAudioWave> Wave;
        int32 Leaf = INDEX_NONE;
        double Start = 0, End = 0; // End: +inf for a voice that loops forever
        double FadeInEnd = 0, FadeOutStart = 0, FadeOutEnd = 0;
        bool bPower = false;       // crossfade_power curves
        float StaticDb = 0, StaticAux = 0;
        TArray<const FSCAudioRtpcCurve*> Dynamic; // curves on RTPCs that change while playing
        const FSCAudioAttenuation* Attenuation = nullptr;
        bool bSpatial = false, bAux = false, bSent = false;
        TObjectPtr<USoundSubmix> DrySubmix; // the voice's bus (a send: the base submix output is off)
        uint32 Bus = 0;
        float LastGain = 0, LastDistance = 0, LastSend = 0;
        bool bStopped = false;      // Stop() set End: the voice ends there (after its fade), not at its media's end
        int32 Plays = 0;            // Play() calls (more than one: the engine stopped the component and it restarted)
        double PlayWall = 0;        // FPlatformTime::Seconds of the first Play()
        int32 LoggedGaps = 0, LoggedSources = 1;
        float MinLinear = 1e9f, LastLinear = 0.f; // the voice volume set (lowest since the last source change; latest)
    };
    struct FRun // a continuous random / sequence container
    {
        int32 Node = INDEX_NONE;
        TArray<int32> Path; // node indices from the action root to Node (inclusive)
        int32 ItemsLeft = 0; // INDEX_NONE = forever
        double NextAt = 0;
        float RandomDb = 0, RandomCents = 0;
    };
    struct FInstance
    {
        int32 Id = INDEX_NONE;
        int32 Group = INDEX_NONE; // the post's handle: the first instance's id
        TWeakPtr<FSCAudioScene> Scene;
        FString Trigger;
        FSCAudioPlace Place;
        const FSCAudioAction* Action = nullptr;
        TArray<FVoice> Voices;
        TArray<FRun> Runs;
        FRandomStream Random;
        bool bStopping = false;
        double StopAt = 0;
        double PostTime = 0;       // the subsystem clock at the post
        double PostAudioClock = 0; // the audio clock its voices are scheduled against (the post's, plus a render lead)
        FString Environment;
        FVector Position = FVector::ZeroVector;
    };
    double Start(FInstance& I, FSCAudioScene& S, const TArray<int32>& Path, double T0, float FadeIn, float CrossfadeOut);
    double StartVoice(FInstance& I, FSCAudioScene& S, const TArray<int32>& Path, double T0, float FadeIn);
    /** When a continuous container's next item starts, after an item that played [ItemStart, ItemEnd). */
    double NextItemAt(const FSCAudioNode& N, FInstance& I, double ItemStart, double ItemEnd) const;
    /** A blend child's gain from its layers at the current RTPC values (dB; -200 when a layer silences it). */
    float BlendGainDb(const FSCAudioScene& S, const FInstance& I, const FSCAudioNode& Blend, int32 ChildPos, const FVector& Emitter) const;
    FSCAudioContainerState& ContainerState(FSCAudioScene& S, int32 Node, const FInstance& I);
    void SetSpatial(FInstance& I, bool bListener);
    void UpdateInstance(FInstance& I, double Now);
    void UpdateAreasAndSpots(FSCAudioScene& S, double Now, int32& StartsLeft);
    float RtpcValue(const FSCAudioScene& S, const FInstance& I, const FString& Name, const FVector& Emitter) const;
    FVector PlacePosition(const FInstance& I) const;
    FString EnvironmentAt(const FSCAudioScene& S, const FInstance& I, const FVector& World) const;
    USoundSubmix* BusSubmix(FSCAudioScene& S, uint32 Bus);
    USoundSubmix* EnvironmentSubmix(FSCAudioScene& S, const FString& Environment);
    void LogScene(const FSCAudioScene& S) const;
    /** Fold an ending voice's render health into the totals; logs AUDIO_VOICE_CUT for audio it did not play out. */
    void RetireVoice(const FInstance& I, FVoice& V, double Now);
    /** A voice's AUDIO_VOICE_GAP / AUDIO_VOICE_RESOURCED lines when its health counters moved. */
    void CheckVoice(const FInstance& I, FVoice& V);

public:
    /** Render-health totals since the last sc.Audio.Health reset (AUDIO_HEALTH). */
    struct FHealthTotals
    {
        int64 Voices = 0, Calls = 0, Frames = 0, DecodeNs = 0;
        int32 Gaps = 0, MaxGapUs = 0, MaxDecodeUs = 0, Resourced = 0, Restarted = 0, Cut = 0, CutAudible = 0, Unrendered = 0;
        double CutSeconds = 0, MaxStartLatency = 0, SumStartLatency = 0;
        int32 StartLatencies = 0;
        double Since = 0; // FPlatformTime::Seconds of the reset
    };
    FHealthTotals Totals;
    /** Log AUDIO_HEALTH (the totals plus the voices playing now); Reset starts a new window. */
    void LogHealth(bool bReset);
    /** Game-thread stalls for measurements (sc.Audio.Hitch): Count stalls of Ms each, one every EveryS seconds. */
    void ScheduleHitches(float Ms, int32 Count, float EveryS);

private:
    float HitchMs = 0, HitchEveryS = 0;
    int32 HitchesLeft = 0;
    double NextHitch = 0;

    TArray<TSharedRef<FSCAudioScene>> Scenes;
    TMap<int32, FInstance> Instances;
    int32 NextId = 1;
    double Clock = 0;
    UPROPERTY() TMap<uint32, TObjectPtr<USoundSubmix>> BusSubmixes;
    UPROPERTY() TMap<FString, TObjectPtr<USoundSubmix>> EnvSubmixes;
    UPROPERTY() TArray<TObjectPtr<UObject>> Keep; // waves and presets alive past their voices (audio-thread users)
    UPROPERTY() TObjectPtr<AActor> Host;
    FString LastLoggedRoom;
};
