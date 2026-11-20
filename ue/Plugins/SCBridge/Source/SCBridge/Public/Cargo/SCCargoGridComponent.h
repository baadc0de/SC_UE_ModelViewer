// A cargo grid in the world (WP-K1): one inventory container, attached to its anchor (the container entity's frame on
// the owner item's helper, so it follows the item's rig). Holds the lattice, what occupies it, its owner labels and
// access state, and maps lattice cells to world transforms. Component axes: X = SC y, Y = SC x, Z = SC z (up).
#pragma once
#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Cargo/SCCargoPlanner.h"
#include "SCCargoGridComponent.generated.h"

class ASCVehicle;
class USCRigComponent;

/** Where the lattice sits relative to the container entity's origin (the K1a contract's grid origin convention). */
enum class ESCCargoGridOrigin : uint8 { MinCorner, BottomCentre, Centre };

/** A clip that moves a grid (extends / retracts it): it animates a bone of the grid's attach chain on a rig above the
 *  grid (a cargo plate's deploy, a spindle's extension) and, evaluated off-actor, carries the grid somewhere else between
 *  its first and last frame. Which end is open is geometric (FSCCargoMover::OpenEnds): of every combination of the grid's
 *  mechanisms' ends, the one where the lattice's centre stands furthest from the owner's centre (out of the hull).
 *  Shared by every grid the same rig and clip carry. */
struct FSCCargoMover
{
    TWeakObjectPtr<USCRigComponent> Rig;
    int32 Clip = INDEX_NONE; // index in the owner vehicle's clip catalogue
    FName ClipId;
    float Seconds = 0;
    bool bForwardOpens = true; // the open pose is the clip's last frame (else its first: played backwards to open)
    double TravelCm = 0;       // how far the clip carries the grid between its ends

    /** The rule for one clip: the grid's frame at the clip's first and last frame (world), the owner's centre. False when
     *  the clip does not carry the grid (under 1 cm and 1 degree): not a mechanism of this grid. Open = the end further
     *  from Centre. */
    static bool Classify(const FTransform& Start, const FTransform& End, const FVector& Centre, bool& bOutForwardOpens, double& OutTravelCm);
    /** The open pose of a grid moved by several mechanisms, innermost first (a plate's own deploy, then the hull's that
     *  carries the plate): Point (the lattice centre, world) moved by each mechanism's world delta at its first or last
     *  frame (FirstDeltas / LastDeltas, applied inner to outer). Returns the ends as a bit mask (bit i: mechanism i open at
     *  its last frame) of the combination that puts Point furthest from Centre; ties go to more last frames. */
    static uint32 OpenEnds(const FVector& Point, const TArray<FTransform>& FirstDeltas, const TArray<FTransform>& LastDeltas, const FVector& Centre);
};

UCLASS(ClassGroup = SC)
class SCBRIDGE_API USCCargoGridComponent : public USceneComponent
{
    GENERATED_BODY()
public:
    USCCargoGridComponent();

    int32 GridId = INDEX_NONE;
    ESCCargoGridKind Kind = ESCCargoGridKind::Vehicle;
    FName GroupKey;
    FText GroupLabel, GridLabel;
    FSCInventoryContainerRecord Record;
    FSCCargoLattice Lattice;
    TWeakObjectPtr<ASCVehicle> OwnerVehicle;
    /** Boxes and modules on the lattice (item ids are the subsystem's). Reserved slots of incoming boxes too. */
    TArray<FSCCargoPlacement> Placed;
    TSet<int32> Reserved; // ids in Placed that are incoming, not yet locked
    /** Clips that extend / retract the grid; empty for a fixed grid (always open). */
    TArray<FSCCargoMover> Movers;
    ESCCargoGridAccess Access = ESCCargoGridAccess::Open;
    /** An environment grid: world seconds when the environment's moving parts in its way (gate, platform) reach the pose
     *  that opens it (FSCEnvironmentAccess); Opening until then. */
    double ReadyAt = 0;
    /** Opened by the access rule (FSCEnvironmentAccess): an environment grid, or a ship's interior hold (a grid that is
     *  not an external container and has no mechanism of its own: the ramp or doors between it and the outside). */
    bool bAccessRule = false;
    /** The access rule has been applied (FSCEnvironmentAccess::OpenGrid): at discovery for an environment grid, on the first
     *  request for a ship's interior hold, whose doors start in their authored state (C2 fixes). Closed until then. */
    bool bAccessOpened = false;
    /** The doors the last access request opened or found opening: the grid is Opening while one plays, Closed once one of
     *  them is closed again (by its pad, a reset). */
    TArray<TWeakObjectPtr<class USCRigComponent>> AccessDoors;
    /** Whose grid it is (its vehicle's owner); None = shared (the hangar's: anyone may load and unload it). */
    FName Owner;
    /** An open interior hold's entrance side (lattice X = 1, Y = 2, none = 0; -1 not probed yet): see EntranceAxis. */
    int32 EntranceAxis = -1;

    /** The grid origin rule in use (sc.Cargo.Origin changes it for verification). */
    static ESCCargoGridOrigin OriginRule;

    /** Lattice min-corner frame in world space (cm): X/Y along cells, Z up. */
    FTransform LatticeToWorld() const;
    /** World transform of an item of SizeM (lattice axes, yaw 0) standing in Slot: its centre, yawed when rotated. */
    FTransform SlotToWorld(const FSCCargoPlacement& Slot, const FVector& SizeM) const;
    FBox WorldBounds() const;
    FVector WorldCenter() const { return WorldBounds().GetCenter(); }
    FSCCargoGridModel Model() const;
    bool IsFreeCell(const FIntVector& Cell) const { return Model().IsFree(Cell, FIntVector(1)); }
    void DebugDraw(float Seconds, bool bCells) const;
};
