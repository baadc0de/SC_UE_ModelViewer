// WP-O1-4c-2: the DSP of the game's Wwise bus effects (SCAudioBusFx.h) in the plugin, so a packaged game needs no
// library of ours for it (they ran on scbridge's sc_eq / sc_limiter / sc_meter before). The EQ and the meters' smoothing
// are UE's signal processing; the limiter is ours (UE's lets transients through, below). Each kernel takes the effect's
// exported parameters; where a UE processor differs from the Wwise effect, the difference is UE's:
// - FSCBusEq: the Wwise Parametric EQ's three bands as UE biquads (Audio::FBiquadFilter, RBJ). A band's Q becomes UE's
//   bandwidth in octaves at its frequency, so low / high / band pass, notch and peaking match the cookbook's Q forms;
//   UE's shelves have a fixed slope (S = 1) and take no Q; UE designs a band at 5 Hz .. 0.45 of the rate (sc_eq: 1 Hz ..
//   0.499; the game's bands lie at 20 Hz .. 20 kHz, inside UE's range at 48 kHz).
// - FSCBusLimiter: the Wwise Peak Limiter (`CAkPeakLimiterFXParams`: threshold, ratio, look-ahead, release, output
//   level, channel link), the C++ of scbridge's sc_limiter (crates/scbridge-ffi/src/audio_dsp.rs, WP-A1 A1-2). The block
//   has no attack: the look-ahead is the time the limiter has to reach a peak's gain. Each frame's target gain (the ratio
//   applied above the threshold) is held as the minimum over the next look-ahead frames, released toward unity with the
//   release time constant and averaged over the look-ahead, the audio delayed by look-ahead - 1 frames: the gain has
//   reached a peak's when the peak leaves the delay, so the output never exceeds threshold * (peak / threshold)^(1 /
//   ratio), whatever the buffer size. Channels share one gain when linked; an LFE channel it does not process is left out
//   of the detector and passed at unity, still delayed. (UE's limiter, Audio::FDynamicsProcessor, follows the signal:
//   single-sample clicks passed 11.8 dB over that ceiling, which made A1-2 replace it; owner's decision 2026-11-24: ours.)
// - FSCBusMeter: the Wwise Meter: per 10 ms block (carried across buffers, so the buffer size does not matter) the
//   block's peak or RMS in dBFS (plus the downstream offset), clamped to [min, max]; UE's envelope follower
//   (Audio::FEnvelopeFollower at one sample per block, analog, its times scaled to give exactly the meter's coefficient
//   1 - exp(-block / time)) smooths those levels in dB, and the value holds for the hold time after its last rise (for ever with infinite hold).
//   This is the value the meter writes to its game parameter. (Following the samples themselves instead reads a slow
//   meter near the average level, not the peaks: 5-11 dB under the game's on the C2's ambience meters.)
// Every kernel is built off the audio thread and processes interleaved float buffers on one thread at a time.
#pragma once
#include "CoreMinimal.h"

namespace Audio
{
class FBiquadFilter;
class FEnvelopeFollower;
} // namespace Audio

class SCBRIDGE_API FSCBusEq
{
public:
    /** Lfe: an LFE channel the EQ leaves as it is (or INDEX_NONE). All bands off, output 0 dB. */
    FSCBusEq(float Rate, int32 Channels, int32 Lfe);
    ~FSCBusEq();
    /** Band 0..2: Wwise AkFilterType 0..6 (low pass, high pass, band pass, notch, low shelf, high shelf, peaking). */
    void SetBand(int32 Band, int32 Type, float FrequencyHz, float Q, float GainDb, bool bOn);
    void SetOutput(float Db);
    void Process(float* Data, int32 Frames);

private:
    float Rate;
    int32 Channels, Lfe;
    TUniquePtr<Audio::FBiquadFilter> Bands[3];
    bool bOn[3] = {false, false, false};
    float Output = 1.f;
    static constexpr int32 HeldFrames = 1024;
    TArray<float> Held; // the LFE samples of a chunk (allocated with the kernel)
};

class SCBRIDGE_API FSCBusLimiter
{
public:
    struct FStats
    {
        float GainMinDb = 0.f; // the lowest gain applied in the buffer (0 when nothing was reduced)
        float InPeakDb = -240.f, OutPeakDb = -240.f;
    };
    /** Parameters as the effect's (look-ahead 0..1 s, ratio >= 1); invalid ones make a limiter that passes its input. */
    FSCBusLimiter(float Rate, int32 Channels, float ThresholdDb, float Ratio, float LookAheadS, float ReleaseS, float OutputDb, bool bLink, int32 Lfe);
    ~FSCBusLimiter();
    void Process(float* Data, int32 Frames, FStats& Stats);
    /** Its latency in frames (the look-ahead less one: the gain ramp spans the look-ahead and ends on the peak). */
    int32 Latency() const { return bValid ? Look - 1 : 0; }

private:
    /** A sliding-window minimum over the last Len values (a monotonic deque in a ring of (index, value)). */
    struct FWindowMin
    {
        uint64 Len = 1;
        TArray<TPair<uint64, double>> Ring;
        int32 Head = 0, Count = 0;
        double Push(uint64 Index, double V);
    };
    /** One detector: the gain path its channels share. */
    struct FPath
    {
        FWindowMin Hold;
        double Released = 1.0;
        TArray<double> Box; // the moving average over the look-ahead
        double BoxSum = 0.0;
        int32 BoxAt = 0;
    };
    double Target(double X) const { return X <= Threshold ? 1.0 : FMath::Pow(Threshold / X, Slope); }
    double Step(FPath& P, double T);
    bool bValid = false, bLink = true;
    int32 Channels = 0, Lfe = INDEX_NONE, Look = 1;
    double Threshold = 1.0, Slope = 0.0, ReleaseCoeff = 1.0, Output = 1.0;
    TArray<FPath> Paths;
    TArray<float> Delay; // look-ahead - 1 frames (interleaved ring)
    int32 DelayAt = 0;
    uint64 Frame = 0, Summed = 0;
    TArray<double> Gains; // per path, the current frame's (allocated once)
};

class SCBRIDGE_API FSCBusMeter
{
public:
    FSCBusMeter(float Rate, int32 Channels, float AttackS, float ReleaseS, float HoldS, float MinDb, float MaxDb, bool bRms, bool bInfiniteHold);
    ~FSCBusMeter();
    /** Added to each level (dB): the downstream volume a meter that applies it measures after. */
    void SetOffset(float Db) { OffsetDb = Db; }
    /** Measure Frames interleaved frames (not modified); the meter's value (dB) after the last complete block. */
    float Process(const float* Data, int32 Frames);

private:
    void CloseBlock();
    int32 Channels, Block;
    double BlockS;
    TUniquePtr<Audio::FEnvelopeFollower> Follower; // over (level - MinDb), one sample per block
    float HoldS, MinDb, MaxDb, OffsetDb = 0.f;
    bool bRms, bInfiniteHold;
    float Value;
    double Held = 0.0; // seconds since the value last rose
    int32 Filled = 0;  // the block being measured: frames so far, peak, sum of squares
    float Peak = 0.f;
    double SumSq = 0.0;
};
