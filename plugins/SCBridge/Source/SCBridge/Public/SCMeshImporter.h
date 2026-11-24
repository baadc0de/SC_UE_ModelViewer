#pragma once
#include "CoreMinimal.h"
#include "SCMeshes.h"
#include "SCPackageSource.h"

/** Reads a package's meshes into FSCDerivedMesh (building them is FSCMeshes). */
class SCBRIDGE_API FSCMeshImporter
{
public:
    static constexpr uint32 DerivedFormatVersion = 3; // Include engine version/build settings in a processed-cache key.
    static bool Derive(const FSCPackageSource& Source, const FString& Path, FSCDerivedMesh& Out, FString& Error);
};
