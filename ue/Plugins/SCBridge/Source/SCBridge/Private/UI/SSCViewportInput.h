// Transparent widget over the 3D view: mouse drag/wheel/keys drive the viewer pawn, double-click focuses the
// component under the cursor. Owning input in Slate keeps camera control independent of the game input setup.
#pragma once
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class USCViewerSubsystem;

class SSCViewportInput : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SSCViewportInput) {}
    SLATE_ARGUMENT(USCViewerSubsystem*, Subsystem)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args);
    virtual bool SupportsKeyboardFocus() const override { return true; }
    virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnMouseButtonDoubleClick(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
    virtual FReply OnKeyUp(const FGeometry& Geometry, const FKeyEvent& Event) override;
    virtual FCursorReply OnCursorQuery(const FGeometry& Geometry, const FPointerEvent& Event) const override;
    virtual void OnFocusLost(const FFocusEvent& Event) override;

private:
    enum class EDrag : uint8 { None, Rotate, Pan };
    bool HandleKey(const FKey& Key, bool bDown);
    void ApplyMove();
    TWeakObjectPtr<USCViewerSubsystem> Subsystem;
    EDrag Drag = EDrag::None;
    TSet<FKey> Held;
};
