// Vehicle parameters and on-demand evaluation of sparse, ordered animation layers.
#include "SCVehicle.h"
#include "SCAudioSystem.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "SCMaterialBuilder.h"
#include "SCAnimImporter.h"
#include "SCLightImporter.h"
#include "SCRigComponent.h"
#include "Materials/MaterialInterface.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Components/StaticMeshComponent.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "PhysicsEngine/BodySetup.h"
#include "SCMeshImporter.h"
#include "Engine/SkeletalMesh.h"
ASCVehicle::ASCVehicle()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("VehicleRoot"));
    RootComponent->SetMobility(EComponentMobility::Movable);
}
bool ASCVehicle::PortRelative(const FString& Id, const FString& Helper, const FMatrix& Offset, FTransform& Out) const
{
    const FSCInstanceHelpers* Helpers = InstanceHelpers.Find(Id);
    const FTransform* Frame = Helpers ? Helpers->Frames.Find(Helper) : nullptr;
    if (!Frame) return false;
    // The helper (anchor space) lands on the joint frame plus the port offset; offset axes are the joint's own.
    Out = Frame->Inverse() * FTransform(FSCMeshImporter::Transform(Offset));
    return true;
}
void ASCVehicle::SetEngineGlow(float Strength)
{
    const float Value = FSCMaterialBuilder::EngineGlowToEmissive(Manifest.EngineGlow, Strength);
    for (UMaterialInstanceDynamic* MID : EngineGlowMaterials)
        if (MID)
        {
            MID->SetScalarParameterValue(TEXT("EmissiveStrength"), Value);
            float ReadBack = -1.f;
            MID->GetScalarParameterValue(FName(TEXT("EmissiveStrength")), ReadBack);
            UE_LOG(LogTemp, Display, TEXT("ENGINE_GLOW control=%g emissive=%g instance=%s parent=%s readback=%g"), Strength, Value,
                   *MID->GetName(), MID->Parent ? *MID->Parent->GetName() : TEXT("none"), ReadBack);
        }
}

