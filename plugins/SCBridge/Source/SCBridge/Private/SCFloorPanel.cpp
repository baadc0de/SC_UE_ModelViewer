// WP-P5 floor panel: the viewer's own floor-select UI on a lift panel's screen face (SCFloorPanel.h): the face's
// geometry and image map, our layout and view, the panel's assembly at import, the aim, E, and the drawing into a
// render target that the face's M_SC_Screen instance shows (the I1-5 face path, ASCVehicle::SetScreenFaceMaterial).
#include "SCFloorPanel.h"
#include "SCVehicle.h"
#include "SCLift.h"
#include "SCInstancing.h"
#include "SCMaterialBuilder.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Components/MeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "StaticMeshAttributes.h"
#include "TextureResource.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCFloorPanel, Log, All);

// ------------------------------------------------------------------------------------------------------ face geometry
bool FSCScreenFaceGeometry::FromDescription(const FMeshDescription& Description, int32 Slot, TFunctionRef<int32(FTriangleID)> NodeOf,
                                            TFunctionRef<bool(FTriangleID)> SoftOf, FSCScreenFaceGeometry& Out)
{
    FStaticMeshConstAttributes A(Description);
    const auto Positions = A.GetVertexPositions();
    const auto Normals = A.GetVertexInstanceNormals();
    const auto UVs = A.GetVertexInstanceUVs();
    Out = FSCScreenFaceGeometry();
    if (!Positions.IsValid() || !Normals.IsValid() || !UVs.IsValid() || UVs.GetNumChannels() < 1) return false;
    for (const FTriangleID T : Description.Triangles().GetElementIDs())
    {
        if (Description.GetTrianglePolygonGroup(T).GetValue() != Slot) continue;
        FVector3f Front = FVector3f::ZeroVector;
        for (const FVertexInstanceID Corner : Description.GetTriangleVertexInstances(T))
        {
            Out.Positions.Add(Positions[Description.GetVertexInstanceVertex(Corner)]);
            Out.UVs.Add(UVs.Get(Corner, 0));
            Front += Normals[Corner];
        }
        Out.Fronts.Add(Front);
        Out.Nodes.Add(NodeOf(T));
        Out.Soft.Add(SoftOf(T));
    }
    return !Out.Positions.IsEmpty();
}

FSCScreenFaceGeometry FSCScreenFaceGeometry::Select(int32 Owner, bool bSoft) const
{
    FSCScreenFaceGeometry Out;
    for (int32 T = 0; T < Nodes.Num(); ++T)
    {
        if ((Owner != INDEX_NONE && Nodes[T] != Owner) || (Soft.IsValidIndex(T) && Soft[T] != bSoft)) continue;
        for (int32 C = 0; C < 3; ++C)
        {
            Out.Positions.Add(Positions[3 * T + C]);
            Out.UVs.Add(UVs[3 * T + C]);
        }
        Out.Fronts.Add(Fronts[T]);
        Out.Nodes.Add(Nodes[T]);
        Out.Soft.Add(Soft.IsValidIndex(T) && Soft[T]);
    }
    return Out;
}

FSCScreenFaceGeometry FSCScreenFaceGeometry::Scaled(const FVector& Scale) const
{
    FSCScreenFaceGeometry Out = *this;
    const FVector3f S(Scale);
    for (FVector3f& P : Out.Positions) P *= S;
    // A normal transforms by the inverse transpose: for a diagonal scale, divide (a reflection flips it).
    for (FVector3f& F : Out.Fronts)
        F = FVector3f(S.X != 0 ? F.X / S.X : 0, S.Y != 0 ? F.Y / S.Y : 0, S.Z != 0 ? F.Z / S.Z : 0);
    return Out;
}

bool FSCScreenFaceGeometry::Build(const FVector& UpHint)
{
    const int32 N = Positions.Num();
    if (N < 3 || N % 3 || UVs.Num() != N) return false;
    // The plane: the area-weighted triangle normal, turned to the side the vertex normals face (the front).
    FVector Sum = FVector::ZeroVector;
    for (int32 I = 0; I < N; I += 3)
        Sum += (FVector(Positions[I + 1]) - FVector(Positions[I])) ^ (FVector(Positions[I + 2]) - FVector(Positions[I]));
    if (Sum.IsNearlyZero()) return false;
    Normal = Sum.GetSafeNormal();
    FVector Front = FVector::ZeroVector;
    for (const FVector3f& F : Fronts) Front += FVector(F);
    if ((Normal | Front) < 0) Normal = -Normal;
    // Up: the hint in the plane; a face lying flat takes the image's own v direction instead.
    FVector U = UpHint - Normal * (UpHint | Normal);
    if (U.SizeSquared() < 1e-6) U = (Normal ^ FVector::RightVector).SizeSquared() > 1e-6 ? Normal ^ FVector::RightVector : Normal ^ FVector::ForwardVector;
    Up = U.GetSafeNormal();
    Right = Normal ^ Up; // seen from the front (looking along -Normal), Right is to the viewer's right
    double RMax = -UE_BIG_NUMBER, UMin = UE_BIG_NUMBER;
    RightMin = UE_BIG_NUMBER;
    UpMax = -UE_BIG_NUMBER;
    Plane = 0;
    for (const FVector3f& P : Positions)
    {
        const double R = FVector(P) | Right, V = FVector(P) | Up;
        RightMin = FMath::Min(RightMin, R);
        RMax = FMath::Max(RMax, R);
        UMin = FMath::Min(UMin, V);
        UpMax = FMath::Max(UpMax, V);
        Plane += (FVector(P) | Normal) / N;
    }
    Width = RMax - RightMin;
    Height = UpMax - UMin;
    if (Width <= UE_KINDA_SMALL_NUMBER || Height <= UE_KINDA_SMALL_NUMBER) return false;
    // Content of each corner, and the affine map from its UV to it (least squares: normal equations over (u, v, 1)).
    double M[3][3] = {}, BX[3] = {}, BY[3] = {};
    TArray<FVector2D> Content;
    Content.Reserve(N);
    for (int32 I = 0; I < N; ++I)
    {
        const FVector P(Positions[I]);
        const FVector2D C(((P | Right) - RightMin) / Width, (UpMax - (P | Up)) / Height);
        Content.Add(C);
        const double Q[3] = {UVs[I].X, UVs[I].Y, 1.0};
        for (int32 R = 0; R < 3; ++R)
        {
            for (int32 K = 0; K < 3; ++K) M[R][K] += Q[R] * Q[K];
            BX[R] += Q[R] * C.X;
            BY[R] += Q[R] * C.Y;
        }
    }
    const double Det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                       M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
    if (FMath::Abs(Det) < 1e-18) return false; // the UVs do not span the face
    auto Solve = [&](const double B[3]) {
        double X[3];
        for (int32 Col = 0; Col < 3; ++Col)
        {
            double T[3][3];
            for (int32 R = 0; R < 3; ++R)
                for (int32 K = 0; K < 3; ++K) T[R][K] = K == Col ? B[R] : M[R][K];
            X[Col] = (T[0][0] * (T[1][1] * T[2][2] - T[1][2] * T[2][1]) - T[0][1] * (T[1][0] * T[2][2] - T[1][2] * T[2][0]) +
                      T[0][2] * (T[1][0] * T[2][1] - T[1][1] * T[2][0])) /
                     Det;
        }
        return FVector(X[0], X[1], X[2]);
    };
    X = Solve(BX);
    Y = Solve(BY);
    FitError = 0;
    for (int32 I = 0; I < N; ++I) FitError = FMath::Max(FitError, (ContentFromUV(UVs[I]) - Content[I]).GetAbsMax());
    return true;
}

