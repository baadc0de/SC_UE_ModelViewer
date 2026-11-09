// Model browser panel: debounced entity search into a virtualised list, recents, the view-options panel bound to
// the subsystem's USCViewOptions, and Load / Cancel with phase progress.
#pragma once
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "SCBridgeClient.h"

class USCViewerSubsystem;
class SSearchBox;
class SWrapBox;
struct FSCSearchResult;

class SSCModelBrowser : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SSCModelBrowser) {}
    SLATE_ARGUMENT(USCViewerSubsystem*, Subsystem)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args);
    virtual void Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime) override;

private:
    using FHit = TSharedPtr<FSCEntityHit>;
    void OnQueryChanged(const FText& Text);
    EActiveTimerReturnType RunSearch(double, float);
    void OnResults(const FSCSearchResult& Result, uint32 Sequence);
    TSharedRef<ITableRow> MakeRow(FHit Hit, const TSharedRef<STableViewBase>& Owner);
    void Select(const FString& Entity, const FString& Label);
    void RebuildRecents();
    FReply Load();
    TSharedRef<SWidget> MakeOptions();
    TSharedRef<SWidget> MakeChoice(const FText& Label, const TArray<int32>& Values, TFunction<int32&()> Field);
    TSharedRef<SWidget> MakeToggle(const FText& Label, TFunction<bool&()> Field);

    TWeakObjectPtr<USCViewerSubsystem> Subsystem;
    TSharedPtr<SSearchBox> SearchBox;
    TSharedPtr<SListView<FHit>> List;
    TSharedPtr<SWrapBox> Recents;
    TArray<FHit> Hits;
    FString Query, SelectedEntity, SelectedLabel;
    FText Status;
    uint32 Sequence = 0;
    TSharedPtr<FActiveTimerHandle> SearchTimer;
    bool bQueryDirty = true; // Query not yet submitted against the current archive session
    bool bWasReady = false;
    int32 RecentsBuiltFrom = INDEX_NONE;
};