void ASCVehicle::MarkDirty(int32 ClipIndex)
{
    for (const auto& Binding : ClipCatalog->Clips[ClipIndex].Bindings)
        DirtyMeshes.Add(Binding.Mesh);
}
double ASCVehicle::ClipPoseCost(const FSCImportedClip& Clip, const FSCClipBinding& Binding, const TArray<FTransform>& Local, double Frame, bool bBackward)
{
    double Cost = 0;
    for (const FSCBoneTrack& T : Binding.Tracks)
    {
        if (!Local.IsValidIndex(T.Bone)) continue;
        const FTransform S = FSCAnimImporter::Sample(T, FSCAnimImporter::TrackFrame(Clip, T, Frame, bBackward));
        if (T.bRotation) Cost += Local[T.Bone].GetRotation().AngularDistance(S.GetRotation());
        if (T.bPosition) Cost += FVector::Dist(Local[T.Bone].GetTranslation(), S.GetTranslation()) / 100.0;
    }
    return Cost;
}
double ASCVehicle::NearestClipFrame(const FSCImportedClip& Clip, const FSCClipBinding& Binding, const TArray<FTransform>& Local, bool bBackward)
{
    double Best = bBackward ? Clip.LastFrame : 0, BestCost = TNumericLimits<double>::Max();
    for (int32 Step = 0; Step <= Clip.LastFrame; ++Step)
    {
        const int32 Frame = bBackward ? Clip.LastFrame - Step : Step;
        const double Cost = ClipPoseCost(Clip, Binding, Local, Frame, bBackward);
        if (Cost < BestCost - 1e-9)
        {
            BestCost = Cost;
            Best = Frame;
        }
    }
    // Between samples: the playhead is fractional (a door stopped mid-key), so refine around the best key frame in tenths.
    const double Centre = Best;
    for (int32 Step = -9; Step <= 9; ++Step)
    {
        const double Frame = Centre + Step * 0.1;
        if (Step == 0 || Frame < 0 || Frame > Clip.LastFrame) continue;
        const double Cost = ClipPoseCost(Clip, Binding, Local, Frame, bBackward);
        if (Cost < BestCost - 1e-9)
        {
            BestCost = Cost;
            Best = Frame;
        }
    }
    return Best;
}
bool ASCVehicle::AppliesTo(const FSCClipPlayback& P, const USCRigComponent* Rig)
{
    // A targeted entry poses its own rig only (none once that rig is gone); an untargeted one every rig its clip binds.
    return P.bTargeted ? (Rig && P.TargetRig.Get() == Rig) : true;
}
bool ASCVehicle::IsCoveredOnRig(const FSCClipPlayback& Entry, const USCRigComponent* Rig) const
{
    // EvaluateClips layers entries by Order: Entry shows nothing on Rig when later entries there drive every channel it drives.
    const USkeletalMesh* Mesh = Rig ? Rig->GetSkeletalMeshAsset() : nullptr;
    if (!Mesh || !ClipCatalog || !ClipCatalog->Clips.IsValidIndex(Entry.Clip)) return false;
    auto BindingOf = [&](int32 Clip) {
        return ClipCatalog->Clips.IsValidIndex(Clip) ? ClipCatalog->Clips[Clip].Bindings.FindByPredicate([&](const FSCClipBinding& B) { return B.Mesh == Mesh; })
                                                     : nullptr;
    };
    const FSCClipBinding* Own = BindingOf(Entry.Clip);
    if (!Own) return false;
    TArray<const FSCClipBinding*> Later;
    for (const TArray<FSCClipPlayback>* List : {&Playback, &HeldPlayback})
        for (const FSCClipPlayback& Other : *List)
            if (Other.Order > Entry.Order && AppliesTo(Other, Rig))
                if (const FSCClipBinding* B = BindingOf(Other.Clip)) Later.Add(B);
    auto Covered = [&](int32 Bone, bool bRotation) {
        return Later.ContainsByPredicate([&](const FSCClipBinding* B) {
            return B->Tracks.ContainsByPredicate([&](const FSCBoneTrack& T) { return T.Bone == Bone && (bRotation ? T.bRotation : T.bPosition); });
        });
    };
    for (const FSCBoneTrack& T : Own->Tracks)
        if ((T.bRotation && !Covered(T.Bone, true)) || (T.bPosition && !Covered(T.Bone, false))) return false;
    return true;
}
bool ASCVehicle::StartClip(int32 Index, float Speed, bool bSnap, bool bLast, const TArray<FName>& Scopes, USCRigComponent* Target, double StartFrame)
{
    if (!ClipCatalog || !ClipCatalog->Clips.IsValidIndex(Index) || !FMath::IsFinite(Speed)) return false;
    const auto& Clip = ClipCatalog->Clips[Index];
    if (Clip.Bindings.IsEmpty()) return false;
    // The same clip on the same target is replaced; targeted entries whose rig is gone are dropped.
    auto Replaced = [&](const FSCClipPlayback& P) {
        return (P.bTargeted && !P.TargetRig.IsValid()) || (P.Clip == Index && P.bTargeted == (Target != nullptr) && P.TargetRig.Get() == Target);
    };
    Playback.RemoveAll(Replaced);
    HeldPlayback.RemoveAll(Replaced);
    FSCClipPlayback P;
    P.Clip = Index;
    P.TargetRig = Target;
    P.bTargeted = Target != nullptr;
    P.Speed = Speed;
    P.Frame = StartFrame >= 0 && !bSnap ? FMath::Clamp(StartFrame, 0.0, double(Clip.LastFrame)) : (bSnap ? bLast : Speed < 0) ? Clip.LastFrame : 0;
    P.bPlaying = !bSnap && Speed != 0 && Clip.LastFrame > 0;
    P.Order = ++NextOrder;
    P.Scopes = Scopes;
    (P.bPlaying ? Playback : HeldPlayback).Add(MoveTemp(P));
    MarkDirty(Index);
    EvaluateClips();
    UpdateTickEnabled();
    return true;
}
void ASCVehicle::ReconcileDoorStates(int32 Index, const USCRigComponent* Target, bool bEndsAtLast)
{
    // A clip started outside a door interaction (sc.Vehicle.Clip, a harness, an environment state) on a door's channels: the
    // door's recorded state is what that clip ends in when it is one of the door's own fragments (an open fragment's end is
    // open, its start closed; a close fragment the reverse; a single-procedural transition's end open, its start closed),
    // otherwise unknown (no state: IsDoorOpen reads closed, so a press, a pad or a sensor opens it from the pose it shows).
    if (!ClipCatalog || !ClipCatalog->Clips.IsValidIndex(Index)) return;
    const FSCImportedClip& Clip = ClipCatalog->Clips[Index];
    auto HasTag = [](const FSCClipFragment& F, FName Tag) { return !Tag.IsNone() && (F.FragmentTags.Contains(Tag) || F.Tags.Contains(Tag)); };
    bool bChanged = false;
    for (const FSCDoorInteraction& D : Doors)
    {
        USCRigComponent* Rig = D.Rig;
        if (!Rig || (Target && Rig != Target)) continue;
        const USkeletalMesh* Mesh = Rig->GetSkeletalMeshAsset();
        const FSCClipBinding* Raw = Clip.Bindings.FindByPredicate([&](const FSCClipBinding& B) { return B.Mesh == Mesh; });
        if (!Raw) continue;
        const bool bOpenClip = Clip.Fragments.ContainsByPredicate([&](const FSCClipFragment& F) { return F.Fragment == D.Fragment && HasTag(F, D.OpenTag); });
        const bool bCloseClip = !D.bReverseClose &&
                                Clip.Fragments.ContainsByPredicate([&](const FSCClipFragment& F) { return F.Fragment == D.Fragment && HasTag(F, D.CloseTag); });
        // Another clip: does it drive a channel the door's own fragments drive?
        bool bShares = bOpenClip || bCloseClip;
        for (int32 C = 0; !bShares && C < ClipCatalog->Clips.Num(); ++C)
        {
            const FSCImportedClip& Own = ClipCatalog->Clips[C];
            if (!Own.Fragments.ContainsByPredicate([&](const FSCClipFragment& F) { return F.Fragment == D.Fragment; })) continue;
            const FSCClipBinding* B = Own.Bindings.FindByPredicate([&](const FSCClipBinding& X) { return X.Mesh == Mesh; });
            bShares = B && B->Tracks.ContainsByPredicate([&](const FSCBoneTrack& T) {
                return Raw->Tracks.ContainsByPredicate([&](const FSCBoneTrack& R) { return R.Bone == T.Bone && ((R.bRotation && T.bRotation) || (R.bPosition && T.bPosition)); });
            });
        }
        if (!bShares) continue;
        const TPair<const USCRigComponent*, FName> Key(Rig, D.Fragment);
        if (bOpenClip || bCloseClip) FragmentTargets.Add(Key, (bOpenClip == bEndsAtLast) ? D.OpenTag : D.CloseTag);
        else FragmentTargets.Remove(Key);
        bChanged = true;
        UE_LOG(LogTemp, Display, TEXT("DOOR_STATE_RAW_CLIP owner=%s fragment=%s clip=%s ends_at_last=%d state=%s"), *Rig->OwnerEntity, *D.Fragment.ToString(),
               *Clip.Id.ToString(), bEndsAtLast, bOpenClip || bCloseClip ? *FragmentTargets[Key].ToString() : TEXT("unknown"));
    }
    if (bChanged && !ScreenFaces.IsEmpty()) RefreshPanelScreens();
}
bool ASCVehicle::PlayClip(FName Id)
{
    if (!ClipCatalog) return false;
    const int32 Index = ClipCatalog->Clips.IndexOfByPredicate([&](const FSCImportedClip& C) { return C.Id == Id; });
    if (!StartClip(Index, 1, false, false)) return false;
    ReconcileDoorStates(Index, nullptr, true);
    return true;
}
bool ASCVehicle::SnapClip(FName Id, bool bLast)
{
    if (!ClipCatalog) return false;
    const int32 Index = ClipCatalog->Clips.IndexOfByPredicate([&](const FSCImportedClip& C) { return C.Id == Id; });
    if (!StartClip(Index, 1, true, bLast)) return false;
    ReconcileDoorStates(Index, nullptr, bLast);
    return true;
}
bool ASCVehicle::PlayClipAt(int32 Index, USCRigComponent* Target, float Speed)
{
    if (!StartClip(Index, Speed, false, false, {}, Target)) return false;
    ReconcileDoorStates(Index, Target, Speed >= 0);
    return true;
}
bool ASCVehicle::SnapClipAt(int32 Index, USCRigComponent* Target, bool bLast)
{
    if (!StartClip(Index, 1, true, bLast, {}, Target)) return false;
    ReconcileDoorStates(Index, Target, bLast);
    return true;
}
bool ASCVehicle::PlayFragment(FName Fragment, FName Tag)
{
    return PlayFragmentAt(Fragment, Tag, nullptr);
}
bool ASCVehicle::PlayFragmentAt(FName Fragment, FName Tag, USCRigComponent* Target, float SpeedScale, ESCFragmentStart Start)
{
    if (!ClipCatalog) return false;
    struct FOption
    {
        FString Key;
        float Weight = 1;
        TArray<FName> Scopes;
        TMap<int32, float> Clips;
    };
    TArray<FOption> Options;
    const uint64 OrderBefore = NextOrder;
    auto Matches = [&](const FSCClipFragment& F) {
        return F.Fragment == Fragment && F.OptionWeight > 0 && (Tag.IsNone() || F.Tags.Contains(Tag) || F.FragmentTags.Contains(Tag));
    };
    // Geometries that share bone names bind each other's clips (the C2's hold and top lift doors): a rig plays the clips
    // authored for its own geometry (the clip's owner) when it has any, never another geometry's at random.
    auto OwnPath = [](FString Path) {
        Path.ReplaceInline(TEXT("\\"), TEXT("/"));
        Path.RemoveFromStart(TEXT("Data/"), ESearchCase::IgnoreCase);
        return Path;
    };
    const FString TargetOwner = Target ? OwnPath(Target->OwnerEntity) : FString();
    auto OwnClip = [&](const FSCImportedClip& C) { return !TargetOwner.IsEmpty() && OwnPath(C.Owner).Equals(TargetOwner, ESearchCase::IgnoreCase); };
    auto Binds = [&](const FSCImportedClip& C) {
        return !C.Bindings.IsEmpty() &&
               (!Target || C.Bindings.ContainsByPredicate([&](const FSCClipBinding& B) { return B.Mesh == Target->GetSkeletalMeshAsset(); }));
    };
    // An alias keeps metadata but plays its canonical animation; only what would play counts for the preference.
    auto Alias = [&](const FSCImportedClip& C, const FSCClipFragment& F) {
        return !F.AnimationNames.Contains(C.Id) && F.AnimationNames.ContainsByPredicate([&](FName Id) {
            return ClipCatalog->Clips.ContainsByPredicate([&](const FSCImportedClip& Other) { return Other.Id == Id; });
        });
    };
    const bool bOwnClips = ClipCatalog->Clips.ContainsByPredicate([&](const FSCImportedClip& C) {
        return Binds(C) && OwnClip(C) && C.Fragments.ContainsByPredicate([&](const FSCClipFragment& F) { return Matches(F) && !Alias(C, F); });
    });
    for (int32 Index = 0; Index < ClipCatalog->Clips.Num(); ++Index)
    {
        const auto& C = ClipCatalog->Clips[Index];
        if (!Binds(C) || (bOwnClips && !OwnClip(C))) continue;
        for (const auto& F : C.Fragments)
        {
            if (!Matches(F)) continue;
            // Aliases keep metadata, but play the canonical animation only once.
            if (Alias(C, F)) continue;
            const FString Key = F.OptionId.IsEmpty() ? F.Json : F.OptionId;
            FOption* Option = Options.FindByPredicate([&](const FOption& O) { return O.Key == Key; });
            if (!Option)
            {
                FOption NewOption;
                NewOption.Key = Key;
                NewOption.Weight = F.OptionWeight;
                NewOption.Scopes = F.Scopes;
                Option = &Options.Add_GetRef(MoveTemp(NewOption));
            }
            Option->Clips.Add(Index, F.Speed * SpeedScale);
        }
    }
    // Weighted alternatives compete for scopes; disjoint scope groups can run together.
    // A reproducible actor-local random stream makes option selection testable.
    auto Conflicts = [](const TArray<FName>& A, const TArray<FName>& B) {
        return A.IsEmpty() || B.IsEmpty() || A.ContainsByPredicate([&](FName Scope) { return B.Contains(Scope); });
    };
    bool Found = false;
    while (!Options.IsEmpty())
    {
        Options.Sort([](const FOption& A, const FOption& B) { return A.Key < B.Key; });
        double Total = 0;
        for (const auto& O : Options)
            Total += O.Weight;
        double Draw = FragmentRandom.FRand() * Total;
        int32 Choice = Options.Num() - 1;
        for (int32 I = 0; I < Options.Num(); ++I)
        {
            Draw -= Options[I].Weight;
            if (Draw < 0)
            {
                Choice = I;
                break;
            }
        }
        const FOption Selected = Options[Choice];
        for (int32 I = Playback.Num() - 1; I >= 0; --I)
            if (Playback[I].bTargeted == (Target != nullptr) && Playback[I].TargetRig.Get() == Target && Conflicts(Selected.Scopes, Playback[I].Scopes))
            {
                Playback[I].bPlaying = false;
                HeldPlayback.Add(Playback[I]);
                Playback.RemoveAt(I);
            }
        TArray<int32> Indices;
        Selected.Clips.GetKeys(Indices);
        Indices.Sort();
        for (int32 Index : Indices)
        {
            const float Speed = Selected.Clips[Index];
            double From = -1;
            // From the rig's current pose: the frame of this clip nearest to what the rig shows now (a door turned around
            // mid-stroke, or one whose rest pose is not its clip's first frame), so nothing snaps.
            // The same clip already on the rig (a transition turned around) continues from its exact playhead, but only while
            // that playhead is what the rig shows, sampled in the direction it will now play. A held layer a later clip covers
            // (an open/close door's Open under the Close that followed it, or the rest pose's Close under an Open) is a stale
            // playhead: resuming it would snap the door to that clip's end with no frame played (and no sound). A clip whose
            // sources differ in length samples other keys at the same playhead once reversed. Otherwise the nearest frame.
            const FSCClipBinding* TargetBinding = Target && Target->GetSkeletalMeshAsset() ? ClipCatalog->Clips[Index].Bindings.FindByPredicate(
                [&](const FSCClipBinding& X) { return X.Mesh == Target->GetSkeletalMeshAsset(); }) : nullptr;
            const FSCClipPlayback* Same = nullptr;
            for (const TArray<FSCClipPlayback>* List : {&Playback, &HeldPlayback})
                for (const FSCClipPlayback& P : *List)
                    if (P.Clip == Index && AppliesTo(P, Target) && (!Same || P.Order > Same->Order)) Same = &P;
            constexpr double ShownCost = 1e-4; // rad + m over the clip's channels: the same pose to sampling precision
            const TCHAR* FromWhat = TEXT("begin");
            if (Start == ESCFragmentStart::CurrentPose && TargetBinding)
            {
                const FSCImportedClip& C = ClipCatalog->Clips[Index];
                if (Same && ClipPoseCost(C, *TargetBinding, Target->LocalPose, Same->Frame, Speed < 0) < ShownCost)
                {
                    From = Same->Frame;
                    FromWhat = TEXT("playhead");
                }
                else
                {
                    From = NearestClipFrame(C, *TargetBinding, Target->LocalPose, Speed < 0);
                    FromWhat = TEXT("nearest_pose");
                }
            }
            if (Start == ESCFragmentStart::CurrentPose)
                UE_LOG(LogTemp, Display, TEXT("DOOR_CLIP_START owner=%s rig=%s fragment=%s tag=%s clip=%s from=%.2f last=%d speed=%.2f start=%s"),
                       Target ? *Target->OwnerEntity : TEXT("-"), Target ? *Target->GetName() : TEXT("-"), *Fragment.ToString(), *Tag.ToString(), *ClipCatalog->Clips[Index].Id.ToString(), From,
                       ClipCatalog->Clips[Index].LastFrame, Speed, FromWhat);
            const bool bHold = Start == ESCFragmentStart::HoldFirst || Start == ESCFragmentStart::HoldLast;
            // Held at the start or the end of play: a fragment that plays its clip backwards (speed < 0) ends at frame 0.
            Found |= StartClip(Index, Speed, bHold, (Start == ESCFragmentStart::HoldLast) == (Speed >= 0), Selected.Scopes, Target, From);
        }
        Options.RemoveAll([&](const FOption& O) { return Conflicts(Selected.Scopes, O.Scopes); });
    }
    if (Found)
    {
        // Record the played state on every rig instance this call actually drove (entries started
        // above), so per-door state and the orbit toggle follow what is posed.
        if (!Target) FragmentTargets.Add({nullptr, Fragment}, Tag);
        auto Record = [&](const FSCClipPlayback& P) {
            if (P.Order <= OrderBefore) return;
            for (USCRigComponent* Rig : Rigs)
                if (AppliesTo(P, Rig) &&
                    ClipCatalog->Clips[P.Clip].Bindings.ContainsByPredicate([&](const FSCClipBinding& B) { return B.Mesh == Rig->GetSkeletalMeshAsset(); }))
                    FragmentTargets.Add({Rig, Fragment}, Tag);
        };
        for (const auto& P : Playback) Record(P);
        for (const auto& P : HeldPlayback) Record(P);
    }
    if (Found && !ScreenFaces.IsEmpty()) RefreshPanelScreens(); // WP-I1 I1-3: the pads of a door that starts moving
    return Found;
}
bool ASCVehicle::IsFragmentOpen(FName Fragment, const USCRigComponent* Rig) const
{
    return FragmentTargets.FindRef({Rig, Fragment}) == FName(TEXT("Open"));
}
bool ASCVehicle::ToggleClip(FName Fragment, FName OpenTag, FName CloseTag)
{
    return PlayFragment(Fragment, FragmentTargets.FindRef({nullptr, Fragment}) == OpenTag ? CloseTag : OpenTag);
}
void ASCVehicle::ResetPose()
{
    Playback.Reset();
    HeldPlayback.Reset();
    DirtyMeshes.Reset();
    FragmentTargets.Reset();
    SensorDoorStates.Reset(); // a door a sensor opened is back at rest: no pending exit closes it later
    UpdateTickEnabled();
    for (USCRigComponent* Rig : Rigs)
        Rig->ResetPose();
    ApplyDoorRestPoses(); // every door back at its authored spawn state (C2 fixes)
    // A lift whose doors were opening or open has them at rest now: it drives them open again (else it waits for them forever).
    for (const TSharedPtr<FSCLift>& Lift : Lifts) Lift->ReopenDoorsAfterReset();
    if (!ScreenFaces.IsEmpty()) RefreshPanelScreens(); // WP-I1 I1-3: every door is back at rest
}
void ASCVehicle::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (DeltaSeconds <= 0) return;
    // Lifts first (pre-physics, before the riders' movement: FSCLift moves the carriage anchor, doors start here).
    for (const TSharedPtr<FSCLift>& Lift : Lifts) Lift->Tick(DeltaSeconds);
    if (!FloorPanels.IsEmpty()) RefreshFloorPanels(); // WP-P5 floor panel: the lift's state on its panels
    // A targeted entry whose rig is gone poses nothing (it is not an untargeted layer for the rigs sharing its mesh).
    auto Orphaned = [](const FSCClipPlayback& P) { return P.bTargeted && !P.TargetRig.IsValid(); };
    Playback.RemoveAll(Orphaned);
    HeldPlayback.RemoveAll(Orphaned);
    RestoreCargoDoorLocks();
    if (!ClipCatalog || Playback.IsEmpty()) { FinishSecureOrders(); UpdateTickEnabled(); return; }
    const USCRigComponent* Traced = Doors.IsValidIndex(TraceDoor) ? Doors[TraceDoor].Rig.Get() : nullptr;
    TArray<FString> TraceLines; // sc.Doors.Trace: the traced door's clips advanced this tick
    for (int32 I = Playback.Num() - 1; I >= 0; --I)
    {
        auto& P = Playback[I];
        const auto& C = ClipCatalog->Clips[P.Clip];
        const double Previous = P.Frame;
        P.Frame = FMath::Clamp(P.Frame + DeltaSeconds * C.Fps * P.Speed, 0., double(C.LastFrame));
        if (P.Frame != Previous) MarkDirty(P.Clip);
        FireClipAudio(P, Previous); // WP-A1: route C, the clip's animation events crossed this frame
        // A forward clip at frame zero has not finished; reverse playback ends at zero.
        const bool bEnded = (P.Speed > 0 && P.Frame >= C.LastFrame) || (P.Speed < 0 && P.Frame <= 0);
        if (Traced && P.TargetRig.Get() == Traced)
            TraceLines.Add(FString::Printf(TEXT("clip=%s from=%.2f frame=%.2f last=%d speed=%.2f ended=%d"), *C.Id.ToString(), Previous, P.Frame, C.LastFrame, P.Speed, bEnded));
        if (bEnded)
        {
            P.bPlaying = false;
            HeldPlayback.Add(P);
            Playback.RemoveAt(I);
        }
    }
    EvaluateClips();
    for (const FString& Line : TraceLines)
    {
        const FSCDoorInteraction& D = Doors[TraceDoor];
        const FVector Tip = GetActorTransform().InverseTransformPosition(D.Rig->GetSocketTransform(D.Joint).TransformPosition(FVector(300)));
        UE_LOG(LogTemp, Display, TEXT("DOOR_TICK door=%d t=%.3f dt=%.4f open=%d %s tip=%.2f,%.2f,%.2f"), TraceDoor, GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0,
               DeltaSeconds, IsDoorOpen(TraceDoor), *Line, Tip.X, Tip.Y, Tip.Z);
    }
    FinishSecureOrders();
    if (!ScreenFaces.IsEmpty()) RefreshPanelScreens(); // WP-I1 I1-3: a door that started or stopped moving
    UpdateTickEnabled();
}
void ASCVehicle::BeginPlay()
{
    Super::BeginPlay();
    if (!CookedAudio.IsEmpty()) FSCAudioScene::LoadCooked(this); // WP-A1: a cooked level's audio
}

