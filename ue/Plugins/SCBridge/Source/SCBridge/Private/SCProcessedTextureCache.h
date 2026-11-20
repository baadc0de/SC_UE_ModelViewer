// Texture worker requests share the viewer's processed asset disk budget.
#pragma once
#include "CoreMinimal.h"

class FSCProcessedCache;

struct FSCTextureCacheRequest
{
    FSCProcessedCache* Cache = nullptr;
    FString ArchiveBuild;
    bool bForceRebuild = false;
};
