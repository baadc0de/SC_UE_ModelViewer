// WP-O1-4c-1: FSCAudioStream's passes over a synthetic decoder (each sample is its media frame), and a SoundWave
// source built as the prep step builds one (PCM raw data, Bink Audio, inline, the loop region a loop cue point) decoded
// by UE: its passes and seeks land on the right media frames. No game data.
#include "SCAudioSource.h"
#include "Misc/AutomationTest.h"
#include "Sound/SoundWave.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
/** One channel whose sample at media frame F is F; ends at Frames (or at EndAt when earlier). */
class FRampDecoder final : public ISCAudioDecoder
{
public:
    FRampDecoder(uint64 InEnd) : End(InEnd) {}
    virtual bool Seek(uint64 Frame) override { Pos = Frame; return Frame <= End; }
    virtual int32 Read(float* Out, int32 Frames) override
    {
        int32 N = 0;
        for (; N < Frames && Pos < End; ++N) Out[N] = float(Pos++);
        return N;
    }
    uint64 Pos = 0, End;
};

class FRampSource final : public FSCAudioSource
{
public:
    uint64 RampEnd = 0;
    virtual TUniquePtr<ISCAudioDecoder> NewDecoder() const override { return MakeUnique<FRampDecoder>(RampEnd); }
    virtual const TCHAR* Kind() const override { return TEXT("ramp"); }
};

/** DecodedEnd: where the decoder ends (0: Frames); bKnown: the source tells it (a prepared wave) or not (the Ogg). */
FSCAudioSourcePtr Ramp(uint64 Frames, uint64 LoopStart, uint64 LoopEnd, uint64 DecodedEnd = 0, bool bKnown = false)
{
    TSharedRef<FRampSource> S = MakeShared<FRampSource>();
    S->Channels = 1;
    S->Rate = 48000;
    S->Frames = Frames;
    S->LoopStart = LoopStart;
    S->LoopEnd = LoopEnd;
    S->RampEnd = DecodedEnd ? DecodedEnd : Frames;
    S->DecodedEnd = bKnown ? S->RampEnd : 0;
    return S;
}

/** Everything the stream plays (at most Max frames), read in odd-sized blocks. */
TArray<float> Drain(FSCAudioStream& S, int32 Max = 100000)
{
    TArray<float> Out;
    float Block[37];
    while (Out.Num() < Max)
    {
        const int64 Got = S.Read(Block, FMath::Min(37, Max - Out.Num()));
        if (Got <= 0) break;
        Out.Append(Block, int32(Got));
    }
    return Out;
}

