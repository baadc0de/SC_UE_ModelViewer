// FSCCalloutSolver: greedy, priority-ordered label placement. Each label tries its previous side, then NE, NW, SE, SW;
// per side four shapes (label clear of its object vertically, or beside it with a longer shelf; a long rise's vertical
// segment at the anchor or after the 45° run) are pushed outward past conflicts, which stacks labels of one side in a
// column with Gap spacing ordered by anchor height. Every rule is hard: inside the screen, no label overlap, no object
// covered, no line crossing a line or a label. Labels that fit nowhere, and those over MaxLabels, become dots.
#include "SCCalloutSolver.h"

namespace
{
constexpr double Eps = 1e-6;
bool IsEast(ESCCalloutSide S) { return S == ESCCalloutSide::NE || S == ESCCalloutSide::SE; }
bool IsNorth(ESCCalloutSide S) { return S == ESCCalloutSide::NE || S == ESCCalloutSide::NW; }
double Cross(const FVector2D& O, const FVector2D& A, const FVector2D& B) { return (A.X - O.X) * (B.Y - O.Y) - (A.Y - O.Y) * (B.X - O.X); }
double LineLength(TConstArrayView<FVector2D> Line)
{
    double L = 0;
    for (int32 I = 1; I < Line.Num(); ++I) L += FVector2D::Distance(Line[I - 1], Line[I]);
    return L;
}
bool Inside(const FBox2D& Inner, const FBox2D& Outer)
{
    return Inner.Min.X >= Outer.Min.X - Eps && Inner.Min.Y >= Outer.Min.Y - Eps && Inner.Max.X <= Outer.Max.X + Eps && Inner.Max.Y <= Outer.Max.Y + Eps;
}
} // namespace

bool FSCCalloutSolver::BoxesOverlap(const FBox2D& A, const FBox2D& B, double Gap)
{
    return A.Min.X < B.Max.X + Gap - Eps && B.Min.X < A.Max.X + Gap - Eps && A.Min.Y < B.Max.Y + Gap - Eps && B.Min.Y < A.Max.Y + Gap - Eps;
}

bool FSCCalloutSolver::SegmentsCross(const FVector2D& A, const FVector2D& B, const FVector2D& C, const FVector2D& D)
{
    const double D1 = Cross(C, D, A), D2 = Cross(C, D, B), D3 = Cross(A, B, C), D4 = Cross(A, B, D);
    const double Tol = 1e-4;
    if (((D1 > Tol && D2 < -Tol) || (D1 < -Tol && D2 > Tol)) && ((D3 > Tol && D4 < -Tol) || (D3 < -Tol && D4 > Tol))) return true;
    // Collinear overlap (two shelves on one height) counts as a crossing; touching end points do not.
    if (FMath::Abs(D1) <= Tol && FMath::Abs(D2) <= Tol)
    {
        const FVector2D Dir = B - A;
        const double Len2 = Dir.SizeSquared();
        if (Len2 < Eps) return false;
        const double T0 = FVector2D::DotProduct(C - A, Dir) / Len2, T1 = FVector2D::DotProduct(D - A, Dir) / Len2;
        const double Lo = FMath::Max(0.0, FMath::Min(T0, T1)), Hi = FMath::Min(1.0, FMath::Max(T0, T1));
        return Hi - Lo > 1e-3;
    }
    return false;
}

bool FSCCalloutSolver::SegmentHitsBox(const FVector2D& A, const FVector2D& B, const FBox2D& Box)
{
    // Liang-Barsky clip against the open box (touching an edge is not a hit).
    double T0 = 0, T1 = 1;
    const FVector2D D = B - A;
    const double P[4] = {-D.X, D.X, -D.Y, D.Y};
    const double Q[4] = {A.X - Box.Min.X, Box.Max.X - A.X, A.Y - Box.Min.Y, Box.Max.Y - A.Y};
    for (int32 I = 0; I < 4; ++I)
    {
        if (FMath::Abs(P[I]) < Eps)
        {
            if (Q[I] <= Eps) return false;
            continue;
        }
        const double R = Q[I] / P[I];
        if (P[I] < 0) T0 = FMath::Max(T0, R);
        else T1 = FMath::Min(T1, R);
        if (T0 > T1 - Eps) return false;
    }
    return true;
}

