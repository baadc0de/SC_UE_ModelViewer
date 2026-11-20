#include "SCRpcRender.h"
#if SC_WITH_RPC
#include "SCRenderPath.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "Engine/GameInstance.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "SCViewerSubsystem.h"
#include "SCVehicle.h"
#include "SCViewerPawn.h"
#include "Camera/CameraComponent.h"
#include "UnrealClient.h"

// Scene captures disable Lumen GI/reflections and halve the surface cache after applying the world's
// volumes. Restore just those overrides from the player's evaluated view. Each capture still evaluates
// its own position, exposure, post-process volumes and tone-mapping output device.
class FSCRenderMainLook final : public FSceneViewExtensionBase
{
public:
    FSCRenderMainLook(const FAutoRegister& Register, const FRenderTarget* Viewport)
        : FSceneViewExtensionBase(Register), MainTarget(Viewport) {}
    virtual void SetupViewFamily(FSceneViewFamily& Family) override
    {
        if (Family.RenderTarget == SdrTarget || Family.RenderTarget == LinearTarget)
            Family.Time = FGameTime::CreateUndilated(ShotTime, FrameDelta);
    }
    virtual void BeginRenderViewFamily(FSceneViewFamily&) override {}
    virtual void SetupView(FSceneViewFamily& Family, FSceneView& View) override
    {
        if (Family.RenderTarget == MainTarget)
        {
            GI = View.FinalPostProcessSettings.DynamicGlobalIlluminationMethod;
            Reflections = View.FinalPostProcessSettings.ReflectionMethod;
            Resolution = View.FinalPostProcessSettings.LumenSurfaceCacheResolution;
            if (View.State) MainExposure = View.State->GetLastAutoExposureReadback().Exposure;
            bReady = true;
        }
        else if (bReady && (Family.RenderTarget == SdrTarget || Family.RenderTarget == LinearTarget))
        {
            View.OverrideFrameIndexValue = SampleIndex;
            View.OverrideOutputFrameIndexValue = OutputIndex;
            CaptureAAMethod = View.AntiAliasingMethod;
            View.FinalPostProcessSettings.DynamicGlobalIlluminationMethod = GI;
            View.FinalPostProcessSettings.ReflectionMethod = Reflections;
            View.FinalPostProcessSettings.LumenSurfaceCacheResolution = Resolution;
            DisplayMethod = View.FinalPostProcessSettings.TonemappingMethod;
            SSDODirect = View.FinalPostProcessSettings.SSDODirectAmount;
            SSDOAmbient = View.FinalPostProcessSettings.SSDOAmbientAmount;
            SSDOReflections = View.FinalPostProcessSettings.SSDOReflectionAmount;
        }
    }
    const FRenderTarget* SdrTarget = nullptr;
    const FRenderTarget* LinearTarget = nullptr;
    uint32 SampleIndex = 0;
    uint32 OutputIndex = 0;
    double ShotTime = 0;
    float FrameDelta = 0;
    float MainExposure = 0;
    EAntiAliasingMethod CaptureAAMethod = AAM_None;
    bool bReady = false;
    ETonemappingMethod DisplayMethod{};
    float SSDODirect = 0, SSDOAmbient = 0, SSDOReflections = 0;
private:
    const FRenderTarget* MainTarget;
    EDynamicGlobalIlluminationMethod::Type GI = EDynamicGlobalIlluminationMethod::None;
    EReflectionMethod::Type Reflections = EReflectionMethod::None;
    float Resolution = 1;
};
using namespace SCRpcJson;
namespace
{
double Value(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, double Default)
{
    double V;
    return O->TryGetNumberField(Key, V) ? V : Default;
}
FString String(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, const FString& Default = {})
{
    FString S;
    return O->TryGetStringField(Key, S) ? S : Default;
}
bool Triple(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, FVector& V)
{
    const TArray<TSharedPtr<FJsonValue>>* A;
    return O->TryGetArrayField(Key, A) && A->Num() == 3 &&
        (*A)[0]->TryGetNumber(V.X) && (*A)[1]->TryGetNumber(V.Y) && (*A)[2]->TryGetNumber(V.Z) && Finite(V);
}
bool ValidateAction(const TSharedPtr<FJsonObject>& A, FString& Error)
{
    int32 NumActions = 0;
    for (const TCHAR* Kind : {TEXT("move"), TEXT("orbit"), TEXT("camera"), TEXT("console")})
        NumActions += A->HasField(Kind);
    if (NumActions != 1) { Error = TEXT("each render action requires exactly one camera, console, move or orbit"); return false; }
    if (A->HasField(TEXT("console")))
    {
        if (String(A, TEXT("console")).IsEmpty()) { Error = TEXT("console requires a nonempty command"); return false; }
        return true;
    }
    if (A->HasField(TEXT("camera")))
    {
        if (!A->HasTypedField<EJson::Object>(TEXT("camera"))) { Error = TEXT("camera requires an object"); return false; }
        return true;
    }
    const bool bOrbit = A->HasField(TEXT("orbit"));
    const TSharedPtr<FJsonObject>* Motion;
    if (!A->TryGetObjectField(bOrbit ? TEXT("orbit") : TEXT("move"), Motion))
    { Error = TEXT("motion requires an object"); return false; }
    const auto& M = *Motion;
    const FString Ease = String(M, TEXT("ease"), bOrbit ? TEXT("linear") : TEXT("inOut"));
    if (Value(M, TEXT("seconds"), 0) <= 0 || (Ease != TEXT("linear") && Ease != TEXT("inOut")))
    { Error = TEXT("motion requires positive seconds and linear or inOut ease"); return false; }
    FVector V;
    if (bOrbit)
    {
        if (Value(M, TEXT("radius"), 0) <= 0 || (!Triple(M, TEXT("center"), V) && String(M, TEXT("center")) != TEXT("vehicle")))
        { Error = TEXT("orbit requires positive radius and a center"); return false; }
        return true;
    }
    const TSharedPtr<FJsonObject>* From;
    const TSharedPtr<FJsonObject>* To;
    if (!M->TryGetObjectField(TEXT("from"), From) || !M->TryGetObjectField(TEXT("to"), To) ||
        !Triple(*From, TEXT("at"), V) || !Triple(*To, TEXT("at"), V) ||
        !((Triple(*From, TEXT("lookAt"), V) && Triple(*To, TEXT("lookAt"), V)) ||
          (Triple(*From, TEXT("rot"), V) && Triple(*To, TEXT("rot"), V)) ||
          (!(*From)->HasField(TEXT("rot")) && !(*From)->HasField(TEXT("lookAt")) &&
           !(*To)->HasField(TEXT("rot")) && !(*To)->HasField(TEXT("lookAt")))))
    { Error = TEXT("move requires two positions with matching lookAt or rot"); return false; }
    return true;
}
bool ValidateSchedule(const TSharedPtr<FJsonObject>& Body, FIntPoint ViewSize, double& Fps, int32& Total,
                      int32& Width, int32& Height, int32& Warmup, FString& Error)
{
    for (const TCHAR* Key : {TEXT("fps"), TEXT("frames"), TEXT("seconds"), TEXT("width"), TEXT("height"), TEXT("warmup_frames")})
    {
        if (Body->HasField(Key) && !Body->HasTypedField<EJson::Number>(Key))
        { Error = FString(Key) + TEXT(" must be numeric"); return false; }
    }
    Fps = Value(Body, TEXT("fps"), 30);
    const double Count = Value(Body, TEXT("frames"), FMath::RoundToDouble(Value(Body, TEXT("seconds"), 0) * Fps));
    const double W = Value(Body, TEXT("width"), ViewSize.X);
    const double H = Value(Body, TEXT("height"), ViewSize.Y);
    const double Warm = Value(Body, TEXT("warmup_frames"), 32);
    if (!(Fps > 0 && Fps <= 240 && Count >= 1 && Count <= MAX_int32 && Count == FMath::FloorToDouble(Count)
        && W >= 1 && W <= 16384 && H >= 1 && H <= 16384 && W == FMath::FloorToDouble(W) && H == FMath::FloorToDouble(H)
        && Warm >= 0 && Warm <= MAX_int32 && Warm == FMath::FloorToDouble(Warm)))
    { Error = TEXT("invalid fps, length, dimensions or warmup_frames"); return false; }
    if ((Body->HasField(TEXT("steps")) && !Body->HasTypedField<EJson::Array>(TEXT("steps"))) ||
        (Body->HasField(TEXT("setup")) && !Body->HasTypedField<EJson::Array>(TEXT("setup"))))
    { Error = TEXT("steps and setup must be arrays"); return false; }
    Width = int32(W); Height = int32(H); Total = int32(Count); Warmup = int32(Warm);
    return true;
}
USceneCaptureComponent2D* CreateCapture(UWorld* World, int32 Width, int32 Height, ESceneCaptureSource Source)
{
    auto* Capture = NewObject<USceneCaptureComponent2D>(World);
    Capture->bCaptureEveryFrame = false;
    Capture->bCaptureOnMovement = false;
    Capture->bAlwaysPersistRenderingState = true;
    Capture->CaptureSource = Source;
    auto* Target = NewObject<UTextureRenderTarget2D>(Capture);
    Target->InitCustomFormat(Width, Height, PF_FloatRGBA, true);
    Capture->TextureTarget = Target;
    Capture->RegisterComponentWithWorld(World);
    // OnRegister rebuilds ShowFlags from the archetype, disabling temporal AA and translucency.
    const auto& Flags = GEngine->GameViewport->EngineShowFlags;
    Capture->ShowFlags.SetTemporalAA(Flags.TemporalAA);
    Capture->ShowFlags.SetMotionBlur(Flags.MotionBlur);
    Capture->ShowFlags.SetSeparateTranslucency(Flags.SeparateTranslucency);
    return Capture;
}
// The readback already has a finite set of half-float bit patterns. Compute the transfer once
// so writing several megapixels does not require three pow() calls per pixel, per PNG format.
struct FHalfToSRGB16
{
    uint16 Codes[1 << 16];
    FHalfToSRGB16()
    {
        for (uint32 Bits = 0; Bits < UE_ARRAY_COUNT(Codes); ++Bits)
        {
            FFloat16 Half;
            Half.Encoded = uint16(Bits);
            const double Linear = FMath::Clamp(double(Half.GetFloat()), 0.0, 1.0);
            const double Gamma = Linear <= 0.0031308 ? 12.92 * Linear : 1.055 * FMath::Pow(Linear, 1.0 / 2.4) - 0.055;
            Codes[Bits] = FMath::IsNaN(Half.GetFloat()) ? 0 : uint16(FMath::RoundToInt(MAX_uint16 * Gamma));
        }
    }
};
const FHalfToSRGB16& SRGBCodes()
{
    static const FHalfToSRGB16 Table;
    return Table;
}
template<typename T> void EncodeSDR(const TArray<FFloat16Color>& Pixels, TArray64<T>& Dest)
{
    const auto& Table = SRGBCodes();
    auto Channel = [&Table](FFloat16 Half) -> T
    {
        const uint16 Encoded = Table.Codes[Half.Encoded];
        if constexpr (sizeof(T) == 1) return T(Encoded >> 8);
        else return T(Encoded);
    };
    Dest.Reserve(int64(Pixels.Num()) * 4);
    for (const FFloat16Color& P : Pixels)
    {
        Dest.Add(Channel(P.R));
        Dest.Add(Channel(P.G));
        Dest.Add(Channel(P.B));
        Dest.Add(TNumericLimits<T>::Max());
    }
}
bool SaveFrame(const FString& Filename, const FString& Format, const TArray<FFloat16Color>& Pixels, int32 W, int32 H)
{
    const bool bEXR = Format == TEXT("exr");
    auto& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    auto Wrapper = Module.CreateImageWrapper(bEXR ? EImageFormat::EXR : EImageFormat::PNG);
    bool OK = false;
    if (bEXR)
        OK = Wrapper->SetRaw(Pixels.GetData(), int64(Pixels.Num()) * sizeof(FFloat16Color), W, H, ERGBFormat::RGBAF, 16);
    else if (Format == TEXT("png8"))
    {
        TArray64<uint8> Bytes;
        EncodeSDR(Pixels, Bytes);
        OK = Wrapper->SetRaw(Bytes.GetData(), Bytes.Num(), W, H, ERGBFormat::RGBA, 8);
    }
    else
    {
        TArray64<uint16> Words;
        EncodeSDR(Pixels, Words);
        OK = Wrapper->SetRaw(Words.GetData(), Words.Num() * sizeof(uint16), W, H, ERGBFormat::RGBA, 16);
    }
    if (!OK) return false;
    const TArray64<uint8>& Data = Wrapper->GetCompressed();
    return !Data.IsEmpty() && FFileHelper::SaveArrayToFile(Data, *Filename);
}
FString FileAt(const FString& Dir, const FString& Format, int32 Index)
{
    return Dir / Format / FString::Printf(TEXT("%06d.%s"), Index, Format == TEXT("exr") ? TEXT("exr") : TEXT("png"));
}
}
FSCRpcRender::~FSCRpcRender() { Stop(TEXT("shutdown")); }