TArray<float> Expected(std::initializer_list<TPair<uint64, uint64>> Ranges)
{
    TArray<float> Out;
    for (const TPair<uint64, uint64>& R : Ranges)
        for (uint64 F = R.Key; F < R.Value; ++F) Out.Add(float(F));
    return Out;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCAudioStreamPassesTest, "SCBridge.Audio.StreamPasses", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCAudioStreamPassesTest::RunTest(const FString&)
{
    FSCAudioStream S;
    // Three passes: start..LoopEnd, the loop region, then on into the tail.
    TestTrue(TEXT("opens"), S.Open(Ramp(400, 100, 300), 3));
    TestTrue(TEXT("three passes"), Drain(S) == Expected({{0, 300}, {100, 300}, {100, 400}}));
    // One pass plays the whole media; the whole-file mode loops all of it.
    S.Open(Ramp(400, 100, 300), 1);
    TestTrue(TEXT("one pass"), Drain(S) == Expected({{0, 400}}));
    S.Open(Ramp(400, 100, 300), 2, /*bWholeFile*/ true);
    TestTrue(TEXT("whole-file passes"), Drain(S) == Expected({{0, 400}, {0, 400}}));
    // Forever: still looping the region after many passes.
    S.Open(Ramp(400, 100, 300), 0);
    const TArray<float> Forever = Drain(S, 300 + 200 * 10);
    TestEqual(TEXT("forever keeps playing"), Forever.Num(), 300 + 200 * 10);
    TestEqual(TEXT("forever: the last frame of a region pass"), Forever.Last(), 299.f);
    // A media whose decoding ends before LoopEnd loops from where it ended.
    S.Open(Ramp(400, 100, 300, /*DecodedEnd*/ 250), 2);
    TestTrue(TEXT("short media"), Drain(S) == Expected({{0, 250}, {100, 250}}));
    // One that ends before LoopStart never reaches the region: it plays once, even looping forever.
    S.Open(Ramp(400, 100, 300, /*DecodedEnd*/ 80), 0);
    TestTrue(TEXT("media ending before its loop"), Drain(S) == Expected({{0, 80}}));
    // A known decoded end (a prepared wave) also holds for a seek before the first read: the loop region ends there.
    for (const bool bKnown : {true, false})
    {
        const FString K = bKnown ? TEXT("known end") : TEXT("end found while decoding");
        S.Open(Ramp(400, 100, 300, 250, bKnown), 2);
        TestTrue(K + TEXT(": short media"), Drain(S) == Expected({{0, 250}, {100, 250}}));
    }
    S.Open(Ramp(400, 100, 300, 250, /*bKnown*/ true), 0);
    S.SeekPlay(260); // 250 of the first pass, 10 into the region [100, 250)
    TestTrue(TEXT("seek with a known short end"), Drain(S, 150 + 5) == Expected({{110, 250}, {100, 115}}));
    S.Open(Ramp(400, 100, 300, 80, /*bKnown*/ true), 0);
    S.SeekPlay(50);
    TestTrue(TEXT("seek in a media ending before its loop"), Drain(S) == Expected({{50, 80}}));
    S.SeekPlay(90);
    TestTrue(TEXT("seek past its end"), Drain(S).IsEmpty());
    // The frames all passes play, as the stream will play them.
    S.Open(Ramp(400, 100, 300), 3);
    TestEqual(TEXT("play frames: three passes"), S.PlayFrames(), int64(300 + 200 + 300));
    S.Open(Ramp(400, 100, 300), 0);
    TestEqual(TEXT("play frames: forever"), S.PlayFrames(), int64(-1));
    S.Open(Ramp(400, 100, 300, 80, /*bKnown*/ true), 3);
    TestEqual(TEXT("play frames: never reaching its loop, once"), S.PlayFrames(), int64(80));
    S.Open(Ramp(400, 100, 300, 80, /*bKnown*/ true), 0);
    TestEqual(TEXT("play frames: once, even looping forever"), S.PlayFrames(), int64(80));
    S.Open(Ramp(400, 100, 300, 250, /*bKnown*/ true), 2);
    TestEqual(TEXT("play frames: a known short end"), S.PlayFrames(), int64(250 + 150));
    // A known decoded end past the stated length: the last pass plays to it, and a seek into that tail lands.
    S.Open(Ramp(400, 100, 300, 450, /*bKnown*/ true), 2);
    TestEqual(TEXT("play frames: to the decoded end"), S.PlayFrames(), int64(450 + 200));
    TestTrue(TEXT("passes to the decoded end"), Drain(S) == Expected({{0, 300}, {100, 450}}));
    S.SeekPlay(300 + 200 + 120); // into the tail past the stated 400
    TestTrue(TEXT("seek into the tail past the stated length"), Drain(S) == Expected({{420, 450}}));
    // An empty media looping forever ends instead of wrapping in place.
    S.Open(Ramp(0, 0, 0), 0);
    TestTrue(TEXT("an empty media ends"), Drain(S).IsEmpty());
    // The whole-file mode wraps where the decoding ends, also past the media's stated length (an overlong last packet).
    for (const bool bKnown : {false, true})
    {
        const FString K = bKnown ? TEXT("known end") : TEXT("end found while decoding");
        S.Open(Ramp(400, 100, 300, 450, bKnown), 2, /*bWholeFile*/ true);
        TestTrue(K + TEXT(": whole-file passes to the decoded end"), Drain(S) == Expected({{0, 450}, {0, 450}}));
    }
    // Seeks to play time: the passes before it are consumed.
    S.Open(Ramp(400, 100, 300), 3);
    TestTrue(TEXT("seek in the first pass"), S.SeekPlay(50));
    TestTrue(TEXT("seek 50"), Drain(S) == Expected({{50, 300}, {100, 300}, {100, 400}}));
    S.SeekPlay(350); // 300 of the first pass, 50 into the second
    TestTrue(TEXT("seek 350"), Drain(S) == Expected({{150, 300}, {100, 400}}));
    S.SeekPlay(300 + 200 + 250); // into the tail of the last pass
    TestTrue(TEXT("seek into the tail"), Drain(S) == Expected({{350, 400}}));
    S.SeekPlay(300 + 200 + 300);
    TestTrue(TEXT("seek past the end"), Drain(S).IsEmpty());
    S.Open(Ramp(400, 100, 300), 0);
    S.SeekPlay(300 + 200 * 7 + 20);
    const TArray<float> Late = Drain(S, 5);
    TestTrue(TEXT("seek forever"), Late == Expected({{120, 125}}));
    return true;
}

#if WITH_EDITORONLY_DATA
namespace
{
/** Low-passed noise (a fixed seed), interleaved stereo: no two stretches alike, so a match has one lag. */
TArray<float> Noise(int32 Frames)
{
    TArray<float> Pcm;
    Pcm.SetNumUninitialized(Frames * 2);
    FRandomStream Random(1234);
    float L = 0, R = 0;
    for (int32 F = 0; F < Frames; ++F)
    {
        L += 0.2f * (Random.FRandRange(-1.f, 1.f) - L);
        R += 0.1f * (Random.FRandRange(-1.f, 1.f) - R);
        Pcm[F * 2] = 1.2f * L;
        Pcm[F * 2 + 1] = 1.5f * R;
    }
    return Pcm;
}

/** A wave as the prep step makes one (FillSoundWave), its compressed audio cached. */
USoundWave* MakeWave(const TArray<float>& Pcm, int32 Rate, uint64 Frames, uint64 LoopStart, uint64 LoopEnd)
{
    USoundWave* Wave = NewObject<USoundWave>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), USoundWave::StaticClass(), TEXT("SCSourceTest")));
    FillSoundWave(Wave, Pcm.GetData(), Pcm.Num() / 2, 2, Rate, Frames, LoopStart, LoopEnd);
    Wave->CachePlatformData(false);
    return Wave;
}

