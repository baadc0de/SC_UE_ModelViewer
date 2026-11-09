#include "SCVehicle.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "SCMaterialBuilder.h"
#include "SCAnimImporter.h"
#include "SCRigComponent.h"
#include "Engine/SkeletalMesh.h"
ASCVehicle::ASCVehicle()
{
    PrimaryActorTick.bCanEverTick = true;
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
            float ReadBack = -1.f; MID->GetScalarParameterValue(FName(TEXT("EmissiveStrength")), ReadBack);
            UE_LOG(LogTemp, Display, TEXT("ENGINE_GLOW control=%g emissive=%g instance=%s parent=%s readback=%g"), Strength, Value,
                *MID->GetName(), MID->Parent ? *MID->Parent->GetName() : TEXT("none"), ReadBack);
        }
}

bool ASCVehicle::PlayClip(FName Id)
{
    if (!ClipCatalog) return false;
    const int32 Index = ClipCatalog->Clips.IndexOfByPredicate([&](const FSCImportedClip& C) { return C.Id == Id; });
    if (Index == INDEX_NONE || ClipCatalog->Clips[Index].Bindings.IsEmpty()) return false;
    Playback.RemoveAll([&](const FSCClipPlayback& P) { return P.Clip == Index; });
    Playback.Add({Index, 0, 1, true}); EvaluateClips(); return true;
}
bool ASCVehicle::SnapClip(FName Id, bool bLast)
{
    if (!PlayClip(Id)) return false;
    auto& P = Playback.Last(); P.Frame = bLast ? ClipCatalog->Clips[P.Clip].LastFrame : 0; P.bPlaying = false;
    EvaluateClips(); return true;
}
bool ASCVehicle::PlayFragment(FName Fragment, FName Tag)
{
    if (!ClipCatalog) return false;
    bool Found = false;
    for (const auto& C : ClipCatalog->Clips)
        for (const auto& F : C.Fragments)
            if (F.Fragment == Fragment && (Tag.IsNone() || F.Tags.Contains(Tag) || F.FragmentTags.Contains(Tag)))
            {
                // Index aliases retain fragment metadata, but must not play twice over the canonical id.
                if (!F.AnimationNames.Contains(C.Id) && F.AnimationNames.ContainsByPredicate([&](FName Id)
                    { return ClipCatalog->Clips.ContainsByPredicate([&](const FSCImportedClip& Other) { return Other.Id == Id; }); })) continue;
                if (PlayClip(C.Id))
                {
                    auto& P = Playback.Last(); P.Speed = F.Speed;
                    if (P.Speed < 0) P.Frame = C.LastFrame;
                    Found = true;
                }
                break;
            }
    EvaluateClips(); return Found;
}
void ASCVehicle::ResetPose()
{
    Playback.Reset(); for (USCRigComponent* Rig : Rigs) Rig->ResetPose();
}
void ASCVehicle::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!ClipCatalog || Playback.IsEmpty()) return;
    for (auto& P : Playback) if (P.bPlaying)
    {
        const auto& C = ClipCatalog->Clips[P.Clip]; P.Frame += DeltaSeconds * C.Fps * P.Speed;
        if (P.Frame >= C.LastFrame || P.Frame <= 0) { P.Frame = FMath::Clamp(P.Frame, 0., double(C.LastFrame)); P.bPlaying = false; }
    }
    EvaluateClips();
}
void ASCVehicle::EvaluateClips()
{
    // Only authored tracks overwrite a bone. Disjoint clips compose; the most recent wins an overlap.
    for (USCRigComponent* Rig : Rigs)
    {
        Rig->LocalPose = Rig->GetSkeletalMeshAsset()->GetRefSkeleton().GetRefBonePose();
        for (const auto& P : Playback)
            for (const auto& B : ClipCatalog->Clips[P.Clip].Bindings) if (B.Mesh == Rig->GetSkeletalMeshAsset())
                for (const auto& T : B.Tracks)
                {
                    const FTransform Sample = FSCAnimImporter::Sample(T, P.Frame);
                    if (T.bRotation) Rig->LocalPose[T.Bone].SetRotation(Sample.GetRotation());
                    if (T.bPosition) Rig->LocalPose[T.Bone].SetTranslation(Sample.GetTranslation());
                }
        Rig->RefreshBoneTransforms();
    }
}
