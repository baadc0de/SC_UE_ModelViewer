// SSCCalloutLayer: target gathering (projection of world bounds into the layer's space), the 100 ms solve, motion and
// painting of anchors, leaders and paper labels.
#include "SSCCalloutLayer.h"
#include "SSCMobiGlasWidgets.h"
#include "SSCMobiGlasPanels.h"
#include "SCMobiGlasModel.h"
#include "SCMobiGlasStyle.h"
#include "SCCargoViewSource.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Widgets/SViewport.h"

#define LOCTEXT_NAMESPACE "SCMobiGlas"
using namespace SCMobiGlasStyle;

namespace
{
constexpr int32 GridKey = 1000000, BoxKey = 2000000, DroneKey = 3000000;
bool IsEast(ESCCalloutSide S) { return S == ESCCalloutSide::NE || S == ESCCalloutSide::SE; }
} // namespace

void SSCCalloutLayer::Construct(const FArguments& Args)
{
    Model = Args._Model;
    SetVisibility(EVisibility::HitTestInvisible);
}

void SSCCalloutLayer::Tick(const FGeometry& Geometry, double Time, float Delta)
{
    Now = Time;
    Model->Refresh(Delta);
    Gather(Geometry);
    // Solved every frame: what is painted is always a placement the solver checked (no overlap, no cover, no
    // crossing). Stability comes from the solver keeping each label's previous side; motion is the fade and the slide
    // of a label around its solved rect, never an interpolated position.
    Solve(Geometry);
    for (auto It = Items.CreateIterator(); It; ++It)
    {
        FItem& I = It.Value();
        I.Offset = I.TargetOffset;
        if (I.Died >= 0 && Now - I.Died > CalloutOut) It.RemoveCurrent();
    }
}

