// SSCViewerPicker: the environment, vehicle and paint lists of the viewer's menu, in the shared HUD look (SCHudStyle).
#include "SSCViewerPicker.h"
#include "SCVehicle.h"
#include "SCViewerSettings.h"
#include "SCViewerShell.h"
#include "SCViewerSubsystem.h"
#include "UI/SCHudStyle.h"
#include "Brushes/SlateColorBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Colors/SColorPicker.h"
#include "Widgets/Layout/SSpacer.h"

#define LOCTEXT_NAMESPACE "SCViewerPicker"
using namespace SCHudStyle;
using ERowState = SSCViewerPicker::ERowState;

namespace
{
/** The environment row for the map's own (procedural) hangar: no archive environment (RequestEnvironment "none"). */
const TCHAR* const MapHangar = TEXT("none");

const FTableViewStyle& ListStyle()
{
    static const FTableViewStyle Style = FTableViewStyle().SetBackgroundBrush(FSlateColorBrush(Paper()));
    return Style;
}
const FTableRowStyle& RowStyle()
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
/** The search field on paper: a paper-dim well and ink text (its hint is a secondary-ink label over the field). */
const FEditableTextBoxStyle& SearchStyle()
{
    static const FEditableTextBoxStyle Style = FEditableTextBoxStyle(FCoreStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox"))
        .SetBackgroundImageNormal(FSlateColorBrush(PaperDim()))
        .SetBackgroundImageHovered(FSlateColorBrush(PaperDim()))
        .SetBackgroundImageFocused(FSlateColorBrush(PaperDim()))
        .SetBackgroundImageReadOnly(FSlateColorBrush(PaperDim()))
        .SetForegroundColor(Ink())
        .SetFocusedForegroundColor(Ink())
        .SetPadding(FMargin(8, 5));
    return Style;
}
/** A row. The current choice is an inverted bar (ink fill, paper text) with the accent edge and "IN SCENE"; a pending
 *  one has the warn edge and "LOADING"; hover is the paper-dim of the row button. */
TSharedRef<SWidget> Row(const FString& Name, const FString& Second, TFunction<ERowState()> State)
{
    auto Text = [State](bool bSecond) {
        return TAttribute<FSlateColor>::CreateLambda([State, bSecond] {
            return State() == ERowState::Current ? FSlateColor(Paper()) : FSlateColor(bSecond ? Ink2() : Ink());
        });
    };
    return SNew(SBorder).BorderImage(White()).Padding(0)
        .BorderBackgroundColor_Lambda([State] { return State() == ERowState::Current ? Ink() : FLinearColor::Transparent; })
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SBox).WidthOverride(FocusWidth * 2)
                [SNew(SBorder).BorderImage(White()).BorderBackgroundColor_Lambda([State] {
                    const ERowState S = State();
                    return S == ERowState::Current ? Accent() : S == ERowState::Pending ? Warn() : FLinearColor::Transparent;
                })]
            ]
            + SHorizontalBox::Slot().FillWidth(1).Padding(8, 5)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Font(Body()).ColorAndOpacity(Text(false)).Text(FText::FromString(Name)).OverflowPolicy(ETextOverflowPolicy::Ellipsis)]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock).Font(Label()).ColorAndOpacity(Text(true)).Text(FText::FromString(Second)).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
                    .Visibility(Second.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
                ]
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
            [
                SNew(STextBlock).Font(Label())
                .ColorAndOpacity_Lambda([State] { return State() == ERowState::Current ? FSlateColor(Paper()) : FSlateColor(Ink2()); })
                .Text_Lambda([State] {
                    const ERowState S = State();
                    return S == ERowState::Current ? LOCTEXT("InScene", "IN SCENE") : S == ERowState::Pending ? LOCTEXT("Loading", "LOADING") : FText();
                })
            ]
        ];
}
FString SecondLine(const FSCCatalogEntry& E)
{
    TArray<FString> Parts;
    if (!E.Group.IsEmpty()) Parts.Add(E.Group);
    if (!E.Detail.IsEmpty()) Parts.Add(E.Detail);
    return FString::Join(Parts, TEXT("  ·  "));
}
bool SameId(const FString& A, const FString& B)
{
    return A.Replace(TEXT("\\"), TEXT("/")).Equals(B.Replace(TEXT("\\"), TEXT("/")), ESearchCase::IgnoreCase);
}
bool IsMapHangar(const FString& Request) { return Request.IsEmpty() || Request.Equals(MapHangar, ESearchCase::IgnoreCase); }
bool IsLoadingVehicle(const USCViewerSubsystem& S)
{
    const ESCLoadPhase Phase = S.GetLoadPhase();
    return Phase == ESCLoadPhase::Exporting || Phase == ESCLoadPhase::Parsing || Phase == ESCLoadPhase::Importing;
}
/** The command line asks for a scene or drives the app: the menu does not open by itself. */
bool StartRequested()
{
    const TCHAR* Cmd = FCommandLine::Get();
    FString Value;
    for (const TCHAR* Key : {TEXT("SCLoad="), TEXT("SCEnvironment="), TEXT("SCScript="), TEXT("SCPackage=")})
        if (FParse::Value(Cmd, Key, Value)) return true;
    for (const TCHAR* Switch : {TEXT("SCPlayerDemo"), TEXT("SCShowcase"), TEXT("SCRpc")})
        if (FParse::Param(Cmd, Switch) || FParse::Value(Cmd, *(FString(Switch) + TEXT("=")), Value)) return true;
    return false;
}
/** The menu built last (one per viewer root), for the console. */
TWeakPtr<SSCViewerPicker> LastPicker;
/** sc.Menu.ShowShip 1|0: the menu's paint-only view (verification and screenshots; the menu's button does the same). */
FAutoConsoleCommand ShowShipCommand(TEXT("sc.Menu.ShowShip"), TEXT("sc.Menu.ShowShip 1|0: open the menu with only the paint column (1), or all lists (0)"),
    FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) {
        if (const TSharedPtr<SSCViewerPicker> Picker = LastPicker.Pin()) Picker->ShowShip(Args.IsEmpty() || Args[0] != TEXT("0"));
    }));
} // namespace

