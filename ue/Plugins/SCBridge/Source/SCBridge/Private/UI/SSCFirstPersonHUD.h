// In-game exploration overlay and teleport picker (Slate, shared with the standalone host).
#pragma once
#include "Widgets/SCompoundWidget.h"
class USCViewerSubsystem;
class SVerticalBox;
class SScrollBox;
class SWidget;
class ASCVehicle;
class SSCFirstPersonHUD : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SSCFirstPersonHUD) {} SLATE_ARGUMENT(USCViewerSubsystem*, Subsystem) SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    virtual void Tick(const FGeometry& Geometry, double Time, float Delta) override;
private:
    TWeakObjectPtr<USCViewerSubsystem> Subsystem;
    // The menu is rebuilt when the vehicle, its spawns, the hangar or its standing places change.
    TWeakObjectPtr<ASCVehicle> Listed;
    int32 ListedRevision = INDEX_NONE;
    TWeakObjectPtr<ASCVehicle> ListedHangar;
    int32 ListedHangarCount = INDEX_NONE;
    int32 ListedHangarRevision = INDEX_NONE;
    int32 ScrolledTo = INDEX_NONE;
    TSharedPtr<SVerticalBox> Menu;
    TArray<TSharedPtr<SWidget>> RowWidgets;    // by menu row (headings included)
    TArray<TSharedPtr<SScrollBox>> RowScrolls; // the section list holding each destination row (null for a heading)
};
