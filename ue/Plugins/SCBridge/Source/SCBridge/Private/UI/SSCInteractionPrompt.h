// WP-I1-4: the first-person interaction popout, in the world-space callouts' look (SSCCalloutLayer, WP-K1c): a leader
// from the crosshair to a paper placard with the thing's name (when the data names it), its actions and the E key cap on
// the one E performs; with several actions the wheel moves the cap (ASCFirstPersonCharacter::GetPrompt). Draw-only.
#pragma once
#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"

class USCViewerSubsystem;

class SSCInteractionPrompt : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SSCInteractionPrompt) {}
    SLATE_ARGUMENT(USCViewerSubsystem*, Subsystem)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args);
    virtual void Tick(const FGeometry& Geometry, double Time, float Delta) override;
    virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& Culling, FSlateWindowElementList& Out, int32 Layer,
                          const FWidgetStyle& Style, bool bParentEnabled) const override;
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }

private:
    TWeakObjectPtr<USCViewerSubsystem> Subsystem;
    double Now = 0, Born = -1; // the time the shown prompt's thing came under the crosshair (the placard's fade-in)
    FString ShownKey;
};
