// SSCViewerPicker: the environment, vehicle and paint lists of the viewer's menu, in the shared HUD look (SCHudStyle).
#include "SSCViewerPicker.h"
#include "SCVehicle.h"
#include "SCViewerShell.h"
#include "SCViewerSubsystem.h"
#include "UI/SCHudStyle.h"
#include "Brushes/SlateColorBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Styling/SlateTypes.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SCViewerPicker"
using namespace SCHudStyle;

namespace
{
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
/** A row: the accent edge on the current choice, the name, and a dim second line. */
TSharedRef<SWidget> Row(const FString& Name, const FString& Second, TFunction<bool()> bCurrent)
{
    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth()
        [
            SNew(SBox).WidthOverride(FocusWidth * 2)
            [SNew(SBorder).BorderImage(White()).BorderBackgroundColor_Lambda([bCurrent] { return bCurrent() ? Accent() : FLinearColor::Transparent; })]
        ]
        + SHorizontalBox::Slot().FillWidth(1).Padding(8, 5)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Font(Body()).ColorAndOpacity(Ink()).Text(FText::FromString(Name)).OverflowPolicy(ETextOverflowPolicy::Ellipsis)]
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock).Font(Label()).ColorAndOpacity(Ink2()).Text(FText::FromString(Second)).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
                .Visibility(Second.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
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
    // The picker itself keeps ticking (a collapsed widget does not: the start check and the lists' refresh run in
    // Tick); its content shows while the menu is open.
    SetVisibility(EVisibility::SelfHitTestInvisible);

    TSharedRef<SWidget> EnvironmentList = MakeList(Environments, LOCTEXT("Environment", "Environment"), LOCTEXT("SearchPlaces", "Search places"),
        [Weak] { return Weak.IsValid() ? Weak->GetEnvironmentRequest() : FString(); },
        [Weak](const FSCCatalogEntry& E) { if (Weak.IsValid()) Weak->SetEnvironment(E.Id, false); });
    TSharedRef<SWidget> VehicleList = MakeList(Vehicles, LOCTEXT("Vehicle", "Vehicle"), LOCTEXT("SearchVehicles", "Search vehicles"),
        [Weak] { return !Weak.IsValid() ? FString() : Weak->GetPendingEntity().IsEmpty() ? Weak->GetLoadedEntity() : Weak->GetPendingEntity(); },
        [Weak](const FSCCatalogEntry& E) { if (Weak.IsValid() && Weak->CanLoad(E.Id)) Weak->LoadEntity(E.Id, Weak->GetOptions()); });

    TSharedRef<SWidget> PaintList = Panel(LOCTEXT("Paint", "Paint"), FText(),
        SAssignNew(PaintView, SListView<FPaint>).ListItemsSource(&Paints).ListViewStyle(&ListStyle()).SelectionMode(ESelectionMode::None)
        .OnGenerateRow_Lambda([Weak](FPaint Item, const TSharedRef<STableViewBase>& Table) {
            const int32 Index = Item->Index;
            return SNew(STableRow<FPaint>, Table).Style(&RowStyle()).Padding(0)
            [
                SNew(SButton).ButtonStyle(&RowButton()).ContentPadding(0).OnClicked_Lambda([Weak, Index] {
                    if (Weak.IsValid()) Weak->SetPaint(Index);
                    return FReply::Handled();
                })
                [Row(Item->Label, FString(), [Weak, Index] {
                    const ASCVehicle* V = Weak.IsValid() ? Weak->GetVehicle() : nullptr;
                    return V && V->GetPaint() == Index;
                })]
            ];
        }));

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

TSharedRef<SWidget> SSCViewerPicker::MakeList(FList& List, const FText& Title, const FText& Hint, TFunction<FString()> Current,
                                              TFunction<void(const FSCCatalogEntry&)> Pick)
{
    FList* L = &List;
    return Panel(Title, TAttribute<FText>::CreateLambda([L] { return FText::AsNumber(L->Shown.Num()); }),
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [
            SNew(SEditableTextBox).HintText(Hint).OnTextChanged_Lambda([this, L](const FText& Value) {
                L->Filter = Value.ToString();
                Refilter(*L);
            })
        ]
        + SVerticalBox::Slot().FillHeight(1)
        [
            SAssignNew(List.View, SListView<FEntry>).ListItemsSource(&List.Shown).ListViewStyle(&ListStyle()).SelectionMode(ESelectionMode::None)
            .OnGenerateRow_Lambda([Current, Pick](FEntry Item, const TSharedRef<STableViewBase>& Table) {
                const FString Id = Item->Id;
                return SNew(STableRow<FEntry>, Table).Style(&RowStyle()).Padding(0).ToolTipText(FText::FromString(Id))
                [
                    SNew(SButton).ButtonStyle(&RowButton()).ContentPadding(0).OnClicked_Lambda([Pick, Item] {
                        Pick(*Item);
                        return FReply::Handled();
                    })
                    [Row(Item->Label, SecondLine(*Item), [Current, Id] { return Current().Equals(Id, ESearchCase::IgnoreCase); })]
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
    if (bOpen && !bWasOpen)
    {
        if (FSlateApplication::IsInitialized())
            FSlateApplication::Get().SetAllUserFocus(SharedThis(this), EFocusCause::SetDirectly); // Esc / F1 reach the menu
        // The current environment and vehicle in view.
        auto Reveal = [](FList& L, const FString& Id) {
            if (const FEntry* E = L.Shown.FindByPredicate([&](const FEntry& X) { return X->Id.Equals(Id, ESearchCase::IgnoreCase); }); E && L.View)
                L.View->RequestScrollIntoView(*E);
        };
        Reveal(Environments, S->GetEnvironmentRequest());
        Reveal(Vehicles, S->GetPendingEntity().IsEmpty() ? S->GetLoadedEntity() : S->GetPendingEntity());
    }
    bWasOpen = bOpen;
    const FSCViewerCatalog& Catalog = S->GetCatalog();
    if (Catalog.Revision != CatalogRevision || Catalog.bReady != bCatalogReady)
    {
        CatalogRevision = Catalog.Revision;
        bCatalogReady = Catalog.bReady;
        auto Fill = [this](FList& L, const TArray<FSCCatalogEntry>& Rows) {
            L.All.Reset();
            for (const FSCCatalogEntry& E : Rows) L.All.Add(MakeShared<FSCCatalogEntry>(E));
            Refilter(L);
        };
        Fill(Environments, Catalog.Environments);
        Fill(Vehicles, Catalog.Vehicles);
    }
    // The paints of the vehicle in the scene, listed again when it changes.
    const ASCVehicle* V = S->GetVehicle();
    const FString Entity = V ? S->GetLoadedEntity() : FString();
    if (Entity != PaintsOf)
    {
        PaintsOf = Entity;
        Paints.Reset();
        if (V)
        {
            Paints.Add(MakeShared<FPaintRow>(FPaintRow{INDEX_NONE, LOCTEXT("OwnPaint", "The vehicle's own").ToString()}));
            for (int32 I = 0; I < V->Manifest.Paints.Num(); ++I) Paints.Add(MakeShared<FPaintRow>(FPaintRow{I, V->Manifest.Paints[I].Label()}));
        }
        if (PaintView) PaintView->RequestListRefresh();
    }
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
    SCViewerShell::SetMenuOpen(Subsystem.Get(), false);
    if (FocusView) FocusView();
}

FText SSCViewerPicker::StatusText() const
{
    const USCViewerSubsystem* S = Subsystem.Get();
    if (!S) return FText();
    if (S->GetArchiveState() == ESCArchiveState::Opening) return LOCTEXT("Opening", "OPENING THE ARCHIVE…");
    if (S->GetArchiveState() == ESCArchiveState::Failed) return LOCTEXT("NoArchive", "NO ARCHIVE");
    const FSCViewerCatalog& Catalog = S->GetCatalog();
    if (!Catalog.bReady) return Catalog.Error.IsEmpty() ? LOCTEXT("Listing", "LISTING…") : FText::FromString(Caps(Catalog.Error));
    if (S->IsBusy() || S->IsEnvironmentLoading()) return LOCTEXT("Loading", "LOADING…");
    return LOCTEXT("Pick", "PICK A PLACE, A VEHICLE AND ITS PAINT");
}

#undef LOCTEXT_NAMESPACE
