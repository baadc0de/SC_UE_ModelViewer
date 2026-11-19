#pragma once
#include "CoreMinimal.h"
#include "SCPackageSource.h"
#include "Cargo/SCCargoTypes.h"
#include "SCPackageReader.generated.h"

/** `ui_bindings[]` of a child or placement (decomposed-export-contract "UI bindings"): a UI screen the entity carries,
 *  its game role and the exporter's render of its default state (WP-I1). */
struct SCBRIDGE_API FSCUiBinding
{
    FString Kind;              // binding_kind: "physical" (a screen of the entity), "mfd", "radar"
    FString SourceEntityName;  // the entity whose UI component this is (the item itself, or a screen entity mounted on it)
    FString HelperName;        // the mesh node of the screen, when named
    FString CanvasRecordName, ContentCanvasRecordName;
    bool bDefaultStateIsOff = false;
    FString ImagePath;         // generated_image_path, normalized package path ("" without a render)
    float AspectWOverH = 0.f;  // ui_screen_aspect_w_over_h, 0 when not measured
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCSceneInstance
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString EntityName;
    UPROPERTY(BlueprintReadOnly) FString MeshAsset;
    UPROPERTY(BlueprintReadOnly) FString MaterialSidecar;
    /** Resolved by the reader: own id, container's, the material set's DefaultPalette, then the host's. */
    UPROPERTY(BlueprintReadOnly) FString PaletteId;
    UPROPERTY(BlueprintReadOnly) FString ParentEntityName;
    UPROPERTY(BlueprintReadOnly) FString ParentNodeName;
    UPROPERTY(BlueprintReadOnly) FString ItemType;
    UPROPERTY(BlueprintReadOnly) FString PortFlags;
    UPROPERTY(BlueprintReadOnly) FString SourceTransformBasis;
    UPROPERTY(BlueprintReadOnly) bool bInvisible = false;
    UPROPERTY(BlueprintReadOnly) TArray<FString> LightJson; // lossless state/style/attachment metadata
    /** Instance id (`children[].id`, player kits); unique where entity names repeat. */
    UPROPERTY(BlueprintReadOnly) FString Id;
    /** Id of the enclosing child for nested assemblies (`children[].children[]`); resolves the parent before the name. */
    UPROPERTY(BlueprintReadOnly) FString ParentId;
    /** Lossless authored item-port record (`port`), empty when the instance has none. */
    UPROPERTY(BlueprintReadOnly) FString PortJson;
    /** Item-port names from the root entity to this item (`port_path`, WP-K1); empty for the root and placements. */
    UPROPERTY(BlueprintReadOnly) TArray<FString> PortPath;
    /** `port.attachment == "bone"` without a resolved local transform: placed by aligning the port's
     *  item_offset_helper with the parent joint plus the port offset (contract "Player kits"). */
    bool bBonePort = false;
    FString PortHelper;           // port.item_offset_helper ("" = the item's source frame)
    FMatrix PortOffset = FMatrix::Identity; // port.offset in native SC axes (row-vector, metres)
    FString ImportWarning; // Invalid optional instance: retain identity to suppress descendants.
    FMatrix LocalMatrix = FMatrix::Identity;
    /** Interior placements: the object-container entity id (unique within its container), the ids its EntityLinks
     *  name (a door panel names its door), and the entity whose inline loadout attached it (a door's own panels). */
    FString EntityId;
    TArray<FString> Links;
    FString LoadoutOwnerEntityId;
    /** `ui_bindings[]`: the UI screens this entity carries (WP-I1). */
    TArray<FSCUiBinding> UiBindings;
    /** The image of this entity's own operating panel (door / lift / light panel, terminal), or "" when it has none
     *  (contract rule, WP-I1): only `physical` bindings with a render and not off by default count; MFD and radar
     *  bindings are vehicle screens. On a child (`bPlacement` false), a binding whose source entity is not the child
     *  itself is a screen entity mounted on it (a dashboard's HUD projector or physical vehicle screen) and does not
     *  count either. An interior placement is the panel entity itself. Every vehicle-screen binding with a render adds
     *  one to `Skipped`. Bindings with different images (a multi-screen panel) need the helper -> face match, which
     *  is not implemented: none is returned and `bAmbiguous` is set. */
    FString PanelImage(bool bPlacement, int32* Skipped = nullptr, bool* bAmbiguous = nullptr) const;
};
/** `interiors[].proximity_sensors[]`: a door proximity sensor in its container's placement frame (CryEngine metres). */
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCProximitySensor
{
    GENERATED_BODY()
    FString EntityClass, EntityId;
    FMatrix LocalMatrix = FMatrix::Identity;
    /** Box half-extents and centre offset in the sensor's own frame (metres). */
    FVector Extent = FVector::ZeroVector, Offset = FVector::ZeroVector;
    float ExitDelay = 0;
    TArray<FString> Links; // entity ids of the doors it drives, in the same container
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCInterior
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FSCSceneInstance Container;
    /** `room_volumes[]` objects verbatim (container-local CryEngine metres); parsed by FSCRoomVolume. */
    UPROPERTY(BlueprintReadOnly) TArray<FString> RoomVolumeJson;
    UPROPERTY(BlueprintReadOnly) TArray<FSCSceneInstance> Placements;
    TArray<FSCProximitySensor> ProximitySensors;
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCSubmaterial
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) int32 Index = 0; // Array ordinal / GLB primitive slot per contract.
    UPROPERTY(BlueprintReadOnly) int32 SourceIndex = 0; // Authored material index can be sparse.
    UPROPERTY(BlueprintReadOnly) FString Name;
    UPROPERTY(BlueprintReadOnly) FString ShaderFamily;
    UPROPERTY(BlueprintReadOnly) bool bBlended = false;
    // Lossless metadata for WP-U2, including texture roles and authored parameters.
    UPROPERTY(BlueprintReadOnly) FString Json;
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCMaterialSet
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString Path;
    UPROPERTY(BlueprintReadOnly) TArray<FSCSubmaterial> Submaterials;
    UPROPERTY(BlueprintReadOnly) FString ContentHash;
    /** Palette id named by the .mtl's authored `DefaultPalette` attribute; empty when the set authors none. */
    UPROPERTY(BlueprintReadOnly) FString DefaultPaletteId;
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCPalette
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString Id;
    UPROPERTY(BlueprintReadOnly) FString Json;
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCClipRecord
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString Name;
    UPROPERTY(BlueprintReadOnly) FString Sidecar;
    UPROPERTY(BlueprintReadOnly) FString Json;
};
/** controls.engine_glow target: main-thruster glow submaterial by sidecar + authored material index. */
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCEngineGlowTarget
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString MaterialSidecar;
    UPROPERTY(BlueprintReadOnly) int32 SourceMaterialIndex = -1;
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCEngineGlow
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) float MinStrength = 0, MaxStrength = 0, DefaultStrength = 0;
    /** `material_property_glow` (CryEngine Glow value) or legacy `emission_strength`. */
    UPROPERTY(BlueprintReadOnly) FString Units;
    UPROPERTY(BlueprintReadOnly) TArray<FSCEngineGlowTarget> Targets;
};
/** `paints.json` `paint_variants[]`: a paint the vehicle can wear (the game's tint system): its tint palette and the
 *  exterior material file it puts on the parts that use the vehicle's own exterior material file. */
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCPaintVariant
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString DisplayName;   // empty for unnamed paints
    UPROPERTY(BlueprintReadOnly) FString PaletteId;
    UPROPERTY(BlueprintReadOnly) FString ExteriorSidecar; // empty: the paint keeps every material file
    UPROPERTY(BlueprintReadOnly) FString SubgeometryTag;
    /** Label for pickers: the display name, else the tag. */
    FString Label() const { return DisplayName.IsEmpty() ? SubgeometryTag : DisplayName; }
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCSceneManifest
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString ScenePath;
    UPROPERTY(BlueprintReadOnly) FSCSceneInstance Root;
    UPROPERTY(BlueprintReadOnly) TArray<FSCSceneInstance> Children;
    UPROPERTY(BlueprintReadOnly) TArray<FSCInterior> Interiors;
    UPROPERTY(BlueprintReadOnly) TArray<FSCMaterialSet> Materials;
    UPROPERTY(BlueprintReadOnly) TArray<FSCPalette> Palettes;
    UPROPERTY(BlueprintReadOnly) TArray<FSCPaintVariant> Paints;
    UPROPERTY(BlueprintReadOnly) TArray<FSCClipRecord> Clips;
    UPROPERTY(BlueprintReadOnly) FSCEngineGlow EngineGlow;
    UPROPERTY(BlueprintReadOnly) FString ContentHash;
    /** WP-P3: `root_entity.physics_grid` as a triangle soup in the root entity's UE frame (cm); empty without one. */
    TArray<FVector> PhysicsGrid;
    TArray<int32> PhysicsGridShapeEnds; // end (exclusive) in PhysicsGrid of each shape: every shape is its own volume
    /** WP-K1: `inventory_containers[]` of the vehicle (loadout items) and of its interiors (InteriorIndex set). */
    UPROPERTY(BlueprintReadOnly) TArray<FSCInventoryContainerRecord> InventoryContainers;
    /** WP-K1: the root item's footprint (`root_entity.inventory_occupancy` & co.); bValid false without one. */
    UPROPERTY(BlueprintReadOnly) FSCItemOccupancy RootOccupancy;
    TArray<FString> MeshAssets() const;
    /** What paint Variant puts on an instance (resolved palettes): an instance on the vehicle's own exterior material
     *  file takes the paint's exterior file; one on the vehicle's palette takes the paint's palette. False when the
     *  paint does not touch the instance. */
    bool PaintTarget(const FSCSceneInstance& I, int32 Variant, FString& OutSidecar, FString& OutPalette) const;
};
class SCBRIDGE_API FSCPackageReader
{
public:
    // Empty ScenePath selects the sole manifest; shared roots with multiple scenes require an explicit path.
    static bool Read(const FSCPackageSource& Source, FSCSceneManifest& Out, FString& Error, const FString& ScenePath = TEXT(""), TArray<FString>* Warnings = nullptr);
    static FString Hash(const TArray<uint8>& Bytes);
    /** `port.offset` as a native-SC-axes row-vector matrix (metres). */
    static FMatrix PortOffset(const TSharedPtr<class FJsonObject>& Port);
};