void SSCCalloutLayer::Gather(const FGeometry& Geometry)
{
    for (auto& Pair : Items) Pair.Value.bSeen = false;
    UWorld* World = SCCargoUI::ViewerWorld();
    APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
    TSharedPtr<SViewport> Viewport = GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetGameViewportWidget() : nullptr;
    if (PC && Viewport && Model->HasSource())
    {
        const FGeometry& View = Viewport->GetCachedGeometry();
        const FVector Eye = PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
        // Game viewport pixels -> Slate absolute (the inverse of SSCViewportInput's pick conversion) -> this layer.
        auto Project = [&](const FVector& Point, FVector2D& Out) {
            FVector2D Pixel;
            if (!PC->ProjectWorldLocationToScreen(Point, Pixel, false)) return false;
            Out = Geometry.AbsoluteToLocal(View.LocalToAbsolute(Pixel / FMath::Max(0.01f, View.Scale)));
            return true;
        };
        const FVector2D LocalSize = Geometry.GetLocalSize();
        Area = FBox2D(FVector2D::ZeroVector, LocalSize);
        if (const TOptional<FSlateRect>& Clear = Model->GetClearArea())
            Area = FBox2D(Geometry.AbsoluteToLocal(FVector2D(Clear->Left, Clear->Top)), Geometry.AbsoluteToLocal(FVector2D(Clear->Right, Clear->Bottom)));
        auto Add = [&](int32 Key, EKind Kind, const FString& Name, const FString& Value, const FVector& AnchorWorld, const FBox& Bounds, int32 Priority,
                       bool bSelected, bool bNeutral = false) {
            FVector2D Anchor;
            if (!Project(AnchorWorld, Anchor) || !Area.IsInside(Anchor)) return; // behind the camera, off screen, under a panel
            FBox2D Object(ForceInit);
            for (int32 C = 0; C < 8 && Bounds.IsValid; ++C)
            {
                FVector2D P;
                const FVector Corner(C & 1 ? Bounds.Max.X : Bounds.Min.X, C & 2 ? Bounds.Max.Y : Bounds.Min.Y, C & 4 ? Bounds.Max.Z : Bounds.Min.Z);
                if (!Project(Corner, P)) { Object = FBox2D(FVector2D::ZeroVector, LocalSize); break; } // the camera is inside / at the object
                Object += P;
            }
            FItem& I = Items.FindOrAdd(Key);
            if (I.Died >= 0) { I = FItem(); } // came back while fading out: start again
            if (I.Born < 0) I.Born = Now;
            I.Kind = Kind;
            I.Name = Name;
            I.Value = Value;
            I.Anchor = Anchor;
            I.Object = Object;
            I.Priority = Priority;
            I.Depth = FVector::Dist(Eye, AnchorWorld);
            I.bSelected = bSelected;
            I.bNeutral = bNeutral;
            I.bSeen = true;
            const FVector2f A = SCMobiGlasPaint::Measure(Name, Label()), B = SCMobiGlasPaint::Measure(Value, Numeric());
            I.LabelSize = FVector2D(FMath::Max(A.X, B.X) + 2 * CalloutPadX + FocusWidth + 1, A.Y + B.Y + 2 * CalloutPadY);
        };
        // Boxes of active jobs: carried ones and the ones still reserved on (or placed onto) a grid.
        TArray<FSCCargoBoxView> Boxes = Model->GetMovingBoxes();
        for (const FSCCargoGridView& G : Model->GetGrids())
            for (const FSCCargoBoxView& B : G.Boxes)
                if (B.State != ESCCargoBoxState::Stored) Boxes.Add(B);
        for (const FSCCargoBoxView& B : Boxes)
        {
            const FSCCargoJobView* Job = Model->FindJob(B.JobId);
            if (!Job) // a carried box without its job id: the only active job, if there is one
            {
                const TArray<FSCCargoJobView>& Jobs = Model->GetJobs();
                const int32 Active = Jobs.FilterByPredicate([](const FSCCargoJobView& J) { return FSCMobiGlasModel::IsActive(J.State); }).Num();
                if (Active == 1) Job = Jobs.FindByPredicate([](const FSCCargoJobView& J) { return FSCMobiGlasModel::IsActive(J.State); });
            }
            if (Job && !FSCMobiGlasModel::IsActive(Job->State)) continue;
            const FString Dest = Job ? Caps(Model->DescribeGrids(Job->ToGrids).ToString()) : FString();
            Add(BoxKey + B.BoxId, EKind::Box, FString::Printf(TEXT("%s · %d SCU%s"), *Caps(B.CommodityLabel.ToString()), B.SCU,
                *(B.bMine || B.OwnerLabel.IsEmpty() ? FString() : TEXT(" · ") + Caps(B.OwnerLabel.ToString()))),
                SCMobiGlasText::BoxState(uint8(B.State)) + (Dest.IsEmpty() ? FString() : TEXT(" → ") + Dest), B.WorldCenter, B.WorldBounds,
                B.bMine ? 3 : 1, false, !B.bMine); // other owners' cargo: neutral, below ours
        }
        for (const FSCCargoDroneView& D : Model->GetDrones())
            if (D.State != ESCCargoDroneState::Docked)
            {
                const FSCCargoJobView* Job = Model->FindJob(D.JobId);
                const bool bOther = Job && !Job->bMine;
                const FString Name = bOther && !D.OwnerLabel.IsEmpty() ? Caps(D.Label.ToString() + TEXT(" · ") + D.OwnerLabel.ToString()) : Caps(D.Label.ToString());
                Add(DroneKey + D.DroneId, EKind::Drone, Name, SCMobiGlasText::DroneState(uint8(D.State)), D.WorldLocation, D.WorldBounds, bOther ? 1 : 3, false, bOther);
            }
        if (Model->IsOpen())
            for (const FSCCargoGridView& G : Model->GetGrids())
            {
                const FVector Centre = G.LatticeToWorld.TransformPosition(FVector(G.Cells) * G.CellSizeM * 50); // m -> cm, half
                const bool bSel = G.GridId == Model->GetSelectedGrid();
                Add(GridKey + G.GridId, EKind::Grid, Caps(Model->ShortGridName(G).ToString()), SCMobiGlasText::IsFixed(G) ? SCMobiGlasText::Scu(G.UsedSCU, G.CapacitySCU).ToString()
                    : SCMobiGlasText::Scu(G.UsedSCU, G.CapacitySCU).ToString() + TEXT(" · ") + SCMobiGlasText::Access(G).ToString(),
                    Centre, G.WorldBounds, bSel ? 2 : 1, bSel);
            }
    }
    for (auto& Pair : Items)
        if (!Pair.Value.bSeen && Pair.Value.Died < 0) Pair.Value.Died = Now;
}

