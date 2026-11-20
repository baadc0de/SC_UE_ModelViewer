// WP-A1 A1-2: the game's bus effects on a bus submix, run on the audio render thread from the manifest's data.
// - FSCRtpcBank: the game parameters shared by every scene (Wwise game parameters are global): meters write them on
//   the render thread (the last write stands, as a game parameter set twice keeps the later value), bus effects and
//   voices read them; the parameters some meter of a loaded scene drives are known before any voice starts;
// - FSCBusDsp: one bus's effect chain configuration in slot order — Gain, Parametric EQ (its parameters at the RTPC
//   values), Meter (ballistics -> the game parameter it writes; a meter that applies the downstream volume measures
//   after the bus's and its parents' static levels), Peak Limiter — and the bus's output-stage gain curves on
//   meter-driven parameters (side-chain ducking) applied after the chain; FSCBusDspState holds one effect instance's
//   kernels (scbridge sc_meter / sc_eq / sc_limiter), built at the instance's Init, never in its audio callback (only a
//   later channel-count change rebuilds them); its trace (sc.Audio.MixTrace) records per buffer, in a preallocated
//   ring, the peaks, the limiter's gain and ratio bound, the meters' values, the side-chain gain and the EQ bands;
// - USCBusFxPreset / FSCBusFx: the UE submix effect; each instance finds its chain by the preset's unique id.
// Not run (listed by Describe): slots whose bypass depends on a game state (no state is modelled), meters scoped to a
// game object, curves with an unsupported scaling or accumulation, and kinds without a port (compressor, RoomVerb, ...).
// The LFE channel of a 5.1 / 7.1 submix (UE's channel order: index 3) is left out where a slot does not process LFE.
#pragma once
#include <atomic>
#include "CoreMinimal.h"
#include "Sound/SoundEffectSubmix.h"
#include "SCAudio.h"
#include "SCAudioBusFx.generated.h"

struct sc_limiter;
struct sc_meter;
struct sc_eq;

/** One game parameter's live value (written on the render thread, read anywhere). */
struct FSCLiveRtpc
{
    FString Name;
    float Default = 0.f;
    std::atomic<float> Value{0.f};
    std::atomic<int32> Writers{0}; // meters registered to write it (information only)
    float Read() const { return Value.load(std::memory_order_relaxed); }
};
using FSCLiveRtpcRef = TSharedRef<FSCLiveRtpc, ESPMode::ThreadSafe>;

class SCBRIDGE_API FSCRtpcBank
{
public:
    /** The live value of Name (created at Default on first use; game thread). */
    FSCLiveRtpcRef Get(const FString& Name, float Default);
    /** Name is written by a meter (game thread; before voices that read it start). */
    void MarkDriven(const FString& Name, float Default);
    const TSet<FString>& Driven() const { return DrivenNames; }
    const FSCLiveRtpc* Find(const FString& Name) const;

private:
    TMap<FString, FSCLiveRtpcRef> Values;
    TSet<FString> DrivenNames;
};

/** One effect instance's kernels and running values. */
struct SCBRIDGE_API FSCBusDspState
{
    ~FSCBusDspState();
    float Rate = 0;
    int32 Channels = 0;
    struct FKernel
    {
        sc_meter* Meter = nullptr;
        sc_eq* Eq = nullptr;
        sc_limiter* Limiter = nullptr;
        uint32 EqKey = 0;
        float LastGain = 1.f; // gain slots
        float LastLimiterInPeak = 0.f;
    };
    TArray<FKernel> Kernels; // per slot of the chain
    float LastPost = 1.f;
    bool bWasTracing = false;
};

/** One bus's effect chain (configuration shared by its effect instances). */
class SCBRIDGE_API FSCBusDsp
{
public:
    /** DownstreamDb: the static levels from this bus's output to the master's (for meters that apply them). */
    FSCBusDsp(uint32 InBus, const FSCAudioBus& Bus, const TMap<FString, float>& Defaults, FSCRtpcBank& Bank, float DownstreamDb);
    /** A meter slot this player runs (and so a game parameter it drives). */
    static bool RunsMeter(const FSCAudioBusEffect& Fx);
    /** Kernels for one effect instance (allocates; not on the audio callback path). */
    TUniquePtr<FSCBusDspState> MakeState(float Rate, int32 Channels) const;
    void Process(FSCBusDspState& State, float* Data, int32 Frames);
    /** A one-line description of what runs and what does not (AUDIO_BUSFX). */
    FString Describe() const;
    bool HasWork() const { return !Slots.IsEmpty() || bSideChain; }
    int32 LowestSlot() const { return Slots.IsEmpty() ? 0 : Slots[0].Fx.Slot; }
    int32 HighestSlot() const { return Slots.IsEmpty() ? 0 : Slots.Last().Fx.Slot; }

