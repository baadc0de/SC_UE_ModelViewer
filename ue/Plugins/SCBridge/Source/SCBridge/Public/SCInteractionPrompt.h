// WP-I1-4: what E does now, as the first-person popout shows it: the thing under the crosshair, the actions it offers
// (from data: a door's verb, a panel's interaction, a usable's interactions that start a use) and the one E performs.
// The wheel moves the selection when there is more than one. ASCFirstPersonCharacter builds it every tick (UpdatePrompt);
// SSCInteractionPrompt draws it.
#pragma once
#include "CoreMinimal.h"

/** One action of the prompt. An action the viewer cannot perform yet is listed, dimmed, with why (Note). */
struct FSCPromptOption
{
    FString Label;
    bool bAvailable = true;
    FString Note;
};

struct SCBRIDGE_API FSCInteractionPrompt
{
    /** What the prompt is for (the selection is kept while it stays the same). Empty: nothing to interact with. */
    FString Key;
    /** The thing's name when the data names it (a door), else empty. */
    FString Title;
    TArray<FSCPromptOption> Options;
    int32 Selected = 0;
    /** A line under the actions (a ladder's climb keys). */
    FString Hint;

    bool IsValid() const { return !Options.IsEmpty(); }
    const FSCPromptOption* SelectedOption() const { return Options.IsValidIndex(Selected) ? &Options[Selected] : nullptr; }
    /** Take Next's actions, keeping the selection when Next is for the same thing and has as many actions. */
    void Update(FSCInteractionPrompt&& Next);
    /** Move the selection Steps actions (wrapping); false when there is nothing to choose between. */
    bool Cycle(int32 Steps);
    /** One line for logs and tests: key, title, actions (the selected one marked with '>', unavailable ones with '-'). */
    FString Describe() const;
};
