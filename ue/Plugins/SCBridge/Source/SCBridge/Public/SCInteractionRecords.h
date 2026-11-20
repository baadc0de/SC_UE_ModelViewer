// Package records of how interior entities are operated and of lifts (WP-P5; decomposed-export-contract "Interaction
// wiring, entities and lifts", cache format 31): interaction links, parent attachments, door timing, interaction points,
// geometry-less entities and lift carriages with their stops. Container-local values are CryEngine metres.
#pragma once
#include "CoreMinimal.h"

/** `interaction_links[]`: when the entity's interaction SourceSlot fires, TargetSlot is delivered to TargetEntityId. */
struct FSCInteractionLink
{
    FName SourceSlot;
    FString TargetEntityId;
    FName TargetSlot;
};
/** `parent_attachment`: the entity rides ParentEntityId, on its node AttachmentTarget. */
struct FSCParentAttachment
{
    FString ParentEntityId;
    FString AttachmentType, AttachmentTarget;
};
/** `door`: SCItemDoorParams timing and state (class value with instance overrides; on placements and, cache format 39,
 *  on loadout children). */
struct FSCDoorTiming
{
    bool bValid = false;
    bool bSpeedKnown = false; // animation_speed was exported (null: absent or not a number in the data)
    float AnimationSpeed = 1.f;
    bool bUseAutoCloseDelay = false;
    float AutoCloseDelay = 0.f;
    /** `is_opened` (IsOpened): the door's authored state at spawn; unset when the export has none. */
    TOptional<bool> bIsOpened;
    TOptional<bool> bIsLocked; // is_locked: absence leaves the door unlocked
    /** `animation_params`: the AnimationParams struct (SCItemDoorSimpleOpenCloseParams, SCItemDoorSingleProceduralParams). */
    FString AnimationParams;
    /** `transition_tag`: a single-procedural door's one transition (its fragment tag, "Open"): the door's state is that
     *  transition's time, closed at its start, open at its end, so it closes by playing it backwards. None otherwise. */
    FName TransitionTag;
    /** `open_tag` / `close_tag`: an open/close door's two fragment tags. None when not authored. */
    FName OpenTag, CloseTag;
};
/** `item_port_links[]` (cache format 39): when the entity's interaction SourceSlot fires, TargetSlot is delivered to the
 *  item in the vehicle's port TargetPort (`SInteractionLinkParams.vehicleItemPortInteractionLinks`). */
struct FSCItemPortLink
{
    FName SourceSlot;
    FString TargetPort;
    FName TargetSlot;
};
/** `sendable_interactions[]` (cache format 39): a Sendable shared interaction and its `LinkingTag` (tag path). */
struct FSCSendableInteraction
{
    FName Name;
    FString LinkingTag;
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
struct FSCPointInteraction
{
    FName Name;           // the shared interaction: the slot interaction_links name
    FString DisplayName;  // English display name ("Top Deck"); empty when the data has none
    bool bPrimary = false;
};
/** `interaction_points[]`: a helper the player aims at and what it offers there. */
struct FSCInteractionPoint
{
    FString HelperName;
    float Radius = 0.f;          // metres, 0 when not authored
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
struct FSCLiftStopLabel
{
    int32 Stop = INDEX_NONE;
    FString Text, Image; // Image: package path of the PNG (alpha = coverage)
    int32 Width = 0, Height = 0;
    float EmPx = 0.f;
};
/** `lifts[].panel_style` (cache format 35): the door pads' look for a floor-select list the viewer draws itself (WP-P5
 *  floor panel). Empty (no labels, no palette) when the export has none. */
struct FSCLiftPanelStyle
{
    FString SourceEntityId, SourceCanvas, StyleRecord;
    /** The pads' brand palette by BB_ColorStyle role name (sRGB). */
    TMap<FString, FColor> Palette;
    FString FontStyle, FontSymbol, LabelColourRole;
    float FontSize = 0.f, LetterSpacing = 0.f;
    bool bUpperCase = false;
    TArray<FSCLiftStopLabel> Labels;
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