FVector2D FSCScreenFaceGeometry::ContentFromUV(const FVector2f& UV) const
{
    return FVector2D(X.X * UV.X + X.Y * UV.Y + X.Z, Y.X * UV.X + Y.Y * UV.Y + Y.Z);
}

bool FSCScreenFaceGeometry::Raycast(const FVector& Origin, const FVector& Dir, double MaxDistance, double& OutDistance, FVector2f& OutUV) const
{
    if ((Dir | Normal) >= 0) return false; // from behind or along the face
    bool bHit = false;
    OutDistance = MaxDistance;
    for (int32 I = 0; I + 2 < Positions.Num(); I += 3)
    {
        // Moller-Trumbore.
        const FVector A(Positions[I]), E1 = FVector(Positions[I + 1]) - A, E2 = FVector(Positions[I + 2]) - A;
        const FVector P = Dir ^ E2;
        const double Det = E1 | P;
        if (FMath::Abs(Det) < 1e-12) continue;
        const double Inv = 1.0 / Det;
        const FVector T = Origin - A;
        const double U = (T | P) * Inv;
        if (U < 0 || U > 1) continue;
        const FVector Q = T ^ E1;
        const double V = (Dir | Q) * Inv;
        if (V < 0 || U + V > 1) continue;
        const double Dist = (E2 | Q) * Inv;
        if (Dist < 0 || Dist > OutDistance) continue;
        OutDistance = Dist;
        OutUV = UVs[I] * float(1 - U - V) + UVs[I + 1] * float(U) + UVs[I + 2] * float(V);
        bHit = true;
    }
    return bHit;
}

FVector FSCScreenFaceGeometry::PointAt(const FVector2D& C) const
{
    return Right * (RightMin + C.X * Width) + Up * (UpMax - C.Y * Height) + Normal * Plane;
}

// ------------------------------------------------------------------------------------------------------ layout, view
FSCFloorPanelLayout FSCFloorPanelLayout::Make(TArray<FSCFloorPanelOption> InOptions, double Aspect)
{
    FSCFloorPanelLayout L;
    L.Options = MoveTemp(InOptions);
    Aspect = Aspect > 0 ? Aspect : 1;
    // Content units: x over the face's width, y over its height (height 1, width Aspect); the margin is square.
    const double Shorter = FMath::Min(Aspect, 1.0);
    const double MX = Margin * Shorter / Aspect, MY = Margin * Shorter;
    L.Status = FBox2D(FVector2D(MX, 1 - MY - StatusHeight), FVector2D(1 - MX, 1 - MY));
    const int32 N = L.Options.Num();
    if (N == 0) return L;
    const double Top = MY, Bottom = L.Status.Min.Y - Gap;
    const double Row = (Bottom - Top - Gap * (N - 1)) / N;
    for (int32 I = 0; I < N; ++I)
    {
        const double Y0 = Top + I * (Row + Gap);
        L.Options[I].Rect = FBox2D(FVector2D(MX, Y0), FVector2D(1 - MX, Y0 + Row));
    }
    return L;
}

int32 FSCFloorPanelLayout::OptionAt(const FVector2D& C) const
{
    for (int32 I = 0; I < Options.Num(); ++I)
        if (Options[I].Rect.IsInside(C)) return I;
    return INDEX_NONE;
}

FSCFloorPanelView FSCFloorPanelView::Of(const FSCLift& Lift, int32 Highlight, double Time)
{
    FSCFloorPanelView V;
    V.Highlight = Highlight;
    V.Current = Lift.CurrentStop;
    V.Target = Lift.TargetStop;
    for (const FSCLiftRequest& R : Lift.Queue) V.Requested.AddUnique(R.Stop);
    V.Requested.Sort();
    if (Lift.Stops.IsValidIndex(V.Target))
    {
        const double Dz = Lift.Stops[V.Target].ZCm - Lift.ZAt(Lift.S);
        V.Direction = FMath::Abs(Dz) < 0.5 ? 0 : (Dz > 0 ? 1 : -1);
        V.bBlink = FMath::Frac(Time * BlinkHz) < 0.5;
    }
    return V;
}

FString FSCFloorPanelView::Describe() const
{
    TArray<FString> Q;
    for (int32 S : Requested) Q.Add(FString::FromInt(S));
    return FString::Printf(TEXT("highlight=%d current=%d target=%d dir=%d requested=%s"), Highlight, Current, Target, Direction,
                           Q.IsEmpty() ? TEXT("-") : *FString::Join(Q, TEXT(",")));
}

