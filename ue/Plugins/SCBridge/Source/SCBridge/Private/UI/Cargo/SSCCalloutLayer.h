// World-space callouts over the 3D view (WP-K1c): boxes of active transfers, drones away from their dock and — while
// the mobiGlas is open — the nearby grids (the selected one in accent). Projects each target's world bounds, solves
// label placement with FSCCalloutSolver every frame (the solver keeps each label's previous side unless it collides),
// and animates lines (140 ms) and labels (120 ms fade, 6 px slide) around their solved places. Draw-only: no input.
#pragma once
#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"
#include "SCCalloutSolver.h"

class FSCMobiGlasModel;

class SSCCalloutLayer : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SSCCalloutLayer) {}
    SLATE_ARGUMENT(TSharedPtr<FSCMobiGlasModel>, Model)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args);
    virtual void Tick(const FGeometry& Geometry, double Time, float Delta) override;
    virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& Culling, FSlateWindowElementList& Out, int32 Layer,
                          const FWidgetStyle& Style, bool bParentEnabled) const override;
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }

private:
    enum class EKind : uint8 { Grid, Box, Drone };
    struct FItem
    {
        EKind Kind = EKind::Grid;
        FString Name, Value; // label line (caps) and numeric line
        bool bSelected = false, bSeen = false, bLabel = false;
        bool bNeutral = false; // another owner's box / drone: no accent
        FVector2D Anchor = FVector2D::ZeroVector;
        FBox2D Object = FBox2D(ForceInit);
        FVector2D LabelSize = FVector2D::ZeroVector;
        int32 Priority = 0;
        double Depth = 0;
        ESCCalloutSide Side = ESCCalloutSide::None;
        FVector2D Offset = FVector2D::ZeroVector, TargetOffset = FVector2D::ZeroVector; // label min corner - anchor
        double Born = -1, Died = -1;
    };
    void Gather(const FGeometry& Geometry);
    void Solve(const FGeometry& Geometry);
    /** Anchor -> [riser] -> 45° elbow -> shelf into the label edge, for the label's current position. */
    static TArray<FVector2f> Leader(const FVector2D& Anchor, const FBox2D& Label, ESCCalloutSide Side);

    TSharedPtr<FSCMobiGlasModel> Model;
    TMap<int32, FItem> Items;
    double Now = 0;
    FBox2D Area = FBox2D(ForceInit); // where callouts may appear (the live view while the mobiGlas is open)
};
