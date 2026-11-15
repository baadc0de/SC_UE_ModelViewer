// Cargo domain value types (WP-K1): package records (inventory containers, item occupancy) and the read-only views
// the cargo subsystem hands to the UI (grids, boxes, drones, transfer jobs, transfer previews).
// The UI depends only on these views and USCCargoSubsystem's API, never on cargo components or actors.
#pragma once
#include "CoreMinimal.h"
#include "SCCargoTypes.generated.h"

/** `container` of an inventory-container record in scene.json (the resolved DataCore `InventoryContainer`), verbatim
 *  in SC units (metres, SC axes x/y/z as authored). */
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCCargoContainerParams
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString Record;
    UPROPERTY(BlueprintReadOnly) FString RecordGuid;
    UPROPERTY(BlueprintReadOnly) FString InventoryType;
    UPROPERTY(BlueprintReadOnly) FVector InteriorDimensions = FVector::ZeroVector; // m
    UPROPERTY(BlueprintReadOnly) FVector MinPermittedItemSize = FVector::ZeroVector; // m
    UPROPERTY(BlueprintReadOnly) FVector MaxPermittedItemSize = FVector::ZeroVector; // m
    UPROPERTY(BlueprintReadOnly) FVector GridPosOffset = FVector::ZeroVector;      // m
    UPROPERTY(BlueprintReadOnly) float GridCellSize = 0;                           // m, as authored
    UPROPERTY(BlueprintReadOnly) bool bExternalContainer = false;
    UPROPERTY(BlueprintReadOnly) TArray<FString> ExcludedItemSubTypes;
    /** The whole `container` object, lossless. */
    UPROPERTY(BlueprintReadOnly) FString Json;
};

/** One `inventory_containers[]` entry: on the vehicle (loadout item, located by port path + parent helper) or in an
 *  interior (socpak entity, located by its container-local transform). */
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCInventoryContainerRecord
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString EntityName;      // vehicle entries
    UPROPERTY(BlueprintReadOnly) FString EntityClass;     // interior entries
    UPROPERTY(BlueprintReadOnly) FString EntityClassGuid;
    UPROPERTY(BlueprintReadOnly) FString EntityId;        // interior entries: unique within the interior
    UPROPERTY(BlueprintReadOnly) TArray<FString> PortPath;       // vehicle entries
    UPROPERTY(BlueprintReadOnly) TArray<FString> ParentPortPath; // vehicle entries; empty = the root entity
    UPROPERTY(BlueprintReadOnly) FString ParentNodeName;  // helper/bone on the parent geometry
    UPROPERTY(BlueprintReadOnly) TArray<FString> Links;
    UPROPERTY(BlueprintReadOnly) int32 InteriorIndex = INDEX_NONE; // index into FSCSceneManifest::Interiors
    /** Native SC row-vector matrix (metres), same convention as FSCSceneInstance::LocalMatrix. */
    FMatrix LocalMatrix = FMatrix::Identity;
    UPROPERTY(BlueprintReadOnly) FSCCargoContainerParams Container;
};

/** `root_entity.inventory_occupancy` & co.: the footprint of an item that can sit in a container (a cargo box). */
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCItemOccupancy
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) bool bValid = false;
    UPROPERTY(BlueprintReadOnly) FVector Dimensions = FVector::ZeroVector; // m, SC axes
    /** `local_bounds_min` / `local_bounds_max`: the occupancy box in the item's own frame (m, SC axes); equal when absent. */
    UPROPERTY(BlueprintReadOnly) FVector LocalBoundsMin = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly) FVector LocalBoundsMax = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly) double Volume = 0;
    UPROPERTY(BlueprintReadOnly) FString VolumeUnit;
    UPROPERTY(BlueprintReadOnly) int32 StandardCargoUnits = 0; // 0 = not a standard cargo unit
    UPROPERTY(BlueprintReadOnly) double Mass = 0;              // raw physics-controller mass (kg)
    UPROPERTY(BlueprintReadOnly) FString ItemType;
    UPROPERTY(BlueprintReadOnly) FString ItemSubType;
    UPROPERTY(BlueprintReadOnly) FString DisplayName;
};

UENUM(BlueprintType)
enum class ESCCargoGridKind : uint8 { Vehicle, Environment };

/** Whether drones can reach the grid now. Grids on items with an open/close (deploy/retract) fragment are Closed
 *  until a job opens them. */
UENUM(BlueprintType)
enum class ESCCargoGridAccess : uint8 { Open, Opening, Closing, Closed };

/** Held: lifted out of the way by drones while a box beneath it is dug out; it goes back afterwards. */
UENUM(BlueprintType)
enum class ESCCargoBoxState : uint8 { Stored, Reserved, Lifting, InTransit, Placing, Held };

/** Holding: hovering with a box held out of the way (dig-out), waiting to put it back. */
UENUM(BlueprintType)
enum class ESCCargoDroneState : uint8 { Docked, Launching, ToPickup, Attaching, Carrying, Releasing, Returning, Docking, Holding };

/** What a planned move does: carry a box of the transfer, move a covering box to a free cell for good (relocate), or
 *  lift a covering box out of the way and put it back once the box beneath is out (hold). */