FSCFloorPanelColours FSCFloorPanelColours::From(const FSCLiftPanelStyle& Style)
{
    // Our choice of the pads' palette roles (WP-P5 "Result (floor panel)"): a darker blue field (Accent2) with the pads'
    // button blue (Base) for the rows, the pads' label colour (the label's own role, Bright) for the text, the selected
    // role for the row under the crosshair (its text in the dark Background), cyan (Accent1) for the current floor, the
    // caution yellow (Moderate) for a called floor, and the dark Background for the status bar.
    FSCFloorPanelColours C;
    auto Role = [&](const TCHAR* Name, const FLinearColor& Without) {
        const FColor* Found = Style.Palette.Find(Name);
        return Found ? FLinearColor::FromSRGBColor(*Found) : Without;
    };
    C.bFromData = !Style.Palette.IsEmpty();
    // Without a palette: white on black only (absence of data; no colour invented).
    const FLinearColor Black = FLinearColor::Black, White = FLinearColor::White;
    C.Field = Role(TEXT("Accent2"), Black);
    C.Row = Role(TEXT("Base"), Black);
    C.RowHighlight = Role(TEXT("Selected"), White);
    C.Label = Role(Style.LabelColourRole.IsEmpty() ? TEXT("Bright") : *Style.LabelColourRole, White);
    C.LabelHighlight = Role(TEXT("Background"), Black);
    C.Current = Role(TEXT("Accent1"), White);
    C.Requested = Role(TEXT("Moderate"), White);
    C.Status = Role(TEXT("Background"), Black);
    C.Arrow = Role(TEXT("Bright"), White);
    return C;
}

// ------------------------------------------------------------------------------------------------------- the vehicle
namespace
{
/** Render-target height: about 1.9 px per mm on the C2's 0.23 m face (labels ~70 px high), and the largest side. Our
 *  resolution choice. */
constexpr int32 PanelPixelsHigh = 448;
constexpr int32 PanelPixelsMax = 2048;
/** A Buttons panel's highlight: a frame around the node under the crosshair, this fraction of the render's shorter side
 *  wide (ours: the game draws its own hover state, which the exporter does not render). */
constexpr double ButtonFrameWidth = 0.012;

/** False when nothing could be drawn (no render target resource, no world, a host that never renders). */
bool DrawPanel(FSCFloorPanel& P, const FSCFloorPanelView& V, UWorld* World)
{
    UTextureRenderTarget2D* RT = P.Target.Get();
    if (!RT || !World || !FApp::CanEverRender()) return false;
    FTextureRenderTargetResource* Resource = RT->GameThread_GetRenderTargetResource();
    if (!Resource) return false;
    const FVector2D Size(RT->SizeX, RT->SizeY);
    const FSCFloorPanelColours& C = P.Colours;
    FCanvas Canvas(Resource, nullptr, World, World->GetFeatureLevel());
    if (P.Kind == ESCPanelKind::Buttons)
    {
        // The game's render of the owner's current state over the whole target (the target is the render's own image
        // space), and our frame around the node under the crosshair.
        Canvas.Clear(FLinearColor::Black);
        if (UTexture2D* T = P.Images.FindRef(V.Image).Get(); T && T->GetResource())
        {
            FCanvasTileItem Tile(FVector2D::ZeroVector, T->GetResource(), Size, FLinearColor::White);
            Tile.BlendMode = SE_BLEND_Opaque;
            Canvas.DrawItem(Tile);
        }
        // WP-I1-4: the game's own hover look when the export rendered it (the render is then of the node aimed at);
        // our frame only for a render without one.
        if (P.Layout.Options.IsValidIndex(V.Highlight) && !V.bHoverImage)
        {
            const FBox2D R(P.Layout.Options[V.Highlight].Rect.Min * Size, P.Layout.Options[V.Highlight].Rect.Max * Size);
            const double W = FMath::Max(2.0, FMath::Min(Size.X, Size.Y) * ButtonFrameWidth);
            for (const FBox2D& Edge : {FBox2D(R.Min, FVector2D(R.Max.X, R.Min.Y + W)), FBox2D(FVector2D(R.Min.X, R.Max.Y - W), R.Max),
                                       FBox2D(R.Min, FVector2D(R.Min.X + W, R.Max.Y)), FBox2D(FVector2D(R.Max.X - W, R.Min.Y), R.Max)})
            {
                FCanvasTileItem Tile(Edge.Min, Edge.GetSize(), P.HighlightColour);
                Tile.BlendMode = SE_BLEND_Opaque;
                Canvas.DrawItem(Tile);
            }
        }
        Canvas.Flush_GameThread(true);
        return true;
    }
    Canvas.Clear(C.Field);
    auto Fill = [&](const FBox2D& R, const FLinearColor& Colour) {
        FCanvasTileItem Tile(R.Min * Size, (R.Max - R.Min) * Size, Colour);
        Tile.BlendMode = SE_BLEND_Opaque;
        Canvas.DrawItem(Tile);
    };
    Fill(P.Layout.Status, C.Status);
    for (int32 I = 0; I < P.Layout.Options.Num(); ++I)
    {
        const FSCFloorPanelOption& O = P.Layout.Options[I];
        const bool bHighlight = I == V.Highlight;
        Fill(O.Rect, bHighlight ? C.RowHighlight : C.Row);
        // The floor's marker: a bar at the row's left edge, its width a fifth of the row's height.
        const double RowH = O.Rect.GetSize().Y;
        const FBox2D Bar(O.Rect.Min, FVector2D(O.Rect.Min.X + RowH * 0.2 * Size.Y / Size.X, O.Rect.Max.Y));
        if (O.Stop == V.Current) Fill(Bar, C.Current);
        else if (O.Stop == V.Target ? V.bBlink : V.Requested.Contains(O.Stop)) Fill(Bar, C.Requested);
        // The label: the stop's mask in the pads' font, its em 45 % of the row, at most 80 % of the row's width.
        const FLinearColor Ink = bHighlight ? C.LabelHighlight : C.Label;
        const FVector2D Centre = O.Rect.GetCenter() * Size;
        UTexture2D* T = P.Masks.IsValidIndex(I) ? P.Masks[I].Get() : nullptr;
        if (T && P.MaskEm[I] > 0)
        {
            double Scale = RowH * Size.Y * 0.45 / P.MaskEm[I];
            Scale = FMath::Min(Scale, O.Rect.GetSize().X * Size.X * 0.8 / FMath::Max(1, T->GetSizeX()));
            const FVector2D Dim(T->GetSizeX() * Scale, T->GetSizeY() * Scale);
            FCanvasTileItem Text(Centre - Dim * 0.5, T->GetResource(), Dim, Ink);
            Text.BlendMode = SE_BLEND_Translucent;
            Canvas.DrawItem(Text);
        }
        else if (GEngine && GEngine->GetLargeFont())
        {
            // No mask (an export before cache format 35, or a name the pads' font cannot spell): the engine font, cased as the
            // pads' label.
            FCanvasTextItem Text(Centre, FText::FromString(P.bUpperCase ? O.Label.ToUpper() : O.Label), GEngine->GetLargeFont(), Ink);
            Text.bCentreX = Text.bCentreY = true;
            const double Em = GEngine->GetLargeFont()->GetMaxCharHeight();
            Text.Scale = FVector2D(RowH * Size.Y * 0.45 / FMath::Max(1.0, Em));
            Canvas.DrawItem(Text);
        }
    }
    // The leg's direction: a chevron in the status bar.
    if (V.Direction != 0)
    {
        const FBox2D S = P.Layout.Status;
        const FVector2D Mid = S.GetCenter() * Size;
        const double H = S.GetSize().Y * Size.Y * 0.6, W = H * 1.2;
        const double Tip = V.Direction > 0 ? -H / 2 : H / 2;
        FCanvasTriangleItem Arrow(Mid + FVector2D(0, Tip), Mid + FVector2D(-W / 2, -Tip), Mid + FVector2D(W / 2, -Tip), GWhiteTexture);
        Arrow.SetColor(C.Arrow);
        Arrow.BlendMode = SE_BLEND_Opaque;
        Canvas.DrawItem(Arrow);
    }
    Canvas.Flush_GameThread(true);
    return true;
}
}

