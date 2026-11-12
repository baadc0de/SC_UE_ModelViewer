// Room volumes of interior object containers and the light confinement derived from them (WP-U3b §12, WP-L1).
// The game shades a light flagged `affects_this_area_only` only on pixels of the clip volume it sits in
// (TiledShading GetClipVolumeWeightBinary; the root volume is "outside"). UE has no per-pixel clip volumes, so the
// rule is mapped onto the three lighting channels: channel 0 = outside every room volume; each room takes channel 1
// or 2, coloured so that rooms an area-only light reaches across do not share one (docs/work-packages/WP-L1-*.md).
#pragma once
#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
class ASCVehicle;
class USceneComponent;
class UPrimitiveComponent;
struct FMeshDescription;

/** One `interiors[].room_volumes[]` segment: container-local CryEngine axes, metres. */
struct SCBRIDGE_API FSCRoomVolume
{
    enum class EShape : uint8 { Box, Prism, Trimesh };
    EShape Shape = EShape::Box;
    FString Room, RoomName, Source; // room GUID; authored names are labels only
    /** Container-local -> segment space (row vectors): the inverse of the exported `transform`. */
    FMatrix FromContainer = FMatrix::Identity;
    FVector Min = FVector::ZeroVector, Max = FVector::ZeroVector; // box
    TArray<FVector> Points; // prism: polygon (xy; the base is the lowest z) / trimesh: three vertices per triangle
    double Height = 0;      // prism extrusion along +z
    static bool Parse(const FString& Json, FSCRoomVolume& Out, FString& Error);
    /** Same tests as the exporter's RoomVolume::contains (box / even-odd polygon / ray-crossing parity). */
    bool Contains(const FVector& ContainerPointM) const;
    /** Distance (m) from a container point to the volume's axis-aligned bounds in its own segment space; 0 inside
     *  them. A lower bound of the distance to the volume, used for the light-reach test. */
    double BoundsDistance(const FVector& ContainerPointM) const;
};

/** The room volumes of one container, attached to the component its lights and placements hang from. */
struct SCBRIDGE_API FSCLightArea
{
    TWeakObjectPtr<USceneComponent> Anchor;
    FMatrix AnchorToContainer = FMatrix::Identity; // anchor-local cm (UE axes) -> container-local metres
    TArray<FSCRoomVolume> Volumes;
    bool Contains(const FVector& World) const;
    /** The room GUID of the first volume containing the world point; empty outside every volume. */
    FString RoomAt(const FVector& World) const;
    /** Smallest BoundsDistance (m) from a world point to the volumes of one room in this area; +inf without any. */
    double RoomDistance(const FVector& World, const FString& Room) const;
};

class SCBRIDGE_API FSCLightAreas
{
public:
    /** Vertex positions kept per mesh for the inside/outside vote (a sampling budget, not game data). */
    static constexpr int32 MaxSamples = 64;
    /** Rooms coloured by trying every split (2^(n-1)); larger room sets fall back to a greedy colouring (a search budget). */
    static constexpr int32 MaxExactRooms = 16;
    /** Up to MaxSamples vertex positions, evenly strided by vertex index (mesh-local cm). */
    static TArray<FVector3f> Sample(const FMeshDescription& Description);
    /** Parse a container's room volumes and keep them on the vehicle; malformed volumes warn and are dropped. */
    static void Register(ASCVehicle* Vehicle, USceneComponent* Anchor, const TArray<FString>& VolumeJson, bool bHelperFrame,
                         TArray<FString>& Warnings);
    /** Classify every light and primitive of the vehicle by room and set their lighting channels; lights of the world
     *  that belong to no vehicle (hangar, sun) are extended to every channel. A vehicle without volumes is left alone. */
    static void Apply(ASCVehicle* Vehicle);
    /** Rooms -> lighting channel 1 or 2. Reach holds one (room A, room B) entry per area-only light of A that reaches B;
     *  the colouring minimises the reaching lights between rooms that share a channel (every colouring is tried up to
     *  MaxExactRooms rooms; greedy above), ties to the more balanced split, the first room on channel 1. Conflicts =
     *  room pairs left on one channel while a light reaches across (odd reach cycles). */
    static TMap<FString, int32> ColorRooms(const TArray<FString>& Rooms, const TArray<TPair<FString, FString>>& Reach,
                                           int32& Conflicts);
    /** Only the given channel (0 = outside, 1/2 = a room's channel). */
    static void SetChannel(UPrimitiveComponent* Component, int32 Channel);
};
