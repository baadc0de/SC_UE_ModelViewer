// SCViewerShell and SCRpcExtensions: the registries an application fills (menu, overlays, interactions, RPC extensions)
// and the menu's open state.
#include "SCViewerShell.h"
#include "SCRpcExtension.h"
#include "SCFirstPersonCharacter.h"
#include "SCViewerSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCViewerShell, Log, All);

namespace
{
template <typename T> struct TNamed
{
    FName Name;
    T Value;
};
struct FShell
{
    FText App;
    bool bCookedLevels = true;
    TOptional<FText> Menu;
    bool bOpen = false; // the UI-level mirror of the open state (see IsMenuOpen)
    TWeakObjectPtr<ASCFirstPersonCharacter> Owner;
    TArray<TNamed<SCViewerShell::FMakeOverlay>> Overlays;
    TArray<TNamed<TSharedRef<ISCInteractionProvider>>> Interactions;
};
FShell& Shell()
{
    static FShell S;
    return S;
}
template <typename T> void Put(TArray<TNamed<T>>& List, FName Name, T Value)
{
    if (TNamed<T>* Found = List.FindByPredicate([&](const TNamed<T>& E) { return E.Name == Name; })) Found->Value = MoveTemp(Value);
    else List.Add({Name, MoveTemp(Value)});
}
ASCFirstPersonCharacter* Character(USCViewerSubsystem* Viewer) { return Viewer ? Viewer->GetFirstPerson() : nullptr; }
} // namespace

namespace SCViewerShell
{
UWorld* ViewerWorld() { return GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetWorld() : nullptr; }

void SetAppName(const FText& Name) { Shell().App = Name; }
FText AppName() { return Shell().App; }
void SetCookedLevels(bool bUse) { Shell().bCookedLevels = bUse; }
bool UsesCookedLevels() { return Shell().bCookedLevels; }
void SetMenu(const FText& Name) { Shell().Menu = Name; }
void ClearMenu()
{
    Shell().Menu.Reset();
    Shell().bOpen = false;
}
bool HasMenu() { return Shell().Menu.IsSet(); }
FText MenuName() { return Shell().Menu.Get(FText::GetEmpty()); }

bool IsMenuOpen(USCViewerSubsystem* Viewer)
{
    // One state: while a first-person character exists it owns it (its F1 / Esc key table changes it), and this mirror
    // follows it every query; when the character goes away (unload, camera mode change) the menu closes — the mirror
    // never re-opens it with a stale value.
    FShell& S = Shell();
    if (ASCFirstPersonCharacter* C = Character(Viewer))
    {
        S.Owner = C;
        S.bOpen = C->bMenuOpen;
    }
    else if (!S.Owner.IsExplicitlyNull())
    {
        S.Owner.Reset();
        S.bOpen = false;
    }
    return S.bOpen;
}
void SetMenuOpen(USCViewerSubsystem* Viewer, bool bOpen)
{
    bOpen = bOpen && HasMenu();
    if (ASCFirstPersonCharacter* C = Character(Viewer)) C->SetMenuOpen(bOpen);
    Shell().bOpen = bOpen;
    UE_LOG(LogSCViewerShell, Display, TEXT("VIEWER_MENU open=%d menu=%s"), bOpen, *MenuName().ToString());
}

void AddOverlay(FName Name, FMakeOverlay Make) { Put(Shell().Overlays, Name, MoveTemp(Make)); }
void RemoveOverlay(FName Name) { Shell().Overlays.RemoveAll([&](const auto& E) { return E.Name == Name; }); }
TArray<FMakeOverlay> Overlays()
{
    TArray<FMakeOverlay> Out;
    for (const auto& E : Shell().Overlays) Out.Add(E.Value);
    return Out;
}

void AddInteraction(FName Name, TSharedRef<ISCInteractionProvider> Provider) { Put(Shell().Interactions, Name, MoveTemp(Provider)); }
void RemoveInteraction(FName Name) { Shell().Interactions.RemoveAll([&](const auto& E) { return E.Name == Name; }); }
TArray<TSharedRef<ISCInteractionProvider>> Interactions()
{
    TArray<TSharedRef<ISCInteractionProvider>> Out;
    for (const auto& E : Shell().Interactions) Out.Add(E.Value);
    return Out;
}
} // namespace SCViewerShell

// ---------------------------------------------------------------------------------------------------- RPC extensions
namespace
{
TArray<TPair<FName, TSharedRef<ISCRpcExtension>>>& RpcExtensions()
{
    static TArray<TPair<FName, TSharedRef<ISCRpcExtension>>> List;
    return List;
}
} // namespace
namespace SCRpcExtensions
{
void Add(FName Name, TSharedRef<ISCRpcExtension> Extension)
{
    Remove(Name);
    RpcExtensions().Emplace(Name, MoveTemp(Extension));
}
void Remove(FName Name) { RpcExtensions().RemoveAll([&](const auto& E) { return E.Key == Name; }); }
TArray<TSharedRef<ISCRpcExtension>> All()
{
    TArray<TSharedRef<ISCRpcExtension>> Out;
    for (const auto& E : RpcExtensions()) Out.Add(E.Value);
    return Out;
}
} // namespace SCRpcExtensions
