// SSCMobiGlas: the app shell (header band, live view, apps rail and the four panels in the lower band), its open /
// close motion, the live-view orbit (drag / wheel outside the panels) and the input rules while open.
#include "SSCMobiGlas.h"
#include "SSCMobiGlasPanels.h"
#include "SCMobiGlasModel.h"
#include "SCMobiGlasStyle.h"
#include "SCCargoViewSource.h"
#include "SCFirstPersonCharacter.h"
#include "SCViewerSubsystem.h"
#include "SCVehicle.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SCMobiGlas"
using namespace SCMobiGlasStyle;

namespace
{
/** Key cap as in the mock: caps label in a 1 px on-scrim frame. */
TSharedRef<SWidget> KeyCap(const FText& Text)
{
    return SNew(SBorder).BorderImage(White()).BorderBackgroundColor(OnScrim().CopyWithNewOpacity(0.8f)).Padding(1)
        [SNew(SBorder).BorderImage(White()).BorderBackgroundColor(ScrimSolid()).Padding(FMargin(8, 3))[Txt(Text, Label(), OnScrim())]];
}
} // namespace

void SSCMobiGlas::Construct(const FArguments& Args)
{
    Model = Args._Model;
    OnClosed = Args._OnClosed;
    SetVisibility(TAttribute<EVisibility>::CreateLambda([this] { return Model->IsOpen() || Openness > 0 ? EVisibility::Visible : EVisibility::Collapsed; }));
    SetRenderOpacity(0);
    ChildSlot
    [
        // Narrow views (side panels shown) scale the layout down instead of squeezing the panels.
        SNew(SDPIScaler).DPIScale_Lambda([this] { return FitScale; })
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [SNew(SBorder).BorderImage(White()).BorderBackgroundColor(Scrim()).Padding(FMargin(SafeMargin, 28, SafeMargin, 20))[MakeHeader()]]
            // The live view: no scrim, the world and its callouts; drag orbits, wheel zooms.
            + SVerticalBox::Slot().FillHeight(0.42f)[SAssignNew(Clear, SSpacer)]
            + SVerticalBox::Slot().FillHeight(0.58f)
            [
                SNew(SBorder).BorderImage(White()).BorderBackgroundColor(Scrim()).Padding(FMargin(SafeMargin, 12, SafeMargin, SafeMargin))
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight().Padding(RailWidth + Gutter, 0, 0, 12)
                    [Txt(TAttribute<FText>::CreateLambda([this] {
                         int32 Active = 0;
                         for (const FSCCargoJobView& J : Model->GetJobs()) Active += J.bMine && FSCMobiGlasModel::IsActive(J.State);
                         return FText::Format(LOCTEXT("LiveView", "LIVE VIEW  ·  DRAG ORBIT  ·  WHEEL ZOOM  ·  {0} ACTIVE {0}|plural(one=TRANSFER,other=TRANSFERS)"), Active);
                     }), Label(), OnScrim().CopyWithNewOpacity(0.72f))]
                    + SVerticalBox::Slot().FillHeight(1)
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().AutoWidth()[MakeRail()]
                        + SHorizontalBox::Slot().FillWidth(320).Padding(Gutter, 0, 0, 0)[SNew(SSCMobiGlasNearby).Model(Model)]
                        + SHorizontalBox::Slot().FillWidth(416).Padding(Gutter, 0, 0, 0)[SNew(SSCMobiGlasDetail).Model(Model)]
                        + SHorizontalBox::Slot().FillWidth(520).Padding(Gutter, 0, 0, 0)[SNew(SSCMobiGlasTransfer).Model(Model)]
                        + SHorizontalBox::Slot().FillWidth(400).Padding(Gutter, 0, 0, 0)[SNew(SSCMobiGlasJobs).Model(Model)]
                    ]
                ]
            ]
        ]
    ];
}

SSCMobiGlas::~SSCMobiGlas() { Orbit.End(); }