void SSCCalloutLayer::Solve(const FGeometry& Geometry)
{
    TArray<FSCCalloutTarget> Targets;
    TArray<int32> Keys;
    for (const auto& Pair : Items)
    {
        const FItem& I = Pair.Value;
        if (!I.bSeen) continue;
        FSCCalloutTarget& T = Targets.AddDefaulted_GetRef();
        T.Key = Pair.Key;
        T.Anchor = I.Anchor;
        T.Object = I.Object;
        T.LabelSize = I.LabelSize;
        T.Priority = I.Priority;
        T.Depth = I.Depth;
        T.PreviousSide = I.bLabel ? I.Side : ESCCalloutSide::None;
        Keys.Add(Pair.Key);
    }
    FBox2D Screen(FVector2D::ZeroVector, Geometry.GetLocalSize());
    if (const TOptional<FSlateRect>& Clear = Model->GetClearArea())
        Screen = FBox2D(Geometry.AbsoluteToLocal(FVector2D(Clear->Left, Clear->Top)), Geometry.AbsoluteToLocal(FVector2D(Clear->Right, Clear->Bottom)));
    Screen = Screen.ExpandBy(-CalloutScreenInset);
    FSCCalloutParams P;
    P.Margin = CalloutMargin;
    P.MinRun = CalloutMinRun;
    P.MaxRun = CalloutMaxRun;
    P.MinShelf = CalloutMinShelf;
    P.Gap = CalloutGap;
    P.MaxRise = CalloutMaxRise;
    P.MaxLabels = CalloutMaxLabels;
    const TArray<FSCCalloutPlacement> Out = FSCCalloutSolver::Solve(Targets, Screen, P);
    for (int32 N = 0; N < Out.Num(); ++N)
    {
        FItem& I = Items[Keys[N]];
        I.bLabel = Out[N].bLabel;
        if (!I.bLabel) continue;
        I.Side = Out[N].Side;
        I.TargetOffset = Out[N].Label.Min - I.Anchor;
    }
}

TArray<FVector2f> SSCCalloutLayer::Leader(const FVector2D& A, const FBox2D& Label, ESCCalloutSide Side)
{
    const FVector2D Edge(IsEast(Side) ? Label.Min.X : Label.Max.X, Label.GetCenter().Y);
    const double Sx = Edge.X >= A.X ? 1 : -1, Sy = Edge.Y >= A.Y ? 1 : -1;
    const double Dy = FMath::Abs(Edge.Y - A.Y), Dx = FMath::Abs(Edge.X - A.X);
    const double Run = FMath::Clamp(FMath::Min(Dx - CalloutMinShelf, double(CalloutMaxRun)), 0.0, Dy);
    TArray<FVector2f> Points{FVector2f(A)};
    if (Dy - Run > 0.5) Points.Add(FVector2f(A.X, A.Y + Sy * (Dy - Run)));
    Points.Add(FVector2f(A.X + Sx * Run, Edge.Y));
    Points.Add(FVector2f(Edge));
    return Points;
}

