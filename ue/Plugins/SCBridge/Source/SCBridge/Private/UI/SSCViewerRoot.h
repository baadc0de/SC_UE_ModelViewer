// Root of the in-game UI: top bar (archive, camera modes, Keep), model browser | 3D view | hierarchy, footer with
// load timings, cache, memory and disk headroom; an archive overlay with the p4k picker until sc_open succeeds.
#pragma once
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class USCViewerSubsystem;

class SSCViewerRoot : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SSCViewerRoot) {}
    SLATE_ARGUMENT(USCViewerSubsystem*, Subsystem)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args);
    virtual ~SSCViewerRoot() override;
    virtual void Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime) override;

private:
    /** Restrict the local player's view rect to the gap between the panels (scene centred, correct aspect). */
    void SetPlayerViewRect(const FVector2D& Origin, const FVector2D& Size);
    TSharedRef<SWidget> MakeTopBar();
    TSharedRef<SWidget> MakeArchiveOverlay();
    TSharedRef<SWidget> MakeFooter();
    TSharedRef<SWidget> MakeCameraButton(const FText& Label, uint8 Mode, const FText& Tip);
    FReply PickArchive();
    FText FooterLoadText() const;
    FText FooterSystemText() const;
    EActiveTimerReturnType SampleSystem(double, float);

    TWeakObjectPtr<USCViewerSubsystem> Subsystem;
    TSharedPtr<SWidget> View;
    FText Notice; // Keep result / picker errors
    bool bOfferedPicker = false; // the picker opens by itself once when the startup open fails
    uint64 UsedMemory = 0, FreeMemory = 0, DiskFree = 0, DiskTotal = 0;
};
