// WP-P5 floor panel: the viewer's own floor-select UI on a lift panel's screen face. The game draws none on the C2's
// inner floor panel (WP-I1 research); the owner approved our design, in the look of the vehicle's door pads.
//
// What is data: the options (the panel's interactions that ask a carriage for a stop, `From Elevator call <N>`), their
// labels (the stops' names, as white glyph masks in the door pads' label font, `lifts[].panel_style.labels[]`), the
// colours (the pads' brand palette by BB_ColorStyle role, `panel_style.palette`), the lift's state (current stop, the
// leg, the queue) and the request E sends (the panel's own interaction, through ASCVehicle::SendInteraction).
// What is ours: the layout (FSCFloorPanelLayout), which role colours what (FSCFloorPanelColours), the markers and the
// blink (FSCFloorPanelView).
//
// - FSCScreenFaceGeometry: a screen face's triangles and UVs, the frame an upright, unmirrored image uses on it, the
//   UV -> image map M_SC_Screen samples through (ScreenUVTransformX / Y), and the ray test the aim uses.
// - FSCFloorPanelLayout: rows top to bottom; OptionAt maps an image point to the row under it.
// - FSCFloorPanelView: what the face shows; the panel is drawn again when it changes.
// - FSCFloorPanel: one drawn panel (its face, layout, render target and label masks), owned by ASCVehicle.
#pragma once
#include "CoreMinimal.h"
#include "MeshDescription.h"
#include "SCInteractionRecords.h"
#include "SCPackageReader.h"

class FSCLift;
class UMeshComponent;
class USceneComponent;
class UTexture2D;
class UTextureRenderTarget2D;
class UMaterialInterface;

/** A screen face (one material slot of one mesh) in its component's space (cm): triangles, UV0, and the image frame.
 *  Image coordinates ("content"): x to the right, y down, 0..1 over the face's extent, as seen from the face's front
 *  (its vertex normals) with "up" the hint given to Build (the vehicle's up). */
struct SCBRIDGE_API FSCScreenFaceGeometry
{
    TArray<FVector3f> Positions; // three per triangle
    TArray<FVector2f> UVs;       // UV0, per corner
    TArray<FVector3f> Fronts;    // per triangle: its corners' summed vertex normals
    TArray<int32> Nodes;         // per triangle: the rig node that owns it (INDEX_NONE without)
    TArray<bool> Soft;           // per triangle: skinned (drawn by the rig component), else rigid
    // Build's results.
    FVector Normal = FVector::ZeroVector, Right = FVector::ZeroVector, Up = FVector::ZeroVector;
    double RightMin = 0, UpMax = 0, Plane = 0;
    double Width = 0, Height = 0; // cm
    /** Content from UV: x = X.X u + X.Y v + X.Z, y = Y.X u + Y.Y v + Y.Z (least squares over the corners). */
    FVector X = FVector(1, 0, 0), Y = FVector(0, 1, 0);
    /** The largest content-space residual of that fit (0 for a planar, affine unwrap). */
    double FitError = 0;

    /** The triangles of material slot Slot of Description, each with its owning node (NodeOf). The mesh build drops the
     *  description, so the importer captures its screen slots while it still has it. */
    static bool FromDescription(const FMeshDescription& Description, int32 Slot, TFunctionRef<int32(FTriangleID)> NodeOf,
                                TFunctionRef<bool(FTriangleID)> SoftOf, FSCScreenFaceGeometry& Out);
    /** The triangles of one drawn part: skinned (bSoft, the rig component) or rigid, owned by node Owner (any for
     *  INDEX_NONE); empty when it has none. */
    FSCScreenFaceGeometry Select(int32 Owner, bool bSoft) const;
    /** The geometry under a component's scale (a reflected or stretched placement): positions scaled, fronts by the
     *  inverse. The face is then handled in the component's frame without scale (FSCFloorPanel::Frame). */
    FSCScreenFaceGeometry Scaled(const FVector& Scale) const;
    /** Frame and UV map. UpHint: the up direction in component space. False for a degenerate face. */
    bool Build(const FVector& UpHint);
    FVector2D ContentFromUV(const FVector2f& UV) const;
    /** A component-space ray against the face's front: the distance along Dir (normalized) and the UV there. */
    bool Raycast(const FVector& Origin, const FVector& Dir, double MaxDistance, double& OutDistance, FVector2f& OutUV) const;
    /** The component-space point of content point C on the face's plane. */
    FVector PointAt(const FVector2D& C) const;
    double Aspect() const { return Height > 0 ? Width / Height : 1; }
};