void SSCViewerPicker::ShowShip(bool bOn)
{
    bPaintOnly = bOn;
    SCViewerShell::SetMenuPassesView(bOn);
    if (bOn && Subsystem.IsValid()) SCViewerShell::SetMenuOpen(Subsystem.Get(), true);
}

void SSCViewerPicker::Construct(const FArguments& Args)
{
    LastPicker = StaticCastSharedRef<SSCViewerPicker>(AsShared());
    Subsystem = Args._Subsystem;
    FocusView = Args._FocusView;
    TWeakObjectPtr<USCViewerSubsystem> Weak = Subsystem;
    // The picker itself keeps ticking (a collapsed widget does not: the start check, queued choices and the lists'
    // refresh run in Tick); its content shows while the menu is open.
    SetVisibility(EVisibility::SelfHitTestInvisible);
    FParse::Value(FCommandLine::Get(), TEXT("SCSearch="), Vehicles.Filter); // verification aid: prefill the vehicle search

    TSharedRef<SWidget> EnvironmentList = MakeList(Environments, LOCTEXT("Environment", "Hangar"), LOCTEXT("SearchPlaces", "Search hangars"),
        [this](const FString& Id) { return EnvironmentState(Id); }, [this](const FString& Id, int32 RowSession) { PickEnvironment(Id, RowSession); });
    TSharedRef<SWidget> VehicleList = MakeList(Vehicles, LOCTEXT("Vehicle", "Vehicle"), LOCTEXT("SearchVehicles", "Search vehicles"),
        [this](const FString& Id) { return VehicleState(Id); }, [this](const FString& Id, int32 RowSession) { PickVehicle(Id, RowSession); });

    TSharedRef<SWidget> PaintList = Panel(LOCTEXT("Paint", "Paint"), FText(),
        SNew(SOverlay)
        + SOverlay::Slot()
        [
            SAssignNew(PaintView, SListView<FPaint>).ListItemsSource(&Paints).ListViewStyle(&ListStyle()).SelectionMode(ESelectionMode::None)
            .OnGenerateRow_Lambda([Weak](FPaint Item, const TSharedRef<STableViewBase>& Table) {
                const int32 Index = Item->Index;
                const TWeakObjectPtr<const ASCVehicle> Owner = Item->Owner;
                // The row acts on the vehicle whose manifest listed it, never on one that replaced it since.
                auto Live = [Weak, Owner]() -> ASCVehicle* {
                    ASCVehicle* V = Weak.IsValid() ? Weak->GetVehicle() : nullptr;
                    return V && V == Owner.Get() ? V : nullptr;
                };
                return SNew(STableRow<FPaint>, Table).Style(&RowStyle()).Padding(0)
                [
                    SNew(SButton).ButtonStyle(&RowButton()).ContentPadding(0).OnClicked_Lambda([Weak, Live, Index] {
                        if (Live()) Weak->SetPaint(Index);
                        return FReply::Handled();
                    })
                    [Row(Item->Label, FString(), [Live, Index] {
                        const ASCVehicle* V = Live();
                        return V && V->GetPaint() == Index ? ERowState::Current : ERowState::None;
                    })]
                ];
            })
        ]
        + SOverlay::Slot().VAlign(VAlign_Top).Padding(8, 5)
        [
            SNew(STextBlock).Font(Body()).ColorAndOpacity(Ink2()).AutoWrapText(true)
            .Text(LOCTEXT("NoVehicle", "Pick a vehicle to see its paints."))
            .Visibility_Lambda([this] { return Paints.IsEmpty() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
        ]);
    // The active paint's colours, below its list: change one by its hex code or the colour picker; saved per vehicle and
    // paint (PaintColors.json), back to the paint's own with Reset.
    TSharedRef<SWidget> ColorPanel = Panel(LOCTEXT("Colours", "Customise colours"), FText(),
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()[SAssignNew(ColorRows, SVerticalBox)]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton).ButtonStyle(&FlatButton())
                .IsEnabled_Lambda([this] {
                    const USCViewerSubsystem* S = Subsystem.Get();
                    const ASCVehicle* V = S ? S->GetVehicle() : nullptr;
                    return V && !V->GetPaintColorOverrides().IsEmpty();
                })
                .OnClicked_Lambda([this] { ResetColors(); return FReply::Handled(); })
                [Txt(LOCTEXT("ResetColours", "RESET COLOURS"), Label(), Ink())]
            ]
        ]);
    TSharedRef<SWidget> PaintColumn = SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, Gutter)
        [
            SNew(SHorizontalBox)
            .Visibility_Lambda([this] { return bPaintOnly ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed; })
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton).ButtonStyle(&FlatButton()).ToolTipText(LOCTEXT("AllListsTip", "Back to the hangar and vehicle lists"))
                .OnClicked_Lambda([this] { ShowShip(false); return FReply::Handled(); })
                [Txt(LOCTEXT("AllLists", "SHOW LISTS"), Label(), Accent())]
            ]
            + SHorizontalBox::Slot().FillWidth(1)
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton).ButtonStyle(&FlatButton()).OnClicked_Lambda([this] { Close(); return FReply::Handled(); })
                [Txt(LOCTEXT("ClosePaint", "F1  CLOSE"), Label(), Ink())]
            ]
        ]
        + SVerticalBox::Slot().FillHeight(1)[PaintList]
        + SVerticalBox::Slot().AutoHeight().Padding(0, Gutter, 0, 0)[ColorPanel];

    ChildSlot
    [
        SNew(SBorder).BorderImage(White()).Padding(SafeMargin)
        .BorderBackgroundColor_Lambda([this] { return bPaintOnly ? FLinearColor::Transparent : ScrimOpen(); })
        .Visibility_Lambda([this, Weak] {
            if (!SCViewerShell::IsMenuOpen(Weak.Get())) return EVisibility::Collapsed;
            return bPaintOnly ? EVisibility::SelfHitTestInvisible : EVisibility::Visible;
        })
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, Gutter)
            [
                SNew(SHorizontalBox)
                .Visibility_Lambda([this] { return bPaintOnly ? EVisibility::Collapsed : EVisibility::SelfHitTestInvisible; })
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [Txt(TAttribute<FText>::CreateLambda([] { return Caps(SCViewerShell::AppName()); }), Display(), OnScrim())]
                + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(Gutter, 0)
                [Txt(TAttribute<FText>::CreateSP(this, &SSCViewerPicker::StatusText), Label(), OnScrim())]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
                [
                    SNew(SButton).ButtonStyle(&FlatButton())
                    .ToolTipText(LOCTEXT("ShowShipTip", "Keep only the paint and colour lists, so you see the ship while you change them"))
                    .Visibility_Lambda([this] { return Subsystem.IsValid() && Subsystem->GetVehicle() ? EVisibility::Visible : EVisibility::Collapsed; })
                    .OnClicked_Lambda([this] { ShowShip(true); return FReply::Handled(); })
                    [Txt(LOCTEXT("ShowShip", "SHOW SHIP"), Label(), Accent())]
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    SNew(SButton).ButtonStyle(&FlatButton()).OnClicked_Lambda([this] { Close(); return FReply::Handled(); })
                    [Txt(LOCTEXT("Close", "F1  CLOSE"), Label(), Ink())]
                ]
            ]
            + SVerticalBox::Slot().FillHeight(1)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1)
                [SNew(SBox).Visibility_Lambda([this] { return bPaintOnly ? EVisibility::Collapsed : EVisibility::SelfHitTestInvisible; })[EnvironmentList]]
                + SHorizontalBox::Slot().FillWidth(1.2f).Padding(Gutter, 0, 0, 0)
                [SNew(SBox).Visibility_Lambda([this] { return bPaintOnly ? EVisibility::Collapsed : EVisibility::SelfHitTestInvisible; })[VehicleList]]
                // Show ship: the view's share of the width, let through to the view (no hit test).
                + SHorizontalBox::Slot().FillWidth(2.2f)
                [SNew(SSpacer).Visibility_Lambda([this] { return bPaintOnly ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })]
                + SHorizontalBox::Slot().FillWidth(0.8f).Padding(Gutter, 0, 0, 0)[PaintColumn]
            ]
        ]
    ];
}