bool FSCCalloutSolver::PolylinesCross(TConstArrayView<FVector2D> A, TConstArrayView<FVector2D> B)
{
    for (int32 I = 1; I < A.Num(); ++I)
        for (int32 J = 1; J < B.Num(); ++J)
            if (SegmentsCross(A[I - 1], A[I], B[J - 1], B[J])) return true;
    return false;
}

bool FSCCalloutSolver::PolylineHitsBox(TConstArrayView<FVector2D> Line, const FBox2D& Box)
{
    for (int32 I = 1; I < Line.Num(); ++I)
        if (SegmentHitsBox(Line[I - 1], Line[I], Box)) return true;
    return false;
}

void FSCCalloutSolver::Build(const FSCCalloutTarget& T, ESCCalloutSide Side, double CentreY, TOptional<double> ClearX, bool bRiserLast, const FSCCalloutParams& P,
                             FSCCalloutPlacement& Out)
{
    const double Sx = IsEast(Side) ? 1 : -1, Sy = IsNorth(Side) ? -1 : 1;
    const double Rise = FMath::Max(P.MinRun, FMath::Abs(CentreY - T.Anchor.Y));
    const double Run = FMath::Min(Rise, P.MaxRun), Riser = Rise - Run;
    const double Y = T.Anchor.Y + Sy * Rise;
    const FVector2D Elbow(T.Anchor.X + Sx * Run, Y);
    double Shelf = P.MinShelf;
    if (ClearX) Shelf = FMath::Max(Shelf, Sx * (*ClearX - Elbow.X));
    const FVector2D Edge(Elbow.X + Sx * Shelf, Y);
    const double W = T.LabelSize.X, H = T.LabelSize.Y;
    Out.Side = Side;
    Out.Label = Sx > 0 ? FBox2D(FVector2D(Edge.X, Y - H / 2), FVector2D(Edge.X + W, Y + H / 2))
                       : FBox2D(FVector2D(Edge.X - W, Y - H / 2), FVector2D(Edge.X, Y + H / 2));
    Out.Line.Reset();
    Out.Line.Add(T.Anchor);
    // A rise beyond MaxRun is a vertical segment at the anchor, or after the 45° run (whichever avoids the others).
    if (Riser > Eps) Out.Line.Add(bRiserLast ? FVector2D(T.Anchor.X + Sx * Run, T.Anchor.Y + Sy * Run) : FVector2D(T.Anchor.X, T.Anchor.Y + Sy * Riser));
    Out.Line.Add(Elbow);
    Out.Line.Add(Edge);
}

bool FSCCalloutSolver::TryPlace(const FSCCalloutTarget& T, int32 Index, ESCCalloutSide Side, bool bStrict, TConstArrayView<FSCCalloutTarget> All,
                                const TArray<FPlaced>& Placed, const FBox2D& Screen, const FSCCalloutParams& P, FSCCalloutPlacement& Out)
{
    const bool bNorth = IsNorth(Side), bEast = IsEast(Side);
    const double H = T.LabelSize.Y, Sy = bNorth ? -1 : 1;
    const FBox2D Own = T.Object.bIsValid ? T.Object.ExpandBy(P.Margin) : FBox2D(T.Anchor, T.Anchor).ExpandBy(P.Margin);
    // Shape 0: the label clears its object above (north) or below (south). Shape 1: it sits beside the object. Shapes 2, 3:
    // the same with the vertical riser after the 45° run.
    const double VerticalY = bNorth ? FMath::Min(T.Anchor.Y - P.MinRun, Own.Min.Y - H / 2) : FMath::Max(T.Anchor.Y + P.MinRun, Own.Max.Y + H / 2);
    const double Starts[2] = {VerticalY, T.Anchor.Y + Sy * P.MinRun};
    const TOptional<double> ClearXs[2] = {TOptional<double>(), TOptional<double>(bEast ? Own.Max.X : Own.Min.X)};
    bool bFound = false;
    double BestLength = 0;
    for (int32 Shape = 0; Shape < 4; ++Shape)
    {
        double Y = Starts[Shape % 2];
        FSCCalloutPlacement Try;
        Try.Key = T.Key;
        for (int32 Step = 0; Step < 48; ++Step)
        {
            Build(T, Side, Y, ClearXs[Shape % 2], Shape >= 2, P, Try);
            if (!Inside(Try.Label, Screen) || BoxesOverlap(Try.Label, Own) || FMath::Abs(Y - T.Anchor.Y) > P.MaxRise) break;
            // The first conflict pushes the label outward (up for north, down for south) past the obstacle.
            TOptional<double> Push;
            auto PastBox = [&](const FBox2D& Box, double Gap) { return bNorth ? Box.Min.Y - Gap - H / 2 : Box.Max.Y + Gap + H / 2; };
            for (const FPlaced& Other : Placed)
            {
                if (BoxesOverlap(Try.Label, Other.Label, P.Gap)) { Push = PastBox(Other.Label, P.Gap); break; }
                if (bStrict && (PolylinesCross(Try.Line, Other.Line) || PolylineHitsBox(Try.Line, Other.Label) || PolylineHitsBox(Other.Line, Try.Label)))
                {
                    Push = Y + Sy * (P.Gap + H / 2);
                    break;
                }
            }
            if (!Push && bStrict)
                for (int32 J = 0; J < All.Num() && !Push; ++J)
                    if (J != Index && All[J].Object.bIsValid && BoxesOverlap(Try.Label, All[J].Object)) Push = PastBox(All[J].Object, P.Gap);
            if (!Push)
            {
                const double Length = LineLength(Try.Line);
                if (!bFound || Length < BestLength - Eps)
                {
                    Out = Try;
                    BestLength = Length;
                    bFound = true;
                }
                break;
            }
            // Never move inward (a push that does not progress would loop).
            const double Next = bNorth ? FMath::Min(*Push, Y - 1) : FMath::Max(*Push, Y + 1);
            Y = Next;
        }
    }
    Out.bLabel = bFound;
    return bFound;
}

