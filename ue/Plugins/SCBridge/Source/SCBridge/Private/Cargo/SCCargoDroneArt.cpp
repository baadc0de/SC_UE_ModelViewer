// FSCCargoDroneArt (Config/Cargo/drone_art.json), the heading policy, the motion clip state machine, the dock
// footprint rule (WP-K4a) and the attitude dynamics of a drone's look (round 4), the grip block and the heading towards
// the box a drone holds (round 5; header: Cargo/SCCargoDroneArt.h).
#include "Cargo/SCCargoDroneArt.h"
#include "Dom/JsonObject.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCCargoDroneArt, Log, All);

namespace
{
const TCHAR* const DirectionKeys[6] = {TEXT("front"), TEXT("back"), TEXT("up"), TEXT("down"), TEXT("left"), TEXT("right")};
const TCHAR* const PhaseKeys[3] = {TEXT("start"), TEXT("loop"), TEXT("end")};

/** An object path from a package path (`/Game/A/B` -> `/Game/A/B.B`); object paths pass through. */
FString ObjectPath(const FString& Path)
{
    const FString P = Path.TrimStartAndEnd();
    if (P.IsEmpty() || P.Contains(TEXT("."))) return P;
    return P + TEXT(".") + FPaths::GetBaseFilename(P);
}
float Num(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, float Default)
{
    double V = Default;
    return O && O->TryGetNumberField(Key, V) ? float(V) : Default;
}
FString Str(const TSharedPtr<FJsonObject>& O, const TCHAR* Key)
{
    FString V;
    return O && O->TryGetStringField(Key, V) ? V : FString();
}
FName Name(const TSharedPtr<FJsonObject>& O, const TCHAR* Key)
{
    const FString V = Str(O, Key);
    return V.IsEmpty() ? NAME_None : FName(*V);
}
} // namespace

FString FSCCargoDroneArt::Clip(ESCDroneMotion Motion, ESCDroneClipPhase Phase, int32 IdleIndex) const
{
    switch (Motion)
    {
    case ESCDroneMotion::Idle: return Idle.IsEmpty() ? FString() : Idle[((IdleIndex % Idle.Num()) + Idle.Num()) % Idle.Num()];
    case ESCDroneMotion::TurnLeft: return TurnLeft;
    case ESCDroneMotion::TurnRight: return TurnRight;
    case ESCDroneMotion::Scanning: return Scanning;
    default: return Directional[int32(Motion) - int32(ESCDroneMotion::Front)][int32(Phase)];
    }
}

TArray<FString> FSCCargoDroneArt::AssetPaths() const
{
    TArray<FString> Out;
    auto Add = [&Out](const FString& P) { if (!P.IsEmpty()) Out.AddUnique(P); };
    Add(Mesh);
    for (const FString& P : Idle) Add(P);
    for (const auto& D : Directional)
        for (const FString& P : D) Add(P);
    Add(TurnLeft);
    Add(TurnRight);
    Add(Scanning);
    return Out;
}

FString FSCCargoDroneArt::ConfigFile()
{
    return FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("Cargo"), TEXT("drone_art.json"));
}

