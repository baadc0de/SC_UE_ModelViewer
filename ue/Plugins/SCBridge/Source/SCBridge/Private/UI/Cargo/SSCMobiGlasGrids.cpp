// SSCMobiGlasNearby and SSCMobiGlasDetail: the grid list grouped by owner and the selected grid's lattice and contents.
#include "SSCMobiGlasPanels.h"
#include "SSCMobiGlasWidgets.h"
#include "SCMobiGlasModel.h"
#include "SCMobiGlasStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SCMobiGlas"
using namespace SCMobiGlasStyle;

namespace SCMobiGlasText
{
bool IsFixed(const FSCCargoGridView& G)
{
    return G.Access == ESCCargoGridAccess::Open && !G.bCanClose && G.CloseBlockedReason.IsEmpty();
}
FText Access(const FSCCargoGridView& G)
{
    if (IsFixed(G)) return LOCTEXT("Fixed", "OPEN");
    switch (G.Access)
    {
    case ESCCargoGridAccess::Open: return LOCTEXT("Extended", "EXTENDED");
    case ESCCargoGridAccess::Opening: return LOCTEXT("Extending", "EXTENDING");
    case ESCCargoGridAccess::Closing: return LOCTEXT("Retracting", "RETRACTING");
    default: return LOCTEXT("Retracted", "RETRACTED");
    }
}
FLinearColor AccessColour(const FSCCargoGridView& G)
{
    switch (G.Access)
    {
    case ESCCargoGridAccess::Open: return Ok();
    case ESCCargoGridAccess::Opening:
    case ESCCargoGridAccess::Closing: return Warn();
    default: return PaperDim();
    }
}
FText Scu(int32 Used, int32 Total) { return FText::Format(LOCTEXT("Scu", "{0} / {1} SCU"), Used, Total); }
FText Metres(float M)
{
    FNumberFormattingOptions O;
    O.SetMinimumFractionalDigits(1).SetMaximumFractionalDigits(1);
    return FText::Format(LOCTEXT("Metres", "{0} m"), FText::AsNumber(M, &O));
}
FText Duration(float Seconds)
{
    const int32 S = FMath::Max(0, FMath::CeilToInt32(Seconds));
    if (S < 60) return FText::Format(LOCTEXT("Sec", "{0} s"), S);
    return FText::Format(LOCTEXT("MinSec", "{0} min {1} s"), S / 60, S % 60);
}
FString DroneState(uint8 S)
{
    switch (ESCCargoDroneState(S))
    {
    case ESCCargoDroneState::Docked: return TEXT("DOCKED");
    case ESCCargoDroneState::Launching: return TEXT("LAUNCHING");
    case ESCCargoDroneState::ToPickup: return TEXT("TO PICKUP");
    case ESCCargoDroneState::Attaching: return TEXT("ATTACHING");
    case ESCCargoDroneState::Carrying: return TEXT("CARRYING");
    case ESCCargoDroneState::Releasing: return TEXT("RELEASING");
    case ESCCargoDroneState::Returning: return TEXT("RETURNING");
    case ESCCargoDroneState::Docking: return TEXT("DOCKING");
    default: return StaticEnum<ESCCargoDroneState>()->GetNameStringByValue(S).ToUpper(); // e.g. HOLDING
    }
}
FString BoxState(uint8 S)
{
    switch (ESCCargoBoxState(S))
    {
    case ESCCargoBoxState::Stored: return TEXT("STORED");
    case ESCCargoBoxState::Reserved: return TEXT("RESERVED");
    case ESCCargoBoxState::Lifting: return TEXT("LIFTING");
    case ESCCargoBoxState::InTransit: return TEXT("IN TRANSIT");
    case ESCCargoBoxState::Placing: return TEXT("PLACING");
    default: return StaticEnum<ESCCargoBoxState>()->GetNameStringByValue(S).ToUpper(); // e.g. HELD
    }
}
} // namespace SCMobiGlasText

