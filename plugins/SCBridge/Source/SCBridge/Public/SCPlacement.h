// WP-O1-4c-3a: placing a loaded vehicle or environment (resting a ship on the floor on its landing gear, presenting an
// environment in a state on the floor), split from USCImportLibrary: prepared levels and imports both place.
#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SCPlacement.generated.h"
class ASCVehicle;
UCLASS()
class SCBRIDGE_API USCPlacementLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    /** The viewer's resting pose: landing gear deployed (its clips), then the lowest visible part moved to FloorZ.
     *  Returns the number of gear clips played. */
    UFUNCTION(BlueprintCallable, Category="SC")
    static int32 RestOnFloor(ASCVehicle* Vehicle, double FloorZ = -5.0);
    /** The door fragments' presented pose ("open", "closed" or "bind"), then the ship elevator moved under the origin
     *  with its top at FloorZ. Returns the offset applied. */
    UFUNCTION(BlueprintCallable, Category="SC")
    static FVector PresentEnvironment(ASCVehicle* Environment, const FString& State = TEXT("open"), double FloorZ = -5.0);
};
