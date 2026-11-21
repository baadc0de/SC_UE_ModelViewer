// Persist the engine's cooked static mesh plus the importer-owned Chaos geometry.
#pragma once

#include "CoreMinimal.h"
#include "SCProcessedCache.h"

class UStaticMesh;
struct FSCDerivedMesh;
struct FSCMeshBuildStats;

namespace SCProcessedMeshCache
{
FString RenderFormat();
UStaticMesh* Load(FSCProcessedCache& Cache, const FSCProcessedCache::FReadPin& Pin, FSCMeshBuildStats& Stats);
bool Store(FSCProcessedCache& Cache, const FString& Key, UStaticMesh& Mesh, const FSCDerivedMesh& Data,
    const FSCMeshBuildStats& Stats);
}
