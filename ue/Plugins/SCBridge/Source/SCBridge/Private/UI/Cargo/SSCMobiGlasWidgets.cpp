// SSCCargoBar, SSCCargoLattice, SSCMobiGlasPicker and the shared paint helpers.
#include "SSCMobiGlasWidgets.h"
#include "SCMobiGlasModel.h"
#include "SCMobiGlasStyle.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

using namespace SCMobiGlasStyle;

namespace SCMobiGlasPaint
{
void Box(FSlateWindowElementList& Out, int32 Layer, const FGeometry& G, FVector2f Pos, FVector2f Size, const FLinearColor& Colour)
{
    FSlateDrawElement::MakeBox(Out, Layer, G.ToPaintGeometry(Size, FSlateLayoutTransform(Pos)), White(), ESlateDrawEffect::None, Colour);
}
void Frame(FSlateWindowElementList& Out, int32 Layer, const FGeometry& G, FVector2f Pos, FVector2f Size, const FLinearColor& Colour, float T)
{
    Box(Out, Layer, G, Pos, FVector2f(Size.X, T), Colour);
    Box(Out, Layer, G, FVector2f(Pos.X, Pos.Y + Size.Y - T), FVector2f(Size.X, T), Colour);
    Box(Out, Layer, G, Pos, FVector2f(T, Size.Y), Colour);
    Box(Out, Layer, G, FVector2f(Pos.X + Size.X - T, Pos.Y), FVector2f(T, Size.Y), Colour);
}
void Lines(FSlateWindowElementList& Out, int32 Layer, const FGeometry& G, TArray<FVector2f> Points, const FLinearColor& Colour, float Thickness)
{
    FSlateDrawElement::MakeLines(Out, Layer, G.ToPaintGeometry(), MoveTemp(Points), ESlateDrawEffect::None, Colour, true, Thickness);
}
void Text(FSlateWindowElementList& Out, int32 Layer, const FGeometry& G, FVector2f Pos, const FString& String, const FSlateFontInfo& Font, const FLinearColor& Colour)
{
    FSlateDrawElement::MakeText(Out, Layer, G.ToPaintGeometry(Measure(String, Font), FSlateLayoutTransform(Pos)), String, Font, ESlateDrawEffect::None, Colour);
}
FVector2f Measure(const FString& String, const FSlateFontInfo& Font)
{
    if (!FSlateApplication::IsInitialized()) return FVector2f::ZeroVector;
    return FVector2f(FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(String, Font));
}
void Hatch(FSlateWindowElementList& Out, int32 Layer, const FGeometry& G, FVector2f Pos, FVector2f Size, float Step, const FLinearColor& Colour)
{
    // 45° lines clipped to the rect: from the top edge / right edge down-left to the left edge / bottom edge.
    for (float K = Step; K < Size.X + Size.Y; K += Step)
    {
        const FVector2f A(Pos.X + FMath::Min(K, Size.X), Pos.Y + FMath::Max(0.f, K - Size.X));
        const FVector2f B(Pos.X + FMath::Max(0.f, K - Size.Y), Pos.Y + FMath::Min(K, Size.Y));
        Lines(Out, Layer, G, {A, B}, Colour, 1);
    }
}
} // namespace SCMobiGlasPaint

// ---- SSCCargoBar ---------------------------------------------------------------------------------------------------
void SSCCargoBar::Construct(const FArguments& Args)
{
    Value = Args._Value;
    Extra = Args._Extra;
    FillColour = Args._FillColour.IsSet() ? Args._FillColour : TAttribute<FLinearColor>(Ink());
    ExtraColour = Args._ExtraColour.IsSet() ? Args._ExtraColour : TAttribute<FLinearColor>(Accent());
    TrackColour = Args._TrackColour.IsSet() ? Args._TrackColour : TAttribute<FLinearColor>(PaperDim());
}
void SSCCargoBar::Tick(const FGeometry& Geometry, double Time, float Delta)
{
    const float Target = FMath::Clamp(Value.Get(), 0.f, 1.f), TargetExtra = FMath::Clamp(Extra.Get(), 0.f, 1.f);
    const float K = 1 - FMath::Exp(-Delta / BarTau);
    Shown = Shown < 0 ? Target : FMath::Lerp(Shown, Target, K);
    ShownExtra = FMath::Lerp(ShownExtra, TargetExtra, K);
}
int32 SSCCargoBar::OnPaint(const FPaintArgs&, const FGeometry& G, const FSlateRect&, FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle&, bool) const
{
    const FVector2f Size(G.GetLocalSize());
    const float V = FMath::Clamp(Shown < 0 ? Value.Get() : Shown, 0.f, 1.f), E = FMath::Clamp(ShownExtra, 0.f, 1.f - V);
    SCMobiGlasPaint::Box(Out, Layer, G, FVector2f(0, 0), Size, TrackColour.Get());
    if (V > 0) SCMobiGlasPaint::Box(Out, Layer + 1, G, FVector2f(0, 0), FVector2f(Size.X * V, Size.Y), FillColour.Get());
    if (E > 0) SCMobiGlasPaint::Box(Out, Layer + 1, G, FVector2f(Size.X * V, 0), FVector2f(Size.X * E, Size.Y), ExtraColour.Get());
    for (int32 T = 1; T < 4; ++T) // a tick every 25 %
        SCMobiGlasPaint::Box(Out, Layer + 2, G, FVector2f(FMath::RoundToFloat(Size.X * T / 4), 0), FVector2f(1, Size.Y), Paper().CopyWithNewOpacity(0.7f));
    return Layer + 2;
}
FVector2D SSCCargoBar::ComputeDesiredSize(float) const { return FVector2D(24, BarHeight); }

