// WP-O1-4c-2: the bus effect kernels of SCAudioBusKernels.h on UE's signal processing.
#include "SCAudioBusKernels.h"
#include "DSP/EnvelopeFollower.h"
#include "DSP/Filter.h"

namespace
{
float ToDb(float X) { return X <= 1e-12f ? -240.f : 20.f * FMath::LogX(10.f, X); }
float Lin(float Db) { return FMath::Pow(10.f, Db / 20.f); }
double AsinhOf(double X) { return FMath::Loge(X + FMath::Sqrt(X * X + 1.0)); }
int32 LfeIndex(int32 Lfe, int32 Channels) { return Lfe >= 0 && Lfe < Channels ? Lfe : INDEX_NONE; }
} // namespace

// ------------------------------------------------------------------------------------------------ parametric EQ
FSCBusEq::FSCBusEq(float InRate, int32 InChannels, int32 InLfe) : Rate(InRate), Channels(InChannels), Lfe(LfeIndex(InLfe, InChannels))
{
    for (TUniquePtr<Audio::FBiquadFilter>& B : Bands)
    {
        B = MakeUnique<Audio::FBiquadFilter>();
        B->Init(Rate, Channels, Audio::EBiquadFilter::ParametricEQ, 1000.f, 1.f, 0.f);
    }
    Held.SetNumZeroed(HeldFrames);
}

FSCBusEq::~FSCBusEq() = default;

void FSCBusEq::SetBand(int32 Band, int32 Type, float FrequencyHz, float Q, float GainDb, bool bInOn)
{
    if (Band < 0 || Band >= 3) return;
    static const Audio::EBiquadFilter::Type Types[] = {Audio::EBiquadFilter::Lowpass, Audio::EBiquadFilter::Highpass, Audio::EBiquadFilter::Bandpass,
                                                       Audio::EBiquadFilter::Notch,   Audio::EBiquadFilter::LowShelf, Audio::EBiquadFilter::HighShelf,
                                                       Audio::EBiquadFilter::ParametricEQ};
    bOn[Band] = bInOn && Type >= 0 && Type < UE_ARRAY_COUNT(Types) && FMath::IsFinite(FrequencyHz) && FMath::IsFinite(Q) && FMath::IsFinite(GainDb);
    if (!bOn[Band]) return;
    // UE's alpha is sin(w) sinh(ln 2 / 2 * BW * w / sin(w)); the cookbook's Q form is sin(w) / (2 Q): the bandwidth that
    // gives the same alpha at the frequency UE designs the band at (FBiquadFilter::ClampCutoffFrequency: 5 Hz .. 0.45 of
    // the rate; sc_eq allowed 1 Hz .. 0.499).
    const float F = FMath::Clamp(FrequencyHz, 5.f, 0.9f * (Rate / 2.f));
    const double W = 2.0 * PI * F / Rate;
    const double Bandwidth = 2.0 * AsinhOf(1.0 / (2.0 * FMath::Max(double(Q), 1e-3))) * FMath::Sin(W) / (UE_LN2 * W);
    Bands[Band]->SetParams(Types[Type], F, float(Bandwidth), GainDb);
}

void FSCBusEq::SetOutput(float Db) { Output = Lin(Db); }

void FSCBusEq::Process(float* Data, int32 Frames)
{
    // In chunks of the LFE buffer allocated with the kernel (nothing is allocated on the render thread).
    for (int32 At = 0; At < Frames; At += HeldFrames)
    {
        const int32 N = FMath::Min(HeldFrames, Frames - At);
        float* Chunk = Data + int64(At) * Channels;
        if (Lfe != INDEX_NONE)
            for (int32 F = 0; F < N; ++F) Held[F] = Chunk[F * Channels + Lfe];
        for (int32 B = 0; B < 3; ++B)
            if (bOn[B]) Bands[B]->ProcessAudio(Chunk, N * Channels, Chunk);
        if (Output != 1.f)
            for (int32 I = 0; I < N * Channels; ++I) Chunk[I] *= Output;
        if (Lfe != INDEX_NONE)
            for (int32 F = 0; F < N; ++F) Chunk[F * Channels + Lfe] = Held[F];
    }
}

