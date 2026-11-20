// mobiGlas Cargo app state (WP-K1c), shared by the app's panels and the callout layer: polls the cargo views once per
// frame (live values for bars and callouts), bumps a structure stamp only when the source's revision or the nearby
// grid set changes (lists rebuild on that), and holds the selection and the transfer form (from / to / commodity /
// amount snapped to the preview's achievable stops).
#pragma once
#include "CoreMinimal.h"
#include "Cargo/SCCargoTypes.h"
#include "Layout/SlateRect.h"
#include "../ShipLink/SCShipLinkModel.h"

class ISCCargoViewSource;
class USCViewerSubsystem;

/** A transfer endpoint: one grid, every grid of a group ("the ship"), or a beacon (loose cargo, WP-K2: a source only). */
struct FSCGridPick
{
    FName Group;
    int32 GridId = INDEX_NONE;
    int32 BeaconId = INDEX_NONE;
    bool IsSet() const { return GridId != INDEX_NONE || !Group.IsNone() || BeaconId != INDEX_NONE; }
    bool IsBeacon() const { return BeaconId != INDEX_NONE; }
    bool IsGroup() const { return GridId == INDEX_NONE && !Group.IsNone(); }
    bool operator==(const FSCGridPick& O) const { return Group == O.Group && GridId == O.GridId && BeaconId == O.BeaconId; }
};

enum class ESCMobiGlasApp : uint8 { Cargo, ShipLink, Garage };

class FSCMobiGlasModel
{
public:
    explicit FSCMobiGlasModel(USCViewerSubsystem* InViewer);

    /** Poll the source; idempotent within a frame (the app and the callout layer both call it). */
    void Refresh(float DeltaSeconds);
    USCViewerSubsystem* GetViewer() const { return Viewer.Get(); }
    ISCCargoViewSource* GetSource() const { return Source; }
    bool HasSource() const { return Source != nullptr; }
    bool IsPreview() const;
    bool IsOpen() const;
    void SetOpen(bool bOpen);
    ESCMobiGlasApp GetApp() const { return App; }
    void SetApp(ESCMobiGlasApp InApp) { App = InApp; }
    FSCShipLinkModel& GetShipLink() { return ShipLink; }
    const FString& GetGarageEntity() const { return GarageEntity; }
    void SetGarageEntity(FString Entity)
    {
        Entity.RemoveFromStart(TEXT("EntityClassDefinition."), ESearchCase::IgnoreCase);
        GarageEntity = MoveTemp(Entity);
    }
    /** Bumps when lists must be rebuilt (revision, nearby grid set, source). */
    int32 GetStamp() const { return Stamp; }

    const TArray<FSCCargoGridView>& GetGrids() const { return Grids; }
    const FSCCargoGridView* FindGrid(int32 GridId) const;
    /** Beacons (loose cargo, WP-K2) near the player, nearest first: shown with their counts, offered as sources. */
    const TArray<FSCCargoBeaconView>& GetBeacons() const { return Beacons; }
    const FSCCargoBeaconView* FindBeacon(int32 BeaconId) const;
    const TArray<FSCCargoJobView>& GetJobs() const { return Jobs; }
    const FSCCargoJobView* FindJob(int32 JobId) const;
    const TArray<FSCCargoDroneView>& GetDrones() const { return Drones; }
    const TArray<FSCCargoBoxView>& GetMovingBoxes() const { return MovingBoxes; }
    float GetNearbyRadiusM() const { return NearbyRadiusM; }
    static bool IsActive(ESCCargoJobState State);

    /** "MISC Hull A · Grid 2": group label and the first part of the grid label. */
    FText ShortGridName(const FSCCargoGridView& Grid) const;
    FText ShortGridName(int32 GridId) const;
    /** Group label of a set of grids, or the single grid's short name. */
    FText DescribeGrids(const TArray<int32>& GridIds) const;
    /** The group's label, numbered when several groups (ships of one type) share it. */
    FText GroupName(FName GroupKey) const;
    /** "Grid 2 · bottom left": the view's grid label with the words its sibling grids share dropped (port-path noise). */
    FText GridName(const FSCCargoGridView& Grid) const;
    static FText HumanGroup(const FText& Label);
    /** Every grid of the group is the player's (other owners' ships are shown, never offered). */
    bool IsGroupMine(FName GroupKey) const;
    int32 CommodityColour(FName Commodity);