namespace
{
TSharedRef<SWidget> Empty(const FText& Text)
{
    return SNew(SBox).Padding(FMargin(0, 8))[SNew(STextBlock).Font(Body()).ColorAndOpacity(Ink2()).AutoWrapText(true).Text(Text)];
}
/** EXTEND / RETRACT for a grid or a whole ship; hidden for fixed grids, disabled (reason as tooltip) when refused. */
TSharedRef<SWidget> AccessButton(TSharedPtr<FSCMobiGlasModel> M, TFunction<TArray<int32>()> Ids)
{
    auto Control = [M, Ids] { return M->GetAccessControl(Ids()); };
    return SNew(SBorder).BorderImage(White()).BorderBackgroundColor(Rule()).Padding(1)
        .Visibility_Lambda([Control] { return Control().bShown ? EVisibility::Visible : EVisibility::Collapsed; })
        .ToolTipText_Lambda([Control] { return Control().Reason; })
        [
            SNew(SButton).ButtonStyle(&FlatButton()).ContentPadding(FMargin(10, 2))
            .IsEnabled_Lambda([Control] { return Control().bEnabled; })
            .OnClicked_Lambda([M, Ids, Control] { M->RequestAccess(Ids(), Control().bExtend); return FReply::Handled(); })
            [SNew(STextBlock).Font(Label()).ColorAndOpacity(FSlateColor::UseForeground()).Text_Lambda([Control] {
                return Control().bExtend ? LOCTEXT("Extend", "EXTEND") : LOCTEXT("Retract", "RETRACT");
            })]
        ];
}
} // namespace

