// FSCVideoRecorder (WP-H1): game view -> GPU readback (render thread) -> JPEGs (worker tasks) -> ffmpeg (hidden) -> MP4.
// See SCRpcVideo.h.
#include "SCRpcVideo.h"
#include "SCRpcJson.h"

#if SC_WITH_RPC
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "Tasks/Task.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCRpcVideo, Log, All);

namespace
{
constexpr int32 MaxInFlight = 12;  // JPEG writes queued at once; beyond, frames are dropped (counted)
constexpr int32 MaxReadbacks = 4;  // GPU copies not yet read back

FString FfmpegExe()
{
    FString Exe;
    if (FParse::Value(FCommandLine::Get(), TEXT("SCFfmpeg="), Exe) && !Exe.IsEmpty()) return Exe;
    return TEXT("ffmpeg"); // on PATH
}

bool IsHDRPreviewSource(EPixelFormat Format, bool bHDRDevice)
{
    return Format == PF_FloatRGBA || (Format == PF_A2B10G10R10 && bHDRDevice);
}

/** Rows of a readback in Format into 8-bit BGRA (gamma as displayed). False for a format it does not know. */
bool ToColors(const uint8* Data, int32 PitchPixels, FIntPoint Size, EPixelFormat Format, TArray<FColor>& Out)
{
    Out.SetNumUninitialized(Size.X * Size.Y);
    for (int32 Y = 0; Y < Size.Y; ++Y)
    {
        FColor* Dst = Out.GetData() + int64(Y) * Size.X;
        switch (Format)
        {
        case PF_B8G8R8A8:
            FMemory::Memcpy(Dst, Data + int64(Y) * PitchPixels * 4, Size.X * 4);
            break;
        case PF_R8G8B8A8:
        {
            const uint8* Src = Data + int64(Y) * PitchPixels * 4;
            for (int32 X = 0; X < Size.X; ++X) Dst[X] = FColor(Src[X * 4], Src[X * 4 + 1], Src[X * 4 + 2], 255);
            break;
        }
        case PF_A2B10G10R10:
        {
            const uint32* Src = reinterpret_cast<const uint32*>(Data) + int64(Y) * PitchPixels;
            for (int32 X = 0; X < Size.X; ++X)
                Dst[X] = FColor(uint8((Src[X] & 0x3FF) >> 2), uint8(((Src[X] >> 10) & 0x3FF) >> 2), uint8(((Src[X] >> 20) & 0x3FF) >> 2), 255);
            break;
        }
        case PF_FloatRGBA:
        {
            const FFloat16Color* Src = reinterpret_cast<const FFloat16Color*>(Data) + int64(Y) * PitchPixels;
            for (int32 X = 0; X < Size.X; ++X) Dst[X] = FLinearColor(Src[X]).ToFColor(true);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}
} // namespace

/** Copies the game viewport's view family target after it renders, when asked, and reads it back a few frames later
 *  (all on the render thread); finished frames wait in Ready for the game thread. */
class FSCRecordExtension final : public FSceneViewExtensionBase
{
public:
    FSCRecordExtension(const FAutoRegister& AutoRegister, const FRenderTarget* InTarget) : FSceneViewExtensionBase(AutoRegister), Target(InTarget) {}
    virtual void SetupViewFamily(FSceneViewFamily&) override {}
    virtual void SetupView(FSceneViewFamily&, FSceneView&) override {}
    virtual void BeginRenderViewFamily(FSceneViewFamily&) override {}

    virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& Family) override
    {
        Poll();
        if (Family.RenderTarget != Target || Family.Views.IsEmpty() || !Family.Views[0]) return;
        if (Requests.load() <= 0) return;
        --Requests;
        if (Pending.Num() >= MaxReadbacks) { ++Dropped; return; }
        FRDGTextureRef Texture = Family.RenderTarget->GetRenderTargetTexture(GraphBuilder);
        if (!Texture) { ++Dropped; return; }
        const FIntRect Rect = Family.Views[0]->UnscaledViewRect;
        if (!bLoggedSource)
        {
            bLoggedSource = true;
            UE_LOG(LogSCRpcVideo, Display, TEXT("RPC_RECORD source texture=%s extent=%dx%d format=%s rect=%s views=%d"), Texture->Name, Texture->Desc.Extent.X, Texture->Desc.Extent.Y,
                   GetPixelFormatString(Texture->Desc.Format), *Rect.ToString(), Family.Views.Num());
        }
        if (IsHDRPreviewSource(Texture->Desc.Format, Family.RenderTarget->GetSceneHDREnabled()) && !SawHDR.exchange(true))
        {
            UE_LOG(LogSCRpcVideo, Warning, TEXT("RPC_RECORD HDR source %s converted to 8-bit preview. Highlights may clip."),
                   GetPixelFormatString(Texture->Desc.Format));
        }
        FPendingFrame& P = Pending.AddDefaulted_GetRef();
        P.Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("SCRecordFrame"));
        P.Size = Rect.Size();
        P.Format = Texture->Desc.Format;
        P.Time = FPlatformTime::Seconds();
        AddEnqueueCopyPass(GraphBuilder, P.Readback.Get(), Texture, FResolveRect(Rect.Min.X, Rect.Min.Y, Rect.Max.X, Rect.Max.Y));
        InGpu = Pending.Num();
    }

    struct FFrame
    {
        TArray<FColor> Pixels;
        FIntPoint Size;
        double Time = 0;
    };
    TArray<FFrame> Take()
    {
        FScopeLock Lock(&Mutex);
        return MoveTemp(Ready);
    }
    FString GetFormatName()
    {
        FScopeLock Lock(&Mutex);
        return FormatName;
    }

    const FRenderTarget* Target;
    std::atomic<bool> SawHDR{false};
    std::atomic<int32> Requests{0}, Dropped{0}, InGpu{0};

private:
    struct FPendingFrame
    {
        TUniquePtr<FRHIGPUTextureReadback> Readback;
        FIntPoint Size;
        EPixelFormat Format = PF_Unknown;
        double Time = 0;
    };
    void Poll()
    {
        while (!Pending.IsEmpty() && Pending[0].Readback->IsReady())
        {
            FPendingFrame& P = Pending[0];
            FFrame F;
            F.Size = P.Size;
            F.Time = P.Time;
            int32 Pitch = 0, Height = 0;
            const uint8* Data = static_cast<const uint8*>(P.Readback->Lock(Pitch, &Height));
            const bool bOK = Data && ToColors(Data, Pitch, P.Size, P.Format, F.Pixels);
            P.Readback->Unlock();
            {
                FScopeLock Lock(&Mutex);
                FormatName = GetPixelFormatString(P.Format);
                if (bOK) Ready.Add(MoveTemp(F));
            }
            if (!bOK) ++Dropped;
            Pending.RemoveAt(0);
        }
        InGpu = Pending.Num();
    }
    TArray<FPendingFrame> Pending; // render thread only
    bool bLoggedSource = false;
    FCriticalSection Mutex;
    TArray<FFrame> Ready;
    FString FormatName;
};

FSCVideoRecorder::FSCVideoRecorder() = default;

FSCVideoRecorder::~FSCVideoRecorder()
{
    if (Extension)
    {
        Extension->Requests = 0;
        FlushRenderingCommands();
    }
    Extension.Reset();
    if (Encoder.IsValid())
    {
        if (FPlatformProcess::IsProcRunning(Encoder)) FPlatformProcess::TerminateProc(Encoder);
        FPlatformProcess::CloseProc(Encoder);
    }
}

bool FSCVideoRecorder::Start(const FString& InPath, double InFps, double InMaxSeconds, int32 InWidth, bool bInKeepFrames, FString& OutError)
{
    if (IsBusy()) { OutError = TEXT("a recording is running"); return false; }
    UGameViewportClient* Client = GEngine ? GEngine->GameViewport.Get() : nullptr;
    const FViewport* Viewport = Client ? Client->Viewport : nullptr;
    if (!Viewport) { OutError = TEXT("no game viewport"); return false; }
    FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper")); // loaded here, used by the workers
    Path = FPaths::ConvertRelativePathToFull(InPath);
    FrameDir = FPaths::Combine(FPaths::GetPath(Path), FPaths::GetBaseFilename(Path) + TEXT("_frames"));
    IFileManager::Get().DeleteDirectory(*FrameDir, false, true);
    if (!IFileManager::Get().MakeDirectory(*FrameDir, true)) { OutError = TEXT("cannot create ") + FrameDir; return false; }
    IFileManager::Get().Delete(*Path, false, true, true);
    Fps = FMath::Clamp(InFps, 1.0, 60.0);
    MaxSeconds = FMath::Clamp(InMaxSeconds, 1.0, 4 * 3600.0); // an interactive session may run for hours
    Width = FMath::Max(0, InWidth);
    bKeepFrames = bInKeepFrames;
    Times.Reset();
    Dropped = 0;
    *InFlight = 0;
    *WriteFailures = 0;
    Error.Reset();
    Format.Reset();
    bSawHDR = false;
    Extension = FSceneViewExtensions::NewExtension<FSCRecordExtension>(Viewport);
    Started = NextAt = FPlatformTime::Seconds();
    Phase = EPhase::Recording;
    UE_LOG(LogSCRpcVideo, Display, TEXT("RPC_RECORD start path=%s fps=%.0f max_s=%.0f width=%d"), *Path, Fps, MaxSeconds, Width);
    return true;
}

void FSCVideoRecorder::Stop()
{
    if (Phase != EPhase::Recording) return;
    Phase = EPhase::Draining;
    StoppedAt = FPlatformTime::Seconds();
    if (Extension) Extension->Requests = 0;
}

void FSCVideoRecorder::Finish(double TimeoutSeconds)
{
    if (!IsBusy()) return;
    Stop();
    UE_LOG(LogSCRpcVideo, Display, TEXT("RPC_RECORD finishing at shutdown path=%s frames=%d"), *Path, Times.Num());
    const double Until = FPlatformTime::Seconds() + TimeoutSeconds;
    while (IsBusy() && FPlatformTime::Seconds() < Until)
    {
        Tick();
        FPlatformProcess::Sleep(0.05f);
    }
    if (IsBusy())
    {
        // The destructor ends ffmpeg; the frames and their timing list stay for a later encode.
        bKeepFrames = true;
        UE_LOG(LogSCRpcVideo, Warning, TEXT("RPC_RECORD not finished within %.0f s at shutdown; frames kept in %s"), TimeoutSeconds, *FrameDir);
    }
}

void FSCVideoRecorder::Collect()
{
    if (!Extension) return;
    for (FSCRecordExtension::FFrame& F : Extension->Take())
    {
        if (*InFlight >= MaxInFlight) { ++Dropped; continue; }
        const FString File = FPaths::Combine(FrameDir, FString::Printf(TEXT("%06d.jpg"), Times.Num()));
        Times.Add(F.Time);
        ++*InFlight;
        UE::Tasks::Launch(TEXT("SCRecordFrame"), [Pixels = MoveTemp(F.Pixels), Size = F.Size, File, Pending = InFlight, Failures = WriteFailures]() {
            IImageWrapperModule& Module = FModuleManager::GetModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
            TSharedPtr<IImageWrapper> Jpeg = Module.CreateImageWrapper(EImageFormat::JPEG);
            bool bOK = Jpeg && Jpeg->SetRaw(Pixels.GetData(), int64(Pixels.Num()) * sizeof(FColor), Size.X, Size.Y, ERGBFormat::BGRA, 8);
            if (bOK) bOK = FFileHelper::SaveArrayToFile(Jpeg->GetCompressed(90), *File);
            if (!bOK) ++*Failures;
            --*Pending;
        });
    }
}

void FSCVideoRecorder::Tick()
{
    if (Phase == EPhase::Idle || IsFinished()) return;
    const double Now = FPlatformTime::Seconds();
    if (Phase == EPhase::Recording)
    {
        if (Now - Started >= MaxSeconds) Stop();
        else if (Now >= NextAt)
        {
            if (Extension->Requests.load() < 2) ++Extension->Requests; // one frame per due time (no backlog)
            NextAt = FMath::Max(NextAt + 1.0 / Fps, Now);
        }
    }
    Collect();
    if (Phase == EPhase::Draining)
    {
        // Copies made before the stop still come back from the GPU (a second at most); then the writes finish.
        if (Extension && (Extension->InGpu.load() == 0 || Now - StoppedAt > 1.0))
        {
            Collect();
            Dropped += Extension->Dropped.load();
            Format = Extension->GetFormatName();
            bSawHDR = Extension->SawHDR.load();
            Extension.Reset(); // unregisters (the engine holds extensions weakly)
        }
        if (!Extension && *InFlight == 0) Encode();
    }
    if (Phase == EPhase::Encoding && !FPlatformProcess::IsProcRunning(Encoder))
    {
        int32 Code = -1;
        FPlatformProcess::GetProcReturnCode(Encoder, &Code);
        FPlatformProcess::CloseProc(Encoder);
        Encoder.Reset();
        if (Code != 0 || IFileManager::Get().FileSize(*Path) <= 0) { Fail(FString::Printf(TEXT("ffmpeg exited with %d (frames kept in %s)"), Code, *FrameDir)); return; }
        if (!bKeepFrames) IFileManager::Get().DeleteDirectory(*FrameDir, false, true);
        Phase = EPhase::Done;
        UE_LOG(LogSCRpcVideo, Display, TEXT("RPC_RECORD done path=%s frames=%d dropped=%d format=%s encode_s=%.1f"), *Path, Times.Num(), Dropped, *Format, Now - EncodeStarted);
    }
}

void FSCVideoRecorder::Encode()
{
    if (Times.IsEmpty()) { Fail(TEXT("no frame was captured")); return; }
    if (*WriteFailures > 0) UE_LOG(LogSCRpcVideo, Warning, TEXT("RPC_RECORD %d frame writes failed"), WriteFailures->load());
    // Each frame shown for the time until the next one was captured: the video plays in real time even when the game
    // ran below the target rate. (The concat demuxer wants the last file listed twice.)
    FString List;
    for (int32 I = 0; I < Times.Num(); ++I)
    {
        const double Duration = I + 1 < Times.Num() ? Times[I + 1] - Times[I] : 1.0 / Fps;
        List += FString::Printf(TEXT("file '%06d.jpg'\nduration %.6f\n"), I, FMath::Max(Duration, 0.001));
    }
    List += FString::Printf(TEXT("file '%06d.jpg'\n"), Times.Num() - 1);
    const FString ListFile = FPaths::Combine(FrameDir, TEXT("frames.txt"));
    FFileHelper::SaveStringToFile(List, *ListFile);
    const FString Scale = Width > 0 ? FString::Printf(TEXT("scale=%d:-2,"), Width) : FString();
    const FString Args = FString::Printf(TEXT("-y -hide_banner -loglevel error -f concat -safe 0 -i \"%s\" -vf \"%spad=ceil(iw/2)*2:ceil(ih/2)*2\" -fps_mode cfr -r %.0f ")
                                         TEXT("-c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p -movflags +faststart \"%s\""),
                                         *ListFile, *Scale, Fps, *Path);
    Encoder = FPlatformProcess::CreateProc(*FfmpegExe(), *Args, /*bLaunchDetached=*/false, /*bLaunchHidden=*/true, /*bLaunchReallyHidden=*/true, nullptr, 0,
                                           *FrameDir, nullptr);
    if (!Encoder.IsValid()) { Fail(FString::Printf(TEXT("ffmpeg could not be started (%s; -SCFfmpeg=<exe>); frames kept in %s"), *FfmpegExe(), *FrameDir)); return; }
    EncodeStarted = FPlatformTime::Seconds();
    Phase = EPhase::Encoding;
}

void FSCVideoRecorder::Fail(const FString& Why)
{
    Error = Why;
    Phase = EPhase::Failed;
    UE_LOG(LogSCRpcVideo, Warning, TEXT("RPC_RECORD failed: %s"), *Why);
}

TSharedRef<FJsonObject> FSCVideoRecorder::Describe() const
{
    TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
    O->SetBoolField(TEXT("ok"), Phase != EPhase::Failed);
    O->SetStringField(TEXT("path"), Path);
    O->SetStringField(TEXT("phase"), Phase == EPhase::Recording ? TEXT("recording") : Phase == EPhase::Draining ? TEXT("draining")
                                     : Phase == EPhase::Encoding ? TEXT("encoding") : Phase == EPhase::Done ? TEXT("done")
                                     : Phase == EPhase::Failed ? TEXT("failed") : TEXT("idle"));
    O->SetNumberField(TEXT("frames"), Times.Num());
    O->SetNumberField(TEXT("dropped"), Dropped);
    O->SetField(TEXT("seconds"), SCRpcJson::Number(Times.IsEmpty() ? 0.0 : Times.Last() - Times[0] + 1.0 / Fps, 0.1));
    const FString CurrentFormat = Extension ? Extension->GetFormatName() : Format;
    if (!CurrentFormat.IsEmpty()) O->SetStringField(TEXT("source_format"), CurrentFormat);
    O->SetBoolField(TEXT("source_hdr"), bSawHDR || (Extension && Extension->SawHDR.load()));
    if (Phase == EPhase::Done) O->SetNumberField(TEXT("bytes"), double(IFileManager::Get().FileSize(*Path)));
    if (!Error.IsEmpty()) { O->SetStringField(TEXT("error"), Error); O->SetStringField(TEXT("frames_dir"), FrameDir); }
    return O;
}
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCRpcVideoSourceTest, "SCBridge.RpcVideo.SourceFormat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCRpcVideoSourceTest::RunTest(const FString&)
{
    TestFalse(TEXT("SDR 10-bit surface"), IsHDRPreviewSource(PF_A2B10G10R10, false));
    TestTrue(TEXT("PQ device with 10-bit surface"), IsHDRPreviewSource(PF_A2B10G10R10, true));
    TestTrue(TEXT("linear float surface"), IsHDRPreviewSource(PF_FloatRGBA, false));
    TestFalse(TEXT("SDR preview surface"), IsHDRPreviewSource(PF_B8G8R8A8, false));
    return true;
}
#endif
#endif // SC_WITH_RPC
