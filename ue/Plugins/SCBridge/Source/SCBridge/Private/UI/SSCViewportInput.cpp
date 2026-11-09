// SSCViewportInput: orbit/pan/zoom/fly input and double-click picking.
#include "SSCViewportInput.h"
#include "SCViewerSubsystem.h"
#include "SCViewerPawn.h"
#include "SCVehicle.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Widgets/SViewport.h"

void SSCViewportInput::Construct(const FArguments& Args)
{
    Subsystem = Args._Subsystem;
    SetVisibility(EVisibility::Visible); // hit-testable although it draws nothing
}
FReply SSCViewportInput::OnMouseButtonDown(const FGeometry&, const FPointerEvent& Event)
{
    const FKey Button = Event.GetEffectingButton();
    const bool bPan = Button == EKeys::MiddleMouseButton || (Button == EKeys::LeftMouseButton && Event.IsShiftDown());
    if (!bPan && Button != EKeys::LeftMouseButton && Button != EKeys::RightMouseButton) return FReply::Unhandled();
    Drag = bPan ? EDrag::Pan : EDrag::Rotate;
    return FReply::Handled().CaptureMouse(AsShared()).UseHighPrecisionMouseMovement(AsShared()).SetUserFocus(AsShared());
}
FReply SSCViewportInput::OnMouseButtonUp(const FGeometry&, const FPointerEvent&)
{
    if (Drag == EDrag::None) return FReply::Unhandled();
    Drag = EDrag::None;
    return FReply::Handled().ReleaseMouseCapture();
}
FReply SSCViewportInput::OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event)
{
    ASCViewerPawn* Pawn = Subsystem.IsValid() ? Subsystem->GetViewerPawn() : nullptr;
    if (Drag == EDrag::None || !Pawn) return FReply::Unhandled();
    const FVector2D Delta = Event.GetCursorDelta();
    if (Drag == EDrag::Rotate)
        Pawn->RotateDrag(Delta);
    else
        Pawn->PanDrag(Delta / FMath::Max(1.0, double(Geometry.GetLocalSize().Y) * Geometry.Scale));
    return FReply::Handled();
}
FReply SSCViewportInput::OnMouseWheel(const FGeometry&, const FPointerEvent& Event)
{
    if (ASCViewerPawn* Pawn = Subsystem.IsValid() ? Subsystem->GetViewerPawn() : nullptr) Pawn->Wheel(Event.GetWheelDelta());
    return FReply::Handled();
}
FReply SSCViewportInput::OnMouseButtonDoubleClick(const FGeometry&, const FPointerEvent& Event)
{
    USCViewerSubsystem* S = Subsystem.Get();
    UWorld* World = S && S->GetVehicle() ? S->GetVehicle()->GetWorld() : nullptr;
    APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
    TSharedPtr<SViewport> Viewport = GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetGameViewportWidget() : nullptr;
    if (!PC || !Viewport) return FReply::Handled();
    // Slate absolute position -> game viewport pixels (local units times the accumulated DPI scale).
    const FGeometry& ViewportGeometry = Viewport->GetCachedGeometry();
    const FVector2D Pixel = ViewportGeometry.AbsoluteToLocal(Event.GetScreenSpacePosition()) * ViewportGeometry.Scale;
    FHitResult Hit;
    if (PC->GetHitResultAtScreenPosition(Pixel, ECC_Visibility, true, Hit) && Hit.GetActor() == S->GetVehicle())
        S->FocusComponent(Hit.GetComponent());
    return FReply::Handled();
}
bool SSCViewportInput::HandleKey(const FKey& Key, bool bDown)
{
    static const FKey Movement[] = {EKeys::W, EKeys::S, EKeys::A, EKeys::D, EKeys::E, EKeys::Q, EKeys::LeftShift, EKeys::LeftControl};
    for (const FKey& K : Movement)
        if (Key == K)
        {
            if (bDown)
                Held.Add(Key);
            else
                Held.Remove(Key);
            ApplyMove();
            return true;
        }
    if (Key == EKeys::F && bDown && Subsystem.IsValid())
    {
        Subsystem->FrameAll(); // restore visibility, clear isolation/selection, frame the whole entity
        return true;
    }
    return false;
}
void SSCViewportInput::ApplyMove()
{
    ASCViewerPawn* Pawn = Subsystem.IsValid() ? Subsystem->GetViewerPawn() : nullptr;
    if (!Pawn) return;
    auto Axis = [&](const FKey& Plus, const FKey& Minus) { return float(Held.Contains(Plus)) - float(Held.Contains(Minus)); };
    Pawn->SetMoveKey(0, Axis(EKeys::W, EKeys::S));
    Pawn->SetMoveKey(1, Axis(EKeys::D, EKeys::A));
    Pawn->SetMoveKey(2, Axis(EKeys::E, EKeys::Q));
    // Shift: 4x faster, Ctrl: 4x slower (fine positioning inside cockpits).
    Pawn->SetSpeedModifier(Held.Contains(EKeys::LeftShift) ? 4.f : Held.Contains(EKeys::LeftControl) ? 0.25f : 1.f);
}
FReply SSCViewportInput::OnKeyDown(const FGeometry&, const FKeyEvent& Event)
{
    return HandleKey(Event.GetKey(), true) ? FReply::Handled() : FReply::Unhandled();
}
FReply SSCViewportInput::OnKeyUp(const FGeometry&, const FKeyEvent& Event)
{
    return HandleKey(Event.GetKey(), false) ? FReply::Handled() : FReply::Unhandled();
}
void SSCViewportInput::OnFocusLost(const FFocusEvent&)
{
    Held.Empty();
    ApplyMove();
}
