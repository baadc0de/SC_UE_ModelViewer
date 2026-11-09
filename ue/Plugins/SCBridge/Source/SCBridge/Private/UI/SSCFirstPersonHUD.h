// In-game exploration overlay and teleport picker (Slate, shared with the standalone host).
#pragma once
#include "Widgets/SCompoundWidget.h"
class USCViewerSubsystem;
class SVerticalBox;
class ASCVehicle;
class SSCFirstPersonHUD : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SSCFirstPersonHUD) {} SLATE_ARGUMENT(USCViewerSubsystem*, Subsystem) SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    virtual void Tick(const FGeometry& Geometry, double Time, float Delta) override;
private:
    TWeakObjectPtr<USCViewerSubsystem> Subsystem;
    TWeakObjectPtr<ASCVehicle> Listed;
    TSharedPtr<SVerticalBox> Menu;
};
