// Cargo planner (WP-K1): pure C++ placement of item footprints on inventory-container lattices and transfer planning.
// No UObjects, no world: the subsystem builds the models from its grids and boxes, the planner decides where boxes go,
// what is refused (with a human reason) and which amounts are achievable with whole boxes. Automation-tested
// (SCBridge.Cargo.*).
//
// Lattice axes are the grid component's UE axes: X = SC y, Y = SC x, Z = SC z (up); see FSCCargoLattice::FromSC.
#pragma once
#include "CoreMinimal.h"
#include "Cargo/SCCargoTypes.h"

/** A container's lattice: cells of the minimum permitted item size per axis (data), the max permitted item size. */
struct SCBRIDGE_API FSCCargoLattice
{
    FIntVector Cells = FIntVector::ZeroValue;
    FVector CellM = FVector::ZeroVector;
    FVector InteriorM = FVector::ZeroVector;
    /** Authored max permitted item size; a zero component carries no authored limit on that axis (the interior bounds). */
    FVector MaxItemM = FVector::ZeroVector;
    TArray<FString> ExcludedSubTypes;

    /** SC (x, y, z) -> lattice axes (y, x, z). */
    static FVector FromSC(const FVector& SC) { return FVector(SC.Y, SC.X, SC.Z); }
    /** False (with Error) when the record has no usable interior or minimum item size. */
    static bool FromContainer(const FSCCargoContainerParams& Container, FSCCargoLattice& Out, FString& Error);
    int32 NumCells() const { return Cells.X * Cells.Y * Cells.Z; }
    FVector SizeM() const { return FVector(Cells) * CellM; }
    /** Cells an item of SizeM (lattice axes) spans: whole cells, rounded up past a 1 mm tolerance. */
    FIntVector CellsFor(const FVector& SizeM) const;
};

/** A movable item: a box (or a dock module) with its occupancy in lattice axes at yaw 0. */
struct SCBRIDGE_API FSCCargoItem
{
    int32 Id = INDEX_NONE;
    FVector SizeM = FVector::ZeroVector;
    int32 SCU = 0;
    double MassKg = 0;
    FString SubType;
    FName Commodity;
    bool bMovable = true; // false: a fixed module (drone dock) that only occupies cells
    double Volume = 0;    // in the beam's volume unit (SCU, see FSCCargoPlanInput::BeamMaxVolume); 0 = no volume limit
};

struct SCBRIDGE_API FSCCargoPlacement
{
    int32 ItemId = INDEX_NONE;
    FIntVector Origin = FIntVector::ZeroValue; // min-corner cell
    FIntVector Cells = FIntVector::ZeroValue;  // as placed (x/y swapped when rotated)
    bool bRotated = false;                     // yaw 90 degrees on the lattice
    int32 KeepClear = 0;                       // cells around it (horizontally) no other item may take: a dock's bays
    bool Overlaps(const FSCCargoPlacement& O) const;
    /** O rests on this placement: O's bottom is this one's top and their footprints overlap. */
    bool Supports(const FSCCargoPlacement& O) const;
};

/** One grid as the planner sees it: its lattice and what occupies it (placed and reserved). */
struct SCBRIDGE_API FSCCargoGridModel
{
    int32 GridId = INDEX_NONE;
    FSCCargoLattice Lattice;
    TArray<FSCCargoPlacement> Placed;
    FString Label;      // grid label, for reasons
    /** A hold entered from one side (its open entrance along lattice X = 1 or Y = 2; 0 = any): boxes are stowed with
     *  their long side along it, so they can come in and out lengthwise. */
    int32 PreferredLongAxis = 0;
    FString GroupLabel; // owner label ("MISC Hull A", "Freight elevator")

    bool IsFree(const FIntVector& Origin, const FIntVector& Cells, int32 IgnoreItem = INDEX_NONE) const;
    /** Gravity: on the floor, or every footprint cell directly on an occupied cell. */
    bool IsSupported(const FIntVector& Origin, const FIntVector& Cells, int32 IgnoreItem = INDEX_NONE, const TSet<int32>* NoSupport = nullptr) const;
    /** Items resting on ItemId and on those, transitively (what must come off before it can be lifted), top first. */
    TArray<int32> CoverOf(int32 ItemId) const;
    /** Items resting directly on ItemId. */
    TArray<int32> ItemsOn(int32 ItemId) const;
    const FSCCargoPlacement* Find(int32 ItemId) const;
};

/** Everything a transfer plan depends on. Grids ordered by preference (nearest first). */
struct SCBRIDGE_API FSCCargoPlanInput
{
    TArray<FSCCargoGridModel> Grids;
    TMap<int32, FSCCargoItem> Items;
    TSet<int32> Busy; // items already reserved by another job
    /** Location ids (WP-K2): grids (In.Grids) or beacons (Beacons). */
    TArray<int32> FromGrids, ToGrids;
    /** Beacons (loose cargo, WP-K2): beacon id -> its member items. Sources only (a beacon in ToGrids is refused);
     *  members are never covered, so they take part in no dig-out. */
    TMap<int32, TArray<int32>> Beacons;
    TMap<int32, FString> BeaconLabels; // for reasons
    FName Commodity;
    int32 AmountSCU = 0;
    /** Lift per drone (N), gravity (m/s^2) and the drones the destination's docks hold; DronesAvailable 0 = no drones. */
    double LiftForceN = 0;
    /** Drone mass (kg) and the least acceleration a loaded team must keep (m/s^2): with them, a team is the smallest N
     *  with (N F - m g) / (N m_drone + m) >= MinLoadAccel. Zero MinLoadAccel: N F >= m g. */
    double DroneMassKg = 0, MinLoadAccelMS2 = 0;
    double GravityMS2 = 0;
    /** The volume one beam takes (the tractor action's maxVolume, in SCU: the unit the boxes' occupancy volume is
     *  authored in); a box larger than that needs that many beams together. 0 = no volume limit. */
    double BeamMaxVolume = 0;
    int32 DronesAvailable = 0;
    /** Labels used in reasons: the commodity's. */
    FString CommodityLabel;
};

