// Video capture of the game view for the RPC harness (WP-H1): the scene of each captured frame copied from the game
// viewport's render target after the view family renders (before the UI; a scene view extension and an asynchronous
// GPU readback: no stall, and it works with -RenderOffScreen), at a target rate, written as JPEGs off the game thread
// with their capture times, then encoded to H.264 MP4 at those times by a hidden ffmpeg (-SCFfmpeg=<exe>, else ffmpeg
// on PATH). Without ffmpeg the frames and their timing list are kept and the answer says so.
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include <atomic>

class FSCRecordExtension;

class FSCVideoRecorder
{
public:
    FSCVideoRecorder();
    ~FSCVideoRecorder(); // both here and in the .cpp: FSCRecordExtension is complete only there
    /** Starts capturing (game thread). Path: the .mp4 to write; Width: output width (0: the view's; ffmpeg scales). */
    bool Start(const FString& Path, double Fps, double MaxSeconds, int32 Width, bool bKeepFrames, FString& OutError);
    /** Game thread, every frame: requests frames at the rate, writes the captured ones, stops at MaxSeconds, then
     *  drains, encodes and cleans up. */
    void Tick();
    /** Ends the capture; the file is finished some time later (IsFinished). */
    void Stop();
    bool IsRecording() const { return Phase == EPhase::Recording; }
    bool IsBusy() const { return Phase == EPhase::Recording || Phase == EPhase::Draining || Phase == EPhase::Encoding; }
    bool IsFinished() const { return Phase == EPhase::Done || Phase == EPhase::Failed; }
    /** ok, path, phase, frames, dropped, seconds, bytes (or error, frames_dir). */
    TSharedRef<FJsonObject> Describe() const;

private:
    enum class EPhase : uint8 { Idle, Recording, Draining, Encoding, Done, Failed };
    void Fail(const FString& Why);
    void Encode();
    void Collect();
    EPhase Phase = EPhase::Idle;
    TSharedPtr<FSCRecordExtension, ESPMode::ThreadSafe> Extension;
    FString Path, FrameDir, Error, Format;
    double Fps = 30, MaxSeconds = 300, Started = 0, NextAt = 0, StoppedAt = 0;
    int32 Width = 0;
    bool bKeepFrames = false;
    TArray<double> Times; // capture time of every frame written (index = file number)
    int32 Dropped = 0;
    TSharedRef<std::atomic<int32>> InFlight = MakeShared<std::atomic<int32>>(0);
    TSharedRef<std::atomic<int32>> WriteFailures = MakeShared<std::atomic<int32>>(0);
    FProcHandle Encoder;
    double EncodeStarted = 0;
};
