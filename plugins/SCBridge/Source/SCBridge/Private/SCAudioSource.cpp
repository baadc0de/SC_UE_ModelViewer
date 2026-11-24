// WP-O1-4c-1: the audio sources of SCAudioSource.h. FSCAudioStream's passes follow the Wwise looping the Ogg stream of
// scbridge (crates/scbridge-ffi/src/audio.rs) implemented first; both sources decode one pass sequentially and seek.
#include "SCAudioSource.h"
#include "Audio.h"
#include "Sound/SoundWave.h"
PRAGMA_DISABLE_INTERNAL_WARNINGS
#include "Sound/SoundWaveProxyPlayer.h"
PRAGMA_ENABLE_INTERNAL_WARNINGS

// ------------------------------------------------------------------------------------------------ the passes
bool FSCAudioStream::Open(const FSCAudioSourcePtr& InSource, uint32 Loops, bool bWholeFile)
{
    Source = InSource;
    Decoder = Source ? Source->NewDecoder() : nullptr;
    if (!Decoder) return false;
    Channels = Source->Channels;
    Rate = Source->Rate;
    Frames = Source->Frames;
    const uint64 End = Source->DecodedEnd;
    Passes = Loops == 0 ? INDEX_NONE : int64(Loops);
    if (bWholeFile)
    {
        // Every pass plays the media to the end of its decoding (an end found while decoding: Read learns it).
        LoopStart = 0;
        LoopEnd = End ? End : MAX_uint64;
        if (End) Frames = End;
        TotalFrames = Passes == INDEX_NONE ? -1 : int64(Frames) * Passes;
    }
    else
    {
        LoopStart = Source->LoopStart;
        LoopEnd = Source->LoopEnd;
        if (End)
        {
            // A known decoded end: what Read would find, settled before any seek. The last pass plays to it, also when
            // the decoding runs past the length the media states (a long last packet).
            Frames = End;
            if (End <= LoopStart) Passes = 1; // never reaches its loop region: plays once
            else LoopEnd = FMath::Min(LoopEnd, End);
        }
        TotalFrames = Passes == INDEX_NONE ? -1 : int64(Frames + uint64(Passes - 1) * (LoopEnd > LoopStart ? LoopEnd - LoopStart : 0));
    }
    PassesLeft = Passes == INDEX_NONE ? INDEX_NONE : Passes - 1;
    Pos = 0;
    bEnded = false;
    return true;
}

bool FSCAudioStream::Wrap()
{
    if (PassesLeft > 0) --PassesLeft;
    Pos = LoopStart;
    return Decoder->Seek(LoopStart);
}

int64 FSCAudioStream::Read(float* Out, int32 Want)
{
    if (!Decoder || !Out) return -1;
    const int32 Ch = FMath::Max(1, Channels);
    int32 Written = 0;
    while (Written < Want && !bEnded)
    {
        // A pass that loops again stops at LoopEnd; the last one plays the media out.
        const uint64 Limit = MorePasses() ? LoopEnd : MAX_uint64;
        if (Pos >= Limit)
        {
            // An empty loop region has nothing to loop over (a wrap would land where it started, for ever).
            if (LoopEnd <= LoopStart) bEnded = true;
            else if (!Wrap()) return -1;
            continue;
        }
        const int32 N = int32(FMath::Min<uint64>(uint64(Want - Written), Limit - Pos));
        const int32 Got = Decoder->Read(Out + int64(Written) * Ch, N);
        if (Got < 0) return -1;
        if (Got > 0)
        {
            Pos += uint64(Got);
            Written += Got;
            continue;
        }
        if (MorePasses() && Pos > LoopStart)
        {
            // The media ended before LoopEnd: loop from here.
            LoopEnd = Pos;
            if (!Wrap()) return -1;
        }
        else
            bEnded = true;
    }
    return Written;
}

bool FSCAudioStream::SeekPlay(uint64 T)
{
    if (!Decoder) return false;
    PassesLeft = Passes == INDEX_NONE ? INDEX_NONE : Passes - 1;
    bEnded = false;
    const uint64 Region = LoopEnd - LoopStart;
    uint64 Media = T;
    if (MorePasses() && Media >= LoopEnd && Region > 0)
    {
        // Whole passes over the loop region after the first.
        const uint64 Over = Media - LoopEnd;
        const uint64 Whole = Over / Region;
        const uint64 Skipped = PassesLeft == INDEX_NONE ? Whole : FMath::Min<uint64>(Whole, uint64(PassesLeft) - 1);
        if (PassesLeft != INDEX_NONE) PassesLeft -= int64(Skipped) + 1;
        Media = LoopStart + (Over - Skipped * Region);
    }
    if (!MorePasses() && Media >= Frames)
    {
        bEnded = true;
        Pos = Frames;
        return true;
    }
    Pos = Media;
    return Decoder->Seek(Media);
}

