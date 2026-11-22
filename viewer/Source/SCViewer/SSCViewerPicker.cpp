// SSCViewerPicker: the environment, vehicle and paint lists of the viewer's menu, in the shared HUD look (SCHudStyle).
#include "SSCViewerPicker.h"
#include "SCVehicle.h"
#include "SCViewerSettings.h"
#include "SCViewerShell.h"
#include "SCViewerSubsystem.h"
#include "UI/SCHudStyle.h"
#include "Brushes/SlateColorBrush.h"
#include "Framework/Application/SlateApplication.h"
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
} // namespace

void SSCViewerPicker::Construct(const FArguments& Args)
{
    Subsystem = Args._Subsystem;
    FocusView = Args._FocusView;
    TWeakObjectPtr<USCViewerSubsystem> Weak = Subsystem;
    // The picker itself keeps ticking (a collapsed widget does not: the start check, queued choices and the lists'
    // refresh run in Tick); its content shows while the menu is open.
    SetVisibility(EVisibility::SelfHitTestInvisible);
    FParse::Value(FCommandLine::Get(), TEXT("SCSearch="), Vehicles.Filter); // verification aid: prefill the vehicle search

    TSharedRef<SWidget> EnvironmentList = MakeList(Environments, LOCTEXT("Environment", "Environment"), LOCTEXT("SearchPlaces", "Search places"),
        [this](const FString& Id) { return EnvironmentState(Id); }, [this](const FString& Id) { PickEnvironment(Id); });
    TSharedRef<SWidget> VehicleList = MakeList(Vehicles, LOCTEXT("Vehicle", "Vehicle"), LOCTEXT("SearchVehicles", "Search vehicles"),
        [this](const FString& Id) { return VehicleState(Id); }, [this](const FString& Id) { PickVehicle(Id); });

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

    ChildSlot
    [
        SNew(SBorder).BorderImage(White()).BorderBackgroundColor(ScrimOpen()).Padding(SafeMargin)
        .Visibility_Lambda([Weak] { return SCViewerShell::IsMenuOpen(Weak.Get()) ? EVisibility::Visible : EVisibility::Collapsed; })
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, Gutter)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [Txt(TAttribute<FText>::CreateLambda([] { return Caps(SCViewerShell::AppName()); }), Display(), OnScrim())]
                + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(Gutter, 0)
                [Txt(TAttribute<FText>::CreateSP(this, &SSCViewerPicker::StatusText), Label(), OnScrim())]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    SNew(SButton).ButtonStyle(&FlatButton()).OnClicked_Lambda([this] { Close(); return FReply::Handled(); })
                    [Txt(LOCTEXT("Close", "F1  CLOSE"), Label(), Ink())]
                ]
            ]
            + SVerticalBox::Slot().FillHeight(1)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1)[EnvironmentList]
                + SHorizontalBox::Slot().FillWidth(1.2f).Padding(Gutter, 0, 0, 0)[VehicleList]
                + SHorizontalBox::Slot().FillWidth(0.8f).Padding(Gutter, 0, 0, 0)[PaintList]
            ]
        ]
    ];
}

TSharedRef<SWidget> SSCViewerPicker::MakeList(FList& List, const FText& Title, const FText& Hint, TFunction<ERowState(const FString&)> State,
                                              TFunction<void(const FString&)> Pick)
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
            SAssignNew(List.View, SListView<FEntry>).ListItemsSource(&List.Shown).ListViewStyle(&ListStyle()).SelectionMode(ESelectionMode::None)
            .OnGenerateRow_Lambda([State, Pick](FEntry Item, const TSharedRef<STableViewBase>& Table) {
                const FString Id = Item->Id;
                const FString Second = SecondLine(*Item);
                // The full name (the row may cut it) over the id the viewer loads.
                const FText Tip = FText::FromString(Item->Label + (Second.IsEmpty() ? FString() : TEXT("\n") + Second) + TEXT("\n") + Id);
                return SNew(STableRow<FEntry>, Table).Style(&RowStyle()).Padding(0).ToolTipText(Tip)
                [
                    SNew(SButton).ButtonStyle(&RowButton()).ContentPadding(0).OnClicked_Lambda([Pick, Id] {
                        Pick(Id);
                        return FReply::Handled();
                    })
                    [Row(Item->Label, Second, [State, Id] { return State(Id); })]
                ];
            })
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

SSCViewerPicker::EVehiclePick SSCViewerPicker::DecideVehiclePick(const FString& Id, const FVehiclePickState& State)
{
    // The latest click wins. The load running for it (and not being stopped): drop any later choice waiting behind it.
    if (State.bLoading && !State.bCancelling && SameId(State.Pending, Id)) return EVehiclePick::Keep;
    // Idle: LoadEntity decides (it keeps the vehicle in the scene when the request is identical for this archive session).
    if (State.bCanLoad) return EVehiclePick::Load;
    // A load runs (or the archive opens): this choice follows it, also when it is the vehicle being replaced.
    return EVehiclePick::Queue;
}

