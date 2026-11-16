// USCRpcSubsystem (WP-H1): the HTTP/JSON routes, deferred answers (waits, package loads, screenshots), activity and
// settling, and the NDJSON state log with size rotation. Everything runs on the game thread (the HTTP server ticks on
// the core ticker); only the log capture device is called from any thread.
#include "SCRpcSubsystem.h"

#if SC_WITH_RPC
#include "SCRpcJson.h"
#include "SCRpcVideo.h"
#include "SCViewerSubsystem.h"
#include "SCViewerPawn.h"
#include "SCVehicle.h"
#include "SCVehicleInstances.h"
#include "SCFirstPersonCharacter.h"
#include "SCEnvironment.h"
#include "Cargo/SCCargoSubsystem.h"
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

FString Str(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, const FString& Default = FString())
{
    FString S;
    if (!O || !O->HasField(Key)) return Default;
    if (O->TryGetStringField(Key, S)) return S;
    double N;
    return O->TryGetNumberField(Key, N) ? FString::Printf(TEXT("%g"), N) : Default;
}
double Num(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, double Default)
{
    double N = Default;
    if (O) O->TryGetNumberField(Key, N);
    return N;
}
bool Flag(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, bool Default)
{
    bool B = Default;
    if (O) O->TryGetBoolField(Key, B);
    return B;
}
bool Vec(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, FVector& Out, double Scale = 100)
{
    const TArray<TSharedPtr<FJsonValue>>* A;
    if (!O || !O->TryGetArrayField(Key, A) || A->Num() < 3) return false;
    Out = FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber()) * Scale;
    return true;
}
} // namespace