int32 SSCCalloutLayer::OnPaint(const FPaintArgs&, const FGeometry& G, const FSlateRect&, FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle&, bool) const
{
    using namespace SCMobiGlasPaint;
    const FLinearColor Halo = Ink().CopyWithNewOpacity(0.55f);
    for (const auto& Pair : Items)
    {
        const FItem& I = Pair.Value;
        const bool bDying = I.Died >= 0;
        const float Fade = bDying ? 1 - float((Now - I.Died) / CalloutOut) : 1.f;
        const float LineT = bDying ? Fade : EaseOutCubic(float((Now - I.Born) / CalloutLineIn));
        const float LabelT = bDying ? Fade : EaseOutCubic(float((Now - I.Born - CalloutLineIn) / CalloutFadeIn));
        const FLinearColor LineColour = (I.bSelected ? Accent() : OnScrim()).CopyWithNewOpacity(Fade);
        const float Width = I.bSelected ? LeaderSelectedWidth : LeaderWidth;
        // Selected grid: accent corner brackets on its projected bounds.
        if (I.bSelected && I.Object.bIsValid && I.Object.GetSize().X < G.GetLocalSize().X)
        {
            const FVector2f Mn(I.Object.Min), Mx(I.Object.Max);
            const float L = FMath::Min(12.f, float(FMath::Min(I.Object.GetSize().X, I.Object.GetSize().Y)) / 3);
            for (int32 C = 0; C < 4; ++C)
            {
                const FVector2f P(C & 1 ? Mx.X : Mn.X, C & 2 ? Mx.Y : Mn.Y), D(C & 1 ? -L : L, C & 2 ? -L : L);
                if (Area.IsInside(FVector2D(P))) // never over the panels
                    Lines(Out, Layer, G, {P + FVector2f(D.X, 0), P, P + FVector2f(0, D.Y)}, LineColour, LeaderSelectedWidth);
            }
        }
        // Anchor: 5 px square with a 1 px dark halo.
        const FVector2f A(I.Anchor);
        Box(Out, Layer, G, A - FVector2f(CalloutDot / 2 + CalloutHalo), FVector2f(CalloutDot + 2 * CalloutHalo), Halo.CopyWithNewOpacity(Halo.A * Fade));
        Box(Out, Layer + 1, G, A - FVector2f(CalloutDot / 2), FVector2f(CalloutDot), LineColour);
        if (!I.bLabel || I.Side == ESCCalloutSide::None) continue;
        const FVector2D Min = I.Anchor + I.Offset;
        const FBox2D Rect(Min, Min + I.LabelSize);
        // Leader, drawn progressively anchor -> elbow -> shelf.
        TArray<FVector2f> Path = Leader(I.Anchor, Rect, I.Side), Drawn{Path[0]};
        float Total = 0;
        for (int32 N = 1; N < Path.Num(); ++N) Total += FVector2f::Distance(Path[N - 1], Path[N]);
        float Budget = Total * FMath::Clamp(LineT, 0.f, 1.f);
        for (int32 N = 1; N < Path.Num() && Budget > 0; ++N)
        {
            const float Seg = FVector2f::Distance(Path[N - 1], Path[N]);
            Drawn.Add(Seg <= Budget ? Path[N] : Path[N - 1] + (Path[N] - Path[N - 1]) * (Budget / FMath::Max(Seg, 1e-3f)));
            Budget -= Seg;
        }
        if (Drawn.Num() > 1)
        {
            Lines(Out, Layer, G, Drawn, Halo.CopyWithNewOpacity(Halo.A * Fade), Width + 2);
            Lines(Out, Layer + 1, G, Drawn, LineColour, Width);
        }
        if (LabelT <= 0) continue;
        // Paper placard sliding in from the shelf side; a colour edge on the shelf side marks targets.
        const float Slide = CalloutSlide * (1 - FMath::Clamp(LabelT, 0.f, 1.f)) * (IsEast(I.Side) ? -1 : 1);
        const FVector2f Pos = FVector2f(Rect.Min) + FVector2f(Slide, 0), Size(I.LabelSize);
        const float Alpha = FMath::Clamp(LabelT, 0.f, 1.f);
        const FLinearColor Edge = I.bSelected || (I.Kind != EKind::Grid && !I.bNeutral) ? Accent() : Ink2();
        Box(Out, Layer + 2, G, Pos, Size, Paper().CopyWithNewOpacity(Alpha));
        Box(Out, Layer + 3, G, IsEast(I.Side) ? Pos : Pos + FVector2f(Size.X - FocusWidth, 0), FVector2f(FocusWidth, Size.Y), Edge.CopyWithNewOpacity(Alpha));
        const float TextX = Pos.X + CalloutPadX + (IsEast(I.Side) ? FocusWidth + 1 : 0);
        const FVector2f NameSize = Measure(I.Name, Label());
        Text(Out, Layer + 3, G, FVector2f(TextX, Pos.Y + CalloutPadY), I.Name, Label(), Ink().CopyWithNewOpacity(Alpha));
        Text(Out, Layer + 3, G, FVector2f(TextX, Pos.Y + CalloutPadY + NameSize.Y), I.Value, Numeric(), Ink2().CopyWithNewOpacity(Alpha));
    }
    return Layer + 4;
}

#undef LOCTEXT_NAMESPACE
