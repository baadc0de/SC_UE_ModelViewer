#include "SSCGarage.h"
#include "../Cargo/SCMobiGlasModel.h"
#include "../Cargo/SCMobiGlasStyle.h"
#include "../Cargo/SCCargoViewSource.h"
#include "SCShipLinkSubsystem.h"
#include "SCVehicle.h"
#include "Engine/World.h"
#include "Brushes/SlateColorBrush.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"

using namespace SCMobiGlasStyle;

namespace
{
FText Text(const TCHAR* Value) { return FText::FromString(Value); }

TSharedRef<SWidget> Button(const TAttribute<FText>& LabelText, TFunction<void()> Press, bool bPrimary = false)
{
    return SNew(SButton).ButtonStyle(bPrimary ? &PrimaryButton() : &FlatButton()).ContentPadding(FMargin(8, 4))
        .OnClicked_Lambda([Press = MoveTemp(Press)] { Press(); return FReply::Handled(); })
        [Txt(LabelText, Label(), bPrimary ? Paper() : Ink())];
}

TSharedRef<SWidget> Future(const TCHAR* Name, const TCHAR* Description)
{
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()[HRule()]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 5, 0, 2)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1)[Txt(Text(Name), Body(), Ink2())]
            + SHorizontalBox::Slot().AutoWidth()[Txt(Text(TEXT("NOT INSTALLED")), Label(), Ink2())]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [SNew(STextBlock).Font(Body()).AutoWrapText(true).ColorAndOpacity(Ink2()).Text(Text(Description))];
}

const FTableViewStyle& FleetViewStyle()
{
    static const FTableViewStyle Style = FTableViewStyle().SetBackgroundBrush(FSlateColorBrush(Paper()));
    return Style;
}

const FTableRowStyle& FleetRowStyle()
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

FString DetailsLine(const FSCGarageRow& Row)
{
    TArray<FString> Details;
    if (!Row.Manufacturer.IsEmpty()) Details.Add(Row.Manufacturer);
    if (!Row.Role.IsEmpty()) Details.Add(Row.Role);
    if (!Row.Size.IsEmpty()) Details.Add(TEXT("Size ") + Row.Size);
    return FString::Join(Details, TEXT("  ·  "));
}

bool Same(const FSCGarageRow& A, const FSCGarageRow& B)
{
    return A.Entity == B.Entity && A.DisplayName == B.DisplayName && A.Status == B.Status &&
        A.Manufacturer == B.Manufacturer && A.Role == B.Role && A.Size == B.Size && A.Error == B.Error && A.Ship == B.Ship && int32(A.Progress * 100) == int32(B.Progress * 100) &&
        FMath::RoundToInt(A.DistanceM) == FMath::RoundToInt(B.DistanceM);
}
}

