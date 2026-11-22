// WP-O1-2: what an application adds to the shared viewer shell (SSCViewerRoot, the first-person character). The viewer and
// UnrealCitizen register theirs from their module's StartupModule: the menu F1 opens (UnrealCitizen: the mobiGlas),
// layers drawn over the view (the mobiGlas, the world callouts), and first-person interactions (a thing under the
// crosshair → popout rows, E → its action). RPC routes are registered the same way (SCRpcExtension.h).
#pragma once
#include "CoreMinimal.h"

class SWidget;
class UWorld;
class USCViewerSubsystem;
class ASCFirstPersonCharacter;
struct FSCInteractionPrompt;

/** What a view overlay is made with. */
struct FSCViewOverlayArgs
{
    USCViewerSubsystem* Subsystem = nullptr;
    /** Gives keyboard focus back to the view (after a menu closes). */
    TFunction<void()> FocusView;
};

/** A kind of thing first person can use, beside the shared ones (doors, panels, lifts, seats, ladders, usables). Asked
 *  before the shared ones, in registration order. */
class ISCInteractionProvider
{
public:
    virtual ~ISCInteractionProvider() = default;
    /** The thing under the view ray (Eye along View, within Reach) as popout rows: Key, Options and an anchor; false when
     *  there is none. */
    virtual bool BuildPrompt(const ASCFirstPersonCharacter& Character, const FVector& Eye, const FVector& View, float Reach,
                             FSCInteractionPrompt& Out) const = 0;
    /** E on it with the popout's Option selected; false when there is nothing of this kind under the view. */
    virtual bool Interact(ASCFirstPersonCharacter& Character, const FVector& Eye, const FVector& View, float Reach, int32 Option) = 0;
};

namespace SCViewerShell
{
/** The game world the viewer shows (the game viewport's world). */
SCBRIDGE_API UWorld* ViewerWorld();

/** The menu F1 opens, by the name help lines use; without one F1 does nothing. */
SCBRIDGE_API void SetMenu(const FText& Name);
/** No menu (an application's shutdown). */
SCBRIDGE_API void ClearMenu();
SCBRIDGE_API bool HasMenu();
SCBRIDGE_API FText MenuName();
/** Its open state: on the first-person character while there is one (its input pauses mouse look), else a UI-level flag
 *  (orbit / fly cameras). Opening is ignored while no menu is registered. */
SCBRIDGE_API bool IsMenuOpen(USCViewerSubsystem* Viewer);
SCBRIDGE_API void SetMenuOpen(USCViewerSubsystem* Viewer, bool bOpen);

using FMakeOverlay = TFunction<TSharedRef<SWidget>(const FSCViewOverlayArgs&)>;
/** A layer over the view, above the first-person HUD; drawn in registration order. Takes effect for roots built after. */
SCBRIDGE_API void AddOverlay(FName Name, FMakeOverlay Make);
SCBRIDGE_API void RemoveOverlay(FName Name);
SCBRIDGE_API TArray<FMakeOverlay> Overlays();

SCBRIDGE_API void AddInteraction(FName Name, TSharedRef<ISCInteractionProvider> Provider);
SCBRIDGE_API void RemoveInteraction(FName Name);
SCBRIDGE_API TArray<TSharedRef<ISCInteractionProvider>> Interactions();
} // namespace SCViewerShell