bool FSCCargoDroneArt::Parse(const FString& Json, FSCCargoDroneArt& Out, FString& Error)
{
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root)
    {
        Error = TEXT("invalid JSON");
        return false;
    }
    const TSharedPtr<FJsonObject>* Heading = nullptr;
    if (Root->TryGetObjectField(TEXT("heading"), Heading))
    {
        Out.TurnRateDegS = FMath::Max(1.f, Num(*Heading, TEXT("turn_rate_deg_s"), Out.TurnRateDegS));
        Out.HoverSpeedCmS = FMath::Max(0.f, Num(*Heading, TEXT("hover_speed_cm_s"), Out.HoverSpeedCmS));
        Out.TurnClipRateDegS = FMath::Max(0.f, Num(*Heading, TEXT("turn_clip_rate_deg_s"), Out.TurnClipRateDegS));
        Out.BlendS = FMath::Max(0.f, Num(*Heading, TEXT("blend_s"), Out.BlendS));
        Out.SmoothingS = FMath::Max(0.f, Num(*Heading, TEXT("smoothing_s"), Out.SmoothingS));
        Out.AxisDwellS = FMath::Max(0.f, Num(*Heading, TEXT("axis_dwell_s"), Out.AxisDwellS));
    }
    // How its look moves about its body (round 4): every drone, the stand-in too.
    const TSharedPtr<FJsonObject>* MotionBlock = nullptr;
    if (Root->TryGetObjectField(TEXT("motion"), MotionBlock))
    {
        FMotion& M = Out.Dynamics;
        M.FreqMinHz = FMath::Max(0.05f, Num(*MotionBlock, TEXT("freq_min_hz"), M.FreqMinHz));
        M.FreqMaxHz = FMath::Max(M.FreqMinHz, Num(*MotionBlock, TEXT("freq_max_hz"), M.FreqMaxHz));
        M.DampMin = FMath::Clamp(Num(*MotionBlock, TEXT("damp_min"), M.DampMin), 0.02f, 2.f);
        M.DampMax = FMath::Clamp(Num(*MotionBlock, TEXT("damp_max"), M.DampMax), M.DampMin, 2.f);
        M.TiltGain = FMath::Max(0.f, Num(*MotionBlock, TEXT("tilt_gain"), M.TiltGain));
        M.GainSpread = FMath::Clamp(Num(*MotionBlock, TEXT("gain_spread"), M.GainSpread), 0.f, 0.9f);
        M.TiltLead = FMath::Max(0.f, Num(*MotionBlock, TEXT("tilt_lead"), M.TiltLead));
        M.TiltLeadCarrying = FMath::Max(0.f, Num(*MotionBlock, TEXT("tilt_lead_carrying"), M.TiltLeadCarrying));
        M.MaxTiltDeg = FMath::Clamp(Num(*MotionBlock, TEXT("max_tilt_deg"), M.MaxTiltDeg), 0.f, 30.f);
        M.KickRateHz = FMath::Max(0.f, Num(*MotionBlock, TEXT("kick_rate_hz"), M.KickRateHz));
        M.KickDegS = FMath::Max(0.f, Num(*MotionBlock, TEXT("kick_deg_s"), M.KickDegS));
        M.HuntCm = FMath::Max(0.f, Num(*MotionBlock, TEXT("hunt_cm"), M.HuntCm));
        M.HuntHz = FMath::Max(0.f, Num(*MotionBlock, TEXT("hunt_hz"), M.HuntHz));
        M.DriftCm = FMath::Max(0.f, Num(*MotionBlock, TEXT("drift_cm"), M.DriftCm));
        M.YawJitterDeg = FMath::Max(0.f, Num(*MotionBlock, TEXT("yaw_jitter_deg"), M.YawJitterDeg));
        M.OnsetMinS = FMath::Max(0.f, Num(*MotionBlock, TEXT("onset_min_s"), M.OnsetMinS));
        M.OnsetMaxS = FMath::Clamp(Num(*MotionBlock, TEXT("onset_max_s"), M.OnsetMaxS), M.OnsetMinS, 1.f);
        M.OnsetJitterS = FMath::Clamp(Num(*MotionBlock, TEXT("onset_jitter_s"), M.OnsetJitterS), 0.f, 1.f);
        M.EventDeg = FMath::Max(0.01f, Num(*MotionBlock, TEXT("event_deg"), M.EventDeg));
        M.EventMinS = FMath::Max(0.f, Num(*MotionBlock, TEXT("event_min_s"), M.EventMinS));
        M.EventKickScale = FMath::Max(0.f, Num(*MotionBlock, TEXT("event_kick_scale"), M.EventKickScale));
        M.KickLogSigma = FMath::Clamp(Num(*MotionBlock, TEXT("kick_log_sigma"), M.KickLogSigma), 0.f, 2.f);
        M.ScatterDeg = FMath::Clamp(Num(*MotionBlock, TEXT("scatter_deg"), M.ScatterDeg), 0.f, 180.f);
        M.OppositeShare = FMath::Clamp(Num(*MotionBlock, TEXT("opposite_share"), M.OppositeShare), 0.f, 1.f);
        M.OffAxisShare = FMath::Clamp(Num(*MotionBlock, TEXT("off_axis_share"), M.OffAxisShare), 0.f, 1.f - M.OppositeShare);
        M.LoadTransfer = FMath::Clamp(Num(*MotionBlock, TEXT("load_transfer"), M.LoadTransfer), 0.f, 1.f);
    }
    // Where a carrying drone holds from (round 5): every drone.
    const TSharedPtr<FJsonObject>* GripBlock = nullptr;
    if (Root->TryGetObjectField(TEXT("grip"), GripBlock))
    {
        FSCCargoGripPick::FCfg& G = Out.Grip;
        G.EndWeight = FMath::Max(0.f, Num(*GripBlock, TEXT("end_weight"), G.EndWeight));
        G.SideWeight = FMath::Max(0.f, Num(*GripBlock, TEXT("side_weight"), G.SideWeight));
        G.OverWeight = FMath::Max(0.f, Num(*GripBlock, TEXT("over_weight"), G.OverWeight));
        G.SameFaceWeight = FMath::Clamp(Num(*GripBlock, TEXT("same_face_weight"), G.SameFaceWeight), 0.f, 1.f);
        G.Spread = FMath::Clamp(Num(*GripBlock, TEXT("spread"), G.Spread), 0.f, 1.f);
        G.ExtraReach = FMath::Clamp(Num(*GripBlock, TEXT("extra_reach"), G.ExtraReach), 0.f, 3.f);
        G.ExtraHeightCm = FMath::Max(0.f, Num(*GripBlock, TEXT("extra_height_cm"), G.ExtraHeightCm));
        G.IntervalMinS = FMath::Max(0.5f, Num(*GripBlock, TEXT("interval_min_s"), G.IntervalMinS));
        G.IntervalMaxS = FMath::Max(G.IntervalMinS, Num(*GripBlock, TEXT("interval_max_s"), G.IntervalMaxS));
        G.LookaheadS = FMath::Max(0.5f, Num(*GripBlock, TEXT("lookahead_s"), G.LookaheadS));
        G.MoveSpeedScale = FMath::Clamp(Num(*GripBlock, TEXT("move_speed_scale"), G.MoveSpeedScale), 0.05f, 1.f);
        G.MaxRepickMoveFrac = FMath::Max(0.f, Num(*GripBlock, TEXT("max_repick_move_frac"), G.MaxRepickMoveFrac));
        G.MaxRepickMoveMinCm = FMath::Max(0.f, Num(*GripBlock, TEXT("max_repick_move_min_cm"), G.MaxRepickMoveMinCm));
        G.MaxRepickMoveMaxCm = FMath::Max(G.MaxRepickMoveMinCm, Num(*GripBlock, TEXT("max_repick_move_max_cm"), G.MaxRepickMoveMaxCm));
    }
    const TSharedPtr<FJsonObject>* Art = nullptr;
    if (!Root->TryGetObjectField(TEXT("art"), Art)) return true; // the stand-in, with the heading policy
    const TSharedPtr<FJsonObject>& A = *Art;
    Out.Mesh = ObjectPath(Str(A, TEXT("mesh")));
    if (Out.Mesh.IsEmpty())
    {
        Error = TEXT("art.mesh is required");
        return false;
    }
    Out.Scale = FMath::Max(1e-3f, Num(A, TEXT("scale"), 1.f));
    Out.YawOffsetDeg = Num(A, TEXT("yaw_offset_deg"), 0.f);
    Out.ActorLeanScale = FMath::Max(0.f, Num(A, TEXT("actor_lean_scale"), 1.f));
    Out.BodyBobCm = FMath::Max(0.f, Num(A, TEXT("body_bob_cm"), 0.f));
    Out.ClipTiltScale = FMath::Clamp(Num(A, TEXT("clip_tilt_scale"), 1.f), 0.f, 1.f);
    Out.CentreBone = Name(A, TEXT("centre_bone"));
    Out.BeamBone = Name(A, TEXT("beam_bone"));
    Out.LightBone = Name(A, TEXT("light_bone"));
    Out.YawLockBone = Name(A, TEXT("yaw_lock_bone"));
    const TArray<TSharedPtr<FJsonValue>>* Axis = nullptr;
    if (A->TryGetArrayField(TEXT("beam_axis"), Axis) && Axis->Num() == 3)
        Out.BeamAxis = FVector((*Axis)[0]->AsNumber(), (*Axis)[1]->AsNumber(), (*Axis)[2]->AsNumber()).GetSafeNormal(1e-6, FVector(0, 0, -1));
    const TArray<TSharedPtr<FJsonValue>>* Hidden = nullptr;
    if (A->TryGetArrayField(TEXT("hidden_slots"), Hidden))
        for (const TSharedPtr<FJsonValue>& V : *Hidden) Out.HiddenSlots.Add(FName(*V->AsString()));
    Out.EmissiveScale = FMath::Max(0.f, Num(A, TEXT("emissive_scale"), 1.f));
    const TSharedPtr<FJsonObject>* Emissive = nullptr;
    if (A->TryGetObjectField(TEXT("emissive_params"), Emissive))
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Slot : (*Emissive)->Values)
            if (const TArray<TSharedPtr<FJsonValue>>* Params = nullptr; Slot.Value->TryGetArray(Params))
                for (const TSharedPtr<FJsonValue>& P : *Params) Out.EmissiveParams.FindOrAdd(FName(*Slot.Key)).Add(FName(*P->AsString()));
    const TSharedPtr<FJsonObject>* Clips = nullptr;
    if (A->TryGetObjectField(TEXT("clips"), Clips))
    {
        const TSharedPtr<FJsonObject>& C = *Clips;
        const TArray<TSharedPtr<FJsonValue>>* Idle = nullptr;
        if (C->TryGetArrayField(TEXT("idle"), Idle))
            for (const TSharedPtr<FJsonValue>& V : *Idle) Out.Idle.Add(ObjectPath(V->AsString()));
        for (int32 D = 0; D < 6; ++D)
        {
            const TSharedPtr<FJsonObject>* Dir = nullptr;
            if (C->TryGetObjectField(DirectionKeys[D], Dir))
                for (int32 P = 0; P < 3; ++P) Out.Directional[D][P] = ObjectPath(Str(*Dir, PhaseKeys[P]));
        }
        Out.TurnLeft = ObjectPath(Str(C, TEXT("turn_left")));
        Out.TurnRight = ObjectPath(Str(C, TEXT("turn_right")));
        Out.Scanning = ObjectPath(Str(C, TEXT("scanning")));
    }
    return true;
}