// ---- Nearby grids ---------------------------------------------------------------------------------------------------
void SSCMobiGlasNearby::Construct(const FArguments& Args)
{
    Model = Args._Model;
    ChildSlot
    [
        Panel(LOCTEXT("Nearby", "Nearby grids"),
              TAttribute<FText>::CreateLambda([this] {
                  return Model->GetNearbyRadiusM() > 0 ? FText::Format(LOCTEXT("Within", "{0} M"), FMath::RoundToInt32(Model->GetNearbyRadiusM()))
                                                       : FText();
              }),
              SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(List, SVerticalBox)])
    ];
}
void SSCMobiGlasNearby::Tick(const FGeometry& Geometry, double Time, float Delta)
{
    SCompoundWidget::Tick(Geometry, Time, Delta);
    if (BuiltStamp != Model->GetStamp()) Rebuild();
}
void SSCMobiGlasNearby::Rebuild()
{
    BuiltStamp = Model->GetStamp();
    List->ClearChildren();
    if (!Model->HasSource()) { List->AddSlot().AutoHeight()[Empty(LOCTEXT("NoSource", "No cargo system in this scene."))]; return; }
    if (Model->GetGrids().IsEmpty()) { List->AddSlot().AutoHeight()[Empty(LOCTEXT("NoGrids", "No cargo grids nearby."))]; return; }
    TArray<FName> Groups;
    for (const FSCCargoGridView& G : Model->GetGrids()) Groups.AddUnique(G.GroupKey);
    TSharedPtr<FSCMobiGlasModel> M = Model;
    // The player's ships and the shared platforms first; other owners' vessels after, dimmed and never offered.
    Groups.StableSort([this](FName A, FName B) { return Model->IsGroupMine(A) && !Model->IsGroupMine(B); });
    bool bOthersHeader = false;
    for (int32 GroupIndex = 0; GroupIndex < Groups.Num(); ++GroupIndex)
    {
        const FName Group = Groups[GroupIndex];
        const bool bOther = !Model->IsGroupMine(Group);
        if (bOther && !bOthersHeader)
        {
            bOthersHeader = true;
            List->AddSlot().AutoHeight().Padding(0, 20, 0, 0)[HRule()];
            List->AddSlot().AutoHeight().Padding(0, 8, 0, 0)[Txt(LOCTEXT("Others", "OTHER VESSELS"), Label(), Ink2())];
        }
        const FSCCargoGridView* First = Model->GetGrids().FindByPredicate([Group](const FSCCargoGridView& G) { return G.GroupKey == Group; });
        List->AddSlot().AutoHeight().Padding(0, GroupIndex ? 16 : 0, 0, 6)
        [
            // Owner block as in the mock: name (caps) over its total; the ship-level Extend / Retract on the right.
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()
                  [SNew(STextBlock).Font(Label()).ColorAndOpacity(bOther ? Ink2() : Ink()).Text(bOther ? FText::Format(LOCTEXT("OtherOwner", "{0} · {1}"), Caps(Model->GroupName(Group)), Caps(First->OwnerLabel)) : Caps(Model->GroupName(Group))).OverflowPolicy(ETextOverflowPolicy::Ellipsis)]
                + SVerticalBox::Slot().AutoHeight()[Txt(TAttribute<FText>::CreateLambda([M, Group] {
                  int32 Used = 0, Total = 0;
                  for (const FSCCargoGridView& G : M->GetGrids()) if (G.GroupKey == Group) { Used += G.UsedSCU; Total += G.CapacitySCU; }
                  return SCMobiGlasText::Scu(Used, Total);
              }), Numeric(), Ink2())]
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0, 0, 0)
              [AccessButton(M, [M, Group, bOther] { return bOther ? TArray<int32>() : M->Resolve(FSCGridPick{Group}); })]
        ];
        for (const FSCCargoGridView& Grid : Model->GetGrids())
        {
            if (Grid.GroupKey != Group) continue;
            const int32 Id = Grid.GridId;
            auto Sel = [M, Id] { return M->GetSelectedGrid() == Id; };
            auto Live = [M, Id]() -> const FSCCargoGridView* { return M->FindGrid(Id); };
            List->AddSlot().AutoHeight().Padding(0, 0, 0, 2)
            [
                SNew(SButton).ButtonStyle(&RowButton()).ContentPadding(0)
                .OnClicked_Lambda([M, Id] { M->SelectGrid(Id); return FReply::Handled(); })
                [
                    SNew(SBorder).BorderImage(White()).Padding(FMargin(10, 8))
                    .BorderBackgroundColor_Lambda([Sel] { return Sel() ? Ink() : FLinearColor::Transparent; })
                    [
                        SNew(SVerticalBox)
                        + SVerticalBox::Slot().AutoHeight()
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().FillWidth(1)
                            [SNew(STextBlock).Font(Body()).Text(Model->GridName(Grid)).ToolTipText(Grid.GridLabel).OverflowPolicy(ETextOverflowPolicy::Ellipsis).ColorAndOpacity_Lambda([Sel, bOther] { return FSlateColor(Sel() ? Paper() : bOther ? Ink2() : Ink()); })]
                            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
                            [Chip(TAttribute<FText>::CreateLambda([Live] { const FSCCargoGridView* G = Live(); return G ? SCMobiGlasText::Access(*G) : FText(); }),
                                  TAttribute<FSlateColor>::CreateLambda([Live] {
                                      const FSCCargoGridView* G = Live();
                                      return FSlateColor(G ? SCMobiGlasText::AccessColour(*G) : PaperDim());
                                  }))]
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                            [
                                SNew(SBox).HeightOverride(BarHeight)
                                [SNew(SSCCargoBar)
                                     .Value_Lambda([Live] { const FSCCargoGridView* G = Live(); return G && G->CapacitySCU ? float(G->UsedSCU) / G->CapacitySCU : 0.f; })
                                     .Extra_Lambda([Live] { const FSCCargoGridView* G = Live(); return G && G->CapacitySCU ? float(G->ReservedSCU) / G->CapacitySCU : 0.f; })
                                     .FillColour_Lambda([Sel, Live, bOther] {
                                         const FSCCargoGridView* G = Live();
                                         const bool bFull = G && G->CapacitySCU && G->UsedSCU >= G->CapacitySCU * 0.9f;
                                         return bFull && !bOther ? Warn() : Sel() ? Paper() : bOther ? Rule() : Ink();
                                     })
                                     .TrackColour_Lambda([Sel] { return Sel() ? Ink2() : PaperDim(); })]
                            ]
                            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0, 0, 0)
                            [SNew(STextBlock).Font(Numeric())
                                 .ColorAndOpacity_Lambda([Sel] { return FSlateColor(Sel() ? Paper() : Ink()); })
                                 .Text_Lambda([Live] { const FSCCargoGridView* G = Live(); return G ? SCMobiGlasText::Scu(G->UsedSCU, G->CapacitySCU) : FText(); })]
                            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0, 0, 0)
                            [SNew(SBox).MinDesiredWidth(64).HAlign(HAlign_Right)
                                 [SNew(STextBlock).Font(Numeric()).ColorAndOpacity_Lambda([Sel] { return FSlateColor(Sel() ? PaperDim() : Ink2()); })
                                      .Text_Lambda([Live] { const FSCCargoGridView* G = Live(); return G ? SCMobiGlasText::Metres(G->DistanceM) : FText(); })]]
                        ]
                    ]
                ]
            ];
        }
    }
}

