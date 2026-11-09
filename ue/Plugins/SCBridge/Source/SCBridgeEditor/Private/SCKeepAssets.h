// Editor-host "Keep as assets": moves the loaded vehicle's transient objects into /Game/SC/<Entity>/ packages and
// saves them (textures, material instances as constants, Nanite static meshes, skeletons, clips). Runtime
// skeletal meshes have no editor source model and are skipped with a warning.
#pragma once
#include "CoreMinimal.h"
#include "SCViewerSubsystem.h"

class ASCVehicle;

namespace SCKeepAssets
{
/** Bound to USCViewerSubsystem::KeepHandler while the editor module is loaded. */
FSCKeepResult Keep(ASCVehicle* Vehicle, const FString& Entity);
}
