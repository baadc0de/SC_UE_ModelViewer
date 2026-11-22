// USCRpcSubsystem (WP-H1): the HTTP/JSON routes, deferred answers (waits, package loads, screenshots), activity and
// settling, and the NDJSON state log with size rotation. Everything runs on the game thread (the HTTP server ticks on
// the core ticker); only the log capture device is called from any thread.
#include "SCRpcSubsystem.h"

#if SC_WITH_RPC
#include "SCRpcJson.h"
#include "SCRpcVideo.h"
#include "SCRpcExtension.h"
#include "SCViewerShell.h"
#include "SCRpcRender.h"
#include "SCViewerSubsystem.h"
#include "SCViewerPawn.h"
#include "SCVehicle.h"
#include "SCVehicleInstances.h"
#include "SCFirstPersonCharacter.h"
#include "SCEnvironment.h"
#include "Camera/CameraComponent.h"
#include "AssetCompilingManager.h"
#include "ContentStreaming.h"
#include "ShaderCompiler.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCRpc, Log, All);

using namespace SCRpcJson;
using namespace SCRpcArgs;

namespace
{
/** NDJSON file rotated by size: Path, Path.1 ... Path.(Files-1) (".ndjson" kept last). */
class FSCStateLog
{
public:
    FString Path;
    int64 MaxBytes = 64ll << 20;
    int32 Files = 4;

    bool Open()
    {
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
        if (IFileManager::Get().FileExists(*Path)) Rotate(false); // the previous run's log becomes .1
        Ar.Reset(IFileManager::Get().CreateFileWriter(*Path, FILEWRITE_AllowRead));
        Size = 0;
        return Ar.IsValid();
    }
    void Write(const FString& Line)
    {
        if (!Ar) return;
        const FTCHARToUTF8 Utf8(*Line);
        if (Size > 0 && Size + Utf8.Length() + 1 > MaxBytes) Rotate(true);
        if (!Ar) return;
        Ar->Serialize(const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length());
        char NewLine = '\n';
        Ar->Serialize(&NewLine, 1);
        Ar->Flush(); // a crash keeps every record before it
        Size += Utf8.Length() + 1;
    }
    void Close() { Ar.Reset(); }

private:
    FString Numbered(int32 N) const
    {
        return N == 0 ? Path : FPaths::Combine(FPaths::GetPath(Path), FString::Printf(TEXT("%s.%d.%s"), *FPaths::GetBaseFilename(Path), N, *FPaths::GetExtension(Path)));
    }
    void Rotate(bool bReopen)
    {
        Ar.Reset();
        IFileManager::Get().Delete(*Numbered(Files - 1), false, true, true);
        for (int32 N = Files - 2; N >= 0; --N)
            if (IFileManager::Get().FileExists(*Numbered(N))) IFileManager::Get().Move(*Numbered(N + 1), *Numbered(N), true, true);
        if (bReopen)
        {
            Ar.Reset(IFileManager::Get().CreateFileWriter(*Path, FILEWRITE_AllowRead));
            Size = 0;
        }
    }
    TUniquePtr<FArchive> Ar;
    int64 Size = 0;
};

/** Log lines worth a state-log record (warnings, errors, job and scene changes), and every line while a console command
 *  of an RPC runs (its answer). */
class FSCRpcLogCapture final : public FOutputDevice
{
public:
    struct FLine { ELogVerbosity::Type Verbosity; FName Category; FString Text; };
    virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
    {
        const ELogVerbosity::Type Level = ELogVerbosity::Type(Verbosity & ELogVerbosity::VerbosityMask);
        FScopeLock Lock(&Mutex);
        if (bCapturing && Captured.Num() < 2000) Captured.Add(V);
        static const TCHAR* Keys[] = {TEXT("CARGO_JOB "), TEXT("CARGO_DRONE_OVERLAP"), TEXT("CARGO_GRIDS_LOST"), TEXT("CARGO_BOX_REMOVED"), TEXT("CARGO_SCENARIO"),
                                      TEXT("CARGO_TRANSFER"), TEXT("VEHICLE_"), TEXT("LOAD_")};
        bool bKeep = Level <= ELogVerbosity::Warning && Level != ELogVerbosity::NoLogging;
        for (const TCHAR* K : Keys) bKeep |= FCString::Strstr(V, K) != nullptr;
        if (bKeep && Pending.Num() < 1000) Pending.Add({Level, Category, V});
    }
    virtual bool CanBeUsedOnAnyThread() const override { return true; }
    virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
    TArray<FLine> Take()
    {
        FScopeLock Lock(&Mutex);
        return MoveTemp(Pending);
    }
    void BeginCapture() { FScopeLock Lock(&Mutex); bCapturing = true; Captured.Reset(); }
    TArray<FString> EndCapture() { FScopeLock Lock(&Mutex); bCapturing = false; return MoveTemp(Captured); }

private:
    FCriticalSection Mutex;
    TArray<FLine> Pending;
    bool bCapturing = false;
    TArray<FString> Captured;
};

const TCHAR* InstanceState(ESCVehicleInstanceState S)
{
    switch (S)
    {
    case ESCVehicleInstanceState::Queued: return TEXT("queued");
    case ESCVehicleInstanceState::Exporting: return TEXT("exporting");
    case ESCVehicleInstanceState::Importing: return TEXT("importing");
    case ESCVehicleInstanceState::Ready: return TEXT("ready");
    default: return TEXT("failed");
    }
}

} // namespace

class FSCRpcServer : public TSharedFromThis<FSCRpcServer>, public ISCRpcHost
{
public:
    explicit FSCRpcServer(UGameInstance* InGame) : Game(InGame) {}

