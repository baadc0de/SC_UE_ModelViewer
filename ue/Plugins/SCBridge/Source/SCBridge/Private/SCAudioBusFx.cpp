// WP-A1 A1-2: the bus effect chain of SCAudioBusFx.h. Every parameter comes from the manifest; the DSP kernels are
// scbridge's (sc_limiter, sc_meter, sc_eq) so they are unit-tested in Rust.
#include "SCAudioBusFx.h"
#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "scbridge.h"

// ------------------------------------------------------------------------------------------------ game parameters
FSCLiveRtpcRef FSCRtpcBank::Get(const FString& Name, float Default)
{
    if (const FSCLiveRtpcRef* Found = Values.Find(Name)) return *Found;
    FSCLiveRtpcRef R = MakeShared<FSCLiveRtpc, ESPMode::ThreadSafe>();
    R->Name = Name;
    R->Default = Default;
    R->Value.store(Default, std::memory_order_relaxed);
    Values.Add(Name, R);
    return R;
}

void FSCRtpcBank::MarkDriven(const FString& Name, float Default)
{
    Get(Name, Default);
    DrivenNames.Add(Name);
}

const FSCLiveRtpc* FSCRtpcBank::Find(const FString& Name) const
{
    const FSCLiveRtpcRef* R = Values.Find(Name);
    return R ? &R->Get() : nullptr;
}

// ------------------------------------------------------------------------------------------------ the chain
namespace
{
/** UE's channel order puts the LFE at index 3 of a 5.1 / 7.1 layout; other layouts have none. */
int32 LfeChannel(int32 Channels) { return Channels == 6 || Channels == 8 ? 3 : -1; }
float Lin(float Db) { return FMath::Pow(10.f, Db / 20.f); }
} // namespace

FSCBusDspState::~FSCBusDspState()
{
    for (FKernel& K : Kernels)
    {
        sc_meter_free(K.Meter);
        sc_eq_free(K.Eq);
        sc_limiter_free(K.Limiter);
    }
}

bool FSCBusDsp::RunsMeter(const FSCAudioBusEffect& Fx)
{
    return Fx.Kind == TEXT("meter") && !Fx.IsStateDependent() && Fx.Meter.bGlobal && !Fx.Meter.GameParameter.IsEmpty();
}

FSCBusDsp::FSCBusDsp(uint32 InBus, const FSCAudioBus& B, const TMap<FString, float>& InDefaults, FSCRtpcBank& Bank, float InDownstreamDb)
    : Bus(InBus), BusData(B), Defaults(InDefaults), DownstreamDb(InDownstreamDb)
{
    auto Need = [&](const FString& Name) {
        if (!Rtpc.Contains(Name))
        {
            const float* D = Defaults.Find(Name);
            Rtpc.Add(Name, Bank.Get(Name, D ? *D : 0.f));
        }
    };
    for (const FSCAudioBusEffect& Fx : B.Effects)
    {
        if (Fx.IsStateDependent())
        {
            NotRun.Add(FString::Printf(TEXT("slot%d=%s(state-dependent)"), Fx.Slot, *Fx.Kind));
            continue;
        }
        const bool bRun = Fx.Kind == TEXT("parametric_eq") || Fx.Kind == TEXT("gain") || RunsMeter(Fx) || (Fx.Kind == TEXT("peak_limiter") && Fx.Limiter.bOn);
        if (!bRun)
        {
            // The convolution reverb runs as UE's own effect on the same submix.
            if (Fx.Kind != TEXT("convolution_reverb")) NotRun.Add(FString::Printf(TEXT("slot%d=%s(no port)"), Fx.Slot, *Fx.Kind));
            continue;
        }
        FSlot& S = Slots.AddDefaulted_GetRef();
        S.Fx = Fx;
        for (const FSCAudioRtpcCurve& C : Fx.Rtpc)
        {
            if (!C.IsSupported()) NotRun.Add(FString::Printf(TEXT("slot%d.%s(%s scaling %d accum %d)"), Fx.Slot, *C.Param, *C.Rtpc, C.Curve.Scaling, C.Accum));
            Need(C.Rtpc);
        }
        if (Fx.Kind == TEXT("meter"))
        {
            const float* D = Defaults.Find(Fx.Meter.GameParameter);
            FSCLiveRtpcRef Out = Bank.Get(Fx.Meter.GameParameter, D ? *D : Fx.Meter.MinDb);
            Bank.MarkDriven(Fx.Meter.GameParameter, Out->Default);
            Out->Writers.fetch_add(1, std::memory_order_relaxed);
            S.MeterOut = Out;
            MeterNames.Add(Fx.Meter.GameParameter);
        }
        if (Fx.Kind == TEXT("peak_limiter") && !Limiter.bOn) Limiter = Fx.Limiter;
    }
    for (const FSCAudioRtpcCurve& C : B.Rtpc)
        if (C.Type == 0 && FSCAudioBus::IsOutputGain(C.Param) && Defaults.Contains(C.Rtpc))
        {
            // Live from the start: a scene loaded later may bring the meter that drives it.
            Need(C.Rtpc);
            bSideChain = true;
        }
}

