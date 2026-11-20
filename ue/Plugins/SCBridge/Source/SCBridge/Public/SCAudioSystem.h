// WP-A1: the viewer's audio player. Plays the game's sounds from a package's audio manifest (SCAudio.h) through UE audio:
// - USCAudioWave: a procedural sound wave that decodes a package's Ogg media on the audio thread (scbridge sc_audio_*);
// - FSCAudioScene: one imported vehicle or environment's audio — manifest, media bytes, impulse responses, the placed
//   spots and areas, and the entities whose class has sound effects;
// - USCAudioSubsystem: posts triggers (the game's container logic over time: random / sequence, step / continuous with
//   crossfades, blends, loops), drives each voice's gain, pitch and reverb send every frame (props, random draws, RTPC
//   curves at the listener-relative and area values, the attenuation curves at the emitter distance), runs the areas and
//   spots, mirrors the game's bus hierarchy as submixes and each room environment's Convolution Reverb as a submix effect
//   over the game's impulse response. Console: sc.Audio.List, sc.Audio.Play, sc.Audio.Stop, sc.Audio.Mute, sc.Audio.Log,
//   sc.Audio.Rooms, sc.Audio.Record, sc.Audio.Sends, sc.Audio.Meter. Logs: AUDIO_SCENE, AUDIO_EMITTERS, AUDIO_PLAY,
//   AUDIO_ROOM, AUDIO_REVERB, AUDIO_LIMITER, AUDIO_METER.
#pragma once
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
struct sc_audio_stream;

UCLASS()
class SCBRIDGE_API USCAudioWave : public USoundWaveProcedural
{
    GENERATED_BODY()
public:
    /** Stream Ogg (shared, not copied per voice beyond the decoder's own copy) `Loops` times (0 = forever). */
    bool Init(const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>& Ogg, int32 Loops);
    virtual int32 OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples) override;
    virtual Audio::EAudioMixerStreamDataFormat::Type GetGeneratedPCMDataFormat() const override
    {
        return Audio::EAudioMixerStreamDataFormat::Float;
    }
    virtual void BeginDestroy() override;
    int32 Channels = 0, Rate = 0;

private:
    FCriticalSection Lock;
    sc_audio_stream* Stream = nullptr;
    int32 LoopsLeft = 0; // remaining plays after the current one; INDEX_NONE = forever
    bool bEnded = false;
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
    int32 Post(FSCAudioScene& Scene, const FString& Trigger, const FSCAudioPlace& Where);
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
    void UpdateAreasAndSpots(FSCAudioScene& S, double Now);
    float RtpcValue(const FSCAudioScene& S, const FInstance& I, const FString& Name, const FVector& Emitter) const;
    FVector PlacePosition(const FInstance& I) const;
    FString EnvironmentAt(const FSCAudioScene& S, const FInstance& I, const FVector& World) const;
    USoundSubmix* BusSubmix(FSCAudioScene& S, uint32 Bus);
    USoundSubmix* EnvironmentSubmix(FSCAudioScene& S, const FString& Environment);
    void LogScene(const FSCAudioScene& S) const;

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
