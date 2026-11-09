// FSCPlayerAnimator: body-clip sampling, blend-space weights, fragment cross-fades, additive and one-shot layers,
// root-motion pin, spine pitch and off-hand two-bone IK. Every name it uses comes from the player block; the only
// vocabulary is Mannequin's (fragment ids, stance tags, blend-space parameter names).
#include "SCPlayerAnimator.h"
#include "SCAnimImporter.h"
#include "ReferenceSkeleton.h"
#include "Algo/BinarySearch.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SCPackageSource.h"
#include "Async/ParallelFor.h"
#include <atomic>
#include "Misc/Paths.h"

const FString FSCPlayerAnimator::StandTag = TEXT("stand");
const FString FSCPlayerAnimator::CrouchTag = TEXT("crouch");
const FName FSCPlayerAnimator::IdleFragment = TEXT("MotionIdle");
const FName FSCPlayerAnimator::MoveFragment = TEXT("MotionMovement");
const FName FSCPlayerAnimator::SelectFragment = TEXT("select");
const FName FSCPlayerAnimator::DeselectFragment = TEXT("deselect");

namespace
{
using FObj = TSharedPtr<FJsonObject>;
const TArray<TSharedPtr<FJsonValue>>& Arr(const FObj& O, const TCHAR* Key)
{
    static const TArray<TSharedPtr<FJsonValue>> Empty;
    const TArray<TSharedPtr<FJsonValue>>* A;
    return O && O->TryGetArrayField(Key, A) ? *A : Empty;
}
// Mannequin blend-space parameter names (CryEngine motion parameters) the locomotion input feeds.
const TCHAR* MoveSpeedParam = TEXT("MoveSpeed");
const TCHAR* TravelAngleParam = TEXT("TravelAngle");

template <typename T> void Bracket(const TArray<float>& Times, const TArray<T>& Keys, float Frame, int32& A, int32& B, float& Alpha)
{
    B = FMath::Clamp(int32(Algo::UpperBound(Times, Frame)), 0, Keys.Num() - 1);
    A = FMath::Max(0, B - 1);
    const float Span = Times[B] - Times[A];
    Alpha = Span > 0 ? FMath::Clamp((Frame - Times[A]) / Span, 0.f, 1.f) : 0.f;
}
FQuat SampleRot(const FSCPlayerClip::FTrack& T, float Frame)
{
    int32 A, B;
    float Alpha;
    Bracket(T.RotTimes, T.Rot, Frame, A, B, Alpha);
    return FQuat(FQuat4f::Slerp(T.Rot[A], T.Rot[B], Alpha));
}
FVector SamplePos(const FSCPlayerClip::FTrack& T, float Frame)
{
    int32 A, B;
    float Alpha;
    Bracket(T.PosTimes, T.Pos, Frame, A, B, Alpha);
    return FVector(FMath::Lerp(T.Pos[A], T.Pos[B], Alpha));
}
void BlendInto(TArray<FTransform>& Acc, const TArray<FTransform>& Pose, float Alpha)
{
    for (int32 I = 0; I < Acc.Num(); ++I)
    {
        Acc[I].SetRotation(FQuat::Slerp(Acc[I].GetRotation(), Pose[I].GetRotation(), Alpha).GetNormalized());
        Acc[I].SetTranslation(FMath::Lerp(Acc[I].GetTranslation(), Pose[I].GetTranslation(), double(Alpha)));
    }
}
// Ericson, Real-Time Collision Detection 5.1.5: barycentrics of the closest point of triangle ABC to P.
FVector ClosestBarycentric(const FVector2D& P, const FVector2D& A, const FVector2D& B, const FVector2D& C)
{
    const FVector2D AB = B - A, AC = C - A, AP = P - A;
    const double D1 = AB | AP, D2 = AC | AP;
    if (D1 <= 0 && D2 <= 0) return FVector(1, 0, 0);
    const FVector2D BP = P - B;
    const double D3 = AB | BP, D4 = AC | BP;
    if (D3 >= 0 && D4 <= D3) return FVector(0, 1, 0);
    const double VC = D1 * D4 - D3 * D2;
    if (VC <= 0 && D1 >= 0 && D3 <= 0) { const double V = D1 / (D1 - D3); return FVector(1 - V, V, 0); }
    const FVector2D CP = P - C;
    const double D5 = AB | CP, D6 = AC | CP;
    if (D6 >= 0 && D5 <= D6) return FVector(0, 0, 1);
    const double VB = D5 * D2 - D1 * D6;
    if (VB <= 0 && D2 >= 0 && D6 <= 0) { const double W = D2 / (D2 - D6); return FVector(1 - W, 0, W); }
    const double VA = D3 * D6 - D5 * D4;
    if (VA <= 0 && (D4 - D3) >= 0 && (D5 - D6) >= 0) { const double W = (D4 - D3) / ((D4 - D3) + (D5 - D6)); return FVector(0, 1 - W, W); }
    const double Denom = 1.0 / (VA + VB + VC);
    const double V = VB * Denom, W = VC * Denom;
    return FVector(1 - V - W, V, W);
}
void ComputeCS(const TArray<FTransform>& Local, const TArray<int32>& Parents, TArray<FTransform>& CS)
{
    CS.SetNum(Local.Num());
    for (int32 I = 0; I < Local.Num(); ++I)
        CS[I] = Parents[I] == INDEX_NONE ? Local[I] : Local[I] * CS[Parents[I]];
}
} // namespace