TArray<FSCFloorPanelOption> ASCVehicle::FloorPanelOptions(const FLiftPanel& Panel, int32& OutLift, bool* bOutSeveralLifts) const
{
    // The panel's interactions whose links ask one carriage for a stop (`From Elevator call <N>`), each labelled with its
    // own display name (else the stop's); rows top to bottom from the highest stop down.
    OutLift = INDEX_NONE;
    if (bOutSeveralLifts) *bOutSeveralLifts = false;
    TArray<FSCFloorPanelOption> Options;
    for (const FSCPointInteraction& Choice : Panel.Choices)
        for (const FSCInteractionNetwork::FDelivery& D : Interactions.Route(Panel.Interior, Panel.EntityId, Choice.Name))
        {
            const FSCInteractionNetwork::FEndpoint* T = Interactions.Find(Panel.Interior, D.Target);
            int32 Stop, Dir;
            if (!T || !Lifts.IsValidIndex(T->Lift) || !FSCInteractionNetwork::StopSlot(D.TargetSlot, Stop, Dir) || Dir != 0 ||
                !Lifts[T->Lift]->Stops.IsValidIndex(Stop))
                continue;
            if (OutLift != INDEX_NONE && OutLift != T->Lift)
            {
                if (bOutSeveralLifts) *bOutSeveralLifts = true;
                OutLift = INDEX_NONE;
                return {};
            }
            OutLift = T->Lift;
            if (!Options.ContainsByPredicate([&](const FSCFloorPanelOption& O) { return O.Interaction == Choice.Name; }))
                Options.Add({Stop, Choice.Name, Choice.DisplayName.IsEmpty() ? Lifts[T->Lift]->Stops[Stop].Name : Choice.DisplayName});
        }
    if (Lifts.IsValidIndex(OutLift))
    {
        const FSCLift& Lift = *Lifts[OutLift];
        Options.StableSort([&](const FSCFloorPanelOption& A, const FSCFloorPanelOption& B) { return Lift.Stops[A.Stop].ZCm > Lift.Stops[B.Stop].ZCm; });
    }
    return Options;
}

FName ASCVehicle::LiftPanelPressInteraction(const FLiftPanel& Panel) const
{
    int32 Lift;
    bool bSeveral = false;
    if (Panel.Choices.IsEmpty() || !FloorPanelOptions(Panel, Lift, &bSeveral).IsEmpty() || bSeveral) return NAME_None;
    const FSCPointInteraction* Primary = Panel.Choices.FindByPredicate([](const FSCPointInteraction& C) { return C.bPrimary; });
    return (Primary ? *Primary : Panel.Choices[0]).Name;
}

FTransform FSCFloorPanel::Frame() const
{
    const UMeshComponent* C = Component.Get();
    return C ? FTransform(C->GetComponentQuat(), C->GetComponentLocation()) : FTransform::Identity;
}