bool FSCRpcRender::Start(UWorld* InWorld, const TSharedPtr<FJsonObject>& Body, FCamera Camera, FString& Error)
{
    if (bActive) { Error = TEXT("render already running"); return false; }
    if (!InWorld || !InWorld->GetFirstPlayerController() || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
    { Error = TEXT("render requires a game viewport"); return false; }
    if (!ValidateSchedule(Body, GEngine->GameViewport->Viewport->GetSizeXY(), Fps, Total, Width, Height, Warmup, Error))
        return false;
    Remaining = Warmup;
    Outputs.Reset();
    const TArray<TSharedPtr<FJsonValue>>* Array;
    if (!Body->HasField(TEXT("outputs"))) Outputs.Add(TEXT("png16"));
    else if (Body->TryGetArrayField(TEXT("outputs"), Array))
        for (const auto& Item : *Array)
        {
            FString F;
            if (!Item->TryGetString(F) || (F != TEXT("png8") && F != TEXT("png16") && F != TEXT("exr")))
            { Error = TEXT("unknown output format"); return false; }
            Outputs.AddUnique(F);
        }
    Encoded.Reset();
    Codec = String(Body, TEXT("encode"));
    if ((!Codec.IsEmpty() && Codec != TEXT("h264") && Codec != TEXT("prores")) ||
        (Codec == TEXT("prores") && !Outputs.Contains(TEXT("png16"))) ||
        (Codec == TEXT("h264") && !(Outputs.Contains(TEXT("png16")) || Outputs.Contains(TEXT("png8")))))
    { Error = TEXT("encode requires prores with png16, or h264 with png16/png8"); return false; }
    if (Outputs.IsEmpty()) { Error = TEXT("outputs must include a format"); return false; }
    Dir = String(Body, TEXT("path"));
    if (Dir.IsEmpty())
    {
        Dir = FPaths::ProjectSavedDir() / TEXT("Renders");
        FParse::Value(FCommandLine::Get(), TEXT("SCShotDir="), Dir);
        Dir /= String(Body, TEXT("name"), FGuid::NewGuid().ToString());
    }
    Dir = FPaths::ConvertRelativePathToFull(Dir);
    for (const FString& F : Outputs)
    {
        TArray<FString> Existing;
        IFileManager::Get().FindFiles(Existing, *(Dir / F / TEXT("*")), true, false);
        if (!Existing.IsEmpty() || !IFileManager::Get().MakeDirectory(*(Dir / F), true))
        { Error = TEXT("output directory occupied or inaccessible: ") + Dir / F; return false; }
    }
    if (IFileManager::Get().FileExists(*(Dir / TEXT("render.json"))))
    { Error = TEXT("render.json already exists in the output directory"); return false; }
    Steps.Reset(); Frames.Reset(); Motion.Reset(); CapturePose.Reset(); Failure.Reset(); Index = NextStep = Holds = 0; QuietSince = -1;
    for (const TCHAR* Key : {TEXT("setup"), TEXT("steps")})
    {
        if (!Body->TryGetArrayField(Key, Array)) continue;
        for (const auto& Item : *Array)
        {
            if (!Item || Item->Type != EJson::Object) { Error = TEXT("render action must be an object"); return false; }
            if (!ValidateAction(Item->AsObject(), Error)) return false;
        }
    }
    if (Body->TryGetArrayField(TEXT("steps"), Array)) Steps = *Array;
    for (const auto& Step : Steps)
        if (Step->Type != EJson::Object || (Value(Step->AsObject(), TEXT("at"), -1) < 0 || Value(Step->AsObject(), TEXT("at"), -1) > (Total - 1) / Fps))
        { Error = TEXT("steps require a nonnegative at"); return false; }
    Steps.StableSort([](const auto& A, const auto& B) { return Value(A->AsObject(), TEXT("at"), 0) < Value(B->AsObject(), TEXT("at"), 0); });
    Request = Body; World = InWorld; ApplyCamera = MoveTemp(Camera);
    if (Body->TryGetArrayField(TEXT("setup"), Array))
        for (const auto& Step : *Array)
            if (Step->Type != EJson::Object || !Apply(Step->AsObject(), 0, true, Error)) return false;
    if (Outputs.Contains(TEXT("exr"))) Linear.Reset(CreateCapture(InWorld, Width, Height, ExrSource));
    if (Outputs.Contains(TEXT("png8")) || Outputs.Contains(TEXT("png16"))) Sdr.Reset(CreateCapture(InWorld, Width, Height, PngSource));
    MainLook = FSceneViewExtensions::NewExtension<FSCRenderMainLook>(GEngine->GameViewport->Viewport);
    if (Sdr) MainLook->SdrTarget = Sdr->TextureTarget->GameThread_GetRenderTargetResource();
    if (Linear) MainLook->LinearTarget = Linear->TextureTarget->GameThread_GetRenderTargetResource();
    Started = FPlatformTime::Seconds(); Finished = 0; bActive = true;
    return true;
}

bool FSCRpcRender::Apply(const TSharedPtr<FJsonObject>& A, double Elapsed, bool bInstant, FString& Error)
{
    const TSharedPtr<FJsonObject>* Obj;
    FString Cmd;
    if (A->TryGetObjectField(TEXT("camera"), Obj))
    {
        CapturePose.Reset();
        FVector At, Rot, Look;
        if (Triple(*Obj, TEXT("at"), At))
        {
            if (Triple(*Obj, TEXT("look_at"), Look)) CapturePose = FTransform((Look - At).Rotation(), At * 100);
            else if (Triple(*Obj, TEXT("rot"), Rot)) CapturePose = FTransform(FRotator(Rot.X, Rot.Y, Rot.Z), At * 100);
        }
        const auto Answer = ApplyCamera(*Obj);
        return !Answer->TryGetStringField(TEXT("error"), Error);
    }
    if (A->TryGetStringField(TEXT("console"), Cmd))
    {
        if (bInstant) World->GetFirstPlayerController()->ConsoleCommand(Cmd);
        return true;
    }
    const bool bOrbit = A->TryGetObjectField(TEXT("orbit"), Obj);
    if (!bOrbit && !A->TryGetObjectField(TEXT("move"), Obj)) { Error = TEXT("unknown render action"); return false; }
    const auto& M = *Obj;
    const double Duration = Value(M, TEXT("seconds"), 0);
    const FString Ease = String(M, TEXT("ease"), bOrbit ? TEXT("linear") : TEXT("inOut"));
    if (Ease != TEXT("linear") && Ease != TEXT("inOut")) { Error = TEXT("ease must be linear or inOut"); return false; }
    if (Duration <= 0) { Error = TEXT("motion requires positive seconds"); return false; }
    const auto U = SCRenderPath::Progress(Elapsed, Duration, Ease == TEXT("inOut"));
    SCRenderPath::FPose Pose;
    if (bOrbit)
    {
        FVector Center;
        if (String(M, TEXT("center")) == TEXT("vehicle"))
        {
            auto* V = World->GetGameInstance()->GetSubsystem<USCViewerSubsystem>();
            if (!V || !V->GetVehicle()) { Error = TEXT("no loaded vehicle for orbit"); return false; }
            Center = V->GetVehicle()->GetActorLocation() / 100;
        }
        else if (!Triple(M, TEXT("center"), Center)) { Error = TEXT("orbit requires center"); return false; }
        Pose = SCRenderPath::Orbit(Center, Value(M,TEXT("radius"),0), Value(M,TEXT("height"),0), Value(M,TEXT("fromDeg"),0), Value(M,TEXT("toDeg"),360), U);
    }
    else
    {
        const TSharedPtr<FJsonObject>* From; const TSharedPtr<FJsonObject>* To;
        FVector P, Q, R, S;
        if (!M->TryGetObjectField(TEXT("from"), From) || !M->TryGetObjectField(TEXT("to"), To) ||
            !Triple(*From,TEXT("at"),P) || !Triple(*To,TEXT("at"),Q))
        { Error = TEXT("move requires from and to poses"); return false; }
        if (Triple(*From,TEXT("lookAt"),R) && Triple(*To,TEXT("lookAt"),S)) Pose = SCRenderPath::MoveLookingAt(P,Q,R,S,U);
        else if (Triple(*From,TEXT("rot"),R) && Triple(*To,TEXT("rot"),S))
            Pose = SCRenderPath::Move({P,FRotator(R.X,R.Y,R.Z)}, {Q,FRotator(S.X,S.Y,S.Z)}, U);
        else if (!(*From)->HasField(TEXT("rot")) && !(*From)->HasField(TEXT("lookAt")) &&
                 !(*To)->HasField(TEXT("rot")) && !(*To)->HasField(TEXT("lookAt")))
        {
            const FRotator Aim = CapturePose.IsSet() ? CapturePose->Rotator() :
                World->GetFirstPlayerController()->PlayerCameraManager->GetCameraRotation();
            Pose = SCRenderPath::Move({P, Aim}, {Q, Aim}, U);
        }
        else { Error = TEXT("both poses require lookAt or both require rot"); return false; }
    }
    CapturePose = FTransform(Pose.Rotation, Pose.Position * 100);
    auto Camera = MakeShared<FJsonObject>();
    Camera->SetField(TEXT("at"), Vector(Pose.Position, 0.000001));
    Camera->SetField(TEXT("rot"), Rotation(Pose.Rotation));
    const auto Answer = ApplyCamera(Camera);
    return !Answer->TryGetStringField(TEXT("error"), Error);
}
bool FSCRpcRender::CaptureFrame(bool bWrite, FString& Error)
{
    auto* PC = World.IsValid() ? World->GetFirstPlayerController() : nullptr;
    if (!PC || !PC->PlayerCameraManager || !PC->GetLocalPlayer()) { Error = TEXT("render world or player camera disappeared"); return false; }
    FMinimalViewInfo View = PC->PlayerCameraManager->GetCameraCacheView();
    auto* Viewer = World->GetGameInstance()->GetSubsystem<USCViewerSubsystem>();
    if (Viewer && Viewer->GetCameraMode() != ESCCameraMode::FirstPerson)
        if (const auto* Pawn = Viewer->GetViewerPawn()) Pawn->Camera->GetCameraView(0, View);
    if (CapturePose.IsSet())
    {
        View.Location = CapturePose->GetTranslation();
        View.Rotation = CapturePose->Rotator();
    }
    const auto Frame = MakeShared<FJsonObject>();
    Frame->SetNumberField(TEXT("index"), Index);
    Frame->SetNumberField(TEXT("t"), Index / Fps);
    Frame->SetField(TEXT("at"), Metres(View.Location));
    Frame->SetField(TEXT("rot"), Rotation(View.Rotation));
    Frame->SetNumberField(TEXT("fov"), View.FOV);
    Frame->SetNumberField(TEXT("holds"), Holds);
    Frame->SetNumberField(TEXT("viewport_exposure"), MainLook->MainExposure);
    // Match Movie Pipeline's per-view temporal seed and material clock. Streaming holds repeat
    // the current sample. Both targets see the same time even when the readbacks take seconds.
    MainLook->OutputIndex = Index;
    MainLook->SampleIndex = Warmup - Remaining + Index;
    MainLook->ShotTime = Index / Fps;
    MainLook->FrameDelta = 1.0 / Fps;
    FSceneViewProjectionData Projection;
    Projection.SetViewRectangle(FIntRect(0, 0, Width, Height));
    FMinimalViewInfo::CalculateProjectionMatrixGivenView(View, PC->GetLocalPlayer()->AspectRatioAxisConstraint,
        GEngine->GameViewport->Viewport, Projection);
    const FIntRect Rect = Projection.GetConstrainedViewRect();
    // Embed the player's constrained rectangle in the requested full-size texture. This keeps
    // the same horizontal/vertical FOV rules even if the requested aspect differs from the window.
    const FScaleMatrix ClipScale(FVector(double(Rect.Width()) / Width, double(Rect.Height()) / Height, 1));
    const FTranslationMatrix ClipOffset(FVector(double(Rect.Min.X + Rect.Max.X) / Width - 1,
        1 - double(Rect.Min.Y + Rect.Max.Y) / Height, 0));
    for (auto* Capture : {Sdr.Get(), Linear.Get()})
    {
        if (!Capture) continue;
        Capture->SetCameraView(View);
        Capture->bUseCustomProjectionMatrix = true;
        Capture->CustomProjectionMatrix = Projection.ProjectionMatrix * ClipScale * ClipOffset;
        Capture->PostProcessSettings = View.PostProcessSettings;
        Capture->PostProcessBlendWeight = View.PostProcessBlendWeight;
        Capture->CaptureScene();
        if (!bWrite) continue;
        TArray<FFloat16Color> Pixels;
        auto* Resource = Capture->TextureTarget->GameThread_GetRenderTargetResource();
        if (!Resource || !Resource->ReadFloat16Pixels(Pixels, FReadSurfaceDataFlags(RCM_MinMax)) || Pixels.Num() != Width * Height)
        { Error = TEXT("incomplete floating-point scene readback"); return false; }
        if (Rect != FIntRect(0, 0, Width, Height))
        {
            const FFloat16Color Black(FLinearColor::Black);
            for (int32 Y = 0; Y < Height; ++Y)
            {
                const bool bBorder = Y < Rect.Min.Y || Y >= Rect.Max.Y;
                auto* Row = Pixels.GetData() + Y * Width;
                for (int32 X = 0; X < (bBorder ? Width : Rect.Min.X); ++X) Row[X] = Black;
                if (!bBorder)
                    for (int32 X = Rect.Max.X; X < Width; ++X) Row[X] = Black;
            }
        }
        if (auto* State = Capture->GetViewState(0); State && (Capture == Sdr.Get() || !Sdr))
            Frame->SetNumberField(TEXT("exposure"), State->GetLastAutoExposureReadback().Exposure);
        for (const auto& Format : Outputs)
        {
            if ((Format == TEXT("exr")) != (Capture == Linear.Get())) continue;
            const FString File = FileAt(Dir, Format, Index);
            if (!SaveFrame(File, Format, Pixels, Width, Height)) { Error = TEXT("write failed: ") + File; return false; }
        }
    }
    if (bWrite)
    {
        Frame->SetStringField(TEXT("display_method"), StaticEnum<ETonemappingMethod>()->GetNameStringByValue(int64(MainLook->DisplayMethod)));
        Frame->SetStringField(TEXT("anti_aliasing_method"), StaticEnum<EAntiAliasingMethod>()->GetNameStringByValue(int64(MainLook->CaptureAAMethod)));
        Frame->SetNumberField(TEXT("ssdo_direct"), MainLook->SSDODirect);
        Frame->SetNumberField(TEXT("ssdo_ambient"), MainLook->SSDOAmbient);
        Frame->SetNumberField(TEXT("ssdo_reflections"), MainLook->SSDOReflections);
        Frames.Add(MakeShared<FJsonValueObject>(Frame));
    }
    return true;
}
void FSCRpcRender::Tick(bool bSceneReady, bool bStreaming)
{
    if (!bActive) return;
    if (Encoder.IsValid())
    {
        if (FPlatformProcess::IsProcRunning(Encoder)) return;
        int32 Code = -1;
        FPlatformProcess::GetProcReturnCode(Encoder, &Code);
        FPlatformProcess::CloseProc(Encoder);
        Encoder.Reset();
        Stop(Code == 0 && IFileManager::Get().FileSize(*Encoded) > 0 ? FString() : FString::Printf(TEXT("ffmpeg failed (exit %d)"), Code));
        return;
    }
    if (!World.IsValid()) { Stop(TEXT("world destroyed")); return; }
    if (!bFixed)
    {
        if (!bSceneReady || !MainLook->bReady) { QuietSince = -1; return; }
        if (QuietSince < 0) QuietSince = FPlatformTime::Seconds();
        if (FPlatformTime::Seconds() - QuietSince < 0.5) return;
        bOldFixed = FApp::UseFixedTimeStep();
        OldDelta = FApp::GetFixedDeltaTime();
        FApp::SetUseFixedTimeStep(true);
        FApp::SetFixedDeltaTime(1 / Fps);
        bFixed = true;
        return;
    }
    FString Error;
    if (Remaining > 0)
    {
        if (!CaptureFrame(false, Error)) { Stop(Error); return; }
        --Remaining;
        return;
    }
    const double T = Index / Fps;
    while (NextStep < Steps.Num() && Value(Steps[NextStep]->AsObject(), TEXT("at"), 0) <= T)
    {
        const auto A = Steps[NextStep++]->AsObject();
        if (A->HasField(TEXT("move")) || A->HasField(TEXT("orbit")))
        { Motion = A; MotionStart = Value(A, TEXT("at"), 0); }
        else
        {
            if (!Apply(A, 0, true, Error)) { Stop(Error); return; }
            if (A->HasField(TEXT("camera"))) Motion.Reset();
        }
    }
    if (Motion && !Apply(Motion, T - MotionStart, false, Error)) { Stop(Error); return; }
    if (!CaptureFrame(!bStreaming, Error)) { Stop(Error); return; }
    if (bStreaming)
    {
        ++Holds;
        FApp::SetFixedDeltaTime(0);
        return;
    }
    FApp::SetFixedDeltaTime(1 / Fps);
    Holds = 0;
    if (++Index == Total)
    {
        if (Codec.IsEmpty()) Stop({});
        else BeginEncode();
    }
}
bool FSCRpcRender::WriteSidecar()
{
    auto O = Describe();
    O->SetObjectField(TEXT("request"), Request);
    O->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
    O->SetStringField(TEXT("engine_branch"), FEngineVersion::Current().GetBranch());
    FString ProjectRepo = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../.."));
    FPaths::CollapseRelativeDirectories(ProjectRepo);
    if (FPaths::FileExists(ProjectRepo / TEXT(".git")) || FPaths::DirectoryExists(ProjectRepo / TEXT(".git")))
    {
        FString Revision;
        int32 ExitCode = -1;
        const FString Args = FString::Printf(TEXT("-C \"%s\" rev-parse --abbrev-ref HEAD"), *ProjectRepo);
        if (FPlatformProcess::ExecProcess(TEXT("git"), *Args, &ExitCode, &Revision, nullptr) && ExitCode == 0)
            O->SetStringField(TEXT("branch"), Revision.TrimStartAndEnd());
        const FString CommitArgs = FString::Printf(TEXT("-C \"%s\" rev-parse HEAD"), *ProjectRepo);
        if (FPlatformProcess::ExecProcess(TEXT("git"), *CommitArgs, &ExitCode, &Revision, nullptr) && ExitCode == 0)
            O->SetStringField(TEXT("commit"), Revision.TrimStartAndEnd());
    }
    auto Settings = MakeShared<FJsonObject>();
    for (const TCHAR* Key : {TEXT("r.ScreenPercentage"), TEXT("r.AntiAliasingMethod"), TEXT("r.Lumen.DiffuseIndirect.Allow"),
                              TEXT("r.DynamicGlobalIlluminationMethod"), TEXT("r.HDR.EnableHDROutput"), TEXT("r.SSDO.Quality"), TEXT("r.SSDO"), TEXT("r.Lumen.HardwareRayTracing"), TEXT("r.MegaLights.Allow")})
        if (auto* CVar = IConsoleManager::Get().FindConsoleVariable(Key)) Settings->SetStringField(Key, CVar->GetString());
    O->SetObjectField(TEXT("cvars"), Settings);
    O->SetArrayField(TEXT("frames"), Frames);
    return FFileHelper::SaveStringToFile(Print(O), *(Dir / TEXT("render.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
void FSCRpcRender::EndCapture()
{
    if (bFixed)
    {
        FApp::SetFixedDeltaTime(OldDelta);
        FApp::SetUseFixedTimeStep(bOldFixed);
        bFixed = false;
    }

    for (auto* Component : {Sdr.Get(), Linear.Get()}) if (Component) Component->DestroyComponent();
    Sdr.Reset(); Linear.Reset(); MainLook.Reset(); ApplyCamera = {};
}
void FSCRpcRender::BeginEncode()
{
    EndCapture();
    const bool bProRes = Codec == TEXT("prores");
    const FString Format = Outputs.Contains(TEXT("png16")) ? TEXT("png16") : TEXT("png8");
    Encoded = Dir / (bProRes ? TEXT("render.mov") : TEXT("render.mp4"));
    FString Exe;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SCFfmpeg="), Exe)) Exe = TEXT("ffmpeg");
    const TCHAR* Options = bProRes ? TEXT("-c:v prores_ks -profile:v 4 -pix_fmt yuva444p10le") :
        TEXT("-vf pad=ceil(iw/2)*2:ceil(ih/2)*2 -c:v libx264 -crf 18 -pix_fmt yuv420p");
    const FString Arguments = FString::Printf(TEXT("-nostdin -n -hide_banner -loglevel error -framerate %.12g -start_number 0 -i \"%s\" %s \"%s\""),
        Fps, *(Dir / Format / TEXT("%06d.png")), Options, *Encoded);
    Encoder = FPlatformProcess::CreateProc(*Exe, *Arguments, false, true, true, nullptr, 0, *Dir, nullptr);
    if (!Encoder.IsValid()) Stop(TEXT("ffmpeg could not start"));
}
void FSCRpcRender::Stop(const FString& Why)
{
    if (!bActive) return;
    Failure = Why;
    if (Encoder.IsValid())
    {
        FPlatformProcess::TerminateProc(Encoder);
        FPlatformProcess::CloseProc(Encoder);
        Encoder.Reset();
    }
    EndCapture();
    bActive = false; Finished = FPlatformTime::Seconds();
    if (!WriteSidecar()) Failure += TEXT(" could not write render.json");
}
TSharedRef<FJsonObject> FSCRpcRender::Describe() const
{
    auto O = MakeShared<FJsonObject>();
    O->SetBoolField(TEXT("ok"), Failure.IsEmpty());
    O->SetBoolField(TEXT("running"), bActive);
    O->SetStringField(TEXT("path"), Dir);
    O->SetStringField(TEXT("sidecar"), Dir / TEXT("render.json"));
    O->SetNumberField(TEXT("frames"), Index); O->SetNumberField(TEXT("frame"), Index);
    O->SetNumberField(TEXT("of"), Total); O->SetNumberField(TEXT("fps"), Fps);
    O->SetNumberField(TEXT("warmup_frames"), Warmup);
    O->SetNumberField(TEXT("render_s"), Started ? (bActive ? FPlatformTime::Seconds() : Finished) - Started : 0);
    O->SetField(TEXT("outputs"), Strings(Outputs));
    auto Files = MakeShared<FJsonObject>();
    if (Index > 0) for (const auto& F : Outputs)
    {
        auto Pair = MakeShared<FJsonObject>();
        Pair->SetStringField(TEXT("first"), FileAt(Dir, F, 0));
        Pair->SetStringField(TEXT("last"), FileAt(Dir, F, Index - 1));
        Files->SetObjectField(F, Pair);
    }
    O->SetObjectField(TEXT("files"), Files);
    if (!Encoded.IsEmpty() && !bActive && Failure.IsEmpty()) O->SetStringField(TEXT("encoded"), Encoded);
    if (!Failure.IsEmpty()) O->SetStringField(TEXT("error"), Failure);
    return O;
}
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCRpcRenderRequestTest, "SCBridge.RpcRender.Request",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCRpcRenderRequestTest::RunTest(const FString&)
{
    FString Why;
    const auto Mixed = Parse(TEXT(R"({"move":{"from":{"at":[1,2,3],"lookAt":[3,2,1]},"to":{"at":[3,2,1],"rot":[0,0,0]},"seconds":2}})"));
    TestFalse(TEXT("mixed lookAt and rot"), ValidateAction(Mixed, Why));
    const auto Orbit = Parse(TEXT(R"({"orbit":{"center":"vehicle","radius":5,"seconds":1}})"));
    TestTrue(TEXT("vehicle orbit"), ValidateAction(Orbit, Why));
    Orbit->SetStringField(TEXT("console"), TEXT("stat fps"));
    TestFalse(TEXT("ambiguous action"), ValidateAction(Orbit, Why));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCRpcRenderScheduleTest, "SCBridge.RpcRender.Schedule",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSCRpcRenderScheduleTest::RunTest(const FString&)
{
    const auto B = MakeShared<FJsonObject>();
    B->SetNumberField(TEXT("fps"), 29.97);
    B->SetNumberField(TEXT("seconds"), 1.2);
    double Rate = 0;
    int32 Count = 0, W = 0, H = 0, Warm = 0;
    FString Why;
    auto Parse = [&] { return ValidateSchedule(B, FIntPoint(123, 234), Rate, Count, W, H, Warm, Why); };
    TestTrue(TEXT("seconds"), Parse());
    TestEqual(TEXT("rounded frame count"), Count, 36);
    TestEqual(TEXT("default viewport width"), W, 123);
    B->SetNumberField(TEXT("frames"), 10000);
    TestTrue(TEXT("frames override seconds"), Parse());
    TestEqual(TEXT("explicit count"), Count, 10000);
    for (int32 N = 0; N < Count; ++N)
        if (!FMath::IsNearlyEqual(N / Rate * Rate, double(N), 1.e-9))
        { AddError(TEXT("frame time drift")); break; }
    B->SetNumberField(TEXT("width"), -1);
    TestFalse(TEXT("invalid width"), Parse());
    B->SetNumberField(TEXT("width"), 1280);
    B->SetNumberField(TEXT("frames"), 2.5);
    TestFalse(TEXT("fractional frames"), Parse());
    B->SetNumberField(TEXT("frames"), 3);
    B->SetStringField(TEXT("steps"), TEXT("invalid"));
    TestFalse(TEXT("invalid steps type"), Parse());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCRpcRenderEncodeTest, "SCBridge.RpcRender.Encode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSCRpcRenderEncodeTest::RunTest(const FString&)
{
    const TArray<FFloat16Color> Pixels = {FFloat16Color(FLinearColor(0.003f, 0.01f, 0.5f, 1.0f)),
                                        FFloat16Color(FLinearColor(3.0f, 0.03f, 0.8f, 1.0f))};
    const FString Base = FPaths::ProjectSavedDir() / TEXT("Automation") / FGuid::NewGuid().ToString();
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Base), true);
    TArray64<uint8> Decoded8, Decoded16;
    auto& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    for (const FString Format : {FString(TEXT("png8")), FString(TEXT("png16")), FString(TEXT("exr"))})
    {
        const bool bEXR = Format == TEXT("exr");
        const FString File = Base + TEXT(".") + Format;
        if (!TestTrue(TEXT("write ") + Format, SaveFrame(File, Format, Pixels, 2, 1))) return false;
        TArray64<uint8> Bytes;
        const bool bRead = FFileHelper::LoadFileToArray(Bytes, *File);
        IFileManager::Get().Delete(*File);
        if (!TestTrue(TEXT("read ") + Format, bRead)) return false;
        const auto Wrapper = Module.CreateImageWrapper(bEXR ? EImageFormat::EXR : EImageFormat::PNG);
        if (!TestTrue(TEXT("valid header ") + Format, Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()))) return false;
        TArray64<uint8> Out;
        if (!TestTrue(TEXT("unpack ") + Format, Wrapper->GetRaw(bEXR ? ERGBFormat::RGBAF : ERGBFormat::RGBA,
                    Format == TEXT("png8") ? 8 : 16, Out))) return false;
        if (bEXR)
        {
            if (!TestEqual(TEXT("EXR bytes"), Out.Num(), int64(2 * sizeof(FFloat16Color)))) return false;
            TestTrue(TEXT("half-floats including highlights above one"), !FMemory::Memcmp(Out.GetData(), Pixels.GetData(), Out.Num()));
        }
        else if (Format == TEXT("png8")) Decoded8 = MoveTemp(Out);
        else Decoded16 = MoveTemp(Out);
    }
    if (!TestEqual(TEXT("8-bit RGBA"), Decoded8.Num(), int64(8)) || !TestEqual(TEXT("16-bit RGBA"), Decoded16.Num(), int64(16))) return false;
    const auto* Words = reinterpret_cast<const uint16*>(Decoded16.GetData());
    for (int32 I = 0; I < 8; ++I) TestEqual(TEXT("same high byte"), Decoded8[I], uint8(Words[I] >> 8));
    return true;
}
#endif
#endif
