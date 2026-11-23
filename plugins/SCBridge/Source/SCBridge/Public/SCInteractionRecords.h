// Package records of how interior entities are operated and of lifts (WP-P5; decomposed-export-contract "Interaction
// wiring, entities and lifts", cache format 31): interaction links, parent attachments, door timing, interaction points,
// geometry-less entities and lift carriages with their stops. Container-local values are CryEngine metres.
#pragma once
#include "CoreMinimal.h"
#include "SCInteractionRecords.generated.h"

/** `interaction_links[]`: when the entity's interaction SourceSlot fires, TargetSlot is delivered to TargetEntityId. */
USTRUCT()
struct SCBRIDGE_API FSCInteractionLink
{
    GENERATED_BODY()
    UPROPERTY() FName SourceSlot;
    UPROPERTY() FString TargetEntityId;
    UPROPERTY() FName TargetSlot;
};
/** `parent_attachment`: the entity rides ParentEntityId, on its node AttachmentTarget. */
struct FSCParentAttachment
{
    FString ParentEntityId;
    FString AttachmentType, AttachmentTarget;
};
/** `door`: SCItemDoorParams timing and state (class value with instance overrides; on placements and, cache format 39,
 *  on loadout children). Reflected: saved with a prepared level's door records (WP-O1-4b). */
USTRUCT()
struct SCBRIDGE_API FSCDoorTiming
{
    GENERATED_BODY()
    UPROPERTY() bool bValid = false;
    UPROPERTY() bool bSpeedKnown = false; // animation_speed was exported (null: absent or not a number in the data)
    UPROPERTY() float AnimationSpeed = 1.f;
    UPROPERTY() bool bUseAutoCloseDelay = false;
    UPROPERTY() float AutoCloseDelay = 0.f;
    /** `is_opened` (IsOpened): the door's authored state at spawn; unset when the export has none. */
    UPROPERTY() TOptional<bool> bIsOpened;
    UPROPERTY() TOptional<bool> bIsLocked; // is_locked: absence leaves the door unlocked
    /** `animation_params`: the AnimationParams struct (SCItemDoorSimpleOpenCloseParams, SCItemDoorSingleProceduralParams). */
    UPROPERTY() FString AnimationParams;
    /** `transition_tag`: a single-procedural door's one transition (its fragment tag, "Open"): the door's state is that
     *  transition's time, closed at its start, open at its end, so it closes by playing it backwards. None otherwise. */
    UPROPERTY() FName TransitionTag;
    /** `open_tag` / `close_tag`: an open/close door's two fragment tags. None when not authored. */
    UPROPERTY() FName OpenTag;
    UPROPERTY() FName CloseTag;
};
/** `item_port_links[]` (cache format 39): when the entity's interaction SourceSlot fires, TargetSlot is delivered to the
 *  item in the vehicle's port TargetPort (`SInteractionLinkParams.vehicleItemPortInteractionLinks`). */
USTRUCT()
struct SCBRIDGE_API FSCItemPortLink
{
    GENERATED_BODY()
    UPROPERTY() FName SourceSlot;
    UPROPERTY() FString TargetPort;
    UPROPERTY() FName TargetSlot;
};
/** `sendable_interactions[]` (cache format 39): a Sendable shared interaction and its `LinkingTag` (tag path). */
USTRUCT()
struct SCBRIDGE_API FSCSendableInteraction
{
    GENERATED_BODY()
    UPROPERTY() FName Name;
    UPROPERTY() FString LinkingTag;
};
/** `light_group.interactions[]` of a light-group entity: the interaction it receives, its linking tag, and the light
 *  state the group ends in (Default, Auxiliary, Emergency, Off, ...). */
