#include "SCAnimImporter.h"
#include "SCSkeletonBuilder.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"
#if WITH_EDITOR
#include "Animation/AnimData/IAnimationDataController.h"
#endif
namespace
{
using FObj = TSharedPtr<FJsonObject>;
const TArray<TSharedPtr<FJsonValue>>& Arr(const FObj& O, const TCHAR* Key)
{
    static TArray<TSharedPtr<FJsonValue>> Empty; const TArray<TSharedPtr<FJsonValue>>* A;
    return O && O->TryGetArrayField(Key, A) ? *A : Empty;
}
FString Str(const FObj& O, const TCHAR* Key) { FString S; if (O) O->TryGetStringField(Key, S); return S; }
FQuat Quat(const TArray<TSharedPtr<FJsonValue>>& A)
{ return A.Num() == 4 ? FQuat(A[1]->AsNumber(), A[2]->AsNumber(), A[3]->AsNumber(), A[0]->AsNumber()).GetNormalized() : FQuat::Identity; }
FVector Vec(const TArray<TSharedPtr<FJsonValue>>& A)
{ return A.Num() == 3 ? FVector(A[0]->AsNumber(), A[1]->AsNumber(), A[2]->AsNumber()) : FVector::ZeroVector; }
TSet<FString> Tokens(FString Path)
{
    Path = Path.ToLower(); for (TCHAR& C : Path) if (!FChar::IsAlnum(C)) C = ' ';
    TArray<FString> Parts; Path.ParseIntoArrayWS(Parts); return TSet<FString>(Parts);
}
}
uint32 FSCAnimImporter::BoneHash(const FString& Name)
{
    // Standard CRC32 of case-sensitive UTF-8, not UE's TCHAR-based StrCrc32.
    FTCHARToUTF8 Bytes(*Name); uint32 CRC = ~0u;
    for (int32 I = 0; I < Bytes.Length(); ++I)
    {
        CRC ^= uint8(Bytes.Get()[I]);
        for (int32 Bit = 0; Bit < 8; ++Bit) CRC = (CRC >> 1) ^ ((CRC & 1) ? 0xedb88320u : 0);
    }
    return ~CRC;
}
FVector FSCAnimImporter::SidecarPosition(const FVector& V)
{
    // Exported node locals already use the sidecar basis under the external wrapper.
    // G=[0 0 -1;1 0 0;0 1 0]. Do not undo/reapply the wrapper on each local track.
    return FSCMeshImporter::Position(V);
}
FQuat FSCAnimImporter::SidecarRotation(const FQuat& Q)
{
    return FQuat(Q.Z, -Q.X, -Q.Y, Q.W).GetNormalized();
}

