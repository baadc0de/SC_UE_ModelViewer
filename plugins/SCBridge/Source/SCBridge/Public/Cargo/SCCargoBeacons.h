// Freestyle cargo beacons (WP-K2): the pure rules, no world. Which beacon a loose box at rest joins (same origin and
// owner, near a member), the random names, and the order a load let go of in the air tries destinations in (nearest
// first). The subsystem keeps the beacons (SCCargoBeacons.cpp); automation-tested (SCBridge.Cargo.Beacon*).
#pragma once
#include "CoreMinimal.h"

struct SCBRIDGE_API FSCCargoBeaconModel
{
    int32 Id = INDEX_NONE;
    int32 OriginId = INDEX_NONE; // the location its boxes were carried from (INDEX_NONE: put there)
    FName Owner;
    TArray<int32> Boxes;
};

class SCBRIDGE_API FSCCargoBeacons
{
public:
    /** How near two loose boxes must lie to share a beacon: the larger of the two boxes' longest sides, between their
     *  bounds (cm). Nothing else in the data says how far a clump reaches; a box's own size is the scale it is seen at. */
    static double NearCm(const FBox& A, const FBox& B);
    /** The beacon a box at rest (Bounds, from Origin, of Owner) joins: one with the same origin and owner with a member
     *  near it (MemberBounds: every member's bounds), the nearest such; INDEX_NONE: none, it founds one. */
    static int32 Join(const TArray<FSCCargoBeaconModel>& Beacons, const TMap<int32, FBox>& MemberBounds, const FBox& Bounds, int32 OriginId, FName Owner);
    /** "Loose cargo Kestrel-42": a word and a number from Rand, not one of Taken (tries a few, then appends a counter). */
    static FString Name(TFunctionRef<int32(int32 Max)> Rand, const TSet<FString>& Taken);
    /** Candidate destinations (location id, bounds) for a load at P, nearest first by distance to their bounds; those in
     *  Tried left out. */
    static TArray<int32> NearestFirst(const TArray<TPair<int32, FBox>>& Candidates, const FVector& P, const TSet<int32>& Tried);
};