float FSCBusDsp::Value(const FString& Name) const
{
    if (const FSCLiveRtpcRef* R = Rtpc.Find(Name)) return (*R)->Read();
    const float* D = Defaults.Find(Name);
    return D ? *D : 0.f;
}

float FSCBusDsp::LimiterCeilingOf(float InPeak) const
{
    const float Thr = Lin(Limiter.ThresholdDb), Out = Lin(Limiter.OutputDb);
    return (InPeak <= Thr || Limiter.Ratio < 1.f ? InPeak : Thr * FMath::Pow(InPeak / Thr, 1.f / Limiter.Ratio)) * Out;
}

TUniquePtr<FSCBusDspState> FSCBusDsp::MakeState(float InRate, int32 InChannels) const
{
    TUniquePtr<FSCBusDspState> St = MakeUnique<FSCBusDspState>();
    St->Rate = InRate;
    St->Channels = InChannels;
    const int32 Lfe = LfeChannel(InChannels);
    auto V = [this](const FString& N) { return Value(N); };
    // Gains start at their current values (a new instance does not ramp in from unity).
    St->LastPost = bSideChain ? Lin(BusData.OutputCurveDeltaDb(V, Defaults)) : 1.f;
    St->Kernels.SetNum(Slots.Num());
    for (int32 I = 0; I < Slots.Num(); ++I)
    {
        const FSCAudioBusEffect& Fx = Slots[I].Fx;
        FSCBusDspState::FKernel& K = St->Kernels[I];
        if (Fx.Kind == TEXT("gain")) K.LastGain = Lin(Fx.GainAt(V));
        if (Fx.Kind == TEXT("meter"))
        {
            K.Meter = sc_meter_new(uint32(InRate), uint32(InChannels), Fx.Meter.AttackS, Fx.Meter.ReleaseS, Fx.Meter.HoldS, Fx.Meter.MinDb, Fx.Meter.MaxDb, Fx.Meter.bRms ? 1u : 0u,
                                   Fx.Meter.bInfiniteHold);
            // A meter that applies the downstream volume measures after this bus's and its parents' levels.
            if (Fx.Meter.bDownstream) sc_meter_set_offset(K.Meter, DownstreamDb);
        }
        else if (Fx.Kind == TEXT("parametric_eq"))
            K.Eq = sc_eq_new(uint32(InRate), uint32(InChannels), Fx.bEqProcessLfe ? -1 : Lfe);
        else if (Fx.Kind == TEXT("peak_limiter"))
            K.Limiter = sc_limiter_new(uint32(InRate), uint32(InChannels), Fx.Limiter.ThresholdDb, Fx.Limiter.Ratio, Fx.Limiter.LookAheadS, Fx.Limiter.ReleaseS,
                                       Fx.Limiter.OutputDb, Fx.Limiter.bChannelLink, Fx.Limiter.bProcessLfe ? -1 : Lfe);
    }
    return St;
}

FString FSCBusDsp::Describe() const
{
    FString Out;
    auto V = [this](const FString& N) { return Value(N); };
    for (const FSlot& S : Slots)
    {
        Out += FString::Printf(TEXT(" slot%d=%s"), S.Fx.Slot, *S.Fx.Kind);
        if (S.Fx.Kind == TEXT("parametric_eq"))
            for (int32 B = 0; B < 3; ++B)
            {
                const FSCAudioBusEffect::FBand Band = S.Fx.BandAt(B, V);
                if (Band.bOn) Out += FString::Printf(TEXT("[b%d type=%d f=%.0f q=%.2f g=%+.1f]"), B, Band.Type, Band.FrequencyHz, Band.Q, Band.GainDb);
            }
        if (S.Fx.Kind == TEXT("gain")) Out += FString::Printf(TEXT("[%+.1f dB]"), S.Fx.GainAt(V));
        if (S.Fx.Kind == TEXT("meter"))
            Out += FString::Printf(TEXT("[->%s %s att=%.2f rel=%.2f hold=%.2f%s range=%.0f..%.0f%s]"), *S.Fx.Meter.GameParameter, S.Fx.Meter.bRms ? TEXT("rms") : TEXT("peak"),
                                   S.Fx.Meter.AttackS, S.Fx.Meter.ReleaseS, S.Fx.Meter.HoldS, S.Fx.Meter.bInfiniteHold ? TEXT("(infinite)") : TEXT(""), S.Fx.Meter.MinDb,
                                   S.Fx.Meter.MaxDb, S.Fx.Meter.bDownstream ? *FString::Printf(TEXT(" downstream=%+.1f"), DownstreamDb) : TEXT(""));
        if (S.Fx.Kind == TEXT("peak_limiter"))
            Out += FString::Printf(TEXT("[thr=%.2f ratio=%.0f look=%.3f rel=%.3f out=%.1f link=%d lfe=%d]"), S.Fx.Limiter.ThresholdDb, S.Fx.Limiter.Ratio, S.Fx.Limiter.LookAheadS,
                                   S.Fx.Limiter.ReleaseS, S.Fx.Limiter.OutputDb, S.Fx.Limiter.bChannelLink, S.Fx.Limiter.bProcessLfe);
    }
    if (bSideChain) Out += TEXT(" side_chain");
    for (const FString& N : NotRun) Out += TEXT(" not_run:") + N;
    return Out;
}

