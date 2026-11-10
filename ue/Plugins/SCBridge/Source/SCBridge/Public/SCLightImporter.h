// Import schema-v2 scene lights and attachment-relative projectors.
#pragma once
#include "CoreMinimal.h"
#include "SCPackageReader.h"
class ASCVehicle;
class USceneComponent;
class FJsonObject;
class SCBRIDGE_API FSCLightImporter
{
public:
    static void RefreshShadows(ASCVehicle* Vehicle);
    // UE candela for one light state. Packages with the authored light fields (bulb_radius_m, …) use the game
    // renderer's model (FSCStarEngine::PointCandela / PlanarCandela; ue/verification/WP-U3b.md §13); older packages
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
    // Container-local CryEngine vector (metres) -> the axes of the component its lights hang from (still metres).
    static FVector ContainerBasis(const FVector& Cry, bool bHelperFrame);
    static void Build(const FSCPackageSource& Source, const FSCSceneInstance& Instance, ASCVehicle* Vehicle, USceneComponent* Parent,
                      TFunction<USceneComponent*(const FString&)> Helper, TArray<FString>& Warnings, bool bHelperFrame = false);
};