    void Start(int32 InPort, bool bLog)
    {
        const TCHAR* Cmd = FCommandLine::Get();
        if (bLog)
        {
            Log.Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir() / TEXT("SCState.ndjson"));
            FParse::Value(Cmd, TEXT("SCStateLog="), Log.Path);
            int32 MaxMB = 64;
            FParse::Value(Cmd, TEXT("SCStateLogMaxMB="), MaxMB);
            Log.MaxBytes = int64(FMath::Max(1, MaxMB)) << 20;
            FParse::Value(Cmd, TEXT("SCStateLogFiles="), Log.Files);
            Log.Files = FMath::Clamp(Log.Files, 1, 32);
            FParse::Value(Cmd, TEXT("SCStateLogHz="), StateHz);
            StateHz = FMath::Clamp(StateHz, 0.1, 30.0);
            bLogOpen = Log.Open();
            UE_LOG(LogSCRpc, Display, TEXT("RPC_STATE_LOG path=%s ok=%d max_mb=%d files=%d hz=%.1f"), *Log.Path, bLogOpen, MaxMB, Log.Files, StateHz);
        }
        Capture = MakeUnique<FSCRpcLogCapture>();
        GLog->AddOutputDevice(Capture.Get());
        FObj Hello = Event(TEXT("start"));
        Hello->SetStringField(TEXT("cmd"), Cmd);
        Hello->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
        Hello->SetNumberField(TEXT("port"), InPort);
        Write(Hello);
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FSCRpcServer::Tick), 0.f);
        if (InPort > 0) StartHttp(InPort);
    }

    void Stop()
    {
        // A recording still running when the app closes is finished here, not lost (encoding a long one takes minutes).
        Renderer.Stop(TEXT("shutdown"));
        Video.Finish(30 * 60);
        if (TickHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        TickHandle.Reset();
        for (FPending& P : Pending) P.Done(Reply(EHttpServerResponseCodes::ServerError, Error(TEXT("shutting down"))));
        Pending.Reset();
        if (Router)
            for (const FHttpRouteHandle& H : Routes) Router->UnbindRoute(H);
        Routes.Reset();
        Router.Reset();
        if (Capture)
        {
            GLog->RemoveOutputDevice(Capture.Get());
            Capture.Reset();
        }
        Write(Event(TEXT("stop")));
        Log.Close();
    }

private:
    using FHandler = TFunction<void(const TSharedPtr<FJsonObject>& Body, const FHttpResultCallback& Done)>;
    struct FPending
    {
        int32 Id = 0;
        FString Path;
        double Started = 0, Timeout = 60;
        uint64 Frame = 0;
        bool bWait = false; // a wait does not keep the scene busy
        TFunction<TSharedPtr<FJsonObject>(bool bTimedOut)> Poll; // null while not done
        FHttpResultCallback Done;
    };

    TWeakObjectPtr<UGameInstance> Game;
    int32 Port = 0;
    TSharedPtr<IHttpRouter> Router;
    TArray<FHttpRouteHandle> Routes;
    FTSTicker::FDelegateHandle TickHandle;
    FSCStateLog Log;
    bool bLogOpen = false;
    double StateHz = 2;
    TUniquePtr<FSCRpcLogCapture> Capture;
    TArray<FPending> Pending;
    int32 NextRequest = 1;
    // Activity
    bool bIdle = false, bSettled = false;
    TArray<FString> Reasons, Unsettled;
    double IdleSince = -1, QuietSince = -1, NextState = 0, NextScan = 0;
    TSet<FString> Anomalies;
    int32 AnomalyCount = 0;
    TMap<FString, int32> LogKinds, LogTotals; // warnings / errors seen per kind, and in all
    FSCVideoRecorder Video;
    FSCRpcRender Renderer;
    FTransform LastCamera = FTransform::Identity;

    // ------------------------------------------------------------------------------------------------ world access
    UWorld* World() const override { return Game.IsValid() ? Game->GetWorld() : nullptr; }
    USCViewerSubsystem* Viewer() const { return Game.IsValid() ? Game->GetSubsystem<USCViewerSubsystem>() : nullptr; }
    double Now() const { return FPlatformTime::Seconds(); }

    // ------------------------------------------------------------------------------------------------ log records
    FObj Event(const TCHAR* Kind) const
    {
        FObj O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("ev"), Kind);
        O->SetField(TEXT("t"), Number(World() ? World()->GetTimeSeconds() : 0.0));
        O->SetStringField(TEXT("wall"), FDateTime::UtcNow().ToIso8601());
        O->SetNumberField(TEXT("frame"), double(GFrameCounter));
        return O;
    }
    void Write(const FObj& O)
    {
        if (bLogOpen) Log.Write(Print(O));
    }
    static FObj Error(const FString& Message)
    {
        FObj O = MakeShared<FJsonObject>();
        O->SetBoolField(TEXT("ok"), false);
        O->SetStringField(TEXT("error"), Message);
        return O;
    }
    static FObj Ok()
    {
        FObj O = MakeShared<FJsonObject>();
        O->SetBoolField(TEXT("ok"), true);
        return O;
    }
    static TUniquePtr<FHttpServerResponse> Reply(EHttpServerResponseCodes Code, const FObj& Body)
    {
        TUniquePtr<FHttpServerResponse> R = FHttpServerResponse::Create(Print(Body), TEXT("application/json"));
        R->Code = Code;
        return R;
    }
    static TUniquePtr<FHttpServerResponse> Reply(const FObj& Body)
    {
        bool bOK = true;
        Body->TryGetBoolField(TEXT("ok"), bOK);
        return Reply(bOK ? EHttpServerResponseCodes::Ok : EHttpServerResponseCodes::BadRequest, Body);
    }

    // ------------------------------------------------------------------------------------------------ state
    TArray<FString> ActivityReasons() const
    {
        TArray<FString> Out;
        if (const USCViewerSubsystem* V = Viewer())
        {
            if (V->IsBusy()) Out.Add(TEXT("loading"));
            if (V->IsSpawningVehicles()) Out.Add(TEXT("vehicle_spawning"));
            if (V->IsEnvironmentLoading()) Out.Add(TEXT("environment"));
            if (V->GetArchiveState() == ESCArchiveState::Opening) Out.Add(TEXT("archive"));
        }
        if (Renderer.IsBusy()) Out.Add(TEXT("rendering"));
        double Speed = 0;
        for (const TSharedRef<ISCRpcExtension>& X : SCRpcExtensions::All()) X->Activity(World(), Out, Speed);
        for (const FPending& P : Pending)
            if (!P.bWait) Out.AddUnique(TEXT("rpc:") + P.Path);
        return Out;
    }
    /** Movement and rendering that keep a settled scene from being settled (empty: settled once idle). */
    TArray<FString> UnsettledReasons(double& OutSpeed) const
    {
        TArray<FString> Out;
        OutSpeed = 0;
        TArray<FString> Busy;
        for (const TSharedRef<ISCRpcExtension>& X : SCRpcExtensions::All()) X->Activity(World(), Busy, OutSpeed);
        if (const USCViewerSubsystem* V = Viewer())
        {
            for (const ASCVehicle* Vehicle : V->GetVehicles())
                if (Vehicle) OutSpeed = FMath::Max(OutSpeed, Vehicle->GetVelocity().Size());
            if (const ASCFirstPersonCharacter* FP = V->GetFirstPerson()) OutSpeed = FMath::Max(OutSpeed, FP->GetVelocity().Size());
        }
        if (OutSpeed > 1.0) Out.Add(TEXT("moving"));
        if (IStreamingManager::Get().GetNumWantingResources() > 0) Out.Add(TEXT("streaming"));
        if (GShaderCompilingManager && GShaderCompilingManager->IsCompiling()) Out.Add(TEXT("shaders"));
        if (FAssetCompilingManager::Get().GetNumRemainingAssets() > 0) Out.Add(TEXT("assets"));
        return Out;
    }

    FObj Snapshot() const
    {
        FObj O = Event(TEXT("state"));
        O->SetBoolField(TEXT("idle"), bIdle);
        O->SetBoolField(TEXT("settled"), bSettled);
        O->SetField(TEXT("reasons"), Strings(Reasons));
        if (!Unsettled.IsEmpty()) O->SetField(TEXT("unsettled"), Strings(Unsettled));
        O->SetField(TEXT("frame_ms"), Number(FApp::GetDeltaTime() * 1000, 0.01));
        O->SetField(TEXT("anomalies"), Strings(Anomalies.Array()));
        O->SetNumberField(TEXT("warnings"), LogTotals.FindRef(TEXT("warnings")));
        O->SetNumberField(TEXT("errors"), LogTotals.FindRef(TEXT("errors")));
        if (Video.IsBusy()) O->SetStringField(TEXT("recording"), Video.Describe()->GetStringField(TEXT("phase")));
        if (const USCViewerSubsystem* V = Viewer())
        {
            FObj Load = MakeShared<FJsonObject>();
            Load->SetStringField(TEXT("entity"), V->GetLoadedEntity());
            Load->SetStringField(TEXT("pending"), V->GetPendingEntity());
            Load->SetStringField(TEXT("phase"), StaticEnum<ESCLoadPhase>()->GetNameStringByValue(int64(V->GetLoadPhase())));
            Load->SetStringField(TEXT("archive"), StaticEnum<ESCArchiveState>()->GetNameStringByValue(int64(V->GetArchiveState())));
            Load->SetStringField(TEXT("environment"), V->GetEnvironmentRequest());
            Load->SetBoolField(TEXT("space"), V->IsSpace()); // WP-K3: no environment, no gravity
            O->SetObjectField(TEXT("load"), Load);
            TArray<TSharedPtr<FJsonValue>> Vehicles;
            for (const FSCVehicleInstanceInfo& I : V->GetVehicleInstances())
            {
                FObj VO = MakeShared<FJsonObject>();
                VO->SetNumberField(TEXT("id"), I.Id);
                VO->SetStringField(TEXT("entity"), I.Entity);
                VO->SetStringField(TEXT("state"), InstanceState(I.State));
                if (!I.Error.IsEmpty()) VO->SetStringField(TEXT("error"), I.Error);
                if (const ASCVehicle* A = I.Actor.Get())
                {
                    VO->SetField(TEXT("pos"), Metres(A->GetActorLocation()));
                    VO->SetField(TEXT("rot"), Rotation(A->GetActorRotation()));
                }
                Vehicles.Add(MakeShared<FJsonValueObject>(VO));
            }
            O->SetArrayField(TEXT("vehicles"), Vehicles);
            FObj CameraObj = MakeShared<FJsonObject>();
            CameraObj->SetStringField(TEXT("mode"), StaticEnum<ESCCameraMode>()->GetNameStringByValue(int64(V->GetCameraMode())));
            if (const ASCFirstPersonCharacter* FP = V->GetFirstPerson())
            {
                CameraObj->SetField(TEXT("pos"), Metres(FP->Camera->GetComponentLocation()));
                CameraObj->SetField(TEXT("rot"), Rotation(FP->GetViewRotation()));
            }
            else if (const ASCViewerPawn* Pawn = V->GetViewerPawn())
            {
                CameraObj->SetField(TEXT("pos"), Metres(Pawn->Camera->GetComponentLocation()));
                CameraObj->SetField(TEXT("rot"), Rotation(Pawn->Camera->GetComponentRotation()));
            }
            O->SetObjectField(TEXT("camera"), CameraObj);
        }
        for (const TSharedRef<ISCRpcExtension>& X : SCRpcExtensions::All()) X->Snapshot(World(), *O);
        if (const TSharedPtr<FJsonValue> Lifts = LiftsJson(); Lifts->AsArray().Num() > 0) O->SetField(TEXT("lifts"), Lifts);
        return O;
    }
    /** WP-P5: one record per lift of the loaded vehicle: state, path position, speed, queue and door state at its stop. */
    TSharedPtr<FJsonValue> LiftsJson() const
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        const USCViewerSubsystem* V = Viewer();
        if (const ASCVehicle* Vehicle = V ? V->GetVehicle() : nullptr)
            for (const TSharedPtr<FSCLift>& L : Vehicle->Lifts)
            {
                FObj LO = MakeShared<FJsonObject>();
                LO->SetNumberField(TEXT("id"), L->Id);
                LO->SetStringField(TEXT("entity"), L->EntityId);
                LO->SetStringField(TEXT("state"), LexToString(L->State));
                LO->SetStringField(TEXT("phase"), LexToString(L->SpeedPhase));
                LO->SetNumberField(TEXT("at"), L->CurrentStop);
                LO->SetNumberField(TEXT("target"), L->TargetStop);
                LO->SetField(TEXT("s_m"), Number(L->S, 0.001));
                LO->SetField(TEXT("v"), Number(L->V, 0.001));
                LO->SetBoolField(TEXT("idle"), L->IsIdle());
                LO->SetStringField(TEXT("doors"), LexToString(L->DoorState(L->CurrentStop)));
                TArray<TSharedPtr<FJsonValue>> Queue;
                for (const FSCLiftRequest& R : L->Queue) Queue.Add(MakeShared<FJsonValueNumber>(R.Stop));
                LO->SetArrayField(TEXT("queue"), Queue);
                Out.Add(MakeShared<FJsonValueObject>(LO));
            }
        return MakeShared<FJsonValueArray>(Out);
    }

    /** Anomalies in a snapshot: positions that are not finite, anything far below the world, and what the application's
     *  extensions find (UnrealCitizen: boxes off their slots, orphaned drones). New ones are logged as they appear,
     *  cleared ones too. */
    void Scan(const FObj& S)
    {
        TSet<FString> Found;
        auto PosBad = [](const TSharedPtr<FJsonObject>& O, double& Z) {
            const TArray<TSharedPtr<FJsonValue>>* P;
            if (!O->TryGetArrayField(TEXT("pos"), P) || P->Num() < 3) return false;
            for (const TSharedPtr<FJsonValue>& V : *P)
                if (V->Type != EJson::Number) return true;
            Z = (*P)[2]->AsNumber();
            return false;
        };
        auto Check = [&](const TCHAR* Kind, const TSharedPtr<FJsonObject>& O) {
            double Z = 0;
            const FString Id = Str(O, TEXT("id"));
            if (PosBad(O, Z)) Found.Add(FString::Printf(TEXT("nan:%s:%s"), Kind, *Id));
            else if (Z < -1000) Found.Add(FString::Printf(TEXT("fell:%s:%s"), Kind, *Id));
        };
        const TArray<TSharedPtr<FJsonValue>>* Arr;
        if (S->TryGetArrayField(TEXT("vehicles"), Arr))
            for (const TSharedPtr<FJsonValue>& V : *Arr) Check(TEXT("vehicle"), V->AsObject());
        for (const TSharedRef<ISCRpcExtension>& X : SCRpcExtensions::All()) X->Scan(World(), *S, Found);
        for (const FString& K : Found)
            if (!Anomalies.Contains(K))
            {
                ++AnomalyCount;
                FObj E = Event(TEXT("anomaly"));
                E->SetStringField(TEXT("key"), K);
                Write(E);
                UE_LOG(LogSCRpc, Warning, TEXT("RPC_ANOMALY %s"), *K);
            }
        for (const FString& K : Anomalies)
            if (!Found.Contains(K))
            {
                FObj E = Event(TEXT("anomaly_cleared"));
                E->SetStringField(TEXT("key"), K);
                Write(E);
            }
        Anomalies = MoveTemp(Found);
    }

    // ------------------------------------------------------------------------------------------------ tick
    bool Tick(float)
    {
        const double T = Now();
        if (Capture)
            for (FSCRpcLogCapture::FLine& L : Capture->Take())
            {
                // Warnings repeat by the thousand on a load (one per mesh): the first three of a kind (category and
                // the message's start, digits aside), then at 10, 100, 1000 … with their count.
                int32 Count = 1;
                if (L.Verbosity <= ELogVerbosity::Warning)
                {
                    FString Kind = L.Category.ToString() + TEXT(":");
                    for (const TCHAR C : L.Text.Left(48)) Kind.AppendChar(FChar::IsDigit(C) ? TEXT('#') : C);
                    Count = ++LogKinds.FindOrAdd(Kind);
                    ++LogTotals.FindOrAdd(L.Verbosity == ELogVerbosity::Warning ? TEXT("warnings") : TEXT("errors"));
                    const bool bDecade = Count >= 10 && FString::FromInt(Count).StartsWith(TEXT("1")) &&
                                         FString::FromInt(Count).RightChop(1).Replace(TEXT("0"), TEXT("")).IsEmpty();
                    if (Count > 3 && !bDecade) continue;
                }
                FObj E = Event(TEXT("log"));
                E->SetStringField(TEXT("level"), ToString(L.Verbosity));
                E->SetStringField(TEXT("cat"), L.Category.ToString());
                E->SetStringField(TEXT("msg"), L.Text.Left(2000));
                if (Count > 1) E->SetNumberField(TEXT("count"), Count);
                Write(E);
            }
        // Idle: nothing loading, spawning, moving cargo or answering; settled: idle, nothing moving and nothing left to
        // stream or compile for SettleSeconds.
        TArray<FString> NowReasons = ActivityReasons();
        if (NowReasons != Reasons || (NowReasons.IsEmpty() != bIdle))
        {
            const bool bWasIdle = bIdle;
            Reasons = NowReasons;
            bIdle = Reasons.IsEmpty();
            if (bIdle) IdleSince = T;
            FObj E = Event(bIdle ? TEXT("idle") : bWasIdle ? TEXT("busy") : TEXT("activity"));
            E->SetField(TEXT("reasons"), Strings(Reasons));
            Write(E);
        }
        double Speed = 0;
        TArray<FString> NowUnsettled = UnsettledReasons(Speed);
        // The camera too (a focus glides there; a frame taken meanwhile is blurred): still to within 0.5 cm / 0.05°.
        if (const USCViewerSubsystem* V = Viewer())
        {
            FTransform Cam = FTransform::Identity;
            if (const ASCFirstPersonCharacter* FP = V->GetFirstPerson()) Cam = FTransform(FP->GetViewRotation(), FP->Camera->GetComponentLocation());
            else if (const ASCViewerPawn* Pawn = V->GetViewerPawn()) Cam = Pawn->Camera->GetComponentTransform();
            if (!Cam.GetLocation().Equals(LastCamera.GetLocation(), 0.5) || !Cam.GetRotation().Equals(LastCamera.GetRotation(), FMath::DegreesToRadians(0.05)))
                NowUnsettled.Add(TEXT("camera"));
            LastCamera = Cam;
        }
        const bool bQuiet = bIdle && NowUnsettled.IsEmpty();
        if (!bQuiet) QuietSince = -1;
        else if (QuietSince < 0) QuietSince = T;
        constexpr double SettleSeconds = 0.5;
        const bool bNowSettled = bQuiet && T - QuietSince >= SettleSeconds;
        if (bNowSettled != bSettled)
        {
            bSettled = bNowSettled;
            FObj E = Event(bSettled ? TEXT("settled") : TEXT("unsettled"));
            if (bSettled) E->SetField(TEXT("idle_for_s"), Number(T - IdleSince, 0.01));
            else E->SetField(TEXT("why"), Strings(bIdle ? NowUnsettled : Reasons));
            Write(E);
        }
        Unsettled = NowUnsettled;
        // State: at StateHz while anything happens, every 10 s once settled; anomalies looked for at StateHz.
        if (T >= NextScan)
        {
            NextScan = T + 1.0 / StateHz;
            const FObj S = Snapshot();
            Scan(S);
            if (T >= NextState)
            {
                S->SetField(TEXT("speed_ms"), Number(Speed / 100, 0.001));
                Write(S);
                NextState = T + (bSettled ? 10.0 : 1.0 / StateHz);
            }
        }
        if (Renderer.IsBusy())
        {
            const bool bOtherwiseIdle = Reasons.Num() == 1 && Reasons.Contains(TEXT("rendering"));
            Renderer.Tick(bOtherwiseIdle && Unsettled.IsEmpty(), Unsettled.Contains(TEXT("streaming")) || Unsettled.Contains(TEXT("assets")) || Unsettled.Contains(TEXT("shaders")));
        }
        Video.Tick();
        // Deferred answers.
        for (int32 I = 0; I < Pending.Num();)
        {
            FPending& P = Pending[I];
            const bool bTimedOut = T - P.Started > P.Timeout;
            TSharedPtr<FJsonObject> Body = P.Poll(bTimedOut);
            if (!Body && bTimedOut) Body = Error(FString::Printf(TEXT("timed out after %.0f s"), P.Timeout));
            if (!Body) { ++I; continue; }
            Finish(P.Id, P.Path, P.Started, P.Done, Body.ToSharedRef());
            Pending.RemoveAt(I);
        }
        return true;
    }

    void Finish(int32 Id, const FString& Path, double Started, const FHttpResultCallback& Done, const FObj& Body)
    {
        bool bOK = true;
        Body->TryGetBoolField(TEXT("ok"), bOK);
        FObj E = Event(TEXT("rpc_done"));
        E->SetNumberField(TEXT("id"), Id);
        E->SetStringField(TEXT("path"), Path);
        E->SetBoolField(TEXT("ok"), bOK);
        E->SetField(TEXT("ms"), Number((Now() - Started) * 1000, 0.1));
        FString Err;
        if (Body->TryGetStringField(TEXT("error"), Err)) E->SetStringField(TEXT("error"), Err);
        Write(E);
        Done(Reply(Body));
    }

    /** Answer later: Poll returns the body once done (or when timed out, else an error is made). */
    void Defer(const FString& Path, double Timeout, bool bWait, const FHttpResultCallback& Done, TFunction<TSharedPtr<FJsonObject>(bool)> Poll)
    {
        FPending& P = Pending.AddDefaulted_GetRef();
        P.Id = CurrentId;
        P.Path = Path;
        P.Started = CurrentStart;
        P.Timeout = Timeout;
        P.Frame = GFrameCounter;
        P.bWait = bWait;
        P.Poll = MoveTemp(Poll);
        P.Done = Done;
    }
    void Defer(const FString& Path, double TimeoutS, FPoll Poll) override { Defer(Path, TimeoutS, false, CurrentDone, MoveTemp(Poll)); }
    int32 CurrentId = 0;
    double CurrentStart = 0;
    FHttpResultCallback CurrentDone; // the request an application route is handling (ISCRpcHost::Defer)

    // ------------------------------------------------------------------------------------------------ http
    void StartHttp(int32 InPort)
    {
        Port = InPort;
        Router = FHttpServerModule::Get().GetHttpRouter(Port, /*bFailOnBindFailure=*/true);
        if (!Router)
        {
            UE_LOG(LogSCRpc, Error, TEXT("RPC_SERVER could not bind port %d"), Port);
            return;
        }
        auto Bind = [this](const TCHAR* Path, FHandler Handler) {
            TWeakPtr<FSCRpcServer> Weak = AsShared();
            const FString P = Path;
            Routes.Add(Router->BindRoute(FHttpPath(Path), EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_POST,
                FHttpRequestHandler::CreateLambda([Weak, P, Handler](const FHttpServerRequest& Request, const FHttpResultCallback& Done) {
                    const TSharedPtr<FSCRpcServer> Self = Weak.Pin();
                    if (!Self) return false;
                    Self->Dispatch(P, Request, Handler, Done);
                    return true;
                })));
        };
        Bind(TEXT("/health"), [this](auto&, auto& Done) { Respond(Done, Health()); });
        Bind(TEXT("/state"), [this](auto&, auto& Done) { FObj S = Snapshot(); S->SetBoolField(TEXT("ok"), true); Respond(Done, S); });
        Bind(TEXT("/catalog"), [this](auto&, auto& Done) { Respond(Done, CatalogBody()); });
        Bind(TEXT("/mark"), [this](auto& B, auto& Done) {
            FObj E = Event(TEXT("mark"));
            E->SetStringField(TEXT("note"), Str(B, TEXT("note")));
            Write(E);
            UE_LOG(LogSCRpc, Display, TEXT("RPC_MARK %s"), *Str(B, TEXT("note")));
            Respond(Done, Ok());
        });
        Bind(TEXT("/wait"), [this](auto& B, auto& Done) { Wait(B, Done); });
        Bind(TEXT("/console"), [this](auto& B, auto& Done) { Console(B, Done); });
        Bind(TEXT("/load"), [this](auto& B, auto& Done) { LoadShip(B, Done); });
        Bind(TEXT("/unload"), [this](auto&, auto& Done) {
            if (USCViewerSubsystem* V = Viewer()) V->Unload();
            Respond(Done, Ok());
        });
        Bind(TEXT("/environment"), [this](auto& B, auto& Done) {
            USCViewerSubsystem* V = Viewer();
            if (!V) return Respond(Done, Error(TEXT("no viewer")));
            V->SetEnvironment(Str(B, TEXT("folder")));
            Respond(Done, Ok());
        });
        // The application's routes (SCRpcExtension.h; UnrealCitizen: /cargo, /ship, /garage).
        for (const TSharedRef<ISCRpcExtension>& X : SCRpcExtensions::All())
        {
            TMap<FString, ISCRpcExtension::FRoute> Extra;
            X->Routes(Extra);
            for (auto& [Path, Route] : Extra)
                Bind(*Path, [this, Route](auto& B, auto& Done) {
                    CurrentDone = Done;
                    Route(B, *this, [this, Done](const TSharedPtr<FJsonObject>& Body) { Respond(Done, Body.ToSharedRef()); });
                });
        }
        Bind(TEXT("/ship/spawn"), [this](auto& B, auto& Done) { SpawnShip(B, Done); });
        Bind(TEXT("/ship/despawn"), [this](auto& B, auto& Done) {
            USCViewerSubsystem* V = Viewer();
            const int32 Id = int32(Num(B, TEXT("id"), -1));
            if (!V || !V->DestroyVehicleInstance(Id)) return Respond(Done, Error(FString::Printf(TEXT("no extra vehicle instance %d (0 is the loaded vehicle: unload)"), Id)));
            Respond(Done, Ok());
        });
        Bind(TEXT("/camera"), [this](auto& B, auto& Done) { Camera(B, Done); });
        Bind(TEXT("/screenshot"), [this](auto& B, auto& Done) { Screenshot(B, Done); });
        Bind(TEXT("/render"), [this](auto& B, auto& Done) {
            if (Str(B, TEXT("action")) == TEXT("status")) return Respond(Done, Renderer.Describe());
            FString Why;
            if (!Renderer.Start(World(), B, [this](const auto& Body) { return ApplyCamera(Body); }, Why)) return Respond(Done, Error(Why));
            Defer(TEXT("/render"), Num(B, TEXT("timeout_s"), 3600), true, Done, [this](bool bTimeout) -> TSharedPtr<FJsonObject> {
                if (bTimeout) Renderer.Stop(TEXT("render timed out"));
                return Renderer.IsBusy() ? nullptr : TSharedPtr<FJsonObject>(Renderer.Describe());
            });
        });
        Bind(TEXT("/record"), [this](auto& B, auto& Done) { Record(B, Done); });
        Bind(TEXT("/ui"), [this](auto& B, auto& Done) {
            USCViewerSubsystem* V = Viewer();
            if (!V) return Respond(Done, Error(TEXT("no viewer")));
            // The application's menu (SCViewerShell): {menu: true|false} opens or closes it.
            if (B->HasField(TEXT("menu"))) SCViewerShell::SetMenuOpen(V, Flag(B, TEXT("menu"), false));
            FObj O = Ok();
            O->SetBoolField(TEXT("menu"), SCViewerShell::IsMenuOpen(V));
            Respond(Done, O);
        });
        Bind(TEXT("/quit"), [this](auto& B, auto& Done) {
            Respond(Done, Ok());
            Renderer.Stop(TEXT("quit"));
            FPlatformMisc::RequestExitWithStatus(false, uint8(Num(B, TEXT("code"), 0)));
        });
        FHttpServerModule::Get().StartAllListeners();
        UE_LOG(LogSCRpc, Display, TEXT("RPC_SERVER listening http://127.0.0.1:%d routes=%d"), Port, Routes.Num());
    }

    void Dispatch(const FString& Path, const FHttpServerRequest& Request, const FHandler& Handler, const FHttpResultCallback& Done)
    {
        TSharedPtr<FJsonObject> Body;
        FString Text;
        if (!Request.Body.IsEmpty())
        {
            const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
            Text = FString(Conv.Length(), Conv.Get());
            Body = Parse(Text);
            if (!Body)
            {
                Done(Reply(EHttpServerResponseCodes::BadRequest, Error(TEXT("body is not a JSON object"))));
                return;
            }
        }
        if (!Body) Body = MakeShared<FJsonObject>();
        for (const auto& [Key, Value] : Request.QueryParams) Body->SetStringField(Key, Value); // GET ?key=value
        CurrentId = NextRequest++;
        CurrentStart = Now();
        FObj E = Event(TEXT("rpc"));
        E->SetNumberField(TEXT("id"), CurrentId);
        E->SetStringField(TEXT("path"), Path);
        if (Body->Values.Num()) E->SetObjectField(TEXT("body"), Body);
        Write(E);
        CurrentPath = Path;
        Handler(Body, Done);
    }
    FString CurrentPath;
    void Respond(const FHttpResultCallback& Done, const FObj& Body) { Finish(CurrentId, CurrentPath, CurrentStart, Done, Body); }

    // ------------------------------------------------------------------------------------------------ handlers
    FObj Health() const
    {
        FObj O = Ok();
        O->SetNumberField(TEXT("port"), Port);
        O->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
        O->SetStringField(TEXT("log"), bLogOpen ? Log.Path : FString());
        O->SetBoolField(TEXT("idle"), bIdle);
        O->SetBoolField(TEXT("settled"), bSettled);
        O->SetField(TEXT("reasons"), Strings(Reasons));
        O->SetNumberField(TEXT("anomalies_total"), AnomalyCount);
        O->SetNumberField(TEXT("warnings"), LogTotals.FindRef(TEXT("warnings")));
        O->SetNumberField(TEXT("errors"), LogTotals.FindRef(TEXT("errors")));
        O->SetNumberField(TEXT("frame"), double(GFrameCounter));
        return O;
    }

    FObj CatalogBody() const
    {
        FObj O = MakeShared<FJsonObject>();
        for (const TSharedRef<ISCRpcExtension>& X : SCRpcExtensions::All()) X->Catalog(World(), *O);
        O->SetBoolField(TEXT("ok"), true);
        if (const USCViewerSubsystem* V = Viewer()) O->SetField(TEXT("environments"), Strings(V->GetEnvironmentChoices()));
        return O;
    }

    void Wait(const TSharedPtr<FJsonObject>& B, const FHttpResultCallback& Done)
    {
        // until: idle | settled | lift_idle (lift: <id>, default every lift; WP-P5: no request queued, not moving, no door in
        // motion) | a condition of the application's (UnrealCitizen: job, with job: <id>, finished). The condition must hold on a frame after the request (what the previous
        // request started has been seen); min_s: and not before this many seconds.
        const FString Until = Str(B, TEXT("until"), TEXT("settled"));
        const int32 LiftId = int32(Num(B, TEXT("lift"), -1));
        const double MinS = Num(B, TEXT("min_s"), 0);
        const uint64 Frame = GFrameCounter;
        const double Started = Now();
        const bool bOwn = Until == TEXT("idle") || Until == TEXT("settled") || Until == TEXT("lift_idle");
        if (!bOwn)
        {
            bool bKnown = false, bReached = false;
            FJsonObject Probe;
            FString Err;
            for (const TSharedRef<ISCRpcExtension>& X : SCRpcExtensions::All()) bKnown = bKnown || X->Wait(World(), Until, B, bReached, Probe, Err);
            if (!bKnown) return Respond(Done, Error(FString::Printf(TEXT("until: idle, settled, lift_idle or an application's condition ('%s' is none)"), *Until)));
        }
        Defer(TEXT("/wait"), Num(B, TEXT("timeout_s"), 120), true, Done, [this, B, Until, LiftId, MinS, Frame, Started](bool bTimedOut) -> TSharedPtr<FJsonObject> {
            if (GFrameCounter < Frame + 2 || (Now() - Started < MinS && !bTimedOut)) return nullptr;
            bool bReached = false;
            FObj O = MakeShared<FJsonObject>();
            if (Until == TEXT("lift_idle"))
            {
                const USCViewerSubsystem* V = Viewer();
                const ASCVehicle* Vehicle = V ? V->GetVehicle() : nullptr;
                if (!Vehicle || Vehicle->Lifts.IsEmpty() || (LiftId >= 0 && !Vehicle->Lifts.IsValidIndex(LiftId)))
                    return Error(FString::Printf(TEXT("no lift %d"), LiftId));
                bReached = true;
                for (int32 I = 0; I < Vehicle->Lifts.Num(); ++I)
                    if (LiftId < 0 || I == LiftId) bReached &= Vehicle->Lifts[I]->IsIdle();
                O->SetField(TEXT("lifts"), LiftsJson());
            }
            else if (Until == TEXT("idle") || Until == TEXT("settled")) bReached = Until == TEXT("idle") ? bIdle : bSettled;
            else
            {
                FString Err;
                for (const TSharedRef<ISCRpcExtension>& X : SCRpcExtensions::All())
                    if (X->Wait(World(), Until, B, bReached, *O, Err)) break;
                if (!Err.IsEmpty()) return Error(Err);
            }
            if (!bReached && !bTimedOut) return nullptr;
            O->SetBoolField(TEXT("ok"), bReached);
            O->SetStringField(TEXT("until"), Until);
            O->SetField(TEXT("waited_s"), Number(Now() - Started, 0.01));
            O->SetBoolField(TEXT("idle"), bIdle);
            O->SetBoolField(TEXT("settled"), bSettled);
            O->SetField(TEXT("reasons"), Strings(Reasons));
            O->SetField(TEXT("unsettled"), Strings(Unsettled));
            O->SetField(TEXT("anomalies"), Strings(Anomalies.Array()));
            if (!bReached) O->SetStringField(TEXT("error"), TEXT("timed out"));
            return O;
        });
    }

    void Console(const TSharedPtr<FJsonObject>& B, const FHttpResultCallback& Done)
    {
        const FString Cmd = Str(B, TEXT("cmd"));
        UWorld* W = World();
        if (Cmd.IsEmpty() || !W) return Respond(Done, Error(TEXT("cmd is needed (and a world)")));
        Capture->BeginCapture();
        bool bHandled;
        if (APlayerController* PC = W->GetFirstPlayerController())
        {
            PC->ConsoleCommand(Cmd);
            bHandled = true;
        }
        else bHandled = GEngine->Exec(W, *Cmd);
        GLog->Flush();
        FObj O = Ok();
        O->SetBoolField(TEXT("handled"), bHandled);
        O->SetField(TEXT("lines"), Strings(Capture->EndCapture()));
        Respond(Done, O);
    }

    void LoadShip(const TSharedPtr<FJsonObject>& B, const FHttpResultCallback& Done)
    {
        USCViewerSubsystem* V = Viewer();
        const FString Entity = Str(B, TEXT("entity"));
        if (!V || Entity.IsEmpty()) return Respond(Done, Error(TEXT("entity is needed")));
        if (!V->CanLoad(Entity)) return Respond(Done, Error(TEXT("cannot load now (archive not ready)")));
        if (!V->LoadEntity(Entity, nullptr)) return Respond(Done, Error(TEXT("load refused (one is running?)")));
        Respond(Done, Ok());
    }

    void SpawnShip(const TSharedPtr<FJsonObject>& B, const FHttpResultCallback& Done)
    {
        USCViewerSubsystem* V = Viewer();
        if (!V || !V->GetVehicle()) return Respond(Done, Error(TEXT("no loaded vehicle (instances spawn beside it)")));
        const FString Entity = Str(B, TEXT("entity"), TEXT("loaded"));
        int32 Id;
        if (B->HasField(TEXT("x_m")) && B->HasField(TEXT("y_m")))
        {
            const double Z = B->HasField(TEXT("z_m")) ? Num(B, TEXT("z_m"), 0) * 100 : V->GetVehicle()->LowestVisibleZ();
            Id = V->SpawnVehicleInstance(Entity, FTransform(FRotator(0, Num(B, TEXT("yaw"), 0), 0), FVector(Num(B, TEXT("x_m"), 0) * 100, Num(B, TEXT("y_m"), 0) * 100, Z)), /*bRest=*/!V->IsSpace());
        }
        else Id = V->SpawnVehicleInstance(Entity);
        if (Id == INDEX_NONE) return Respond(Done, Error(TEXT("spawn refused")));
        FObj O = Ok();
        O->SetNumberField(TEXT("id"), Id);
        if (!Flag(B, TEXT("wait"), false)) return Respond(Done, O);
        // wait: answer when the instance is ready (or failed).
        Defer(TEXT("/ship/spawn"), Num(B, TEXT("timeout_s"), 600), false, Done, [this, Id, O](bool) -> TSharedPtr<FJsonObject> {
            const USCViewerSubsystem* Vw = Viewer();
            for (const FSCVehicleInstanceInfo& I : Vw ? Vw->GetVehicleInstances() : TArray<FSCVehicleInstanceInfo>())
                if (I.Id == Id)
                {
                    if (I.State == ESCVehicleInstanceState::Ready) return O;
                    if (I.State == ESCVehicleInstanceState::Failed) return Error(I.Error);
                    return nullptr;
                }
            return Error(TEXT("the instance is gone"));
        });
    }

    FObj ApplyCamera(const TSharedPtr<FJsonObject>& B)
    {
        USCViewerSubsystem* V = Viewer();
        if (!V) return Error(TEXT("no viewer"));
        const FString Mode = Str(B, TEXT("mode"));
        if (!Mode.IsEmpty())
        {
            const ESCCameraMode M = Mode == TEXT("first") ? ESCCameraMode::FirstPerson : Mode == TEXT("orbit") ? ESCCameraMode::Orbit : ESCCameraMode::Fly;
            if (!V->SetCameraMode(M)) return Error(TEXT("camera mode refused"));
        }
        FVector At, LookAt;
        const bool bAt = Vec(B, TEXT("at"), At), bLook = Vec(B, TEXT("look_at"), LookAt);
        const FString Focus = Str(B, TEXT("focus"));
        if (bAt || !Focus.IsEmpty())
        {
            if (V->GetCameraMode() == ESCCameraMode::FirstPerson && !V->SetCameraMode(ESCCameraMode::Fly))
                return Error(TEXT("cannot leave first person"));
            // A pose (at) is the fly camera's: the orbit camera keeps its own pivot and distance, and puts a view set on it
            // elsewhere (below the floor in the C2 hangar).
            if (bAt && Mode.IsEmpty() && V->GetCameraMode() == ESCCameraMode::Orbit && !V->SetCameraMode(ESCCameraMode::Fly))
                return Error(TEXT("cannot switch to the fly camera"));
            ASCViewerPawn* Pawn = V->GetViewerPawn();
            if (!Pawn) return Error(TEXT("no viewer pawn"));
            if (!Focus.IsEmpty())
            {
                FBox Box(ForceInit);
                for (const FSCVehicleInstanceInfo& I : V->GetVehicleInstances())
                    if (I.Actor.IsValid() && (Focus == TEXT("all") || FString::FromInt(I.Id) == Focus)) Box += FSCEnvironment::SolidBounds(I.Actor.Get());
                if (!Box.IsValid) return Error(TEXT("focus: a vehicle id or all"));
                Pawn->FocusBounds(Box);
            }
            else
            {
                FVector Rot(0, 0, 0);
                const FRotator R = bLook ? (LookAt - At).Rotation() : Vec(B, TEXT("rot"), Rot, 1) ? FRotator(Rot.X, Rot.Y, Rot.Z) : Pawn->Camera->GetComponentRotation();
                Pawn->SetView(At, R);
            }
        }
        FObj O = Ok();
        O->SetStringField(TEXT("mode"), StaticEnum<ESCCameraMode>()->GetNameStringByValue(int64(V->GetCameraMode())));
        return O;
    }

    void Camera(const TSharedPtr<FJsonObject>& B, const FHttpResultCallback& Done)
    {
        Respond(Done, ApplyCamera(B));
    }

    void Screenshot(const TSharedPtr<FJsonObject>& B, const FHttpResultCallback& Done)
    {
        FString Path = Str(B, TEXT("path"));
        if (Path.IsEmpty())
        {
            FString Dir = FPaths::ProjectSavedDir() / TEXT("Screenshots/rpc");
            FParse::Value(FCommandLine::Get(), TEXT("SCShotDir="), Dir);
            Path = FPaths::Combine(Dir, Str(B, TEXT("name"), FString::Printf(TEXT("shot-%d"), CurrentId)) + TEXT(".png"));
        }
        Path = FPaths::ConvertRelativePathToFull(Path);
        IFileManager::Get().Delete(*Path, false, true, true);
        FScreenshotRequest::RequestScreenshot(Path, Flag(B, TEXT("ui"), false), /*bAddFilenameSuffix=*/false);
        // Done when the file is written and its size holds still for a frame.
        TSharedRef<int64> Last = MakeShared<int64>(-1);
        Defer(TEXT("/screenshot"), Num(B, TEXT("timeout_s"), 30), false, Done, [Path, Last](bool) -> TSharedPtr<FJsonObject> {
            const int64 Size = IFileManager::Get().FileSize(*Path);
            if (Size <= 0 || Size != *Last) { *Last = Size; return nullptr; }
            FObj O = Ok();
            O->SetStringField(TEXT("path"), Path);
            O->SetNumberField(TEXT("bytes"), double(Size));
            return O;
        });
    }

    void Record(const TSharedPtr<FJsonObject>& B, const FHttpResultCallback& Done)
    {
        // action: start (name or path, fps, max_s, width, keep_frames) | stop (answers when the MP4 is written) | status.
        const FString Action = Str(B, TEXT("action"), TEXT("status"));
        if (Action == TEXT("start"))
        {
            FString Path = Str(B, TEXT("path"));
            if (Path.IsEmpty())
            {
                FString Dir = FPaths::ProjectSavedDir() / TEXT("Videos/rpc");
                FParse::Value(FCommandLine::Get(), TEXT("SCShotDir="), Dir);
                Path = FPaths::Combine(Dir, Str(B, TEXT("name"), FString::Printf(TEXT("video-%d"), CurrentId)) + TEXT(".mp4"));
            }
            FString Err;
            if (!Video.Start(Path, Num(B, TEXT("fps"), 30), Num(B, TEXT("max_s"), 300), int32(Num(B, TEXT("width"), 0)), Flag(B, TEXT("keep_frames"), false), Err))
                return Respond(Done, Error(Err));
            return Respond(Done, Video.Describe());
        }
        if (Action == TEXT("stop"))
        {
            if (!Video.IsBusy() && !Video.IsFinished()) return Respond(Done, Error(TEXT("not recording")));
            Video.Stop();
            Defer(TEXT("/record"), Num(B, TEXT("timeout_s"), 600), false, Done, [this](bool) -> TSharedPtr<FJsonObject> {
                return Video.IsFinished() ? TSharedPtr<FJsonObject>(Video.Describe()) : nullptr;
            });
            return;
        }
        Respond(Done, Video.Describe());
    }
};
#endif // SC_WITH_RPC

void USCRpcSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
#if SC_WITH_RPC
    if (IsRunningCommandlet()) return;
    const TCHAR* Cmd = FCommandLine::Get();
    int32 Port = 0;
    if (FParse::Value(Cmd, TEXT("SCRpc="), Port)) {}
    else if (FParse::Param(Cmd, TEXT("SCRpc"))) Port = 8770;
    FString Ignored;
    const bool bLog = Port > 0 || FParse::Param(Cmd, TEXT("SCStateLog")) || FParse::Value(Cmd, TEXT("SCStateLog="), Ignored);
    if (Port <= 0 && !bLog) return;
    Server = MakeShared<FSCRpcServer>(GetGameInstance());
    Server->Start(Port, bLog);
#endif
}

void USCRpcSubsystem::Deinitialize()
{
#if SC_WITH_RPC
    if (Server) Server->Stop();
#endif
    Server.Reset();
    Super::Deinitialize();
}