const FSCCargoDroneArt& FSCCargoDroneArt::Get()
{
    static const FSCCargoDroneArt Art = [] {
        FSCCargoDroneArt A;
        A.Source = TEXT("defaults");
        // -SCDroneArt=<file> reads another description, -SCDroneArt=none none (the stand-in; for comparisons).
        FString File = ConfigFile();
        FParse::Value(FCommandLine::Get(), TEXT("SCDroneArt="), File);
        FString Text, Error;
        FSCCargoDroneArt Parsed;
        if (File.Equals(TEXT("none"), ESearchCase::IgnoreCase))
        {
            UE_LOG(LogSCCargoDroneArt, Display, TEXT("CARGO_DRONE_ART none (-SCDroneArt=none): the stand-in drone, default heading"));
        }
        else if (!FFileHelper::LoadFileToString(Text, *File))
        {
            UE_LOG(LogSCCargoDroneArt, Display, TEXT("CARGO_DRONE_ART none (%s missing): the stand-in drone, default heading"), *File);
        }
        else if (!Parse(Text, Parsed, Error))
        {
            UE_LOG(LogSCCargoDroneArt, Warning, TEXT("CARGO_DRONE_ART %s: %s: the stand-in drone, default heading"), *File, *Error);
        }
        else
        {
            A = Parsed;
            A.Source = File;
            UE_LOG(LogSCCargoDroneArt, Display, TEXT("CARGO_DRONE_ART %s mesh=%s turn_deg_s=%.0f hover_cm_s=%.0f clips=%d"), *File, A.HasArt() ? *A.Mesh : TEXT("(stand-in)"),
                   A.TurnRateDegS, A.HoverSpeedCmS, A.AssetPaths().Num() - (A.HasArt() ? 1 : 0));
        }
        return A;
    }();
    return Art;
}

