// Vehicle parameters and on-demand evaluation of sparse, ordered animation layers.
#include "SCVehicle.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "SCMaterialBuilder.h"
#include "SCAnimImporter.h"
#include "SCLightImporter.h"
#include "SCRigComponent.h"
#include "Engine/SkeletalMesh.h"
ASCVehicle::ASCVehicle()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("VehicleRoot"));
    RootComponent->SetMobility(EComponentMobility::Movable);
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
bool ASCVehicle::StartClip(int32 Index, float Speed, bool bSnap, bool bLast, const TArray<FName>& Scopes, USCRigComponent* Target)
{
    if (!ClipCatalog || !ClipCatalog->Clips.IsValidIndex(Index) || !FMath::IsFinite(Speed)) return false;
    const auto& Clip = ClipCatalog->Clips[Index];
    if (Clip.Bindings.IsEmpty()) return false;
    Playback.RemoveAll([&](const FSCClipPlayback& P) { return P.Clip == Index && P.TargetRig.Get() == Target; });
    HeldPlayback.RemoveAll([&](const FSCClipPlayback& P) { return P.Clip == Index && P.TargetRig.Get() == Target; });
    FSCClipPlayback P;
    P.Clip = Index;
    P.TargetRig = Target;
    P.Speed = Speed;
    P.Frame = (bSnap ? bLast : Speed < 0) ? Clip.LastFrame : 0;
    P.bPlaying = !bSnap && Speed != 0 && Clip.LastFrame > 0;
    P.Order = ++NextOrder;
    P.Scopes = Scopes;
    (P.bPlaying ? Playback : HeldPlayback).Add(MoveTemp(P));
    MarkDirty(Index);
    EvaluateClips();
    SetActorTickEnabled(!Playback.IsEmpty());
    return true;
}
bool ASCVehicle::PlayClip(FName Id)
{
    if (!ClipCatalog) return false;
    return StartClip(ClipCatalog->Clips.IndexOfByPredicate([&](const FSCImportedClip& C) { return C.Id == Id; }), 1, false, false);
}
bool ASCVehicle::SnapClip(FName Id, bool bLast)
{
    if (!ClipCatalog) return false;
    return StartClip(ClipCatalog->Clips.IndexOfByPredicate([&](const FSCImportedClip& C) { return C.Id == Id; }), 1, true, bLast);
}
bool ASCVehicle::PlayFragment(FName Fragment, FName Tag)
{
    return PlayFragmentAt(Fragment, Tag, nullptr);
}
bool ASCVehicle::PlayFragmentAt(FName Fragment, FName Tag, USCRigComponent* Target)
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
    for (int32 Index = 0; Index < ClipCatalog->Clips.Num(); ++Index)
    {
        const auto& C = ClipCatalog->Clips[Index];
        if (C.Bindings.IsEmpty() || (Target && !C.Bindings.ContainsByPredicate([&](const FSCClipBinding& B) { return B.Mesh == Target->GetSkeletalMeshAsset(); }))) continue;
        for (const auto& F : C.Fragments)
        {
            if (F.Fragment != Fragment || F.OptionWeight <= 0 || (!Tag.IsNone() && !F.Tags.Contains(Tag) && !F.FragmentTags.Contains(Tag)))
                continue;
            // Aliases keep metadata, but play the canonical animation only once.
            if (!F.AnimationNames.Contains(C.Id) && F.AnimationNames.ContainsByPredicate([&](FName Id) {
                    return ClipCatalog->Clips.ContainsByPredicate([&](const FSCImportedClip& Other) { return Other.Id == Id; });
                }))
                continue;
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
            Option->Clips.Add(Index, F.Speed);
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
            if (Playback[I].TargetRig.Get() == Target && Conflicts(Selected.Scopes, Playback[I].Scopes))
            {
                Playback[I].bPlaying = false;
                HeldPlayback.Add(Playback[I]);
                Playback.RemoveAt(I);
            }
        TArray<int32> Indices;
        Selected.Clips.GetKeys(Indices);
        Indices.Sort();
        for (int32 Index : Indices)
            Found |= StartClip(Index, Selected.Clips[Index], false, false, Selected.Scopes, Target);
        Options.RemoveAll([&](const FOption& O) { return Conflicts(Selected.Scopes, O.Scopes); });
    }
    if (Found) FragmentTargets.Add(Fragment, Tag);
    return Found;
}
bool ASCVehicle::ToggleClip(FName Fragment, FName OpenTag, FName CloseTag)
{
    return PlayFragment(Fragment, FragmentTargets.FindRef(Fragment) == OpenTag ? CloseTag : OpenTag);
}
void ASCVehicle::ResetPose()
{
    Playback.Reset();
    HeldPlayback.Reset();
    DirtyMeshes.Reset();
    FragmentTargets.Reset();
    for (auto& Door : Doors) Door.bOpen = false;
    SetActorTickEnabled(false);
    for (USCRigComponent* Rig : Rigs)
        Rig->ResetPose();
}
void ASCVehicle::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!ClipCatalog || Playback.IsEmpty() || DeltaSeconds <= 0) return;
    for (int32 I = Playback.Num() - 1; I >= 0; --I)
    {
        auto& P = Playback[I];
        const auto& C = ClipCatalog->Clips[P.Clip];
        const double Previous = P.Frame;
        P.Frame = FMath::Clamp(P.Frame + DeltaSeconds * C.Fps * P.Speed, 0., double(C.LastFrame));
        if (P.Frame != Previous) MarkDirty(P.Clip);
        // A forward clip at frame zero has not finished; reverse playback ends at zero.
        if ((P.Speed > 0 && P.Frame >= C.LastFrame) || (P.Speed < 0 && P.Frame <= 0))
        {
            P.bPlaying = false;
            HeldPlayback.Add(P);
            Playback.RemoveAt(I);
        }
    }
    EvaluateClips();
    SetActorTickEnabled(!Playback.IsEmpty());
}
void ASCVehicle::EvaluateClips()
{
    if (DirtyMeshes.IsEmpty()) return;
    TArray<FSCClipPlayback> Layers = HeldPlayback;
    Layers.Append(Playback);
    Layers.Sort([](const FSCClipPlayback& A, const FSCClipPlayback& B) { return A.Order < B.Order; });
    // Only affected rigs are rebuilt. Held layers preserve the last authored pose and overlap order.
    for (USCRigComponent* Rig : Rigs)
    {
        if (!DirtyMeshes.Contains(Rig->GetSkeletalMeshAsset())) continue;
        TArray<FTransform> Pose = Rig->GetSkeletalMeshAsset()->GetRefSkeleton().GetRefBonePose();
        for (const auto& P : Layers)
            for (const auto& B : ClipCatalog->Clips[P.Clip].Bindings)
                if ((!P.TargetRig.IsValid() || P.TargetRig.Get() == Rig) && B.Mesh == Rig->GetSkeletalMeshAsset())
                    for (const auto& T : B.Tracks)
                    {
                        const FTransform Sample = FSCAnimImporter::Sample(T, P.Frame);
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
