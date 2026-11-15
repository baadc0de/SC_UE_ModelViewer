// Where the mobiGlas and the callouts read cargo views from (WP-K1c): the world's USCCargoSubsystem, or — only when
// `sc.MobiGlas.Preview 1` is on — a synthetic UI-preview source for layout work without cargo in the scene. Both
// speak the subsystem's API and view structs (SCCargoTypes.h); the UI never touches cargo actors or components.
#pragma once
#include "CoreMinimal.h"
#include "Cargo/SCCargoTypes.h"

class UWorld;
class USCViewerSubsystem;

class ISCCargoViewSource
{
public:
    virtual ~ISCCargoViewSource() = default;
    /** Synthetic data for UI previews, not game data (the UI says so on screen). */
    virtual bool IsPreview() const = 0;
    virtual int32 GetRevision() const = 0;
    virtual TArray<FSCCargoGridView> GetGrids(const FVector& Viewer, float RadiusM) const = 0;
    virtual float GetNearbyRadiusM() const = 0;
    virtual TArray<FSCCargoCommodityView> GetCommodities(const TArray<int32>& Grids) const = 0;
    virtual TArray<FSCCargoDroneView> GetDrones() const = 0;
    virtual TArray<FSCCargoJobView> GetJobs() const = 0;
    /** Boxes off their grids (lifted, in transit, being placed) with live world bounds. */
    virtual TArray<FSCCargoBoxView> GetMovingBoxes() const = 0;
    virtual FSCCargoTransferPreview Preview(const FSCCargoTransferRequest& Request) const = 0;
    virtual int32 Submit(const FSCCargoTransferRequest& Request, FText& OutError) = 0;
    virtual bool Cancel(int32 JobId) = 0;
    /** Extend (bOpen) / retract a grid; false with OutError when refused. */
    virtual bool RequestGridAccess(int32 GridId, bool bOpen, FText& OutError) = 0;
    /** Advance a simulated source (the preview); the subsystem ticks itself. */
    virtual void Advance(float DeltaSeconds) {}
};

namespace SCCargoUI
{
/** The game world the viewer shows (the game viewport's world). */
UWorld* ViewerWorld();
/** The active source: the preview when enabled, else the world's cargo subsystem, else null (no cargo). */
ISCCargoViewSource* GetSource(UWorld* World);
/** The player's camera location (cm), the viewer point for "nearby". */
bool ViewerLocation(UWorld* World, FVector& Out);

/** mobiGlas open state: on the first-person character while there is one (its input pauses mouse look), else a
 *  UI-level flag (orbit / fly cameras). */
bool IsMobiGlasOpen(USCViewerSubsystem* Viewer);
void SetMobiGlasOpen(USCViewerSubsystem* Viewer, bool bOpen);

/** The preview source (created on demand around the current camera); see SCCargoPreviewSource.cpp. */
bool IsPreviewEnabled();
void SetPreviewEnabled(bool bEnabled, UWorld* World);
TUniquePtr<ISCCargoViewSource> MakePreviewSource(UWorld* World);
} // namespace SCCargoUI

/** Form actions queued by `sc.MobiGlas.*` console commands (routes); the mobiGlas model applies them on its next
 *  refresh, exactly like the matching clicks. */
struct FSCMobiGlasRequests
{
    TOptional<FString> SelectGrid, From, To; // grid selectors: an id, "ship" / "vehicle", "elevator" / "environment", or label text
    TOptional<int32> Layer, Amount;
    bool bConfirm = false;
    TOptional<int32> CancelJob;
    TOptional<bool> AccessSelectedGroup; // extend (true) / retract the selected grid's group
    /** Consumed by the mobiGlas shell (live-view orbit), not by the model. */
    FVector2D OrbitPixels = FVector2D::ZeroVector;
    float ZoomNotches = 0;
    static FSCMobiGlasRequests& Get();
};
