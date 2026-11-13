// Data-derived exploration locations and individually addressable fragment interactions.
#pragma once
#include "CoreMinimal.h"
#include "SCExploration.generated.h"
class USCRigComponent;
/** Spawn categories. Each comes from one data source; StartOrder is the first-person start ranking. */
struct SCBRIDGE_API FSCSpawnKind
{
    static const FName Entrance, SeatAccess, Seat, Interior, Door, Hull, Selected;
    static const TArray<FName>& StartOrder();
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCSpawnPoint
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FName Kind;
    UPROPERTY(BlueprintReadOnly) FString DisplayName;
    /** Vehicle-local floor/helper transform; the character resolves capsule clearance at teleport time. */
    UPROPERTY(BlueprintReadOnly) FTransform Transform;
    /** Vehicle-local box of the item's collidable geometry; invalid when the item carries none. */
    UPROPERTY(BlueprintReadOnly) FBox Bounds = FBox(ForceInit);
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCDoorInteraction
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FName Fragment;
    UPROPERTY(BlueprintReadOnly) FString DisplayName;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<USCRigComponent> Rig;
    UPROPERTY(BlueprintReadOnly) FName Joint;
    // Open state is derived from the fragment tag last played on Rig (ASCVehicle::IsDoorOpen).
};
/** A door proximity sensor (the game's Door_Ship_Sensor): while the player stands in its box the doors it drives open;
 *  ExitDelay seconds after the player left every sensor of a door that a sensor opened, the door closes. */
struct FSCDoorSensor
{
    TWeakObjectPtr<USceneComponent> Anchor; // the sensor's frame
    FBox Box = FBox(ForceInit);             // anchor-local cm
    float ExitDelay = 0;
    TArray<TWeakObjectPtr<USCRigComponent>> Doors;
};
