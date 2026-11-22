// Runtime animation import API; CPU binding is independent of skeletal UObject construction.
#pragma once
#include "CoreMinimal.h"
#include "SCClipCatalog.h"
#include "SCMeshImporter.h"
#include "SCPackageReader.h"
class SCBRIDGE_API FSCAnimImporter
{
public:
    static uint32 BoneHash(const FString& Name);
    /** Sidecar Blender basis -> raw GLB-node basis -> UE. Wrapper remains a bone, applied once. */
    static FVector SidecarPosition(const FVector& V);
    static FQuat SidecarRotation(const FQuat& Q);
    static FQuat LinearRotation(FQuat A, FQuat B, double Alpha);
    static int32 ProvenanceScore(const FString& ReceiverName, const FString& ReceiverPath, const FString& SourceName,
                                 const FString& SourcePath);
    static bool Read(const FSCPackageSource& Source, const FSCClipRecord& Record, const FString& Owner, FSCImportedClip& Clip,
                     TSharedPtr<FJsonObject>& Json, FString& Error);
    static bool DeriveBinding(const TSharedPtr<FJsonObject>& Json, const FSCDerivedMesh& Rig, const FString& Path, FSCImportedClip& Clip,
                              FSCClipBinding& Binding, FString& Error);
    static void AttachBinding(USkeletalMesh* Mesh, UObject* Outer, FSCImportedClip& Clip, FSCClipBinding Binding);
    static bool Bind(const TSharedPtr<FJsonObject>& Json, const FSCDerivedMesh& Rig, const FString& Path, USkeletalMesh* Mesh,
                     UObject* Outer, FSCImportedClip& Clip, FString& Error);
    static void Finalize(USkeletalMesh* RootMesh, UObject* Outer, FSCImportedClip& Clip);
    static FTransform Sample(const FSCBoneTrack& Track, double Frame);
    /** Clip frame -> the track's own frame. Each rig plays its own animation of the clip's name: forward they start
     *  together, backward each starts at its own end. */
    static double TrackFrame(const FSCImportedClip& Clip, const FSCBoneTrack& Track, double ClipFrame, bool bFromEnd);
};
