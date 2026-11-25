// Root of the in-game UI: top bar (archive, camera modes, Keep), model browser | 3D view | hierarchy, footer with
// load timings, cache, memory and disk headroom; an archive overlay with the p4k picker until sc_open succeeds; the
// mobiGlas and cargo callouts over the view (WP-K1c).
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
    TSharedRef<SWidget> MakeLoadingOverlay();
    /** Top bar and footer: hidden in the showcase (-SCShowcase). */
    EVisibility ChromeVisibility() const;
    /** Collapsed while the application's F1 menu is open (first-person HUD, key help). */
    EVisibility MenuHidden() const;
    TSharedRef<SWidget> MakeFooter();
    TSharedRef<SWidget> MakeCameraButton(const FText& Label, uint8 Mode, const FText& Tip);
    FReply PickArchive();
    FText FooterLoadText() const;
    FText FooterSystemText() const;
    /** What a player calls the entity / environment: its row label in the viewer's lists, else its id. */
    FString VehicleName(const FString& Entity) const;
    /** The loading overlay's model row: a named entity loading, or the last load failed. */
    bool ModelRowShown() const;
    FString EnvironmentName(const FString& Request) const;
    EActiveTimerReturnType SampleSystem(double, float);

    TWeakObjectPtr<USCViewerSubsystem> Subsystem;
    TSharedPtr<SWidget> View;
    TSharedPtr<SWidget> ViewportInput; // focus returns here when the menu closes
    /** The application's layers over the view (SCViewerShell::Overlays; UnrealCitizen: the mobiGlas and the callouts). */
    TSharedPtr<class SOverlay> Overlays;
    FText Notice; // Keep result / picker errors
    bool bFooterDetails = false; // the footer shows load timings and caches (developer details)
    bool bArchiveDetails = false; // the archive screen shows the open error's text
    uint64 UsedMemory = 0, FreeMemory = 0, DiskFree = 0, DiskTotal = 0;
};