// ---------------------------------------------------------------- clips

bool FSCPlayerClip::Parse(const FString& Text, TFunctionRef<int32(const FString&)> BoneIndex, FSCPlayerClip& Out, FString& Error)
{
    Out = FSCPlayerClip();
    FObj Json;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json)
    {
        Error = TEXT("Invalid clip JSON");
        return false;
    }
    double Fps = 0, Frames = 0, Duration = 0;
    Json->TryGetNumberField(TEXT("fps"), Fps);
    Json->TryGetNumberField(TEXT("frame_count"), Frames);
    if (!FMath::IsFinite(Fps) || Fps <= 0 || !FMath::IsFinite(Frames) || Frames < 0)
    {
        Error = TEXT("Invalid clip frame rate/length");
        return false;
    }
    Out.Name = FName(*Json->GetStringField(TEXT("name")));
    Out.Fps = float(Fps);
    Out.Duration = Json->TryGetNumberField(TEXT("duration"), Duration) && Duration > 0 ? float(Duration) : float(Frames / Fps);
    Out.Frames = Out.Duration * Out.Fps;
    Json->TryGetBoolField(TEXT("additive"), Out.bAdditive);
    Json->TryGetBoolField(TEXT("cyclic"), Out.bCyclic);
    FString Skeleton;
    Json->TryGetStringField(TEXT("source_skeleton_path"), Skeleton);
    const FObj* Bones;
    if (!Json->TryGetObjectField(TEXT("bones"), Bones))
    {
        Error = TEXT("Clip has no bones object");
        return false;
    }
    for (const auto& Pair : (*Bones)->Values)
    {
        TArray<TSharedPtr<FJsonValue>> Variants;
        if (Pair.Value->Type == EJson::Array) Variants = Pair.Value->AsArray(); else Variants.Add(Pair.Value);
        FObj Channel;
        int32 Bone = INDEX_NONE;
        for (const auto& V : Variants)
        {
            if (V->Type != EJson::Object) continue;
            const FObj O = V->AsObject();
            const int32 Index = BoneIndex(O->GetStringField(TEXT("source_node_name")));
            if (Index == INDEX_NONE) continue;
            // Several source skeletons may share a CRC: the clip's own skeleton wins.
            if (!Channel || O->GetStringField(TEXT("source_skeleton_path")) == Skeleton)
            {
                Channel = O;
                Bone = Index;
            }
        }
        if (!Channel) continue;
        FTrack Track;
        Track.Bone = Bone;
        bool HR = true, HP = true;
        Channel->TryGetBoolField(TEXT("has_rotation"), HR);
        Channel->TryGetBoolField(TEXT("has_position"), HP);
        auto Times = [&](const TCHAR* Key, int32 Count, TArray<float>& Dest) {
            const auto& T = Arr(Channel, Key);
            for (int32 K = 0; K < Count; ++K)
                Dest.Add(T.IsValidIndex(K) ? float(T[K]->AsNumber()) : float(K));
        };
        if (HR)
            for (const auto& K : Arr(Channel, TEXT("rotation")))
            {
                const auto& Q = K->AsArray();
                if (Q.Num() != 4) continue;
                Track.Rot.Add(FQuat4f(FSCAnimImporter::SidecarRotation(
                    FQuat(Q[1]->AsNumber(), Q[2]->AsNumber(), Q[3]->AsNumber(), Q[0]->AsNumber()).GetNormalized())));
            }
        if (HP)
            for (const auto& K : Arr(Channel, TEXT("position")))
            {
                const auto& P = K->AsArray();
                if (P.Num() != 3) continue;
                Track.Pos.Add(FVector3f(FSCAnimImporter::SidecarPosition(FVector(P[0]->AsNumber(), P[1]->AsNumber(), P[2]->AsNumber()))));
            }
        Times(TEXT("rotation_time"), Track.Rot.Num(), Track.RotTimes);
        Times(TEXT("position_time"), Track.Pos.Num(), Track.PosTimes);
        if (!Track.Rot.IsEmpty() || !Track.Pos.IsEmpty()) Out.Tracks.Add(MoveTemp(Track));
    }
    return true;
}
void FSCPlayerClip::Sample(float Seconds, TArray<FTransform>& Pose) const
{
    const float Frame = FMath::Clamp(Seconds * Fps, 0.f, Frames);
    for (const FTrack& T : Tracks)
    {
        if (!Pose.IsValidIndex(T.Bone)) continue;
        if (!T.Rot.IsEmpty()) Pose[T.Bone].SetRotation(SampleRot(T, Frame));
        if (!T.Pos.IsEmpty()) Pose[T.Bone].SetTranslation(SamplePos(T, Frame));
    }
}
void FSCPlayerClip::Overlay(float Seconds, float Alpha, TArray<FTransform>& Pose) const
{
    if (Alpha <= 0) return;
    const float Frame = FMath::Clamp(Seconds * Fps, 0.f, Frames);
    for (const FTrack& T : Tracks)
    {
        if (!Pose.IsValidIndex(T.Bone)) continue;
        if (!T.Rot.IsEmpty()) Pose[T.Bone].SetRotation(FQuat::Slerp(Pose[T.Bone].GetRotation(), SampleRot(T, Frame), Alpha).GetNormalized());
        if (!T.Pos.IsEmpty()) Pose[T.Bone].SetTranslation(FMath::Lerp(Pose[T.Bone].GetTranslation(), SamplePos(T, Frame), double(Alpha)));
    }
}
void FSCPlayerClip::ApplyAdditive(float Seconds, float Alpha, TArray<FTransform>& Pose) const
{
    if (Alpha <= 0) return;
    const float Frame = FMath::Clamp(Seconds * Fps, 0.f, Frames);
    for (const FTrack& T : Tracks)
    {
        if (!Pose.IsValidIndex(T.Bone)) continue;
        if (!T.Rot.IsEmpty())
        {
            const FQuat Delta = FQuat::Slerp(FQuat::Identity, SampleRot(T, Frame), Alpha);
            Pose[T.Bone].SetRotation((Delta * Pose[T.Bone].GetRotation()).GetNormalized()); // parent-space delta on the base
        }
        if (!T.Pos.IsEmpty()) Pose[T.Bone].AddToTranslation(SamplePos(T, Frame) * Alpha);
    }
}