void ASCVehicle::BuildFloorPanels(TArray<FSCBlankScreenFace>& Blank, TFunctionRef<UTexture2D*(const FString&)> Texture)
{
    for (const auto& [AnchorKey, Panel] : LiftPanels)
    {
        int32 LiftIndex = INDEX_NONE;
        bool bSeveral = false;
        TArray<FSCFloorPanelOption> Options = FloorPanelOptions(Panel, LiftIndex, &bSeveral);
        if (bSeveral)
            UE_LOG(LogSCFloorPanel, Warning, TEXT("FLOOR_PANEL_BUILD panel=%s: options reach more than one lift; not drawn, E sends nothing"), *Panel.EntityId);
        if (Options.IsEmpty()) continue; // a call button: it calls, it does not pick a floor
        // Its screen face: the largest blank face of the placement.
        FSCBlankScreenFace* Face = nullptr;
        double Area = 0;
        for (FSCBlankScreenFace& B : Blank)
        {
            if (B.Panel.Get() != AnchorKey.Get() || !B.Component.IsValid()) continue;
            double A = 0;
            for (int32 I = 0; I + 2 < B.Geometry.Positions.Num(); I += 3)
                A += ((FVector(B.Geometry.Positions[I + 1]) - FVector(B.Geometry.Positions[I])) ^
                      (FVector(B.Geometry.Positions[I + 2]) - FVector(B.Geometry.Positions[I])))
                         .Size() / 2;
            if (A > Area) { Area = A; Face = &B; }
        }
        if (!Face)
        {
            UE_LOG(LogSCFloorPanel, Warning, TEXT("FLOOR_PANEL_BUILD panel=%s options=%d face=none: the panel has no blank screen face; not drawn, E sends nothing"),
                   *Panel.EntityId, Options.Num());
            continue;
        }
        UMeshComponent* Component = Face->Component.Get();
        FSCFloorPanel P;
        P.Lift = LiftIndex;
        P.Interior = Panel.Interior;
        P.EntityId = Panel.EntityId;
        P.Anchor = AnchorKey;
        P.Component = Component;
        P.Slot = Face->Slot;
        // In the component's frame without scale: a reflected or stretched placement keeps the image upright, unmirrored and
        // at the face's true aspect.
        P.Geometry = Face->Geometry.Scaled(Component->GetComponentScale());
        if (!P.Geometry.Build(P.Frame().InverseTransformVectorNoScale(GetActorUpVector())))
        {
            UE_LOG(LogSCFloorPanel, Warning, TEXT("FLOOR_PANEL_BUILD panel=%s: the screen face is degenerate (no plane or no UV span); not drawn, E sends nothing"),
                   *Panel.EntityId);
            continue;
        }
        const FSCLift& Lift = *Lifts[LiftIndex];
        P.Layout = FSCFloorPanelLayout::Make(MoveTemp(Options), P.Geometry.Aspect());
        P.Colours = FSCFloorPanelColours::From(Lift.PanelStyle);
        // Each row's mask: the label exported for its stop under its own text (cased as the pads' label).
        P.bUpperCase = Lift.PanelStyle.FontSymbol.IsEmpty() || Lift.PanelStyle.bUpperCase;
        for (const FSCFloorPanelOption& O : P.Layout.Options)
        {
            const FString Text = P.bUpperCase ? O.Label.ToUpper() : O.Label;
            const FSCLiftStopLabel* L = Lift.PanelStyle.Labels.FindByPredicate([&](const FSCLiftStopLabel& E) { return E.Stop == O.Stop && E.Text == Text; });
            UTexture2D* Mask = L && L->EmPx > 0 ? Texture(L->Image) : nullptr;
            P.Masks.Add(Mask);
            P.MaskEm.Add(Mask ? L->EmPx : 0.f);
        }
        UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(this, NAME_None, RF_Transient);
        // Linear float: the canvas writes the colours as given (gamma 1) and M_SC_Screen reads them back unchanged.
        RT->RenderTargetFormat = RTF_RGBA16f;
        RT->ClearColor = P.Colours.Field;
        RT->AddressX = RT->AddressY = TA_Clamp;
        // PanelPixelsHigh on the face's height, both sides at most PanelPixelsMax (a long thin face keeps its aspect).
        const double Aspect = P.Geometry.Aspect();
        const double Fit = FMath::Min(1.0, PanelPixelsMax / FMath::Max(PanelPixelsHigh * Aspect, double(PanelPixelsHigh)));
        RT->InitAutoFormat(FMath::Clamp(FMath::RoundToInt(PanelPixelsHigh * Aspect * Fit), 16, PanelPixelsMax),
                           FMath::Clamp(FMath::RoundToInt(PanelPixelsHigh * Fit), 16, PanelPixelsMax));
        RT->UpdateResourceImmediate(true);
        FloorPanelTargets.Add(RT);
        P.Target = RT;
        UMaterialInterface* Material = Face->MakeMaterial ? Face->MakeMaterial(RT) : nullptr;
        if (auto* MID = Cast<UMaterialInstanceDynamic>(Material))
        {
            FloorPanelMaterials.Add(MID);
            MID->SetVectorParameterValue(TEXT("ScreenUVTransformX"), FLinearColor(P.Geometry.X.X, P.Geometry.X.Y, P.Geometry.X.Z, 0));
            MID->SetVectorParameterValue(TEXT("ScreenUVTransformY"), FLinearColor(P.Geometry.Y.X, P.Geometry.Y.Y, P.Geometry.Y.Z, 0));
        }
        else
        {
            UE_LOG(LogSCFloorPanel, Warning, TEXT("FLOOR_PANEL_BUILD panel=%s: no screen instance for the face; not drawn, E sends nothing"), *Panel.EntityId);
            continue;
        }
        // The face joins the screen faces and takes the drawn image through the general face path (I1-5).
        FSCScreenFace& Screen = ScreenFaces.AddDefaulted_GetRef();
        Screen.Component = Component;
        Screen.Slot = Face->Slot;
        Screen.Panel = AnchorKey;
        Screen.Default = Component->GetMaterial(Face->Slot);
        Screen.Image = TEXT("floor-panel");
        P.Face = ScreenFaces.Num() - 1;
        SetScreenFaceMaterial(P.Face, Material);
        Component->ComponentTags.AddUnique(FSCInstancing::KeepTag);
        TArray<FString> Rows;
        for (int32 R = 0; R < P.Layout.Options.Num(); ++R)
        {
            const FSCFloorPanelOption& O = P.Layout.Options[R];
            Rows.Add(FString::Printf(TEXT("%d:%s(%s)%s"), O.Stop, *O.Label, *O.Interaction.ToString(), P.Masks[R] ? TEXT("") : TEXT("[engine font]")));
        }
        UE_LOG(LogSCFloorPanel, Display,
               TEXT("FLOOR_PANEL_BUILD panel=%s lift=%d face=%d component=%s slot=%d size_cm=%.1fx%.1f fit_error=%.4f target=%dx%d style=%s font=%s rows=%s"),
               *P.EntityId, LiftIndex, P.Face, *Component->GetName(), P.Slot, P.Geometry.Width, P.Geometry.Height, P.Geometry.FitError, RT->SizeX, RT->SizeY,
               P.Colours.bFromData ? *Lift.PanelStyle.StyleRecord : TEXT("none(white-on-black)"),
               Lift.PanelStyle.FontStyle.IsEmpty() ? TEXT("engine") : *Lift.PanelStyle.FontStyle, *FString::Join(Rows, TEXT(" ")));
        FloorPanels.Add(MoveTemp(P));
    }
    RefreshFloorPanels();
}

bool ASCVehicle::AimFloorPanel(FVector Eye, FVector Direction, float Reach, const AActor* Ignore, int32& OutPanel, int32& OutOption, FVector2D* OutContent) const
{
    OutPanel = OutOption = INDEX_NONE;
    const FVector Dir = Direction.GetSafeNormal();
    double Best = Reach;
    FVector BestPoint;
    FVector2D BestContent;
    FVector2f BestUV;
    for (int32 I = 0; I < FloorPanels.Num(); ++I)
    {
        const FSCFloorPanel& P = FloorPanels[I];
        const UMeshComponent* C = P.Component.Get();
        if (!C || !C->IsVisible()) continue;
        const FTransform T = P.Frame();
        const FVector LocalEye = T.InverseTransformPosition(Eye);
        const FVector LocalRay = T.InverseTransformPosition(Eye + Dir * Reach) - LocalEye;
        const double LocalReach = LocalRay.Size();
        double Distance;
        FVector2f UV;
        if (LocalReach <= 0 || !P.Geometry.Raycast(LocalEye, LocalRay / LocalReach, LocalReach, Distance, UV)) continue;
        const FVector Point = T.TransformPosition(LocalEye + LocalRay / LocalReach * Distance);
        const double World = FVector::Dist(Eye, Point);
        if (World >= Best) continue;
        Best = World;
        BestPoint = Point;
        BestContent = P.Geometry.ContentFromUV(UV);
        BestUV = UV;
        OutPanel = I;
    }
    if (OutPanel == INDEX_NONE) return false;
    // Nothing that answers a trace in front of the face, the panel's own housing included (a bezel over the screen at a
    // grazing angle); the trace stops half a centimetre short of the face, so the face itself does not count.
    if (UWorld* World = GetWorld())
    {
        FHitResult Hit;
        FCollisionQueryParams Params(SCENE_QUERY_STAT(SCFloorPanelAim), true, Ignore);
        if (World->LineTraceSingleByChannel(Hit, Eye, BestPoint - Dir * 0.5, ECC_Visibility, Params))
        {
            OutPanel = INDEX_NONE;
            return false;
        }
    }
    // Options are in the panel's own space: content (our floor list) or the render's image (a game panel's nodes).
    const FSCFloorPanel& Hit = FloorPanels[OutPanel];
    OutOption = Hit.Layout.OptionAt(Hit.OptionPoint(BestContent, BestUV));
    if (OutContent) *OutContent = BestContent;
    return true;
}