void SSCGarage::Construct(const FArguments& Args)
{
    Model = Args._Model;
    TSharedRef<SVerticalBox> FleetPane = SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1)
            [
                SNew(SEditableTextBox).HintText(Text(TEXT("Search ships")))
                .OnTextChanged_Lambda([this](const FText& Value) { Search = Value.ToString(); Refresh(); })
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
            [Button(TAttribute<FText>::CreateLambda([this] { return Text(bBrowse ? TEXT("MY FLEET") : TEXT("BROWSE SHIPS")); }),
                [this] { bBrowse = !bBrowse; Refresh(); })]
        ]
        + SVerticalBox::Slot().FillHeight(1)
        [
            SAssignNew(List, SListView<TSharedPtr<FSCGarageRow>>).ListItemsSource(&Visible)
            .ListViewStyle(&FleetViewStyle())
            .SelectionMode(ESelectionMode::Single)
            .OnSelectionChanged_Lambda([this](TSharedPtr<FSCGarageRow> Item, ESelectInfo::Type Type) {
                if (!Item || Type == ESelectInfo::Direct) return;
                Model->SetGarageEntity(Item->Entity);
                ConfirmEntity.Reset();
                Error.Reset();
                DetailKey.Reset();
            })
            .OnGenerateRow_Lambda([this](TSharedPtr<FSCGarageRow> Item, const TSharedRef<STableViewBase>& Table) {
                if (auto* G = Garage()) G->RequestDetails(Item->Entity);
                FString Status = FSCGaragePolicy::StatusName(Item->Status);
                if (Item->Status == ESCGarageStatus::InHangar) Status += FString::Printf(TEXT(" · %.0f m"), Item->DistanceM);
                if (Item->Status == ESCGarageStatus::Retrieving && Item->Progress > 0) Status += FString::Printf(TEXT(" · %.0f%%"), 100 * Item->Progress);
                FString Metadata = DetailsLine(*Item);
                if (const FString* Hint = EntityHints.Find(FName(*Item->Entity)))
                    Metadata = Metadata.IsEmpty() ? *Hint : *Hint + TEXT("  ·  ") + Metadata;
                return SNew(STableRow<TSharedPtr<FSCGarageRow>>, Table).Style(&FleetRowStyle()).Padding(FMargin(8, 6))
                    .ToolTipText(FText::FromString(Item->Entity))
                    [
                        SNew(SVerticalBox)
                        + SVerticalBox::Slot().AutoHeight()[HRule()]
                        + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
                        [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                        [
                            SNew(SVerticalBox)
                            + SVerticalBox::Slot().AutoHeight()
                            [SNew(STextBlock).Font(Body()).Text(FText::FromString(Item->DisplayName)).OverflowPolicy(ETextOverflowPolicy::Ellipsis)]
                            + SVerticalBox::Slot().AutoHeight()
                            [
                                SNew(STextBlock).Font(Label()).ColorAndOpacity(Ink2()).Text(FText::FromString(Metadata))
                                .Visibility(Metadata.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
                                .OverflowPolicy(ETextOverflowPolicy::Ellipsis)
                            ]
                        ]
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0)
                        [Txt(FText::FromString(Caps(Status)), Label(), Ink2())]
                        ]
                    ];
            })
        ];

    ChildSlot
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(510)[Panel(Text(TEXT("Fleet")), FText(), FleetPane)]
        + SHorizontalBox::Slot().FillWidth(960).Padding(Gutter, 0, 0, 0)
        [
            Panel(Text(TEXT("Configuration")), FText(),
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock).Font(Body()).ColorAndOpacity(Alert()).AutoWrapText(true)
                    .Text_Lambda([this] { return FText::FromString(Error); })
                    .Visibility_Lambda([this] { return Error.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
                ]
                + SVerticalBox::Slot().FillHeight(1)[SAssignNew(DetailBody, SBox)]
            )
        ]
    ];
}

USCGarageSubsystem* SSCGarage::Garage() const { return USCGarageSubsystem::Get(SCCargoUI::ViewerWorld()); }

const FSCGarageRow* SSCGarage::Selected() const
{
    const FString& Entity = Model->GetGarageEntity();
    return Fleet.FindByPredicate([&](const FSCGarageRow& R) { return R.Entity.Equals(Entity, ESearchCase::IgnoreCase); });
}

void SSCGarage::Tick(const FGeometry& Geometry, double Time, float Delta)
{
    SCompoundWidget::Tick(Geometry, Time, Delta);
    if (Model->GetApp() != ESCMobiGlasApp::Garage) return;
    Age += Delta;
    if (Age >= 0.25f) Refresh();
    const FSCGarageRow* Row = Selected();
    const USCShipLinkSubsystem* Link = USCShipLinkSubsystem::Get(SCCargoUI::ViewerWorld());
    const ASCVehicle* Ship = Row && Link ? Link->FindShip(Row->Ship) : nullptr;
    const FString Key = Row ? FString::Printf(TEXT("%s:%d:%d:%d"), *Row->Entity, int32(Row->Status), Row->Ship, Ship ? Ship->GetPaint() : INDEX_NONE) + Row->Manufacturer + Row->Role + Row->Size
                            : Model->GetGarageEntity();
    if (DetailKey != Key)
    {
        DetailKey = Key;
        DetailBody->SetContent(Details());
    }
}

