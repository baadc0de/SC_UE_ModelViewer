// ASCViewerGameMode / ASCViewerPlayerController: pawn wiring, cursor/input mode and the Slate UI root.
#include "SCViewerGameMode.h"
#include "SCViewerExposure.h"
#include "SCLook.h"
#include "SCLookLighting.h"
#include "SCRenderPath.h"
#include "SCViewerSettings.h"
#include "SCViewerPawn.h"
#include "UI/SSCViewerRoot.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

ASCViewerGameMode::ASCViewerGameMode()
{
    DefaultPawnClass = ASCViewerPawn::StaticClass();
    PlayerControllerClass = ASCViewerPlayerController::StaticClass();
}

void ASCViewerGameMode::StartPlay()
{
    // -SCHostExposure keeps the map's exposure bounds and UE's tonemapper (comparison captures only).
    if (UWorld* World = GetWorld(); World && !FParse::Param(FCommandLine::Get(), TEXT("SCHostExposure")))
    {
        SCViewerExposure::Install(*World);
        SCLookLighting::ApplyDefaults(World); // LOOK-8: bloom, threshold, exposure bias
    }
    SCRenderPath::Apply(SCRenderPath::Resolve()); // RTXPT-1: the saved / -SCRenderPath lighting path
    Super::StartPlay();
}

ASCViewerPlayerController::ASCViewerPlayerController()
{
    bShowMouseCursor = true;
}
void ASCViewerPlayerController::BeginPlay()
{
    Super::BeginPlay();
    if (!IsLocalController() || !GEngine || !GEngine->GameViewport) return;
    // Harness review runs (-SCPackage, capture_views.sh) and -SCNoUI keep the bare viewport.
    FString HarnessPackage;
    if (FParse::Param(FCommandLine::Get(), TEXT("SCNoUI")) || (FParse::Value(FCommandLine::Get(), TEXT("SCPackage="), HarnessPackage) && !FParse::Param(FCommandLine::Get(), TEXT("SCExplore")))) return;
    USCViewerSubsystem* Subsystem = USCViewerSubsystem::Get(this);
    if (!Subsystem) return;
    Root = SNew(SSCViewerRoot).Subsystem(Subsystem);
    GEngine->GameViewport->AddViewportWidgetContent(Root.ToSharedRef(), 10);
    FInputModeGameAndUI InputMode;
    InputMode.SetHideCursorDuringCapture(false);
    InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
    SetInputMode(InputMode);
}
void ASCViewerPlayerController::EndPlay(const EEndPlayReason::Type Reason)
{
    if (Root && GEngine && GEngine->GameViewport) GEngine->GameViewport->RemoveViewportWidgetContent(Root.ToSharedRef());
    Root.Reset();
    Super::EndPlay(Reason);
}
