// The mobiGlas (WP-K1c): F1 opens it over the 3D view in any camera mode; Esc / F1 close it. Layout after the mock
// (docs/cargo/mobiglas-mock.html): a header band, the live view in the upper part, and the Cargo app's four panels
// (Nearby grids, Grid detail, Transfer, Transfers) in the lower ~55 %. While open the cursor is free, the panels take
// clicks, and drag / wheel on the live view orbit and zoom a camera around the selected grid's ship or platform
// (FSCMobiGlasOrbit); the first-person player stands still and gets its own view back on close.
#pragma once
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "SCMobiGlasOrbit.h"

class FSCMobiGlasModel;

class SSCMobiGlas : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SSCMobiGlas) {}
    SLATE_ARGUMENT(TSharedPtr<FSCMobiGlasModel>, Model)
    /** Closed while it (or a child) had keyboard focus: the owner gives focus back to the view. */
    SLATE_EVENT(FSimpleDelegate, OnClosed)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args);
    virtual ~SSCMobiGlas() override;
    virtual void Tick(const FGeometry& Geometry, double Time, float Delta) override;
    virtual bool SupportsKeyboardFocus() const override { return true; }
    virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
    virtual FReply OnKeyUp(const FGeometry& Geometry, const FKeyEvent& Event) override;
    virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual void OnMouseCaptureLost(const FCaptureLostEvent& Event) override;
    virtual FCursorReply OnCursorQuery(const FGeometry& Geometry, const FPointerEvent& Event) const override;

private:
    TSharedRef<SWidget> MakeHeader();
    TSharedRef<SWidget> MakeRail();
    bool InLiveView(const FPointerEvent& Event) const;
    FBox FocusBox() const;
    /** Source -> destination of our active transfers (horizontal), for framing across the drones' route. */
    TOptional<FVector2D> WorkAxis() const;
    bool ForwardKey(const FKeyEvent& Event, bool bDown);

    TSharedPtr<FSCMobiGlasModel> Model;
    FSimpleDelegate OnClosed;
    TSharedPtr<SWidget> Clear; // the live view, handed to the callouts as their area while open
    FSCMobiGlasOrbit Orbit;
    enum class EDrag : uint8 { None, Rotate, Pan } Drag = EDrag::None;
    FName FocusGroup;
    int32 FocusJobs = 0; // our active transfers when last framed: a new one widens the framing to its work area
    float Openness = 0; // 0 closed .. 1 open (fade + slide)
    float FitScale = 1;
    bool bWasOpen = false;
};