TSharedRef<SWidget> SSCMobiGlas::MakeHeader()
{
    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Txt(LOCTEXT("MobiGlas", "MOBIGLAS"), Display(), OnScrim())]
        + SHorizontalBox::Slot().AutoWidth().Padding(Pad + 8, 0)
          [SNew(SBox).WidthOverride(RuleWidth)[SNew(SBorder).BorderImage(White()).BorderBackgroundColor(OnScrim().CopyWithNewOpacity(0.3f))]]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()[Txt(LOCTEXT("Network", "LOGISTICS / LOCAL NETWORK"), Label(), OnScrim().CopyWithNewOpacity(0.72f))]
            + SVerticalBox::Slot().AutoHeight()[Txt(LOCTEXT("CargoApp", "CARGO OPERATIONS"), Title(), OnScrim())]
        ]
        + SHorizontalBox::Slot().FillWidth(1)[SNew(SSpacer)]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, Pad, 0)
        [
            SNew(SBox).Visibility_Lambda([this] { return Model->IsPreview() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
            [Chip(LOCTEXT("PreviewData", "UI PREVIEW DATA · NOT GAME DATA"), FSlateColor(Warn()))]
        ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, Pad, 0)
        [Txt(TAttribute<FText>::CreateLambda([this] {
             if (!Model->HasSource()) return LOCTEXT("NoCargo", "NO CARGO SYSTEM");
             return FText::Format(LOCTEXT("Status", "{0} GRIDS · {1} DRONES"), Model->GetGrids().Num(), Model->GetDrones().Num());
         }), Label(), OnScrim())]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)[KeyCap(LOCTEXT("F1Close", "F1 CLOSE"))]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[KeyCap(LOCTEXT("EscClose", "ESC CLOSE"))];
}

TSharedRef<SWidget> SSCMobiGlas::MakeRail()
{
    // One app exists (Cargo); no placeholder apps.
    return SNew(SBox).WidthOverride(RailWidth)
    [
        SNew(SBorder).BorderImage(White()).BorderBackgroundColor(OnScrim().CopyWithNewOpacity(0.3f)).Padding(1)
        [
            SNew(SBorder).BorderImage(White()).BorderBackgroundColor(ScrimSolid()).Padding(0)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0, 14)[Txt(LOCTEXT("Apps", "APPS"), Label(), OnScrim())]
                + SVerticalBox::Slot().AutoHeight()
                [SNew(SBorder).BorderImage(White()).BorderBackgroundColor(Paper()).HAlign(HAlign_Center).Padding(FMargin(0, 22))
                     [Txt(LOCTEXT("Cargo", "CARGO"), Label(), Ink())]]
            ]
        ]
    ];
}

bool SSCMobiGlas::InLiveView(const FPointerEvent& Event) const
{
    return Clear && Clear->GetTickSpaceGeometry().IsUnderLocation(Event.GetScreenSpacePosition());
}

FBox SSCMobiGlas::FocusBox() const
{
    // The selected grid's group (the ship, the elevator platform) and the drones' work around it: boxes in the air and
    // drones away from their dock. Else the player.
    FBox Box(ForceInit);
    TArray<int32> Ids;
    const FSCCargoGridView* Selected = Model->FindGrid(Model->GetSelectedGrid());
    if (Selected) Ids = Model->Resolve(FSCGridPick{Selected->GroupKey});
    // A ship's grids are its cargo plates: frame the whole ship around them.
    if (Selected && Selected->Kind == ESCCargoGridKind::Vehicle && Model->GetViewer() && Model->GetViewer()->GetVehicle())
        Box += Model->GetViewer()->GetVehicle()->ExplorationBounds();
    // The drones' work: the source and destination grids of our active transfers.
    for (const FSCCargoJobView& J : Model->GetJobs())
        if (J.bMine && FSCMobiGlasModel::IsActive(J.State)) { Ids.Append(J.FromGrids); Ids.Append(J.ToGrids); }
    for (int32 Id : Ids)
        if (const FSCCargoGridView* G = Model->FindGrid(Id); G && G->WorldBounds.IsValid) Box += G->WorldBounds;
    for (const FSCCargoBoxView& B : Model->GetMovingBoxes()) if (B.WorldBounds.IsValid) Box += B.WorldBounds;
    for (const FSCCargoDroneView& D : Model->GetDrones())
        if (D.State != ESCCargoDroneState::Docked && D.WorldBounds.IsValid) Box += D.WorldBounds;
    if (!Box.IsValid)
        if (ASCFirstPersonCharacter* C = Model->GetViewer() ? Model->GetViewer()->GetFirstPerson() : nullptr)
            Box = FBox::BuildAABB(C->GetActorLocation(), FVector(150));
    return Box;
}


