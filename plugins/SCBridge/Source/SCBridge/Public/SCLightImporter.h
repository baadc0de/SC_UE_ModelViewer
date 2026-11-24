// Import schema-v2 scene lights and attachment-relative projectors.
#pragma once
#include "CoreMinimal.h"
#include "SCPackageReader.h"
#include "SCLights.h"
class ASCVehicle;
class USceneComponent;
/** Builds a package's scene lights and projectors (the runtime half is FSCLights). */
class SCBRIDGE_API FSCLightImporter
{
public:
    // A projector's gobo: the package texture at Path (EXR keeps its HDR values, PNG / DDS decode linear), clamped, loaded once
    // per vehicle (Vehicle->GoboTextures, the cache key is the package path). Null when the package cannot supply it.
    static class UTexture2D* LoadGobo(const FSCPackageSource& Source, ASCVehicle* Vehicle, const FString& Path);
    static void Build(const FSCPackageSource& Source, const FSCSceneInstance& Instance, ASCVehicle* Vehicle, USceneComponent* Parent,
                      TFunction<USceneComponent*(const FString&)> Helper, TArray<FString>& Warnings, bool bHelperFrame = false);
};