FQuat FSCAnimImporter::LinearRotation(FQuat A, FQuat B, double Alpha)
{
    if ((A | B) < 0) B = B * -1.;
    return (A * (1 - Alpha) + B * Alpha).GetNormalized();
}
int32 FSCAnimImporter::ProvenanceScore(const FString& ReceiverName, const FString& ReceiverPath, const FString& SourceName, const FString& SourcePath)
{
    int32 Score = ReceiverName.Equals(SourceName, ESearchCase::IgnoreCase) ? 1000 : 0;
    const auto A = Tokens(ReceiverPath), B = Tokens(SourcePath);
    for (const auto& T : A) if (B.Contains(T)) Score += 4;
    for (const FString D : {TEXT("front"), TEXT("rear"), TEXT("back"), TEXT("left"), TEXT("right")})
        if (A.Contains(D) != B.Contains(D)) Score -= 20;
    return Score;
}
bool FSCAnimImporter::Read(const FSCPackageSource& Source, const FSCClipRecord& Record, const FString& Owner,
    FSCImportedClip& Clip, FObj& Json, FString& Error)
{
    TArray<uint8> Bytes;
    if (!Source.ReadBytes(Record.Sidecar, Bytes, Error)) return false;
    const FString Text(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num()));
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json) { Error = TEXT("Invalid clip JSON"); return false; }
    double Fps = 0, Frames = 0;
    if (!Json->TryGetNumberField(TEXT("fps"), Fps) || !Json->TryGetNumberField(TEXT("frame_count"), Frames)
        || !FMath::IsFinite(Fps) || Fps <= 0 || Fps > 1000 || !FMath::IsFinite(Frames) || Frames < 0 || Frames > 100000)
    { Error = TEXT("Invalid clip frame rate/length (host limits: 1000 fps, 100000 frames)"); return false; }
    Clip.Id = FName(*Record.Name); Clip.Name = Record.Name; Clip.DisplayName = Str(Json, TEXT("display_name"));
    if (Clip.DisplayName.IsEmpty()) Clip.DisplayName = Clip.Name;
    Clip.Owner = Owner; Clip.Source = Record.Sidecar; Clip.Fps = Fps; Clip.LastFrame = FMath::CeilToInt(Frames);
    for (const auto& V : Arr(Json, TEXT("fragments")))
    {
        if (V->Type != EJson::Object) continue;
        const auto O = V->AsObject(); FSCClipFragment F; F.Fragment = FName(*Str(O, TEXT("fragment")));
        auto Names = [&](const TCHAR* Key, TArray<FName>& Dest) { for (const auto& X : Arr(O, Key)) if (X->Type == EJson::String) Dest.Add(FName(*X->AsString())); };
        Names(TEXT("tags"), F.Tags); Names(TEXT("frag_tags"), F.FragmentTags); Names(TEXT("scopes"), F.Scopes);
        for (const auto& A : Arr(O, TEXT("animations"))) if (A->Type == EJson::Object)
        {
            const FString Animation = Str(A->AsObject(), TEXT("name")); F.AnimationNames.Add(FName(*Animation));
            if (Animation == Clip.Name)
            { double Speed; if (A->AsObject()->TryGetNumberField(TEXT("speed"), Speed) && FMath::IsFinite(Speed)) F.Speed = Speed; }
        }
        FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<>::Create(&F.Json)); Clip.Fragments.Add(MoveTemp(F));
    }
    Clip.bBoomerang = true;
    return true;
}
bool FSCAnimImporter::Bind(const FObj& Json, const FSCDerivedMesh& Rig, const FString& Path,
    USkeletalMesh* Mesh, UObject* Outer, FSCImportedClip& Clip, FString& Error)
{
    const FObj* Bones;
    if (!Json->TryGetObjectField(TEXT("bones"), Bones)) { Error = TEXT("Clip has no bones object"); return false; }
    FSCClipBinding Binding; Binding.Mesh = Mesh;
    for (int32 I = 0; I < Rig.RigNodes.Num(); ++I)
    {
        const auto& Node = Rig.RigNodes[I]; if (!Node.bAnimate) continue;
        const auto Entry = (*Bones)->TryGetField(FString::Printf(TEXT("0x%X"), BoneHash(Node.SourceName)));
        if (!Entry) continue;
        TArray<TSharedPtr<FJsonValue>> Variants;
        if (Entry->Type == EJson::Array) Variants = Entry->AsArray(); else Variants.Add(Entry);
        FObj Channel; int32 Best = MIN_int32; FString BestKey;
        for (const auto& V : Variants)
        {
            if (V->Type != EJson::Object) continue;
            const auto O = V->AsObject(); const FString SourcePath = Str(O, TEXT("source_skeleton_path")), Name = Str(O, TEXT("source_node_name"));
            const int32 Score = ProvenanceScore(Node.SourceName, Path, Name, SourcePath);
            const FString Key = SourcePath + TEXT("/") + Name;
            if (!Channel || Score > Best || (Score == Best && Key < BestKey)) { Channel = O; Best = Score; BestKey = Key; }
        }
        if (!Channel) continue;
        // A singleton hash may still belong to another entity's generic bone (Body, Root).
        // Exact basename is not required: CHR/SKIN naming and LOD suffixes differ.
        const FString SourcePath = Str(Channel, TEXT("source_skeleton_path"));
        Clip.Contributors.AddUnique(SourcePath);
        const auto& Rot = Arr(Channel, TEXT("rotation")); const auto& Pos = Arr(Channel, TEXT("position"));
        const auto& RT = Arr(Channel, TEXT("rotation_time")); const auto& PT = Arr(Channel, TEXT("position_time"));
        bool HR = !Rot.IsEmpty(), HP = !Pos.IsEmpty(); Channel->TryGetBoolField(TEXT("has_rotation"), HR); Channel->TryGetBoolField(TEXT("has_position"), HP);
        HR &= !Rot.IsEmpty(); HP &= !Pos.IsEmpty(); if (!HR && !HP) continue;
        auto Validate = [&](const auto& Values, const auto& Times, int32 Size)
        {
            if (!Times.IsEmpty() && Times.Num() != Values.Num()) return false;
            double Previous = -1;
            for (int32 K = 0; K < Values.Num(); ++K)
            {
                const TArray<TSharedPtr<FJsonValue>>* A;
                if (!Values[K]->TryGetArray(A) || A->Num() != Size) return false;
                double Norm = 0;
                for (const auto& X : *A) { double N; if (!X->TryGetNumber(N) || !FMath::IsFinite(N)) return false; Norm += N*N; }
                if (Size == 4 && Norm < SMALL_NUMBER) return false;
                double T = K; if (!Times.IsEmpty() && !Times[K]->TryGetNumber(T)) return false;
                if (!FMath::IsFinite(T) || T < Previous || T < 0) return false;
                Previous = T;
            }
            return true;
        };
        if (!Validate(Rot, RT, 4) || !Validate(Pos, PT, 3)) { Error = TEXT("Invalid channel values/times: ") + Node.SourceName; return false; }
        auto Interval = [](const auto& Values, const auto& Times, double Frame, int32& A, int32& B, double& Alpha)
        {
            auto Time = [&](int32 K) { return Times.IsEmpty() ? double(K) : Times[K]->AsNumber(); };
            B = 0; while (B + 1 < Values.Num() && Time(B) < Frame) ++B;
            A = FMath::Max(0, B - 1); const double Span = Time(B) - Time(A);
            Alpha = Span > 0 ? FMath::Clamp((Frame - Time(A)) / Span, 0., 1.) : 0.;
        };
        FSCBoneTrack Track; Track.Bone = I + 1; Track.bRotation = HR; Track.bPosition = HP;
        const FTransform Bind = Node.Local;
        const auto& Anchor = Arr(Json, TEXT("start_rotation"));
        const FQuat Start = SidecarRotation(Anchor.Num() == 4 ? Quat(Anchor) : HR ? Quat(Rot[0]->AsArray()) : FQuat::Identity);
        for (int32 Frame = 0; Frame <= Clip.LastFrame; ++Frame)
        {
            FTransform Key = Bind; int32 A, B; double Alpha;
            if (HR)
            {
                Interval(Rot, RT, Frame, A, B, Alpha);
                const FQuat Q = SidecarRotation(LinearRotation(Quat(Rot[A]->AsArray()), Quat(Rot[B]->AsArray()), Alpha));
                // A non-identity authored origin is anchor-relative (§14.6). Identity clip-root
                // metadata is not a per-bone bind: current exporter/default-pose and addon use absolute keys.
                Key.SetRotation(Anchor.Num() == 4 && !Start.Equals(FQuat::Identity, 1.e-6)
                    ? (Bind.GetRotation() * Start.Inverse() * Q).GetNormalized() : Q);
            }
            if (HP)
            {
                Interval(Pos, PT, Frame, A, B, Alpha);
                const FVector P = SidecarPosition(FMath::Lerp(Vec(Pos[A]->AsArray()), Vec(Pos[B]->AsArray()), Alpha));
                // Position keys are absolute parent-local coordinates (not clip-root translation).
                Key.SetTranslation(P);
            }
            if (!Track.Keys.IsEmpty() && (Track.Keys.Last().GetRotation() | Key.GetRotation()) < 0) Key.SetRotation(Key.GetRotation() * -1.);
            Track.Keys.Add(Key);
        }
        Clip.bBoomerang &= Track.Keys[0].GetRotation().AngularDistance(Track.Keys.Last().GetRotation()) < FMath::DegreesToRadians(0.1)
            && Track.Keys[0].GetTranslation().Equals(Track.Keys.Last().GetTranslation(), 0.01);
        Binding.Tracks.Add(MoveTemp(Track));
    }
    if (Binding.Tracks.IsEmpty()) return true;
    Binding.Sequence = NewObject<UAnimSequence>(Outer, NAME_None, RF_Transient);
    Binding.Sequence->SetSkeleton(Mesh->GetSkeleton()); Binding.Sequence->Interpolation = EAnimInterpolationType::Linear;
#if WITH_EDITOR
    auto& Controller = Binding.Sequence->GetController();
    Controller.OpenBracket(FText::FromString(TEXT("SC clip import")), false);
    Controller.InitializeModel(); Controller.SetFrameRate(FFrameRate(FMath::RoundToInt(Clip.Fps * 1000), 1000), false);
    Controller.SetNumberOfFrames(FFrameNumber(FMath::Max(1, Clip.LastFrame)), false);
    for (const auto& T : Binding.Tracks)
    {
        TArray<FVector3f> P, S; TArray<FQuat4f> R;
        for (const FTransform& K : T.Keys) { P.Add(FVector3f(K.GetTranslation())); R.Add(FQuat4f(K.GetRotation())); S.Add(FVector3f(K.GetScale3D())); }
        if (T.Keys.Num() == 1) { P.Add(FVector3f(P[0])); R.Add(FQuat4f(R[0])); S.Add(FVector3f(S[0])); }
        const FName Bone = Mesh->GetRefSkeleton().GetBoneName(T.Bone);
        Controller.AddBoneCurve(Bone, false); Controller.SetBoneTrackKeys(Bone, P, R, S, false);
    }
    Controller.NotifyPopulated(); Controller.CloseBracket(false);
#endif
    Clip.Bindings.Add(MoveTemp(Binding)); return true;
}
FTransform FSCAnimImporter::Sample(const FSCBoneTrack& Track, double Frame)
{
    if (Track.Keys.IsEmpty()) return FTransform::Identity;
    Frame = FMath::Clamp(Frame, 0., double(Track.Keys.Num()-1));
    const int32 A = FMath::FloorToInt(Frame), B = FMath::Min(A+1, Track.Keys.Num()-1); const double T = Frame-A;
    return FTransform(LinearRotation(Track.Keys[A].GetRotation(), Track.Keys[B].GetRotation(), T),
        FMath::Lerp(Track.Keys[A].GetTranslation(), Track.Keys[B].GetTranslation(), T),
        FMath::Lerp(Track.Keys[A].GetScale3D(), Track.Keys[B].GetScale3D(), T));
}

void FSCAnimImporter::Finalize(USkeletalMesh* RootMesh, UObject* Outer, FSCImportedClip& Clip)
{
    if (!Clip.Bindings.IsEmpty()) { Clip.Sequence = Clip.Bindings[0].Sequence; return; }
    Clip.bBoomerang = false;
    Clip.Sequence = NewObject<UAnimSequence>(Outer, NAME_None, RF_Transient);
    Clip.Sequence->SetSkeleton(RootMesh->GetSkeleton()); Clip.Sequence->Interpolation = EAnimInterpolationType::Linear;
#if WITH_EDITOR
    auto& Controller = Clip.Sequence->GetController(); Controller.OpenBracket(FText::FromString(TEXT("SC unbound clip metadata")),false);
    Controller.InitializeModel(); Controller.SetFrameRate(FFrameRate(FMath::RoundToInt(Clip.Fps*1000),1000),false);
    Controller.SetNumberOfFrames(FFrameNumber(FMath::Max(1,Clip.LastFrame)),false); Controller.NotifyPopulated(); Controller.CloseBracket(false);
#endif
}
