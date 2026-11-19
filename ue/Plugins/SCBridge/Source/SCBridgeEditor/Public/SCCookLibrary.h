// WP-C1 editor cook: an imported environment becomes a saveable level actor whose meshes and textures live in a pool
// shared across environments and whose materials are constant instances.
#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SCCookLibrary.generated.h"

class ASCVehicle;

UCLASS()
class SCBRIDGEEDITOR_API USCCookLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    /** Version of what a cook writes: the level, the pooled meshes' section topology and the material constants. The cook
     *  scripts stamp each cooked level with it (<level>.cookformat beside the .umap); a level with an older or no stamp is
     *  stale: the cook scripts report it with a recook instruction and package_showcase.ps1 refuses to package it.
     *  2: WP-I1 review (screens: M_SC_Screen / _Translucent masters and per-face panel images; pooled meshes keyed by
     *  their section topology, so a changed glass / screen split never reuses an older pooled mesh).
     *  3: WP-I1 I1-5 (panel renders at their face aspect with owner values; state images swapped per face).
     *  4: WP-I1 fix pass (screens: the DisplayScreenPS emission with the DBA multiplier and new instance constants; the
     *  panel renders' layout, owner defaults and brand). */
    static constexpr int32 FormatVersion = 4;
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