// ---------------------------------------------------------------- blend spaces

void FSCBlendSampler::Weights(const FSCBlendSpace& Space, TConstArrayView<float> Params, TArray<TPair<int32, float>>& Out)
{
    Out.Reset();
    const int32 Dims = Space.Dimensions.Num();
    if (Dims < 1 || Dims > 2 || Params.Num() != Dims) return;
    auto Normalised = [&](TConstArrayView<float> P) {
        FVector2D V(0, 0);
        for (int32 D = 0; D < Dims; ++D)
        {
            const auto& Dim = Space.Dimensions[D];
            const float Lo = FMath::Min(Dim.Min, Dim.AuthoredMin), Hi = FMath::Max(Dim.Max, Dim.AuthoredMax);
            V[D] = (P[D] - Lo) / (Hi - Lo > UE_KINDA_SMALL_NUMBER ? Hi - Lo : 1.f);
        }
        return V;
    };
    auto Usable = [&](int32 E) {
        if (!Space.Examples.IsValidIndex(E) || Space.Examples[E].bMissing || Space.Examples[E].Params.Num() != Dims) return false;
        for (float X : Space.Examples[E].Params)
            if (!FMath::IsFinite(X)) return false;
        return true;
    };
    const FVector2D P = Normalised(Params);
    double Best = TNumericLimits<double>::Max();
    // Faces are fans (quads split along their first diagonal); a 1-D space uses its face edges as segments.
    for (const auto& Face : Space.Faces)
    {
        bool bOK = Face.Num() >= 2;
        for (int32 E : Face) bOK &= Usable(E);
        if (!bOK) continue;
        auto Try = [&](int32 IA, int32 IB, int32 IC) {
            const FVector2D A = Normalised(Space.Examples[IA].Params), B = Normalised(Space.Examples[IB].Params);
            const FVector2D C = IC == INDEX_NONE ? B : Normalised(Space.Examples[IC].Params);
            FVector Bary;
            if (IC == INDEX_NONE || FMath::Abs((B - A) ^ (C - A)) < 1e-9)
            {
                // Segment (or degenerate triangle): closest point on the longest edge.
                const FVector2D AB = B - A;
                const double T = AB.SizeSquared() > 0 ? FMath::Clamp(((P - A) | AB) / AB.SizeSquared(), 0.0, 1.0) : 0.0;
                Bary = FVector(1 - T, T, 0);
            }
            else
                Bary = ClosestBarycentric(P, A, B, C);
            const FVector2D Q = A * Bary.X + B * Bary.Y + C * Bary.Z;
            const double Distance = FVector2D::DistSquared(P, Q);
            if (Distance < Best - 1e-12)
            {
                Best = Distance;
                Out = {{IA, float(Bary.X)}, {IB, float(Bary.Y)}};
                if (IC != INDEX_NONE) Out.Add({IC, float(Bary.Z)});
            }
        };
        if (Face.Num() == 2) Try(Face[0], Face[1], INDEX_NONE);
        for (int32 K = 1; K + 1 < Face.Num(); ++K)
            Try(Face[0], Face[K], Face[K + 1]);
    }
    // Merge repeated examples, drop zero weights, renormalise (sum exactly 1).
    TArray<TPair<int32, float>> Merged;
    float Sum = 0;
    for (const auto& W : Out)
    {
        const float X = FMath::Max(0.f, W.Value);
        if (X <= 1e-6f) continue;
        if (auto* Found = Merged.FindByPredicate([&](const TPair<int32, float>& M) { return M.Key == W.Key; })) Found->Value += X;
        else Merged.Add({W.Key, X});
        Sum += X;
    }
    for (auto& W : Merged) W.Value /= Sum;
    Out = MoveTemp(Merged);
}
TArray<float> FSCBlendSampler::LocomotionParams(const FSCBlendSpace& Space, float Speed, float TravelAngle)
{
    TArray<float> P;
    P.SetNumZeroed(Space.Dimensions.Num());
    if (const int32 S = Space.Dimension(MoveSpeedParam); S != INDEX_NONE) P[S] = Speed;
    if (const int32 A = Space.Dimension(TravelAngleParam); A != INDEX_NONE) P[A] = TravelAngle;
    return P;
}
float FSCBlendSampler::NominalForwardSpeed(const FSCBlendSpace& Space)
{
    const int32 S = Space.Dimension(MoveSpeedParam), A = Space.Dimension(TravelAngleParam);
    if (S == INDEX_NONE) return 0;
    // "Forward" examples lie closer to angle 0 than half the smallest authored non-zero angle.
    float Smallest = UE_PI;
    for (const auto& E : Space.Examples)
        if (A != INDEX_NONE && E.Params.IsValidIndex(A) && FMath::Abs(E.Params[A]) > 1e-2f) Smallest = FMath::Min(Smallest, FMath::Abs(E.Params[A]));
    const float Grid = Space.Dimensions[S].AuthoredMax * 1.01f; // float tolerance on the authored grid bound
    float Best = 0;
    for (const auto& E : Space.Examples)
    {
        if (E.bMissing || !E.Params.IsValidIndex(S) || !FMath::IsFinite(E.Params[S])) continue;
        if (A != INDEX_NONE && (!E.Params.IsValidIndex(A) || FMath::Abs(E.Params[A]) > Smallest / 2)) continue;
        if (E.Params[S] <= Grid) Best = FMath::Max(Best, E.Params[S]);
    }
    return Best;
}
bool FSCBlendSampler::CoversAngle(const FSCBlendSpace& Space, float TravelAngle)
{
    const int32 A = Space.Dimension(TravelAngleParam);
    if (A == INDEX_NONE) return true;
    float Lo = TNumericLimits<float>::Max(), Hi = -TNumericLimits<float>::Max();
    for (const auto& E : Space.Examples)
        if (!E.bMissing && E.Params.IsValidIndex(A) && FMath::IsFinite(E.Params[A]))
        {
            Lo = FMath::Min(Lo, E.Params[A]);
            Hi = FMath::Max(Hi, E.Params[A]);
        }
    return TravelAngle >= Lo - 1e-3f && TravelAngle <= Hi + 1e-3f;
}

