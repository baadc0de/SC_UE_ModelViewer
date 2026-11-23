// WP-C1 editor cook: an imported environment becomes a saveable level actor whose meshes and textures live in a pool
// shared across environments and whose materials are constant instances.
#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UCScenario.h"
#include "SCCookLibrary.generated.h"

class ASCVehicle;

UCLASS()
class UNREALCITIZENEDITOR_API USCCookLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    /** Version of what a cook writes (the level, the pooled meshes' section topology and the material constants): the
     *  prep step's format, FUCPrepared::FormatVersion (its history is there). Each prepared level is stamped with it
     *  (<level>.cookformat beside the .umap); a level with an older or no stamp is stale: the prep step prepares it again
     *  and package_showcase.ps1 refuses to package it. */
    static constexpr int32 FormatVersion = FUCPrepared::FormatVersion;
    UFUNCTION(BlueprintCallable, Category = "SC|Cook")
    static int32 CookFormatVersion() { return FormatVersion; }
    /** Bakes rigs static (presented pose), re-instances, drops runtime-only proxies, keeps meshes/textures in
     *  SharedRoot (reused when an earlier environment already saved them) and materials as constants under Root, and
     *  makes the actor and its components persistent. The caller saves the level. Returns false on any failed save. */
    UFUNCTION(BlueprintCallable, Category = "SC|Cook")
    static bool CookEnvironment(ASCVehicle* Environment, const FString& Root, const FString& SharedRoot = TEXT("/Game/SC/Hangars/Shared"));
    /** Measures a presented environment (rigs alive: before CookEnvironment) and records its ship capacity under
     *  Style/Name in the hangar index (USCHangarIndex::AssetPath), saving it. Returns false without a ship elevator. */
    UFUNCTION(BlueprintCallable, Category = "SC|Cook")
    static bool RecordHangarCapacity(ASCVehicle* Environment, const FString& Style, const FString& Name);
};
