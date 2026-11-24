// WP-O1-4c-3a: the lights a vehicle carries at run time — their authored states and styles, shadows, look scales, the
// flashlight, gobo light functions, the container basis lights and audio hang from, verification and probes. Split from
// FSCLightImporter, which keeps what builds lights from a package (Build, LoadGobo), so the importer can leave the runtime
// module (SCBridgeImport, O1-4c-3).
#pragma once
#include "CoreMinimal.h"
class ASCVehicle;
struct FSCImportedLightState;
class FJsonObject;
class ULightComponent;
enum class ESCLightIntensityScale : uint8 { Wall, FloorWash };
class SCBRIDGE_API FSCLights
{
public:
    /** Component tag prefix naming the entity an attached light rides (`parent_entity_id`, WP-P5). */
    static constexpr const TCHAR* RidesTagPrefix = TEXT("SCRides:");
    static void RefreshShadows(ASCVehicle* Vehicle);
    /** Put the light's authored state StateKey (`defaultState`, `auxiliaryState`, `emergencyState`, ...) on light Index of
     *  Vehicle->LightStates: intensity (StateCandela), colour and style. False when the light authors no such state. */
    static bool ApplyState(ASCVehicle* Vehicle, int32 Index, FName StateKey);
    /** State.AuthoredStates from its light record (State.Json): each authored state's intensity, colour and style, as
     *  ApplyState puts it on. The import calls it; a prepared level saves the result. */
    static void AuthorStates(FSCImportedLightState& State);
    static void TickStyles(ASCVehicle* Vehicle);
    static void ResumeStyles(ASCVehicle* Vehicle);
    /** Set one look adjustment on a package light, returning false for legacy components. */
    static bool SetIntensityScale(ASCVehicle* Vehicle, ULightComponent* Light, ESCLightIntensityScale Layer, float Scale);
    // UE candela for one light state. Packages with the authored light fields (bulb_radius_m, …) use the game
    // renderer's model (FSCStarEngine::PointCandela / PlanarCandela; viewer/verification/WP-U3b.md §13); older packages
    // keep the x1500 proxy.
    static double StateCandela(const TSharedPtr<FJsonObject>& Light, const TSharedPtr<FJsonObject>& State);
    // The component class Build creates for a light record; nullptr for the records it builds nothing for (the
    // ambient proxy, a foreign transform basis, an unknown kind).
    static UClass* ComponentClass(const TSharedPtr<FJsonObject>& Light);
    // Fallback shadow rule (no MegaLights) of one scene instance: the upper-quartile attenuation radius (metres) of
    // its point, spot and area records.
    static double ShadowRadiusThreshold(const TArray<FString>& LightJson);
    // Whether the vehicle's light components are what Build makes of its manifest: order, state, the candela of the
    // intensity rule in candela units, shadow flags and gobo materials.
    static bool Verify(ASCVehicle* Vehicle);
    // Diagnostic: the Top local lights of World by unshadowed illuminance (lux at normal incidence, UE's photometric
    // model: candela / d^2, attenuation window, spot cone) at a world point, each with a visibility trace to the point.
    // ReceiverChannels (lighting-channel bits of the surface at Point; 0 = ignore channels) leaves out the lights
    // that cannot shade that surface and reports them as `gated`.
    static TArray<FString> Probe(const UWorld* World, const FVector& Point, const AActor* Ignore, int32 Top, uint8 ReceiverChannels = 0);
    // A game projector is its texture over a square frustum of half-angle FOV / 2 (TiledShading samples SpotTexArray at
    // the projected position with a border sampler) with no cone falloff. The engine fork's spot light shape (WP-E1 E1-3)
    // renders exactly that: Shape SquareFrustum with OuterConeAngle = FOV / 2 (UE's light function UVs then span the
    // square: LightFunctionScale stays uniform) and LightFunctionAddress Border (black outside the gobo). The engine keeps
    // UE's 88.9-degree limit on the half-FOV. The game's near-depth cut (projDepthCutoff) is not set: the CPU mapping of
    // projectorNearPlane is unverified (ProjectorNearClip stays 0).
    static void MakeProjector(class USpotLightComponent* Spot, double HalfFovDeg);
    // The light-function material of a gobo: one M_SC_LightFunction instance per texture on the vehicle (the light function atlas
    // de-duplicates slots by material; a MID per light exhausts it). Null without a texture or the master material.
    static class UMaterialInterface* GoboLightFunction(ASCVehicle* Vehicle, class UTexture2D* Texture);
    // The game's personal light (an item's SCItemFlashlightParams split, FSCFlashlight) as UE spot-light settings: the light
    // model of every imported light (FSCStarEngine::PointCandela of the authored diffuseMult over the attenuation bulb, no
    // attenuation tweak; the bulb offset law; the forced range fade), radius = distance, the SRGB8 colour, and a projector
    // (MakeProjector) of half-angle fov / 2 whose gobo is the light cookie.
    struct FFlashlightSettings
    {
        double Candela = 0, RadiusCm = 0, BulbCm = 0, OffsetCm = 0, HalfFovDeg = 0, RangeFadeFraction = 0;
        FLinearColor Color = FLinearColor::White;
        bool bBulbOffset = false; // the bulb offset law applies (ULocalLightComponent::IsValidBulbOffsetAttenuation)
    };
    static FFlashlightSettings FlashlightSettings(const struct FSCFlashlight& Light);
    // Apply FlashlightSettings to a movable spot light; LightFunction is the cookie's (GoboLightFunction), null when unavailable.
    static void ApplyFlashlight(class USpotLightComponent* Spot, const struct FSCFlashlight& Light, class UMaterialInterface* LightFunction);
    // Container-local CryEngine vector (metres) -> the axes of the component its lights hang from (still metres).
    static FVector ContainerBasis(const FVector& Cry, bool bHelperFrame);
    /** Put light Index's style (its authored light style at the vehicle's style time) on its component. */
    static void ApplyStyle(ASCVehicle* Vehicle, int32 Index);
};
