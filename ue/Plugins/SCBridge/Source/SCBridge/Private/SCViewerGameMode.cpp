// ASCViewerGameMode / ASCViewerPlayerController: pawn wiring, cursor/input mode and the Slate UI root.
#include "SCViewerGameMode.h"
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
    if (FParse::Param(FCommandLine::Get(), TEXT("SCNoUI")) || FParse::Value(FCommandLine::Get(), TEXT("SCPackage="), HarnessPackage)) return;
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
