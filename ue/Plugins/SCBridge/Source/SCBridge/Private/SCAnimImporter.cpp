// Sidecar decoding, sparse channel routing, and transient animation sequence construction.
#include "SCAnimImporter.h"
#include "SCSkeletonBuilder.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#if WITH_EDITOR
#include "Animation/AnimData/IAnimationDataController.h"
#endif
namespace
{
using FObj = TSharedPtr<FJsonObject>;
const TArray<TSharedPtr<FJsonValue>>& Arr(const FObj& O, const TCHAR* Key)
{
    static TArray<TSharedPtr<FJsonValue>> Empty;
    const TArray<TSharedPtr<FJsonValue>>* A;
    return O && O->TryGetArrayField(Key, A) ? *A : Empty;
}
FString Str(const FObj& O, const TCHAR* Key)
{
    FString S;
    if (O) O->TryGetStringField(Key, S);
    return S;
}
FQuat Quat(const TArray<TSharedPtr<FJsonValue>>& A)
{
    return A.Num() == 4 ? FQuat(A[1]->AsNumber(), A[2]->AsNumber(), A[3]->AsNumber(), A[0]->AsNumber()).GetNormalized() : FQuat::Identity;
}
FVector Vec(const TArray<TSharedPtr<FJsonValue>>& A)
{
    return A.Num() == 3 ? FVector(A[0]->AsNumber(), A[1]->AsNumber(), A[2]->AsNumber()) : FVector::ZeroVector;
}
// An animation's rate and length, for the clip and each of its source animations: the rate a whole number of
// frames a second (contract), so it survives the track's float and the sequence's frame rate; both within
// the host's limits.
bool ValidTiming(double Fps, double Frames)
{
    return FMath::IsFinite(Fps) && Fps >= 1 && Fps == FMath::FloorToDouble(Fps) && Fps <= 1000 && FMath::IsFinite(Frames) &&
           Frames >= 0 && Frames <= 100000;
}
TSet<FString> Tokens(FString Path)
{
    Path = Path.ToLower();
    for (TCHAR& C : Path)
        if (!FChar::IsAlnum(C)) C = ' ';
    TArray<FString> Parts;
    Path.ParseIntoArrayWS(Parts);
    return TSet<FString>(Parts);
}
} // namespace
uint32 FSCAnimImporter::BoneHash(const FString& Name)
{
    // Standard CRC32 of case-sensitive UTF-8, not UE's TCHAR-based StrCrc32.
    FTCHARToUTF8 Bytes(*Name);
    uint32 CRC = ~0u;
    for (int32 I = 0; I < Bytes.Length(); ++I)
    {
        CRC ^= uint8(Bytes.Get()[I]);
        for (int32 Bit = 0; Bit < 8; ++Bit)
            CRC = (CRC >> 1) ^ ((CRC & 1) ? 0xedb88320u : 0);
    }
    return ~CRC;
}
FVector FSCAnimImporter::SidecarPosition(const FVector& V)
{
    // Undo exporter (x,y,z)->(x,-z,y), then GLB G=(-z,x,y), metres to cm.
    return FVector(V.Y, V.X, V.Z) * 100.;
}
FQuat FSCAnimImporter::SidecarRotation(const FQuat& Q)
{
    return FQuat(-Q.Y, -Q.X, -Q.Z, Q.W).GetNormalized();
}