void FSCBusDsp::BeginTrace(double StartSeconds)
{
    FScopeLock G(&TraceLock);
    Trace.Reset();
    Trace.Reserve(1 << 15); // 5+ minutes of buffers; rows past it are dropped and counted, the array never grows
    TraceStart = StartSeconds;
    TraceDropped = 0;
    bTrace = true;
}

TArray<FSCBusDsp::FTraceRow> FSCBusDsp::EndTrace(int32& OutDropped)
{
    bTrace = false;
    FScopeLock G(&TraceLock);
    OutDropped = TraceDropped.load();
    TArray<FTraceRow> Rows = MoveTemp(Trace);
    Trace.Reset();
    return Rows;
}

void FSCBusDsp::Process(FSCBusDspState& St, float* Data, int32 Frames)
{
    const int32 Channels = St.Channels;
    const int32 N = Frames * Channels;
    auto V = [this](const FString& Name) { return Value(Name); };
    auto Ramp = [&](float& Last, float Target) {
        if (Last == 1.f && Target == 1.f) return;
        for (int32 F = 0; F < Frames; ++F)
        {
            const float G = Last + (Target - Last) * (float(F + 1) / Frames);
            for (int32 C = 0; C < Channels; ++C) Data[F * Channels + C] *= G;
        }
        Last = Target;
    };
    const bool bTracing = bTrace.load(std::memory_order_relaxed);
    if (bTracing && !St.bWasTracing)
        for (FSCBusDspState::FKernel& K : St.Kernels) K.EqKey = 0; // the trace's first row records the EQ bands
    St.bWasTracing = bTracing;
    FTraceRow Row;
    if (bTracing)
        for (int32 I = 0; I < N; ++I) Row.InPeak = FMath::Max(Row.InPeak, FMath::Abs(Data[I]));
    for (int32 I = 0; I < Slots.Num() && I < St.Kernels.Num(); ++I)
    {
        const FSlot& S = Slots[I];
        FSCBusDspState::FKernel& K = St.Kernels[I];
        if (S.Fx.Kind == TEXT("gain"))
            Ramp(K.LastGain, Lin(S.Fx.GainAt(V))); // the full-band gain at its slot (the LFE gain parameter is not exported)
        else if (K.Meter)
        {
            const float M = sc_meter_process(K.Meter, Data, size_t(Frames));
            if (S.MeterOut) S.MeterOut->Value.store(M, std::memory_order_relaxed);
            if (bTracing && Row.NumMeters < MaxMeters) Row.Meters[Row.NumMeters++] = M;
        }
        else if (K.Eq)
        {
            uint32 Key = 0;
            FSCAudioBusEffect::FBand Bands[3];
            for (int32 B = 0; B < 3; ++B)
            {
                Bands[B] = S.Fx.BandAt(B, V);
                Key = HashCombine(Key, HashCombine(GetTypeHash(Bands[B].bOn ? Bands[B].FrequencyHz : -1.f), HashCombine(GetTypeHash(Bands[B].GainDb), GetTypeHash(Bands[B].Q))));
            }
            const float OutDb = S.Fx.OutputAt(V);
            Key = HashCombine(Key, GetTypeHash(OutDb)) | 1u;
            if (Key != K.EqKey)
            {
                K.EqKey = Key;
                for (int32 B = 0; B < 3; ++B)
                    sc_eq_set_band(K.Eq, uint32(B), uint32(FMath::Max(0, Bands[B].Type)), Bands[B].FrequencyHz, Bands[B].Q, Bands[B].GainDb, Bands[B].bOn);
                sc_eq_set_output(K.Eq, OutDb);
                if (bTracing && Row.NumEq < MaxEqSlots)
                {
                    FTraceRow::FEq& E = Row.Eq[Row.NumEq++];
                    E.Slot = S.Fx.Slot;
                    for (int32 B = 0; B < 3; ++B) E.Bands[B] = Bands[B];
                }
            }
            sc_eq_process(K.Eq, Data, size_t(Frames));
        }
        else if (K.Limiter && bLimiterOn.load(std::memory_order_relaxed))
        {
            sc_limiter_stats Stats{};
            sc_limiter_process(K.Limiter, Data, size_t(Frames), &Stats);
            // The ratio bound of this buffer's output: its peaks entered within a look-ahead (this buffer or the last).
            const float In = FMath::Pow(10.f, Stats.input_peak_db / 20.f);
            if (bTracing)
            {
                Row.LimiterInPeak = In;
                Row.LimiterOutPeak = FMath::Pow(10.f, Stats.output_peak_db / 20.f);
                Row.LimiterGainDb = Stats.gain_min_db;
                Row.LimiterBound = LimiterCeilingOf(FMath::Max(In, K.LastLimiterInPeak));
            }
            K.LastLimiterInPeak = In;
        }
    }
    // Side-chain: the output-stage gain curves on meter-driven parameters, relative to their values at the defaults
    // (those are in the submix's static level).
    const float PostDb = bSideChain ? BusData.OutputCurveDeltaDb(V, Defaults) : 0.f;
    Ramp(St.LastPost, Lin(PostDb));
    if (bTracing)
    {
        for (int32 I = 0; I < N; ++I) Row.OutPeak = FMath::Max(Row.OutPeak, FMath::Abs(Data[I]));
        Row.PostDb = PostDb;
        Row.Time = FPlatformTime::Seconds() - TraceStart;
        if (TraceLock.TryLock())
        {
            if (Trace.Num() < Trace.Max()) Trace.Add(Row);
            else TraceDropped.fetch_add(1, std::memory_order_relaxed);
            TraceLock.Unlock();
        }
        else
            TraceDropped.fetch_add(1, std::memory_order_relaxed);
    }
}