void ASCVehicle::FireClipAudio(FSCClipPlayback& P, double Previous)
{
    USCAudioSubsystem* Audio = USCAudioSubsystem::Get(this);
    if (!ClipCatalog || !ClipCatalog->Clips.IsValidIndex(P.Clip)) return;
    const double Lo = FMath::Min(Previous, P.Frame), Hi = FMath::Max(Previous, P.Frame);
    if (Lo == Hi) return; // a zero-length step keeps the start frame for the first real advance
    const bool bFresh = P.bAudioFresh;
    P.bAudioFresh = false;
    const int8 Dir = P.Speed >= 0 ? 1 : -1;
    const double Last = ClipCatalog->Clips[P.Clip].LastFrame;
    for (const FSCClipBinding& B : ClipCatalog->Clips[P.Clip].Bindings)
    {
        if (B.AudioEvents.IsEmpty()) continue;
        for (USCRigComponent* Rig : Rigs)
        {
            if (!Rig || Rig->GetSkeletalMeshAsset() != B.Mesh || !AppliesTo(P, Rig)) continue;
            int8 Shown = -1; // whether this entry shows on Rig at all (a later layer may cover every channel it drives there)
            for (const FSCClipAudioEvent& E : B.AudioEvents)
            {
                if (E.Direction != 0 && E.Direction != Dir) continue;
                // The event's clip frame in this direction (reverse playback aligns a source with the clip's end).
                const double F = Dir < 0 && E.FrameReverse >= 0 ? E.FrameReverse : E.Frame;
                // Crossed this frame: (Lo, Hi] forward, [Lo, Hi) backward. On the first advance, an event the authors put at the
                // start of play in this direction (within one clip frame of it: the C2 doors' audio_trigger at t 0.0001 and
                // audio_trigger_reverse at t 0.9999) is the motion's start sound: it plays also when the clip starts past it (a
                // door turned around mid-stroke starts its other clip or direction at the frame showing its pose).
                const bool bStartSound = bFresh && (Dir > 0 ? F <= Lo && F <= 1.0 : F >= Hi && F >= Last - 1.0);
                const bool bCrossed = bStartSound || (Dir > 0 ? F > Lo && F <= Hi : F < Hi && F >= Lo);
                if (!bCrossed) continue;
                if (Shown < 0) Shown = IsCoveredOnRig(P, Rig) ? 0 : 1;
                if (!Shown) continue;
                ++ClipAudioEvents;
                LastClipAudioTrigger = E.Trigger;
                if (TraceDoor != INDEX_NONE && Doors.IsValidIndex(TraceDoor) && Doors[TraceDoor].Rig == Rig)
                    UE_LOG(LogTemp, Display, TEXT("DOOR_AUDIO_EVENT door=%d clip=%s trigger=%s event_frame=%.2f from=%.2f to=%.2f speed=%.2f"), TraceDoor,
                           *ClipCatalog->Clips[P.Clip].Id.ToString(), *E.Trigger, F, Previous, P.Frame, P.Speed);
                if (Audio) Audio->PlayClipEvent(this, Rig, E.Trigger, E.Bone, E.Offset);
            }
        }
    }
}