FQuat FSCAnimImporter::LinearRotation(FQuat A, FQuat B, double Alpha)
{
    if ((A | B) < 0) B = B * -1.;
    return (A * (1 - Alpha) + B * Alpha).GetNormalized();
}
int32 FSCAnimImporter::ProvenanceScore(const FString& ReceiverName, const FString& ReceiverPath, const FString& SourceName,
                                       const FString& SourcePath)
{
    int32 Score = ReceiverName.Equals(SourceName, ESearchCase::IgnoreCase) ? 1000 : 0;
    const auto A = Tokens(ReceiverPath), B = Tokens(SourcePath);
    for (const auto& T : A)
        if (B.Contains(T)) Score += 4;
    for (const FString D : {TEXT("front"), TEXT("rear"), TEXT("back"), TEXT("left"), TEXT("right")})
        if (A.Contains(D) != B.Contains(D)) Score -= 20;
    return Score;
}
bool FSCAnimImporter::Read(const FSCPackageSource& Source, const FSCClipRecord& Record, const FString& Owner, FSCImportedClip& Clip,
                           FObj& Json, FString& Error)
{
    TArray<uint8> Bytes;
    if (!Source.ReadBytes(Record.Sidecar, Bytes, Error)) return false;
    const FString Text(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num()));
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json)
    {
        Error = TEXT("Invalid clip JSON");
        return false;
    }
    double Fps = 0, Frames = 0;
    if (!Json->TryGetNumberField(TEXT("fps"), Fps) || !Json->TryGetNumberField(TEXT("frame_count"), Frames) || !ValidTiming(Fps, Frames))
    {
        Error = TEXT("Invalid clip frame rate/length (a whole rate; host limits: 1000 fps, 100000 frames)");
        return false;
    }
    Clip.Id = FName(*Record.Name);
    Clip.Name = Record.Name;
    Clip.DisplayName = Str(Json, TEXT("display_name"));
    if (Clip.DisplayName.IsEmpty()) Clip.DisplayName = Clip.Name;
    Clip.Owner = Owner;
    Json->TryGetStringField(TEXT("owner_entity"), Clip.Owner);
    Clip.Source = Record.Sidecar;
    Clip.Fps = Fps;
    Clip.LastFrame = FMath::CeilToInt(Frames);
    for (const auto& V : Arr(Json, TEXT("fragments")))
    {
        if (V->Type != EJson::Object) continue;
        const auto O = V->AsObject();
        FSCClipFragment F;
        F.Fragment = FName(*Str(O, TEXT("fragment")));
        auto Names = [&](const TCHAR* Key, TArray<FName>& Dest) {
            for (const auto& X : Arr(O, Key))
                if (X->Type == EJson::String) Dest.Add(FName(*X->AsString()));
        };
        F.OptionId = Str(O, TEXT("guid"));
        double Weight = 1;
        if (O->TryGetNumberField(TEXT("option_weight"), Weight) && FMath::IsFinite(Weight)) F.OptionWeight = FMath::Max(0., Weight);
        Names(TEXT("tags"), F.Tags);
        Names(TEXT("frag_tags"), F.FragmentTags);
        Names(TEXT("scopes"), F.Scopes);
        for (const auto& A : Arr(O, TEXT("animations")))
            if (A->Type == EJson::Object)
            {
                const FString Animation = Str(A->AsObject(), TEXT("name"));
                F.AnimationNames.Add(FName(*Animation));
                if (Animation == Clip.Name)
                {
                    double Speed;
                    if (A->AsObject()->TryGetNumberField(TEXT("speed"), Speed) && FMath::IsFinite(Speed)) F.Speed = Speed;
                }
            }
        FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<>::Create(&F.Json));
        Clip.Fragments.Add(MoveTemp(F));
    }
    // Cyclic clips return all moving channels to their first sample. Do not infer from names.
    Clip.bCyclic = true;
    bool Moving = false;
    const FObj* Bones;
    if (Json->TryGetObjectField(TEXT("bones"), Bones))
        for (const auto& Pair : (*Bones)->Values)
        {
            TArray<TSharedPtr<FJsonValue>> Channels;
            if (Pair.Value->Type == EJson::Array)
                Channels = Pair.Value->AsArray();
            else
                Channels.Add(Pair.Value);
            for (const auto& V : Channels)
            {
                if (V->Type != EJson::Object) continue;
                const auto& R = Arr(V->AsObject(), TEXT("rotation"));
                const auto& P = Arr(V->AsObject(), TEXT("position"));
                if (R.Num() > 1)
                {
                    const FQuat First = Quat(R[0]->AsArray());
                    Clip.bCyclic &= First.AngularDistance(Quat(R.Last()->AsArray())) < FMath::DegreesToRadians(0.1);
                    for (const auto& K : R)
                        Moving |= First.AngularDistance(Quat(K->AsArray())) > FMath::DegreesToRadians(0.1);
                }
                if (P.Num() > 1)
                {
                    const FVector First = Vec(P[0]->AsArray());
                    Clip.bCyclic &= First.Equals(Vec(P.Last()->AsArray()), 0.0001);
                    for (const auto& K : P)
                        Moving |= !First.Equals(Vec(K->AsArray()), 0.0001);
                }
            }
        }
    Clip.bCyclic &= Moving;
    Clip.bBoomerang = true;
    return true;
}
bool FSCAnimImporter::DeriveBinding(const FObj& Json, const FSCDerivedMesh& Rig, const FString& Path, FSCImportedClip& Clip,
                                    FSCClipBinding& Binding, FString& Error)
{
    Binding = FSCClipBinding();
    const FObj* Bones;
    if (!Json->TryGetObjectField(TEXT("bones"), Bones))
    {
        Error = TEXT("Clip has no bones object");
        return false;
    }
    // A clip merged by name lists every contributing animation. Clips without the list have one animation, timed by
    // the clip itself; a list of any other shape is a broken clip, not a clip without one.
    const TSharedPtr<FJsonValue> SourceList = Json->TryGetField(TEXT("sources"));
    if (SourceList && SourceList->Type != EJson::Array)
    {
        Error = TEXT("Clip source animation list is not an array");
        return false;
    }
    for (int32 I = 0; I < Rig.RigNodes.Num(); ++I)
    {
        const auto& Node = Rig.RigNodes[I];
        if (!Node.bAnimate) continue;
        const auto Entry = (*Bones)->TryGetField(FString::Printf(TEXT("0x%X"), BoneHash(Node.SourceName)));
        if (!Entry) continue;
        TArray<TSharedPtr<FJsonValue>> Variants;
        if (Entry->Type == EJson::Array)
            Variants = Entry->AsArray();
        else
            Variants.Add(Entry);
        FObj Channel;
        int32 Best = MIN_int32;
        FString BestKey;
        for (const auto& V : Variants)
        {
            if (V->Type != EJson::Object) continue;
            const auto O = V->AsObject();
            const FString SourcePath = Str(O, TEXT("source_skeleton_path")), Name = Str(O, TEXT("source_node_name"));
            const int32 Score = ProvenanceScore(Node.SourceName, Path, Name, SourcePath);
            const FString Key = SourcePath + TEXT("/") + Name;
            if (!Channel || Score > Best || (Score == Best && Key < BestKey))
            {
                Channel = O;
                Best = Score;
                BestKey = Key;
            }
        }
        if (!Channel) continue;
        // A singleton hash may still belong to another entity's generic bone (Body, Root).
        // Exact basename is not required: CHR/SKIN naming and LOD suffixes differ.
        const FString SourcePath = Str(Channel, TEXT("source_skeleton_path"));
        Clip.Contributors.AddUnique(SourcePath);
        const auto& Rot = Arr(Channel, TEXT("rotation"));
        const auto& Pos = Arr(Channel, TEXT("position"));
        const auto& RT = Arr(Channel, TEXT("rotation_time"));
        const auto& PT = Arr(Channel, TEXT("position_time"));
        bool HR = !Rot.IsEmpty(), HP = !Pos.IsEmpty();
        Channel->TryGetBoolField(TEXT("has_rotation"), HR);
        Channel->TryGetBoolField(TEXT("has_position"), HP);
        HR &= !Rot.IsEmpty();
        HP &= !Pos.IsEmpty();
        if (!HR && !HP) continue;
        auto Validate = [&](const auto& Values, const auto& Times, int32 Size) {
            if (!Times.IsEmpty() && Times.Num() != Values.Num()) return false;
            double Previous = -1;
            for (int32 K = 0; K < Values.Num(); ++K)
            {
                const TArray<TSharedPtr<FJsonValue>>* A;
                if (!Values[K]->TryGetArray(A) || A->Num() != Size) return false;
                double Norm = 0;
                for (const auto& X : *A)
                {
                    double N;
                    if (!X->TryGetNumber(N) || !FMath::IsFinite(N)) return false;
                    Norm += N * N;
                }
                if (Size == 4 && Norm < SMALL_NUMBER) return false;
                double T = K;
                if (!Times.IsEmpty() && !Times[K]->TryGetNumber(T)) return false;
                if (!FMath::IsFinite(T) || T < Previous || T < 0) return false;
                Previous = T;
            }
            return true;
        };
        if (!Validate(Rot, RT, 4) || !Validate(Pos, PT, 3))
        {
            Error = TEXT("Invalid channel values/times: ") + Node.SourceName;
            return false;
        }
        auto Interval = [](const auto& Values, const auto& Times, double Frame, int32& A, int32& B, double& Alpha) {
            auto Time = [&](int32 K) { return Times.IsEmpty() ? double(K) : Times[K]->AsNumber(); };
            B = 0;
            while (B + 1 < Values.Num() && Time(B) < Frame)
                ++B;
            A = FMath::Max(0, B - 1);
            const double Span = Time(B) - Time(A);
            Alpha = Span > 0 ? FMath::Clamp((Frame - Time(A)) / Span, 0., 1.) : 0.;
        };
        FSCBoneTrack Track;
        Track.Bone = I + 1;
        Track.bRotation = HR;
        Track.bPosition = HP;
        Track.Fps = Clip.Fps;
        // The channel names its own animation in the clip's list, whose rate, length and start pose time and
        // anchor this track.
        FObj Timing = Json;
        int32 OwnLast = Clip.LastFrame;
        if (SourceList)
        {
            const auto& Sources = SourceList->AsArray();
            // The index is a JSON number that must be a whole, listed position: no rounding, no numeric strings.
            const TSharedPtr<FJsonValue> Named = Channel->TryGetField(TEXT("source"));
            const double Index = Named && Named->Type == EJson::Number ? Named->AsNumber() : -1.;
            double Fps = 0, Frames = -1;
            if (!FMath::IsFinite(Index) || Index < 0 || Index >= Sources.Num() || Index != FMath::FloorToDouble(Index) ||
                Sources[int32(Index)]->Type != EJson::Object)
            {
                Error = TEXT("Channel names no listed source animation: ") + Node.SourceName;
                return false;
            }
            Timing = Sources[int32(Index)]->AsObject();
            if (!Timing->TryGetNumberField(TEXT("fps"), Fps) || !Timing->TryGetNumberField(TEXT("frame_count"), Frames) ||
                !ValidTiming(Fps, Frames))
            {
                Error = TEXT("Invalid source animation rate/length: ") + Node.SourceName;
                return false;
            }
            Track.Fps = Fps;
            OwnLast = FMath::CeilToInt(Frames);
        }
        // An animation holds every key of its channels, and the clip every source animation in full: anything
        // else would cut authored keys off or stop a track short of its end.
        auto LastTime = [](const auto& Values, const auto& Times) {
            return Values.IsEmpty() ? 0. : Times.IsEmpty() ? double(Values.Num() - 1) : Times.Last()->AsNumber();
        };
        if (FMath::Max(LastTime(Rot, RT), LastTime(Pos, PT)) > OwnLast || OwnLast * double(Clip.Fps) > Clip.LastFrame * double(Track.Fps))
        {
            Error = TEXT("Channel keys past their animation's length, or an animation longer than its clip: ") + Node.SourceName;
            return false;
        }
        const FTransform Bind = Node.Local;
        const auto& Anchor = Arr(Timing, TEXT("start_rotation"));
        const FQuat Start = SidecarRotation(Anchor.Num() == 4 ? Quat(Anchor) : HR ? Quat(Rot[0]->AsArray()) : FQuat::Identity);
        for (int32 Frame = 0; Frame <= OwnLast; ++Frame)
        {
            FTransform Key = Bind;
            int32 A, B;
            double Alpha;
            if (HR)
            {
                Interval(Rot, RT, Frame, A, B, Alpha);
                const FQuat Q = SidecarRotation(LinearRotation(Quat(Rot[A]->AsArray()), Quat(Rot[B]->AsArray()), Alpha));
                // A non-identity authored origin is anchor-relative (§14.6). Identity clip-root
                // metadata is not a per-bone bind: current exporter/default-pose and addon use absolute keys.
                Key.SetRotation(Anchor.Num() == 4 && !Start.Equals(FQuat::Identity, 1.e-6)
                                    ? (Bind.GetRotation() * Start.Inverse() * Q).GetNormalized()
                                    : Q);
            }
            if (HP)
            {
                Interval(Pos, PT, Frame, A, B, Alpha);
                const FVector P = SidecarPosition(FMath::Lerp(Vec(Pos[A]->AsArray()), Vec(Pos[B]->AsArray()), Alpha));
                // Cyclic motion is a delta about this receiver's bind; other clips are absolute locals.
                Key.SetTranslation(Clip.bCyclic ? Bind.GetTranslation() + P - SidecarPosition(Vec(Pos[0]->AsArray())) : P);
            }
            if (!Track.Keys.IsEmpty() && (Track.Keys.Last().GetRotation() | Key.GetRotation()) < 0)
                Key.SetRotation(Key.GetRotation() * -1.);
            Track.Keys.Add(Key);
        }
        Clip.bBoomerang &= Track.Keys[0].GetRotation().AngularDistance(Track.Keys.Last().GetRotation()) < FMath::DegreesToRadians(0.1) &&
                           Track.Keys[0].GetTranslation().Equals(Track.Keys.Last().GetTranslation(), 0.01);
        Binding.Tracks.Add(MoveTemp(Track));
    }
    return true;
}
bool FSCAnimImporter::Bind(const FObj& Json, const FSCDerivedMesh& Rig, const FString& Path, USkeletalMesh* Mesh, UObject* Outer,
                           FSCImportedClip& Clip, FString& Error)
{
    FSCClipBinding Binding;
    if (!DeriveBinding(Json, Rig, Path, Clip, Binding, Error)) return false;
    if (!Binding.Tracks.IsEmpty()) AttachBinding(Mesh, Outer, Clip, MoveTemp(Binding));
    return true;
}
void FSCAnimImporter::AttachBinding(USkeletalMesh* Mesh, UObject* Outer, FSCImportedClip& Clip, FSCClipBinding Binding)
{
    Binding.Mesh = Mesh;
    // A sequence has one rate and one length; the tracks of a merged clip keep their own animation's. Tracks that
    // share a timing share a sequence, so every key is stored as authored and each sequence can be played from its
    // own end, as the runtime plays its tracks.
    TArray<TPair<float, int32>> Timings;
    for (const auto& T : Binding.Tracks)
        Timings.AddUnique(TPair<float, int32>(T.Fps, T.Keys.Num()));
    for (const auto& Timing : Timings)
    {
        UAnimSequence* Sequence = Binding.Sequences.Add_GetRef(NewObject<UAnimSequence>(Outer, NAME_None, RF_Transient));
        Sequence->SetSkeleton(Mesh->GetSkeleton());
        Sequence->Interpolation = EAnimInterpolationType::Linear;
#if WITH_EDITOR
        auto& Controller = Sequence->GetController();
        Controller.OpenBracket(FText::FromString(TEXT("SC clip import")), false);
        Controller.InitializeModel();
        Controller.SetFrameRate(FFrameRate(FMath::RoundToInt(Timing.Key * 1000), 1000), false);
        Controller.SetNumberOfFrames(FFrameNumber(FMath::Max(1, Timing.Value - 1)), false);
        for (const auto& T : Binding.Tracks)
        {
            if (T.Fps != Timing.Key || T.Keys.Num() != Timing.Value) continue;
            TArray<FVector3f> P, S;
            TArray<FQuat4f> R;
            for (const FTransform& K : T.Keys)
            {
                P.Add(FVector3f(K.GetTranslation()));
                R.Add(FQuat4f(K.GetRotation()));
                S.Add(FVector3f(K.GetScale3D()));
            }
            if (P.Num() == 1) // a sequence has at least one frame: a still pose is held across it
            {
                P.Add(FVector3f(P[0]));
                R.Add(FQuat4f(R[0]));
                S.Add(FVector3f(S[0]));
            }
            const FName Bone = Mesh->GetRefSkeleton().GetBoneName(T.Bone);
            Controller.AddBoneCurve(Bone, false);
            Controller.SetBoneTrackKeys(Bone, P, R, S, false);
        }
        Controller.NotifyPopulated();
        Controller.CloseBracket(false);
#endif
    }
    Clip.Bindings.Add(MoveTemp(Binding));
}
FTransform FSCAnimImporter::Sample(const FSCBoneTrack& Track, double Frame)
{
    if (Track.Keys.IsEmpty()) return FTransform::Identity;
    Frame = FMath::Clamp(Frame, 0., double(Track.Keys.Num() - 1));
    const int32 A = FMath::FloorToInt(Frame), B = FMath::Min(A + 1, Track.Keys.Num() - 1);
    const double T = Frame - A;
    return FTransform(LinearRotation(Track.Keys[A].GetRotation(), Track.Keys[B].GetRotation(), T),
                      FMath::Lerp(Track.Keys[A].GetTranslation(), Track.Keys[B].GetTranslation(), T),
                      FMath::Lerp(Track.Keys[A].GetScale3D(), Track.Keys[B].GetScale3D(), T));
}
double FSCAnimImporter::TrackFrame(const FSCImportedClip& Clip, const FSCBoneTrack& Track, double ClipFrame, bool bFromEnd)
{
    checkf(Clip.Fps > 0 && Track.Fps > 0, TEXT("clip and track rates are set by the importer"));
    const double Last = FMath::Max(0, Track.Keys.Num() - 1);
    const double Scale = Track.Fps / Clip.Fps;
    return bFromEnd ? FMath::Max(0., Last - (Clip.LastFrame - ClipFrame) * Scale) : FMath::Min(Last, ClipFrame * Scale);
}

void FSCAnimImporter::Finalize(USkeletalMesh* RootMesh, UObject* Outer, FSCImportedClip& Clip)
{
    if (!Clip.Bindings.IsEmpty())
    {
        Clip.Sequence = Clip.Bindings[0].Sequences[0];
        return;
    }
    Clip.bBoomerang = false;
    Clip.Sequence = NewObject<UAnimSequence>(Outer, NAME_None, RF_Transient);
    Clip.Sequence->SetSkeleton(RootMesh ? RootMesh->GetSkeleton() : NewObject<USkeleton>(Outer, NAME_None, RF_Transient));
    Clip.Sequence->Interpolation = EAnimInterpolationType::Linear;
#if WITH_EDITOR
    auto& Controller = Clip.Sequence->GetController();
    Controller.OpenBracket(FText::FromString(TEXT("SC unbound clip metadata")), false);
    Controller.InitializeModel();
    Controller.SetFrameRate(FFrameRate(FMath::RoundToInt(Clip.Fps * 1000), 1000), false);
    Controller.SetNumberOfFrames(FFrameNumber(FMath::Max(1, Clip.LastFrame)), false);
    Controller.NotifyPopulated();
    Controller.CloseBracket(false);
#endif
}
