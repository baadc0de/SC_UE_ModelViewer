#pragma once
#include "CoreMinimal.h"
#include "SCPackageSource.h"
#include "Cargo/SCCargoTypes.h"
#include "SCInteractionRecords.h"
#include "SCPackageReader.generated.h"

/** `ui_bindings[].state_images[]` (WP-I1 I1-3): the render of the UI owner in one other state of its interaction state
 *  machine (`StateType` / `State`: the game's names, `DoorOpenState` / `DoorOpened`). */
struct SCBRIDGE_API FSCUiStateImage
{
    FString StateType, State; // a single-state render; empty for a combined one
    /** Every pinned state of the render, "<StateType>.<State>" (cache format 39 `states[]`: one for a single-state render,
     *  two for a combined one), sorted. */
    TArray<FString> States;
    FString ImagePath; // normalized package path
    /** "<StateType>.<State>" for a single-state render (the key a screen face's state images are held by); the sorted
     *  states joined with "+" for a combined one. */
    FString Key() const { return States.Num() > 1 ? FString::Join(States, TEXT("+")) : StateType + TEXT(".") + State; }
};
/** `ui_bindings[].buttons[]` (cache format 39): a clickable node of the default render, the owner interaction its
 *  left click sends, and its rectangle in the generated image's pixels. */
struct FSCUiButton
{
    FString Node;
    FName Interaction;
    FBox2D RectPx = FBox2D(ForceInit);
};
/** `ui_bindings[].state_types[]`: one state type of the UI owner's state machine, its default, the owner's own
 *  interactions that set a state (`transitions`) and the linked entities' state tags that set one (`linked`). */
