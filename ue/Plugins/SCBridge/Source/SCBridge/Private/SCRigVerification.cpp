// Headless rig invariants, source-based regressions, and the quarantined WP-R6 pose-export comparison.
#include "SCImporter.h"
#include "SCVehicle.h"
#include "SCRigComponent.h"
#include "SCAnimImporter.h"
#include "SCLightImporter.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/AnimSequence.h"
#include "Components/LightComponent.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Misc/Paths.h"

bool SCVerifySourcePose(ASCVehicle* Vehicle);

bool USCImportLibrary::VerifyRig(ASCVehicle* Vehicle, const FString& PoseReference, FName PoseClip)
{
    if (!Vehicle || !Vehicle->ClipCatalog) return false;
    bool OK = Vehicle->ClipCatalog->Clips.Num() == Vehicle->Manifest.Clips.Num();
    int32 Bound = 0, Sequences = 0, Tracks = 0, SoftTriangles = 0;
    for (const auto& Clip : Vehicle->ClipCatalog->Clips)
    {
        OK &= Clip.Sequence != nullptr;
        Bound += !Clip.Bindings.IsEmpty();
        for (const auto& Binding : Clip.Bindings)
        {
            OK &= !Binding.Sequences.IsEmpty();
            for (const UAnimSequence* Sequence : Binding.Sequences)
                OK &= Sequence && Sequence->GetSkeleton() == Binding.Mesh->GetSkeleton();
            Sequences += Binding.Sequences.Num();
            Tracks += Binding.Tracks.Num();
            for (const auto& Track : Binding.Tracks)
            {
                // A track keeps its own animation's rate and length, never longer than the clip.
                OK &= Track.Fps > 0 && !Track.Keys.IsEmpty() &&
                      (Track.Keys.Num() - 1) / Track.Fps <= Clip.LastFrame / Clip.Fps + 1.e-4;
                for (int32 K = 1; K < Track.Keys.Num(); ++K)
                    OK &= !Track.Keys[K].ContainsNaN() && (Track.Keys[K - 1].GetRotation() | Track.Keys[K].GetRotation()) >= -1.e-6;
            }
        }
    }
    for (USkeletalMesh* Mesh : Vehicle->SkeletalMeshes)
        if (auto* Render = Mesh->GetResourceForRendering())
            for (const auto& LOD : Render->LODRenderData)
                for (const auto& Section : LOD.RenderSections)
                    SoftTriangles += Section.NumTriangles;
    for (USCRigComponent* Rig : Vehicle->Rigs)
    {
        OK &= Rig->LocalPose.Num() == Rig->GetSkeletalMeshAsset()->GetRefSkeleton().GetNum();
        if (Rig->LocalPose.Num() > 1)
        {
            const FName Bone = Rig->GetSkeletalMeshAsset()->GetRefSkeleton().GetBoneName(1);
            OK &= Rig->DoesSocketExist(Bone);
        }
    }
    // Advance a pair of clips and assert sparse composition uses the latest authored track.
    if (Vehicle->ClipCatalog->Clips.Num() > 1)
    {
        for (const auto& Clip : Vehicle->ClipCatalog->Clips)
            if (!Clip.Bindings.IsEmpty())
            {
                Vehicle->PlayClip(Clip.Id);
                if (Vehicle->Playback.Num() == 2) break;
            }
        Vehicle->Tick(0.25f);
        TArray<FSCClipPlayback> Layers = Vehicle->HeldPlayback;
        Layers.Append(Vehicle->Playback);
        Layers.Sort([](const FSCClipPlayback& A, const FSCClipPlayback& B) { return A.Order < B.Order; });
        for (USCRigComponent* Rig : Vehicle->Rigs)
        {
            TMap<int32, FTransform> Expected;
            for (const auto& Play : Layers)
                for (const auto& B : Vehicle->ClipCatalog->Clips[Play.Clip].Bindings)
                    if (B.Mesh == Rig->GetSkeletalMeshAsset())
                        for (const auto& T : B.Tracks)
                        {
                            FTransform* X = Expected.Find(T.Bone);
                            if (!X) X = &Expected.Add(T.Bone, Rig->GetSkeletalMeshAsset()->GetRefSkeleton().GetRefBonePose()[T.Bone]);
                            const auto Sample = FSCAnimImporter::Sample(
                                T, FSCAnimImporter::TrackFrame(Vehicle->ClipCatalog->Clips[Play.Clip], T, Play.Frame, Play.Speed < 0));
                            if (T.bRotation) X->SetRotation(Sample.GetRotation());
                            if (T.bPosition) X->SetTranslation(Sample.GetTranslation());
                        }
            for (const auto& Pair : Expected)
                OK &= Rig->LocalPose[Pair.Key].Equals(Pair.Value, 1.e-5);
        }
        Vehicle->ResetPose();
    }
    UE_LOG(LogSCImport, Display, TEXT("RIG_VERIFY passed=%d clips=%d bound=%d sequences=%d tracks=%d rigs=%d soft_triangles=%d lights=%d"),
           OK, Vehicle->ClipCatalog->Clips.Num(), Bound, Sequences, Tracks, Vehicle->Rigs.Num(), SoftTriangles, Vehicle->Lights.Num());
    OK &= SCVerifySourcePose(Vehicle);
    OK &= FSCLightImporter::Verify(Vehicle);
    if (!PoseReference.IsEmpty())
    {
        FSCFolderPackageSource Source(FPaths::GetPath(PoseReference));
        FSCDerivedMesh Reference;
        FString Error;
        if (!FSCMeshImporter::Derive(Source, FPaths::GetCleanFilename(PoseReference), Reference, Error) ||
            !Vehicle->SnapClip(PoseClip, true))
        {
            UE_LOG(LogSCImport, Error, TEXT("POSE_CHECK failed: %s clip=%s"), *Error, *PoseClip.ToString());
            return false;
        }
        const auto* Clip = Vehicle->ClipCatalog->Clips.FindByPredicate([&](const FSCImportedClip& C) { return C.Id == PoseClip; });
        double MaxDegrees = 0, MaxAuthoredDegrees = 0, MaxCentimetres = 0;
        int32 Compared = 0, Authored = 0, UnauthoredDifferences = 0;
        for (USCRigComponent* Rig : Vehicle->Rigs)
            for (const auto& B : Clip->Bindings)
                if (B.Mesh == Rig->GetSkeletalMeshAsset())
                    for (const auto& T : B.Tracks)
                    {
                        const FString Bone = Rig->GetSkeletalMeshAsset()->GetRefSkeleton().GetBoneName(T.Bone).ToString();
                        const auto* Ref =
                            Reference.RigNodes.FindByPredicate([&](const FSCRigNode& N) { return N.bAnimate && N.SourceName == Bone; });
                        if (!Ref) continue;
                        const double Degrees =
                            FMath::RadiansToDegrees(Rig->LocalPose[T.Bone].GetRotation().AngularDistance(Ref->Local.GetRotation()));
                        const double Centimetres = FVector::Distance(Rig->LocalPose[T.Bone].GetTranslation(), Ref->Local.GetTranslation());
                        MaxCentimetres = FMath::Max(MaxCentimetres, Centimetres);
                        MaxDegrees = FMath::Max(MaxDegrees, Degrees);
                        ++Compared;
                        if (T.bRotation)
                        {
                            ++Authored;
                            MaxAuthoredDegrees = FMath::Max(MaxAuthoredDegrees, Degrees);
                        }
                        else if (Degrees > 5)
                            ++UnauthoredDifferences;
                        if (Degrees > 5 || Centimetres > 0.01)
                            UE_LOG(LogSCImport, Display, TEXT("POSE_DIFFERENCE bone=%s degrees=%.5f centimetres=%.6f"), *Bone, Degrees,
                                   Centimetres);
                    }
        const bool PoseOK = Compared > 0 && MaxDegrees <= 5 && MaxCentimetres <= 0.01;
        OK &= PoseOK;
        UE_LOG(LogSCImport, Display,
               TEXT("POSE_CHECK pending=WP-R6 oracle=0 passed=%d clip=%s bones=%d max_degrees=%.6f max_centimetres=%.6f "
                    "tolerance_degrees=5 tolerance_cm=0.01 reference=%s"),
               PoseOK, *PoseClip.ToString(), Compared, MaxDegrees, MaxCentimetres, *PoseReference);
        UE_LOG(LogSCImport, Display,
               TEXT("POSE_AUTHORED_ROTATIONS passed=%d bones=%d max_degrees=%.6f unauthored_reference_differences=%d"),
               Authored > 0 && MaxAuthoredDegrees <= 5, Authored, MaxAuthoredDegrees, UnauthoredDifferences);
        Vehicle->ResetPose();
    }
    return OK;
}