/** One option: a panel interaction that asks the carriage for a stop, labelled with that interaction's display name (else
 *  the stop's). Rect in content coordinates. */
struct FSCFloorPanelOption
{
    int32 Stop = INDEX_NONE;
    FName Interaction;
    FString Label;
    FBox2D Rect = FBox2D(ForceInit);
    FString Node; // a Buttons panel: the render's node (`buttons[].node`), whose hover render shows it aimed at
};

/** Our layout: the rows top to bottom in the order given, over the face above a thin status bar. */
struct SCBRIDGE_API FSCFloorPanelLayout
{
    TArray<FSCFloorPanelOption> Options;
    FBox2D Status = FBox2D(ForceInit);
    /** Margin (fraction of the face's shorter side), status bar and gap (fractions of its height): our UI. */
    static constexpr double Margin = 0.07;
    static constexpr double StatusHeight = 0.09;
    static constexpr double Gap = 0.03;
    /** Aspect: face width / height (the rows keep a square margin). */
    static FSCFloorPanelLayout Make(TArray<FSCFloorPanelOption> Options, double Aspect);
    /** The option whose row contains content point C, INDEX_NONE outside every row. */
    int32 OptionAt(const FVector2D& C) const;
};

/** What a panel shows. */
struct SCBRIDGE_API FSCFloorPanelView
{
    int32 Highlight = INDEX_NONE; // option under the crosshair
    FString Image;                // a button panel: the state image under the highlight (package path)
    int32 Current = INDEX_NONE;   // the stop the carriage stands at; INDEX_NONE while it travels
    int32 Target = INDEX_NONE;    // the stop of the leg chosen or in progress
    TArray<int32> Requested;      // stops queued (sorted)
    int8 Direction = 0;           // +1 up, -1 down: the leg's direction
    bool bBlink = false;          // the target's marker phase
    bool bHoverImage = false;     // a button panel: Image is the render with the cursor on the highlighted node (no frame)
    /** Our blink rate for the target's marker (Hz). */
    static constexpr double BlinkHz = 1.5;
    static FSCFloorPanelView Of(const FSCLift& Lift, int32 Highlight, double Time);
    bool operator==(const FSCFloorPanelView& O) const
    {
        return Highlight == O.Highlight && Image == O.Image && Current == O.Current && Target == O.Target && Requested == O.Requested &&
               Direction == O.Direction && bBlink == O.bBlink && bHoverImage == O.bHoverImage;
    }
    FString Describe() const;
};

/** Which palette role colours what: our choice over the pads' palette (linear colours). Without a palette (an export
 *  before cache format 35) a neutral white-on-black set stands in (absence of data, logged). */
struct SCBRIDGE_API FSCFloorPanelColours
{
    FLinearColor Field, Row, RowHighlight, Label, LabelHighlight, Current, Requested, Status, Arrow;
    bool bFromData = false;
    static FSCFloorPanelColours From(const FSCLiftPanelStyle& Style);
};

/** What an operable panel is (C2 fixes: one mechanism for both).
 *  - FloorSelect: our own floor list on a lift panel's blank face (options in content coordinates).
 *  - Buttons: the game's own panel UI (a light panel, `ui_bindings[].buttons[]`): the exporter's render of the owner's
 *    current state drawn on the face, its clickable nodes the options (in image coordinates: 0..1 over the render). */
enum class ESCPanelKind : uint8 { FloorSelect, Buttons };

/** One drawn operable panel (ASCVehicle::FloorPanels): the crosshair highlights the option under it, E sends that
 *  option's interaction. */