void ASCVehicle::UpdateTickEnabled()
{
    bool bLifts = false;
    for (const TSharedPtr<FSCLift>& Lift : Lifts) bLifts |= Lift->NeedsTick();
    SetActorTickEnabled(ShipDoors.ContainsByPredicate([](const FShipDoor& Door) { return Door.bCargoRelock || Door.bSecurePending; }) || !Playback.IsEmpty() || bLifts);
}
void ASCVehicle::EvaluateClips()
{
    if (DirtyMeshes.IsEmpty()) return;
    TArray<FSCClipPlayback> ClipLayers = HeldPlayback;
    ClipLayers.Append(Playback);
    ClipLayers.Sort([](const FSCClipPlayback& A, const FSCClipPlayback& B) { return A.Order < B.Order; });
    // Only affected rigs are rebuilt. Held layers preserve the last authored pose and overlap order.
    for (USCRigComponent* Rig : Rigs)
    {
        if (!DirtyMeshes.Contains(Rig->GetSkeletalMeshAsset())) continue;
        TArray<FTransform> Pose = Rig->GetSkeletalMeshAsset()->GetRefSkeleton().GetRefBonePose();
        for (const auto& P : ClipLayers)
            for (const auto& B : ClipCatalog->Clips[P.Clip].Bindings)
                if (AppliesTo(P, Rig) && B.Mesh == Rig->GetSkeletalMeshAsset())
                    for (const auto& T : B.Tracks)
                    {
                        const FTransform Sample =
                            FSCAnimImporter::Sample(T, FSCAnimImporter::TrackFrame(ClipCatalog->Clips[P.Clip], T, P.Frame, P.Speed < 0));
                        if (T.bRotation) Pose[T.Bone].SetRotation(Sample.GetRotation());
                        if (T.bPosition) Pose[T.Bone].SetTranslation(Sample.GetTranslation());
                    }
        bool Changed = Pose.Num() != Rig->LocalPose.Num();
        for (int32 I = 0; !Changed && I < Pose.Num(); ++I)
            Changed |= !Pose[I].Equals(Rig->LocalPose[I], 1.e-8);
        if (!Changed) continue;
        Rig->LocalPose = MoveTemp(Pose);
        Rig->RefreshBoneTransforms();
        ++PoseEvaluations;
    }
    DirtyMeshes.Reset();
}

