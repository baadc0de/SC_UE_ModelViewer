// Persist the engine's cooked static mesh plus the importer-owned Chaos geometry.
#pragma once

#include "CoreMinimal.h"

class FSCProcessedCache;
class UStaticMesh;
struct FSCDerivedMesh;
struct FSCMeshBuildStats;

namespace SCProcessedMeshCache
{
FString RenderFormat();
UStaticMesh* Load(FSCProcessedCache& Cache, const FString& Directory, FSCMeshBuildStats& Stats);
bool Store(FSCProcessedCache& Cache, const FString& Key, UStaticMesh& Mesh, const FSCDerivedMesh& Data,
    const FSCMeshBuildStats& Stats);
}