TArray<FSCCalloutPlacement> FSCCalloutSolver::Solve(TConstArrayView<FSCCalloutTarget> Targets, const FBox2D& Screen, const FSCCalloutParams& P)
{
    TArray<FSCCalloutPlacement> Result;
    Result.SetNum(Targets.Num());
    TArray<int32> Order;
    for (int32 I = 0; I < Targets.Num(); ++I)
    {
        Order.Add(I);
        Result[I].Key = Targets[I].Key;
        Result[I].Line.Add(Targets[I].Anchor);
    }
    // The budget goes by priority, then distance.
    Order.Sort([&](int32 A, int32 B) {
        const FSCCalloutTarget &TA = Targets[A], &TB = Targets[B];
        if (TA.Priority != TB.Priority) return TA.Priority > TB.Priority;
        if (TA.Depth != TB.Depth) return TA.Depth < TB.Depth;
        return TA.Key < TB.Key;
    });
    if (Order.Num() > P.MaxLabels) Order.SetNum(FMath::Max(0, P.MaxLabels));
    // Placement: priority first (best slots); within a priority, the lowest anchor first, so labels pushed upward by
    // the ones below them stack in anchor order and their leaders run parallel instead of crossing.
    Order.Sort([&](int32 A, int32 B) {
        const FSCCalloutTarget &TA = Targets[A], &TB = Targets[B];
        if (TA.Priority != TB.Priority) return TA.Priority > TB.Priority;
        if (TA.Anchor.Y != TB.Anchor.Y) return TA.Anchor.Y > TB.Anchor.Y;
        return TA.Key < TB.Key;
    });
    TArray<FPlaced> Placed;
    static const ESCCalloutSide Preference[] = {ESCCalloutSide::NE, ESCCalloutSide::NW, ESCCalloutSide::SE, ESCCalloutSide::SW};
    for (int32 I : Order)
    {
        const FSCCalloutTarget& T = Targets[I];
        TArray<ESCCalloutSide, TInlineAllocator<5>> Sides;
        if (T.PreviousSide != ESCCalloutSide::None) Sides.Add(T.PreviousSide);
        for (ESCCalloutSide S : Preference) Sides.AddUnique(S);
        FSCCalloutPlacement Out;
        bool bPlaced = false;
        for (int32 Pass = 0; Pass < 1 && !bPlaced; ++Pass) // strict only: a label that cannot keep every rule is a dot
            for (ESCCalloutSide S : Sides)
                if (TryPlace(T, I, S, Pass == 0, Targets, Placed, Screen, P, Out)) { bPlaced = true; break; }
        if (!bPlaced) continue;
        Out.Key = T.Key;
        Result[I] = Out;
        Placed.Add({Out.Label, Out.Line, I});
    }
    return Result;
}
