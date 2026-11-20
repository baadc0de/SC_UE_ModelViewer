// Runtime fixed-step scene capture used by /render.
#pragma once
#include "CoreMinimal.h"
#include "SCRpcJson.h"
#include "UObject/StrongObjectPtr.h"
#include "HAL/PlatformProcess.h"

#include "Components/SceneCaptureComponent2D.h"
class FSCRenderMainLook;

class FSCRpcRender
{
public:
    static constexpr ESceneCaptureSource PngSource = SCS_FinalToneCurveHDR;
    static constexpr ESceneCaptureSource ExrSource = SCS_FinalColorHDR;
    using FCamera = TFunction<TSharedRef<FJsonObject>(const TSharedPtr<FJsonObject>&)>;
    ~FSCRpcRender();
    bool Start(UWorld* World, const TSharedPtr<FJsonObject>& Body, FCamera Camera, FString& Error);
    void Tick(bool bSceneReady, bool bStreaming);
    void Stop(const FString& Reason);
    bool IsBusy() const { return bActive; }
    bool IsCapturing() const { return bFixed; }
    TSharedRef<FJsonObject> Describe() const;
private:
    bool Apply(const TSharedPtr<FJsonObject>& Action, double Elapsed, bool bInstant, FString& Error);
    bool CaptureFrame(bool bWrite, FString& Error);
    bool WriteSidecar();
    void EndCapture();
    void BeginEncode();
    FString Codec, Encoded;
    FProcHandle Encoder;
    bool bActive = false, bFixed = false, bOldFixed = false;
    double OldDelta = 0, Started = 0, Finished = 0, Fps = 30, QuietSince = -1;
    int32 Width = 0, Height = 0, Total = 0, Warmup = 0, Remaining = 0, Index = 0, NextStep = 0, Holds = 0;
    FString Dir, Failure;
    TArray<FString> Outputs;
    TArray<TSharedPtr<FJsonValue>> Frames, Steps;
    TSharedPtr<FJsonObject> Request, Motion;
    double MotionStart = 0;
    TWeakObjectPtr<UWorld> World;
    FCamera ApplyCamera;
    TOptional<FTransform> CapturePose;
    TSharedPtr<FSCRenderMainLook, ESPMode::ThreadSafe> MainLook;
    TStrongObjectPtr<USceneCaptureComponent2D> Sdr, Linear;
};
