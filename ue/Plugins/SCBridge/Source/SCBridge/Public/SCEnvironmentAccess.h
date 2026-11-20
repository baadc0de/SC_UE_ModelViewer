// Access to inventory containers (WP-K1): the moving parts between a container's volume and the open space around it
// (an elevator's front gate, a shutter, a lid, the platform under it) take the authored end pose that clears the way.
// The rule is geometric: which pose of a rig leaves the container's access corridors free, never a name.
#pragma once
#include "CoreMinimal.h"

class ASCVehicle;
class USCRigComponent;
class USCCargoGridComponent;

/** One rig the access rule considered and the pose it kept. */
struct FSCAccessMove
{
    TWeakObjectPtr<USCRigComponent> Rig;
    int32 Clip = INDEX_NONE; // clip whose last frame is the kept pose; INDEX_NONE = the current pose was kept
    FName ClipId;
    double BlockedBefore = 0, BlockedAfter = 0; // m3 of the rig's collidable parts inside the access corridors
    float Seconds = 0;                          // the clip's length (0 when kept)
    bool bDoor = false; // moved by a door interaction (ASCVehicle::PlayDoor): waited for by its playback, not Seconds
    int32 Door = INDEX_NONE; // that interaction (ASCVehicle::Doors index); also a door found open that the corridors need
};

struct FSCAccessResult
{
    int32 RigsTested = 0;
    TArray<FSCAccessMove> Moves; // rigs whose pose changed (their clips are playing)
    float Seconds = 0;           // until every moved rig is at its end pose
};

namespace FSCEnvironmentAccess
{
/** Opens the way into Volume (a box in Frame, cm; Frame's Z is the container's up, its floor at Volume.Min.Z).
 *  Candidates per rig of Vehicle: its current pose and each bound clip's last frame. The kept pose has the least
 *  collidable geometry in the access corridors (Volume shrunk by ToleranceCm and swept by ReachCm along +-X, +-Y and
 *  +Z: everything that would stop an item leaving the volume sideways or upwards); at equal obstruction a pose in which
 *  a part is the floor (its top within FloorBandCm under the floor, covering most of the footprint: a platform raised
 *  to the container) wins; otherwise the current pose stays. Rigs whose kept pose differs play that clip to its end.
 *  The request stays registered under Key: FSCEnvironment::SetState re-poses the environment and then calls Reapply.
 *  Cooked environments (rigs baked static) have no rigs and report RigsTested = 0. */
SCBRIDGE_API FSCAccessResult Open(ASCVehicle* Vehicle, FName Key, const FTransform& Frame, const FBox& Volume, double ReachCm,
                                  double ToleranceCm, double FloorBandCm, double MinGainM3 = 1e-3);
/** Open for a cargo grid: its lattice box (LatticeToWorld, InteriorM); reach = the largest permitted item edge (an
 *  item of that size must be able to leave the volume), tolerance = the authored grid cell size (parts reaching less
 *  than a cell into the corridors, e.g. the platform the floor rests on, do not count), floor band = the minimum
 *  permitted item height (one lattice layer), least gain = one lattice cell's volume (a pose that clears less than a
 *  cell of the corridors is not opening anything an item could pass: an unrelated clip, e.g. a turret deploying, is
 *  not played). Key "grid:<GridId>". */
SCBRIDGE_API FSCAccessResult OpenGrid(const USCCargoGridComponent* Grid);
SCBRIDGE_API FName GridKey(const USCCargoGridComponent* Grid);
/** Forget a request (poses stay as they are; nothing closes). */
SCBRIDGE_API void Release(ASCVehicle* Vehicle, FName Key);
/** Re-run every registered request of Vehicle (after its pose was reset); returns the rigs moved. */
SCBRIDGE_API int32 Reapply(ASCVehicle* Vehicle);
/** Requests registered for Vehicle. */
SCBRIDGE_API int32 NumRequests(const ASCVehicle* Vehicle);
}
