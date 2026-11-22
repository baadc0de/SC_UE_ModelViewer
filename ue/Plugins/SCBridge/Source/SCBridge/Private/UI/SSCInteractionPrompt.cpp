// SSCInteractionPrompt: the first-person popout's layout and paint (tokens: SCMobiGlasStyle, the callouts' language).
#include "SSCInteractionPrompt.h"
#include "SCViewerSubsystem.h"
#include "SCFirstPersonCharacter.h"
#include "Cargo/SCMobiGlasStyle.h"
#include "Cargo/SSCMobiGlasWidgets.h"

using namespace SCMobiGlasStyle;

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
}

int32 SSCInteractionPrompt::OnPaint(const FPaintArgs&, const FGeometry& G, const FSlateRect&, FSlateWindowElementList& Out, int32 Layer,
                                     const FWidgetStyle&, bool) const
{
    using namespace SCMobiGlasPaint;
    const ASCFirstPersonCharacter* C = Character(Subsystem);
    if (!C || Born < 0 || !C->GetPrompt().IsValid()) return Layer;
    const FSCInteractionPrompt& P = C->GetPrompt();
    const float T = FMath::Clamp(EaseOutCubic(float((Now - Born) / CalloutFadeIn)), 0.f, 1.f);
    const FSlateFontInfo Row = Body(), Small = Label();

    // Measure: the title, each action (label, and why it is unavailable), the footer.
    const FString Title = Caps(P.Title);
    const FVector2f TitleSize = Title.IsEmpty() ? FVector2f::ZeroVector : Measure(Title, Small);
    TArray<FString> Notes;
    float RowsWidth = 0, RowHeight = KeyCap;
    for (const FSCPromptOption& O : P.Options)
    {
        Notes.Add(O.Note.IsEmpty() ? FString() : Caps(O.Note));
        const FVector2f L = Measure(O.Label, Row), N = Notes.Last().IsEmpty() ? FVector2f::ZeroVector : Measure(Notes.Last(), Small);
        RowsWidth = FMath::Max(RowsWidth, KeyCap + KeyGap + L.X + (N.X > 0 ? KeyGap + N.X : 0));
        RowHeight = FMath::Max(RowHeight, L.Y);
    }
    RowHeight += 2 * RowPadY;
    TArray<FString> Footer;
    if (!P.Hint.IsEmpty()) Footer.Add(Caps(P.Hint));
    if (P.Options.Num() > 1) Footer.Add(TEXT("WHEEL  CHOOSE"));
    const FString FooterText = FString::Join(Footer, TEXT("   "));
    const FVector2f FooterSize = FooterText.IsEmpty() ? FVector2f::ZeroVector : Measure(FooterText, Small);
    const float InnerW = FMath::Max3(RowsWidth, TitleSize.X, FooterSize.X);
    const float TitleH = TitleSize.Y > 0 ? TitleSize.Y + PromptRowGap : 0, FooterH = FooterSize.Y > 0 ? FooterSize.Y + PromptRowGap : 0;
    const FVector2f Size(InnerW + 2 * CalloutPadX + FocusWidth, TitleH + P.Options.Num() * RowHeight + (P.Options.Num() - 1) * PromptRowGap + FooterH + 2 * CalloutPadY);

    // Leader: crosshair -> 45° rise -> shelf into the placard's edge at its first action; the placard slides in along it.
    const FVector2f Centre = FVector2f(G.GetLocalSize()) * 0.5f;
    const FVector2f A = Centre + FVector2f(LeaderStart, -LeaderStart), E = A + FVector2f(LeaderRise, -LeaderRise);
    const FVector2f Edge = E + FVector2f(LeaderShelf, 0);
    const FVector2f Pos = Edge + FVector2f(CalloutSlide * (1 - T), -(CalloutPadY + TitleH + RowHeight * 0.5f));
    const FLinearColor Halo = Ink().CopyWithNewOpacity(0.55f * T), Line = OnScrim().CopyWithNewOpacity(T);
    Lines(Out, Layer, G, {A, E, Edge}, Halo, LeaderWidth + 2);
    Lines(Out, Layer + 1, G, {A, E, Edge}, Line, LeaderWidth);
    Box(Out, Layer, G, A - FVector2f(CalloutDot / 2 + CalloutHalo), FVector2f(CalloutDot + 2 * CalloutHalo), Halo);
    Box(Out, Layer + 1, G, A - FVector2f(CalloutDot / 2), FVector2f(CalloutDot), Line);

    // Placard: paper, the accent edge on the shelf side (a target), the title, the actions, the footer.
    Box(Out, Layer + 2, G, Pos, Size, Paper().CopyWithNewOpacity(T));
    Box(Out, Layer + 3, G, Pos, FVector2f(FocusWidth, Size.Y), Accent().CopyWithNewOpacity(T));
    const float X = Pos.X + FocusWidth + CalloutPadX;
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
            Box(Out, Layer + 3, G, FVector2f(Pos.X + FocusWidth, Y), FVector2f(Size.X - FocusWidth, RowHeight), PaperDim().CopyWithNewOpacity(T));
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