UENUM(BlueprintType)
enum class ESCCargoMoveKind : uint8 { Carry, Relocate, Hold };

UENUM(BlueprintType)
/** Queued: waiting for a shared resource (boxes or cells another transfer holds, the cargo lift busy) - it plans again
 *  and starts when they are free. */
enum class ESCCargoJobState : uint8 { Planned, OpeningGrids, Moving, ClosingGrids, Done, Cancelled, Failed, Queued };

USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCCargoBoxView
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) int32 BoxId = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly) FName Commodity;     // grouping key (stable)
    UPROPERTY(BlueprintReadOnly) FText CommodityLabel;
    UPROPERTY(BlueprintReadOnly) int32 SCU = 0;
    UPROPERTY(BlueprintReadOnly) FVector SizeM = FVector::ZeroVector; // occupancy dimensions, SC axes
    UPROPERTY(BlueprintReadOnly) double MassKg = 0;
    UPROPERTY(BlueprintReadOnly) int32 GridId = INDEX_NONE; // INDEX_NONE while carried
    UPROPERTY(BlueprintReadOnly) FIntVector Origin = FIntVector::ZeroValue; // lattice cell of the min corner
    UPROPERTY(BlueprintReadOnly) FIntVector Cells = FIntVector::ZeroValue;  // footprint in lattice cells (as placed)
    UPROPERTY(BlueprintReadOnly) ESCCargoBoxState State = ESCCargoBoxState::Stored;
    UPROPERTY(BlueprintReadOnly) int32 JobId = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly) FVector WorldCenter = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly) FBox WorldBounds = FBox(ForceInit);
    /** Owner (multi-owner hangars): whose it is; bMine false = shown dimmed and never offered for a transfer.
     *  Declared by K1c, filled by K1b ("Shared" for the hangar's grids and drones). */
    UPROPERTY(BlueprintReadOnly) FText OwnerLabel;
    UPROPERTY(BlueprintReadOnly) bool bMine = true;
};

USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCCargoGridView
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) int32 GridId = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly) ESCCargoGridKind Kind = ESCCargoGridKind::Vehicle;
    /** Owner group (one ship, one elevator platform): grids of one group are offered together ("to: ship"). */
    UPROPERTY(BlueprintReadOnly) FName GroupKey;
    UPROPERTY(BlueprintReadOnly) FText GroupLabel; // e.g. ship display name, "Freight elevator"
    UPROPERTY(BlueprintReadOnly) FText GridLabel;  // e.g. "Grid 2 · bottom left" (derived from the port path)
    UPROPERTY(BlueprintReadOnly) FIntVector Cells = FIntVector::ZeroValue;       // lattice size (x, y, z up)
    UPROPERTY(BlueprintReadOnly) FVector CellSizeM = FVector::ZeroVector;        // lattice step
    UPROPERTY(BlueprintReadOnly) FVector MaxItemSizeM = FVector::ZeroVector;     // permitted item size
    UPROPERTY(BlueprintReadOnly) int32 CapacitySCU = 0;
    UPROPERTY(BlueprintReadOnly) int32 UsedSCU = 0;
    UPROPERTY(BlueprintReadOnly) int32 ReservedSCU = 0; // incoming boxes of active jobs
    UPROPERTY(BlueprintReadOnly) ESCCargoGridAccess Access = ESCCargoGridAccess::Open;
    /** Whether RequestGridAccess(Id, false) may retract the grid now; a loaded grid cannot. The UI reads false with
     *  an empty CloseBlockedReason as "no open/close at all" (fixed open, e.g. an elevator platform) and offers no
     *  Extend / Retract. Declared by K1c from the coordinator's API note; filled by K1b (GetGrid). */
    UPROPERTY(BlueprintReadOnly) bool bCanClose = false;
    UPROPERTY(BlueprintReadOnly) FText CloseBlockedReason; // e.g. "Grid 2 holds 16 SCU; unload it before retracting"
    UPROPERTY(BlueprintReadOnly) float DistanceM = 0; // from the viewer passed to GetGrids
    /** Grid-local lattice frame in world space: origin at the lattice min corner, X/Y along cells, Z up (cm). */
    UPROPERTY(BlueprintReadOnly) FTransform LatticeToWorld;
    UPROPERTY(BlueprintReadOnly) FBox WorldBounds = FBox(ForceInit);
    UPROPERTY(BlueprintReadOnly) TArray<FSCCargoBoxView> Boxes;
    /** Cells held by non-box items (drone dock), lattice coordinates. */
    UPROPERTY(BlueprintReadOnly) TArray<FIntVector> BlockedCells;
    /** Owner (multi-owner hangars): whose it is; bMine false = shown dimmed and never offered for a transfer.
     *  Declared by K1c, filled by K1b ("Shared" for the hangar's grids and drones). */
    UPROPERTY(BlueprintReadOnly) FText OwnerLabel;
    UPROPERTY(BlueprintReadOnly) bool bMine = true;
};

USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCCargoCommodityView
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FName Commodity;
    UPROPERTY(BlueprintReadOnly) FText Label;
    UPROPERTY(BlueprintReadOnly) int32 TotalSCU = 0;
    UPROPERTY(BlueprintReadOnly) TArray<int32> BoxSizesSCU; // distinct, ascending
};

USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCCargoTransferRequest
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadWrite) TArray<int32> FromGrids;
    UPROPERTY(BlueprintReadWrite) TArray<int32> ToGrids;
    UPROPERTY(BlueprintReadWrite) FName Commodity;
    UPROPERTY(BlueprintReadWrite) int32 AmountSCU = 0;
    /** Who asks (multi-owner hangars): only their boxes move, only to grids they may use; None = the player. */
    UPROPERTY(BlueprintReadWrite) FName Requester;
};

USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCCargoMovePlan
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) int32 BoxId = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly) int32 SCU = 0;
    UPROPERTY(BlueprintReadOnly) int32 FromGrid = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly) int32 ToGrid = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly) FIntVector Origin = FIntVector::ZeroValue;
    UPROPERTY(BlueprintReadOnly) FIntVector Cells = FIntVector::ZeroValue;
    UPROPERTY(BlueprintReadOnly) bool bRotated = false; // yaw 90 degrees on the destination lattice
    UPROPERTY(BlueprintReadOnly) int32 DronesNeeded = 1;
    /** Carry (the transfer's boxes; SCU counts), Relocate or Hold (dig-out of ForBoxId; a hold returns to its own slot:
     *  ToGrid = FromGrid, Origin = its cell). */
    UPROPERTY(BlueprintReadOnly) ESCCargoMoveKind Kind = ESCCargoMoveKind::Carry;
    UPROPERTY(BlueprintReadOnly) int32 ForBoxId = INDEX_NONE;
};

/** A refusal of part of a request, with the human reason ("24 SCU box is 7.5 m long; Hull A grids take up to 5.0 m"). */
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCCargoRefusal
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) int32 BoxId = INDEX_NONE; // INDEX_NONE: about the request as a whole
    UPROPERTY(BlueprintReadOnly) int32 SCU = 0;
    UPROPERTY(BlueprintReadOnly) FText Reason;
};

USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCCargoTransferPreview
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) bool bValid = false; // at least one move and nothing blocking
    UPROPERTY(BlueprintReadOnly) int32 RequestedSCU = 0;
    UPROPERTY(BlueprintReadOnly) int32 PlannedSCU = 0;
    UPROPERTY(BlueprintReadOnly) TArray<FSCCargoMovePlan> Moves;
    UPROPERTY(BlueprintReadOnly) TArray<FSCCargoRefusal> Refusals;
    /** Amounts (SCU, ascending) this source/destination/commodity can satisfy exactly with whole boxes that fit:
     *  the stops of the amount slider. */
    UPROPERTY(BlueprintReadOnly) TArray<int32> AchievableSCU;
    UPROPERTY(BlueprintReadOnly) float EstimatedSeconds = 0;
};

USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCCargoJobView
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) int32 JobId = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly) ESCCargoJobState State = ESCCargoJobState::Planned;
    UPROPERTY(BlueprintReadOnly) FText Summary; // "Organic · 32 SCU · Freight elevator → MISC Hull A"
    UPROPERTY(BlueprintReadOnly) FText StateText;
    UPROPERTY(BlueprintReadOnly) FText FailureReason;
    UPROPERTY(BlueprintReadOnly) int32 TotalSCU = 0;
    UPROPERTY(BlueprintReadOnly) int32 DoneSCU = 0;
    UPROPERTY(BlueprintReadOnly) int32 Moves = 0;
    UPROPERTY(BlueprintReadOnly) int32 MovesDone = 0;
    UPROPERTY(BlueprintReadOnly) float Progress = 0; // 0..1
    UPROPERTY(BlueprintReadOnly) float EtaSeconds = 0;
    UPROPERTY(BlueprintReadOnly) TArray<int32> Drones;
    UPROPERTY(BlueprintReadOnly) TArray<int32> FromGrids;
    UPROPERTY(BlueprintReadOnly) TArray<int32> ToGrids;
    /** Owner (multi-owner hangars): whose it is; bMine false = shown dimmed and never offered for a transfer.
     *  Declared by K1c, filled by K1b ("Shared" for the hangar's grids and drones). */
    UPROPERTY(BlueprintReadOnly) FText OwnerLabel;
    UPROPERTY(BlueprintReadOnly) bool bMine = true;
};

USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCCargoDroneView
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) int32 DroneId = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly) ESCCargoDroneState State = ESCCargoDroneState::Docked;
    UPROPERTY(BlueprintReadOnly) FText Label; // "Drone 1"
    UPROPERTY(BlueprintReadOnly) int32 JobId = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly) int32 CarryingBoxId = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly) FVector WorldLocation = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly) FBox WorldBounds = FBox(ForceInit);
    /** Owner (multi-owner hangars): its dock's owner ("Shared" for the hangar's docks). Declared by K1c, filled by K1b. */
    UPROPERTY(BlueprintReadOnly) FText OwnerLabel;
};
