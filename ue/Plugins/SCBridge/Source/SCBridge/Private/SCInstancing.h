// Consolidation of repeated rigid parts into instanced static mesh components (environments).
#pragma once
#include "CoreMinimal.h"

class ASCVehicle;

struct FSCInstancingStats
{
    int32 Components = 0;  // static mesh components considered
    int32 Consolidated = 0; // components replaced by instances
    int32 Instanced = 0;   // instanced components created
};

namespace FSCInstancing
{
// Groups identical rigid static parts of the actor (not under a rig, no attached children) and replaces every group of
// two or more with one UInstancedStaticMeshComponent on the actor root carrying the same settings.
FSCInstancingStats Consolidate(ASCVehicle* Vehicle);
}
