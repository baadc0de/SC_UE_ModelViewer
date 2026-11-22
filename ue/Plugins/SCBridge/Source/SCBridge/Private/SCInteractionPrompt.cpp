// WP-I1-4: the first-person prompt's selection rules (SCInteractionPrompt.h).
#include "SCInteractionPrompt.h"

void FSCInteractionPrompt::Update(FSCInteractionPrompt&& Next)
{
    // The same thing offering the same number of actions keeps the wheel's choice; anything else starts at the first.
    const bool bSame = !Key.IsEmpty() && Next.Key == Key;
    const bool bKeep = bSame && Next.Options.Num() == Options.Num();
    const int32 Keep = Selected;
    const FVector KeptAnchor = Anchor;
    const bool bKeptAnchor = bSame && bAnchored && Next.bAnchored;
    *this = MoveTemp(Next);
    Selected = bKeep && Options.IsValidIndex(Keep) ? Keep : 0;
    // The same thing keeps the point the popout was pinned to: it stays on the thing, not on the moving crosshair.
    if (bKeptAnchor) Anchor = KeptAnchor;
}

bool FSCInteractionPrompt::Cycle(int32 Steps)
{
    if (Options.Num() < 2 || Steps == 0) return false;
    Selected = ((Selected + Steps) % Options.Num() + Options.Num()) % Options.Num();
    return true;
}

FString FSCInteractionPrompt::Describe() const
{
    TArray<FString> Rows;
    for (int32 I = 0; I < Options.Num(); ++I)
        Rows.Add(FString::Printf(TEXT("%s%s%s"), I == Selected ? TEXT(">") : TEXT(""), Options[I].bAvailable ? TEXT("") : TEXT("-"), *Options[I].Label));
    return FString::Printf(TEXT("key=\"%s\" title=\"%s\" options=[%s]%s"), *Key, *Title, *FString::Join(Rows, TEXT(", ")),
                           Hint.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" hint=\"%s\""), *Hint));
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

namespace
{
FSCInteractionPrompt Prompt(const TCHAR* Key, std::initializer_list<const TCHAR*> Labels)
{
    FSCInteractionPrompt P;
    P.Key = Key;
    for (const TCHAR* L : Labels) P.Options.Add({L});
    return P;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCInteractionPromptSelectionTest, "SCBridge.Interaction.PromptSelection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCInteractionPromptSelectionTest::RunTest(const FString&)
{
    FSCInteractionPrompt P;
    P.Update(Prompt(TEXT("bed"), {TEXT("Sit"), TEXT("Lie Down"), TEXT("Sleep")}));
    TestEqual(TEXT("starts at the first action"), P.Selected, 0);
    TestTrue(TEXT("the wheel moves down"), P.Cycle(1));
    TestEqual(TEXT("second"), P.Selected, 1);
    P.Cycle(-2);
    TestEqual(TEXT("wraps upwards"), P.Selected, 2);
    P.Cycle(1);
    TestEqual(TEXT("wraps downwards"), P.Selected, 0);
    P.Cycle(1);
    P.Update(Prompt(TEXT("bed"), {TEXT("Sit"), TEXT("Lie Down"), TEXT("Sleep")}));
    TestEqual(TEXT("the same thing keeps the choice"), P.Selected, 1);
    P.Update(Prompt(TEXT("sofa"), {TEXT("Sit"), TEXT("Lie Down"), TEXT("Sleep")}));
    TestEqual(TEXT("another thing starts again"), P.Selected, 0);
    P.Cycle(2);
    P.Update(Prompt(TEXT("sofa"), {TEXT("Sit")}));
    TestEqual(TEXT("fewer actions start again"), P.Selected, 0);
    TestFalse(TEXT("one action: nothing to choose"), P.Cycle(1));
    // The anchor: held for the same thing, taken anew for another.
    FSCInteractionPrompt A = Prompt(TEXT("pad"), {TEXT("Open")});
    A.Anchor = FVector(1, 2, 3);
    A.bAnchored = true;
    P.Update(MoveTemp(A));
    FSCInteractionPrompt Moved = Prompt(TEXT("pad"), {TEXT("Open")});
    Moved.Anchor = FVector(9, 9, 9);
    Moved.bAnchored = true;
    P.Update(MoveTemp(Moved));
    TestEqual(TEXT("the same thing keeps its point"), P.Anchor, FVector(1, 2, 3));
    FSCInteractionPrompt Other = Prompt(TEXT("bed"), {TEXT("Sit")});
    Other.Anchor = FVector(5, 5, 5);
    Other.bAnchored = true;
    P.Update(MoveTemp(Other));
    TestEqual(TEXT("another thing takes its own"), P.Anchor, FVector(5, 5, 5));
    P.Update(FSCInteractionPrompt());
    TestFalse(TEXT("nothing aimed at"), P.IsValid());
    TestFalse(TEXT("no wheel without actions"), P.Cycle(1));
    return true;
}
#endif
