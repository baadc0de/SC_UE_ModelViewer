// WP-O1-3: the viewer's menu (F1, SCViewerShell): an environment, a vehicle and its paint, picked from the lists the
// core chose from game data (USCViewerSubsystem::GetCatalog, FSCViewerCatalog). A choice applies at once, or as soon as
// the running load lets it (the latest choice waits, marked LOADING); the menu opens by itself at start when the command
// line asks for nothing (no -SCLoad / -SCEnvironment, no scripted or RPC run).
#pragma once
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "SCViewerCatalog.h"

class USCViewerSubsystem;
class ASCVehicle;
class SEditableTextBox;

class SSCViewerPicker : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SSCViewerPicker) {}
    SLATE_ARGUMENT(USCViewerSubsystem*, Subsystem)
    SLATE_ARGUMENT(TFunction<void()>, FocusView)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args);
    virtual ~SSCViewerPicker() override;
    virtual void Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime) override;
    virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
    virtual bool SupportsKeyboardFocus() const override { return true; }
    /** Opens the menu with only the paint column (the ship stays in view), or back to all lists. */
    void ShowShip(bool bOn);

    /** A paint row of Owner's manifest: INDEX_NONE is the vehicle's own. */
    struct FPaintRow
    {
        int32 Index = INDEX_NONE;
        FString Label;
        TWeakObjectPtr<const ASCVehicle> Owner;
    };
    using FEntry = TSharedPtr<FSCCatalogEntry>;
    using FPaint = TSharedPtr<FPaintRow>;
    /** A row's relation to the scene: in it, on its way (loading, or waiting for the running load), or neither. */
    enum class ERowState : uint8 { None, Current, Pending };

    // What a click does, from the load state alone (pure; SCBridge.Viewer.PickerQueue walks the click sequences).
    /** Keep: the running load is this choice (a later one waiting behind it is dropped); Load: now; Queue: after the
     *  running load (also the vehicle it replaces, or one whose load is being cancelled); Drop: the row came from an
     *  earlier archive session's lists (nothing is requested, queued or saved). */
    enum class EVehiclePick : uint8 { Keep, Load, Queue, Drop };
    struct FVehiclePickState
    {
        int32 RowSession = 0;     // the archive session of the list the clicked row came from
        int32 Session = 0;        // the current archive session
        bool bLoading = false;    // a vehicle export, parse or import runs
        bool bCancelling = false; // ... and CancelLoad was asked
        bool bCanLoad = false;    // LoadEntity would take a request now
        FString Pending;          // the entity it loads
    };
    static EVehiclePick DecideVehiclePick(const FString& Id, const FVehiclePickState& State);
    /** There: in the scene (of this archive session) or on its way and not being stopped; Request: now; Queue: once the
     *  environment's running export, parse or import has stopped or finished; Drop: as for vehicles. */
    enum class EEnvironmentPick : uint8 { There, Request, Queue, Drop };
    struct FEnvironmentPickState
    {
        int32 RowSession = 0;     // the archive session of the list the clicked row came from
        int32 Session = 0;        // the current archive session
        FString Request;          // the subsystem's environment request ("" / "none": the procedural hangar)
        bool bInstalled = false;  // the requested one is in the scene, built in this archive session
        bool bLoading = false;    // the request is on its way (pending, exporting, parsing, importing, placing)
        bool bCancelling = false; // ... and its export or parse is being stopped
        bool bWorking = false;    // an export, parse or import runs (RequestEnvironment would wait for it)
    };
    static EEnvironmentPick DecideEnvironmentPick(const FString& Id, const FEnvironmentPickState& State);
    /** A list row's click: Pick with the row's id and the archive session of the list when the row was made (bound by
     *  value now, never read at click time). */
    static TFunction<void()> RowClick(TFunction<void(const FString&, int32)> Pick, const FString& Id, int32 RowSession);

private:
    struct FList
    {
        TArray<FEntry> All, Shown;
        TSharedPtr<SListView<FEntry>> View;
        FString Filter;
        int32 Session = 0; // the archive session the rows were listed in (the catalogue's revision)
    };
    TSharedRef<SWidget> MakeList(FList& List, const FText& Title, const FText& Hint, TFunction<ERowState(const FString&)> State,
                                 TFunction<void(const FString&, int32)> Pick);
    void Refilter(FList& List);
    void Close();
    FText StatusText() const;
    void PickEnvironment(const FString& Id, int32 RowSession);
    void PickVehicle(const FString& Id, int32 RowSession);
    ERowState EnvironmentState(const FString& Id) const;
    ERowState VehicleState(const FString& Id) const;
    /** The lists' current rows (environment, vehicle, paint) scrolled into view. */
    void RevealCurrent();
    void RestoreScreenMessages();

    TWeakObjectPtr<USCViewerSubsystem> Subsystem;
    TFunction<void()> FocusView;
    FList Environments, Vehicles;
    TArray<FPaint> Paints;
    TSharedPtr<SListView<FPaint>> PaintView;
    uint32 CatalogRevision = MAX_uint32;
    bool bCatalogReady = false;
    FString PaintsOf; // the entity and actor the paint rows were listed for
    TWeakObjectPtr<const ASCVehicle> PaintsOwner;
    bool bStartDecided = false;
    bool bWasOpen = false;
    bool bHidScreenMessages = false; // the menu turned GAreScreenMessagesEnabled off when it opened
    /** Show ship: only the paint column, over the view (the rest of the screen stays the view's: orbit while painting). */
    bool bPaintOnly = false;
    // The latest choice the running load could not take yet (applied from Tick), and the archive session it was made in
    // (the lists it came from): a choice from an earlier session is dropped, not applied to another archive.
    TOptional<FString> QueuedEnvironment, QueuedVehicle;
    TOptional<FString> SaveEnvironment; // picked, saved as the user's environment once it is in the scene
    int32 QueuedEnvironmentSession = 0, QueuedVehicleSession = 0, SaveEnvironmentSession = 0;
    /** Drops the queued and to-be-saved choices made in an archive session that is no longer the current one. */
    void DropSupersededChoices(const USCViewerSubsystem& S);

    // Custom colours of the active paint (its palette channels; USCViewerSubsystem::SetPaintColors): the rows are made again
    // when the vehicle or its paint changes.
    TSharedPtr<class SVerticalBox> ColorRows;
    TMap<FString, TSharedPtr<SEditableTextBox>> ColorFields;
    TWeakObjectPtr<const ASCVehicle> ColorsOwner;
    int32 ColorsPaint = MIN_int32;
    void RebuildColors(ASCVehicle* V);
    /** The channel's colour now: the custom one, else the paint's. */
    FLinearColor ColorOf(const FString& Channel) const;
    void ApplyColor(const FString& Channel, const FLinearColor& Linear, bool bSave);
    void ResetColors();
    void OpenColorPicker(const FString& Channel);
};