// ---- SSCCargoLattice -------------------------------------------------------------------------------------------------
FVector2D SSCCargoLattice::ComputeDesiredSize(float) const { return FVector2D(120, LatticeHeight); }

int32 SSCCargoLattice::OnPaint(const FPaintArgs&, const FGeometry& G, const FSlateRect&, FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle&, bool) const
{
    const FSCCargoGridView* Grid = Model ? Model->FindGrid(Model->GetSelectedGrid()) : nullptr;
    if (!Grid || Grid->Cells.X <= 0 || Grid->Cells.Y <= 0) return Layer;
    const int32 Z = FMath::Clamp(Model->GetLayer(), 0, FMath::Max(0, Grid->Cells.Z - 1));
    // The longer lattice axis runs across the panel.
    const bool bSwap = Grid->Cells.Y > Grid->Cells.X;
    const int32 Cols = bSwap ? Grid->Cells.Y : Grid->Cells.X, Rows = bSwap ? Grid->Cells.X : Grid->Cells.Y;
    const FVector2f Avail(G.GetLocalSize());
    const float Cell = FMath::FloorToFloat(FMath::Min3(LatticeMaxCell, (Avail.X - 1) / Cols, (Avail.Y - 1) / Rows));
    const FVector2f Origin(FMath::RoundToFloat((Avail.X - Cell * Cols) / 2), FMath::RoundToFloat((Avail.Y - Cell * Rows) / 2));
    auto CellPos = [&](int32 X, int32 Y) { return Origin + FVector2f(bSwap ? Y : X, bSwap ? X : Y) * Cell; };
    auto Span = [&](const FIntVector& C) { return FVector2f(bSwap ? C.Y : C.X, bSwap ? C.X : C.Y) * Cell; };
    // Cells.
    SCMobiGlasPaint::Box(Out, Layer, G, Origin, FVector2f(Cols, Rows) * Cell, PaperDim());
    for (const FIntVector& B : Grid->BlockedCells)
        if (B.Z == Z) SCMobiGlasPaint::Hatch(Out, Layer + 1, G, CellPos(B.X, B.Y), FVector2f(Cell), HatchStep, Ink2());
    for (int32 C = 0; C <= Cols; ++C) SCMobiGlasPaint::Lines(Out, Layer + 2, G, {Origin + FVector2f(C * Cell, 0), Origin + FVector2f(C * Cell, Rows * Cell)}, Rule());
    for (int32 R = 0; R <= Rows; ++R) SCMobiGlasPaint::Lines(Out, Layer + 2, G, {Origin + FVector2f(0, R * Cell), Origin + FVector2f(Cols * Cell, R * Cell)}, Rule());
    // Boxes that occupy this layer.
    for (const FSCCargoBoxView& Box : Grid->Boxes)
    {
        if (Z < Box.Origin.Z || Z >= Box.Origin.Z + Box.Cells.Z) continue;
        const FVector2f Pos = CellPos(Box.Origin.X, Box.Origin.Y) + FVector2f(2, 2), Size = Span(Box.Cells) - FVector2f(4, 4);
        // Other owners' boxes on a shared grid: paper-dim and hatched, not in a commodity colour.
        const FLinearColor Fill = Box.bMine ? Commodity(Model->CommodityColour(Box.Commodity)) : PaperDim();
        SCMobiGlasPaint::Box(Out, Layer + 3, G, Pos, Size, Fill);
        if (!Box.bMine) SCMobiGlasPaint::Hatch(Out, Layer + 3, G, Pos, Size, HatchStep, Rule());
        const bool bJob = Box.State != ESCCargoBoxState::Stored;
        SCMobiGlasPaint::Frame(Out, Layer + 4, G, Pos, Size, bJob ? Accent() : Ink(), bJob ? FocusWidth : 1);
        const FString Label = FString::Printf(TEXT("%d"), Box.SCU);
        const FVector2f TextSize = SCMobiGlasPaint::Measure(Label, SCMobiGlasStyle::Label());
        if (TextSize.X < Size.X - 4 && TextSize.Y < Size.Y)
            SCMobiGlasPaint::Text(Out, Layer + 5, G, Pos + (Size - TextSize) / 2, Label, SCMobiGlasStyle::Label(), Box.bMine ? TextOn(Fill) : Ink2());
    }
    return Layer + 5;
}