// ------------------------------------------------------------------------------------------------ submix effect
namespace
{
// The chain of each preset, by the preset's unique id. A submix creates its effect instances with no preset data and
// without a preset change, and the preset's first instance resets its settings copy from the saved settings (which a
// game build never writes), so the instance looks its chain up here.
FCriticalSection GDspLock;
TMap<uint32, TWeakPtr<FSCBusDsp, ESPMode::ThreadSafe>> GDsps;
} // namespace

void USCBusFxPreset::SetDsp(const TSharedPtr<FSCBusDsp, ESPMode::ThreadSafe>& Dsp)
{
    FScopeLock G(&GDspLock);
    GDsps.Add(GetUniqueID(), Dsp);
    Settings.Dsp = Dsp;
}

TSharedPtr<FSCBusDsp, ESPMode::ThreadSafe> USCBusFxPreset::FindDsp(uint32 PresetUniqueId)
{
    FScopeLock G(&GDspLock);
    const TWeakPtr<FSCBusDsp, ESPMode::ThreadSafe>* D = GDsps.Find(PresetUniqueId);
    return D ? D->Pin() : nullptr;
}

void USCBusFxPreset::BeginDestroy()
{
    {
        FScopeLock G(&GDspLock);
        GDsps.Remove(GetUniqueID());
    }
    Super::BeginDestroy();
}

void FSCBusFx::Init(const FSoundEffectSubmixInitData& InitData)
{
    Rate = InitData.SampleRate;
    // The device's channel count, so the kernels exist before the first buffer (no allocation in the callback).
    if (FAudioDeviceManager* M = FAudioDeviceManager::Get())
        if (FAudioDeviceHandle H = M->GetAudioDevice(InitData.DeviceID))
            DeviceChannels = static_cast<Audio::FMixerDevice*>(H.GetAudioDevice())->GetNumDeviceChannels();
    Dsp = USCBusFxPreset::FindDsp(InitData.ParentPresetUniqueId);
    if (Dsp) State = Dsp->MakeState(Rate, DeviceChannels);
}

void FSCBusFx::OnPresetChanged()
{
    if (TSharedPtr<FSCBusDsp, ESPMode::ThreadSafe> D = USCBusFxPreset::FindDsp(GetParentPresetId()); D && D != Dsp)
    {
        Dsp = D;
        State = Dsp->MakeState(Rate, DeviceChannels);
    }
}

void FSCBusFx::OnProcessAudio(const FSoundEffectSubmixInputData& In, FSoundEffectSubmixOutputData& Out)
{
    *Out.AudioBuffer = *In.AudioBuffer;
    if (!Dsp) return;
    if (!State || State->Channels != In.NumChannels) State = Dsp->MakeState(Rate, In.NumChannels); // a layout change only
    Dsp->Process(*State, Out.AudioBuffer->GetData(), In.NumFrames);
}
