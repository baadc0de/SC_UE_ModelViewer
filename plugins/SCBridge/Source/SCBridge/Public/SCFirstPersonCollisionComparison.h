// Test-only comparison of exact animated triangle bodies with per-bone convex bodies.
#pragma once
#include "CoreMinimal.h"
class ASCVehicle;
class USCRigComponent;
SCBRIDGE_API bool SCCompareDoorCollision(ASCVehicle* Vehicle, USCRigComponent* Rig, FName Joint);
