// The live view while the mobiGlas is open (WP-K1c, owner feedback): an orbit camera around the cargo being worked on,
// driven by drag / wheel on the free 3D area. In first person the player stays exactly where it stands: the player
// controller's view target switches to the viewer's orbit pawn (ASCViewerPawn, the same orbit math as the Orbit
// camera mode) and back on close. In the orbit / fly camera modes the possessed viewer pawn is driven directly.
#pragma once
#include "CoreMinimal.h"

class ASCViewerPawn;
class ASCFirstPersonCharacter;
class USCViewerSubsystem;

class FSCMobiGlasOrbit
{
public:
    /** Start orbiting Focus (world box, cm); first person: from the player's eye. */
    void Begin(USCViewerSubsystem* Viewer, const FBox& Focus);
    /** Back to the player's own view (first person) / leave the viewer pawn where it is (other modes). */
    void End();
    bool IsActive() const { return bActive; }
    /** Ease to a new focus (another group selected). */
    void Refocus(const FBox& Focus);
    void Rotate(const FVector2D& Pixels);
    void Pan(const FVector2D& ViewHeightFraction);
    void Zoom(float Notches);
    /** First person: a move of the player from outside (console teleport, sc.Cargo.View) while orbiting re-targets the
     *  orbit to that viewpoint, so the rendered view is never stale; call every frame. */
    void Sync();
    /** The live view's vertical extent as fractions of the view height (top, bottom): framing fits the focus into it. */
    void SetBand(const FVector2D& TopBottom) { Band = TopBottom; }
    /** Horizontal direction of the drones' work (source -> destination), when a transfer runs: wide framing looks across it. */
    void SetWorkAxis(const TOptional<FVector2D>& Axis) { WorkAxis = Axis; }

private:
    ASCViewerPawn* Pawn() const;
    TWeakObjectPtr<USCViewerSubsystem> Viewer;
    TWeakObjectPtr<ASCViewerPawn> OrbitPawn;          // first person: the un-possessed viewer pawn we view through
    TWeakObjectPtr<ASCFirstPersonCharacter> Player;   // first person: whose view comes back on close
    bool bActive = false, bSpawnedPawn = false, bPlayerWasThirdPerson = false;
    FVector PlayerAt = FVector::ZeroVector; // player location / control rotation last seen (the player stands still)
    FRotator PlayerView = FRotator::ZeroRotator;
    FBox LastFocus = FBox(ForceInit);
    FVector2D Band = FVector2D(0, 1);
    TOptional<FVector2D> WorkAxis;
    /** Spring-arm framing of Focus into the live band (see the .cpp); bSnap: the move is a jump (open), else a re-focus. */
    void Frame(ASCViewerPawn* P, const FBox& Focus, const FVector& From, bool bSnap);
};