struct FSCLightGroupInteraction
{
    FName Name;
    FString LinkingTag;
    FString LightState;
};
/** One interaction offered at an interaction point. */
USTRUCT()
struct SCBRIDGE_API FSCPointInteraction
{
    GENERATED_BODY()
    UPROPERTY() FName Name;           // the shared interaction: the slot interaction_links name
    UPROPERTY() FString DisplayName;  // English display name ("Top Deck"); empty when the data has none
    UPROPERTY() bool bPrimary = false;
};
/** `interaction_points[]`: a helper the player aims at and what it offers there. */
struct FSCInteractionPoint
{
    FString HelperName;
    float Radius = 0.f;          // metres, 0 when not authored
    TOptional<float> AngleRange, ViewAngleLimit; // degrees
    bool bHasHelperTransform = false;
    FMatrix HelperTransform = FMatrix::Identity; // entity-local metres (geometry-less entities)
    TArray<FSCPointInteraction> Interactions;
};
/** `traversal_links[]`: an interaction link the game builds at run time from the entity's traversal tree. */
struct FSCTraversalLink
{
    FName LocalInteraction;      // the entity's own interaction (GatewayOpen)
    FString ItemPortName;        // what the tree reaches: a vehicle item port, not resolved by the exporter
    FName LinkedInteraction;     // delivered there (OpenSelf, Close)
};
/** `interiors[].entities[]`: a geometry-less interactive entity (path node, transit gateway, helper-only button). */
struct FSCInteriorEntity
{
    FString EntityId, EntityName, EntityClass, Kind;
    FString EntityClassGuid; // WP-A1 (lower case)
    FMatrix LocalMatrix = FMatrix::Identity;
    TArray<FString> Links;
    TArray<FSCInteractionLink> InteractionLinks;
    FSCParentAttachment Parent;
    TArray<FSCInteractionPoint> InteractionPoints;
    TArray<FSCTraversalLink> TraversalLinks;
    /** Cache format 39: `kind == "light_group"` entities carry what they receive; `logic` entities what they send. */
    TArray<FSCLightGroupInteraction> LightGroupInteractions;
    TArray<FSCSendableInteraction> SendableInteractions;
};
/** `lifts[].stops[]`. */
struct FSCLiftStopRecord
{
    int32 Index = 0;
    FString NodeEntityId, NodeEntityName, Kind;
    FVector Position = FVector::ZeroVector; // container-local metres
    TArray<FString> LandingDoorIds;
    float TeleportTimeScale = 1.f;
    float GatewayTimingInTransit = -1.f; // -1: not authored
    FString GatewayInteraction;          // the node's gateway interaction ("" without)
    FString AccessibilityKind;           // AccessibilityExteriorZoneParams, ... ("" without)
    float AccessibleTimingInTransit = -1.f;
};
/** `lifts[].panel_style.labels[]`: a stop's name as a white glyph mask in the door pads' label font. */
USTRUCT()
struct SCBRIDGE_API FSCLiftStopLabel
{
    GENERATED_BODY()
    UPROPERTY() int32 Stop = INDEX_NONE;
    UPROPERTY() FString Text;
    UPROPERTY() FString Image; // package path of the PNG (alpha = coverage)
    UPROPERTY() int32 Width = 0;
    UPROPERTY() int32 Height = 0;
    UPROPERTY() float EmPx = 0.f;
};
/** `lifts[].panel_style` (cache format 35): the door pads' look for a floor-select list the viewer draws itself (WP-P5
 *  floor panel). Empty (no labels, no palette) when the export has none. Saved with a prepared lift. */
USTRUCT()
struct SCBRIDGE_API FSCLiftPanelStyle
{
    GENERATED_BODY()
    UPROPERTY() FString SourceEntityId;
    UPROPERTY() FString SourceCanvas;
    UPROPERTY() FString StyleRecord;
    /** The pads' brand palette by BB_ColorStyle role name (sRGB). */
    UPROPERTY() TMap<FString, FColor> Palette;
    UPROPERTY() FString FontStyle;
    UPROPERTY() FString FontSymbol;
    UPROPERTY() FString LabelColourRole;
    UPROPERTY() float FontSize = 0.f;
    UPROPERTY() float LetterSpacing = 0.f;
    UPROPERTY() bool bUpperCase = false;
    UPROPERTY() TArray<FSCLiftStopLabel> Labels;
    bool IsValid() const { return !Labels.IsEmpty() && !Palette.IsEmpty(); }
    const FSCLiftStopLabel* LabelOf(int32 Stop) const { return Labels.FindByPredicate([Stop](const FSCLiftStopLabel& L) { return L.Stop == Stop; }); }
};
/** `lifts[]`: one carriage (SCItemElevatorParams). Values are -1 / false when the data omits them. */
struct FSCLiftRecord
{
    FString EntityId, EntityName, EntityClass;
    float MaxSpeed = -1.f, EasingDistance = -1.f, OpenWaitTime = -1.f, QueueWaitTime = -1.f, IdlingPeriod = -1.f;
    bool bSlowDownAtBends = false;
    FName OpenInnerDoorInteraction, OpenInnerDoorFinishedInteraction, CloseAllDoorsInteraction, CloseInnerDoorFinishedInteraction;
    FString GridGeometry, RestNodeId;
    TArray<FSCLiftStopRecord> Stops;
    TArray<FString> Riders;
    FSCLiftPanelStyle PanelStyle;
};