/** The lag in [-Search, Search] (frames) at which Got best matches Want's media frames from Start, and its relative
 *  RMS error (left channel, Len frames). */
TPair<int32, double> BestLag(const float* Got, const TArray<float>& Want, int64 Start, int32 Len, int32 Search)
{
    int32 Best = 0;
    double BestErr = 1e30, Energy = 0;
    for (int32 Lag = -Search; Lag <= Search; ++Lag)
    {
        double Err = 0, E = 0;
        for (int32 I = 0; I < Len; ++I)
        {
            const int64 F = Start + I + Lag;
            const double W = (F >= 0 && F * 2 < Want.Num()) ? Want[F * 2] : 0.0;
            Err += FMath::Square(Got[I * 2] - W);
            E += W * W;
        }
        if (Err < BestErr) { BestErr = Err; Best = Lag; Energy = E; }
    }
    return {Best, FMath::Sqrt(BestErr / FMath::Max(Energy, 1e-12))};
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCAudioSoundWaveSourceTest, "SCBridge.Audio.SoundWaveSource", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCAudioSoundWaveSourceTest::RunTest(const FString&)
{
    constexpr int32 Rate = 48000, Frames = 120000;
    constexpr int64 LoopStart = 24000, LoopEnd = 96000;
    // Bink Audio is lossy: the decoded media match the PCM within this relative RMS error, at one lag only.
    constexpr double MaxError = 0.35;
    const TArray<float> Pcm = Noise(Frames);
    // The prep step's waves are inline: a streamed wave's first decode can find its chunk not loaded yet.
    {
        const FString Mode = TEXT("inline");
        USoundWave* Wave = MakeWave(Pcm, Rate, Frames, LoopStart, LoopEnd);
        FString Error;
        const FSCAudioSourcePtr Source = MakeSoundWaveSource(Wave, Error);
        if (!TestTrue(Mode + TEXT(": a source: ") + Error, Source.IsValid())) return false;
        TestEqual(Mode + TEXT(": channels"), Source->Channels, 2);
        TestEqual(Mode + TEXT(": rate"), Source->Rate, Rate);
        TestEqual(Mode + TEXT(": frames"), Source->Frames, uint64(Frames));
        TestEqual(Mode + TEXT(": loop start"), Source->LoopStart, uint64(LoopStart));
        TestEqual(Mode + TEXT(": loop end"), Source->LoopEnd, uint64(LoopEnd));
        // Three passes: their lengths and, around each wrap, the media frames they play.
        FSCAudioStream S;
        if (!TestTrue(Mode + TEXT(": opens"), S.Open(Source, 3))) return false;
        TArray<float> Out;
        Out.SetNumZeroed((Frames * 3) * 2);
        int64 Got = 0;
        for (int64 N; (N = S.Read(Out.GetData() + Got * 2, 1000)) > 0;) Got += N;
        const int64 Total = LoopEnd + (LoopEnd - LoopStart) + (Frames - LoopStart);
        TestEqual(Mode + TEXT(": three passes' frames"), Got, Total);
        struct FCheck { int64 Out, Media; const TCHAR* What; };
        for (const FCheck& C : {FCheck{4000, 4000, TEXT("first pass")}, FCheck{LoopEnd + 100, LoopStart + 100, TEXT("after the first wrap")},
                                FCheck{LoopEnd + (LoopEnd - LoopStart) + 100, LoopStart + 100, TEXT("after the second wrap")},
                                FCheck{Total - 3000, Frames - 3000, TEXT("the tail")}})
        {
            const TPair<int32, double> L = BestLag(Out.GetData() + C.Out * 2, Pcm, C.Media, 1024, 500);
            AddInfo(FString::Printf(TEXT("%s %s: lag %d, error %.4f"), *Mode, C.What, L.Key, L.Value));
            TestEqual(FString::Printf(TEXT("%s %s: lag"), *Mode, C.What), L.Key, 0);
            TestTrue(FString::Printf(TEXT("%s %s: error %.4f"), *Mode, C.What, L.Value), L.Value < MaxError);
        }
        // The decoder's seeks land on the frame asked for, inside and outside the loop region.
        for (const int64 Target : {960, 24001, 30000, 58000, 60000, 100000})
        {
            TUniquePtr<ISCAudioDecoder> D = Source->NewDecoder();
            TestTrue(FString::Printf(TEXT("%s: decoder seeks to %lld"), *Mode, Target), D && D->Seek(uint64(Target)));
            TArray<float> Buf;
            Buf.SetNumZeroed(4096 * 2);
            int32 R = 0;
            for (int32 N; D && R < 4096 && (N = D->Read(Buf.GetData() + R * 2, 4096 - R)) > 0;) R += N;
            const TPair<int32, double> L = BestLag(Buf.GetData(), Pcm, Target, 2048, 3000);
            AddInfo(FString::Printf(TEXT("%s decoder seek %lld: lag %d, error %.4f"), *Mode, Target, L.Key, L.Value));
            TestEqual(FString::Printf(TEXT("%s: decoder seek %lld lag"), *Mode, Target), L.Key, 0);
            TestTrue(FString::Printf(TEXT("%s: decoder seek %lld error %.4f"), *Mode, Target, L.Value), L.Value < MaxError);
        }
        // A seek to play time 130000: 96000 of the first pass, then 34000 into the second (media frame 58000).
        TestTrue(Mode + TEXT(": seeks"), S.SeekPlay(130000));
        TArray<float> After;
        After.SetNumZeroed(2048 * 2);
        int64 Read = 0;
        for (int64 N; Read < 2048 && (N = S.Read(After.GetData() + Read * 2, int32(2048 - Read))) > 0;) Read += N;
        const TPair<int32, double> L = BestLag(After.GetData(), Pcm, 58000, 1024, 2000);
        AddInfo(FString::Printf(TEXT("%s seek: lag %d, error %.4f"), *Mode, L.Key, L.Value));
        TestEqual(Mode + TEXT(": seek lag"), L.Key, 0);
        TestTrue(FString::Printf(TEXT("%s: seek error %.4f"), *Mode, L.Value), L.Value < MaxError);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCAudioSoundWaveLengthTest, "SCBridge.Audio.SoundWaveLength", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCAudioSoundWaveLengthTest::RunTest(const FString&)
{
    // 12007 frames at 48 kHz: UE's seconds-times-rate length makes 12006 of it. The wave keeps the exact count.
    constexpr int32 Rate = 48000, Frames = 12007;
    const TArray<float> Pcm = Noise(Frames);
    FString Error;
    const FSCAudioSourcePtr Whole = MakeSoundWaveSource(MakeWave(Pcm, Rate, Frames, 0, Frames), Error);
    if (!TestTrue(TEXT("a source: ") + Error, Whole.IsValid())) return false;
    TestEqual(TEXT("exact frames"), Whole->Frames, uint64(Frames));
    TestEqual(TEXT("the whole media loops"), Whole->LoopEnd, uint64(Frames));
    FSCAudioStream S;
    S.Open(Whole, 3);
    TArray<float> Out;
    Out.SetNumZeroed(Frames * 4 * 2);
    int64 Got = 0;
    for (int64 N; (N = S.Read(Out.GetData() + Got * 2, 1000)) > 0;) Got += N;
    TestEqual(TEXT("three whole passes, no frame lost or padded"), Got, int64(Frames) * 3);
    // A media whose decoding ends before its loop region (the region as the media gave it): it plays once.
    const FSCAudioSourcePtr Early = MakeSoundWaveSource(MakeWave(Pcm, Rate, 24000, 15000, 20000), Error);
    if (!TestTrue(TEXT("a source: ") + Error, Early.IsValid())) return false;
    TestEqual(TEXT("the media's frames"), Early->Frames, uint64(24000));
    TestEqual(TEXT("its loop start"), Early->LoopStart, uint64(15000));
    S.Open(Early, 0);
    Got = 0;
    for (int64 N; Got < Frames * 4 && (N = S.Read(Out.GetData() + Got * 2, 1000)) > 0;) Got += N;
    TestEqual(TEXT("ending before its loop: once"), Got, int64(Frames));
    return true;
}
#endif // WITH_EDITORONLY_DATA
#endif // WITH_DEV_AUTOMATION_TESTS

