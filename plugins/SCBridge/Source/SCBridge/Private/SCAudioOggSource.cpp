// WP-O1-4c-3b: the importer's Ogg Vorbis source (scbridge's decoder) behind MakeOggSource (SCAudioSource.h), split from
// SCAudioSource.cpp: the runtime's sources need no archive library.
#include "SCAudioSource.h"
#include "scbridge.h"

// ------------------------------------------------------------------------------------------------ Ogg media
namespace
{
class FOggDecoder final : public ISCAudioDecoder
{
public:
    explicit FOggDecoder(sc_audio_stream* InStream) : Stream(InStream) {}
    virtual ~FOggDecoder() override { sc_audio_close(Stream); }
    // A one-pass stream: its play time is the media frame.
    virtual bool Seek(uint64 Frame) override { return sc_audio_seek(Stream, Frame) == SC_STATUS_OK; }
    virtual int32 Read(float* Out, int32 Frames) override { return int32(sc_audio_read(Stream, Out, size_t(Frames))); }

private:
    sc_audio_stream* Stream;
};

class FOggSource final : public FSCAudioSource
{
public:
    explicit FOggSource(const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>& InBytes) : Bytes(InBytes) {}
    virtual TUniquePtr<ISCAudioDecoder> NewDecoder() const override
    {
        sc_audio_stream* Stream = nullptr;
        uint32 Ch = 0, R = 0;
        if (sc_audio_open(Bytes->GetData(), Bytes->Num(), 1, &Stream, &Ch, &R) != SC_STATUS_OK || !Stream) return nullptr;
        // The format and loop region, read once from the first decoder (a scene's media are not opened until played).
        FScopeLock Guard(&FormatLock);
        if (!bFormat)
        {
            FOggSource& Self = const_cast<FOggSource&>(*this);
            Self.Channels = int32(Ch);
            Self.Rate = int32(R);
            sc_audio_loop_region(Stream, &Self.LoopStart, &Self.LoopEnd, &Self.Frames);
            bFormat = true;
        }
        return MakeUnique<FOggDecoder>(Stream);
    }
    virtual const TCHAR* Kind() const override { return TEXT("ogg"); }

private:
    TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Bytes;
    mutable FCriticalSection FormatLock;
    mutable bool bFormat = false;
};
} // namespace

FSCAudioSourcePtr MakeImporterOggSource(const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>& Ogg, FString& Error)
{
    if (!Ogg || Ogg->IsEmpty())
    {
        Error = TEXT("no Ogg bytes");
        return nullptr;
    }
    // Nothing is decoded here: the first decoder (a voice's Open) reads the format, and a media that does not decode
    // fails there, as it did before sources.
    return MakeShared<FOggSource>(Ogg);
}