// ------------------------------------------------------------------------------------------------ peak limiter
double FSCBusLimiter::FWindowMin::Push(uint64 Index, double V)
{
    const int32 Cap = Ring.Num();
    while (Count > 0 && Ring[(Head + Count - 1) % Cap].Value >= V) --Count;
    Ring[(Head + Count) % Cap] = {Index, V};
    ++Count;
    while (Ring[Head].Key + Len <= Index)
    {
        Head = (Head + 1) % Cap;
        --Count;
    }
    return Ring[Head].Value;
}

FSCBusLimiter::FSCBusLimiter(float Rate, int32 InChannels, float ThresholdDb, float Ratio, float LookAheadS, float ReleaseS, float OutputDb, bool bInLink,
                             int32 InLfe)
{
    const bool bFinite = FMath::IsFinite(ThresholdDb) && FMath::IsFinite(Ratio) && FMath::IsFinite(LookAheadS) && FMath::IsFinite(ReleaseS) && FMath::IsFinite(OutputDb);
    bValid = bFinite && Rate >= 8000.f && Rate <= 384000.f && InChannels >= 1 && InChannels <= 64 && Ratio >= 1.f && LookAheadS >= 0.f && LookAheadS <= 1.f &&
             ReleaseS >= 0.f;
    Channels = FMath::Max(1, InChannels);
    if (!bValid) return;
    Lfe = LfeIndex(InLfe, Channels);
    bLink = bInLink;
    Look = FMath::Max(1, FMath::RoundToInt32(double(LookAheadS) * Rate));
    Threshold = FMath::Pow(10.0, double(ThresholdDb) / 20.0);
    Slope = 1.0 - 1.0 / double(Ratio);
    const double ReleaseFrames = double(ReleaseS) * Rate;
    ReleaseCoeff = ReleaseFrames > 0.0 ? 1.0 - FMath::Exp(-1.0 / ReleaseFrames) : 1.0;
    Output = FMath::Pow(10.0, double(OutputDb) / 20.0);
    Paths.SetNum(bLink ? 1 : Channels);
    for (FPath& P : Paths)
    {
        P.Hold.Len = uint64(Look);
        P.Hold.Ring.SetNumZeroed(Look + 1);
        P.Box.Init(1.0, Look);
        P.BoxSum = double(Look);
    }
    Delay.SetNumZeroed(FMath::Max(1, Look - 1) * Channels);
    Gains.Init(1.0, Paths.Num());
}

FSCBusLimiter::~FSCBusLimiter() = default;

double FSCBusLimiter::Step(FPath& P, double T)
{
    const double Held = P.Hold.Push(Frame, T);
    P.Released = Held <= P.Released ? Held : P.Released + (Held - P.Released) * ReleaseCoeff;
    P.BoxSum += P.Released - P.Box[P.BoxAt];
    P.Box[P.BoxAt] = P.Released;
    P.BoxAt = (P.BoxAt + 1) % Look;
    return P.BoxSum / Look;
}

void FSCBusLimiter::Process(float* Data, int32 Frames, FStats& Stats)
{
    double GainMin = 1.0, InPeak = 0.0, OutPeak = 0.0;
    if (!bValid)
    {
        for (int32 I = 0; I < Frames * Channels; ++I) InPeak = OutPeak = FMath::Max(InPeak, double(FMath::Abs(Data[I])));
        Stats = {0.f, ToDb(float(InPeak)), ToDb(float(OutPeak))};
        return;
    }
    const int32 DelayFrames = Look - 1;
    for (int32 F = 0; F < Frames; ++F)
    {
        float* Fr = Data + int64(F) * Channels;
        if (bLink)
        {
            double X = 0.0;
            for (int32 C = 0; C < Channels; ++C)
                if (C != Lfe) X = FMath::Max(X, double(FMath::Abs(Fr[C])));
            InPeak = FMath::Max(InPeak, X);
            Gains[0] = Step(Paths[0], Target(X));
        }
        else
            for (int32 C = 0; C < Channels; ++C)
            {
                if (C == Lfe) continue;
                const double X = FMath::Abs(Fr[C]);
                InPeak = FMath::Max(InPeak, X);
                Gains[C] = Step(Paths[C], Target(X));
            }
        for (int32 C = 0; C < Channels; ++C)
        {
            const double G = C == Lfe ? 1.0 : Gains[bLink ? 0 : C];
            GainMin = FMath::Min(GainMin, G);
            float Delayed = Fr[C];
            if (DelayFrames > 0)
            {
                float& Slot = Delay[DelayAt * Channels + C];
                Delayed = Slot;
                Slot = Fr[C];
            }
            const float Y = float(double(Delayed) * G * Output);
            OutPeak = FMath::Max(OutPeak, double(FMath::Abs(Y)));
            Fr[C] = Y;
        }
        if (DelayFrames > 0) DelayAt = (DelayAt + 1) % DelayFrames;
        ++Frame;
        // Re-sum the moving average now and then so rounding cannot drift.
        if (++Summed >= (1u << 20))
        {
            Summed = 0;
            for (FPath& P : Paths)
            {
                P.BoxSum = 0.0;
                for (const double B : P.Box) P.BoxSum += B;
            }
        }
    }
    Stats.GainMinDb = GainMin >= 1.0 ? 0.f : ToDb(float(GainMin));
    Stats.InPeakDb = ToDb(float(InPeak));
    Stats.OutPeakDb = ToDb(float(OutPeak));
}