    // Selection (grid detail, highlighted callout).
    int32 GetSelectedGrid() const { return SelectedGrid; }
    void SelectGrid(int32 GridId);
    int32 GetLayer() const { return Layer; }
    void SetLayer(int32 InLayer) { Layer = InLayer; }

    // Transfer form.
    /** Endpoints to offer: grids and groups; beacons too as a source (bAsSource), never as a destination. */
    TArray<FSCGridPick> PickOptions(bool bAsSource = false) const;
    FText PickLabel(const FSCGridPick& Pick) const;
    TArray<int32> Resolve(const FSCGridPick& Pick) const;
    /** Route selector -> pick: a grid id, "ship" / "vehicle" or "elevator" / "environment" (the nearest such group, whole),
     *  or text in a grid or group label (the group when every grid of it matches). Unset when nothing matches. */
    FSCGridPick PickFor(const FString& Selector) const;
    const FSCGridPick& GetFrom() const { return From; }
    const FSCGridPick& GetTo() const { return To; }
    void SetFrom(const FSCGridPick& Pick);
    void SetTo(const FSCGridPick& Pick);
    const TArray<FSCCargoCommodityView>& GetCommodities() const { return Commodities; }
    FName GetCommodity() const { return Commodity; }
    void SetCommodity(FName InCommodity);
    int32 GetAmount() const { return Amount; }
    void SetAmount(int32 SCU);
    const FSCCargoTransferPreview& GetPreview() const { return PreviewResult; }
    /** Bumps whenever the preview was recomputed (the fit list rebuilds on it). */
    int32 GetPreviewStamp() const { return PreviewStamp; }
    /** Submit the current form; on failure the error is kept for the inline message. */
    bool Confirm();
    const FText& GetSubmitError() const { return SubmitError; }
    bool CancelJob(int32 JobId);

    /** Extend / Retract for a set of grids (one grid, or a ship whose grids share one state). */
    struct FAccessControl
    {
        bool bShown = false;   // grids that open and close, all in one state
        bool bExtend = false;  // the action: extend (retracted) or retract (extended)
        bool bEnabled = false; // not moving, and (retract) none loaded
        FText Reason;          // why it is disabled
    };
    FAccessControl GetAccessControl(const TArray<int32>& GridIds) const;
    bool RequestAccess(const TArray<int32>& GridIds, bool bOpen);
    const FText& GetAccessError() const { return AccessError; }
    /** Grids of Ids that must extend before drones can reach them. */
    TArray<int32> NeedExtending(const TArray<int32>& GridIds) const;

    /** Absolute rect the open mobiGlas leaves clear (its centre), set by the app each frame; callouts stay inside it. */
    void SetClearArea(const TOptional<FSlateRect>& Area) { ClearArea = Area; }
    const TOptional<FSlateRect>& GetClearArea() const { return ClearArea; }

private:
    void Reset();
    void FixSelection();
    void RefreshCommodities();
    void RefreshPreview();
    FSCCargoTransferRequest Request() const;

    TWeakObjectPtr<USCViewerSubsystem> Viewer;
    ISCCargoViewSource* Source = nullptr;
    uint64 LastFrame = ~0ull;
    int32 Stamp = 1, SeenRevision = INDEX_NONE;
    float ViewsAge = 1e6f; // seconds since grids (and, closed, jobs) were fetched
    bool bWasOpen = false;
    static constexpr float OpenRefreshSeconds = 0.25f, ClosedRefreshSeconds = 2.f;
    TArray<int32> SeenGridIds;
    TArray<FSCCargoGridView> Grids;
    TArray<FSCCargoBeaconView> Beacons;
    TArray<FSCCargoJobView> Jobs;
    TArray<FSCCargoDroneView> Drones;
    TArray<FSCCargoBoxView> MovingBoxes;
    float NearbyRadiusM = 0;
    TMap<FName, int32> Colours;
    int32 SelectedGrid = INDEX_NONE, Layer = 0;
    FSCGridPick From, To;
    FName Commodity;
    int32 Amount = 0;
    TArray<FSCCargoCommodityView> Commodities;
    FSCCargoTransferPreview PreviewResult;
    bool bPreviewDirty = true;
    int32 PreviewStamp = 0;
    FText SubmitError, AccessError;
    TOptional<FSlateRect> ClearArea;
    ESCMobiGlasApp App = ESCMobiGlasApp::Cargo;
    FSCShipLinkModel ShipLink;
    FString GarageEntity;
};
