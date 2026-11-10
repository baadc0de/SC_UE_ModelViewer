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
    // UE candela for one light state. Packages with the authored light fields (bulb_radius_m, …) use CryEngine's
    // model (ue/verification/WP-U3b.md §11): pi * 1e4 * intensity_raw * bulb_radius_m^2 for point/spot lights and
    // 1e4 * intensity_raw * plane_width_m * plane_height_m for planar lights; older packages keep the x1500 proxy.
    static double StateCandela(const TSharedPtr<FJsonObject>& Light, const TSharedPtr<FJsonObject>& State);
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
