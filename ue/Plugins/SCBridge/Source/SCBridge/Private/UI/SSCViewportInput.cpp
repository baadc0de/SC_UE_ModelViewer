// SSCViewportInput: orbit/pan/zoom/fly input and double-click picking.
#include "SSCViewportInput.h"
#include "SCViewerSubsystem.h"
#include "SCViewerPawn.h"
#include "SCFirstPersonCharacter.h"
#include "SCVehicle.h"
#include "SCLook.h"
#include "Cargo/SCCargoViewSource.h"
#include "Engine/GameInstance.h"
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
    // Clicking to capture the mouse must not drop movement keys held in first person.
    const FKey Button = Event.GetEffectingButton();
    const bool bPan = Button == EKeys::MiddleMouseButton || (Button == EKeys::LeftMouseButton && Event.IsShiftDown());
    if (!bPan && Button != EKeys::LeftMouseButton && Button != EKeys::RightMouseButton) return FReply::Unhandled();
    if (SCCargoUI::IsMobiGlasOpen(Subsystem.Get())) return FReply::Unhandled(); // the cursor belongs to the mobiGlas
    // First person: the button is also a key-table input (right button held: aim down sights).
    const bool bFirstPerson = Subsystem.IsValid() && Subsystem->GetFirstPerson();
    if (bFirstPerson) Subsystem->GetFirstPerson()->HandleKey(Button, true);
    // Already captured (a second button while looking: right button to aim, left to fire): keep the capture. Asking for it again
    // makes Slate release it first (FSlateUser::SetPointerCaptor), whose OnMouseCaptureLost ended the look until the window was
    // refocused (2026-11-19).
    const bool bKeepCapture = HasMouseCapture() && Drag != EDrag::None;
    if (!bKeepCapture || !bFirstPerson) Drag = bPan ? EDrag::Pan : EDrag::Rotate;
    if (bKeepCapture) return FReply::Handled();
    return FReply::Handled().CaptureMouse(AsShared()).UseHighPrecisionMouseMovement(AsShared()).SetUserFocus(AsShared());
}
FReply SSCViewportInput::OnMouseButtonUp(const FGeometry&, const FPointerEvent& Event)
{
    if (Subsystem.IsValid() && Subsystem->GetFirstPerson()) Subsystem->GetFirstPerson()->HandleKey(Event.GetEffectingButton(), false);
    if (Subsystem.IsValid() && Subsystem->GetFirstPerson() && Drag != EDrag::None) return FReply::Handled();
    if (Drag == EDrag::None) return FReply::Unhandled();
    Drag = EDrag::None;
    return FReply::Handled().ReleaseMouseCapture();
}
FReply SSCViewportInput::OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event)
{
    if (Subsystem.IsValid() && Subsystem->GetFirstPerson() && Drag != EDrag::None)
    {
        Subsystem->GetFirstPerson()->Look(Event.GetCursorDelta());
        return FReply::Handled();
    }
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
    if (Subsystem.IsValid() && Subsystem->GetFirstPerson()) return Subsystem->GetFirstPerson()->HandleKey(Key, bDown);
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
    if (Key == EKeys::Escape && bDown && SCCargoUI::IsMobiGlasOpen(Subsystem.Get()))
    {
        SCCargoUI::SetMobiGlasOpen(Subsystem.Get(), false); // in case focus stayed on the view
        return true;
    }
    if (Key == EKeys::F1 && bDown)
    {
        SCCargoUI::SetMobiGlasOpen(Subsystem.Get(), !SCCargoUI::IsMobiGlasOpen(Subsystem.Get())); // orbit / fly: no character key table
        return true;
    }
    if (Key == EKeys::F && bDown && Subsystem.IsValid())
    {
        Subsystem->FrameAll(); // restore visibility, clear isolation/selection, frame the whole entity
        return true;
    }
    if (Key == EKeys::G && bDown && Subsystem.IsValid() && Subsystem->GetGameInstance())
    {
        SCLook::ToggleGlassRefraction(Subsystem->GetGameInstance()->GetWorld()); // the first person's key table binds G the same
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
    if (Event.IsRepeat()) return FReply::Handled();
    const bool Handled = HandleKey(Event.GetKey(), true);
    if (Subsystem.IsValid() && Subsystem->GetFirstPerson() && (Event.GetKey() == EKeys::Escape || Event.GetKey() == EKeys::T || Event.GetKey() == EKeys::F1))
    {
        Drag = EDrag::None;
        return FReply::Handled().ReleaseMouseCapture();
    }
    return Handled ? FReply::Handled() : FReply::Unhandled();
}
FReply SSCViewportInput::OnKeyUp(const FGeometry&, const FKeyEvent& Event)
{
    return HandleKey(Event.GetKey(), false) ? FReply::Handled() : FReply::Unhandled();
}
void SSCViewportInput::OnFocusLost(const FFocusEvent&)
{
    // Focus moving into the open mobiGlas: it forwards key releases to the player (the character cleared its input on open).
    if (Subsystem.IsValid() && Subsystem->GetFirstPerson() && !SCCargoUI::IsMobiGlasOpen(Subsystem.Get())) Subsystem->GetFirstPerson()->ClearInput();
    Held.Empty();
    ApplyMove();
    Drag = EDrag::None; // no mouse look/orbit without focus
}
void SSCViewportInput::OnMouseCaptureLost(const FCaptureLostEvent& Event)
{
    SCompoundWidget::OnMouseCaptureLost(Event);
    Drag = EDrag::None; // capture taken away (alt-tab, another widget): stop look/orbit/pan
}

FCursorReply SSCViewportInput::OnCursorQuery(const FGeometry&, const FPointerEvent&) const
{
    return Subsystem.IsValid() && Subsystem->GetFirstPerson() && HasMouseCapture()
        ? FCursorReply::Cursor(EMouseCursor::None) : FCursorReply::Unhandled();
}
