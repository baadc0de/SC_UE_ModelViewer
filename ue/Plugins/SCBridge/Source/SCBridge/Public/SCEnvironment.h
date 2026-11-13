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
// The ship elevator PlaceOnFloor stands the ship on: world bounds of that part in the current pose (rigs alive: before a
// cook bakes them). False without one.
SCBRIDGE_API bool FindShipBay(const ASCVehicle* Environment, FBox& OutBay, FString* OutName = nullptr);
// Clear height above the bay's top: the lowest environment surface hit by vertical traces over its centre and four inset
// points (cm); negative when nothing is hit within MaxClearanceCm (an open roof). Ignore (the shown ship) is not traced.
SCBRIDGE_API double BayClearance(const ASCVehicle* Environment, const FBox& Bay, const AActor* Ignore = nullptr);
constexpr double MaxClearanceCm = 50000.0;
// Ships come in through the hangar's largest moving part above the bay (its doors; the bay and the shaft covers around it
// are at floor level): true when that part lies flat (a roof hatch: its thinnest axis is vertical), false when it
// stands (front doors) or there is none.
SCBRIDGE_API bool IsTopLoading(const ASCVehicle* Environment, const FBox& Bay);
// World bounds of what a ship occupies: visible, shadow-casting primitives (not glass-free glow cards, light beams,
// hidden Lumen proxies or game-collision helpers).
SCBRIDGE_API FBox SolidBounds(const ASCVehicle* Ship);
// Rigid parts and lights under each rig are re-parented to the actor root in their current (posed) transform and the
// rigs are removed: a static environment for the cook (animation is not kept).
SCBRIDGE_API FSCRigBakeStats BakeRigsStatic(ASCVehicle* Environment);
}