void ASCVehicle::SetFloorPanelHighlight(int32 Panel, int32 Option)
{
    for (int32 I = 0; I < FloorPanels.Num(); ++I) FloorPanels[I].Highlight = I == Panel ? Option : INDEX_NONE;
}

int32 ASCVehicle::RefreshFloorPanels()
{
    int32 Drawn = 0;
    const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0;
    for (int32 I = 0; I < FloorPanels.Num(); ++I)
    {
        FSCFloorPanel& P = FloorPanels[I];
        FSCFloorPanelView V;
        if (P.Kind == ESCPanelKind::Buttons)
        {
            V.Highlight = P.Highlight;
            V.Image = P.ImageForState();
            // An export with hover renders (cache format 43) gives the game's look of the aimed node: its render, or none
            // when hovering changes nothing there (the active row); either way not our frame.
            V.bHoverImage = P.Layout.Options.IsValidIndex(P.Highlight) && !P.HoverImages.IsEmpty();
            if (const FString Hover = P.HoverImageFor(V.Image, P.Highlight); !Hover.IsEmpty()) V.Image = Hover;
        }
        else if (Lifts.IsValidIndex(P.Lift)) V = FSCFloorPanelView::Of(*Lifts[P.Lift], P.Highlight, Now);
        else continue;
        if (P.bDrawn && V == P.Shown) continue;
        if (!DrawPanel(P, V, GetWorld())) continue; // tried again on the next refresh
        FSCFloorPanelView Steady = V, Before = P.Shown;
        Steady.bBlink = Before.bBlink = false;
        if (!P.bDrawn || !(Steady == Before)) // the blink alone is not logged
            UE_LOG(LogSCFloorPanel, Display, TEXT("FLOOR_PANEL_VIEW panel=%d %s%s lift_state=%s t=%.3f"), I, *V.Describe(),
                   V.Image.IsEmpty() ? TEXT("") : *(TEXT(" image=") + FPaths::GetBaseFilename(V.Image)),
                   Lifts.IsValidIndex(P.Lift) ? LexToString(Lifts[P.Lift]->State) : TEXT("-"), Now);
        P.Shown = V;
        P.bDrawn = true;
        ++Drawn;
    }
    return Drawn;
}

int32 ASCVehicle::PressFloorPanel(int32 Panel, int32 Option)
{
    if (!FloorPanels.IsValidIndex(Panel) || !FloorPanels[Panel].Layout.Options.IsValidIndex(Option)) return 0;
    if (FloorPanels[Panel].Kind == ESCPanelKind::Buttons) return PressButtonPanel(Panel, Option); // a game panel (SCLightControl.cpp)
    const FSCFloorPanel& P = FloorPanels[Panel];
    const FSCFloorPanelOption& O = P.Layout.Options[Option];
    const int32 Handled = SendInteractionIn(P.Interior, P.EntityId, O.Interaction);
    UE_LOG(LogSCFloorPanel, Display, TEXT("FLOOR_PANEL_PRESS panel=%d entity=%s option=%d stop=%d label=\"%s\" interaction=\"%s\" handled=%d"), Panel,
           *P.EntityId, Option, O.Stop, *O.Label, *O.Interaction.ToString(), Handled);
    RefreshFloorPanels();
    return Handled;
}

int32 ASCVehicle::FloorPanelOf(const FString& EntityId) const
{
    return FloorPanels.IndexOfByPredicate([&](const FSCFloorPanel& P) { return P.EntityId == EntityId; });
}

bool ASCVehicle::FloorPanelWorldPoint(int32 Panel, int32 Option, FVector& Out) const
{
    if (!FloorPanels.IsValidIndex(Panel) || !FloorPanels[Panel].Layout.Options.IsValidIndex(Option)) return false;
    const FSCFloorPanel& P = FloorPanels[Panel];
    const UMeshComponent* C = P.Component.Get();
    if (!C) return false;
    Out = P.Frame().TransformPosition(P.Geometry.PointAt(P.ContentOf(P.Layout.Options[Option].Rect.GetCenter())));
    return true;
}

FString FSCFloorPanel::ImageForState() const
{
    // The owner's current non-default states, and the render that pins the most of them without pinning another.
    TArray<FString> Current;
    for (const FSCUiStateType& T : StateTypes)
        if (const FString* S = UiState.Find(T.Name); S && *S != T.DefaultState) Current.Add(T.Name + TEXT(".") + *S);
    FString Best = DefaultImage;
    int32 BestPins = 0;
    for (const FSCUiStateImage& I : StateImages)
        if (I.States.Num() > BestPins && !I.States.ContainsByPredicate([&](const FString& S) { return !Current.Contains(S); }))
        {
            Best = I.ImagePath;
            BestPins = I.States.Num();
        }
    return Best;
}

FString FSCFloorPanel::HoverImageFor(const FString& Image, int32 Option) const
{
    if (!Layout.Options.IsValidIndex(Option) || Layout.Options[Option].Node.IsEmpty()) return FString();
    // The render shown: the default (no pins) or a state image (its pins); its hovered form pins the same states.
    TArray<FString> Pins;
    if (Image != DefaultImage)
    {
        const FSCUiStateImage* S = StateImages.FindByPredicate([&](const FSCUiStateImage& I) { return I.ImagePath == Image; });
        if (!S) return FString();
        Pins = S->States;
    }
    const FString& Node = Layout.Options[Option].Node;
    const FSCUiHoverImage* H = HoverImages.FindByPredicate([&](const FSCUiHoverImage& I) { return I.Node == Node && I.States == Pins; });
    return H ? H->ImagePath : FString();
}

