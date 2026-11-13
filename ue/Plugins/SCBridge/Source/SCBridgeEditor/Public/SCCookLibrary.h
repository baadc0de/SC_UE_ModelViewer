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