// ---------------------------------------------------------------- weapon state machine

ESCWeaponAction FSCWeaponStateMachine::Request(int32 Index) const
{
    switch (State)
    {
    case ESCWeaponState::Unarmed: return Index == INDEX_NONE ? ESCWeaponAction::None : ESCWeaponAction::Select;
    case ESCWeaponState::Armed: return ESCWeaponAction::Deselect; // own key or H holsters; another key swaps via deselect
    default: return ESCWeaponAction::None;                        // mid-transition: ignored
    }
}
void FSCWeaponStateMachine::BeginSelect(int32 Index, float Seconds)
{
    State = ESCWeaponState::Selecting;
    Active = Index;
    Queued = INDEX_NONE;
    Elapsed = 0;
    Duration = FMath::Max(0.f, Seconds);
}
void FSCWeaponStateMachine::BeginDeselect(float Seconds, int32 Then)
{
    State = ESCWeaponState::Deselecting;
    Queued = Then == Active ? INDEX_NONE : Then;
    Elapsed = 0;
    Duration = FMath::Max(0.f, Seconds);
}
FSCWeaponStateMachine::EEvent FSCWeaponStateMachine::Tick(float DeltaSeconds)
{
    if (State != ESCWeaponState::Selecting && State != ESCWeaponState::Deselecting) return EEvent::None;
    Elapsed += DeltaSeconds;
    if (Elapsed < Duration) return EEvent::None;
    if (State == ESCWeaponState::Selecting)
    {
        State = ESCWeaponState::Armed;
        return EEvent::Selected;
    }
    State = ESCWeaponState::Unarmed;
    Active = INDEX_NONE;
    return EEvent::Holstered;
}
const TCHAR* FSCWeaponStateMachine::Name(ESCWeaponState S)
{
    switch (S)
    {
    case ESCWeaponState::Unarmed: return TEXT("Unarmed");
    case ESCWeaponState::Selecting: return TEXT("Selecting");
    case ESCWeaponState::Armed: return TEXT("Armed");
    case ESCWeaponState::Deselecting: return TEXT("Deselecting");
    }
    return TEXT("?");
}

// ---------------------------------------------------------------- animator

