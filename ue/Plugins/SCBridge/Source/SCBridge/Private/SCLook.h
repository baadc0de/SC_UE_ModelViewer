// Look switches shared by the console commands (SCLook.cpp) and the viewer's startup defaults.
#pragma once
#include "CoreMinimal.h"

class UWorld;

namespace SCLook
{
// FFT (convolution) bloom on every unbound post-process volume with a generated kernel: a sharp peak plus <Points>
// diffraction streaks of <Length> (fraction of the kernel radius); Points 0 restores the standard bloom.
void SetBloomStar(UWorld* World, int32 Points, float Length, float Intensity);
/** Ray-traced geometry segments of the world: one per section of every primitive in the ray-tracing scene (instanced
 *  components count once). On Vulkan the RHI keeps no persistent shader binding table, so any ray-generation pass
 *  rebuilds one record per segment every frame (render and RHI thread time grow with this count). */
int64 RayTracingSegments(UWorld* World, int32* OutPrimitives = nullptr);
/** Lumen short-range AO traced with hardware ray tracing (the LOOK-1 look) while the scene's ray-tracing segments stay
 *  within USCViewerSettings::TracedShortRangeAOMaxSegments, else Lumen's screen-space short-range AO: the traced pass
 *  is a ray-generation pass, so it costs the per-frame binding table above. Called after every load. */
void UpdateShortRangeAO(UWorld* World);
}