void SSCGarage::Refresh()
{
    Age = 0;
    USCGarageSubsystem* Storehouse = Garage();
    if (!Storehouse || !List) return;
    Storehouse->RequestCatalog();
    Fleet = Storehouse->Rows();
    TArray<FSCGarageRow> Rows = Fleet;
    if (bBrowse)
        for (const FSCEntityHit& Entity : Storehouse->GetCatalog())
            if (!Rows.ContainsByPredicate([&](const FSCGarageRow& R) { return R.Entity == Entity.Name; }))
            {
                FSCGarageRow& Entry = Rows.AddDefaulted_GetRef();
                Entry.Entity = Entity.Name;
                Entry.DisplayName = Entity.DisplayName.IsEmpty() ? Entity.Name : Entity.DisplayName;
            }
    if (!Search.IsEmpty())
        Rows.RemoveAll([&](const FSCGarageRow& Row) {
            return !Row.DisplayName.Contains(Search) && !Row.Entity.Contains(Search);
        });
    if (Visible.Num() == Rows.Num())
    {
        bool bSame = true;
        for (int32 I = 0; I < Rows.Num() && bSame; ++I) bSame = Same(*Visible[I], Rows[I]);
        if (bSame) { SyncSelection(false); return; }
    }
    struct FDisplayGroup
    {
        const FString* Shortest = nullptr;
        int32 Count = 0;
    };
    TMap<FString, FDisplayGroup> Groups;
    for (const FSCGarageRow& Row : Rows)
    {
        FDisplayGroup& Group = Groups.FindOrAdd(Row.DisplayName);
        if (!Group.Shortest || Row.Entity.Len() < Group.Shortest->Len()) Group.Shortest = &Row.Entity;
        ++Group.Count;
    }
    EntityHints.Reset();
    for (const FSCGarageRow& Row : Rows)
    {
        const FDisplayGroup& Group = Groups.FindChecked(Row.DisplayName);
        if (Group.Count < 2) continue;
        // Several authored classes share a localized title. Keep each class addressable and show its identifier.
        const FString Prefix = *Group.Shortest + TEXT("_");
        const FString Identifier = Row.Entity.StartsWith(Prefix) ? Row.Entity.RightChop(Prefix.Len()) : Row.Entity;
        EntityHints.Add(FName(*Row.Entity), Identifier.Replace(TEXT("_"), TEXT(" ")));
    }
    Visible.Reset();
    for (FSCGarageRow& Row : Rows) Visible.Add(MakeShared<FSCGarageRow>(MoveTemp(Row)));
    List->RequestListRefresh();
    SyncSelection(true);
}

void SSCGarage::SyncSelection(bool bRowsChanged)
{
    if (Visible.IsEmpty()) return;
    if (Model->GetGarageEntity().IsEmpty()) Model->SetGarageEntity(Visible[0]->Entity);
    const FString& SelectedEntity = Model->GetGarageEntity();
    const bool bSelectionChanged = LastSelectedEntity != SelectedEntity;
    if (!bRowsChanged && !bSelectionChanged) return;
    LastSelectedEntity = SelectedEntity;
    if (const auto* Match = Visible.FindByPredicate([&](const auto& Row) { return Row->Entity.Equals(SelectedEntity, ESearchCase::IgnoreCase); }))
    {
        List->SetSelection(*Match, ESelectInfo::Direct);
        if (bSelectionChanged || (*Match)->Status == ESCGarageStatus::InHangar) List->RequestScrollIntoView(*Match);
    }
}