bool FSCPlayerAnimator::Init(const FSCPlayerKit& InKit, const FReferenceSkeleton& Ref, FSCPlayerClipMap InClips, FString& Error)
{
    Kit = &InKit;
    Clips = MoveTemp(InClips);
    RefPose = Ref.GetRefBonePose();
    Parents.SetNum(Ref.GetNum());
    BoneIndex.Reset();
    for (int32 I = 0; I < Ref.GetNum(); ++I)
    {
        Parents[I] = Ref.GetParentIndex(I);
        BoneIndex.Add(Ref.GetBoneName(I).ToString(), I);
    }
    RootMotion = Bone(Kit->RootMotionBone);
    const int32 Camera = Bone(Kit->CameraBone);
    if (RootMotion == INDEX_NONE || Camera == INDEX_NONE)
    {
        Error = FString::Printf(TEXT("Body skeleton lacks %s or %s"), *Kit->RootMotionBone, *Kit->CameraBone);
        return false;
    }
    // Pitch chain: the camera joint's ancestors below the body root (the root-motion joint's child).
    SpineChain.Reset();
    for (int32 B = Parents[Camera]; B != INDEX_NONE && Parents[B] != RootMotion && B != RootMotion; B = Parents[B])
        SpineChain.Insert(B, 0);
    // Facing: native +Y of the root-motion joint (CryEngine character forward), in component space.
    TArray<FTransform> CS;
    ComputeCS(RefPose, Parents, CS);
    ForwardCS = CS[RootMotion].TransformVectorNoScale(FSCAnimImporter::SidecarPosition(FVector(0, 0, 1)) / 100.0);
    ForwardCS.Z = 0;
    ForwardCS = ForwardCS.GetSafeNormal();
    Pose = RefPose;
    Sources.Reset();
    OneShot = FOneShot();
    return true;
}
int32 FSCPlayerAnimator::Bone(const FString& Name) const
{
    const int32* Found = BoneIndex.Find(Name);
    return Found ? *Found : INDEX_NONE;
}
const FSCPlayerClip* FSCPlayerAnimator::Clip(FName Name) const
{
    const TSharedPtr<const FSCPlayerClip>* Found = Clips.Find(Name);
    return Found ? Found->Get() : nullptr;
}
FTransform FSCPlayerAnimator::ComponentSpace(int32 B) const
{
    FTransform T = FTransform::Identity;
    for (; Pose.IsValidIndex(B); B = Parents[B]) T = T * Pose[B];
    return T;
}
void FSCPlayerAnimator::SpeedTags(const FString& Weapon, const FString& Stance, bool bFirstPerson, FString& Slow, FString& Fast) const
{
    // Speed tags ordered by their nominal forward speed (name-free): the slowest is walking, the fastest running.
    float SlowSpeed = TNumericLimits<float>::Max(), FastSpeed = -1;
    Slow.Reset();
    Fast.Reset();
    for (const auto& S : Kit->States)
    {
        if (S.WeaponChildId != Weapon || S.Stance != Stance || S.bFirstPerson != bFirstPerson || S.MoveSpeedTag.IsEmpty()) continue;
        const FSCMotionFragment* Move = S.Default(MoveFragment);
        const FSCMotionLayer* Layer = Move ? BaseLayer(*Move) : nullptr;
        const FSCBlendSpace* Space = Layer ? Kit->BlendSpaces.Find(Layer->BlendSpace) : nullptr;
        if (!Space) continue;
        const float Speed = FSCBlendSampler::NominalForwardSpeed(*Space);
        if (Speed <= 0) continue;
        if (Speed < SlowSpeed) { SlowSpeed = Speed; Slow = S.MoveSpeedTag; }
        if (Speed > FastSpeed) { FastSpeed = Speed; Fast = S.MoveSpeedTag; }
    }
}
void FSCPlayerAnimator::LocomotionSpeeds(const FString& Weapon, bool bFirstPerson, float& SlowOut, float& FastOut) const
{
    SlowOut = FastOut = 0;
    FString Slow, Fast;
    SpeedTags(Weapon, StandTag, bFirstPerson, Slow, Fast);
    auto Nominal = [&](const FString& Tag) {
        const FSCMotionState* S = Kit->FindState(Weapon, StandTag, Tag, bFirstPerson);
        const FSCMotionFragment* Move = S ? S->Default(MoveFragment) : nullptr;
        const FSCMotionLayer* Layer = Move ? BaseLayer(*Move) : nullptr;
        const FSCBlendSpace* Space = Layer ? Kit->BlendSpaces.Find(Layer->BlendSpace) : nullptr;
        return Space ? FSCBlendSampler::NominalForwardSpeed(*Space) : 0.f;
    };
    SlowOut = Nominal(Slow);
    FastOut = Nominal(Fast);
}
const FSCMotionState* FSCPlayerAnimator::ResolveState(const FSCAnimatorInput& Input, bool* bFastOut) const
{
    FString Stance = Input.bCrouched ? CrouchTag : StandTag;
    if (!Kit->FindState(Input.WeaponChildId, Stance, FString(), Input.bFirstPerson)) Stance = StandTag;
    FString Slow, Fast;
    SpeedTags(Input.WeaponChildId, Stance, Input.bFirstPerson, Slow, Fast);
    FString Tag = Input.bFast ? Fast : Slow;
    if (Input.bFast)
    {
        // The fast space may not cover sideways/backward travel (sprint): use the slow tag there.
        const FSCMotionState* S = Kit->FindState(Input.WeaponChildId, Stance, Fast, Input.bFirstPerson);
        const FSCMotionFragment* Move = S ? S->Default(MoveFragment) : nullptr;
        const FSCMotionLayer* Layer = Move ? BaseLayer(*Move) : nullptr;
        const FSCBlendSpace* Space = Layer ? Kit->BlendSpaces.Find(Layer->BlendSpace) : nullptr;
        if (Space && Input.Speed >= MoveThreshold && !FSCBlendSampler::CoversAngle(*Space, Input.TravelAngle)) Tag = Slow;
    }
    if (bFastOut) *bFastOut = Tag == Fast && !Fast.IsEmpty();
    const FSCMotionState* State = Kit->FindState(Input.WeaponChildId, Stance, Tag, Input.bFirstPerson);
    return State ? State : Kit->FindState(Input.WeaponChildId, Stance, FString(), Input.bFirstPerson);
}
const FSCMotionLayer* FSCPlayerAnimator::BaseLayer(const FSCMotionFragment& F) const
{
    for (const auto& L : F.Layers)
        if (!L.bAdditive) return &L;
    return nullptr;
}
float FSCPlayerAnimator::Length(const FSCMotionFragment& F) const
{
    float Seconds = 0;
    for (const auto& L : F.Layers)
        if (const FSCPlayerClip* C = Clip(L.Clip)) Seconds = FMath::Max(Seconds, C->Duration / FMath::Max(L.Speed, UE_KINDA_SMALL_NUMBER));
    return Seconds;
}
void FSCPlayerAnimator::EvaluateSource(FSource& S, const FSCAnimatorInput& Input, TArray<FTransform>& Out, bool bAdvance)
{
    Out = RefPose;
    const FSCMotionLayer* Layer = S.Fragment ? BaseLayer(*S.Fragment) : nullptr;
    if (!Layer) return;
    const float Dt = bAdvance ? Input.DeltaSeconds * Layer->Speed : 0.f;
    if (const FSCPlayerClip* C = Clip(Layer->Clip))
    {
        C->Sample(S.Time, Out);
        S.Time += Dt;
        if (Layer->bLoop && C->Duration > 0) S.Time = FMath::Fmod(S.Time, C->Duration);
        else S.Time = FMath::Min(S.Time, C->Duration);
        return;
    }
    const FSCBlendSpace* Space = Kit->BlendSpaces.Find(Layer->BlendSpace);
    if (!Space) return;
    TArray<TPair<int32, float>> Weights;
    FSCBlendSampler::Weights(*Space, FSCBlendSampler::LocomotionParams(*Space, Input.Speed, Input.TravelAngle), Weights);
    // Examples share one normalised phase (in step); the phase rate is the weighted rate of the examples.
    float Rate = 0, Accumulated = 0;
    for (const auto& W : Weights)
    {
        const FSCBlendExample& E = Space->Examples[W.Key];
        const FSCPlayerClip* C = Clip(E.Clip);
        if (!C || C->Duration <= 0) continue;
        Scratch2 = RefPose;
        C->Sample(S.Time * C->Duration, Scratch2);
        if (Accumulated <= 0) Out = Scratch2;
        else BlendInto(Out, Scratch2, W.Value / (Accumulated + W.Value));
        Accumulated += W.Value;
        Rate += W.Value * E.PlaybackScale / C->Duration;
    }
    S.Time = FMath::Fmod(S.Time + Dt * Rate, 1.f);
}
float FSCPlayerAnimator::PlayOneShot(FName Fragment, const FSCAnimatorInput& Input)
{
    const FSCMotionState* State = ResolveState(Input);
    const FSCMotionFragment* F = State ? State->Default(Fragment) : nullptr;
    OneShot = FOneShot();
    if (!F) return 0;
    OneShot.Fragment = F;
    OneShot.Duration = Length(*F);
    return OneShot.Duration;
}
float FSCPlayerAnimator::OneShotProgress() const
{
    return OneShot.Fragment && OneShot.Duration > 0 ? OneShot.Time / OneShot.Duration : -1.f;
}
void FSCPlayerAnimator::Tick(const FSCAnimatorInput& Input)
{
    if (!IsReady()) return;
    bool bFast = false;
    const FSCMotionState* State = ResolveState(Input, &bFast);
    const bool bMoving = Input.Speed >= MoveThreshold;
    const FSCMotionFragment* Fragment = State ? State->Default(bMoving ? MoveFragment : IdleFragment) : nullptr;
    if (!Fragment && State) Fragment = State->Default(IdleFragment);
    const FSCMotionLayer* Layer = Fragment ? BaseLayer(*Fragment) : nullptr;
    const FName Key = Layer ? (Layer->Clip.IsNone() ? Layer->BlendSpace : Layer->Clip) : NAME_None;
    if (Layer && (Sources.IsEmpty() || Sources.Last().Key != Key))
    {
        FSource New;
        New.Fragment = Fragment;
        New.Key = Key;
        // Blend spaces stay in step across a switch (walk <-> run): carry the normalised phase.
        const bool bSpace = Layer->Clip.IsNone();
        const bool bPrevSpace = !Sources.IsEmpty() && Sources.Last().Fragment && BaseLayer(*Sources.Last().Fragment) &&
                                BaseLayer(*Sources.Last().Fragment)->Clip.IsNone();
        New.Time = bSpace && bPrevSpace ? Sources.Last().Time : Layer->StartTime;
        New.Weight = Sources.IsEmpty() ? 1.f : 0.f;
        New.FadeRate = Layer->BlendIn > 0 ? 1.f / Layer->BlendIn : 1e6f;
        Sources.Add(New);
        if (Sources.Num() > 3) Sources.RemoveAt(0);
    }
    else if (Fragment && !Sources.IsEmpty())
        Sources.Last().Fragment = Fragment; // same base motion, other state (e.g. FP <-> TP): no fade
    // Cross-fade: the newest source rises linearly over its blend-in, older ones share the remainder.
    if (!Sources.IsEmpty())
    {
        FSource& Top = Sources.Last();
        Top.Weight = FMath::Min(1.f, Top.Weight + Input.DeltaSeconds * Top.FadeRate);
        float Older = 0;
        for (int32 I = 0; I + 1 < Sources.Num(); ++I) Older += Sources[I].Weight;
        for (int32 I = 0; I + 1 < Sources.Num(); ++I) Sources[I].Weight = Older > 0 ? Sources[I].Weight / Older * (1 - Top.Weight) : 0;
        for (int32 I = Sources.Num() - 2; I >= 0; --I)
            if (Sources[I].Weight < 1e-3f) Sources.RemoveAt(I);
    }
    float Accumulated = 0;
    for (FSource& S : Sources)
    {
        EvaluateSource(S, Input, Scratch, true);
        if (Accumulated <= 0) Pose = Scratch;
        else BlendInto(Pose, Scratch, S.Weight / (Accumulated + S.Weight));
        Accumulated += S.Weight;
    }
    if (Sources.IsEmpty()) Pose = RefPose;
    // Additive layers of the current base fragment (layer weight; zero stays zero).
    if (!Sources.IsEmpty() && Sources.Last().Fragment)
        for (const auto& L : Sources.Last().Fragment->Layers)
            if (L.bAdditive)
                if (const FSCPlayerClip* C = Clip(L.Clip))
                    C->ApplyAdditive(L.Speed > 0 ? FMath::Fmod(Sources.Last().Time * L.Speed, FMath::Max(C->Duration, UE_KINDA_SMALL_NUMBER)) : 0.f,
                                     L.Weight * Sources.Last().Weight, Pose);
    // One-shot fragment (select/deselect): full layers override sparsely, additive layers compose on top.
    if (OneShot.Fragment)
    {
        OneShot.Time += Input.DeltaSeconds;
        for (const auto& L : OneShot.Fragment->Layers)
        {
            const FSCPlayerClip* C = Clip(L.Clip);
            if (!C) continue;
            const float T = OneShot.Time * L.Speed;
            const float End = C->Duration / FMath::Max(L.Speed, UE_KINDA_SMALL_NUMBER);
            float W = L.Weight * (L.BlendIn > 0 ? FMath::Min(1.f, OneShot.Time / L.BlendIn) : 1.f);
            if (L.bAdditive)
                C->ApplyAdditive(T, W, Pose);
            else
            {
                if (L.BlendOut > 0) W *= FMath::Clamp((End - OneShot.Time) / L.BlendOut, 0.f, 1.f);
                C->Overlay(T, W, Pose);
            }
        }
        if (OneShot.Time >= OneShot.Duration) OneShot = FOneShot();
    }
    // The capsule moves the character; the root-motion joint stays at its bind so the mesh does not run away.
    if (Pose.IsValidIndex(RootMotion)) Pose[RootMotion] = RefPose[RootMotion];
    ApplyPitch(Input.PitchDegrees);
    SolveOffHand(Input);
    Debug = FString::Printf(TEXT("state=%s/%s/%s/%s fragment=%s base=%s sources=%d fast=%d oneshot=%s"),
        State ? *State->WeaponChildId : TEXT("-"), State ? *State->Stance : TEXT("-"), State ? *State->MoveSpeedTag : TEXT("-"),
        State && State->bFirstPerson ? TEXT("fp") : TEXT("tp"), Fragment ? *Fragment->Fragment.ToString() : TEXT("-"), *Key.ToString(),
        Sources.Num(), bFast, OneShot.Fragment ? *OneShot.Fragment->Fragment.ToString() : TEXT("-"));
}
void FSCPlayerAnimator::ApplyPitch(float Degrees)
{
    if (SpineChain.IsEmpty() || FMath::IsNearlyZero(Degrees)) return;
    TArray<FTransform> CS;
    ComputeCS(Pose, Parents, CS);
    // Rotating every chain joint by the same component-space increment: L' = Rp^-1 * dq * Rp * L with the
    // parent's original rotation Rp (the increments of the ancestors compose on the way down).
    const FVector Axis = FVector::CrossProduct(ForwardCS, FVector::UpVector).GetSafeNormal();
    const FQuat Step(Axis, FMath::DegreesToRadians(Degrees) / SpineChain.Num());
    for (int32 B : SpineChain)
    {
        const FQuat Rp = Parents[B] == INDEX_NONE ? FQuat::Identity : CS[Parents[B]].GetRotation();
        Pose[B].SetRotation((Rp.Inverse() * Step * Rp * Pose[B].GetRotation()).GetNormalized());
    }
}
void FSCPlayerAnimator::SolveOffHand(const FSCAnimatorInput& Input)
{
    OffHandBlend = FMath::FInterpConstantTo(OffHandBlend, Input.OffHandWeight, Input.DeltaSeconds, 5.f);
    OffHandError = -1;
    const int32 Effector = Input.OffHandBone;
    if (!Pose.IsValidIndex(Effector) || !Pose.IsValidIndex(Input.HandBone)) return;
    const int32 End = Parents[Effector], Lower = End != INDEX_NONE ? Parents[End] : INDEX_NONE, Upper = Lower != INDEX_NONE ? Parents[Lower] : INDEX_NONE;
    if (Upper == INDEX_NONE) return;
    TArray<FTransform> CS;
    ComputeCS(Pose, Parents, CS);
    const FVector Target = (Input.OffHandTargetInHand * CS[Input.HandBone]).GetLocation();
    const FVector E = CS[Effector].GetLocation();
    OffHandError = FVector::Dist(E, Target);
    if (OffHandBlend <= 0) return;
    const FVector Goal = FMath::Lerp(E, Target, double(OffHandBlend));
    const FVector A = CS[Upper].GetLocation(), B = CS[Lower].GetLocation(), C = CS[End].GetLocation();
    const FVector CGoal = Goal - (E - C); // the effector rides on the end joint, whose rotation is kept
    const double La = FVector::Dist(A, B), Lb = FVector::Dist(B, C);
    FVector ToGoal = CGoal - A;
    const double Dist = FMath::Clamp(ToGoal.Size(), FMath::Abs(La - Lb) + 0.01, La + Lb - 0.01);
    const FVector D = ToGoal.GetSafeNormal();
    FVector Bend = (B - A) - D * ((B - A) | D);
    if (Bend.IsNearlyZero()) Bend = FVector::CrossProduct(FVector::CrossProduct(C - A, B - A), D);
    Bend = Bend.GetSafeNormal();
    const double CosA = FMath::Clamp((La * La + Dist * Dist - Lb * Lb) / (2 * La * Dist), -1.0, 1.0);
    const FVector B2 = A + D * (La * CosA) + Bend * (La * FMath::Sqrt(1 - CosA * CosA));
    const FVector C2 = A + D * Dist;
    const FQuat UpperDelta = FQuat::FindBetweenVectors(B - A, B2 - A);
    const FQuat UpperRot = UpperDelta * CS[Upper].GetRotation();
    const FQuat LowerRot0 = UpperDelta * CS[Lower].GetRotation();
    const FQuat LowerDelta = FQuat::FindBetweenVectors(UpperDelta.RotateVector(C - B), C2 - B2);
    const FQuat LowerRot = LowerDelta * LowerRot0;
    const FQuat ParentRot = Parents[Upper] == INDEX_NONE ? FQuat::Identity : CS[Parents[Upper]].GetRotation();
    Pose[Upper].SetRotation((ParentRot.Inverse() * UpperRot).GetNormalized());
    Pose[Lower].SetRotation((UpperRot.Inverse() * LowerRot).GetNormalized());
    Pose[End].SetRotation((LowerRot.Inverse() * CS[End].GetRotation()).GetNormalized());
}