void ASCVehicle::RefreshLightShadowPolicy()
{
    FSCLightImporter::RefreshShadows(this);
}

bool ASCVehicle::IsInsideLightArea(const FVector& World) const
{
    for (const FSCLightArea& Area : LightAreas)
        if (Area.Contains(World)) return true;
    return false;
}
FString ASCVehicle::LightRoomAt(const FVector& World) const
{
    for (const FSCLightArea& Area : LightAreas)
        if (FString Room = Area.RoomAt(World); !Room.IsEmpty()) return Room;
    return FString();
}
int32 ASCVehicle::LightChannelAt(const FVector& World) const
{
    const FString Room = LightRoomAt(World);
    if (Room.IsEmpty()) return 0;
    const int32* Channel = LightRoomChannels.Find(Room);
    return Channel ? *Channel : 1; // before FSCLightAreas::Apply: the WP-U3b inside channel
}
bool ASCVehicle::IsInPhysicsGrid(const FVector& World) const
{
    if (PhysicsGridTriangles.Num() < 3) return false;
    const FVector P = GetActorTransform().InverseTransformPosition(World);
    if (!PhysicsGridBounds.IsInside(P)) return false;
    // Parity of crossings along a ray off every axis and diagonal (no grazing of axis-aligned edges); the ray ends
    // past the bounds, so every crossing is counted. Each shape is its own closed volume (overlapping shapes: inside
    // any), and hits at the same distance are one crossing (a ray through an edge shared by two triangles).
    const FVector End = P + FVector(1, 0.0013, 0.0007).GetSafeNormal() * (PhysicsGridBounds.GetSize().Size() * 2 + 100);
    int32 Begin = 0;
    TArray<double> Hits;
    for (const int32 ShapeEnd : PhysicsGridShapeEnds.IsEmpty() ? TArray<int32>{PhysicsGridTriangles.Num()} : PhysicsGridShapeEnds)
    {
        Hits.Reset();
        for (int32 T = Begin; T + 2 < FMath::Min(ShapeEnd, PhysicsGridTriangles.Num()); T += 3)
        {
            FVector Hit, Normal;
            if (FMath::SegmentTriangleIntersection(P, End, PhysicsGridTriangles[T], PhysicsGridTriangles[T + 1], PhysicsGridTriangles[T + 2], Hit, Normal))
                Hits.Add(FVector::Dist(P, Hit));
        }
        Begin = ShapeEnd;
        Hits.Sort();
        int32 Crossings = 0;
        for (int32 H = 0; H < Hits.Num(); ++H)
            if (H == 0 || Hits[H] - Hits[H - 1] > 1e-3) ++Crossings; // 10 um apart: the same surface point
        if (Crossings % 2 == 1) return true;
    }
    return false;
}
bool ASCVehicle::IsInPhysicsGrid(const FVector& Bottom, const FVector& Top) const
{
    if (PhysicsGridTriangles.Num() < 3) return false;
    if (IsInPhysicsGrid(Bottom)) return true;
    const FTransform& Actor = GetActorTransform();
    const FVector A = Actor.InverseTransformPosition(Bottom), B = Actor.InverseTransformPosition(Top);
    if (!PhysicsGridBounds.Intersect(FBox(FVector::Min(A, B), FVector::Max(A, B)))) return false;
    for (int32 T = 0; T + 2 < PhysicsGridTriangles.Num(); T += 3)
    {
        FVector Hit, Normal;
        if (FMath::SegmentTriangleIntersection(A, B, PhysicsGridTriangles[T], PhysicsGridTriangles[T + 1], PhysicsGridTriangles[T + 2], Hit, Normal))
            return true;
    }
    return false;
}