SSCViewerPicker::EEnvironmentPick SSCViewerPicker::DecideEnvironmentPick(const FString& Id, const FEnvironmentPickState& State)
{
    // Already there, or on its way and not being stopped: nothing to do (a failed, stopped or other archive's one is
    // requested again).
    const bool bThere = IsMapHangar(Id) ? IsMapHangar(State.Request) && !State.bWorking
                                        : SameId(Id, State.Request) && (State.bInstalled || (State.bLoading && !State.bCancelling));
    if (bThere) return EEnvironmentPick::There;
    // A running export, parse or import would make RequestEnvironment wait for its workers on the game thread.
    return State.bWorking ? EEnvironmentPick::Queue : EEnvironmentPick::Request;
}

void SSCViewerPicker::PickEnvironment(const FString& Id)
{
    USCViewerSubsystem* S = Subsystem.Get();
    if (!S) return;
    // Saved as the user's environment once it is in the scene (Tick), so a failed choice is not restored at the next start.
    SaveEnvironment = Id;
    FEnvironmentPickState State;
    State.Request = S->GetEnvironmentRequest();
    State.bInstalled = S->IsEnvironmentOfThisArchive();
    State.bLoading = S->IsEnvironmentLoading();
    State.bCancelling = S->IsEnvironmentCancelling();
    State.bWorking = S->IsEnvironmentWorking();
    switch (DecideEnvironmentPick(Id, State))
    {
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

void SSCViewerPicker::PickVehicle(const FString& Id)
{
    USCViewerSubsystem* S = Subsystem.Get();
    if (!S) return;
    FVehiclePickState State;
    State.bLoading = IsLoadingVehicle(*S);
    State.bCancelling = S->IsLoadCancelling();
    State.bCanLoad = S->CanLoad(Id);
    State.Pending = S->GetPendingEntity();
    switch (DecideVehiclePick(Id, State))
    {
    case EVehiclePick::Keep:
        QueuedVehicle.Reset();
        break;
    case EVehiclePick::Load:
        QueuedVehicle.Reset();
        S->LoadEntity(Id, S->GetOptions());
        break;
    case EVehiclePick::Queue:
        QueuedVehicle = Id;
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
    // nothing loads (a vehicle waits for the environment it goes into).
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
        RestoreScreenMessages();
        if (FocusView) FocusView(); // however it closed (F1, Esc, the button, RPC /ui): keys go back to the view
    }
    bWasOpen = bOpen;

    const FSCViewerCatalog& Catalog = S->GetCatalog();
    if (Catalog.Revision != CatalogRevision || Catalog.bReady != bCatalogReady)
    {
        CatalogRevision = Catalog.Revision;
        bCatalogReady = Catalog.bReady;
        auto Fill = [this](FList& L, const TArray<FSCCatalogEntry>& Rows, const FSCCatalogEntry* First) {
            L.All.Reset();
            if (First) L.All.Add(MakeShared<FSCCatalogEntry>(*First));
            for (const FSCCatalogEntry& E : Rows) L.All.Add(MakeShared<FSCCatalogEntry>(E));
            Refilter(L);
        };
        FSCCatalogEntry Procedural;
        Procedural.Id = MapHangar;
        Procedural.Label = LOCTEXT("MapHangar", "Procedural hangar").ToString();
        Procedural.Detail = LOCTEXT("MapHangarDetail", "The viewer's own, no game environment").ToString();
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
            Paints.Add(MakeShared<FPaintRow>(FPaintRow{INDEX_NONE, LOCTEXT("OwnPaint", "The vehicle's own").ToString(), V}));
            for (int32 I = 0; I < V->Manifest.Paints.Num(); ++I) Paints.Add(MakeShared<FPaintRow>(FPaintRow{I, V->Manifest.Paints[I].Label(), V}));
        }
        if (PaintView) PaintView->RequestListRefresh();
        bReveal |= bOpen;
    }
    if (bReveal) RevealCurrent();
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
    if (S->GetArchiveState() == ESCArchiveState::Opening) return LOCTEXT("Opening", "OPENING THE ARCHIVE…");
    if (S->GetArchiveState() == ESCArchiveState::Failed || S->GetArchiveState() == ESCArchiveState::Closed) return LOCTEXT("NoArchive", "NO ARCHIVE");
    if (const FString Error = S->GetCatalogError(); !Error.IsEmpty()) return FText::FromString(Caps(Error));
    if (!S->GetCatalog().bReady) return LOCTEXT("Listing", "LISTING…");
    if (QueuedEnvironment.IsSet() || QueuedVehicle.IsSet()) return LOCTEXT("Queued", "LOADING…  YOUR NEXT CHOICE FOLLOWS");
    if (S->IsBusy() || S->IsEnvironmentLoading()) return LOCTEXT("LoadingStatus", "LOADING…");
    return LOCTEXT("Pick", "PICK A PLACE, A VEHICLE AND ITS PAINT");
}

#undef LOCTEXT_NAMESPACE