// ------------------------------------------------------------------------------------------------ SoundWave assets
namespace
{
/** The cue points of a prepared media (FillSoundWave): its loop region and its exact frame counts. */
constexpr int32 CueLoop = 0, CueDecodedEnd = 1, CueFrames = 2;
/** Silence after the decoded frames: more than UE's seconds-times-rate length can lose. */
constexpr int32 PadFrames = 64;
} // namespace

// FSoundWaveProxyPlayer is UE_INTERNAL: the engine decodes a SoundWave with it (the MetaSound wave player's path), and
// it is the one decoder over a SoundWave that seeks to a frame.
PRAGMA_DISABLE_INTERNAL_WARNINGS
namespace
{
class FWaveDecoder final : public ISCAudioDecoder
{
public:
    FWaveDecoder(const TSharedRef<const FSoundWaveData>& InData, int32 InChannels, int32 InRate, uint64 InEnd)
        : Data(InData), Channels(InChannels), Rate(InRate), End(InEnd) {}
    bool Start()
    {
        Pos = 0;
        Player = FSoundWaveProxyPlayer::Create(FSoundWaveProxyPlayer::FSettings(float(Rate), Channels));
        return Player && Player->SetSoundWave(Data);
    }
    virtual bool Seek(uint64 Frame) override
    {
        // A fresh decoder is at the start exactly; any other frame is the codec's seek (Bink: to the frame).
        if (Frame == 0) return Start();
        if (!Player || Frame > uint64(MAX_int32)) return false;
        Pos = Frame;
        return Frame >= End || Player->SeekToFrame(int32(Frame));
    }
    virtual int32 Read(float* Out, int32 Frames) override
    {
        if (!Player || !Player->IsPlayerValid()) return -1;
        // The media ends at its last decoded frame, not in the padding after it.
        const int32 N = int32(FMath::Min<uint64>(uint64(FMath::Max(Frames, 0)), End > Pos ? End - Pos : 0));
        if (N == 0) return 0;
        const int32 Got = Player->GenerateSourceAudio(TArrayView<float>(Out, N * Channels));
        Pos += uint64(FMath::Max(Got, 0));
        return Got;
    }

private:
    TSharedRef<const FSoundWaveData> Data;
    TUniquePtr<FSoundWaveProxyPlayer> Player;
    int32 Channels, Rate;
    uint64 End, Pos = 0;
};

class FWaveSource final : public FSCAudioSource
{
public:
    explicit FWaveSource(const TSharedRef<const FSoundWaveData>& InData) : Data(InData) {}
    virtual TUniquePtr<ISCAudioDecoder> NewDecoder() const override
    {
        TUniquePtr<FWaveDecoder> D = MakeUnique<FWaveDecoder>(Data, Channels, Rate, DecodedEnd);
        return D->Start() ? MoveTemp(D) : nullptr;
    }
    virtual const TCHAR* Kind() const override { return TEXT("soundwave"); }

private:
    TSharedRef<const FSoundWaveData> Data;
};
} // namespace

FSCAudioSourcePtr MakeSoundWaveSource(USoundWave* Wave, FString& Error)
{
    check(IsInGameThread());
    if (!Wave)
    {
        Error = TEXT("no wave");
        return nullptr;
    }
    // A wave that is not streamed decodes from its resource, which UE loads when a voice first plays it (FMixerBuffer):
    // load it here, as the mixer would.
    if (!Wave->IsStreaming() && !Wave->InitAudioResource(Wave->GetRuntimeFormat()))
    {
        Error = Wave->GetName() + TEXT(": no compressed audio");
        return nullptr;
    }
    const FSoundWaveProxyPtr Proxy = Wave->CreateSoundWaveProxy();
    const TSharedPtr<const FSoundWaveData> Data = Proxy ? Proxy->GetSoundWaveDataRef().ToSharedPtr() : nullptr;
    const int32 Channels = Data ? int32(Data->GetNumChannels()) : 0;
    const int32 Rate = Data ? FMath::RoundToInt32(Data->GetSampleRate()) : 0;
    const int64 Frames = Data ? int64(Data->GetNumFrames()) : 0;
    if (!Data || Channels <= 0 || Channels > 8 || Rate <= 0 || Frames <= 0 || (!Data->IsStreaming() && Data->GetResourceSize() <= 0))
    {
        Error = FString::Printf(TEXT("%s: no audio (channels %d, rate %d, frames %lld)"), *Wave->GetName(), Channels, Rate, Frames);
        return nullptr;
    }
    TSharedRef<FWaveSource> S = MakeShared<FWaveSource>(Data.ToSharedRef());
    S->Channels = Channels;
    S->Rate = Rate;
    // UE's length (seconds times rate) unless the wave names its exact counts.
    S->Frames = uint64(Frames);
    S->DecodedEnd = uint64(Frames);
    for (const FSoundWaveCuePoint& C : Wave->GetCuePoints())
    {
        if (C.FramePosition < 0) continue;
        if (C.CuePointID == CueFrames) S->Frames = uint64(C.FramePosition);
        else if (C.CuePointID == CueDecodedEnd) S->DecodedEnd = uint64(FMath::Min(C.FramePosition, Frames));
    }
    S->LoopStart = 0;
    S->LoopEnd = S->Frames;
    // The loop region as the media gave it, as the Ogg stream keeps it (FSCAudioStream handles a media that ends early).
    for (const FSoundWaveCuePoint& L : Wave->GetLoopRegions())
        if (L.CuePointID == CueLoop && L.FramePosition >= 0 && L.FrameLength > 0)
        {
            S->LoopStart = uint64(L.FramePosition);
            S->LoopEnd = uint64(L.FramePosition + L.FrameLength);
            break;
        }
    return S;
}

