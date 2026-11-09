// Package regressions use the original GLB bind and raw sidecars, never clip_final_pose exports.
#include "SCImporter.h"
#include "SCVehicle.h"
#include "SCAnimImporter.h"
#include "SCRigComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Dom/JsonObject.h"
#include "Misc/CommandLine.h"

bool SCVerifySourcePose(ASCVehicle* Vehicle)
{
    FString Folder;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SCPackage="), Folder)) return true;
    const FName ClipId(TEXT("landing_gear_extend"));
    const auto* Clip = Vehicle->ClipCatalog->Clips.FindByPredicate([&](const FSCImportedClip& C) { return C.Id == ClipId; });
    if (!Clip || Clip->Bindings.IsEmpty()) return true;
    const auto HasBone = [&](const TCHAR* Bone) {
        return Vehicle->SkeletalMeshes.ContainsByPredicate(
            [&](const USkeletalMesh* M) { return M->GetRefSkeleton().FindBoneIndex(Bone) != INDEX_NONE; });
    };
    const bool Scorpius = HasBone(TEXT("BONE_Front_Base_Arm"));
    const bool Gladius = HasBone(TEXT("LG_L_Back_CableTop_Skin_bn"));
    if (!Scorpius && !Gladius) return true; // These are regression fixtures, not production routing rules.
    FSCFolderPackageSource Source(Folder);
    const auto* Record = Vehicle->Manifest.Clips.FindByPredicate([&](const FSCClipRecord& R) { return FName(*R.Name) == ClipId; });
    FSCImportedClip Metadata;
    TSharedPtr<FJsonObject> Json;
    FString Error;
    if (!Record || !FSCAnimImporter::Read(Source, *Record, TEXT("Verification"), Metadata, Json, Error)) return false;
    const auto Bones = Json->GetObjectField(TEXT("bones"));
    Vehicle->ResetPose();
    if (!Vehicle->SnapClip(ClipId, Scorpius)) return false;
    bool OK = true;
    int32 Positions = 0;
    int32 BindEndpoints = 0;
    int32 CableChecks = 0;
    double MaxPosition = 0;
    double MaxBindPosition = 0;
    double MaxCableAngle = 0;
    TSet<USkeletalMesh*> Checked;
    for (USCRigComponent* Rig : Vehicle->Rigs)
    {
        USkeletalMesh* Mesh = Rig->GetSkeletalMeshAsset();
        if (Checked.Contains(Mesh)) continue;
        Checked.Add(Mesh);
        const auto* Binding = Clip->Bindings.FindByPredicate([&](const FSCClipBinding& B) { return B.Mesh == Mesh; });
        if (!Binding) continue;
        FSCDerivedMesh Original;
        if (!FSCMeshImporter::Derive(Source, Rig->MeshAsset, Original, Error)) return false;
        for (const auto& Track : Binding->Tracks)
        {
            const auto& Node = Original.RigNodes[Track.Bone - 1];
            const auto& Pose = Rig->LocalPose[Track.Bone];
            if (Gladius && Node.SourceName == TEXT("LG_L_Back_CableTop_Skin_bn"))
            {
                const double Angle = FMath::RadiansToDegrees(Pose.GetRotation().AngularDistance(Node.Local.GetRotation()));
                MaxCableAngle = FMath::Max(MaxCableAngle, Angle);
                ++CableChecks;
                OK &= Angle <= 0.05;
                UE_LOG(LogSCImport, Display, TEXT("SOURCE_CABLE_FRAME0 bone=%s degrees=%.9f tolerance_degrees=0.05"), *Node.SourceName,
                       Angle);
            }
            if (!Scorpius || !Track.bPosition) continue;
            const auto Entry = Bones->TryGetField(FString::Printf(TEXT("0x%X"), FSCAnimImporter::BoneHash(Node.SourceName)));
            if (!Entry)
            {
                OK = false;
                continue;
            }
            TArray<TSharedPtr<FJsonValue>> Variants;
            if (Entry->Type == EJson::Array)
                Variants = Entry->AsArray();
            else
                Variants.Add(Entry);
            TSharedPtr<FJsonObject> Channel;
            int32 Best = MIN_int32;
            FString BestKey;
            for (const auto& V : Variants)
            {
                const auto O = V->AsObject();
                FString Name, Path;
                O->TryGetStringField(TEXT("source_node_name"), Name);
                O->TryGetStringField(TEXT("source_skeleton_path"), Path);
                const int32 Score = FSCAnimImporter::ProvenanceScore(Node.SourceName, Rig->MeshAsset, Name, Path);
                const FString Key = Path + TEXT("/") + Name;
                if (!Channel || Score > Best || (Score == Best && Key < BestKey))
                {
                    Channel = O;
                    Best = Score;
                    BestKey = Key;
                }
            }
            // Reconstruct the original native local from GLB hierarchy, then apply the native delta.
            // This independently exercises both the sidecar swap and GLB matrix conversion.
            const FMatrix NativeLocal =
                Node.Parent == INDEX_NONE ? Node.World : Node.World * Original.RigNodes[Node.Parent].World.Inverse();
            const FVector NativeBind = NativeLocal.GetOrigin();
            const auto& Samples = Channel->GetArrayField(TEXT("position"));
            const auto& Last = Samples.Last()->AsArray();
            const FVector NativeLast(Last[0]->AsNumber(), Last[2]->AsNumber(), -Last[1]->AsNumber());
            const FVector NativeDelta = NativeLast - NativeBind;
            const FVector Expected = Node.Local.GetTranslation() + FSCMeshImporter::Transform(FTranslationMatrix(NativeDelta)).GetOrigin();
            const double Distance = FVector::Distance(Pose.GetTranslation(), Expected);
            MaxPosition = FMath::Max(MaxPosition, Distance);
            ++Positions;
            OK &= Distance <= 0.01;
            // Seven endpoints actually coincide with bind. Other bones have authored endpoint offsets.
            if (NativeDelta.Size() <= 0.0001)
            {
                const double BindDistance = FVector::Distance(Pose.GetTranslation(), Node.Local.GetTranslation());
                MaxBindPosition = FMath::Max(MaxBindPosition, BindDistance);
                ++BindEndpoints;
                OK &= BindDistance <= 0.01;
            }
            if (Node.SourceName == TEXT("BONE_Front_Base_Arm"))
            {
                OK &= FVector::Distance(Pose.GetTranslation(), Node.Local.GetTranslation()) <= 0.01;
                const FVector World = Rig->GetBoneTransform(Track.Bone).GetLocation();
                UE_LOG(LogSCImport, Display,
                       TEXT("SOURCE_FRONT_BASE_ARM local_cm=(%.9f,%.9f,%.9f) world_cm=(%.9f,%.9f,%.9f) bind_error_cm=%.9f"),
                       Pose.GetTranslation().X, Pose.GetTranslation().Y, Pose.GetTranslation().Z, World.X, World.Y, World.Z,
                       FVector::Distance(Pose.GetTranslation(), Node.Local.GetTranslation()));
            }
        }
    }
    OK &= !Scorpius || (Positions == 15 && BindEndpoints == 7);
    OK &= !Gladius || CableChecks > 0;
    UE_LOG(LogSCImport, Display,
           TEXT("SOURCE_POSE_CHECK passed=%d positions=%d max_position_cm=%.9f tolerance_cm=0.01 bind_endpoints=%d max_bind_cm=%.9f "
                "cable_checks=%d max_cable_degrees=%.9f"),
           OK, Positions, MaxPosition, BindEndpoints, MaxBindPosition, CableChecks, MaxCableAngle);
    Vehicle->ResetPose();
    return OK;
}
