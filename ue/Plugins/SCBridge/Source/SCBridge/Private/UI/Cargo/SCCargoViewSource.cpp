// Cargo view sources for the UI: the subsystem adapter, source resolution, the mobiGlas open state and the UI console
// commands (`sc.MobiGlas`, `sc.MobiGlas.Preview`, `sc.UI.Shot`).
#include "SCCargoViewSource.h"
#include "Cargo/SCCargoSubsystem.h"
#include "SCFirstPersonCharacter.h"
#include "SCViewerSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCMobiGlas, Log, All);

namespace
{
/** USCCargoSubsystem behind the UI's source interface. */
class FSubsystemSource final : public ISCCargoViewSource
{
public:
    TWeakObjectPtr<USCCargoSubsystem> Cargo;
    virtual bool IsPreview() const override { return false; }
    virtual int32 GetRevision() const override { return Cargo.IsValid() ? Cargo->GetRevision() : 0; }
    virtual TArray<FSCCargoGridView> GetGrids(const FVector& Viewer, float RadiusM) const override
    {
        return Cargo.IsValid() ? Cargo->GetGrids(Viewer, RadiusM) : TArray<FSCCargoGridView>();
    }
    virtual float GetNearbyRadiusM() const override { return Cargo.IsValid() ? Cargo->GetNearbyRadiusM() : 0; }
    virtual TArray<FSCCargoBeaconView> GetBeacons(const FVector& Viewer, float RadiusM) const override
    {
        return Cargo.IsValid() ? Cargo->GetBeacons(Viewer, RadiusM) : TArray<FSCCargoBeaconView>();
    }
    virtual TArray<FSCCargoCommodityView> GetCommodities(const TArray<int32>& Grids) const override
    {
        return Cargo.IsValid() ? Cargo->GetCommodities(Grids) : TArray<FSCCargoCommodityView>();
    }
    virtual TArray<FSCCargoDroneView> GetDrones() const override { return Cargo.IsValid() ? Cargo->GetDrones() : TArray<FSCCargoDroneView>(); }
    virtual TArray<FSCCargoJobView> GetJobs() const override { return Cargo.IsValid() ? Cargo->GetJobs() : TArray<FSCCargoJobView>(); }
    virtual TArray<FSCCargoBoxView> GetMovingBoxes() const override { return Cargo.IsValid() ? Cargo->GetMovingBoxes() : TArray<FSCCargoBoxView>(); }
    virtual FSCCargoTransferPreview Preview(const FSCCargoTransferRequest& Request) const override
    {
        return Cargo.IsValid() ? Cargo->Preview(Request) : FSCCargoTransferPreview();
    }
    virtual int32 Submit(const FSCCargoTransferRequest& Request, FText& OutError) override
    {
        if (Cargo.IsValid()) return Cargo->Submit(Request, OutError);
        OutError = NSLOCTEXT("SCMobiGlas", "NoCargo", "No cargo system in this world.");
        return INDEX_NONE;
    }
    virtual bool Cancel(int32 JobId) override { return Cargo.IsValid() && Cargo->Cancel(JobId); }
    virtual bool RequestGridAccess(int32 GridId, bool bOpen, FText& OutError) override
    {
        if (Cargo.IsValid()) return Cargo->RequestGridAccess(GridId, bOpen, OutError);
        OutError = NSLOCTEXT("SCMobiGlas", "NoCargoAccess", "No cargo system in this world.");
        return false;
    }
};

FSubsystemSource GSubsystemSource;
TUniquePtr<ISCCargoViewSource> GPreviewSource;
bool GOpen = false; // the mobiGlas open state (see IsMobiGlasOpen)

ASCFirstPersonCharacter* Character(USCViewerSubsystem* Viewer) { return Viewer ? Viewer->GetFirstPerson() : nullptr; }

FAutoConsoleCommandWithWorldAndArgs GMobiGlasCommand(TEXT("sc.MobiGlas"), TEXT("sc.MobiGlas [0|1]: open / close the mobiGlas (no argument: toggle)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(World ? World : SCCargoUI::ViewerWorld());
        const bool bOpen = Args.IsEmpty() ? !SCCargoUI::IsMobiGlasOpen(Viewer) : FCString::Atoi(*Args[0]) != 0;
        SCCargoUI::SetMobiGlasOpen(Viewer, bOpen);
    }));