// ---------------------------------------------------------------------------------------------------- heading

float FSCCargoHeading::Target(float Yaw, const FVector& Travel, float MinSpeedCmS, const TOptional<float>& BayYaw, const TOptional<float>& HoldYaw)
{
    if (BayYaw) return FRotator::NormalizeAxis(*BayYaw);
    if (HoldYaw) return FRotator::NormalizeAxis(*HoldYaw);
    const FVector2D Plan(Travel.X, Travel.Y);
    if (Plan.Size() >= FMath::Max(MinSpeedCmS, 1e-3f)) return float(FMath::RadiansToDegrees(FMath::Atan2(Plan.Y, Plan.X)));
    return FRotator::NormalizeAxis(Yaw);
}

float FSCCargoHeading::Turn(float Yaw, float Target, float RateDegS, float Dt)
{
    return FRotator::NormalizeAxis(FMath::FixedTurn(Yaw, Target, FMath::Max(0.f, RateDegS * Dt)));
}

// ---------------------------------------------------------------------------------------------------- motion

ESCDroneMotion FSCCargoDroneMotion::Want(const FInput& In, ESCDroneMotion Current, const FSCCargoDroneArt& Art)
{
    if (In.bScanning) return ESCDroneMotion::Scanning;
    const FVector V = In.LocalVelocity;
    const bool bMoving = IsDirectional(Current);
    const double Threshold = Art.HoverSpeedCmS * (bMoving ? 0.75 : 1.25);
    if (V.Size() < FMath::Max(Threshold, 1e-3))
    {
        if (FMath::Abs(In.YawRateDegS) >= FMath::Max(Art.TurnClipRateDegS, 1e-3f) && Art.TurnClipRateDegS > 0)
            return In.YawRateDegS > 0 ? ESCDroneMotion::TurnRight : ESCDroneMotion::TurnLeft;
        return ESCDroneMotion::Idle;
    }
    const FVector A = V.GetAbs();
    int32 Axis = A.X >= A.Y && A.X >= A.Z ? 0 : (A.Y >= A.Z ? 1 : 2);
    // Hysteresis: the current axis holds until another is 25 % larger.
    if (bMoving)
    {
        const int32 Held = Current == ESCDroneMotion::Front || Current == ESCDroneMotion::Back ? 0 : (Current == ESCDroneMotion::Left || Current == ESCDroneMotion::Right ? 1 : 2);
        if (A[Axis] < A[Held] * 1.25) Axis = Held;
    }
    switch (Axis)
    {
    case 0: return V.X >= 0 ? ESCDroneMotion::Front : ESCDroneMotion::Back;
    case 1: return V.Y >= 0 ? ESCDroneMotion::Right : ESCDroneMotion::Left;
    default: return V.Z >= 0 ? ESCDroneMotion::Up : ESCDroneMotion::Down;
    }
}