TOptional<FVector2D> SSCMobiGlas::WorkAxis() const
{
    FVector From = FVector::ZeroVector, To = FVector::ZeroVector;
    int32 NFrom = 0, NTo = 0;
    for (const FSCCargoJobView& J : Model->GetJobs())
    {
        if (!J.bMine || !FSCMobiGlasModel::IsActive(J.State)) continue;
        for (int32 Id : J.FromGrids) if (const FSCCargoGridView* G = Model->FindGrid(Id)) { From += G->WorldBounds.GetCenter(); ++NFrom; }
        for (int32 Id : J.ToGrids) if (const FSCCargoGridView* G = Model->FindGrid(Id)) { To += G->WorldBounds.GetCenter(); ++NTo; }
    }
    if (!NFrom || !NTo) return TOptional<FVector2D>();
    const FVector D = To / NTo - From / NFrom;
    return FVector2D(D.X, D.Y);
}
void SSCMobiGlas::Tick(const FGeometry& Geometry, double Time, float Delta)
{
    SCompoundWidget::Tick(Geometry, Time, Delta);
    Model->Refresh(Delta);
    FitScale = FMath::Clamp(float(Geometry.GetLocalSize().X) / MinLayoutWidth, 0.5f, 1.f);
    const bool bOpen = Model->IsOpen();
    if (Clear && Geometry.GetAbsoluteSize().Y > 1)
    {
        // The live view's share of the view height: the orbit frames its focus into it.
        const FGeometry& Live = Clear->GetTickSpaceGeometry();
        const double Top = (Live.GetAbsolutePosition().Y - Geometry.GetAbsolutePosition().Y) / Geometry.GetAbsoluteSize().Y;
        Orbit.SetBand(FVector2D(Top, Top + Live.GetAbsoluteSize().Y / Geometry.GetAbsoluteSize().Y));
    }
    if (bOpen && !bWasOpen)
    {
        if (FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().ReleaseAllPointerCapture(); // free the cursor
            // Keys come here while open (F1 / Esc close in every camera mode); the view gets focus back on close.
            FSlateApplication::Get().SetAllUserFocus(SharedThis(this), EFocusCause::SetDirectly);
        }
        const FSCCargoGridView* Selected = Model->FindGrid(Model->GetSelectedGrid());
        FocusGroup = Selected ? Selected->GroupKey : NAME_None;
        FocusJobs = 0;
        for (const FSCCargoJobView& J : Model->GetJobs()) FocusJobs += J.bMine && FSCMobiGlasModel::IsActive(J.State);
        Orbit.SetWorkAxis(WorkAxis());
        Orbit.Begin(Model->GetViewer(), FocusBox());
    }
    if (!bOpen && bWasOpen)
    {
        Orbit.End();
        Drag = EDrag::None;
        if (HasFocusedDescendants() || HasKeyboardFocus()) OnClosed.ExecuteIfBound();
    }
    bWasOpen = bOpen;
    if (bOpen)
    {
        // Selecting a grid of another ship / platform, or a transfer starting or ending, re-frames the orbit.
        const FSCCargoGridView* Selected = Model->FindGrid(Model->GetSelectedGrid());
        int32 Jobs = 0;
        for (const FSCCargoJobView& J : Model->GetJobs()) Jobs += J.bMine && FSCMobiGlasModel::IsActive(J.State);
        if ((Selected && Selected->GroupKey != FocusGroup) || Jobs != FocusJobs)
        {
            FocusGroup = Selected ? Selected->GroupKey : NAME_None;
            FocusJobs = Jobs;
            Orbit.SetWorkAxis(WorkAxis());
            Orbit.Refocus(FocusBox());
        }
        FSCMobiGlasRequests& R = FSCMobiGlasRequests::Get(); // route equivalents of drag / wheel
        if (!R.OrbitPixels.IsZero()) Orbit.Rotate(R.OrbitPixels);
        if (R.ZoomNotches != 0) Orbit.Zoom(R.ZoomNotches);
        Orbit.Sync(); // an outside teleport / view command while open is shown, never left stale
        R.OrbitPixels = FVector2D::ZeroVector;
        R.ZoomNotches = 0;
    }
    Openness = bOpen ? FMath::Min(1.f, Openness + Delta / PanelIn) : FMath::Max(0.f, Openness - Delta / PanelOut);
    const float Shown = bOpen ? EaseOutCubic(Openness) : 1 - EaseInQuad(1 - Openness);
    SetRenderOpacity(Shown);
    SetRenderTransform(FSlateRenderTransform(FVector2f(0, PanelSlide * (1 - Shown))));
    if (bOpen && Clear)
    {
        const FGeometry& Gap = Clear->GetTickSpaceGeometry();
        Model->SetClearArea(FSlateRect(Gap.GetAbsolutePosition(), Gap.GetAbsolutePosition() + Gap.GetAbsoluteSize()));
    }
    else
        Model->SetClearArea(TOptional<FSlateRect>());
}

