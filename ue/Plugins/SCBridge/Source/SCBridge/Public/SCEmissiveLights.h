// Emissive surfaces in Lumen (look, 2026-11-12): see SCEmissiveLights.cpp.
#pragma once
#include "CoreMinimal.h"

class UWorld;

struct SCBRIDGE_API FSCEmissive
{
    /** Hidden opaque Lumen copies of the vehicle's glowing sections Lumen cannot see; false removes them. */
    static void SetLumen(class ASCVehicle* Vehicle, bool bOn, double MinNits = 1.0);
};
