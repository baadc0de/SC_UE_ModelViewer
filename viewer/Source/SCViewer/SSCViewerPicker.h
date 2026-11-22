// WP-O1-3: the viewer's menu (F1, SCViewerShell): an environment, a vehicle and its paint, picked from the lists the
// core chose from game data (USCViewerSubsystem::GetCatalog, FSCViewerCatalog). A choice applies at once; the menu opens
// by itself at start when the command line asks for nothing (no -SCLoad / -SCEnvironment, no scripted or RPC run).
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

private:
    struct FList
    {
        TArray<FEntry> All, Shown;
        TSharedPtr<SListView<FEntry>> View;
        FString Filter;
    };
    TSharedRef<SWidget> MakeList(FList& List, const FText& Title, const FText& Hint, TFunction<FString()> Current,
                                 TFunction<void(const FSCCatalogEntry&)> Pick);
    void Refilter(FList& List);
    void Close();
    FText StatusText() const;

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
};