TSharedRef<SWidget> SSCGarage::Details()
{
    const FSCGarageRow* Row = Selected();
    if (!Row)
        return Model->GetGarageEntity().IsEmpty() ? Txt(Text(TEXT("Select a ship in your fleet.")), Body(), Ink2())
            : Button(Text(TEXT("ADD TO FLEET")), [this] { AddToFleet(); }, true);

    TSharedRef<SScrollBox> BodyRows = SNew(SScrollBox);
    BodyRows->AddSlot().Padding(0, 0, 0, 6)[Txt(FText::FromString(Row->DisplayName), Title(), Ink())];
    if (const FString Metadata = DetailsLine(*Row); !Metadata.IsEmpty())
        BodyRows->AddSlot().Padding(0, 0, 0, 6)[Txt(FText::FromString(Metadata), Label(), Ink2())];
    if (!Row->Error.IsEmpty())
        BodyRows->AddSlot().Padding(0, 3)
        [SNew(STextBlock).Text(FText::FromString(Row->Error)).ColorAndOpacity(Alert()).Font(Body()).AutoWrapText(true)];
    if (Row->Status == ESCGarageStatus::Stored || Row->Status == ESCGarageStatus::Unavailable)
        BodyRows->AddSlot().Padding(0, 4)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
            [Button(TAttribute<FText>::CreateLambda([this] {
                return Text(ConfirmEntity == Model->GetGarageEntity() ? TEXT("STORE AND RETRIEVE") : TEXT("RETRIEVE TO BAY"));
            }), [this] { Retrieve(ESCGaragePlacement::Bay); }, true)]
            + SHorizontalBox::Slot().AutoWidth()[Button(Text(TEXT("RETRIEVE TO FLOOR")), [this] { Retrieve(ESCGaragePlacement::Floor); })]
        ];
    else if (Row->Status == ESCGarageStatus::InHangar)
        BodyRows->AddSlot().Padding(0, 4)[SNew(SBox).HAlign(HAlign_Left)[Button(Text(TEXT("STORE SHIP")), [this] { Store(); }, true)]];
    else
        BodyRows->AddSlot().Padding(0, 4)[Txt(Text(TEXT("RETRIEVING")), Label(), Ink2())];

    const USCShipLinkSubsystem* Link = USCShipLinkSubsystem::Get(SCCargoUI::ViewerWorld());
    const ASCVehicle* Ship = Link ? Link->FindShip(Row->Ship) : nullptr;
    if (Ship)
    {
        TSharedRef<SHorizontalBox> Paints = SNew(SHorizontalBox);
        for (int32 Index = INDEX_NONE; Index < Ship->Manifest.Paints.Num(); ++Index)
        {
            const FString Label = Index == INDEX_NONE ? FString(TEXT("DEFAULT")) : Ship->Manifest.Paints[Index].Label();
            const FString Palette = Index == INDEX_NONE ? FString() : Ship->Manifest.Paints[Index].PaletteId;
            Paints->AddSlot().AutoWidth().Padding(0, 0, 5, 0)
                [Button(FText::FromString(Label), [this, Id = Row->Ship, Palette] {
                    if (auto* G = Garage()) Error = G->Paint(Id, Palette);
                }, Index == Ship->GetPaint())];
        }
        BodyRows->AddSlot().Padding(0, 12, 0, 5)[Txt(Text(TEXT("PAINT")), Section(), Ink())];
        BodyRows->AddSlot()[SNew(SScrollBox).Orientation(Orient_Horizontal) + SScrollBox::Slot()[Paints]];
        BodyRows->AddSlot().Padding(0, 12, 0, 5)[Txt(Text(TEXT("LOADOUT")), Section(), Ink())];
        TMap<FString, TArray<const FSCSceneInstance*>> Parts;
        for (const auto& Item : Ship->Manifest.Children)
            if (!Item.PortPath.IsEmpty() && !Item.EntityName.IsEmpty())
                Parts.FindOrAdd(Item.ItemType.IsEmpty() ? TEXT("Other") : Item.ItemType).Add(&Item);
        TArray<FString> Groups;
        Parts.GetKeys(Groups);
        Groups.Sort();
        for (const FString& Group : Groups)
        {
            BodyRows->AddSlot().Padding(0, 8, 0, 2)[Txt(FText::FromString(Caps(Group)), Label(), Ink2())];
            for (const FSCSceneInstance* Item : Parts.FindChecked(Group))
            {
                const FString Name = Item->DisplayName.IsEmpty() ? Item->EntityName.Replace(TEXT("_"), TEXT(" ")) : Item->DisplayName;
                const FString Port = Item->PortPath.Last().Replace(TEXT("_"), TEXT(" "));
                const FString Size = Item->ItemSize.IsEmpty() ? FString() : TEXT("  ·  ") + Item->ItemSize;
                BodyRows->AddSlot().Padding(0, 2)
                    [SNew(STextBlock).Text(FText::FromString(Port + TEXT("  ·  ") + Name + Size)).Font(Body()).ColorAndOpacity(Ink()).AutoWrapText(true)];
            }
        }
    }
    TSharedRef<SScrollBox> FutureRows = SNew(SScrollBox);
    FutureRows->AddSlot().Padding(0, 12, 0, 0)[Future(TEXT("Loadout changes"), TEXT("Swap weapons, shields and components."))];
    FutureRows->AddSlot()[Future(TEXT("Ship delivery"), TEXT("Watch the hangar elevator bring the ship up and take it down."))];
    FutureRows->AddSlot()[Future(TEXT("Insurance claim"), TEXT("Replace a lost or destroyed ship."))];
    FutureRows->AddSlot()[Future(TEXT("Transfer"), TEXT("Move a ship to another hangar or location."))];
    FutureRows->AddSlot()[Future(TEXT("Hand over"), TEXT("Give a ship to another player."))];
    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(0.62f)[BodyRows]
        + SHorizontalBox::Slot().FillWidth(0.38f).Padding(16, 0, 0, 0)[FutureRows];
}

void SSCGarage::Retrieve(ESCGaragePlacement At)
{
    if (USCGarageSubsystem* G = Garage())
    {
        const FString Entity = Model->GetGarageEntity();
        const FSCGarageOrder Result = G->Retrieve(Entity, At, ConfirmEntity == Entity, ConfirmOccupant);
        Error = Result.Refusal;
        ConfirmEntity = Result.bConfirmStore ? Entity : FString();
        ConfirmOccupant = Result.bConfirmStore ? Result.Occupant : INDEX_NONE;
        Refresh();
    }
}

void SSCGarage::Store()
{
    if (const FSCGarageRow* Row = Selected())
        if (USCGarageSubsystem* G = Garage())
        {
            Error = G->Store(Row->Ship);
            ConfirmEntity.Reset();
            Refresh();
        }
}

void SSCGarage::AddToFleet()
{
    if (USCGarageSubsystem* G = Garage())
    {
        Error = G->AddToFleet(Model->GetGarageEntity()) ? FString() : FString(TEXT("Could not add that ship to the fleet"));
        DetailKey.Reset();
        Refresh();
    }
}
