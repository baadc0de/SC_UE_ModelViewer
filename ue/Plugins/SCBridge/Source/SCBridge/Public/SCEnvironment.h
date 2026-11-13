// Environment (object container) presentation shared by the viewer and the editor cook: the door fragments' presented
// pose, the ship floor placement, and baking rigs into static parts.
#pragma once
#include "CoreMinimal.h"

class AActor;
class ASCVehicle;

struct FSCRigBakeStats
{
    int32 Rigs = 0;          // rig components removed
    int32 Parts = 0;         // rigid parts and lights re-parented to the actor root in their posed transform
    int32 SkinnedDropped = 0; // rigs that also drew skinned geometry (lost when baked)
};

namespace FSCEnvironment
{
// The door fragments' end pose: "open"/"closed" snap each owner's best clip (static Opened/Closed first, then the last
// frame of Open/Close); anything else is the bind pose. Returns the clips posed.
SCBRIDGE_API int32 SetState(ASCVehicle* Environment, const FString& State);
// Moves the environment so its ship elevator (largest flat moving part >= 20 m across, in the presented pose) is centred under
// the world origin with its top at FloorZ; without one, the floor under the origin goes to FloorZ. Ignore (the shown
// ship) is not traced. Returns the offset applied.
SCBRIDGE_API FVector PlaceOnFloor(ASCVehicle* Environment, double FloorZ, const AActor* Ignore);
// Rigid parts and lights under each rig are re-parented to the actor root in their current (posed) transform and the
// rigs are removed: a static environment for the cook (animation is not kept).
SCBRIDGE_API FSCRigBakeStats BakeRigsStatic(ASCVehicle* Environment);
}