bool FSCCargoDroneMotion::Step(const FInput& In, float Dt, const FSCCargoDroneArt& Art, TFunctionRef<float(ESCDroneMotion, ESCDroneClipPhase)> Length)
{
    Time += FMath::Max(Dt, 0.f);
    const ESCDroneMotion W = Want(In, Motion, Art);
    const float Len = Length(Motion, Phase);
    const bool bDone = Time >= Len;
    auto Enter = [this](ESCDroneMotion M, ESCDroneClipPhase P) {
        Motion = M;
        Phase = P;
        Time = 0;
        return true;
    };
    auto EnterWanted = [&](ESCDroneMotion M) {
        if (IsDirectional(M)) return Enter(M, Length(M, ESCDroneClipPhase::Start) > 0 ? ESCDroneClipPhase::Start : ESCDroneClipPhase::Loop);
        return Enter(M, ESCDroneClipPhase::Loop);
    };
    if (IsDirectional(Motion))
    {
        if (W == Motion)
        {
            if (Phase == ESCDroneClipPhase::Start && bDone) return Enter(Motion, ESCDroneClipPhase::Loop);
            if (Phase == ESCDroneClipPhase::End) return EnterWanted(Motion); // came back while leaving it
            return false;
        }
        if (IsDirectional(W)) return Time >= Art.AxisDwellS ? EnterWanted(W) : false;
        if (Phase != ESCDroneClipPhase::End && Length(Motion, ESCDroneClipPhase::End) > 0) return Enter(Motion, ESCDroneClipPhase::End);
        if (Phase == ESCDroneClipPhase::End && !bDone) return false;
        if (W == ESCDroneMotion::Idle) ++IdleIndex;
        return EnterWanted(W);
    }
    // Idle, turning, scanning: a short dwell keeps a clip from flickering (scanning starts at once).
    if (W != Motion)
    {
        if (W != ESCDroneMotion::Scanning && Time < FMath::Min(Art.BlendS, 0.3f) + 0.1f && Motion != ESCDroneMotion::Idle) return false;
        if (W == ESCDroneMotion::Idle) ++IdleIndex;
        return EnterWanted(W);
    }
    if (Motion == ESCDroneMotion::Idle && Len > 0 && bDone)
    {
        ++IdleIndex; // the next idle clip
        return Enter(ESCDroneMotion::Idle, ESCDroneClipPhase::Loop);
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------- attitude

namespace
{
// The g a drone's look tilts against (cm / s^2): a constant of the look, the same in vacuum (a viewer choice).
constexpr double LookG = 981.0;
// Sub-steps of the attitude's springs (s): stable and smooth at any frame rate.
constexpr double AttitudeStepS = 1.0 / 240.0;
// Clamps V to a disc of Radius; the rate's outward part is taken off at the rim.
template <typename T>
void Rim(T& V, T& Rate, double Radius)
{
    const double Size = V.Size();
    if (Size <= Radius || Size < 1e-9) return;
    const T N = V / Size;
    V = N * Radius;
    const double Out = (Rate | N);
    if (Out > 0) Rate -= N * Out;
}
} // namespace

double FSCCargoAttitude::OnsetFor(int32 Index, const FSCCargoDroneArt::FMotion& Cfg)
{
    const double Golden = 0.6180339887498949, U = FMath::Frac(FMath::Max(Index, 0) * Golden + 0.5);
    return FMath::Lerp(double(Cfg.OnsetMinS), double(FMath::Max(Cfg.OnsetMaxS, Cfg.OnsetMinS)), U);
}

FVector2D FSCCargoAttitude::DrawKick(FRandomStream& Rng, const FVector2D& Expected, double MedianDegS, const FSCCargoDroneArt::FMotion& Cfg)
{
    // Two normals (Box-Muller) from the drone's own stream.
    auto Normal = [&Rng]() { return FMath::Sqrt(-2.0 * FMath::Loge(FMath::Max(double(Rng.FRand()), 1e-9))) * FMath::Cos(UE_DOUBLE_TWO_PI * Rng.FRand()); };
    double Angle = Expected.SizeSquared() > 1e-12 ? FMath::Atan2(Expected.Y, Expected.X) : Rng.FRand() * UE_DOUBLE_TWO_PI;
    const double Pick = Rng.FRand();
    if (Pick < Cfg.OppositeShare) Angle += UE_DOUBLE_PI;
    else if (Pick < Cfg.OppositeShare + Cfg.OffAxisShare) Angle += Rng.FRand() < 0.5 ? UE_DOUBLE_HALF_PI : -UE_DOUBLE_HALF_PI;
    Angle += FMath::DegreesToRadians(Cfg.ScatterDeg) * Normal();
    const double Size = FMath::Min(MedianDegS * FMath::Exp(Cfg.KickLogSigma * Normal()), 3.0 * MedianDegS);
    return FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Size;
}

void FSCCargoAttitude::Init(uint32 Seed, const FSCCargoDroneArt::FMotion& Cfg, int32 Index)
{
    Rng.Initialize(int32(Seed * 2654435761u ^ 0x7a71u));
    Omega = UE_DOUBLE_TWO_PI * FMath::Lerp(double(Cfg.FreqMinHz), double(Cfg.FreqMaxHz), double(Rng.FRand()));
    Zeta = FMath::Lerp(double(Cfg.DampMin), double(Cfg.DampMax), double(Rng.FRand()));
    HuntHz = Cfg.HuntHz * FMath::Lerp(0.7, 1.3, double(Rng.FRand()));
    Gain = 1.0 + Cfg.GainSpread * (2.0 * Rng.FRand() - 1.0);
    HuntPhase[0] = Rng.FRand() * UE_DOUBLE_TWO_PI;
    HuntPhase[1] = Rng.FRand() * UE_DOUBLE_TWO_PI;
    Onset = Index >= 0 ? OnsetFor(Index, Cfg) : FMath::Lerp(double(Cfg.OnsetMinS), double(FMath::Max(Cfg.OnsetMaxS, Cfg.OnsetMinS)), double(Rng.FRand()));
    Reset();
    Time = Rng.FRand() * 10.0; // each its own phase of the hunt
}

void FSCCargoAttitude::Reset()
{
    Tilt = Rate = Command = FVector2D::ZeroVector;
    Offset = OffsetRate = FVector::ZeroVector;
    Yaw = YawRate = YawTarget = 0;
    bStarted = false;
    History.Reset();
    Pending.Reset();
    LastEvent = FVector2D::ZeroVector;
    LastEventTime = -1e9;
}

FVector2D FSCCargoAttitude::CommandFor(const FVector& A, const FSCCargoDroneArt::FMotion& Cfg)
{
    // The thrust tilted to give A with g: nose down to go forward (pitch < 0), rolled towards a push to the right.
    const double Up = FMath::Max(LookG + A.Z, 0.2 * LookG);
    return FVector2D(-FMath::RadiansToDegrees(FMath::Atan2(A.X, Up)), FMath::RadiansToDegrees(FMath::Atan2(A.Y, Up))) * Cfg.TiltGain;
}

void FSCCargoAttitude::Step(const FVector& AccelLocal, const FVector2D& Gust, double Dt, const FSCCargoDroneArt::FMotion& Cfg, bool bKicks)
{
    if (Dt <= 0 || Omega <= 0) return;
    // Its command as it answers it, Onset late (each drone its own: teammates never start together).
    const FVector2D Raw = CommandFor(AccelLocal, Cfg) * Gain;
    if (History.IsEmpty()) History.Add({Time, FVector2D::ZeroVector}); // at rest until now
    History.Add({Time + Dt, Raw});
    const double Then = Time + Dt - Onset;
    while (History.Num() > 2 && History[1].Key <= Then) History.RemoveAt(0, EAllowShrinking::No);
    FVector2D Late = History[0].Value;
    if (History.Num() > 1 && Then > History[0].Key)
        Late = FMath::Lerp(History[0].Value, History[1].Value, FMath::Clamp((Then - History[0].Key) / FMath::Max(History[1].Key - History[0].Key, 1e-9), 0.0, 1.0));
    // The command and how fast it moves (its lead is the kick a start or a stop gives).
    const FVector2D Cmd = Late + Gust;
    const FVector2D CmdRate = bStarted ? ((Cmd - Command) / Dt).ClampAxes(-3000.0, 3000.0) : FVector2D::ZeroVector;
    Command = Cmd;
    bStarted = true;
    if (bKicks)
    {
        // A motion starting or changing (its command moved on): a kick of its own, its direction about the change's
        // (biased: mostly that way, some off to a side, a few the other way), its size its own, a moment later.
        if (const FVector2D Change = Late - LastEvent; Change.Size() >= Cfg.EventDeg && Time - LastEventTime >= Cfg.EventMinS)
        {
            Pending.Add({Time + Rng.FRand() * Cfg.OnsetJitterS, DrawKick(Rng, Change, Cfg.KickDegS * Cfg.EventKickScale, Cfg)});
            LastEvent = Late;
            LastEventTime = Time;
        }
        // Its own corrections, now and then: a kick, mostly back towards level, and a new heading it wanders to.
        if (Cfg.KickRateHz > 0 && Rng.FRand() < 1.0 - FMath::Exp(-Cfg.KickRateHz * Dt))
        {
            Pending.Add({Time, DrawKick(Rng, Tilt.Size() > 0.3 ? -Tilt : FVector2D::ZeroVector, Cfg.KickDegS, Cfg)});
            YawTarget = Rng.FRandRange(-1.f, 1.f) * Cfg.YawJitterDeg;
        }
    }
    // Its drift and its yaw: slower springs of its own, better damped (drift) or as its tilt (yaw).
    const double Wp = 0.7 * Omega, Zp = 0.7, Wy = 0.6 * Omega;
    const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Dt / AttitudeStepS));
    const double H = Dt / Steps;
    for (int32 K = 0; K < Steps; ++K)
    {
        // The kicks whose moment has come.
        for (int32 I = Pending.Num() - 1; I >= 0; --I)
            if (Pending[I].At <= Time)
            {
                Rate += Pending[I].Rate;
                Pending.RemoveAtSwap(I);
            }
        Rate += ((Cmd - Tilt) * (Omega * Omega) + (CmdRate * Cfg.TiltLead - Rate) * (2 * Zeta * Omega)) * H;
        Tilt += Rate * H;
        Rim(Tilt, Rate, Cfg.MaxTiltDeg);
        // Tilted ahead of its thrust's command it is pushed that way: g tan(the error).
        const FVector2D Err = (Tilt - Cmd) * (UE_DOUBLE_PI / 180.0);
        const FVector2D Push(-LookG * FMath::Tan(Err.X), LookG * FMath::Tan(Err.Y));
        FVector2D P(Offset.X, Offset.Y), V(OffsetRate.X, OffsetRate.Y);
        V += (Push - P * (Wp * Wp) - V * (2 * Zp * Wp)) * H;
        P += V * H;
        Rim(P, V, Cfg.DriftCm);
        Offset.X = P.X;
        Offset.Y = P.Y;
        OffsetRate.X = V.X;
        OffsetRate.Y = V.Y;
        YawRate += ((YawTarget - Yaw) * (Wy * Wy) - YawRate * (2 * Zeta * Wy)) * H;
        Yaw += YawRate * H;
        Time += H;
    }
    Yaw = FMath::Clamp(Yaw, -2.0 * Cfg.YawJitterDeg, 2.0 * Cfg.YawJitterDeg);
    // Its altitude hunts a little, its own two tones.
    Offset.Z = Cfg.HuntCm * (0.65 * FMath::Sin(UE_DOUBLE_TWO_PI * HuntHz * Time + HuntPhase[0]) + 0.35 * FMath::Sin(UE_DOUBLE_TWO_PI * 1.73 * HuntHz * Time + HuntPhase[1]));
}

// ---------------------------------------------------------------------------------------------------- dock

FIntVector FSCCargoDockFootprint::Cells(const FVector& MeshSizeCm, const FVector& CellCm)
{
    FIntVector C;
    for (int32 A = 0; A < 3; ++A) C[A] = FMath::Max(1, FMath::RoundToInt(MeshSizeCm[A] / FMath::Max(CellCm[A], 1e-3)));
    return C;
}

double FSCCargoDockFootprint::Scale(const FVector& MeshSizeCm, const FIntVector& Cells, const FVector& CellCm)
{
    const double X = Cells.X * CellCm.X / FMath::Max(MeshSizeCm.X, 1e-3), Y = Cells.Y * CellCm.Y / FMath::Max(MeshSizeCm.Y, 1e-3);
    return FMath::Min(X, Y);
}

TArray<int32> FSCCargoDockFootprint::Turns(const FIntVector& Cells)
{
    if (Cells.X == Cells.Y) return {0, 1, 2, 3};
    return {0, 2};
}