struct FSCUiStateType
{
    FString Name, DefaultState;
    TArray<FString> States;
    /** The owner's own interaction -> the state it sets, from state From ("" = from any state). */
    struct FTransition { FString From; FName Interaction; FString State; };
    TArray<FTransition> Transitions;
    /** The state Interaction sets from Current (a transition from Current first, else one from any state); null without. */
    const FString* Next(FName Interaction, const FString& Current) const
    {
        const FTransition* T = Transitions.FindByPredicate([&](const FTransition& X) { return X.Interaction == Interaction && X.From == Current; });
        if (!T) T = Transitions.FindByPredicate([&](const FTransition& X) { return X.Interaction == Interaction && X.From.IsEmpty(); });
        return T ? &T->State : nullptr;
    }
    TArray<TPair<FString, FString>> Linked; // tag path -> state
};
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
    FString SourceItemType;    // source_item_type: the UI owner's AttachDef.Type ("Display", "ControlPanel", ...; "" when null)
    bool bClassified = false;  // the export carries source_item_type and source_owner_resolved; an older export cannot be classified
    bool bOwnerResolved = false; // source_owner_resolved: the UI owner's record was found (else its item type is unknown)
    bool bOperated = false;    // source_kiosk_interaction: the owner's UI is operated by the player (door, light, transit panels, kiosks)
    bool bPropagated = false;  // on a child: the binding of another (geometry-less) entity carried by this one
    /** Cache format 40 (`interaction_distance`): how far from its screen the player operates the owner's UI, metres
     *  (UIRenderToTextureEntityComponentParams.interactionDistance); 0 when not exported. */
    float InteractionDistance = 0.f;
    bool bInteractionDistanceExported = false; // the key is present (null included): an export before format 40 has none
    /** The owner's other states the canvas shows (`state_images[]`; images outside the package dropped). */
    TArray<FSCUiStateImage> StateImages;
    /** Cache format 39: the clickable nodes of the default render (`buttons[]`) in an image of ImageSizePx pixels, and
     *  the owner's state types (`state_types[]`). */
    TArray<FSCUiButton> Buttons;
    FVector2D ImageSizePx = FVector2D::ZeroVector;
    TArray<FSCUiStateType> StateTypes;

    /** WP-I1 rule (owner decision: door and lift panels are the game's, HUDs and MFDs are the viewer's own UI): the UI
     *  of an operating panel. Derived from data only: a `physical` binding of a resolved owner that is not a display
     *  item (`Display`: vehicle MFDs, physical screens, HUD projectors, radar displays, signage) or is a display the
     *  player operates (a kiosk), whoever carries it (a door-control child's binding propagated to its parent stays a
     *  panel; a self-owned cockpit screen does not). */
    bool IsOperatingPanel() const
    {
        return bClassified && bOwnerResolved && Kind == TEXT("physical") && (SourceItemType != TEXT("Display") || bOperated);
    }
    /** The image this binding puts on its screen face: an operating panel's render, on by default; "" = dark glass. */
    FString PanelImage() const { return IsOperatingPanel() && !bDefaultStateIsOff ? ImagePath : FString(); }
    /** The state images a face showing PanelImage() can swap to (none when PanelImage() is ""). */
    const TArray<FSCUiStateImage>& PanelStateImages() const
    {
        static const TArray<FSCUiStateImage> None;
        return PanelImage().IsEmpty() ? None : StateImages;
    }
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
    FString EntityClassGuid; // WP-A1: the entity class (audio effects by class); empty for geometry without one
    FString EntityId;
    TArray<FString> Links;
    FString LoadoutOwnerEntityId;
    /** WP-P5 (interior placements): the `.soc` entity name, its interaction links, the entity it rides, its door timing
     *  and the interaction points of its class. */
    FString SocEntityName;
    TArray<FSCInteractionLink> InteractionLinks;
    FSCParentAttachment Parent;
    FSCDoorTiming Door; // placements and (cache format 39) loadout children
    TArray<FSCInteractionPoint> InteractionPoints;
    /** Cache format 39: interactions forwarded to the items of vehicle ports, and the Sendable interactions with tags. */
    TArray<FSCItemPortLink> ItemPortLinks;
    TArray<FSCSendableInteraction> SendableInteractions;
    /** `ui_bindings[]`: the UI screens this entity carries (WP-I1). */
    TArray<FSCUiBinding> UiBindings;
    /** Every image a screen face of this instance may show (FSCUiBinding::PanelImage), decoded with the material
     *  textures. `Excluded` counts bindings with a render left unbound by the rule (vehicle screens, displays),
     *  `Unclassified` those of an export without source_item_type or of an unresolved owner (left dark). */
    void PanelImages(TSet<FString>& Out, int32* Excluded = nullptr, int32* Unclassified = nullptr) const;
    /** WP-I1 per-face rule: what one screen material slot of this instance shows; "" = dark glass.
     *  MeshNodes: the lower-case names of the mesh's nodes. Owners: one entry per node owning the slot's triangles in
     *  the drawn variant / LOD, that node's lower-case name followed by its ancestors'. A binding whose helper names a
     *  node of the mesh owns that node's subtree (the nearest such helper on the chain); the entity's own other bindings
     *  own every face no helper claims, and a propagated binding (bPropagated) whose helper is not on the mesh claims none. A face nobody owns is dark; a slot whose faces resolve to
     *  different outcomes (two images, or an image and a dark screen) cannot be told apart and stays dark, with
     *  bAmbiguous set. */
    FString ScreenSlotImage(const TSet<FString>& MeshNodes, const TArray<TArray<FString>>& Owners, bool* bAmbiguous = nullptr) const;
    /** The operating-panel binding whose default image is Image (ScreenSlotImage's outcome), or null. */
    const FSCUiBinding* PanelBinding(const FString& Image) const
    {
        return Image.IsEmpty() ? nullptr : UiBindings.FindByPredicate([&](const FSCUiBinding& B) { return B.PanelImage() == Image; });
    }
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
    /** WP-A1: `audio_spots[]` / `audio_areas[]` objects verbatim (container-local CryEngine metres); read by FSCAudioScene. */
    TArray<FString> AudioSpotJson, AudioAreaJson, RoomConnectorJson;
    UPROPERTY(BlueprintReadOnly) TArray<FSCSceneInstance> Placements;
    TArray<FSCProximitySensor> ProximitySensors;
    /** WP-P5: geometry-less interactive entities (`entities[]`) and lift carriages (`lifts[]`). */
    TArray<FSCInteriorEntity> Entities;
    TArray<FSCLiftRecord> Lifts;
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
