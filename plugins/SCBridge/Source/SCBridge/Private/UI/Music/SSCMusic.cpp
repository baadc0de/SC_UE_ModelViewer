#include "SSCMusic.h"
#include "../Cargo/SCMobiGlasModel.h"
#include "../Cargo/SCMobiGlasStyle.h"
#include "SCViewerSubsystem.h"
#include "Brushes/SlateColorBrush.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SCMobiGlasMusic"
using namespace SCMobiGlasStyle;

namespace
{
FText Duration(double Seconds)
{
    const int32 WholeSeconds = FMath::FloorToInt(Seconds);
    return FText::FromString(FString::Printf(TEXT("%d:%02d"), WholeSeconds / 60, WholeSeconds % 60));
}

const FTableRowStyle& CueRowStyle()
{
    static const FTableRowStyle Style = FTableRowStyle()
        .SetEvenRowBackgroundBrush(FSlateColorBrush(Paper()))
        .SetOddRowBackgroundBrush(FSlateColorBrush(Paper()))
        .SetEvenRowBackgroundHoveredBrush(FSlateColorBrush(PaperDim()))
        .SetOddRowBackgroundHoveredBrush(FSlateColorBrush(PaperDim()))
        .SetActiveBrush(FSlateColorBrush(PaperDim()))
        .SetInactiveBrush(FSlateColorBrush(PaperDim()))
        .SetActiveHoveredBrush(FSlateColorBrush(PaperDim()))
        .SetInactiveHoveredBrush(FSlateColorBrush(PaperDim()))
        .SetTextColor(Ink()).SetSelectedTextColor(Ink());
    return Style;
}
}

void SSCMusic::Construct(const FArguments& Args)
{
    Model = Args._Model;
    const TSharedRef<SWidget> Library = SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, RowGap)
        [Txt(TAttribute<FText>::CreateLambda([this] { return LibraryStatus(); }), Body(), Ink2())]
        + SVerticalBox::Slot().FillHeight(1)
        [
            SAssignNew(List, SListView<TSharedPtr<FSCMusicCue>>)
            .ListItemsSource(&Rows)
            .SelectionMode(ESelectionMode::Single)
            .OnSelectionChanged_Lambda([this](TSharedPtr<FSCMusicCue> Cue, ESelectInfo::Type) { Selected = MoveTemp(Cue); })
            .OnMouseButtonDoubleClick_Lambda([this](TSharedPtr<FSCMusicCue> Cue) {
                if (Cue)
                    if (auto* Player = Music()) Player->Play(Cue->Id);
            })
            .OnGenerateRow_Lambda([](TSharedPtr<FSCMusicCue> Cue, const TSharedRef<STableViewBase>& Table) {
                return SNew(STableRow<TSharedPtr<FSCMusicCue>>, Table).Style(&CueRowStyle())
                    .ToolTipText(FText::FromString(Cue->AuthoredName))
                    [
                        SNew(SVerticalBox)
                        + SVerticalBox::Slot().AutoHeight()[HRule()]
                        + SVerticalBox::Slot().AutoHeight().Padding(0, RowGap)
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().FillWidth(1)
                            [SNew(STextBlock).Text(FText::FromString(Cue->Title)).Font(Body()).OverflowPolicy(ETextOverflowPolicy::Ellipsis)]
                            + SHorizontalBox::Slot().AutoWidth().Padding(Pad, 0, RowGap, 0)
                            [Txt(Duration(Cue->Seconds), Numeric(), Ink2())]
                        ]
                    ];
            })
        ];

    const TSharedRef<SWidget> Transport = SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, RowGap)
        [Txt(TAttribute<FText>::CreateLambda([this] { return TrackTitle(); }), Title(), Ink())]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, RowGap)
        [Txt(TAttribute<FText>::CreateLambda([this] { return TrackTime(); }), Numeric(), Ink2())]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, Pad)
        [
            SNew(SProgressBar).FillColorAndOpacity(Accent()).Percent_Lambda([this]() -> TOptional<float> {
                const auto* Player = Music();
                const FSCMusicCue* Cue = CurrentCue();
                return Cue && Player ? FMath::Clamp(float(Player->SecondsPlayed() / Cue->Seconds), 0.f, 1.f) : 0.f;
            })
        ]
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, RowGap, 0)
            [
                SNew(SButton).ButtonStyle(&PrimaryButton())
                .IsEnabled_Lambda([this] { return Selected.IsValid() && Music() != nullptr; })
                .OnClicked_Lambda([this] {
                    if (Selected)
                        if (auto* Player = Music()) Player->Play(Selected->Id);
                    return FReply::Handled();
                })
                [Txt(LOCTEXT("Play", "PLAY"), Label(), Paper())]
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, RowGap, 0)
            [
                SNew(SButton).ButtonStyle(&FlatButton())
                .IsEnabled_Lambda([this] { const auto* Player = Music(); return Player && Player->HasVoice(); })
                .OnClicked_Lambda([this] {
                    if (auto* Player = Music()) Player->TogglePause();
                    return FReply::Handled();
                })
                [Txt(TAttribute<FText>::CreateLambda([this] {
                    const auto* Player = Music();
                    return Player && Player->IsPaused() ? LOCTEXT("Resume", "RESUME") : LOCTEXT("Pause", "PAUSE");
                }), Label(), Ink())]
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton).ButtonStyle(&FlatButton())
                .IsEnabled_Lambda([this] { return CurrentCue() != nullptr; })
                .OnClicked_Lambda([this] {
                    if (auto* Player = Music()) Player->Stop();
                    return FReply::Handled();
                })
                [Txt(LOCTEXT("Stop", "STOP"), Label(), Ink())]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, Pad, 0, 0)
        [
            SNew(STextBlock).Font(Body()).ColorAndOpacity(Alert()).AutoWrapText(true).Text_Lambda([this] {
                const auto* Player = Music();
                return Player ? Player->GetError() : FText::GetEmpty();
            })
        ];

    ChildSlot
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, Gutter, 0)
        [Panel(LOCTEXT("Library", "SOUNDTRACK LIBRARY"), FText::GetEmpty(), Library)]
        + SHorizontalBox::Slot().FillWidth(0.65f)
        [Panel(LOCTEXT("Playback", "PLAYBACK"), FText::GetEmpty(), Transport)]
    ];
}

