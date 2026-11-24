// WP-O1-4c-1: what a voice (USCAudioWave) plays. A source is one media: its format, its loop region and new decoders
// over it. FSCAudioStream plays passes over the loop region as Wwise does, whatever decodes the media:
// - a prepared level's media are SoundWave assets (the prep step writes them, the media's loop region a loop cue point);
//   UE decodes them (MakeSoundWaveSource), so a packaged game needs no decoder of its own;
// - a package's media are Ogg Vorbis (the run-time import, the viewer); scbridge decodes them (MakeOggSource).
#pragma once
#include "CoreMinimal.h"

class USoundWave;

/** Sequential decoding of one media from a media frame. One thread at a time. */
class ISCAudioDecoder
{
public:
    virtual ~ISCAudioDecoder() = default;
    /** Place the decoder at media frame Frame, sample-exact; false when it cannot. */
    virtual bool Seek(uint64 Frame) = 0;
    /** Decode up to Frames interleaved frames into Out: the frames written, 0 at the media's end, < 0 on an error. */
    virtual int32 Read(float* Out, int32 Frames) = 0;
};

/** One media and how to decode it. Its voices share it (any thread). Its format and loop region are known once a
 *  decoder has opened (FSCAudioStream::Open reads them after NewDecoder). */
class SCBRIDGE_API FSCAudioSource
{
public:
    virtual ~FSCAudioSource() = default;
    int32 Channels = 0, Rate = 0;
    /** The media's frames and its loop region [LoopStart, LoopEnd) (0..Frames for a media without loop points). */
    uint64 Frames = 0, LoopStart = 0, LoopEnd = 0;
    /** Where its decoding ends, when known before decoding (a prepared wave's); 0: found while decoding (the Ogg's). */
    uint64 DecodedEnd = 0;
    virtual TUniquePtr<ISCAudioDecoder> NewDecoder() const = 0;
    /** For logs: what decodes it. */
    virtual const TCHAR* Kind() const = 0;
};
using FSCAudioSourcePtr = TSharedPtr<const FSCAudioSource, ESPMode::ThreadSafe>;

/** Passes over a source's loop region, as Wwise plays a looping media: the first pass from the start to LoopEnd, every
 *  further pass LoopStart..LoopEnd, and the last pass of a finite count on into the tail after LoopEnd. A media whose
 *  decoding ends before LoopEnd loops from where it ended; one that ends before LoopStart plays once. A source that knows
 *  its decoded end applies this from the start (seeks included); otherwise the stream finds it while decoding. One thread
 *  at a time. */
class SCBRIDGE_API FSCAudioStream
{
public:
    /** Play Loops passes (0 = forever). bWholeFile loops the whole media, to the end of its decoding, instead of its loop
     *  region (the behaviour before the WP-A1 playback fix, kept for A/B measurement: sc.Audio.Legacy 1). False when no
     *  decoder opens. */
    bool Open(const FSCAudioSourcePtr& Source, uint32 Loops, bool bWholeFile = false);
    /** Fill Out with up to Frames interleaved frames: the frames written, 0 once every pass has played, < 0 on an error. */
    int64 Read(float* Out, int32 Frames);
    /** Move to play time Frame (frames since the first pass began, counting the passes over the loop region), as a Wwise
     *  voice resumed "from elapsed time" does; past the end of a finite play nothing is left. False on a decoder error. */
    bool SeekPlay(uint64 Frame);
    bool IsOpen() const { return Decoder.IsValid(); }
    /** The frames all passes play (< 0: forever), from the passes the stream will really play (a media that never
     *  reaches its loop region plays once). With an end found only while decoding, the media's own length stands in. */
    int64 PlayFrames() const { return TotalFrames; }
    int32 Channels = 0, Rate = 0;
    uint64 Frames = 0, LoopStart = 0, LoopEnd = 0;

private:
    bool MorePasses() const { return PassesLeft != 0; }
    bool Wrap();
    FSCAudioSourcePtr Source;
    TUniquePtr<ISCAudioDecoder> Decoder;
    uint64 Pos = 0;              // the media frame of the next frame read
    int64 PassesLeft = INDEX_NONE; // passes after the current one; INDEX_NONE = forever
    int64 Passes = INDEX_NONE;     // the passes it was opened for
    int64 TotalFrames = -1;
    bool bEnded = false;
};

/** A prepared level's media: a SoundWave asset FillSoundWave made (a wave without its cue points plays whole: UE's own
 *  length, no loop region). Call on the game thread; the wave must stay referenced while the source is created. Null
 *  with Error when it has no audio. */
SCBRIDGE_API FSCAudioSourcePtr MakeSoundWaveSource(USoundWave* Wave, FString& Error);

#if WITH_EDITORONLY_DATA
/** Make Wave (new) a prepared media: Decoded (DecodedFrames interleaved frames, as the media decoded) as its PCM raw data,
 *  Bink Audio, inline (loaded with its level, so a voice's first decode never waits on a streamed chunk), and its cue
 *  points: the media's loop region [LoopStart, LoopEnd) as given (a media whose decoding ends before LoopEnd loops from
 *  where it ended, one that ends before LoopStart plays once, as FSCAudioStream does with the Ogg) and the exact frame
 *  counts, the media's Frames and DecodedFrames (UE keeps a length only as seconds times rate, which can lose a frame).
 *  Silence pads the raw data so UE's length never cuts a decoded frame. The caller caches its compressed data and saves. */
SCBRIDGE_API void FillSoundWave(USoundWave* Wave, const float* Decoded, int64 DecodedFrames, int32 Channels, int32 Rate, uint64 Frames,
                                uint64 LoopStart, uint64 LoopEnd);
#endif

/** A package's Ogg Vorbis media (loop points from its LoopStart / LoopEnd comments), decoded by the importer's decoder
 *  (scbridge). Nothing is decoded until a voice opens it (a scene registers its media without reading them). Null with
 *  Error in a build without the importer. */
SCBRIDGE_API FSCAudioSourcePtr MakeOggSource(const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>& Ogg, FString& Error);
/** The importer registers its Ogg decoder (game thread; an empty function unregisters). */
SCBRIDGE_API void SetOggSourceFactory(TFunction<FSCAudioSourcePtr(const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>&, FString&)> Factory);
