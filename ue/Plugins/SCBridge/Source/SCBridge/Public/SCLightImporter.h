// Import schema-v2 scene lights and attachment-relative projectors.
#pragma once
#include "CoreMinimal.h"
#include "SCPackageReader.h"
class ASCVehicle;
class USceneComponent;
class SCBRIDGE_API FSCLightImporter
{
public:
    static void RefreshShadows(ASCVehicle* Vehicle);
    static bool Verify(ASCVehicle* Vehicle);
    static void Build(const FSCPackageSource& Source, const FSCSceneInstance& Instance, ASCVehicle* Vehicle, USceneComponent* Parent,
                      TFunction<USceneComponent*(const FString&)> Helper, TArray<FString>& Warnings, bool bHelperFrame = false);
};
