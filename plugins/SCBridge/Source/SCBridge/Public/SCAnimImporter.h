// Runtime animation import API; CPU binding is independent of skeletal UObject construction.
#pragma once
#include "CoreMinimal.h"
#include "SCPackageSource.h"
#include "SCClipCatalog.h"
#include "SCMeshes.h"
#include "SCSceneManifest.h"
class SCBRIDGE_API FSCAnimImporter
{
public:
    static int32 ProvenanceScore(const FString& ReceiverName, const FString& ReceiverPath, const FString& SourceName,
                                 const FString& SourcePath);
    static bool Read(const FSCPackageSource& Source, const FSCClipRecord& Record, const FString& Owner, FSCImportedClip& Clip,
                     TSharedPtr<FJsonObject>& Json, FString& Error);
    static bool DeriveBinding(const TSharedPtr<FJsonObject>& Json, const FSCDerivedMesh& Rig, const FString& Path, FSCImportedClip& Clip,
                              FSCClipBinding& Binding, FString& Error);
    static void AttachBinding(USkeletalMesh* Mesh, UObject* Outer, FSCImportedClip& Clip, FSCClipBinding Binding);
    /** Binding's editor sequences from its bone tracks on Mesh's skeleton, one per timing (rate and key count), added to
     *  Binding.Sequences (transient, in Outer). The import builds them as it binds; Keep as assets builds them for a
     *  prepared vehicle, whose bindings keep only the tracks. Editor builds only (a sequence without a data model
     *  otherwise). */
    static void BuildSequences(USkeletalMesh* Mesh, UObject* Outer, FSCClipBinding& Binding);
    static bool Bind(const TSharedPtr<FJsonObject>& Json, const FSCDerivedMesh& Rig, const FString& Path, USkeletalMesh* Mesh,
                     UObject* Outer, FSCImportedClip& Clip, FString& Error);
    static void Finalize(USkeletalMesh* RootMesh, UObject* Outer, FSCImportedClip& Clip);
};