class SCBRIDGE_API FSCCargoPlanner
{
public:
    /** Whether Size fits the lattice's max item size and interior at yaw 0 or 90; the reason when not. */
    static bool FitsMaxItem(const FSCCargoLattice& Lattice, const FVector& SizeM, bool& bOutNeedsRotation, FString* OutWhy = nullptr,
                            const FString& GroupLabel = FString(), int32 SCU = 0);
    /** Best free, supported slot for Item on Grid (lowest layer, most contact, then y, x); yaw 0 before 90. */
    static bool FindSlot(const FSCCargoGridModel& Grid, const FSCCargoItem& Item, FSCCargoPlacement& Out);
    /** FindSlot where items in NoSupport bear nothing and Reject vetoes candidate slots. */
    static bool FindSlot(const FSCCargoGridModel& Grid, const FSCCargoItem& Item, FSCCargoPlacement& Out, const TSet<int32>* NoSupport,
                         const TFunction<bool(const FSCCargoPlacement&)>& Reject);
    /** Dig-out of Target on Grid (Sources[GridIndex]; Sources are working copies, updated): its cover comes off top
     *  first; the lowest covering boxes are held (lifted aside and put back) while the drones left after Target's team
     *  allow, the others are relocated to free cells of the source grids (not over Target). Appends Relocate / Hold
     *  moves, then removes Target from the working grid. False with Why when the cover cannot be cleared. */
    static bool PlanDigOut(const FSCCargoPlanInput& In, TArray<FSCCargoGridModel>& Sources, int32 GridIndex, int32 Target, int32 TargetTeam,
                           TFunctionRef<int32(const FSCCargoItem&)> TeamFor, TArray<FSCCargoMovePlan>& OutMoves, FString& Why);
    /** Packs Items (largest first) onto Grids in order, appending to the grids' Placed lists; false if any does not fit.
     *  OutPlacements[i] holds the slot of Items[i] and OutGrid[i] its grid index. */
    /** Where a held box goes back, decided when it goes back: the planned slot while it is still free and supported in
     *  Grid as it is now (ignoring the box's own entry), otherwise the best free slot there (a cancelled dig-out leaves the
     *  target in place, so a slot planned for after its removal may overlap it). False when nothing fits. */
    static bool ReturnSlot(const FSCCargoGridModel& Grid, const FSCCargoItem& Item, const FSCCargoPlacement& Planned, FSCCargoPlacement& Out);
    static bool Pack(TArray<FSCCargoGridModel>& Grids, const TArray<FSCCargoItem>& Items, TArray<FSCCargoPlacement>& OutPlacements,
                     TArray<int32>& OutGrid);
    /** Drones needed to lift MassKg: ceil(m g / F); 1 weightless (g = 0: space); 0 when the lift is unknown. */
    static int32 DronesFor(double MassKg, double GravityMS2, double LiftForceN);
    /** A box's team: max(by force: lift against its weight, with the input's acceleration reserve; by volume:
     *  ceil(volume / beam max volume)); 0 when no team of up to max(MaxDrones, by volume) keeps the
     *  acceleration reserve. It may exceed MaxDrones (the caller refuses with the count). The two terms in OutByForce /
     *  OutByVolume (for the log). */
    static int32 TeamSize(const FSCCargoPlanInput& In, const FSCCargoItem& Item, int32 MaxDrones, int32* OutByForce = nullptr, int32* OutByVolume = nullptr);
    /** The smallest team that lifts MassKg and still accelerates at MinAccel; 0 when no team up to MaxDrones can. */
    static int32 DronesFor(double MassKg, double GravityMS2, double LiftForceN, double DroneKg, double MinAccelMS2, int32 MaxDrones);
    /** Pick-time fallback: another box for a move whose box could not be lifted. Same commodity on the same source
     *  grids, not busy, not Excluded, that CanPick accepts and that fits the destination grids now (In.Grids carry the
     *  current reservations); the same SCU first (keeps the amount), else the planner's next best (largest first).
     *  OutWhy collects CanPick's reasons for the boxes it rejected. */
    static bool Substitute(const FSCCargoPlanInput& In, const FSCCargoMovePlan& Failed, const TSet<int32>& Excluded,
                           TFunctionRef<bool(int32 BoxId, FString& Why)> CanPick, FSCCargoMovePlan& Out, TArray<FString>& OutWhy);
    /** Plan a transfer: moves (top-down on the sources), refusals with reasons, the achievable amounts. */
    static FSCCargoTransferPreview Plan(const FSCCargoPlanInput& Input);
};