    uint32 Bus = 0;
    std::atomic<bool> bLimiterOn{true}; // sc.Audio.Limiter

    // ---- trace (sc.Audio.MixTrace): fixed-size rows in a ring preallocated by BeginTrace (game thread)
    static constexpr int32 MaxMeters = 4;
    static constexpr int32 MaxEqSlots = 4;
    struct FTraceRow
    {
        double Time = 0; // seconds since the trace began (render wall clock)
        float InPeak = 0, OutPeak = 0, LimiterInPeak = 0, LimiterOutPeak = 0, LimiterGainDb = 0, LimiterBound = 0, PostDb = 0;
        int32 NumMeters = 0;
        float Meters[MaxMeters] = {};
        int32 NumEq = 0; // EQ slots recorded in this row (when their bands changed, or the trace's first row)
        struct FEq { int32 Slot = 0; FSCAudioBusEffect::FBand Bands[3]; } Eq[MaxEqSlots];
    };
    void BeginTrace(double StartSeconds);
    /** Stop and take the rows recorded (game thread). */
    TArray<FTraceRow> EndTrace(int32& OutDropped);
    TArray<FString> MeterNames; // the game parameter of each meter slot, in slot order (trace columns)
    /** The limiter's ratio bound for an input peak (linear): threshold * (peak / threshold)^(1/ratio) * output. */
    float LimiterCeilingOf(float InPeak) const;
    bool HasLimiter() const { return Limiter.bOn; }

private:
    struct FSlot
    {
        FSCAudioBusEffect Fx;
        TSharedPtr<FSCLiveRtpc, ESPMode::ThreadSafe> MeterOut; // owned: an instance may outlive the bank
    };
    TArray<FSlot> Slots;
    TArray<FString> NotRun;
    FSCAudioBus BusData;
    FSCAudioLimiter Limiter;
    TMap<FString, float> Defaults;
    TMap<FString, FSCLiveRtpcRef> Rtpc; // every parameter this chain reads
    float DownstreamDb = 0;
    bool bSideChain = false;
    float Value(const FString& Name) const;

    std::atomic<bool> bTrace{false};
    double TraceStart = 0;
    FCriticalSection TraceLock; // the render thread only try-locks it
    TArray<FTraceRow> Trace;    // capacity reserved by BeginTrace; never grows on the render thread
    std::atomic<int32> TraceDropped{0};
};

USTRUCT()
struct SCBRIDGE_API FSCBusFxSettings
{
    GENERATED_BODY()
    TSharedPtr<FSCBusDsp, ESPMode::ThreadSafe> Dsp;
};

class SCBRIDGE_API FSCBusFx : public FSoundEffectSubmix
{
public:
    virtual void Init(const FSoundEffectSubmixInitData& InitData) override;
    virtual void OnPresetChanged() override;
    virtual void OnProcessAudio(const FSoundEffectSubmixInputData& In, FSoundEffectSubmixOutputData& Out) override;

private:
    TSharedPtr<FSCBusDsp, ESPMode::ThreadSafe> Dsp;
    TUniquePtr<FSCBusDspState> State;
    float Rate = 48000.f;
    int32 DeviceChannels = 2;
};

UCLASS(MinimalAPI)
class USCBusFxPreset : public USoundEffectSubmixPreset
{
    GENERATED_BODY()
public:
    EFFECT_PRESET_METHODS(SCBusFx)
    /** Give the preset its chain: the effect instances a submix creates find it by the preset's unique id. */
    SCBRIDGE_API void SetDsp(const TSharedPtr<FSCBusDsp, ESPMode::ThreadSafe>& Dsp);
    /** The chain of the preset with this unique id (any thread). */
    static TSharedPtr<FSCBusDsp, ESPMode::ThreadSafe> FindDsp(uint32 PresetUniqueId);
    virtual void BeginDestroy() override;
    UPROPERTY(Transient)
    FSCBusFxSettings Settings;
};
