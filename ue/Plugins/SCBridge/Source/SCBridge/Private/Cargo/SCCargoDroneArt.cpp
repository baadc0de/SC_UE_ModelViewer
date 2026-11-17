// FSCCargoDroneArt (Config/Cargo/drone_art.json), the heading policy, the motion clip state machine and the dock
// footprint rule (WP-K4a; header: Cargo/SCCargoDroneArt.h).
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

float FSCCargoHeading::Target(float Yaw, const FVector& Travel, float MinSpeedCmS, const TOptional<float>& BayYaw)
{
    if (BayYaw) return FRotator::NormalizeAxis(*BayYaw);
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
