// Small mobiGlas widgets (WP-K1c): the 6 px capacity / progress bar, the top-down grid lattice, the paper drop-down
// picker, and the paint helpers they and the callout layer share.
#pragma once
#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SCompoundWidget.h"

class FSCMobiGlasModel;
class SMenuAnchor;
class SVerticalBox;
struct FSlateFontInfo;

namespace SCMobiGlasPaint
{
void Box(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, FVector2f Pos, FVector2f Size, const FLinearColor& Colour);
/** 1 px (or Thickness) outline of a rect. */
void Frame(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, FVector2f Pos, FVector2f Size, const FLinearColor& Colour, float Thickness = 1);
void Lines(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, TArray<FVector2f> Points, const FLinearColor& Colour, float Thickness = 1);
void Text(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, FVector2f Pos, const FString& Text, const FSlateFontInfo& Font, const FLinearColor& Colour);
FVector2f Measure(const FString& Text, const FSlateFontInfo& Font);
/** Diagonal hatching inside a square-cornered rect (blocked cells). */
void Hatch(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry, FVector2f Pos, FVector2f Size, float Step, const FLinearColor& Colour);
} // namespace SCMobiGlasPaint

/** Flat 6 px bar on a track, ticks every 25 %, optional second segment (reserved); tracks its value with τ 120 ms. */
class SSCCargoBar : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SSCCargoBar) : _Value(0.f), _Extra(0.f) {}
    SLATE_ATTRIBUTE(float, Value)
    SLATE_ATTRIBUTE(float, Extra)
    SLATE_ATTRIBUTE(FLinearColor, FillColour)
    SLATE_ATTRIBUTE(FLinearColor, ExtraColour)
    SLATE_ATTRIBUTE(FLinearColor, TrackColour)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    virtual void Tick(const FGeometry& Geometry, double Time, float Delta) override;
    virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& Culling, FSlateWindowElementList& Out, int32 Layer,
                          const FWidgetStyle& Style, bool bParentEnabled) const override;
    virtual FVector2D ComputeDesiredSize(float) const override;

private:
    TAttribute<float> Value, Extra;
    TAttribute<FLinearColor> FillColour, ExtraColour, TrackColour;
    float Shown = -1, ShownExtra = 0;
};

/** Top-down lattice of the selected grid at the selected layer: cells, boxes coloured by commodity and labelled with
 *  SCU, blocked cells (drone dock) hatched, boxes of active jobs outlined in accent. */
class SSCCargoLattice : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SSCCargoLattice) {}
    SLATE_ARGUMENT(TSharedPtr<FSCMobiGlasModel>, Model)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args) { Model = Args._Model; }
    virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& Culling, FSlateWindowElementList& Out, int32 Layer,
                          const FWidgetStyle& Style, bool bParentEnabled) const override;
    virtual FVector2D ComputeDesiredSize(float) const override;

private:
    TSharedPtr<FSCMobiGlasModel> Model;
};

/** A paper drop-down: a 1 px framed field with its caps caption and the current value; the menu lists Options. */
class SSCMobiGlasPicker : public SCompoundWidget
{
public:
    struct FOption
    {
        FText Label;
        FText Note; // right-aligned (e.g. "64 SCU")
        bool bSelected = false;
        TFunction<void()> Pick;
    };
    DECLARE_DELEGATE_RetVal(TArray<FOption>, FGetOptions);
    SLATE_BEGIN_ARGS(SSCMobiGlasPicker) {}
    SLATE_ARGUMENT(FText, Caption)
    SLATE_ATTRIBUTE(FText, Value)
    SLATE_EVENT(FGetOptions, Options)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);

private:
    TSharedRef<SWidget> MakeMenu();
    FGetOptions Options;
    TSharedPtr<SMenuAnchor> Anchor;
};