FString FSCPanelAim::DoorButtonOf(const TArray<FSCUiButton>& Buttons, const TArray<FSCUiStateType>& StateTypes)
{
    for (const FSCUiButton& B : Buttons)
        if (!StateTypes.ContainsByPredicate([&](const FSCUiStateType& T) {
                return T.Transitions.ContainsByPredicate([&](const FSCUiStateType::FTransition& X) { return X.Interaction == B.Interaction; });
            }))
            return B.Node;
    return FString();
}

FTransform FSCPanelAim::Frame() const
{
    const UMeshComponent* C = Component.Get();
    return C ? FTransform(C->GetComponentQuat(), C->GetComponentLocation()) : FTransform::Identity;
}

void ASCVehicle::BuildButtonPanels(TArray<FSCButtonFace>& Faces, TFunctionRef<UTexture2D*(const FString&)> Texture)
{
    // The highlight frame takes the pads' Selected role (the vehicle's door-pad palette, `lifts[].panel_style`: the brand
    // style the panel renders with too) when the export has one; white otherwise.
    FLinearColor Frame = FLinearColor::White;
    for (const TSharedPtr<FSCLift>& Lift : Lifts)
        if (Lift->PanelStyle.Palette.Contains(TEXT("Selected"))) { Frame = FSCFloorPanelColours::From(Lift->PanelStyle).RowHighlight; break; }
    int32 NotModelled = 0;
    for (FSCButtonFace& F : Faces)
    {
        UMeshComponent* Component = F.Component.Get();
        if (!Component || F.ImageSizePx.X <= 0 || F.ImageSizePx.Y <= 0 || F.Buttons.IsEmpty()) continue;
        // WP-I1-4: a face whose hover renders are swapped on it (a door pad, a call panel, a kiosk: not drawn as a panel)
        // is aimed at by its buttons. Off every button, a door pad's door button is what E presses: the button whose
        // interaction sets none of the pad's own states (its open/close acts on the door, whose state comes back by
        // link; the lock and light buttons set the pad's lock and light states). A face with one node: that node.
        auto AddAim = [&](bool bDoorPad) {
            const int32 Face = ScreenFaces.IndexOfByPredicate([&](const FSCScreenFace& S) { return S.Component.Get() == Component && S.Slot == F.Slot; });
            if (Face == INDEX_NONE || ScreenFaces[Face].Hovers.IsEmpty()) return;
            FSCPanelAim Aim;
            Aim.Component = Component;
            Aim.Face = Face;
            Aim.Reach = PanelReachOf(F.Panel.Get());
            Aim.Geometry = F.Geometry.Scaled(Component->GetComponentScale());
            if (!Aim.Geometry.Build(Aim.Frame().InverseTransformVectorNoScale(GetActorUpVector()))) return;
            FBox Box(ForceInit);
            for (const FVector3f& Corner : Aim.Geometry.Positions) Box += FVector(Corner);
            Aim.Centre = Box.GetCenter();
            Aim.Radius = Box.GetExtent().Size();
            FVector2D Mean = FVector2D::ZeroVector;
            for (const FVector2f& UV : Aim.Geometry.UVs) Mean += FVector2D(UV.X, UV.Y) / Aim.Geometry.UVs.Num();
            Aim.UVBase = FVector2D(FMath::FloorToDouble(Mean.X), FMath::FloorToDouble(Mean.Y));
            TSet<FString> Nodes;
            for (const FSCUiButton& B : F.Buttons)
            {
                Aim.Buttons.Add({B.Node, B.Interaction, FBox2D(B.RectPx.Min / F.ImageSizePx, B.RectPx.Max / F.ImageSizePx)});
                Nodes.Add(B.Node);
            }
            if (bDoorPad) Aim.Fallback = FSCPanelAim::DoorButtonOf(F.Buttons, F.StateTypes);
            else if (Nodes.Num() == 1) Aim.Fallback = F.Buttons[0].Node;
            PanelAims.Add(MoveTemp(Aim));
        };
        if (DoorForPanel(F.Panel.Get()))
        {
            AddAim(true);
            if (F.bHideDoorLockControl) continue;
            FSCFloorPanel LockFace;
            LockFace.Anchor = const_cast<USceneComponent*>(F.Panel.Get());
            LockFace.Component = Component;
            LockFace.StateTypes = F.StateTypes;
            LockFace.Geometry = F.Geometry.Scaled(Component->GetComponentScale());
            if (!LockFace.Geometry.Build(LockFace.Frame().InverseTransformVectorNoScale(GetActorUpVector()))) continue;
            FVector2D Mean = FVector2D::ZeroVector;
            for (const FVector2f& UV : LockFace.Geometry.UVs) Mean += FVector2D(UV.X, UV.Y) / LockFace.Geometry.UVs.Num();
            LockFace.UVBase = FVector2D(FMath::FloorToDouble(Mean.X), FMath::FloorToDouble(Mean.Y));
            for (const FSCUiButton& B : F.Buttons)
                if (B.Interaction == FName(TEXT("LockDoor")) || B.Interaction == FName(TEXT("UnlockDoor")))
                    LockFace.Layout.Options.Add({INDEX_NONE, B.Interaction, B.Interaction.ToString(), FBox2D(B.RectPx.Min / F.ImageSizePx, B.RectPx.Max / F.ImageSizePx)});
            if (!LockFace.Layout.Options.IsEmpty()) DoorPanelLocks.Add(MoveTemp(LockFace));
            continue;
        }
        // Operated only where every node's press does what the game does: it sends a tag a light group the owner drives
        // receives (a light panel). A door pad (its door button is the door interaction, FindInteraction, and its face
        // follows the door), kiosks and the rest stay the exporter's render.
        const int32 Interior = F.Interior != INDEX_NONE ? F.Interior : Interactions.InteriorOf(F.EntityId);
        const TArray<int32> Driven = LightGroupsLinkedBy(Interior, F.Links);
        const bool bLightPanel = !Driven.IsEmpty() && !F.Buttons.ContainsByPredicate([&](const FSCUiButton& B) {
            const FSCSendableInteraction* S = F.Sendables.FindByPredicate([&](const FSCSendableInteraction& X) { return X.Name == B.Interaction; });
            return !S || !Driven.ContainsByPredicate([&](int32 G) {
                return LightGroups[G].Interactions.ContainsByPredicate([&](const FSCLightGroupInteraction& I) { return I.LinkingTag == S->LinkingTag; });
            });
        });
        if (!bLightPanel)
        {
            ++NotModelled;
            AddAim(false);
            continue;
        }
        FSCFloorPanel P;
        P.Kind = ESCPanelKind::Buttons;
        P.EntityId = F.EntityId;
        P.Interior = Interior;
        P.Anchor = const_cast<USceneComponent*>(F.Panel.Get());
        P.Component = Component;
        P.Slot = F.Slot;
        P.Geometry = F.Geometry.Scaled(Component->GetComponentScale());
        if (!P.Geometry.Build(P.Frame().InverseTransformVectorNoScale(GetActorUpVector())))
        {
            UE_LOG(LogSCFloorPanel, Warning, TEXT("LIGHT_PANEL_BUILD panel=%s: the screen face is degenerate; not operable"), *F.EntityId);
            continue;
        }
        // The render covers one UV cell of the face (the face samples it at UV0, wrapping): the cell its UVs lie in.
        FVector2D Mean = FVector2D::ZeroVector;
        for (const FVector2f& UV : P.Geometry.UVs) Mean += FVector2D(UV.X, UV.Y) / P.Geometry.UVs.Num();
        P.UVBase = FVector2D(FMath::FloorToDouble(Mean.X), FMath::FloorToDouble(Mean.Y));
        for (const FSCUiButton& B : F.Buttons)
            P.Layout.Options.Add({INDEX_NONE, B.Interaction, B.Label.IsEmpty() ? B.Interaction.ToString() : B.Label,
                                  FBox2D(B.RectPx.Min / F.ImageSizePx, B.RectPx.Max / F.ImageSizePx), B.Node});
        P.StateTypes = F.StateTypes;
        P.HoverImages = F.HoverImages;
        P.StateImages = F.StateImages;
        P.DefaultImage = F.DefaultImage;
        for (const FSCUiStateType& T : P.StateTypes) P.UiState.Add(T.Name, T.DefaultState);
        P.Sendables = F.Sendables;
        P.Links = F.Links;
        P.HighlightColour = Frame;
        P.Images.Add(P.DefaultImage, Texture(P.DefaultImage));
        for (const FSCUiStateImage& I : P.StateImages) P.Images.Add(I.ImagePath, Texture(I.ImagePath));
        for (const FSCUiHoverImage& I : P.HoverImages) P.Images.Add(I.ImagePath, Texture(I.ImagePath));
        UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(this, NAME_None, RF_Transient);
        RT->RenderTargetFormat = RTF_RGBA16f;
        RT->ClearColor = FLinearColor::Black;
        RT->AddressX = RT->AddressY = TA_Wrap; // as the face samples the render
        const double Fit = FMath::Min(1.0, PanelPixelsMax / FMath::Max(F.ImageSizePx.X, F.ImageSizePx.Y));
        RT->InitAutoFormat(FMath::Clamp(FMath::RoundToInt(F.ImageSizePx.X * Fit), 16, PanelPixelsMax),
                           FMath::Clamp(FMath::RoundToInt(F.ImageSizePx.Y * Fit), 16, PanelPixelsMax));
        RT->UpdateResourceImmediate(true);
        FloorPanelTargets.Add(RT);
        P.Target = RT;
        UMaterialInterface* Material = F.MakeMaterial ? F.MakeMaterial(RT) : nullptr;
        if (!Material)
        {
            UE_LOG(LogSCFloorPanel, Warning, TEXT("LIGHT_PANEL_BUILD panel=%s: no screen instance for the face; not operable"), *F.EntityId);
            continue;
        }
        FloorPanelMaterials.Add(Material);
        // The face is already a screen face (its state images): the drawn render replaces them there.
        P.Face = ScreenFaces.IndexOfByPredicate([&](const FSCScreenFace& S) { return S.Component.Get() == Component && S.Slot == F.Slot; });
        if (P.Face == INDEX_NONE)
        {
            FSCScreenFace& Screen = ScreenFaces.AddDefaulted_GetRef();
            Screen.Component = Component;
            Screen.Slot = F.Slot;
            Screen.Panel = const_cast<USceneComponent*>(F.Panel.Get());
            Screen.Default = Component->GetMaterial(F.Slot);
            Screen.Image = F.DefaultImage;
            P.Face = ScreenFaces.Num() - 1;
        }
        SetScreenFaceMaterial(P.Face, Material);
        Component->ComponentTags.AddUnique(FSCInstancing::KeepTag);
        TArray<FString> Groups, Rows;
        for (const int32 G : LightGroupsLinkedBy(P.Interior, P.Links)) Groups.Add(LightGroups[G].Name);
        for (const FSCFloorPanelOption& O : P.Layout.Options)
            Rows.Add(FString::Printf(TEXT("%s[%.3f,%.3f..%.3f,%.3f]"), *O.Interaction.ToString(), O.Rect.Min.X, O.Rect.Min.Y, O.Rect.Max.X, O.Rect.Max.Y));
        UE_LOG(LogSCFloorPanel, Display, TEXT("LIGHT_PANEL_BUILD panel=%s index=%d interior=%d face=%d component=%s slot=%d size_cm=%.1fx%.1f uv_base=%s target=%dx%d groups=%s options=%s"),
               *P.EntityId, FloorPanels.Num(), P.Interior, P.Face, *Component->GetName(), P.Slot, P.Geometry.Width, P.Geometry.Height,
               *P.UVBase.ToString(), RT->SizeX, RT->SizeY, Groups.IsEmpty() ? TEXT("-") : *FString::Join(Groups, TEXT(",")), *FString::Join(Rows, TEXT(" ")));
        FloorPanels.Add(MoveTemp(P));
    }
    if (!Faces.IsEmpty())
        UE_LOG(LogSCFloorPanel, Display, TEXT("LIGHT_PANELS faces=%d operable=%d not_modelled=%d (a node whose press reaches none of the light groups the owner drives)"), Faces.Num(),
               FloorPanels.FilterByPredicate([](const FSCFloorPanel& P) { return P.Kind == ESCPanelKind::Buttons; }).Num(), NotModelled);
    if (!Faces.IsEmpty())
        UE_LOG(LogSCFloorPanel, Display, TEXT("PANEL_HOVER_FACES aims=%d light_panels_with_hover=%d (WP-I1-4: faces swapped to the game's hover render under the crosshair)"),
               PanelAims.Num(), FloorPanels.FilterByPredicate([](const FSCFloorPanel& P) { return !P.HoverImages.IsEmpty(); }).Num());
    RefreshFloorPanels();
}
