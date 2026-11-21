// Look defaults applied at startup and the environment lighting experiments of LOOK-8 (ue/verification/LOOK-8.md).
#pragma once
#include "CoreMinimal.h"

class UWorld;
class FSceneView;
class FSceneViewFamily;
class ISceneViewExtension;

namespace SCLookLighting
{
/** Select the display transform after the player view has evaluated its volumes. */
void ApplyDisplayOutput(const FSceneViewFamily& Family, FSceneView& View, bool bNativeHDR);
TSharedRef<ISceneViewExtension, ESPMode::ThreadSafe> InstallDisplayOutput(UWorld& World);
/** USCViewerSettings look defaults on every unbound post-process volume: star or standard bloom, bloom intensity and
 *  threshold (standard bloom), exposure bias over the game's compensation curve. */
void ApplyDefaults(UWorld* World);
/** USCViewerSettings::MigrateLook, saved and logged when it changed something. Before the viewer volume is installed, which reads
 *  bStarEngineSSDO. */
void MigrateLookSettings();
/** Environment spot/rect lights whose beams land almost entirely on floor outside the ship bay (perimeter floor
 *  washes, classified by tracing their emission against the environment) get Scale x their authored intensity;
 *  1 restores. Returns the lights scaled. */
int32 SetPerimeterFloorWash(UWorld* World, float Scale);
}
