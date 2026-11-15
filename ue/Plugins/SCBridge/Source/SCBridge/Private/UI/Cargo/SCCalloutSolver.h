// Pure placement of world-space callout labels (WP-K1c, research-starfield-ui.md §7 "World-space callouts"): given
// screen anchors, the objects' projected screen rects, label sizes, priorities and the sides used last time, choose a
// label rect and a leader polyline (0°/45°/90° segments) for at most MaxLabels targets. No Slate, no world: tested by
// SCBridge.UI.Callouts.*.
#pragma once
#include "CoreMinimal.h"
#include "Math/Box2D.h"

enum class ESCCalloutSide : uint8 { None, NE, NW, SE, SW };

struct FSCCalloutTarget
{
    int32 Key = 0;                         // stable identity (hysteresis, animation)
    FVector2D Anchor = FVector2D::ZeroVector;
    FBox2D Object = FBox2D(ForceInit);     // projected bounds; the label never covers it (+ Margin)
    FVector2D LabelSize = FVector2D::ZeroVector;
    int32 Priority = 0;                    // higher first
    double Depth = 0;                      // tie-break: nearer first
    ESCCalloutSide PreviousSide = ESCCalloutSide::None; // kept unless it collides
};

struct FSCCalloutPlacement
{
    int32 Key = 0;
    bool bLabel = false; // false: dot only (over the label budget or no free slot)
    ESCCalloutSide Side = ESCCalloutSide::None;
    FBox2D Label = FBox2D(ForceInit);
    /** Anchor, [vertical riser], elbow (end of the 45° run), shelf end at the label edge. */
    TArray<FVector2D, TInlineAllocator<4>> Line;
};

struct FSCCalloutParams
{
    double Margin = 16;   // label clearance around its own object
    double MinRun = 24;   // 45° run, min .. max
    double MaxRun = 64;
    double MinShelf = 16; // horizontal shelf into the label edge
    double MaxRise = 160; // longest vertical reach from the anchor to the shelf (longer leaders read as stray lines)
    double Gap = 6;       // between stacked labels
    int32 MaxLabels = 8;
};

class FSCCalloutSolver
{
public:
    /** Placements in the order of Targets. Screen: labels stay inside it. */
    static TArray<FSCCalloutPlacement> Solve(TConstArrayView<FSCCalloutTarget> Targets, const FBox2D& Screen,
                                             const FSCCalloutParams& Params = FSCCalloutParams());

    // Geometry predicates (shared with the tests).
    static bool BoxesOverlap(const FBox2D& A, const FBox2D& B, double Gap = 0);
    static bool SegmentsCross(const FVector2D& A, const FVector2D& B, const FVector2D& C, const FVector2D& D);
    static bool SegmentHitsBox(const FVector2D& A, const FVector2D& B, const FBox2D& Box);
    static bool PolylinesCross(TConstArrayView<FVector2D> A, TConstArrayView<FVector2D> B);
    static bool PolylineHitsBox(TConstArrayView<FVector2D> Line, const FBox2D& Box);

private:
    struct FPlaced
    {
        FBox2D Label;
        TArray<FVector2D, TInlineAllocator<4>> Line;
        int32 Target;
    };
    static bool TryPlace(const FSCCalloutTarget& Target, int32 Index, ESCCalloutSide Side, bool bStrict, TConstArrayView<FSCCalloutTarget> All,
                         const TArray<FPlaced>& Placed, const FBox2D& Screen, const FSCCalloutParams& P, FSCCalloutPlacement& Out);
    static void Build(const FSCCalloutTarget& Target, ESCCalloutSide Side, double CentreY, TOptional<double> ClearX, bool bRiserLast, const FSCCalloutParams& P,
                      FSCCalloutPlacement& Out);
};