// ------------------------------------------------------------------------------------------------ meter
FSCBusMeter::FSCBusMeter(float Rate, int32 InChannels, float AttackS, float ReleaseS, float InHoldS, float InMinDb, float InMaxDb, bool bInRms,
                         bool bInInfiniteHold)
    : Channels(FMath::Max(1, InChannels)), Block(FMath::Max(1, int32(Rate) / 100)), BlockS(double(Block) / Rate), HoldS(FMath::Max(InHoldS, 0.f)),
      MinDb(InMinDb), MaxDb(FMath::Max(InMinDb, InMaxDb)), bRms(bInRms), bInfiniteHold(bInInfiniteHold), Value(InMinDb)
{
    Audio::FEnvelopeFollowerInitParams P;
    P.SampleRate = float(1.0 / BlockS); // one sample per block
    P.NumChannels = 1;
    // UE's analog coefficient is exp(-T / time) with T = FAttackRelease::AnalogTimeConstant (1.00239343) block periods per
    // time; the Wwise meter's (sc_meter) is exp(-block / time): the times scaled by T give exactly it. A zero time is an
    // immediate one (UE's coefficient is then 0).
    constexpr float UEAnalogTimeConstant = 1.00239343f;
    P.AttackTimeMsec = FMath::Max(AttackS * 1000.f * UEAnalogTimeConstant, 1e-3f);
    P.ReleaseTimeMsec = FMath::Max(ReleaseS * 1000.f * UEAnalogTimeConstant, 1e-3f);
    P.Mode = Audio::EPeakMode::Peak; // the levels are already the block's
    P.bIsAnalog = true;
    Follower = MakeUnique<Audio::FEnvelopeFollower>(P);
    // Its first call allocates its work buffer: make it here (a zero level leaves it at MinDb, where it starts).
    const float Zero = 0.f;
    Follower->ProcessAudio(&Zero, 1);
}

FSCBusMeter::~FSCBusMeter() = default;

void FSCBusMeter::CloseBlock()
{
    const float Level = bRms ? float(FMath::Sqrt(SumSq / (double(Block) * Channels))) : Peak;
    const float LevelDb = FMath::Clamp(ToDb(Level) + OffsetDb, MinDb, MaxDb);
    // The follower rises toward a higher level; a lower one starts its release only after the hold time (never with
    // infinite hold): until then it is given the value it holds.
    float Input = LevelDb;
    if (LevelDb >= Value) Held = 0.0;
    else if (bInfiniteHold) Input = Value;
    else
    {
        Held += BlockS;
        if (Held <= HoldS) Input = Value;
    }
    const float Shifted = Input - MinDb; // >= 0: the follower's peak mode takes the magnitude
    Follower->ProcessAudio(&Shifted, 1);
    Value = FMath::Clamp(MinDb + Follower->GetEnvelopeValues()[0], MinDb, MaxDb);
    Filled = 0;
    Peak = 0.f;
    SumSq = 0.0;
}

float FSCBusMeter::Process(const float* Data, int32 Frames)
{
    for (int32 F = 0; F < Frames; ++F)
    {
        const float* Frame = Data + int64(F) * Channels;
        for (int32 C = 0; C < Channels; ++C)
        {
            Peak = FMath::Max(Peak, FMath::Abs(Frame[C]));
            SumSq += double(Frame[C]) * Frame[C];
        }
        if (++Filled == Block) CloseBlock();
    }
    return Value;
}
