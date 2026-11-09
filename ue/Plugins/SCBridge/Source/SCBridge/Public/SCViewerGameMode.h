// Game mode of the viewer app: viewer pawn + controller; the controller shows the in-game UI on local players.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "SCViewerGameMode.generated.h"

class SWidget;

UCLASS()
class SCBRIDGE_API ASCViewerGameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    ASCViewerGameMode();
};

UCLASS()
class SCBRIDGE_API ASCViewerPlayerController : public APlayerController
{
    GENERATED_BODY()
public:
    ASCViewerPlayerController();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
    TSharedPtr<SWidget> Root;
};
