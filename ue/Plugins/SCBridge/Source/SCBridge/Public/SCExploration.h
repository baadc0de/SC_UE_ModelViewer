// Data-derived exploration locations and individually addressable fragment interactions.
#pragma once
#include "CoreMinimal.h"
#include "SCExploration.generated.h"
class USCRigComponent;
/** Spawn categories. Each comes from one data source; StartOrder is the first-person start ranking. */
struct SCBRIDGE_API FSCSpawnKind
{
    // One declarator per declaration: MSVC rejects the later declarators of a comma list in an exported class (C2487).
    static const FName Entrance;
    static const FName SeatAccess;
    static const FName Seat;
    static const FName Interior;
    static const FName Door;
    static const FName Hull;
    static const FName Selected;
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
/** One mesh slot a paint touches: its bound (default) instance and each paint's (null: the paint leaves it). The
 *  instances are owned by the vehicle (ASCVehicle::Materials). */
struct FSCPaintSlot
{
    TWeakObjectPtr<class UMeshComponent> Component;
    int32 Slot = 0;
    TWeakObjectPtr<class UMaterialInterface> Default;
    TArray<TWeakObjectPtr<class UMaterialInterface>> Paints;
};
struct FSCDoorSensor
{
    TWeakObjectPtr<USceneComponent> Anchor; // the sensor's frame
    FBox Box = FBox(ForceInit);             // anchor-local cm
    float ExitDelay = 0;
    TArray<TWeakObjectPtr<USCRigComponent>> Doors;
};