double ASCVehicle::LowestVisibleZ(FString* OutPart) const
{
    // Candidates by bounds (a rig's bounds are its reference pose, a rotated part's box reaches below its surface), then
    // the true lowest surface of every candidate near the bottom: a rig's CPU-skinned collision (current pose), else a
    // fine grid of vertical traces against the part's own collision; bounds only for parts without collision.
    TArray<const UPrimitiveComponent*> Parts;
    double BoundsLowest = TNumericLimits<double>::Max();
    ForEachComponent<UPrimitiveComponent>(false, [&](const UPrimitiveComponent* C) {
        if (!C->IsRegistered() || !C->IsVisible() || C->bHiddenInGame || C->ComponentHasTag(TEXT("SCGameCollision"))) return;
        // Only solid surfaces bear weight: parts the importer drew without shadows (glass, glow, decals, light beams) and
        // parts drawn entirely with translucent materials are skipped.
        if (!C->CastShadow) return;
        if (const auto* SM = Cast<UStaticMeshComponent>(C); SM && SM->GetStaticMesh() && SM->GetStaticMesh()->GetRenderData() &&
                                                            !SM->GetStaticMesh()->GetRenderData()->LODResources.IsEmpty())
        {
            bool bSolid = false;
            for (const FStaticMeshSection& Section : SM->GetStaticMesh()->GetRenderData()->LODResources[0].Sections)
                if (const UMaterialInterface* M = SM->GetMaterial(Section.MaterialIndex); M && !IsTranslucentBlendMode(*M))
                    bSolid = true;
            if (!bSolid) return;
        }
        Parts.Add(C);
        BoundsLowest = FMath::Min(BoundsLowest, C->Bounds.GetBox().Min.Z);
    });
    constexpr double CandidateBandCm = 150.0; // a part whose box starts this close to the lowest box may hold the lowest point
    double Lowest = TNumericLimits<double>::Max();
    auto TakeAt = [&](const FVector& P, const UPrimitiveComponent* C) {
        if (P.Z >= Lowest) return;
        Lowest = P.Z;
        if (OutPart)
            *OutPart = FString::Printf(TEXT("%s at %s socket=%s"), C->ComponentTags.Num() ? *C->ComponentTags[0].ToString() : *C->GetName(),
                                       *P.ToCompactString(), *C->GetAttachSocketName().ToString());
    };
    auto Take = [&](double Z, const UPrimitiveComponent* C) { TakeAt(FVector(C->Bounds.Origin.X, C->Bounds.Origin.Y, Z), C); };
    TArray<FVector3f> Skinned;
    for (const UPrimitiveComponent* C : Parts)
    {
        const FBox Box = C->Bounds.GetBox();
        if (Box.Min.Z > BoundsLowest + CandidateBandCm) continue;
        if (const auto* Rig = Cast<USCRigComponent>(C); Rig && Rig->ReadCollisionVertices(Skinned) && !Skinned.IsEmpty())
        {
            const FTransform& World = Rig->GetComponentTransform();
            for (const FVector3f& V : Skinned) TakeAt(World.TransformPosition(FVector(V)), C);
            continue;
        }
        // The part's collision triangles (local space) placed by its current transform: exact, and independent of when the
        // physics scene catches up with a pose snapped this frame.
        bool bVertices = false;
        if (UBodySetup* Body = const_cast<UPrimitiveComponent*>(C)->GetBodySetup())
        {
            const FTransform& World = C->GetComponentTransform();
            for (const Chaos::FTriangleMeshImplicitObjectPtr& Mesh : Body->TriMeshGeometries)
            {
                if (!Mesh) continue;
                const auto& Particles = Mesh->Particles();
                for (uint32 I = 0; I < Particles.Size(); ++I) TakeAt(World.TransformPosition(FVector(Particles.GetX(I))), C);
                bVertices |= Particles.Size() > 0;
            }
        }
        if (!bVertices) Take(Box.Min.Z, C);
    }
    return Lowest;
}
int32 ASCVehicle::RestOnLandingGear()
{
    if (!ClipCatalog) return 0;
    int32 Posed = 0;
    for (int32 Index = 0; Index < ClipCatalog->Clips.Num(); ++Index)
    {
        const FSCImportedClip& C = ClipCatalog->Clips[Index];
        if (!C.Bindings.IsEmpty() && C.Fragments.ContainsByPredicate([](const auto& F) {
                return F.Fragment == FName(TEXT("Landing_Gear")) && F.FragmentTags.Contains(FName(TEXT("Compress")));
            }))
            Posed += StartClip(Index, 1, true, true) ? 1 : 0;
    }
    return Posed;
}
