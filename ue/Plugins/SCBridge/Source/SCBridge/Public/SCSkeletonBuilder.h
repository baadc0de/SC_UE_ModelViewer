#pragma once
#include "CoreMinimal.h"
#include "SCMeshImporter.h"
class USkeleton;
class USkeletalMesh;

/** GLB node hierarchy is the reference pose; skin inverse binds must agree with it. */
class SCBRIDGE_API FSCSkeletonBuilder
{
public:
    static USkeletalMesh* Build(const FSCDerivedMesh& Data, UObject* Outer, USkeleton*& Skeleton);
    static FName BoneName(const FSCDerivedMesh& Data, int32 Node);
};
class SCBRIDGE_API FSCSkeletalMeshImporter
{
public:
    /** Build GPU skin buffers without editor modules; empty geometry is a socket-only rig. */
    static bool Build(const FSCDerivedMesh& Data, USkeletalMesh* Mesh, FString& Error);
};