struct FSCFloorPanel
{
    ESCPanelKind Kind = ESCPanelKind::FloorSelect;
    int32 Lift = INDEX_NONE, Interior = INDEX_NONE, Face = INDEX_NONE;
    FString EntityId;
    TWeakObjectPtr<const USceneComponent> Anchor;
    TWeakObjectPtr<UMeshComponent> Component;
    int32 Slot = INDEX_NONE;
    FSCScreenFaceGeometry Geometry;
    FSCFloorPanelLayout Layout;
    FSCFloorPanelColours Colours;
    TWeakObjectPtr<UTextureRenderTarget2D> Target;
    /** Per row: the label's mask in the pads' font and its em (none: drawn in the engine font). */
    TArray<TWeakObjectPtr<UTexture2D>> Masks;
    TArray<float> MaskEm;
    /** How a label is cased: the pads' label case (capitals when the export has no label style: the owner's style). */
    bool bUpperCase = true;
    int32 Highlight = INDEX_NONE;
    FSCFloorPanelView Shown;
    bool bDrawn = false;
    /** The face's frame: the component's rotation and location (its scale is in Geometry, FSCScreenFaceGeometry::Scaled). */
    FTransform Frame() const;

    // Buttons panels (Kind == Buttons).
    /** The integer UV cell the face samples the render in (the render is 0..1 over it). */
    FVector2D UVBase = FVector2D::ZeroVector;
    /** The owner's state types (defaults, transitions, linked tags) and its state images, from its UI binding. */
    TArray<FSCUiStateType> StateTypes;
    TArray<FSCUiStateImage> StateImages;
    FString DefaultImage;
    /** WP-I1-4: the renders with the cursor on a node (`hover_images[]`). */
    TArray<FSCUiHoverImage> HoverImages;
    /** The owner's current state per state type (its default until something sets it). */
    TMap<FString, FString> UiState;
    /** Decoded renders by package path (the default and every state image). */
    TMap<FString, TWeakObjectPtr<UTexture2D>> Images;
    /** The owner's Sendable interactions with their linking tags, and its EntityLinks. */
    TArray<FSCSendableInteraction> Sendables;
    TArray<FString> Links;
    FLinearColor HighlightColour = FLinearColor::White;
    /** Options live in content coordinates (FloorSelect) or image coordinates (Buttons): the point of each from a hit on
     *  the face (its content point and UV), and the content point of an option point. */
    FVector2D OptionPoint(const FVector2D& Content, const FVector2f& UV) const
    {
        return Kind == ESCPanelKind::Buttons ? FVector2D(UV.X, UV.Y) - UVBase : Content;
    }
    FVector2D ContentOf(const FVector2D& Option) const
    {
        return Kind == ESCPanelKind::Buttons ? Geometry.ContentFromUV(FVector2f(float(UVBase.X + Option.X), float(UVBase.Y + Option.Y))) : Option;
    }
    /** The render for the current UiState: the state image whose pinned states are all current and that pins the most of
     *  them (exactly the current non-default states when one does), else the default render. */
    FString ImageForState() const;
    /** WP-I1-4: the render Image (the default or a state image) with the cursor on option Option's node; "" without one. */
    FString HoverImageFor(const FString& Image, int32 Option) const;
};
/** A screen face showing an operating panel's render whose binding has clickable nodes (SCImportJob collects them):
 *  the face's geometry and what a drawn panel needs. */
struct FSCButtonFace
{
    TWeakObjectPtr<UMeshComponent> Component;
    int32 Slot = INDEX_NONE;
    TWeakObjectPtr<const USceneComponent> Panel; // the placement's anchor
    FSCScreenFaceGeometry Geometry;
    int32 Interior = INDEX_NONE; // the interior it was placed in (entity ids are unique within one container)
    FString EntityId;
    TArray<FString> Links;
    TArray<FSCSendableInteraction> Sendables;
    TArray<FSCUiButton> Buttons;
    FVector2D ImageSizePx = FVector2D::ZeroVector;
    TArray<FSCUiStateType> StateTypes;
    TArray<FSCUiStateImage> StateImages;
    TArray<FSCUiHoverImage> HoverImages;
    FString DefaultImage;
    bool bHideDoorLockControl = false;
    TFunction<UMaterialInterface*(UTexture* Image)> MakeMaterial;
};

/** A screen face no panel image binds (dark glass), with what a viewer-drawn image needs (SCImportJob collects them). */
struct FSCBlankScreenFace
{
    TWeakObjectPtr<UMeshComponent> Component;
    int32 Slot = INDEX_NONE;
    TWeakObjectPtr<const USceneComponent> Panel; // the placement's anchor
    FSCScreenFaceGeometry Geometry;
    /** An instance of the face's screen material that emits at its authored glow and takes an image. */
    TFunction<UMaterialInterface*(UTexture* Image)> MakeMaterial;
};
