// The four panels of the mobiGlas Cargo app (WP-K1c): Nearby grids, Grid detail (SSCMobiGlasGrids.cpp), Transfer and
// Transfers (SSCMobiGlasTransfer.cpp). Each rebuilds its rows only when the model's structure stamp changes; values in
// the rows (bars, distances, progress) bind to the model's live views.
#pragma once
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FSCMobiGlasModel;
class SVerticalBox;
class SScrollBox;
struct FSCCargoGridView;

#define SC_MOBIGLAS_PANEL(Name)                                                                                     \
    class Name : public SCompoundWidget                                                                             \
    {                                                                                                               \
    public:                                                                                                         \
        SLATE_BEGIN_ARGS(Name) {}                                                                                   \
        SLATE_ARGUMENT(TSharedPtr<FSCMobiGlasModel>, Model)                                                         \
        SLATE_END_ARGS()                                                                                            \
        void Construct(const FArguments& Args);                                                                     \
        virtual void Tick(const FGeometry& Geometry, double Time, float Delta) override;                           \
                                                                                                                    \
    private:                                                                                                        \
        void Rebuild();                                                                                             \
        TSharedPtr<FSCMobiGlasModel> Model;                                                                         \
        TSharedPtr<SVerticalBox> List, Extra; /* Extra: a second rebuilt list (grid-detail contents) */             \
        int32 BuiltStamp = 0;                                                                                       \
    };

/** Grids around the player grouped by owner: label, used / total SCU bar, access chip, distance; click selects. */
SC_MOBIGLAS_PANEL(SSCMobiGlasNearby)
/** The selected grid: title, lattice facts, layer selector, top-down lattice, contents table. */
SC_MOBIGLAS_PANEL(SSCMobiGlasDetail)
/** From / to / commodity pickers, amount slider on achievable stops, fit preview with refusals, ETA, Confirm. */
SC_MOBIGLAS_PANEL(SSCMobiGlasTransfer)
/** Transfer jobs: summary, state, progress, drones, ETA, Cancel. */
SC_MOBIGLAS_PANEL(SSCMobiGlasJobs)

#undef SC_MOBIGLAS_PANEL

namespace SCMobiGlasText
{
/** EXTENDED / RETRACTED / EXTENDING / RETRACTING for grids that open and close (Hull A plates), OPEN for fixed ones. */
FText Access(const FSCCargoGridView& Grid);
FLinearColor AccessColour(const FSCCargoGridView& Grid);
/** The grid has no open / close (fixed open: bCanClose false and no reason, see FSCCargoGridView). */
bool IsFixed(const FSCCargoGridView& Grid);
FText Scu(int32 Used, int32 Total); // "48 / 64 SCU"
FText Metres(float Metres);         // "12.4 m"
FText Duration(float Seconds);      // "1 min 20 s" / "42 s"
/** Caps state words for drones and boxes; values added after this was written (e.g. Holding / Held) fall back to the
 *  enum's own name, so a new state shows instead of breaking the build. */
FString DroneState(uint8 State); // ESCCargoDroneState
FString BoxState(uint8 State);   // ESCCargoBoxState
} // namespace SCMobiGlasText