PRAGMA_ENABLE_INTERNAL_WARNINGS

#if WITH_EDITORONLY_DATA
namespace
{
/** A private SoundWave property the importer sets (USoundFactory is the wave's friend; this is not). */
template <typename T>
T* WaveProperty(USoundWave* Wave, const TCHAR* Name)
{
    const FProperty* P = USoundWave::StaticClass()->FindPropertyByName(Name);
    return P ? P->ContainerPtrToValuePtr<T>(Wave) : nullptr;
}

/** Bink Audio quality of the prepared media: they are already lossy (Vorbis), so a high one. */
constexpr int32 MediaQuality = 80;
} // namespace

void FillSoundWave(USoundWave* Wave, const float* Decoded, int64 DecodedFrames, int32 Channels, int32 Rate, uint64 Frames, uint64 LoopStart,
                   uint64 LoopEnd)
{
    const int64 Padded = DecodedFrames + PadFrames;
    TArray<int16> Pcm;
    Pcm.SetNumZeroed(Padded * Channels);
    for (int64 I = 0; I < DecodedFrames * Channels; ++I) Pcm[I] = int16(FMath::Clamp(FMath::RoundToInt32(Decoded[I] * 32767.f), -32768, 32767));
    TArray<uint8> Wav;
    SerializeWaveFile(Wav, reinterpret_cast<const uint8*>(Pcm.GetData()), int32(Pcm.Num() * sizeof(int16)), Channels, Rate);
    Wave->RawData.UpdatePayload(FSharedBuffer::Clone(Wav.GetData(), Wav.Num()));
    Wave->SetImportedSampleRate(Rate);
    Wave->SetSampleRate(Rate);
    Wave->NumChannels = Channels;
    Wave->Duration = float(double(Padded) / Rate);
    Wave->TotalSamples = float(Padded);
    Wave->SetSoundAssetCompressionType(ESoundAssetCompressionType::BinkAudio, false);
    if (int32* Quality = WaveProperty<int32>(Wave, TEXT("CompressionQuality"))) *Quality = MediaQuality;
    Wave->LoadingBehavior = ESoundWaveLoadingBehavior::ForceInline;
    auto Cue = [](int32 Id, const TCHAR* Label, uint64 Position, uint64 Length) {
        FSoundWaveCuePoint C;
        C.CuePointID = Id;
        C.Label = Label;
        C.FramePosition = int64(Position);
        C.FrameLength = int64(Length);
        C.SetLoopRegion(Length > 0);
        return C;
    };
    TArray<FSoundWaveCuePoint> Cues = {Cue(CueDecodedEnd, TEXT("decoded_end"), uint64(DecodedFrames), 0), Cue(CueFrames, TEXT("frames"), Frames, 0)};
    if (LoopEnd > LoopStart) Cues.Add(Cue(CueLoop, TEXT("loop"), LoopStart, LoopEnd - LoopStart));
    // UE reads the loop regions from the cue points when they come from the wave file, as an import's do.
    Wave->SetCuePointOrigin(ESoundWaveCuePointOrigin::WaveFile);
    Wave->SetSoundWaveCuePoints(Cues);
    Wave->InvalidateCompressedData(true);
    Wave->PostImport();
}
#endif // WITH_EDITORONLY_DATA

// ------------------------------------------------------------------------------------------------ Ogg media
namespace
{
TFunction<FSCAudioSourcePtr(const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>&, FString&)> GOggFactory;
} // namespace
void SetOggSourceFactory(TFunction<FSCAudioSourcePtr(const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>&, FString&)> Factory)
{
    check(IsInGameThread());
    GOggFactory = MoveTemp(Factory);
}
FSCAudioSourcePtr MakeOggSource(const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>& Ogg, FString& Error)
{
    if (!GOggFactory)
    {
        Error = TEXT("no Ogg decoder (the importer is not loaded)");
        return nullptr;
    }
    return GOggFactory(Ogg, Error);
}
