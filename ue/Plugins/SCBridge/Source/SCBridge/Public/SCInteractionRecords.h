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
/** `door`: SCItemDoorParams timing (class value with instance overrides). */
struct FSCDoorTiming
{
    bool bValid = false;
    bool bSpeedKnown = false; // animation_speed was exported (null: absent or not a number in the data)
    float AnimationSpeed = 1.f;
    bool bUseAutoCloseDelay = false;
    float AutoCloseDelay = 0.f;
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
    FMatrix LocalMatrix = FMatrix::Identity;
    TArray<FString> Links;
    TArray<FSCInteractionLink> InteractionLinks;
    FSCParentAttachment Parent;
    TArray<FSCInteractionPoint> InteractionPoints;
    TArray<FSCTraversalLink> TraversalLinks;
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
};
