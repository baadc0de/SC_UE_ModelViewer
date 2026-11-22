// Live cargo spawning (pure part): the game's box records (DataCore entity records with a standard-cargo-unit
// occupancy and a resource container), the catalogue of them by commodity, where a spawn's boxes go on grids (the
// planner's packing rules, refusals with reasons) and which boxes a clear may take. No UObjects, no world: the cargo
// subsystem reads the records through the FFI and places the boxes (SCCargoSpawnLive.cpp). Automation-tested
// (SCBridge.Cargo.Spawn*).
#pragma once
#include "CoreMinimal.h"
#include "Cargo/SCCargoPlanner.h"
#include "Cargo/SCCargoTypes.h"

/** One box record of the game data a live spawn can put on a grid. */
struct UNREALCITIZEN_API FSCCargoBoxRecord
{
    FString Entity;      // record name (EntityClassDefinition.<Entity>): the export key
    FString DisplayName; // the entity's localised name (entity index), may be empty
    /** The resource it holds: its default composition's first ResourceType, else its first inclusive
     *  ResourceTypeGroup (a mixed box of that group); the record's short name once resolved. */
    FName Commodity;
    FText CommodityLabel;
    int32 SCU = 0;                          // AttachDef.inventoryOccupancyVolume.standardCargoUnits, whole
    FVector DimensionsSC = FVector::ZeroVector; // AttachDef.inventoryOccupancyDimensions (m, SC axes)
    FString SubType;                        // AttachDef.SubType (grids may exclude sub types)
    FVector SizeM() const { return FSCCargoLattice::FromSC(DimensionsSC); }
};

namespace SCCargoSpawn
{
/** Reads a box record (sc_datacore_record JSON: `__type`, references as GUID strings; the CLI's `_Type_` / reference
 *  objects also accepted): SCU, dimensions, sub type, and the reference to its commodity (OutCommodityRef: a GUID or
 *  a record name; bOutGroup when it is a group). False with Why when the record is not a whole-SCU box holding a
 *  resource. Entity, DisplayName and the commodity key are the caller's. */
bool ParseBoxRecord(const FString& Json, FSCCargoBoxRecord& Out, FString& OutCommodityRef, bool& bOutGroup, FString& Why);
/** A record's short name from its JSON (`__record_name` after the type prefix); empty when absent. */
FString RecordShortName(const FString& Json);
} // namespace SCCargoSpawn

/** The box records by commodity. */
class UNREALCITIZEN_API FSCCargoBoxCatalog
{
public:
    /** Adds a record; labels and order are settled by Finish. */
    void Add(const FSCCargoBoxRecord& Record);
    /** Labels each commodity (the display name most of its boxes share, else its key) and orders the records. */
    void Finish();
    int32 Num() const { return Records.Num(); }
    const FSCCargoBoxRecord* FindEntity(const FString& Entity) const;
    /** A commodity by key or label (case-insensitive), else by text unique to one key or label. None with Why when
     *  unknown or ambiguous. */
    FName FindCommodity(const FString& Name, FString& Why) const;
    /** The commodity's box records, largest first (variants of one size too: a plan takes the first that fits). */
    TArray<const FSCCargoBoxRecord*> BoxesOf(FName Commodity) const;
    FText LabelOf(FName Commodity) const;
    /** Every commodity, by label. */
    TArray<FSCCargoSpawnCommodity> Commodities() const;

private:
    TArray<FSCCargoBoxRecord> Records;
};

namespace SCCargoSpawn
{
/** Where the boxes of a spawn go on Grids (in order; the lattices as they are now, reservations included), packed
 *  as the planner packs (largest first, best fit, upright at yaw 0 or 90, on the floor or on boxes). Boxes: the
 *  commodity's records (per size the first variant that fits; or the one record of a named entity). Request's
 *  AmountSCU / Count / SizeSCU decide the boxes (FSCCargoSpawnRequest; an amount is tried as whole boxes from the
 *  largest down, then other exact mixes, bounded); refusals: size (no such size, or none fits a grid's max item or
 *  interior), amount (no whole boxes make it, or too many boxes), full (no room for them all; FitCount: how many
 *  of the size tried fit). FirstItemId: ids the planned items take (above every id on the grids). NoSupport: items on
 *  the grids that occupy their cells but bear nothing (slots reserved for incoming boxes, boxes about to leave). */
FSCCargoSpawnPreview Plan(const TArray<FSCCargoGridModel>& Grids, const TArray<const FSCCargoBoxRecord*>& Boxes, const FSCCargoSpawnRequest& Request,
                          int32 FirstItemId, const TSet<int32>& NoSupport = TSet<int32>());
/** The most boxes one spawn may put down (a typo of a count must not fill a hangar with thousands). */
inline constexpr int32 MaxBoxes = 256;
/** Of Candidates (box ids on Grid), those a clear may take: every candidate but the ones beneath something that stays
 *  (a box of a transfer, a slot reserved for an incoming box, a dock), however deep. */
TArray<int32> Clearable(const FSCCargoGridModel& Grid, const TSet<int32>& Candidates);
} // namespace SCCargoSpawn

namespace SCCargoSpawn
{
/** A console amount word: "48" (SCU) or "x3" (boxes). NotAmount: a word of the commodity's name; Invalid: an amount
 *  that is not a whole number from 1 to 999999 ("0", "x0", "2.5", "-3", "x2.5"). */
enum class EAmountWord : uint8 { NotAmount, Amount, Invalid };
EAmountWord ParseAmountWord(const FString& Word, int32& OutScu, int32& OutCount);
/** A whole number from 1 to 999999, every character a digit. */
bool ParseWhole(const FString& Text, int32& Out);
} // namespace SCCargoSpawn