// ---- SSCMobiGlasPicker -----------------------------------------------------------------------------------------------
namespace
{
/** Down chevron drawn with two 1.5 px lines (no glyph dependency). */
class SChevron : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SChevron) {}
    SLATE_END_ARGS()
    void Construct(const FArguments&) {}
    virtual int32 OnPaint(const FPaintArgs&, const FGeometry& G, const FSlateRect&, FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle& Style,
                          bool) const override
    {
        SCMobiGlasPaint::Lines(Out, Layer, G, {FVector2f(1, 3), FVector2f(6, 8), FVector2f(11, 3)}, Ink(), 1.5f);
        return Layer;
    }
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(12, 10); }
};
} // namespace

void SSCMobiGlasPicker::Construct(const FArguments& Args)
{
    Options = Args._Options;
    ChildSlot
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()
        [
            SAssignNew(Anchor, SMenuAnchor)
            .Method(EPopupMethod::UseCurrentWindow)
            .Placement(MenuPlacement_ComboBox)
            .OnGetMenuContent(FOnGetContent::CreateSP(this, &SSCMobiGlasPicker::MakeMenu))
            [
                SNew(SBorder).BorderImage(White()).BorderBackgroundColor(Rule()).Padding(1)
                [
                    SNew(SButton).ButtonStyle(&FlatButton()).ContentPadding(FMargin(10, 6))
                    .OnClicked_Lambda([this] { Anchor->SetIsOpen(!Anchor->IsOpen()); return FReply::Handled(); })
                    [
                        SNew(SHorizontalBox)
                        // The caption sits in the field, as in the mock ("FROM   Freight elevator").
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                        [SNew(SBox).MinDesiredWidth(104)[Txt(Caps(Args._Caption), Label(), Ink2())]]
                        + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                        [SNew(STextBlock).Font(Body()).ColorAndOpacity(Ink()).Text(Args._Value).OverflowPolicy(ETextOverflowPolicy::Ellipsis)]
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)[SNew(SChevron)]
                    ]
                ]
            ]
        ]
    ];
}

TSharedRef<SWidget> SSCMobiGlasPicker::MakeMenu()
{
    TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
    const TArray<FOption> Items = Options.IsBound() ? Options.Execute() : TArray<FOption>();
    for (const FOption& Item : Items)
    {
        const TFunction<void()> Pick = Item.Pick;
        const bool bSelected = Item.bSelected;
        List->AddSlot().AutoHeight()
        [
            SNew(SButton).ButtonStyle(&RowButton()).ContentPadding(0)
            .OnClicked_Lambda([this, Pick] { if (Pick) Pick(); Anchor->SetIsOpen(false); return FReply::Handled(); })
            [
                SNew(SBorder).BorderImage(White()).BorderBackgroundColor(bSelected ? Ink() : FLinearColor::Transparent).Padding(FMargin(10, 6))
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().FillWidth(1)[Txt(Item.Label, Body(), bSelected ? Paper() : Ink())]
                    + SHorizontalBox::Slot().AutoWidth().Padding(12, 0, 0, 0)[Txt(Item.Note, Numeric(), bSelected ? PaperDim() : Ink2())]
                ]
            ]
        ];
    }
    if (Items.IsEmpty()) List->AddSlot().AutoHeight().Padding(10, 6)[Txt(NSLOCTEXT("SCMobiGlas", "NoOptions", "Nothing to choose"), Body(), Ink2())];
    return SNew(SBorder).BorderImage(White()).BorderBackgroundColor(Rule()).Padding(1)
        [SNew(SBorder).BorderImage(White()).BorderBackgroundColor(Paper()).Padding(0)[SNew(SBox).MaxDesiredHeight(360).MinDesiredWidth(FMath::Max(120.f, float(Anchor->GetCachedGeometry().GetLocalSize().X) - 2))[SNew(SScrollBox) + SScrollBox::Slot()[List]]]];
}
