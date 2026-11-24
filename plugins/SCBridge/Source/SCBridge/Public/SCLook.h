// Look switches shared by the console commands (SCLook.cpp) and the viewer's startup defaults.
#pragma once
#include "CoreMinimal.h"

class AActor;
class UMaterialInterface;
class UMaterialInstanceDynamic;
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
/** -SCMaxQuality (the standalone Win64 showcase, viewer/scripts/package_showcase.ps1): Lumen global illumination and
 *  reflections at Cinematic (BaseScalability @Cine: 8x screen probes, larger trace budgets, front-layer translucency
 *  reflections) and hardware ray tracing with hit lighting for GI and reflections and area shadows at hits. Applied
 *  once the game's own settings are (the subsystem's first tick with a world); logs the effective values. */
void ApplyMaxQuality();
/** Which of the owner's refraction profiles a glass instance takes (viewer/verification/WP-U3b.md §14.10), from data only:
 *  Canopy = glass authored not to reflect (game F0, SpecularColor.r, below 1e-6: the reflectance floor's own test);
 *  Kit = glass on the first-person player kit actor (USCPlayerBodyComponent::GetKit(): weapon optics, visor);
 *  Default = every other glass. */
enum class EGlassProfile : uint8 { Canopy, Kit, Default };
/** The owner's tuned refraction look per profile (in person, C2 demo, 2026-11-19): the values SetGlassRefraction puts on each
 *  glass instance it switches on. IOR and thickness keep the master defaults. The dispersion is the colour fringing, rendered by
 *  the fork's per-channel distortion pass (WP-E1 E1-8, r.Refraction.Dispersion). */
struct FGlassRefractionProfile
{
    float Strength;   // GlassRefractionStrength
    float Dispersion; // GlassDispersion
};
inline constexpr FGlassRefractionProfile GlassProfileCanopy{1.f, 0.25f};
inline constexpr FGlassRefractionProfile GlassProfileKit{0.125f, 0.25f}; // owner, in person (live): 5 / 1 broke the optic; settled 0.125 / 0.25
inline constexpr FGlassRefractionProfile GlassProfileDefault{5.f, 0.5f};
const FGlassRefractionProfile& GlassRefractionProfile(EGlassProfile Profile);
/** The profile of a glass instance: Kit when it is on a player kit, else Canopy when its F0 (SpecularColor.r, through its
 *  parents: a cooked constant's value) is zero, else Default. Pure. */
SCBRIDGE_API EGlassProfile SelectGlassProfile(const UMaterialInterface* Glass, bool bOnPlayerKit);
/** Puts the profile's values on a glass instance. */
void ApplyGlassProfile(UMaterialInstanceDynamic* MID, EGlassProfile Profile);
/** The player kit actors of the world's first-person bodies (USCPlayerBodyComponent::GetKit()). */
SCBRIDGE_API TArray<const AActor*> PlayerKits(UWorld* World);
/** Glass instances SetGlassRefraction gave each profile (only while switching the look on). */
struct FGlassProfileCounts { int32 Canopy = 0, Kit = 0, Default = 0; };
/** The glass refraction look (USCViewerSettings::bGlassRefraction, M_SC_Glass GlassRefractionLook; viewer/verification/WP-U3b.md
 *  §14.10): always on in the viewer's look; off only for a session through sc.Look.GlassRefraction 0 (diagnostics). Sets the
 *  setting and the parameter on every M_SC_Glass instance of the world's vehicles and environments (instances built later take
 *  it from the setting). Switching on also puts each instance's profile on it (SelectGlassProfile, the kits from PlayerKits).
 *  A constant glass instance (a cooked ship or hangar) gets a dynamic instance over it (one per actor and constant, shared by its
 *  slots), on so it takes its profile, off so the diagnostic reaches it too. Logs
 *  `LOOK glass_refraction on=… instances=… converted=… canopy=… kit=… default=…` and one `LOOK glass_refraction_actor` line per
 *  vehicle with glass; returns the instances set. */
SCBRIDGE_API int32 SetGlassRefraction(UWorld* World, bool bOn);
/** SetGlassRefraction with the player kit actors given (tests); OutCounts receives the profile counts. */
SCBRIDGE_API int32 SetGlassRefraction(UWorld* World, bool bOn, TConstArrayView<const AActor*> Kits, FGlassProfileCounts* OutCounts = nullptr);
/** After a load: SetGlassRefraction(World, the setting): the loaded vehicle's glass takes its profile (on, the default) or the
 *  diagnostic off, a cooked ship's or hangar's constant glass a dynamic instance over it either way. */
SCBRIDGE_API void RefreshGlassRefraction(UWorld* World);
}