FAutoConsoleCommandWithWorldAndArgs GPreviewCommand(TEXT("sc.MobiGlas.Preview"),
    TEXT("sc.MobiGlas.Preview 0|1: feed the mobiGlas and callouts synthetic UI-preview views (placed around the camera) instead of the cargo subsystem"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        SCCargoUI::SetPreviewEnabled(Args.IsEmpty() || FCString::Atoi(*Args[0]) != 0, World ? World : SCCargoUI::ViewerWorld());
    }));
FAutoConsoleCommandWithWorldAndArgs GShotCommand(TEXT("sc.UI.Shot"),
    TEXT("sc.UI.Shot <tag>: capture the window with the Slate UI to <-SCShotDir>/<tag>.png (route shots capture the bare viewport)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*) {
        FString Dir;
        if (!FParse::Value(FCommandLine::Get(), TEXT("SCShotDir="), Dir)) Dir = FPaths::ScreenShotDir();
        const FString Path = FPaths::Combine(Dir, (Args.IsEmpty() ? FString(TEXT("ui")) : Args[0]) + TEXT(".png"));
        FScreenshotRequest::RequestScreenshot(Path, /*bInShowUI=*/true, /*bAddFilenameSuffix=*/false);
        UE_LOG(LogSCMobiGlas, Display, TEXT("UI_SHOT path=%s"), *Path);
    }));
FAutoConsoleCommand GMobiGlasAppCommand(TEXT("sc.MobiGlas.App"), TEXT("sc.MobiGlas.App Cargo|ShipLink|Garage: open a mobiGlas app"),
    FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) {
        if (!Args.IsEmpty()) FSCMobiGlasRequests::Get().App = Args[0];
    }));
FAutoConsoleCommand GSelectCommand(TEXT("sc.MobiGlas.Select"), TEXT("sc.MobiGlas.Select <grid>: select a grid (id, ship, elevator or label text) as a click on its row"),
    FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) { if (!Args.IsEmpty()) FSCMobiGlasRequests::Get().SelectGrid = FString::Join(Args, TEXT(" ")); }));
FAutoConsoleCommand GFromCommand(TEXT("sc.MobiGlas.From"), TEXT("sc.MobiGlas.From <grid>: the transfer source (id, ship / elevator = the whole group, or label text)"),
    FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) { if (!Args.IsEmpty()) FSCMobiGlasRequests::Get().From = FString::Join(Args, TEXT(" ")); }));
FAutoConsoleCommand GToCommand(TEXT("sc.MobiGlas.To"), TEXT("sc.MobiGlas.To <grid>: the transfer destination (id, ship / elevator = the whole group, or label text)"),
    FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) { if (!Args.IsEmpty()) FSCMobiGlasRequests::Get().To = FString::Join(Args, TEXT(" ")); }));
FAutoConsoleCommand GLayerCommand(TEXT("sc.MobiGlas.Layer"), TEXT("sc.MobiGlas.Layer <n>: show lattice layer n (1-based) of the selected grid"),
    FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) { if (!Args.IsEmpty()) FSCMobiGlasRequests::Get().Layer = FCString::Atoi(*Args[0]) - 1; }));
FAutoConsoleCommand GAmountCommand(TEXT("sc.MobiGlas.Amount"), TEXT("sc.MobiGlas.Amount <scu>: set the transfer amount (snaps to the achievable stops)"),
    FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) { if (!Args.IsEmpty()) FSCMobiGlasRequests::Get().Amount = FCString::Atoi(*Args[0]); }));
FAutoConsoleCommand GConfirmCommand(TEXT("sc.MobiGlas.Confirm"), TEXT("sc.MobiGlas.Confirm: press CONFIRM TRANSFER"),
    FConsoleCommandDelegate::CreateLambda([] { FSCMobiGlasRequests::Get().bConfirm = true; }));
FAutoConsoleCommand GCancelCommand(TEXT("sc.MobiGlas.CancelJob"), TEXT("sc.MobiGlas.CancelJob <job id>: press CANCEL on a transfer"),
    FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) { if (!Args.IsEmpty()) FSCMobiGlasRequests::Get().CancelJob = FCString::Atoi(*Args[0]); }));
