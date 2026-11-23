// Data-derived exploration locations and individually addressable fragment interactions.
#pragma once
#include "CoreMinimal.h"
#include "SCInteractionRecords.h"
#include "SCHangarWaypoints.h"
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
    /** A hangar waypoint (ASCVehicle::Waypoints of the environment; never a vehicle spawn, never ranked). */
    static const FName Hangar;
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
    /** Hangar waypoints: why the record is one (SCHangarWaypoints::SourceName); None for vehicle spawns. */
    UPROPERTY(BlueprintReadOnly) FName Source;
};
/** A record the environment import chose as a waypoint: its anchor and the standing hint in the anchor's frame (cm). Saved
 *  with a prepared level (WP-O1-4b), so a waypoint on a mechanism (a kiosk on the ship elevator) follows it there too. */
USTRUCT()
struct FSCWaypointSource
{
    GENERATED_BODY()
    UPROPERTY() ESCWaypointSource Source = ESCWaypointSource::Panel;
    UPROPERTY() FString Label;
    UPROPERTY() TWeakObjectPtr<USceneComponent> Anchor;
    UPROPERTY() FVector Local = FVector::ZeroVector;
};
/** How ASCVehicle::FindInteraction found its interaction (the E-press log). */
struct FSCInteractionPick
{
    FString Via = TEXT("none"); // panel, rig, nearest, none
    FString Hit;                // the first component the view ray met ("-": none within the longest reach)
    double Distance = -1;       // to that hit, cm
    double Reach = 0;           // the reach that applied, cm
    bool bLockControl = false;  // the pad's authored LockDoor / UnlockDoor hit region
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
    /** From the door's `SCItemDoorParams` (FSCDoorTiming; the Mannequin Open / Close pair without door data). OpenTag
     *  opens; a single-procedural door (`transition_tag`) has no close fragment and closes by playing its transition
     *  backwards (bReverseClose); CloseTag is then only the recorded state. */
    FName OpenTag = TEXT("Open");
    FName CloseTag = TEXT("Close");
    bool bReverseClose = false;
    /** `is_opened`: the door's state at spawn (closed when the data has none). */
    bool bStartsOpen = false;
    /** `animation_speed` (DoorAnimationSpeed): the clip-rate scale of the door's own presses. */
    float Speed = 1.f;
    /** `item_port_links`: interactions the door forwards to the items of vehicle ports (the C2's front hatch opens the
     *  front doors and the ramp). */
    TArray<FSCItemPortLink> PortLinks;
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
