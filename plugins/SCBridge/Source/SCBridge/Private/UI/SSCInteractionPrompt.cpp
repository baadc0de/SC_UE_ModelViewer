// SSCInteractionPrompt: the first-person popout's layout and paint (tokens: SCHudStyle, the callouts' language).
#include "SSCInteractionPrompt.h"
#include "SCViewerSubsystem.h"
#include "SCFirstPersonCharacter.h"
#include "UI/SCHudStyle.h"
#include "UI/SCHudPaint.h"
#include "SCViewerShell.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Widgets/SViewport.h"

using namespace SCHudStyle;

namespace
{
// Our layout (1080p reference pixels): the leader leaves the crosshair at a 45° rise, then a shelf into the placard.
constexpr float LeaderStart = 10, LeaderRise = 28, LeaderShelf = 18;
constexpr float KeyCap = 22, PromptRowGap = 4, RowPadY = 3, KeyGap = 8;

const ASCFirstPersonCharacter* Character(const TWeakObjectPtr<USCViewerSubsystem>& S)
{
    return S.IsValid() ? S->GetFirstPerson() : nullptr;
}
} // namespace

void SSCInteractionPrompt::Construct(const FArguments& Args)
{
    Subsystem = Args._Subsystem;
    SetVisibility(EVisibility::HitTestInvisible);
}

void SSCInteractionPrompt::Tick(const FGeometry& Geometry, double Time, float Delta)
{
    Now = Time;
    const ASCFirstPersonCharacter* C = Character(Subsystem);
    const FString Key = C && C->GetPrompt().IsValid() ? C->GetPrompt().Key : FString();
    if (Key != ShownKey)
    {
        ShownKey = Key;
        Born = Key.IsEmpty() ? -1 : Time;
    }
    // The anchor through the player's view (game viewport pixels -> Slate absolute -> this widget, as the callouts do).
    Projected.Reset();
    if (!C || !C->GetPrompt().bAnchored) return;
    UWorld* World = SCViewerShell::ViewerWorld();
    APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
    TSharedPtr<SViewport> Viewport = GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetGameViewportWidget() : nullptr;
    FVector2D Pixel;
    if (!PC || !Viewport || !PC->ProjectWorldLocationToScreen(C->GetPrompt().Anchor, Pixel, false)) return;
    const FGeometry& View = Viewport->GetCachedGeometry();
    Projected = Geometry.AbsoluteToLocal(View.LocalToAbsolute(Pixel / FMath::Max(0.01f, View.Scale)));
}