TSharedRef<SWidget> SSCViewerPicker::MakeList(FList& List, const FText& Title, const FText& Hint, TFunction<ERowState(const FString&)> State,
                                              TFunction<void(const FString&, int32)> Pick)
{
    FList* L = &List;
    return Panel(Title, TAttribute<FText>::CreateLambda([L] { return FText::AsNumber(L->Shown.Num()); }),
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [
            SNew(SOverlay)
            + SOverlay::Slot()
            [
                SNew(SEditableTextBox).Style(&SearchStyle()).Font(Body()).Text(FText::FromString(List.Filter))
                .AccessibleText(Hint) // the visible hint is a sibling label
                .OnTextChanged_Lambda([this, L](const FText& Value) {
                    L->Filter = Value.ToString();
                    Refilter(*L);
                })
            ]
            + SOverlay::Slot().VAlign(VAlign_Center).Padding(10, 0) // the field's hint, in secondary ink (the engine's is too faint on paper)
            [
                SNew(STextBlock).Font(Body()).ColorAndOpacity(Ink2()).Text(Hint)
                .Visibility_Lambda([L] { return L->Filter.IsEmpty() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
            ]
        ]
        + SVerticalBox::Slot().FillHeight(1)
        [
            SNew(SOverlay)
            + SOverlay::Slot()
            [
            SAssignNew(List.View, SListView<FEntry>).ListItemsSource(&List.Shown).ListViewStyle(&ListStyle()).SelectionMode(ESelectionMode::None)
            .OnGenerateRow_Lambda([State, Pick, L](FEntry Item, const TSharedRef<STableViewBase>& Table) {
                const FString Id = Item->Id;
                const TFunction<void()> Click = RowClick(Pick, Id, L->Session); // the session of the list it was made from
                const FString Second = SecondLine(*Item);
                // The full name (the row may cut it) over the id the viewer loads.
                const FText Tip = FText::FromString(Item->Label + (Second.IsEmpty() ? FString() : TEXT("\n") + Second) + TEXT("\n") + Id);
                return SNew(STableRow<FEntry>, Table).Style(&RowStyle()).Padding(0).ToolTipText(Tip)
                [
                    SNew(SButton).ButtonStyle(&RowButton()).ContentPadding(0).OnClicked_Lambda([Click] {
                        Click();
                        return FReply::Handled();
                    })
                    [Row(Item->Label, Second, [State, Id] { return State(Id); })]
                ];
            })
            ]
            + SOverlay::Slot().VAlign(VAlign_Center).Padding(16, 0)
            [
                SNew(STextBlock).Font(Body()).ColorAndOpacity(Ink2()).AutoWrapText(true).Justification(ETextJustify::Center)
                .Visibility_Lambda([this, L] { return !bCatalogReady || L->Shown.IsEmpty() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
                .Text_Lambda([this] {
                    const USCViewerSubsystem* S = Subsystem.Get();
                    if (S && (S->GetArchiveState() == ESCArchiveState::Failed || S->GetArchiveState() == ESCArchiveState::Closed))
                        return LOCTEXT("EmptyNoArchive", "Star Citizen was not found: choose its Data.p4k first.");
                    return bCatalogReady ? LOCTEXT("EmptySearch", "Nothing matches the search.")
                                         : LOCTEXT("EmptyReading", "Reading your Star Citizen files…\nThe first start takes a minute or two.");
                })
            ]
        ]);
}

void SSCViewerPicker::Refilter(FList& List)
{
    TArray<FString> Terms;
    List.Filter.ParseIntoArrayWS(Terms);
    List.Shown.Reset();
    for (const FEntry& E : List.All)
        if (!Terms.ContainsByPredicate([&](const FString& T) {
                return !E->Label.Contains(T) && !E->Group.Contains(T) && !E->Detail.Contains(T) && !E->Id.Contains(T);
            }))
            List.Shown.Add(E);
    if (List.View) List.View->RequestListRefresh();
}

TFunction<void()> SSCViewerPicker::RowClick(TFunction<void(const FString&, int32)> Pick, const FString& Id, int32 RowSession)
{
    return [Pick = MoveTemp(Pick), Id, RowSession] { Pick(Id, RowSession); };
}

SSCViewerPicker::EVehiclePick SSCViewerPicker::DecideVehiclePick(const FString& Id, const FVehiclePickState& State)
{
    if (State.RowSession != State.Session) return EVehiclePick::Drop; // a row of an earlier archive's lists
    // The latest click wins. The load running for it (and not being stopped): drop any later choice waiting behind it.
    if (State.bLoading && !State.bCancelling && SameId(State.Pending, Id)) return EVehiclePick::Keep;
    // Idle: LoadEntity decides (it keeps the vehicle in the scene when the request is identical for this archive session).
    if (State.bCanLoad) return EVehiclePick::Load;
    // A load runs (or the archive opens): this choice follows it, also when it is the vehicle being replaced.
    return EVehiclePick::Queue;
}

SSCViewerPicker::EEnvironmentPick SSCViewerPicker::DecideEnvironmentPick(const FString& Id, const FEnvironmentPickState& State)
{
    if (State.RowSession != State.Session) return EEnvironmentPick::Drop; // a row of an earlier archive's lists
    // Already there, or on its way and not being stopped: nothing to do (a failed, stopped or other archive's one is
    // requested again).
    const bool bThere = IsMapHangar(Id) ? IsMapHangar(State.Request) && !State.bWorking
                                        : SameId(Id, State.Request) && (State.bInstalled || (State.bLoading && !State.bCancelling));
    if (bThere) return EEnvironmentPick::There;
    // A running export, parse or import would make RequestEnvironment wait for its workers on the game thread.
    return State.bWorking ? EEnvironmentPick::Queue : EEnvironmentPick::Request;
}

void SSCViewerPicker::PickEnvironment(const FString& Id, int32 RowSession)
{
    USCViewerSubsystem* S = Subsystem.Get();
    if (!S) return;
    FEnvironmentPickState State;
    State.RowSession = RowSession;
    State.Session = S->GetArchiveGeneration();
    State.Request = S->GetEnvironmentRequest();
    State.bInstalled = S->IsEnvironmentOfThisArchive();
    State.bLoading = S->IsEnvironmentLoading();
    State.bCancelling = S->IsEnvironmentCancelling();
    State.bWorking = S->IsEnvironmentWorking();
    const EEnvironmentPick Decision = DecideEnvironmentPick(Id, State);
    if (Decision == EEnvironmentPick::Drop)
    {
        UE_LOG(LogTemp, Display, TEXT("VIEWER_MENU dropped environment %s: a row of an earlier archive's lists"), *Id);
        return;
    }
    // Saved as the user's environment once it is in the scene (Tick), so a failed choice is not restored at the next start.
    SaveEnvironment = Id;
    SaveEnvironmentSession = QueuedEnvironmentSession = RowSession;
    switch (Decision)
    {
    case EEnvironmentPick::Drop:
        break;
    case EEnvironmentPick::There:
        QueuedEnvironment.Reset();
        break;
    case EEnvironmentPick::Queue: // the export or parse is asked to stop; the choice is taken once nothing runs (Tick)
        QueuedEnvironment = Id;
        S->CancelEnvironmentWork();
        break;
    case EEnvironmentPick::Request:
        QueuedEnvironment.Reset();
        S->SetEnvironment(Id, false);
        break;
    }
}

void SSCViewerPicker::PickVehicle(const FString& Id, int32 RowSession)
{
    USCViewerSubsystem* S = Subsystem.Get();
    if (!S) return;
    FVehiclePickState State;
    State.RowSession = RowSession;
    State.Session = S->GetArchiveGeneration();
    State.bLoading = IsLoadingVehicle(*S);
    State.bCancelling = S->IsLoadCancelling();
    State.bCanLoad = S->CanLoad(Id);
    State.Pending = S->GetPendingEntity();
    switch (DecideVehiclePick(Id, State))
    {
    case EVehiclePick::Drop:
        UE_LOG(LogTemp, Display, TEXT("VIEWER_MENU dropped vehicle %s: a row of an earlier archive's lists"), *Id);
        break;
    case EVehiclePick::Keep:
        QueuedVehicle.Reset();
        break;
    case EVehiclePick::Load:
        QueuedVehicle.Reset();
        S->LoadEntity(Id, S->GetOptions());
        break;
    case EVehiclePick::Queue:
        QueuedVehicle = Id;
        QueuedVehicleSession = RowSession;
        break;
    }
}

ERowState SSCViewerPicker::EnvironmentState(const FString& Id) const
{
    const USCViewerSubsystem* S = Subsystem.Get();
    if (!S) return ERowState::None;
    if (QueuedEnvironment.IsSet()) return SameId(*QueuedEnvironment, Id) ? ERowState::Pending : ERowState::None;
    const FString Request = S->GetEnvironmentRequest();
    if (IsMapHangar(Id)) return IsMapHangar(Request) ? ERowState::Current : ERowState::None;
    if (!SameId(Id, Request)) return ERowState::None;
    if (S->IsEnvironmentLoading()) return ERowState::Pending;
    return S->IsEnvironmentOfThisArchive() ? ERowState::Current : ERowState::None; // failed, or an earlier archive's
}

ERowState SSCViewerPicker::VehicleState(const FString& Id) const
{
    const USCViewerSubsystem* S = Subsystem.Get();
    if (!S) return ERowState::None;
    // What is in the scene reads as such even while its return is queued behind another load.
    if (S->IsVehicleOfThisArchive() && SameId(S->GetLoadedEntity(), Id)) return ERowState::Current;
    if (QueuedVehicle.IsSet() && SameId(*QueuedVehicle, Id)) return ERowState::Pending;
    return IsLoadingVehicle(*S) && SameId(S->GetPendingEntity(), Id) ? ERowState::Pending : ERowState::None;
}

void SSCViewerPicker::DropSupersededChoices(const USCViewerSubsystem& S)
{
    const int32 Session = S.GetArchiveGeneration();
    auto Drop = [Session](TOptional<FString>& Choice, int32 MadeIn, const TCHAR* What) {
        if (!Choice.IsSet() || MadeIn == Session) return;
        UE_LOG(LogTemp, Display, TEXT("VIEWER_MENU dropped %s %s: picked from an earlier archive's lists"), What, **Choice);
        Choice.Reset();
    };
    Drop(QueuedEnvironment, QueuedEnvironmentSession, TEXT("environment"));
    Drop(QueuedVehicle, QueuedVehicleSession, TEXT("vehicle"));
    Drop(SaveEnvironment, SaveEnvironmentSession, TEXT("saved environment"));
}

void SSCViewerPicker::RevealCurrent()
{
    auto Reveal = [this](FList& L, bool bEnvironment) {
        if (!L.View) return;
        // What is in the scene, else what is on its way.
        const FEntry* Found = nullptr;
        for (ERowState Want : {ERowState::Current, ERowState::Pending})
            if (!Found)
                Found = L.Shown.FindByPredicate([&](const FEntry& E) { return (bEnvironment ? EnvironmentState(E->Id) : VehicleState(E->Id)) == Want; });
        if (Found) L.View->RequestScrollIntoView(*Found);
    };
    Reveal(Environments, true);
    Reveal(Vehicles, false);
    const ASCVehicle* V = Subsystem.IsValid() ? Subsystem->GetVehicle() : nullptr;
    if (const FPaint* P = V && PaintView ? Paints.FindByPredicate([V](const FPaint& R) { return R->Index == V->GetPaint(); }) : nullptr)
        PaintView->RequestScrollIntoView(*P);
}

void SSCViewerPicker::Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime)
{
    SCompoundWidget::Tick(Geometry, CurrentTime, DeltaTime);
    USCViewerSubsystem* S = Subsystem.Get();
    if (!S) return;
    // Open by itself once, at start, when the command line asks for no scene.
    if (!bStartDecided)
    {
        bStartDecided = true;
        if (!StartRequested()) SCViewerShell::SetMenuOpen(S, true);
    }
    const bool bOpen = SCViewerShell::IsMenuOpen(S);
    if (bOpen) S->RequestCatalog(); // listed when first shown, not for a run that never opens the menu

    // The choices the running loads could not take yet: the environment once nothing of it runs, the vehicle once
    // nothing loads (a vehicle waits for the environment it goes into); those of an earlier archive session are dropped.
    DropSupersededChoices(*S);
    if (QueuedEnvironment.IsSet() && !S->IsEnvironmentWorking())
    {
        const FString Id = *QueuedEnvironment;
        QueuedEnvironment.Reset();
        S->SetEnvironment(Id, false);
    }
    if (QueuedVehicle.IsSet() && !QueuedEnvironment.IsSet() && !S->IsBusy() && S->GetArchiveState() != ESCArchiveState::Opening)
    {
        const FString Id = *QueuedVehicle;
        QueuedVehicle.Reset();
        if (S->CanLoad(Id)) S->LoadEntity(Id, S->GetOptions());
    }
    // The environment picked is saved once it is in the scene; a failed one is not.
    if (SaveEnvironment.IsSet() && !QueuedEnvironment.IsSet() && !S->IsEnvironmentLoading())
    {
        const FString Id = *SaveEnvironment;
        SaveEnvironment.Reset();
        const FString Request = S->GetEnvironmentRequest();
        if (IsMapHangar(Id) ? IsMapHangar(Request) : SameId(Id, Request) && S->IsEnvironmentOfThisArchive())
        {
            USCViewerSettings* Settings = USCViewerSettings::Get();
            Settings->Environment = IsMapHangar(Id) ? FString(MapHangar) : Id;
            Settings->Save();
        }
    }

    bool bReveal = bOpen && !bWasOpen;
    if (bOpen && !bWasOpen)
    {
        if (FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().ReleaseAllPointerCapture(); // free the cursor (orbit drags, first-person look)
            FSlateApplication::Get().SetAllUserFocus(SharedThis(this), EFocusCause::SetDirectly); // Esc / F1 reach the menu
        }
        // The engine's on-screen diagnostics draw over all UI, across the menu's header: off while it is open, when
        // they were on, back on when it closes if still off.
        bHidScreenMessages = GAreScreenMessagesEnabled;
        GAreScreenMessagesEnabled = false;
    }
    if (!bOpen && bWasOpen)
    {
        bPaintOnly = false;
        SCViewerShell::SetMenuPassesView(false);
        RestoreScreenMessages();
        if (FocusView) FocusView(); // however it closed (F1, Esc, the button, RPC /ui): keys go back to the view
    }
    bWasOpen = bOpen;

    const FSCViewerCatalog& Catalog = S->GetCatalog();
    if (Catalog.Revision != CatalogRevision || Catalog.bReady != bCatalogReady)
    {
        CatalogRevision = Catalog.Revision;
        bCatalogReady = Catalog.bReady;
        auto Fill = [this, Session = int32(Catalog.Revision)](FList& L, const TArray<FSCCatalogEntry>& Rows, const FSCCatalogEntry* First) {
            L.Session = Session;
            L.All.Reset();
            if (First) L.All.Add(MakeShared<FSCCatalogEntry>(*First));
            for (const FSCCatalogEntry& E : Rows) L.All.Add(MakeShared<FSCCatalogEntry>(E));
            Refilter(L);
        };
        FSCCatalogEntry Procedural;
        Procedural.Id = MapHangar;
        Procedural.Label = LOCTEXT("MapHangar", "Empty studio").ToString();
        Procedural.Detail = LOCTEXT("MapHangarDetail", "A plain room, not from the game").ToString();
        Fill(Environments, Catalog.Environments, &Procedural);
        Fill(Vehicles, Catalog.Vehicles, nullptr);
        bReveal |= bOpen;
    }
    // The paints of the vehicle in the scene, listed again when it changes (another entity or another actor).
    const ASCVehicle* V = S->GetVehicle();
    const FString Entity = V ? S->GetLoadedEntity() : FString();
    if (Entity != PaintsOf || V != PaintsOwner.Get())
    {
        PaintsOf = Entity;
        PaintsOwner = V;
        Paints.Reset();
        if (V)
        {
            Paints.Add(MakeShared<FPaintRow>(FPaintRow{INDEX_NONE, LOCTEXT("OwnPaint", "Default paint").ToString(), V}));
            for (int32 I = 0; I < V->Manifest.Paints.Num(); ++I) Paints.Add(MakeShared<FPaintRow>(FPaintRow{I, V->Manifest.Paints[I].Label().Replace(TEXT("_"), TEXT(" ")), V}));
        }
        if (PaintView) PaintView->RequestListRefresh();
        bReveal |= bOpen;
    }
    // The colour rows of the paint on it, made again when the vehicle or its paint changes.
    if (V != ColorsOwner.Get() || (V ? V->GetPaint() : MIN_int32) != ColorsPaint) RebuildColors(S->GetVehicle());
    else if (bOpen) // a colour changed elsewhere (the colour picker, the console, a saved file): the field follows, unless typed in
        for (const auto& [Channel, Field] : ColorFields)
            if (Field && !Field->HasKeyboardFocus())
                if (const FString Hex = USCViewerSubsystem::ToHex(ColorOf(Channel)); Field->GetText().ToString() != Hex) Field->SetText(FText::FromString(Hex));
    if (bReveal) RevealCurrent();
}

FLinearColor SSCViewerPicker::ColorOf(const FString& Channel) const
{
    const USCViewerSubsystem* S = Subsystem.Get();
    const ASCVehicle* V = S ? S->GetVehicle() : nullptr;
    if (!V) return FLinearColor::Black;
    if (const FLinearColor* Custom = V->GetPaintColorOverrides().Find(Channel)) return *Custom;
    return BaseColors.FindRef(Channel); // the paint's own (RebuildColors)
}

void SSCViewerPicker::RebuildColors(ASCVehicle* V)
{
    ColorsOwner = V;
    ColorsPaint = V ? V->GetPaint() : MIN_int32;
    ColorFields.Reset();
    BaseColors = V ? V->PaintColors(V->GetPaint()) : TMap<FString, FLinearColor>();
    if (!ColorRows) return;
    ColorRows->ClearChildren();
    const TMap<FString, FLinearColor>& Base = BaseColors;
    if (Base.IsEmpty())
    {
        ColorRows->AddSlot().AutoHeight()
        [
            SNew(STextBlock).Font(Body()).ColorAndOpacity(Ink2()).AutoWrapText(true)
            .Text(V ? LOCTEXT("NoColours", "This paint has no colours to change.") : LOCTEXT("ColoursNoVehicle", "Pick a vehicle to change its paint colours."))
        ];
        return;
    }
    static const TMap<FString, FText> Names = {
        {TEXT("primary"), LOCTEXT("Primary", "Main")}, {TEXT("secondary"), LOCTEXT("Secondary", "Second")},
        {TEXT("tertiary"), LOCTEXT("Tertiary", "Accent")}, {TEXT("glass"), LOCTEXT("Glass", "Glass")},
        {TEXT("decal_red"), LOCTEXT("Decal1", "Decal 1")}, {TEXT("decal_green"), LOCTEXT("Decal2", "Decal 2")},
        {TEXT("decal_blue"), LOCTEXT("Decal3", "Decal 3")}};
    static const TMap<FString, FText> Tips = {
        {TEXT("primary"), LOCTEXT("PrimaryTip", "The paint's main hull colour")},
        {TEXT("secondary"), LOCTEXT("SecondaryTip", "The paint's second hull colour")},
        {TEXT("tertiary"), LOCTEXT("TertiaryTip", "The paint's accent colour (trim, details)")},
        {TEXT("glass"), LOCTEXT("GlassTip", "The tint of the canopy and windows")},
        {TEXT("decal_red"), LOCTEXT("Decal1Tip", "A colour of the logos, stripes and markings; not every paint uses it")},
        {TEXT("decal_green"), LOCTEXT("Decal2Tip", "A colour of the logos, stripes and markings; not every paint uses it")},
        {TEXT("decal_blue"), LOCTEXT("Decal3Tip", "A colour of the logos, stripes and markings; not every paint uses it")}};
    for (const FString& Channel : ASCVehicle::PaintChannels())
    {
        if (!Base.Contains(Channel)) continue;
        TSharedPtr<SEditableTextBox> Field;
        ColorRows->AddSlot().AutoHeight().Padding(0, 2)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [SNew(SBox).WidthOverride(64)[SNew(STextBlock).Font(Body()).ColorAndOpacity(Ink()).Text(Names.FindRef(Channel)).ToolTipText(Tips.FindRef(Channel))]]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
            [
                SNew(SButton).ButtonStyle(&FlatButton()).ContentPadding(0)
                .ToolTipText(LOCTEXT("PickColour", "Choose a colour"))
                .OnClicked_Lambda([this, Channel] { OpenColorPicker(Channel); return FReply::Handled(); })
                [
                    SNew(SBox).WidthOverride(36).HeightOverride(22)
                    [SNew(SBorder).BorderImage(White()).BorderBackgroundColor_Lambda([this, Channel] { return ColorOf(Channel); })]
                ]
            ]
            + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
            [
                SAssignNew(Field, SEditableTextBox).Style(&SearchStyle()).Font(Body())
                .Text(FText::FromString(USCViewerSubsystem::ToHex(ColorOf(Channel))))
                .ToolTipText(LOCTEXT("HexHint", "Type a colour as #RRGGBB (for example #FFB000) and press Enter, or click the swatch to pick one"))
                .OnTextCommitted_Lambda([this, Channel](const FText& Value, ETextCommit::Type How) {
                    FLinearColor C;
                    if (How != ETextCommit::OnCleared && USCViewerSubsystem::FromHex(Value.ToString(), C)) ApplyColor(Channel, C, true);
                    else if (const TSharedPtr<SEditableTextBox>* F = ColorFields.Find(Channel)) (*F)->SetText(FText::FromString(USCViewerSubsystem::ToHex(ColorOf(Channel))));
                })
            ]
        ];
        ColorFields.Add(Channel, Field);
    }
}

void SSCViewerPicker::ApplyColor(const FString& Channel, const FLinearColor& Linear, bool bSave)
{
    USCViewerSubsystem* S = Subsystem.Get();
    ASCVehicle* V = S ? S->GetVehicle() : nullptr;
    if (!V) return;
    TMap<FString, FLinearColor> Overrides = V->GetPaintColorOverrides();
    Overrides.Add(Channel, FLinearColor(Linear.R, Linear.G, Linear.B, 1.f));
    S->SetPaintColors(Overrides, bSave);
    if (const TSharedPtr<SEditableTextBox>* F = ColorFields.Find(Channel)) (*F)->SetText(FText::FromString(USCViewerSubsystem::ToHex(Linear)));
}

void SSCViewerPicker::RemoveColor(const FString& Channel)
{
    USCViewerSubsystem* S = Subsystem.Get();
    ASCVehicle* V = S ? S->GetVehicle() : nullptr;
    if (!V || !V->GetPaintColorOverrides().Contains(Channel)) return;
    TMap<FString, FLinearColor> Overrides = V->GetPaintColorOverrides();
    Overrides.Remove(Channel);
    S->SetPaintColors(Overrides, true);
    if (const TSharedPtr<SEditableTextBox>* F = ColorFields.Find(Channel)) (*F)->SetText(FText::FromString(USCViewerSubsystem::ToHex(ColorOf(Channel))));
}

void SSCViewerPicker::ResetColors()
{
    if (USCViewerSubsystem* S = Subsystem.Get()) S->SetPaintColors({}, true);
    for (const auto& [Channel, Field] : ColorFields)
        if (Field) Field->SetText(FText::FromString(USCViewerSubsystem::ToHex(ColorOf(Channel))));
}

void SSCViewerPicker::OpenColorPicker(const FString& Channel)
{
    FColorPickerArgs Args;
    Args.bIsModal = false;
    Args.bUseAlpha = false;
    Args.sRGBOverride = true; // its hex agrees with the field's (sRGB)
    Args.bOnlyRefreshOnMouseUp = true; // the ship's materials follow when the mouse is let go
    Args.InitialColor = ColorOf(Channel);
    Args.ParentWidget = AsShared();
    // The window can outlive the menu (Quit, a world change): the callbacks hold it weakly, and act only on the vehicle and
    // paint the picker was opened for.
    const TWeakPtr<SSCViewerPicker> Weak = StaticCastSharedRef<SSCViewerPicker>(AsShared());
    const TWeakObjectPtr<const ASCVehicle> For = ColorsOwner;
    const int32 ForPaint = ColorsPaint;
    auto Live = [Weak, For, ForPaint]() -> TSharedPtr<SSCViewerPicker> {
        const TSharedPtr<SSCViewerPicker> P = Weak.Pin();
        const USCViewerSubsystem* S = P ? P->Subsystem.Get() : nullptr;
        const ASCVehicle* V = S ? S->GetVehicle() : nullptr;
        return V && V == For.Get() && V->GetPaint() == ForPaint ? P : nullptr;
    };
    Args.OnColorCommitted = FOnLinearColorValueChanged::CreateLambda([Live, Channel](FLinearColor C) {
        if (const TSharedPtr<SSCViewerPicker> P = Live()) P->ApplyColor(Channel, C, false);
    });
    // Closed: the colour it left is saved, if it is a custom one (opening and closing without a change saves nothing).
    Args.OnColorPickerWindowClosed = FOnWindowClosed::CreateLambda([Live, Channel](const TSharedRef<SWindow>&) {
        const TSharedPtr<SSCViewerPicker> P = Live();
        const ASCVehicle* V = P ? P->Subsystem->GetVehicle() : nullptr;
        if (const FLinearColor* Custom = V ? V->GetPaintColorOverrides().Find(Channel) : nullptr) P->ApplyColor(Channel, *Custom, true);
    });
    // Cancelled: back to the colour the channel had when the picker opened.
    const TOptional<FLinearColor> Before = [&]() -> TOptional<FLinearColor> {
        const ASCVehicle* V = Subsystem.IsValid() ? Subsystem->GetVehicle() : nullptr;
        const FLinearColor* Custom = V ? V->GetPaintColorOverrides().Find(Channel) : nullptr;
        return Custom ? TOptional<FLinearColor>(*Custom) : TOptional<FLinearColor>();
    }();
    Args.OnColorPickerCancelled = FOnColorPickerCancelled::CreateLambda([Live, Channel, Before](FLinearColor) {
        const TSharedPtr<SSCViewerPicker> P = Live();
        if (!P) return;
        if (Before.IsSet()) P->ApplyColor(Channel, *Before, true);
        else P->RemoveColor(Channel);
    });
    ::OpenColorPicker(Args);
}

SSCViewerPicker::~SSCViewerPicker()
{
    RestoreScreenMessages();
}

void SSCViewerPicker::RestoreScreenMessages()
{
    if (bHidScreenMessages && !GAreScreenMessagesEnabled) GAreScreenMessagesEnabled = true;
    bHidScreenMessages = false;
}

FReply SSCViewerPicker::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event)
{
    if (Event.GetKey() == EKeys::Escape || Event.GetKey() == EKeys::F1)
    {
        Close();
        return FReply::Handled();
    }
    return SCompoundWidget::OnKeyDown(Geometry, Event);
}

void SSCViewerPicker::Close()
{
    SCViewerShell::SetMenuOpen(Subsystem.Get(), false); // Tick hands the keys back to the view
}

FText SSCViewerPicker::StatusText() const
{
    const USCViewerSubsystem* S = Subsystem.Get();
    if (!S) return FText();
    if (S->GetArchiveState() == ESCArchiveState::Opening) return LOCTEXT("Opening", "READING YOUR STAR CITIZEN FILES…");
    if (S->GetArchiveState() == ESCArchiveState::Failed || S->GetArchiveState() == ESCArchiveState::Closed) return LOCTEXT("NoArchive", "STAR CITIZEN NOT FOUND");
    if (const FString Error = S->GetCatalogError(); !Error.IsEmpty()) return FText::FromString(Caps(Error));
    if (!S->GetCatalog().bReady) return LOCTEXT("Listing", "READING THE SHIP AND HANGAR LISTS…");
    if (QueuedEnvironment.IsSet() || QueuedVehicle.IsSet()) return LOCTEXT("Queued", "LOADING…  YOUR NEXT CHOICE FOLLOWS");
    if (S->IsBusy() || S->IsEnvironmentLoading()) return LOCTEXT("LoadingStatus", "LOADING…");
    if (S->GetVehicle()) return LOCTEXT("Loaded", "F1 SHOWS THE SHIP  ·  SHOW SHIP KEEPS THE PAINTS BESIDE IT");
    return LOCTEXT("Pick", "PICK A HANGAR, A VEHICLE AND ITS PAINT");
}

#undef LOCTEXT_NAMESPACE
