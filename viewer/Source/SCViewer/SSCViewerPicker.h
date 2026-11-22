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

    /** A paint row: INDEX_NONE is the vehicle's own. */
    struct FPaintRow
    {
        int32 Index = INDEX_NONE;
        FString Label;
    };
    using FEntry = TSharedPtr<FSCCatalogEntry>;
    using FPaint = TSharedPtr<FPaintRow>;
    /** A row's relation to the scene: in it, on its way (loading, or waiting for the running load), or neither. */
    enum class ERowState : uint8 { None, Current, Pending };

private:
    struct FList
    {
        TArray<FEntry> All, Shown;
        TSharedPtr<SListView<FEntry>> View;
        FString Filter;
    };
    TSharedRef<SWidget> MakeList(FList& List, const FText& Title, const FText& Hint, TFunction<ERowState(const FString&)> State,
                                 TFunction<void(const FString&)> Pick);
    void Refilter(FList& List);
    void Close();
    FText StatusText() const;
    void PickEnvironment(const FString& Id);
    void PickVehicle(const FString& Id);
    ERowState EnvironmentState(const FString& Id) const;
    ERowState VehicleState(const FString& Id) const;
    /** The lists' current rows (environment, vehicle, paint) scrolled into view. */
    void RevealCurrent();

    TWeakObjectPtr<USCViewerSubsystem> Subsystem;
    TFunction<void()> FocusView;
    FList Environments, Vehicles;
    TArray<FPaint> Paints;
    TSharedPtr<SListView<FPaint>> PaintView;
    uint32 CatalogRevision = MAX_uint32;
    bool bCatalogReady = false;
    FString PaintsOf; // the entity the paint rows were listed for
    bool bStartDecided = false;
    bool bWasOpen = false;
    bool bScreenMessages = true; // GAreScreenMessagesEnabled before the menu opened (hidden while open)
    // The latest choice the running load could not take yet (applied from Tick).
    TOptional<FString> QueuedEnvironment, QueuedVehicle;
};