class FSCRpcServer : public TSharedFromThis<FSCRpcServer>
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
    bool bPanelsSet = false;
    FTransform LastCamera = FTransform::Identity;

    // ------------------------------------------------------------------------------------------------ world access
    UWorld* World() const { return Game.IsValid() ? Game->GetWorld() : nullptr; }
    USCViewerSubsystem* Viewer() const { return Game.IsValid() ? Game->GetSubsystem<USCViewerSubsystem>() : nullptr; }
    USCCargoSubsystem* Cargo() const { return USCCargoSubsystem::Get(World()); }
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
        if (const USCCargoSubsystem* C = Cargo()) Out.Append(C->ActivityReasons());
        for (const FPending& P : Pending)
            if (!P.bWait) Out.AddUnique(TEXT("rpc:") + P.Path);
        return Out;
    }
    /** Movement and rendering that keep a settled scene from being settled (empty: settled once idle). */
    TArray<FString> UnsettledReasons(double& OutSpeed) const
    {
        TArray<FString> Out;
        OutSpeed = 0;
        if (const USCCargoSubsystem* C = Cargo()) OutSpeed = C->MaxSpeedCmS();
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
        if (const USCCargoSubsystem* C = Cargo()) O->SetObjectField(TEXT("cargo"), C->StateJson());
        return O;
    }

    /** Anomalies in a snapshot: positions that are not finite, stored boxes off their slots, boxes without actors or
     *  simulated while stored, anything far below the world. New ones are logged as they appear, cleared ones too. */
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
        const TSharedPtr<FJsonObject>* CargoObj;
        if (S->TryGetObjectField(TEXT("cargo"), CargoObj))
        {
            const TSharedPtr<FJsonObject>& C = *CargoObj;
            if (C->TryGetArrayField(TEXT("boxes"), Arr))
                for (const TSharedPtr<FJsonValue>& V : *Arr)
                {
                    const TSharedPtr<FJsonObject> B = V->AsObject();
                    Check(TEXT("box"), B);
                    const FString Id = Str(B, TEXT("id")), State = Str(B, TEXT("state"));
                    if (!Flag(B, TEXT("actor"), true)) Found.Add(TEXT("box_no_actor:") + Id);
                    if (Num(B, TEXT("slot_err_cm"), 0) > 5) Found.Add(TEXT("box_off_slot:") + Id);
                    const bool bOnGrid = Num(B, TEXT("grid"), -1) >= 0;
                    if (bOnGrid && State == TEXT("Stored") && Flag(B, TEXT("sim"), false)) Found.Add(TEXT("box_simulated_on_grid:") + Id);
                    // Off every grid and in no job: let down by a failed move (nothing picks up a box that is not on a grid).
                    if (!bOnGrid && (State == TEXT("Stored") || State == TEXT("Reserved"))) Found.Add(TEXT("box_loose:") + Id);
                }
            if (C->TryGetArrayField(TEXT("drones"), Arr))
                for (const TSharedPtr<FJsonValue>& V : *Arr) Check(TEXT("drone"), V->AsObject());
        }
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
        // Driven runs show the full-width scene (as scripted routes do); /ui {panels: true} shows the panels again.
        if (!bPanelsSet)
            if (USCViewerSubsystem* V = Viewer()) { V->SetPanelsVisible(false); bPanelsSet = true; }
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
    int32 CurrentId = 0;
    double CurrentStart = 0;

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
        Bind(TEXT("/ship/spawn"), [this](auto& B, auto& Done) { SpawnShip(B, Done); });
        Bind(TEXT("/ship/despawn"), [this](auto& B, auto& Done) {
            USCViewerSubsystem* V = Viewer();
            const int32 Id = int32(Num(B, TEXT("id"), -1));
            if (!V || !V->DestroyVehicleInstance(Id)) return Respond(Done, Error(FString::Printf(TEXT("no extra vehicle instance %d (0 is the loaded vehicle: unload)"), Id)));
            Respond(Done, Ok());
        });
        Bind(TEXT("/cargo/package"), [this](auto& B, auto& Done) { CargoPackage(B, Done, false); });
        Bind(TEXT("/cargo/spawn"), [this](auto& B, auto& Done) { CargoPackage(B, Done, true); });
        Bind(TEXT("/cargo/remove"), [this](auto& B, auto& Done) { CargoRemove(B, Done); });
        Bind(TEXT("/cargo/dock"), [this](auto& B, auto& Done) {
            USCCargoSubsystem* C = Cargo();
            FString Err;
            const int32 Grid = ResolveGrid(B, TEXT("grid"), Err);
            if (Grid == INDEX_NONE) return Respond(Done, Error(Err));
            if (!C->AddDock(Grid, int32(Num(B, TEXT("drones"), 2)), Err)) return Respond(Done, Error(Err));
            Respond(Done, Ok());
        });
        Bind(TEXT("/cargo/transfer"), [this](auto& B, auto& Done) { Transfer(B, Done); });
        Bind(TEXT("/cargo/cancel"), [this](auto& B, auto& Done) {
            USCCargoSubsystem* C = Cargo();
            if (!C || !C->Cancel(int32(Num(B, TEXT("job"), -1)))) return Respond(Done, Error(TEXT("no active job with that id")));
            Respond(Done, Ok());
        });
        Bind(TEXT("/cargo/access"), [this](auto& B, auto& Done) {
            USCCargoSubsystem* C = Cargo();
            FString Err;
            const int32 Grid = ResolveGrid(B, TEXT("grid"), Err);
            if (Grid == INDEX_NONE) return Respond(Done, Error(Err));
            FText Why;
            if (!C->RequestGridAccess(Grid, Flag(B, TEXT("open"), true), Why)) return Respond(Done, Error(Why.ToString()));
            Respond(Done, Ok());
        });
        Bind(TEXT("/camera"), [this](auto& B, auto& Done) { Camera(B, Done); });
        Bind(TEXT("/screenshot"), [this](auto& B, auto& Done) { Screenshot(B, Done); });
        Bind(TEXT("/record"), [this](auto& B, auto& Done) { Record(B, Done); });
        Bind(TEXT("/ui"), [this](auto& B, auto& Done) {
            USCViewerSubsystem* V = Viewer();
            if (!V) return Respond(Done, Error(TEXT("no viewer")));
            if (B->HasField(TEXT("panels"))) V->SetPanelsVisible(Flag(B, TEXT("panels"), false));
            FObj O = Ok();
            O->SetBoolField(TEXT("panels"), V->ArePanelsVisible());
            Respond(Done, O);
        });
        Bind(TEXT("/quit"), [this](auto& B, auto& Done) {
            Respond(Done, Ok());
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
        const USCCargoSubsystem* C = Cargo();
        FObj O = C ? C->CatalogJson() : MakeShared<FJsonObject>();
        O->SetBoolField(TEXT("ok"), C != nullptr);
        if (const USCViewerSubsystem* V = Viewer()) O->SetField(TEXT("environments"), Strings(V->GetEnvironmentChoices()));
        return O;
    }

    int32 ResolveGrid(const TSharedPtr<FJsonObject>& B, const TCHAR* Key, FString& OutError) const
    {
        const USCCargoSubsystem* C = Cargo();
        const FString Sel = Str(B, Key);
        if (!C || Sel.IsEmpty()) { OutError = FString::Printf(TEXT("%s: a grid id or selector is needed"), Key); return INDEX_NONE; }
        const TArray<int32> Ids = C->ResolveGrids(Sel, FName(*Str(B, TEXT("owner"))));
        if (Ids.IsEmpty()) { OutError = FString::Printf(TEXT("no grid matches \"%s\""), *Sel); return INDEX_NONE; }
        return Ids[0];
    }

    void Wait(const TSharedPtr<FJsonObject>& B, const FHttpResultCallback& Done)
    {
        // until: idle | settled | job (job: <id>, finished). The condition must hold on a frame after the request (what
        // the previous request started has been seen); min_s: and not before this many seconds.
        const FString Until = Str(B, TEXT("until"), TEXT("settled"));
        const int32 Job = int32(Num(B, TEXT("job"), -1));
        const double MinS = Num(B, TEXT("min_s"), 0);
        const uint64 Frame = GFrameCounter;
        const double Started = Now();
        if (Until != TEXT("idle") && Until != TEXT("settled") && Until != TEXT("job"))
            return Respond(Done, Error(TEXT("until: idle, settled or job")));
        Defer(TEXT("/wait"), Num(B, TEXT("timeout_s"), 120), true, Done, [this, Until, Job, MinS, Frame, Started](bool bTimedOut) -> TSharedPtr<FJsonObject> {
            if (GFrameCounter < Frame + 2 || (Now() - Started < MinS && !bTimedOut)) return nullptr;
            bool bReached = false;
            FObj O = MakeShared<FJsonObject>();
            if (Until == TEXT("job"))
            {
                ESCCargoJobState State;
                FText Reason;
                const USCCargoSubsystem* C = Cargo();
                if (!C || !C->FindJobState(Job, State, Reason)) return Error(FString::Printf(TEXT("no job %d"), Job));
                bReached = State == ESCCargoJobState::Done || State == ESCCargoJobState::Failed || State == ESCCargoJobState::Cancelled;
                O->SetStringField(TEXT("job_state"), StaticEnum<ESCCargoJobState>()->GetNameStringByValue(int64(State)));
                if (!Reason.IsEmpty()) O->SetStringField(TEXT("job_reason"), Reason.ToString());
            }
            else bReached = Until == TEXT("idle") ? bIdle : bSettled;
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
            Id = V->SpawnVehicleInstance(Entity, FTransform(FRotator(0, Num(B, TEXT("yaw"), 0), 0), FVector(Num(B, TEXT("x_m"), 0) * 100, Num(B, TEXT("y_m"), 0) * 100, Z)));
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

    void CargoPackage(const TSharedPtr<FJsonObject>& B, const FHttpResultCallback& Done, bool bSpawn)
    {
        USCCargoSubsystem* C = Cargo();
        const FString Entity = Str(B, TEXT("entity"));
        if (!C || Entity.IsEmpty()) return Respond(Done, Error(TEXT("entity is needed")));
        FString Err;
        int32 Grid = INDEX_NONE;
        if (bSpawn && (Grid = ResolveGrid(B, TEXT("grid"), Err)) == INDEX_NONE) return Respond(Done, Error(Err));
        const int32 Count = FMath::Clamp(int32(Num(B, TEXT("count"), 1)), 1, 500);
        const FString Commodity = Str(B, TEXT("commodity"), TEXT("Test"));
        const FText Label = FText::FromString(Str(B, TEXT("label"), Commodity));
        const FName Owner(*Str(B, TEXT("owner")));
        if (C->RequestBoxPackage(Entity, Err) < 0) return Respond(Done, Error(Err));
        // The package exports and imports once (it may take a while the first time): answer when it is ready.
        Defer(bSpawn ? TEXT("/cargo/spawn") : TEXT("/cargo/package"), Num(B, TEXT("timeout_s"), 300), false, Done,
              [this, Entity, Grid, Count, Commodity, Label, Owner, bSpawn](bool) -> TSharedPtr<FJsonObject> {
                  USCCargoSubsystem* Cg = Cargo();
                  if (!Cg) return Error(TEXT("no cargo subsystem"));
                  FString E;
                  const int32 State = Cg->RequestBoxPackage(Entity, E);
                  if (State < 0) return Error(E);
                  if (State == 0) return nullptr;
                  FObj O = Ok();
                  if (!bSpawn) return O;
                  const TArray<int32> Ids = Cg->AddBoxes(Grid, Entity, Count, FName(*Commodity), Label, Owner, E);
                  O->SetField(TEXT("boxes"), Ints(Ids));
                  if (!E.IsEmpty()) O->SetStringField(TEXT("warning"), E);
                  O->SetBoolField(TEXT("ok"), !Ids.IsEmpty());
                  if (Ids.IsEmpty()) O->SetStringField(TEXT("error"), E);
                  return O;
              });
    }

    void CargoRemove(const TSharedPtr<FJsonObject>& B, const FHttpResultCallback& Done)
    {
        USCCargoSubsystem* C = Cargo();
        if (!C) return Respond(Done, Error(TEXT("no cargo subsystem")));
        const bool bForce = Flag(B, TEXT("force"), false);
        // box: one id; grid: every box on the grids that match; all: every box.
        TArray<int32> Targets;
        const TSharedRef<FJsonObject> S = C->StateJson();
        TArray<int32> Grids;
        if (B->HasField(TEXT("grid")))
        {
            FString Err;
            Grids = C->ResolveGrids(Str(B, TEXT("grid")), FName(*Str(B, TEXT("owner"))));
            if (Grids.IsEmpty()) return Respond(Done, Error(TEXT("no grid matches")));
        }
        if (B->HasField(TEXT("box"))) Targets.Add(int32(Num(B, TEXT("box"), -1)));
        else
            for (const TSharedPtr<FJsonValue>& V : S->GetArrayField(TEXT("boxes")))
            {
                const TSharedPtr<FJsonObject> Box = V->AsObject();
                if (Flag(B, TEXT("all"), false) || Grids.Contains(int32(Num(Box, TEXT("grid"), -1)))) Targets.Add(int32(Num(Box, TEXT("id"), -1)));
            }
        if (Targets.IsEmpty()) return Respond(Done, Error(TEXT("nothing to remove (box, grid or all)")));
        TArray<int32> Removed;
        TArray<FString> Errors;
        for (const int32 Id : Targets)
        {
            if (Removed.Contains(Id)) continue;
            FString Err;
            if (!C->RemoveBox(Id, bForce, Removed, Err)) Errors.Add(Err);
        }
        FObj O = Ok();
        O->SetField(TEXT("removed"), Ints(Removed));
        O->SetField(TEXT("refused"), Strings(Errors));
        O->SetBoolField(TEXT("ok"), !Removed.IsEmpty());
        if (Removed.IsEmpty()) O->SetStringField(TEXT("error"), Errors.IsEmpty() ? TEXT("nothing removed") : Errors[0]);
        Respond(Done, O);
    }

    void Transfer(const TSharedPtr<FJsonObject>& B, const FHttpResultCallback& Done)
    {
        USCCargoSubsystem* C = Cargo();
        if (!C) return Respond(Done, Error(TEXT("no cargo subsystem")));
        FSCCargoTransferRequest R;
        R.Requester = FName(*Str(B, TEXT("owner")));
        R.FromGrids = C->ResolveGrids(Str(B, TEXT("from")), R.Requester);
        R.ToGrids = C->ResolveGrids(Str(B, TEXT("to")), R.Requester);
        R.Commodity = C->ResolveCommodity(Str(B, TEXT("commodity")));
        R.AmountSCU = int32(Num(B, TEXT("scu"), 0));
        if (R.FromGrids.IsEmpty() || R.ToGrids.IsEmpty() || R.Commodity.IsNone() || R.AmountSCU <= 0)
            return Respond(Done, Error(TEXT("from, to (grid selectors), commodity (on some grid) and scu > 0 are needed")));
        const FSCCargoTransferPreview P = C->Preview(R);
        FObj O = Ok();
        O->SetNumberField(TEXT("planned_scu"), P.PlannedSCU);
        O->SetNumberField(TEXT("moves"), P.Moves.Num());
        TArray<FString> Refusals;
        for (const FSCCargoRefusal& F : P.Refusals) Refusals.Add(F.Reason.ToString());
        O->SetField(TEXT("refusals"), Strings(Refusals));
        if (Flag(B, TEXT("preview"), false)) return Respond(Done, O);
        FText Err;
        const int32 Job = C->Submit(R, Err);
        O->SetNumberField(TEXT("job"), Job);
        if (Job == INDEX_NONE)
        {
            O->SetBoolField(TEXT("ok"), false);
            O->SetStringField(TEXT("error"), Err.ToString());
        }
        Respond(Done, O);
    }

    void Camera(const TSharedPtr<FJsonObject>& B, const FHttpResultCallback& Done)
    {
        USCViewerSubsystem* V = Viewer();
        if (!V) return Respond(Done, Error(TEXT("no viewer")));
        const FString Mode = Str(B, TEXT("mode"));
        if (!Mode.IsEmpty())
        {
            const ESCCameraMode M = Mode == TEXT("first") ? ESCCameraMode::FirstPerson : Mode == TEXT("orbit") ? ESCCameraMode::Orbit : ESCCameraMode::Fly;
            if (!V->SetCameraMode(M)) return Respond(Done, Error(TEXT("camera mode refused")));
        }
        FVector At, LookAt;
        const bool bAt = Vec(B, TEXT("at"), At), bLook = Vec(B, TEXT("look_at"), LookAt);
        const FString Focus = Str(B, TEXT("focus"));
        if (bAt || !Focus.IsEmpty())
        {
            if (V->GetCameraMode() == ESCCameraMode::FirstPerson && !V->SetCameraMode(ESCCameraMode::Fly))
                return Respond(Done, Error(TEXT("cannot leave first person")));
            ASCViewerPawn* Pawn = V->GetViewerPawn();
            if (!Pawn) return Respond(Done, Error(TEXT("no viewer pawn")));
            if (!Focus.IsEmpty())
            {
                FBox Box(ForceInit);
                for (const FSCVehicleInstanceInfo& I : V->GetVehicleInstances())
                    if (I.Actor.IsValid() && (Focus == TEXT("all") || FString::FromInt(I.Id) == Focus)) Box += FSCEnvironment::SolidBounds(I.Actor.Get());
                if (!Box.IsValid) return Respond(Done, Error(TEXT("focus: a vehicle id or all")));
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
        Respond(Done, O);
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