// ---- Grid detail ----------------------------------------------------------------------------------------------------
void SSCMobiGlasDetail::Construct(const FArguments& Args)
{
    Model = Args._Model;
    // Head (name, facts, state, layer), the lattice filling the rest of the panel, the contents table under it.
    ChildSlot
    [
        Panel(LOCTEXT("Detail", "Grid detail"), FText(),
              SNew(SVerticalBox)
              + SVerticalBox::Slot().AutoHeight()[SAssignNew(List, SVerticalBox)]
              + SVerticalBox::Slot().FillHeight(1).Padding(0, 8)
              [SNew(SBox).MinDesiredHeight(LatticeHeight)
                   .Visibility_Lambda([this] { return Model->FindGrid(Model->GetSelectedGrid()) ? EVisibility::Visible : EVisibility::Collapsed; })
                   [SNew(SSCCargoLattice).Model(Model)]]
              + SVerticalBox::Slot().AutoHeight()[SNew(SBox).MaxDesiredHeight(ContentsMaxHeight)[SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(Extra, SVerticalBox)]]])
    ];
}
void SSCMobiGlasDetail::Tick(const FGeometry& Geometry, double Time, float Delta)
{
    SCompoundWidget::Tick(Geometry, Time, Delta);
    // Rebuild on structure change or another grid selected (the layer count and contents differ).
    const int32 Want = Model->GetStamp() * 7919 + Model->GetSelectedGrid();
    if (BuiltStamp != Want) { Rebuild(); BuiltStamp = Want; }
}
void SSCMobiGlasDetail::Rebuild()
{
    List->ClearChildren();
    Extra->ClearChildren();
    const FSCCargoGridView* Grid = Model->FindGrid(Model->GetSelectedGrid());
    if (!Grid) { List->AddSlot().AutoHeight()[Empty(LOCTEXT("NoSelection", "Select a grid to see its layout."))]; return; }
    TSharedPtr<FSCMobiGlasModel> M = Model;
    FNumberFormattingOptions OneDecimal;
    OneDecimal.SetMinimumFractionalDigits(1).SetMaximumFractionalDigits(2);
    auto Num = [&](double V) { return FText::AsNumber(V, &OneDecimal); };
    const FText Facts = FText::Format(LOCTEXT("Facts", "{0} × {1} × {2} cells · {3} m · max {4} × {5} × {6} m"), Grid->Cells.X, Grid->Cells.Y,
                                      Grid->Cells.Z, Num(Grid->CellSizeM.X), Num(Grid->MaxItemSizeM.X), Num(Grid->MaxItemSizeM.Y), Num(Grid->MaxItemSizeM.Z));
    List->AddSlot().AutoHeight()[Txt(Caps(Model->ShortGridName(*Grid)), Title(), Ink())];
    List->AddSlot().AutoHeight().Padding(0, 4, 0, 12)[SNew(STextBlock).Font(Label()).ColorAndOpacity(Ink2()).AutoWrapText(true).Text(Caps(Facts))];
    // Layer selector (inverted bar = selected).
    TSharedRef<SHorizontalBox> Layers = SNew(SHorizontalBox);
    Layers->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)[Txt(LOCTEXT("Layer", "LAYER"), Label(), Ink2())];
    for (int32 Z = 0; Z < FMath::Max(1, Grid->Cells.Z); ++Z)
    {
        auto Sel = [M, Z] { return M->GetLayer() == Z; };
        Layers->AddSlot().AutoWidth().Padding(0, 0, 4, 0)
        [
            SNew(SButton).ButtonStyle(&FlatButton()).ContentPadding(0).OnClicked_Lambda([M, Z] { M->SetLayer(Z); return FReply::Handled(); })
            [
                SNew(SBorder).BorderImage(White()).Padding(FMargin(10, 3))
                .BorderBackgroundColor_Lambda([Sel] { return Sel() ? Ink() : PaperDim(); })
                [SNew(STextBlock).Font(Numeric()).Text(FText::AsNumber(Z + 1)).ColorAndOpacity_Lambda([Sel] { return FSlateColor(Sel() ? Paper() : Ink()); })]
            ]
        ];
    }
    const int32 Id = Grid->GridId;
    // State: extended / retracted (cargo only rides on an extended grid) and the control to change it.
    List->AddSlot().AutoHeight().Padding(0, 0, 0, 10)
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)[Txt(LOCTEXT("State", "STATE"), Label(), Ink2())]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
          [Chip(TAttribute<FText>::CreateLambda([M, Id] { const FSCCargoGridView* G = M->FindGrid(Id); return G ? SCMobiGlasText::Access(*G) : FText(); }),
                TAttribute<FSlateColor>::CreateLambda([M, Id] { const FSCCargoGridView* G = M->FindGrid(Id); return FSlateColor(G ? SCMobiGlasText::AccessColour(*G) : PaperDim()); }))]
        + SHorizontalBox::Slot().FillWidth(1)[SNew(SSpacer)]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[AccessButton(M, [Id] { return TArray<int32>{Id}; })]
    ];
    List->AddSlot().AutoHeight().Padding(0, 0, 0, 10)
    [
        SNew(STextBlock).Font(Body()).AutoWrapText(true)
        .ColorAndOpacity_Lambda([M] { return FSlateColor(M->GetAccessError().IsEmpty() ? Ink2() : Alert()); })
        .Visibility_Lambda([M, Id] {
            const FSCMobiGlasModel::FAccessControl C = M->GetAccessControl({Id});
            return !M->GetAccessError().IsEmpty() || (C.bShown && !C.bEnabled && !C.Reason.IsEmpty()) ? EVisibility::Visible : EVisibility::Collapsed;
        })
        .Text_Lambda([M, Id] { return M->GetAccessError().IsEmpty() ? M->GetAccessControl({Id}).Reason : M->GetAccessError(); })
    ];
    List->AddSlot().AutoHeight()[Layers];
    // Contents: one row per commodity (and owner); its box sizes listed in the BOXES column ("2 × 8 · 2 × 4").
    struct FRow { FName Commodity; FText Label; int32 SCU = 0, Count = 0, Active = 0; bool bMine = true; FText Owner; TMap<int32, int32> Sizes; };
    TArray<FRow> Rows;
    for (const FSCCargoBoxView& B : Grid->Boxes)
    {
        FRow* Row = Rows.FindByPredicate([&B](const FRow& R) { return R.Commodity == B.Commodity && R.bMine == B.bMine && R.Owner.EqualTo(B.OwnerLabel); });
        if (!Row) Row = &Rows.Add_GetRef(FRow{B.Commodity, B.CommodityLabel, 0, 0, 0, B.bMine, B.OwnerLabel});
        ++Row->Count;
        Row->SCU += B.SCU;
        ++Row->Sizes.FindOrAdd(B.SCU);
        Row->Active += B.State != ESCCargoBoxState::Stored;
    }
    Rows.Sort([](const FRow& A, const FRow& B) { return A.SCU > B.SCU; });
    Extra->AddSlot().AutoHeight()
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1)[Txt(LOCTEXT("Commodity", "COMMODITY"), Label(), Ink2())]
        + SHorizontalBox::Slot().AutoWidth()[SNew(SBox).MinDesiredWidth(80)[Txt(LOCTEXT("Boxes", "BOXES"), Label(), Ink2())]]
        + SHorizontalBox::Slot().AutoWidth()[SNew(SBox).MinDesiredWidth(56).HAlign(HAlign_Right)[Txt(LOCTEXT("ScuHead", "SCU"), Label(), Ink2())]]
    ];
    Extra->AddSlot().AutoHeight().Padding(0, 4, 0, 4)[HRule()];
    if (Rows.IsEmpty()) Extra->AddSlot().AutoHeight()[Empty(LOCTEXT("EmptyGrid", "Empty."))];
    for (const FRow& R : Rows)
    {
        const FLinearColor Swatch = R.bMine ? Commodity(Model->CommodityColour(R.Commodity)) : PaperDim();
        const FText RowLabel = R.bMine || R.Owner.IsEmpty() ? R.Label : FText::Format(LOCTEXT("OwnedBy", "{0} · {1}"), R.Label, R.Owner);
        TSharedRef<SHorizontalBox> Line = SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
              [SNew(SBox).WidthOverride(12).HeightOverride(12)[SNew(SBorder).BorderImage(White()).BorderBackgroundColor(Swatch)]]
            + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(0, 0, 8, 0)
              [SNew(STextBlock).Font(Body()).ColorAndOpacity(R.bMine ? Ink() : Ink2()).Text(RowLabel).OverflowPolicy(ETextOverflowPolicy::Ellipsis)];
        if (R.Active) Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0)[Chip(LOCTEXT("InJob", "IN TRANSFER"), FSlateColor(Accent()))];
        TArray<int32> Sizes;
        R.Sizes.GetKeys(Sizes);
        Sizes.Sort(TGreater<int32>());
        FString Boxes;
        for (int32 S : Sizes) Boxes += FString::Printf(TEXT("%s%d × %d"), Boxes.IsEmpty() ? TEXT("") : TEXT(" · "), R.Sizes[S], S);
        Line->AddSlot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).MinDesiredWidth(80)[Txt(FText::FromString(Boxes), Numeric(), Ink2())]];
        Line->AddSlot().AutoWidth().VAlign(VAlign_Center)
            [SNew(SBox).MinDesiredWidth(56).HAlign(HAlign_Right)[Txt(FText::AsNumber(R.SCU), Numeric(), Ink())]];
        Extra->AddSlot().AutoHeight().Padding(0, 3)[Line];
    }
    Extra->AddSlot().AutoHeight().Padding(0, 4, 0, 4)[HRule()];
    Extra->AddSlot().AutoHeight()
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1)[Txt(LOCTEXT("Total", "TOTAL"), Label(), Ink2())]
        + SHorizontalBox::Slot().AutoWidth()
          [Txt(TAttribute<FText>::CreateLambda([M, Id] {
               const FSCCargoGridView* G = M->FindGrid(Id);
               if (!G) return FText();
               return G->ReservedSCU ? FText::Format(LOCTEXT("TotalIncoming", "{0}  +{1} incoming"), SCMobiGlasText::Scu(G->UsedSCU, G->CapacitySCU), G->ReservedSCU)
                                     : SCMobiGlasText::Scu(G->UsedSCU, G->CapacitySCU);
           }), Numeric(), Ink())]
    ];
}

#undef LOCTEXT_NAMESPACE
