// Room volumes of interior object containers and the light confinement derived from them (WP-U3b §12).
// The game shades a light flagged `affects_this_area_only` only on pixels of the clip volume it sits in
// (TiledShading GetClipVolumeWeightBinary; the root volume is "outside"). UE has no per-pixel clip volumes, so the
// rule is collapsed to two lighting channels: channel 0 = outside every room volume, channel 1 = inside one.
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
};

/** The room volumes of one container, attached to the component its lights and placements hang from. */
struct SCBRIDGE_API FSCLightArea
{
    TWeakObjectPtr<USceneComponent> Anchor;
    FMatrix AnchorToContainer = FMatrix::Identity; // anchor-local cm (UE axes) -> container-local metres
    TArray<FSCRoomVolume> Volumes;
    bool Contains(const FVector& World) const;
};

class SCBRIDGE_API FSCLightAreas
{
public:
    /** Vertex positions kept per mesh for the inside/outside vote (a sampling budget, not game data). */
    static constexpr int32 MaxSamples = 64;
    /** Up to MaxSamples vertex positions, evenly strided by vertex index (mesh-local cm). */
    static TArray<FVector3f> Sample(const FMeshDescription& Description);
    /** Parse a container's room volumes and keep them on the vehicle; malformed volumes warn and are dropped. */
    static void Register(ASCVehicle* Vehicle, USceneComponent* Anchor, const TArray<FString>& VolumeJson, bool bHelperFrame,
                         TArray<FString>& Warnings);
    /** Classify every light and primitive of the vehicle and set their lighting channels; lights of the world that
     *  belong to no vehicle (hangar, sun) are extended to both channels. A vehicle without volumes is left alone. */
    static void Apply(ASCVehicle* Vehicle);
    /** Channel 1 only when inside, channel 0 only (the engine default) when outside. */
    static void SetChannels(UPrimitiveComponent* Component, bool bInside);
};