FAutoConsoleCommand GAccessCommand(TEXT("sc.MobiGlas.Extend"), TEXT("sc.MobiGlas.Extend 1|0: press EXTEND / RETRACT for the selected grid's ship (its whole group)"),
    FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) { FSCMobiGlasRequests::Get().AccessSelectedGroup = Args.IsEmpty() || FCString::Atoi(*Args[0]) != 0; }));
FAutoConsoleCommand GOrbitCommand(TEXT("sc.MobiGlas.Orbit"), TEXT("sc.MobiGlas.Orbit <dx px> <dy px>: drag the live view of the open mobiGlas (orbit)"),
    FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) {
        if (Args.Num() >= 2) FSCMobiGlasRequests::Get().OrbitPixels += FVector2D(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]));
    }));
FAutoConsoleCommand GZoomCommand(TEXT("sc.MobiGlas.Zoom"), TEXT("sc.MobiGlas.Zoom <notches>: mouse wheel on the live view of the open mobiGlas (+ in)"),
    FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) { if (!Args.IsEmpty()) FSCMobiGlasRequests::Get().ZoomNotches += FCString::Atof(*Args[0]); }));
FAutoConsoleCommandWithWorldAndArgs GStateCommand(TEXT("sc.MobiGlas.State"), TEXT("sc.MobiGlas.State: log whether the mobiGlas is open (MOBIGLAS_STATE)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World) {
        USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(World ? World : SCCargoUI::ViewerWorld());
        UE_LOG(LogSCMobiGlas, Display, TEXT("MOBIGLAS_STATE open=%d first_person=%d"), SCCargoUI::IsMobiGlasOpen(Viewer), Viewer && Viewer->GetFirstPerson() != nullptr);
    }));
} // namespace

FSCMobiGlasRequests& FSCMobiGlasRequests::Get()
{
    static FSCMobiGlasRequests Requests;
    return Requests;
}

namespace SCCargoUI
{
UWorld* ViewerWorld() { return GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetWorld() : nullptr; }

ISCCargoViewSource* GetSource(UWorld* World)
{
    if (GPreviewSource) return GPreviewSource.Get();
    USCCargoSubsystem* Cargo = World ? World->GetSubsystem<USCCargoSubsystem>() : nullptr;
    GSubsystemSource.Cargo = Cargo;
    return Cargo ? &GSubsystemSource : nullptr;
}

bool ViewerLocation(UWorld* World, FVector& Out)
{
    APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
    if (!PC || !PC->PlayerCameraManager) return false;
    Out = PC->PlayerCameraManager->GetCameraLocation();
    return true;
}

bool IsMobiGlasOpen(USCViewerSubsystem* Viewer)
{
    // One state: while a first-person character exists it owns it (its F1 / Esc key table changes it), and this
    // mirror follows it every query; when the character goes away (unload, camera mode change) the mobiGlas closes —
    // the mirror never re-opens it with a stale value.
    static TWeakObjectPtr<ASCFirstPersonCharacter> Owner;
    if (ASCFirstPersonCharacter* C = Character(Viewer))
    {
        Owner = C;
        GOpen = C->bMobiGlasOpen;
    }
    else if (!Owner.IsExplicitlyNull())
    {
        Owner.Reset();
        GOpen = false;
    }
    return GOpen;
}
void SetMobiGlasOpen(USCViewerSubsystem* Viewer, bool bOpen)
{
    if (ASCFirstPersonCharacter* C = Character(Viewer)) C->SetMobiGlasOpen(bOpen);
    GOpen = bOpen;
    UE_LOG(LogSCMobiGlas, Display, TEXT("MOBIGLAS open=%d preview=%d"), bOpen, IsPreviewEnabled());
}

bool IsPreviewEnabled() { return GPreviewSource.IsValid(); }
void SetPreviewEnabled(bool bEnabled, UWorld* World)
{
    GPreviewSource.Reset();
    if (bEnabled) GPreviewSource = MakePreviewSource(World);
    UE_LOG(LogSCMobiGlas, Display, TEXT("MOBIGLAS_PREVIEW on=%d (synthetic UI-preview views, not game data)"), bEnabled);
}
} // namespace SCCargoUI