USCMusicSubsystem* SSCMusic::Music() const
{
    return USCMusicSubsystem::Get(Model->GetViewer());
}

void SSCMusic::Tick(const FGeometry& Geometry, double Time, float Delta)
{
    SCompoundWidget::Tick(Geometry, Time, Delta);
    auto* Player = Music();
    auto* Viewer = Model->GetViewer();
    if (!Player || !Viewer || !Model->IsOpen()) return;

    const bool bNewArchive = Generation != Viewer->GetArchiveGeneration();
    if (bNewArchive)
    {
        Generation = Viewer->GetArchiveGeneration();
        Player->RequestCatalogue();
    }
    const auto& Catalogue = Player->GetCues();
    if (!bNewArchive && Rows.Num() == Catalogue.Num()) return;
    Rows.Reset();
    TMap<FString, int32> TitleCounts;
    TMap<FString, int32> Variants;
    for (const FSCMusicCue& Cue : Catalogue)
        ++TitleCounts.FindOrAdd(Cue.Title);
    for (const FSCMusicCue& Cue : Catalogue)
    {
        const auto Row = MakeShared<FSCMusicCue>(Cue);
        if (TitleCounts.FindRef(Cue.Title) > 1)
            Row->Title = FText::Format(LOCTEXT("Variant", "{0} ({1})"), FText::FromString(Cue.Title), ++Variants.FindOrAdd(Cue.Title)).ToString();
        Rows.Add(Row);
    }
    Selected.Reset();
    List->ClearSelection();
    List->RequestListRefresh();
    if (!Rows.IsEmpty()) List->SetSelection(Rows[0]);
}

const FSCMusicCue* SSCMusic::CurrentCue() const
{
    const auto* Player = Music();
    if (!Player || Player->GetCurrentId().IsEmpty()) return nullptr;
    const auto* Row = Rows.FindByPredicate([Player](const TSharedPtr<FSCMusicCue>& Cue) {
        return Cue->Id == Player->GetCurrentId();
    });
    return Row ? Row->Get() : nullptr;
}

FText SSCMusic::LibraryStatus() const
{
    const auto* Viewer = Model->GetViewer();
    const auto* Player = Music();
    if (!Viewer || !Player || Viewer->GetArchiveState() == ESCArchiveState::Closed ||
        Viewer->GetArchiveState() == ESCArchiveState::Failed)
        return LOCTEXT("NeedsArchive", "Open Star Citizen’s game archive to browse the soundtrack.");
    if (!Player->HasCatalogue() && !Player->GetError().IsEmpty())
        return LOCTEXT("Unavailable", "The soundtrack library is unavailable.");
    if (!Player->HasCatalogue())
        return LOCTEXT("Loading", "Loading soundtrack library…");
    if (Player->GetCues().IsEmpty())
        return LOCTEXT("Empty", "No standalone soundtrack cues were found.");
    return FText::Format(LOCTEXT("Count", "{0} {0}|plural(one=cue,other=cues)"), Player->GetCues().Num());
}

FText SSCMusic::TrackTitle() const
{
    if (const FSCMusicCue* Cue = CurrentCue())
        return FText::FromString(Cue->Title);
    return LOCTEXT("NoCue", "Nothing playing");
}

FText SSCMusic::TrackTime() const
{
    const auto* Player = Music();
    const FSCMusicCue* Cue = CurrentCue();
    if (!Player || !Cue) return FText::GetEmpty();
    if (Player->IsLoadingCue()) return LOCTEXT("LoadingCue", "Loading cue…");
    return FText::Format(LOCTEXT("Progress", "{0} / {1}"), Duration(Player->SecondsPlayed()), Duration(Cue->Seconds));
}
#undef LOCTEXT_NAMESPACE