bool SSCMobiGlas::ForwardKey(const FKeyEvent& Event, bool bDown)
{
    // The player stands still while the mobiGlas is up (the character ignores movement presses); F1 / Esc close it.
    const FKey Key = Event.GetKey();
    ASCFirstPersonCharacter* C = Model->GetViewer() ? Model->GetViewer()->GetFirstPerson() : nullptr;
    if (C) return C->HandleKey(Key, bDown);
    if (bDown && (Key == EKeys::F1 || Key == EKeys::Escape))
    {
        Model->SetOpen(false);
        return true;
    }
    return false;
}
FReply SSCMobiGlas::OnKeyDown(const FGeometry&, const FKeyEvent& Event)
{
    if (Event.IsRepeat()) return FReply::Handled();
    return ForwardKey(Event, true) ? FReply::Handled() : FReply::Unhandled();
}
FReply SSCMobiGlas::OnKeyUp(const FGeometry&, const FKeyEvent& Event)
{
    return ForwardKey(Event, false) ? FReply::Handled() : FReply::Unhandled();
}

FReply SSCMobiGlas::OnMouseButtonDown(const FGeometry&, const FPointerEvent& Event)
{
    // Panels handle their own clicks; what reaches here is the live view (orbit) or empty band space (swallowed).
    if (!InLiveView(Event)) return FReply::Handled();
    const FKey Button = Event.GetEffectingButton();
    const bool bPan = Button == EKeys::MiddleMouseButton || (Button == EKeys::LeftMouseButton && Event.IsShiftDown());
    if (!bPan && Button != EKeys::LeftMouseButton && Button != EKeys::RightMouseButton) return FReply::Handled();
    Drag = bPan ? EDrag::Pan : EDrag::Rotate;
    return FReply::Handled().CaptureMouse(AsShared());
}
FReply SSCMobiGlas::OnMouseButtonUp(const FGeometry&, const FPointerEvent&)
{
    if (Drag == EDrag::None) return FReply::Handled();
    Drag = EDrag::None;
    return FReply::Handled().ReleaseMouseCapture();
}
FReply SSCMobiGlas::OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event)
{
    if (Drag == EDrag::None) return FReply::Unhandled();
    const FVector2D Delta = Event.GetCursorDelta();
    if (Drag == EDrag::Rotate)
        Orbit.Rotate(Delta);
    else if (Clear)
        Orbit.Pan(Delta / FMath::Max(1.0, double(Clear->GetTickSpaceGeometry().GetAbsoluteSize().Y)));
    return FReply::Handled();
}
FReply SSCMobiGlas::OnMouseWheel(const FGeometry&, const FPointerEvent& Event)
{
    if (InLiveView(Event)) Orbit.Zoom(Event.GetWheelDelta());
    return FReply::Handled();
}
void SSCMobiGlas::OnMouseCaptureLost(const FCaptureLostEvent& Event)
{
    SCompoundWidget::OnMouseCaptureLost(Event);
    Drag = EDrag::None;
}
FCursorReply SSCMobiGlas::OnCursorQuery(const FGeometry&, const FPointerEvent& Event) const
{
    if (Drag != EDrag::None) return FCursorReply::Cursor(Drag == EDrag::Pan ? EMouseCursor::GrabHandClosed : EMouseCursor::CardinalCross);
    return FCursorReply::Cursor(EMouseCursor::Default);
}

#undef LOCTEXT_NAMESPACE