// ---------------------------------------------------------------- package loading

bool SCLoadPlayerKit(const FSCPackageSource& Source, const FString& ScenePath, const TMap<FString, int32>& Bones, TSharedPtr<FSCPlayerKit>& Kit,
                     TSharedPtr<FSCPlayerClipMap>& Clips, int32& Failed, FString& Error)
{
    TArray<uint8> Bytes;
    if (!Source.ReadBytes(ScenePath, Bytes, Error)) return false;
    Kit = MakeShared<FSCPlayerKit>();
    if (!FSCPlayerKit::Parse(FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num())), FPaths::GetPath(ScenePath), *Kit,
                             Error))
        return false;
    const TArray<FName> Names = Kit->ReferencedClips().Array();
    TArray<TSharedPtr<FSCPlayerClip>> Parsed;
    Parsed.SetNum(Names.Num());
    std::atomic<int32> Failures{0};
    ParallelFor(Names.Num(), [&](int32 I) {
        const FString* Sidecar = Kit->ClipSidecars.Find(Names[I]);
        TArray<uint8> ClipBytes;
        FString ClipError;
        auto Clip = MakeShared<FSCPlayerClip>();
        if (!Sidecar || !Source.ReadBytes(*Sidecar, ClipBytes, ClipError) ||
            !FSCPlayerClip::Parse(FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(ClipBytes.GetData()), ClipBytes.Num())),
                                  [&](const FString& Name) { const int32* B = Bones.Find(Name); return B ? *B : INDEX_NONE; }, *Clip, ClipError))
        {
            ++Failures;
            return;
        }
        Clip->Name = Names[I];
        Parsed[I] = Clip;
    });
    Clips = MakeShared<FSCPlayerClipMap>();
    for (int32 I = 0; I < Names.Num(); ++I)
        if (Parsed[I]) Clips->Add(Names[I], Parsed[I]);
    Failed = Failures;
    return true;
}