int32 SSCInteractionPrompt::OnPaint(const FPaintArgs&, const FGeometry& G, const FSlateRect&, FSlateWindowElementList& Out, int32 Layer,
                                     const FWidgetStyle&, bool) const
{
    using namespace SCHudPaint;
    const ASCFirstPersonCharacter* C = Character(Subsystem);
    if (!C || Born < 0 || !C->GetPrompt().IsValid()) return Layer;
    const FSCInteractionPrompt& P = C->GetPrompt();
    const float T = FMath::Clamp(EaseOutCubic(float((Now - Born) / CalloutFadeIn)), 0.f, 1.f);
    const FSlateFontInfo Row = Body(), Small = Label();

    // Measure: the title, each action (label, and why it is unavailable), the footer.
    const FString Title = Caps(P.Title);
    const FVector2f TitleSize = Title.IsEmpty() ? FVector2f::ZeroVector : Measure(Title, Small);
    // Several actions held back for one reason: the reason once, in the footer, not on every row.
    const bool bSharedNote = P.Options.Num() > 1 && !P.Options[0].Note.IsEmpty() &&
                             !P.Options.ContainsByPredicate([&](const FSCPromptOption& O) { return O.Note != P.Options[0].Note; });
    TArray<FString> Notes;
    float RowsWidth = 0, RowHeight = KeyCap;
    for (const FSCPromptOption& O : P.Options)
    {
        Notes.Add(O.Note.IsEmpty() || bSharedNote ? FString() : Caps(O.Note));
        const FVector2f L = Measure(O.Label, Row), N = Notes.Last().IsEmpty() ? FVector2f::ZeroVector : Measure(Notes.Last(), Small);
        RowsWidth = FMath::Max(RowsWidth, KeyCap + KeyGap + L.X + (N.X > 0 ? KeyGap + N.X : 0));
        RowHeight = FMath::Max(RowHeight, L.Y);
    }
    RowHeight += 2 * RowPadY;
    TArray<FString> Footer;
    if (bSharedNote) Footer.Add(Caps(P.Options[0].Note));
    if (!P.Hint.IsEmpty()) Footer.Add(Caps(P.Hint));
    if (P.Options.Num() > 1) Footer.Add(TEXT("WHEEL  CHOOSE"));
    const FString FooterText = FString::Join(Footer, TEXT("   "));
    const FVector2f FooterSize = FooterText.IsEmpty() ? FVector2f::ZeroVector : Measure(FooterText, Small);
    const float InnerW = FMath::Max3(RowsWidth, TitleSize.X, FooterSize.X);
    const float TitleH = TitleSize.Y > 0 ? TitleSize.Y + PromptRowGap : 0, FooterH = FooterSize.Y > 0 ? FooterSize.Y + PromptRowGap : 0;
    const FVector2f Size(InnerW + 2 * CalloutPadX + FocusWidth, TitleH + P.Options.Num() * RowHeight + (P.Options.Num() - 1) * PromptRowGap + FooterH + 2 * CalloutPadY);

    // Leader: the anchor -> 45° rise -> shelf into the placard's edge at its first action; the placard slides in along it.
    // The anchor is the thing's point in the world, projected (off the view or behind the camera: no popout); a prompt
    // without one (seated, on a ladder) starts by the crosshair. Up-right of it, mirrored left at the view's right edge,
    // and kept inside the view vertically.
    const FVector2f ViewSize(G.GetLocalSize());
    const FVector2f Centre = ViewSize * 0.5f;
    FVector2f A = Centre + FVector2f(LeaderStart, -LeaderStart);
    if (P.bAnchored)
    {
        if (!Projected.IsSet()) return Layer;
        A = FVector2f(Projected.GetValue());
        if (A.X < 0 || A.Y < 0 || A.X > ViewSize.X || A.Y > ViewSize.Y) return Layer;
    }
    const float Side = A.X + LeaderRise + LeaderShelf + Size.X + CalloutScreenInset > ViewSize.X ? -1.f : 1.f;
    const float Lift = FMath::Clamp(LeaderRise, A.Y - (ViewSize.Y - CalloutScreenInset - Size.Y + CalloutPadY + TitleH + RowHeight * 0.5f),
                                    A.Y - (CalloutScreenInset + CalloutPadY + TitleH + RowHeight * 0.5f));
    const FVector2f E = A + FVector2f(Side * FMath::Abs(Lift), -Lift);
    const FVector2f Edge = E + FVector2f(Side * LeaderShelf, 0);
    const float Left = Side > 0 ? Edge.X : Edge.X - Size.X;
    const FVector2f Pos(Left + Side * CalloutSlide * (1 - T), Edge.Y - (CalloutPadY + TitleH + RowHeight * 0.5f));
    const FLinearColor Halo = Ink().CopyWithNewOpacity(0.55f * T), Line = OnScrim().CopyWithNewOpacity(T);
    Lines(Out, Layer, G, {A, E, Edge}, Halo, LeaderWidth + 2);
    Lines(Out, Layer + 1, G, {A, E, Edge}, Line, LeaderWidth);
    Box(Out, Layer, G, A - FVector2f(CalloutDot / 2 + CalloutHalo), FVector2f(CalloutDot + 2 * CalloutHalo), Halo);
    Box(Out, Layer + 1, G, A - FVector2f(CalloutDot / 2), FVector2f(CalloutDot), Line);

    // Placard: paper, the accent edge on the shelf side (a target), the title, the actions, the footer.
    Box(Out, Layer + 2, G, Pos, Size, Paper().CopyWithNewOpacity(T));
    Box(Out, Layer + 3, G, Side > 0 ? Pos : Pos + FVector2f(Size.X - FocusWidth, 0), FVector2f(FocusWidth, Size.Y), Accent().CopyWithNewOpacity(T));
    const float X = Pos.X + (Side > 0 ? FocusWidth : 0) + CalloutPadX;
    float Y = Pos.Y + CalloutPadY;
    if (!Title.IsEmpty())
    {
        Text(Out, Layer + 3, G, FVector2f(X, Y), Title, Small, Ink2().CopyWithNewOpacity(T));
        Y += TitleH;
    }
    for (int32 I = 0; I < P.Options.Num(); ++I)
    {
        const FSCPromptOption& O = P.Options[I];
        const bool bSelected = I == P.Selected;
        if (bSelected && P.Options.Num() > 1)
            Box(Out, Layer + 3, G, FVector2f(Pos.X + (Side > 0 ? FocusWidth : 0), Y), FVector2f(Size.X - FocusWidth, RowHeight), PaperDim().CopyWithNewOpacity(T));
        const float Mid = Y + RowHeight * 0.5f;
        if (bSelected) // the key cap: E performs this one
        {
            const FVector2f Cap(X, Mid - KeyCap * 0.5f);
            Box(Out, Layer + 4, G, Cap, FVector2f(KeyCap), (O.bAvailable ? Ink() : Ink2()).CopyWithNewOpacity(T));
            const FVector2f K = Measure(TEXT("E"), Small);
            Text(Out, Layer + 5, G, Cap + (FVector2f(KeyCap) - K) * 0.5f, TEXT("E"), Small, Paper().CopyWithNewOpacity(T));
        }
        const FVector2f L = Measure(O.Label, Row);
        const float TextX = X + KeyCap + KeyGap;
        Text(Out, Layer + 4, G, FVector2f(TextX, Mid - L.Y * 0.5f), O.Label, Row, (O.bAvailable ? Ink() : Ink2()).CopyWithNewOpacity(T));
        if (!Notes[I].IsEmpty())
        {
            const FVector2f N = Measure(Notes[I], Small);
            Text(Out, Layer + 4, G, FVector2f(TextX + L.X + KeyGap, Mid - N.Y * 0.5f), Notes[I], Small, Ink2().CopyWithNewOpacity(T));
        }
        Y += RowHeight + PromptRowGap;
    }
    if (!FooterText.IsEmpty()) Text(Out, Layer + 3, G, FVector2f(X, Y), FooterText, Small, Ink2().CopyWithNewOpacity(T));
    return Layer + 6;
}
