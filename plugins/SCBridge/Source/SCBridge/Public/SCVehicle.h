// Imported vehicle ownership and sparse clip/fragment playback API.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SCPackageReader.h"
#include "SCClipCatalog.h"
#include "SCLightState.h"
#include "SCLightAreas.h"
#include "UObject/ObjectKey.h"
#include "SCExploration.h"
#include "SCLift.h"
#include "SCFloorPanel.h"
#include "SCShipLinkSubsystem.h"
#include "SCVehicle.generated.h"
class UTexture2D;
class UMaterialInstanceDynamic;
class UTextureRenderTarget2D;

class USCRigComponent;
class USkeletalMesh;
class ULightComponent;
class UPrimitiveComponent;
class UMeshComponent;
class UMaterialInterface;
/** Named helper frames of one placed instance, in its anchor's space (cm, UE axes). "" is the item's source frame. */
USTRUCT()
struct SCBRIDGE_API FSCInstanceHelpers
{
    GENERATED_BODY()
    UPROPERTY() TMap<FString, FTransform> Frames;
};
/** WP-I1 I1-3: a screen face (one material slot of one component) showing an operating panel's image, and the exporter's
 *  renders of the panel owner's other states (`ui_bindings[].state_images[]`), one material instance each. The face swaps
 *  instances; any other material (a viewer-drawn render target, later) can be put on it with SetScreenFaceMaterial. */
struct FSCScreenFace
{
    TWeakObjectPtr<UMeshComponent> Component;
    int32 Slot = INDEX_NONE;
    TWeakObjectPtr<const USceneComponent> Panel; // the panel instance's anchor (the DoorControls key)
    TWeakObjectPtr<UMaterialInterface> Default;  // the default render (a door pad: closed, unlocked, powered)
    TMap<FString, TWeakObjectPtr<UMaterialInterface>> States; // FSCUiStateImage::Key() -> instance
    FString Image;  // the default image (package path), for listings
    FString Shown;  // the state key shown ("" = the default)
    FString Pinned; // sc.Screens.States: a state held whatever the door does ("" = follow the door)
    TWeakObjectPtr<UMaterialInterface> Custom; // SetScreenFaceMaterial: other content, kept until cleared
    /** WP-I1-4: the renders with the cursor on a button, by FSCUiHoverImage::FaceKey (node | state key), and the button
     *  node under the crosshair now ("" none: the face shows its state at rest). */
    TMap<FString, TWeakObjectPtr<UMaterialInterface>> Hovers;
    FString Hovered;
};
/** WP-I1-4: one interaction point of a placed usable: where its helper is (anchor space, when the mesh names it), how
 *  near the player must be (its `radius`) and what it offers a standing player there (its interactions that start a use). */
struct FSCUsablePoint
{
    FString Helper;
    FVector Local = FVector::ZeroVector;
    bool bLocated = false;
    float RadiusCm = 0; // 0: not authored (no limit)
    float ViewAngleLimit = 0; // degrees: the point is used only within half this of the view; 0 (or >= 180): any
    TArray<FSCPointInteraction> Options;
};
/** WP-I1-4: a placed usable (a bed, a sofa, a chair: `usable_interactions[]`) and its points. */
struct FSCUsableTarget
{
    int32 Interior = INDEX_NONE;
    FString EntityId, Label;
    TArray<FSCUsablePoint> Points;
};
/** WP-I1-4: an operating panel's face whose render has clickable nodes and that is not drawn as a FloorPanel (a door pad,
 *  a lift call panel, a kiosk): where its buttons are on the face, for the game's hover look under the crosshair. */
struct FSCPanelAim
{
    TWeakObjectPtr<UMeshComponent> Component;
    int32 Face = INDEX_NONE;            // the ScreenFaces entry that shows the hover renders
    FSCScreenFaceGeometry Geometry;     // in the component's frame, its scale applied (FSCScreenFaceGeometry::Scaled)
    FVector2D UVBase = FVector2D::ZeroVector; // the integer UV cell the face samples the render in
    TArray<FSCUiButton> Buttons;        // RectPx normalized to the render (0..1)
    /** The node E acts on when the crosshair is on the face but off every button ("" none): a door pad's door button. */
    FString Fallback;
    /** How far it is operated from (cm): its UI's interaction distance (ASCVehicle::PanelReach); 0 without one. */
    double Reach = 0;
    /** The face's bounding sphere in the component's frame: a face out of reach is skipped before its triangles. */
    FVector Centre = FVector::ZeroVector;
    double Radius = 0;
    FTransform Frame() const;
    /** A door pad's door button: the first whose interaction sets none of the pad's own states (`state_types[]`
     *  transitions): its open / close acts on the door, whose state comes back by link, while the lock and light buttons
     *  set the pad's own lock and light states. "" when every button sets one. */
    static FString DoorButtonOf(const TArray<FSCUiButton>& Buttons, const TArray<FSCUiStateType>& StateTypes);
};
/** Where a fragment's clips start (ASCVehicle::PlayFragmentAt). */
enum class ESCFragmentStart : uint8
{
    Begin,       // at the clip's start in its direction of play (frame 0 forward, the last frame backward)
    CurrentPose, // at the frame whose pose is nearest the rig's current pose (a door turned around mid-stroke)
    HoldFirst,   // held at the start of play (frame 0 forward, the last frame for a clip played backwards)
    HoldLast,    // held at the end of play (the last frame forward, frame 0 backwards)
};
/** A door's data (`door`, `item_port_links`) by its rig, collected at import (ASCVehicle::DoorRecords). */
struct FSCDoorRecord
{
    FSCDoorTiming Door;
    TArray<FSCItemPortLink> PortLinks;
};
/** A light group of an interior (`interiors[].entities[]`, kind `light_group`): what it receives and the lights it holds
 *  (FSCImportedLightState::GroupEntityId). */
struct FSCLightGroup
{
    int32 Interior = INDEX_NONE;
    FString EntityId, Name;
    TArray<FString> Links;
    TArray<FSCLightGroupInteraction> Interactions;
    FString LightState; // the light state the group was last set to ("" = as imported)
};
struct FSCClipPlayback
{
    TWeakObjectPtr<USCRigComponent> TargetRig;
    int32 Clip = INDEX_NONE;
    double Frame = 0;
    float Speed = 1;
    bool bPlaying = true;
    uint64 Order = 0;
    TArray<FName> Scopes;
    bool bAudioFresh = true; // WP-A1: the first advance plays the start-of-play events (they sit at t = 0.0001 / 0.9999)
    bool bTargeted = false;  // started for one rig (TargetRig): once that rig is gone the entry poses nothing
    int32 OpeningGangway = INDEX_NONE;
};
UCLASS(BlueprintType)
class SCBRIDGE_API ASCVehicle : public AActor
{
    GENERATED_BODY()
public:
    ASCVehicle();
    UPROPERTY(BlueprintReadOnly, Category = "SC|Exploration") TArray<FSCSpawnPoint> SpawnPoints;
    UPROPERTY(BlueprintReadOnly, Category = "SC|Exploration") TArray<FSCDoorInteraction> Doors;
    TArray<FSCLadderAnchor> LadderAnchors;
    struct FShipDoor
    {
        struct FMovementCollider
        {
            struct FSample
            {
                FTransform Pose; // relative to the ship
                double TravelCm = 0; // maximum point travel before the next sample
            };
            TWeakObjectPtr<UPrimitiveComponent> Component;
            FName Socket;
            FTransform ClosedPose; // relative to the ship
            TArray<FSample> Sweep;
            FTransform GroundPose = FTransform::Identity;
        };
        ESCShipDoorGroup Group = ESCShipDoorGroup::Other;
        FName GroupSource;
        FString Label;
        FBox Aperture = FBox(ForceInit); // vehicle-local, at the authored closed pose when available
        FBox MovementBounds = FBox(ForceInit);
        TArray<FMovementCollider> MovementColliders;
        bool bLocked = false;
        bool bSecurePending = false;
        bool bCargoRelock = false;
        bool bActorOpenedWithoutAccess = false; // checked by the RPC state journal
        bool bRamp = false;
        struct FGroundProbeSet
        {
            int32 Bone = INDEX_NONE;
            TArray<FVector> Points; // actual rigid collision surface points in the socket bone's frame
        };
        TArray<FGroundProbeSet> GroundProbes;
        int32 GroundLastSample = INDEX_NONE; // only for an exact, reversible single-clip cargo sweep
    };
    TArray<FShipDoor> ShipDoors;
    void BuildShipDoors();
    void BuildGangwayProbes(int32 Index);
    void GangwayStops(float Seconds, TMap<int32, double>& OutFractions) const;
    void RememberGangwayStop(int32 Index);
    FString ShipDoorLabel(int32 Index) const;
    bool IsDoorLocked(int32 Index) const;
    bool SetDoorLocked(int32 Index, bool bLocked);
    bool DoorwayOccupied(int32 Index) const;
    /** Things in a doorway the doors wait for: actors with this tag and a box root (UnrealCitizen: cargo boxes). */
    static const FName DoorwayBlockerTag;
    /** Why something holds door Index as it is ("" = nothing): an application's systems register one (UnrealCitizen:
     *  a cargo job using an access door). Closing is refused with the reason, and a cargo relock waits for it. */
    using FDoorUseQuery = TFunction<FString(const ASCVehicle& Ship, int32 Door)>;
    static void AddDoorUseQuery(FName Name, FDoorUseQuery Query);
    static void RemoveDoorUseQuery(FName Name);
    FString DoorInUse(int32 Index) const;
    FName ShipLinkPlayer() const;
    FString DoorOrderRefusal(int32 Index, ESCShipDoorAction Action, FName Actor, const TOptional<FVector>& LocalUse = {}) const;
    FString OrderShipDoor(int32 Index, ESCShipDoorAction Action, FName Actor, const TOptional<FVector>& LocalUse = {});
    TArray<FSCShipDoorResult> SecureShip(FName Actor);
    void FinishSecureOrders();
    void RestoreCargoDoorLocks();
    TArray<FSCFloorPanel> DoorPanelLocks;
    TMap<TWeakObjectPtr<const USCRigComponent>, FString> DoorPanelLabels;
    void RememberDoorPanelLabel(const USCRigComponent* Rig, const FSCUiBinding& Binding);
    bool AimDoorPanelLock(const USceneComponent* Panel, const FVector& Eye, const FVector& Direction, double Reach, bool bLocked) const;
    // ------------------------------------------------------------------------- WP-I1-4 panel hover (SCPanelScreens.cpp)
    /** Operating-panel faces with clickable nodes that are not FloorPanels (BuildButtonPanels). */
    TArray<FSCPanelAim> PanelAims;
    /** The PanelAims face under the view ray within Reach (its front; nothing that collides in front of it) and the button
     *  node under the crosshair there, else the face's Fallback. False without a face under the ray. */
    bool AimPanelButton(FVector Eye, FVector Direction, float Reach, const AActor* Ignore, int32& OutAim, FString& OutNode) const;
    /** Show Node hovered on aim Aim's face and nothing hovered on the others (INDEX_NONE: none anywhere). Returns the faces
     *  swapped (RefreshPanelScreens). */
    int32 SetPanelHover(int32 Aim, const FString& Node);
    /** The world point at the centre of button Node on aim Aim's face (the interaction popout's anchor). */
    bool PanelButtonWorldPoint(int32 Aim, const FString& Node, FVector& Out) const;
    // ------------------------------------------------------------------------- WP-I1-4 usables
    TMap<TWeakObjectPtr<const USceneComponent>, FSCUsableTarget> UsableTargets;
    /** The usable whose geometry the view ray meets first within Reach (by its anchor) and the point the player uses
     *  there (OutPoint): of its located points within their radius of the eye, the one nearest where the ray hits; a
     *  usable whose mesh names none of its helpers offers its first point. Null when no point is in reach. */
    const FSCUsableTarget* FindUsableTarget(FVector Eye, FVector Direction, float Reach, const AActor* Ignore, int32* OutPoint = nullptr,
                                            FVector* OutWorld = nullptr) const;
    /** Hangar waypoints (environments only; SCHangarWaypoints.h): kind Hangar, actor-relative standing hints. Built by
     *  BuildWaypoints from WaypointSources in the presented pose; saved with a cooked environment. */
    UPROPERTY(BlueprintReadOnly, Category = "SC|Exploration") TArray<FSCSpawnPoint> Waypoints;
    int32 WaypointRevision = 0;
    /** Authored vehicle-retrieval interfaces on hangar operating panels, and their reach in cm. */
    UPROPERTY() TMap<TObjectPtr<USceneComponent>, float> GarageKiosks;
    bool AimGarageKiosk(const FVector& Eye, const FVector& Direction, float Reach, const AActor* Ignore) const;
    /** A drawn operating panel that is a vehicle kiosk: a console use and the vehicle list / retrieval states (WP-M2). */
    static bool IsGarageKiosk(const FSCUiBinding& Binding);
    /** The records the import chose as waypoints (SCHangarWaypoints::Select) with their anchors; transient. */
    TArray<FSCWaypointSource> WaypointSources;
    /** Waypoints from WaypointSources and the ship pad in the current pose; a cooked environment (no sources, no rigs)
     *  keeps the waypoints it was saved with. Returns the waypoints. */
    int32 BuildWaypoints();
    UPROPERTY(BlueprintReadOnly, Category = "SC|Exploration") bool bCollisionReady = false;
    /** Incremented whenever SpawnPoints changes (UI lists rebuild on change). */
    int32 SpawnRevision = 0;
    void AddSpawnPoint(FName Kind, const FString& Label, const FTransform& WorldTransform);
    /** Replace the single user-selected spawn ("spawn where I look"); returns its index. */
    int32 SetSelectedSpawn(const FTransform& WorldTransform);
    void AddItemSpawn(const FSCSceneInstance& Instance, USceneComponent* Anchor);
    void BuildExploration();
    FBox ExplorationBounds() const;
    UFUNCTION(BlueprintCallable, Category = "SC|Exploration") int32 NearestDoor(FVector Position, float Reach = 300) const;
    /** Interaction under the view ray within Reach (rig hit -> its nearest driven joint), else NearestDoor. A door panel
     *  (DoorControls) the ray meets first is reached from as far as its own UI is operated (PanelReach) when that is
     *  farther than Reach. Pick (optional) says how the interaction was found. */
    int32 FindInteraction(FVector Eye, FVector Direction, float Reach = 300, const AActor* Ignore = nullptr, FSCInteractionPick* Pick = nullptr) const;
    UFUNCTION(BlueprintCallable, Category = "SC|Exploration") bool ToggleDoor(int32 Index);
    UFUNCTION(BlueprintPure, Category = "SC|Exploration") bool IsDoorOpen(int32 Index) const;
    /** A clip of door Index's own fragment plays on its rig (another door on the same rig moving does not count). */
    bool IsDoorPlaying(int32 Index) const;
    /** Door control panels (the game's pads): a panel's anchor -> the rig of the door it operates (scene `links`, or
     *  the door whose loadout attached it). FindInteraction resolves a panel under the crosshair to its door. */
    TMap<TWeakObjectPtr<const USceneComponent>, TWeakObjectPtr<USCRigComponent>> DoorControls;
    /** How far (cm) the player operates a door panel from: its UI's `interaction_distance` (the game's render-to-texture
     *  interactionDistance; the C2's ramp and hatch pads 15 m, high on the closed door). Panels without one are not here. */
    TMap<TWeakObjectPtr<const USceneComponent>, float> PanelReach;
    float MaxPanelReach = 0;
    /** Record a panel's operating distance (metres, from its operating bindings; <= 0: none). */
    void SetPanelReach(const USceneComponent* Panel, float Metres);
    TArray<FSCDoorSensor> DoorSensors;
    /** WP-I1 I1-3: operating-panel screen faces with state images (FSCScreenFace). */
    TArray<FSCScreenFace> ScreenFaces;
    /** Put each face's state image on it: a door pad follows its door (DoorControls) — moving, opened or closed — unless
     *  a state is pinned; a face without a state image for the state shows its default. Returns the faces swapped. */
    int32 RefreshPanelScreens();
    /** Put Material on face Face (null: back to what its state shows). The general hook for screen content. */
    bool SetScreenFaceMaterial(int32 Face, UMaterialInterface* Material);
    /** The game's DoorOpenState (FSCUiStateImage::Key) a pad shows for its door: DoorOpening / DoorClosing while it moves
     *  towards open / closed, else DoorOpened / DoorClosed. */
    static FString DoorOpenStateKey(bool bOpen, bool bMoving);
    /** Paints the vehicle can wear (Manifest.Paints) and the mesh slots they touch; SetPaint swaps those slots.
     *  INDEX_NONE = the vehicle's own (default) paint. */
    TArray<FSCPaintSlot> PaintSlots;
    UFUNCTION(BlueprintCallable, Category = "SC|Paint") bool SetPaint(int32 Index);
    UFUNCTION(BlueprintPure, Category = "SC|Paint") int32 GetPaint() const { return ActivePaint; }
    int32 ActivePaint = INDEX_NONE;
    /** First-person tick: opens doors whose sensors hold the player, closes the ones a sensor opened after their delay. */
    void UpdateDoorSensors(const FVector& PlayerWorld, double Now);
    /** The door interaction driven by a rig (the first one; INDEX_NONE without). */
    int32 DoorForRig(const USCRigComponent* Rig) const;
    /** Whether the last tag played for Fragment on Rig (nullptr: the untargeted orbit-UI toggle) is Open. */
    bool IsFragmentOpen(FName Fragment, const USCRigComponent* Rig = nullptr) const;
    /** SpeedScale multiplies the fragment's own clip speed (a door's DoorAnimationSpeed; negative plays it backwards). */
    bool PlayFragmentAt(FName Fragment, FName Tag, USCRigComponent* Target, float SpeedScale = 1.f, ESCFragmentStart Start = ESCFragmentStart::Begin);
    /** The frame of Clip (bound to Binding) whose pose is nearest Local (rotation angle in radians plus translation in
     *  metres, summed over the clip's tracks); searched from the start of play (frame 0 forward, the last frame backward),
     *  so a tie keeps the start. */
    static double NearestClipFrame(const FSCImportedClip& Clip, const FSCClipBinding& Binding, const TArray<FTransform>& Local, bool bBackward);
    /** How far Local is from Clip's pose at Frame sampled in that direction (rotation radians plus translation metres, summed
     *  over the binding's tracks). */
    static double ClipPoseCost(const FSCImportedClip& Clip, const FSCClipBinding& Binding, const TArray<FTransform>& Local, double Frame, bool bBackward);
    /** Whether playback entry P poses Rig (its own rig when targeted, none once that rig is gone; every rig when untargeted). */
    static bool AppliesTo(const FSCClipPlayback& P, const USCRigComponent* Rig);
    // ------------------------------------------------------------------------------------- doors (data state, C2 fixes)
    /** Door data by rig (`door`, `item_port_links` of the instance that carries the rig), filled at import. */
    TMap<TWeakObjectPtr<const USCRigComponent>, FSCDoorRecord> DoorRecords;
    /** Rigs of loadout items by port path (SCCargoRecords::PortKey): where `item_port_links` deliver. */
    TMap<FString, TWeakObjectPtr<USCRigComponent>> PortRigs;
    /** Open (true) or close door Index through its own fragments, from the rig's current pose (never a snap): an open/close
     *  door plays its open or close tag; a single-procedural door plays its transition forwards to open and backwards to
     *  close. SpeedScale <= 0: the door's DoorAnimationSpeed. Then the door's `item_port_links` for that verb are delivered
     *  (Depth bounds a link cycle). Records the door's state for IsDoorOpen and its pads. */
    bool PlayDoor(int32 Index, bool bOpen, float SpeedScale = 0.f, int32 Depth = 0, FName AsActor = NAME_None,
        const TOptional<FVector>& LocalUse = {}, bool bCargoAccess = false);
    /** Every door at its authored spawn state (`is_opened`, closed without data): a single-procedural door holds its
     *  transition at the start (closed) or the end (open); an open/close door opened in the data holds its open fragment's
     *  end; a closed one keeps the bind pose (the start of its open fragment). Also after ResetPose. Returns the doors posed. */
    int32 ApplyDoorRestPoses();
    // -------------------------------------------------------------------------------------- light groups (C2 fixes)
    TArray<FSCLightGroup> LightGroups;
    /** The light groups an entity of Interior with these EntityLinks drives: a group it links, or one that links an entity
     *  it links too (the chipset both a panel and its group name). */
    TArray<int32> LightGroupsLinkedBy(int32 Interior, const TArray<FString>& Links) const;
    /** Deliver a linking tag to a group: the interaction it receives with that tag, and its light state on every light of
     *  the group (FSCLightImporter::ApplyState). Returns the lights changed; OutState the group's light state ("" when the
     *  group receives no such tag). */
    int32 DeliverLightGroupTag(int32 Group, const FString& LinkingTag, FString* OutState = nullptr);

    // ------------------------------------------------------------------------------------------ WP-P5 lifts and links
    /** One lift per package `lifts[]` record, ticked by the vehicle. */
    TArray<TSharedPtr<FSCLift>> Lifts;
    /** Exported interaction links by interior, and what each entity is (door, carriage, stop). */
    FSCInteractionNetwork Interactions;
    /** A panel that operates a lift (its links reach a carriage or a stop): the press path until WP-I1 has its own. */
    struct FLiftPanel
    {
        int32 Interior = INDEX_NONE;
        FString EntityId, Label;
        TArray<FSCPointInteraction> Choices; // the interaction point's interactions that send a link, in authored order
    };
    TMap<TWeakObjectPtr<const USceneComponent>, FLiftPanel> LiftPanels;
    /** Doors a lift opens and closes itself (its gate and landing doors): not toggled by E (FindInteraction / NearestDoor),
     *  as the C2's lift doors offer the player no interaction of their own. sc.FP.Door still drives them. */
    TSet<TWeakObjectPtr<const USCRigComponent>> LiftDrivenRigs;
    /** WP-P5 §6: follow SourceEntityId's exported links whose source slot is Interaction and deliver each target slot to its
     *  target (a lift carriage or stop, or a door). Returns the deliveries handled. The first interior holding the id. */
    int32 SendInteraction(const FString& SourceEntityId, FName Interaction);
    int32 SendInteractionIn(int32 Interior, const FString& SourceEntityId, FName Interaction);
    /** Nesting of SendInteractionIn (a lift's own events are sent while a delivery runs); bounds link cycles in the data. */
    int32 SendDepth = 0;
    /** The lift panel under the view ray (its anchor owns the hit part), or null. */
    const FLiftPanel* FindLiftPanel(FVector Eye, FVector Direction, float Reach, const AActor* Ignore) const;
    /** WP-P5 section 5 step 3: which door stands in lift LiftIndex's leg From -> To (a sweep of the carriage's collision box). */
    FSCLiftHost::ELeg SweepLiftLeg(int32 LiftIndex, int32 From, int32 To, FString& OutBy);
    /** Deliver again the opening door deliveries of a source's interaction (a queued request's gateway); returns the doors started. */
    int32 RedeliverDoorLinks(int32 Interior, const FString& SourceEntityId, FName Interaction);
    /** The lift whose carriage carries component C (C or an ancestor is its anchor), INDEX_NONE otherwise. */
    int32 LiftCarrying(const USceneComponent* C) const;
    /** C2 fixes (rider retention): the lift whose moving carriage carries a capsule whose feet are at Bottom: its movement
     *  base is part of the carriage, or one of the carriage's colliders lies under the feet within FootReach (cm); INDEX_NONE
     *  without. The stand-in for membership of the carriage's own physics grid. */
    int32 LiftRiddenBy(const FVector& Bottom, const UPrimitiveComponent* Base = nullptr, double FootReach = 50.0) const;
    /** What lift Lift's current leg passes through: the colliders that block the player inside the box of the carriage's
     *  colliders swept from where it stands to the leg's stop, other than the carriage and its riders, door rigs (the leg
     *  check decides on doors) and pawns. Its riders are carried through them (the game carries the members of the
     *  carriage's grid). Computed once per leg. */
    const TArray<TWeakObjectPtr<UPrimitiveComponent>>& LiftPathObstacles(int32 Lift);
    struct FLiftPath
    {
        int32 From = INDEX_NONE, To = INDEX_NONE;
        double Clock = -1;
        TArray<TWeakObjectPtr<UPrimitiveComponent>> Obstacles;
    };
    TMap<int32, FLiftPath> LiftPaths;
    /** Door helpers by rig (the rig's door interaction, BuildExploration). */
    bool PlayDoorRig(USCRigComponent* Rig, bool bOpen, float Speed);
    bool IsRigOpen(const USCRigComponent* Rig) const;
    bool IsRigPlaying(const USCRigComponent* Rig) const;
    // ------------------------------------------------------------------------- WP-P5 floor panel (SCFloorPanel.h)
    /** Lift panels whose options are drawn on their screen face (the viewer's own floor-select UI). */
    TArray<FSCFloorPanel> FloorPanels;
    /** The panels' render targets and screen instances: this actor's own (not in Materials, which an import shares). */
    UPROPERTY() TArray<TObjectPtr<UTextureRenderTarget2D>> FloorPanelTargets;
    UPROPERTY() TArray<TObjectPtr<UMaterialInterface>> FloorPanelMaterials;
    /** At import, after the lifts: a lift panel whose interactions ask one carriage for its stops and that has a blank
     *  screen face gets a drawn floor list there (the face joins ScreenFaces, its material set by SetScreenFaceMaterial).
     *  Texture: a decoded package image by path (the label masks). */
    void BuildFloorPanels(TArray<FSCBlankScreenFace>& Blank, TFunctionRef<UTexture2D*(const FString&)> Texture);
    /** C2 fixes, the same mechanism for the game's own panel UIs: a face showing an operating panel's render whose binding
     *  has clickable nodes (a light panel) becomes a Buttons panel of FloorPanels: the render of the owner's current state
     *  drawn into a render target on the face, the node under the crosshair framed, E sends its interaction. */
    void BuildButtonPanels(TArray<FSCButtonFace>& Faces, TFunctionRef<UTexture2D*(const FString&)> Texture);
    /** E on a Buttons panel's option: the owner's own state machine takes the interaction (`transitions`), its linking tag
     *  goes to the light groups the panel drives (DeliverLightGroupTag), and the panel's linked state follows the group
     *  (`linked`). Returns the lights changed (or 1 when only the panel's state changed). */
    int32 PressButtonPanel(int32 Panel, int32 Option);
    /** A lift panel's floor options: its interactions whose links ask one carriage for a stop (`From Elevator call <N>`),
     *  labelled with the stop's name, from the highest stop down; empty for a call button or a panel reaching two lifts. */
    TArray<FSCFloorPanelOption> FloorPanelOptions(const FLiftPanel& Panel, int32& OutLift, bool* bOutSeveralLifts = nullptr) const;
    /** What E on a lift panel sends without aiming at a drawn row: a call button its primary interaction (its first when
     *  none is marked); a floor-select panel (FloorPanelOptions) nothing, drawn or not: a floor is chosen on its face. */
    FName LiftPanelPressInteraction(const FLiftPanel& Panel) const;
    /** The floor panel face under the view ray within Reach (its front, nothing that collides in front of it) and the
     *  option under the crosshair there (INDEX_NONE off every row). False: no panel face under the ray. */
    bool AimFloorPanel(FVector Eye, FVector Direction, float Reach, const AActor* Ignore, int32& OutPanel, int32& OutOption,
                       FVector2D* OutContent = nullptr) const;
    /** Highlight Option on Panel and nothing on the other panels (INDEX_NONE panel: none anywhere). */
    void SetFloorPanelHighlight(int32 Panel, int32 Option);
    /** Draw the panels whose view changed; returns the panels drawn. */
    int32 RefreshFloorPanels();
    /** E on an option: its interaction through the dispatcher. Returns the deliveries handled. */
    int32 PressFloorPanel(int32 Panel, int32 Option);
    int32 FloorPanelOf(const FString& EntityId) const;
    /** World point of an option's centre on its face (scripted aim). */
    bool FloorPanelWorldPoint(int32 Panel, int32 Option, FVector& Out) const;

    /** Tick while clips, lifts, pending door work, or light styles are active. */
    void UpdateTickEnabled();

    virtual void Tick(float DeltaSeconds) override;
    UPROPERTY(BlueprintReadOnly, Category = "SC") TArray<TObjectPtr<USkeletalMesh>> SkeletalMeshes;
    UPROPERTY(BlueprintReadOnly, Category = "SC") TArray<TObjectPtr<USCRigComponent>> Rigs;
    UPROPERTY(BlueprintReadOnly, Category = "SC") TArray<TObjectPtr<ULightComponent>> Lights;
    UPROPERTY() TMap<FString, TObjectPtr<UTexture2D>> GoboTextures;
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") TArray<FSCImportedLightState> LightStates;
    TSet<int32> LoopingLights; // indices into LightStates; only time-varying, enabled presets
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") bool bMegaLightsActive = false;
    /** Refresh after changing the project/scalability ray-tracing settings at runtime. */
    UFUNCTION(BlueprintCallable, Category = "SC|Lights") void RefreshLightShadowPolicy();
    // Lowest point of the visible geometry (not the game's invisible colliders): where the model rests, e.g. landing gear.
    double LowestVisibleZ(FString* OutPart = nullptr) const;
    // The ship standing on the ground: every Landing_Gear Mannequin fragment tagged Compress snapped to its last frame
    // (the game drives that clip by suspension load; its end is the gear compressed, feet flat). Returns the clips posed.
    int32 RestOnLandingGear();
    /** Room volumes of the interior containers (WP-U3b §12); empty when the vehicle authors no rooms. */
    TArray<FSCLightArea> LightAreas;
    /** One anchor per interior object container (scene `interiors[]`, in order): the unit of interior streaming. */
    UPROPERTY() TArray<TObjectPtr<USceneComponent>> InteriorAnchors;
    /** WP-A1: per interior anchor, whether it hangs from a helper frame (the container -> anchor mapping of its audio). */
    UPROPERTY() TArray<bool> InteriorHelperFrames;
    /** WP-A1: per interior anchor, its index in the scene's `interiors[]` (an interior without a parent has no anchor). */
    UPROPERTY() TArray<int32> InteriorSceneIndex;
    /** WP-A1: a cooked level's audio (UnrealCitizen's prep step, SCPrepareAudio.h): its folder under the content directory and the package
     *  scene path inside it; BeginPlay loads it. Empty for a live import (its audio comes from the package). */
    UPROPERTY(EditAnywhere, Category = "SC|Audio") FString CookedAudio;
    UPROPERTY(EditAnywhere, Category = "SC|Audio") FString CookedAudioScene;
    virtual void BeginPlay() override;
    /** Vertex samples per imported mesh asset (mesh-local cm): the input of the inside/outside vote. */
    TMap<TObjectKey<UObject>, TArray<FVector3f>> MeshSamples;
    /** World point inside any room volume of this vehicle (the game's "not the root clip volume"). */
    UFUNCTION(BlueprintCallable, Category = "SC|Lights") bool IsInsideLightArea(const FVector& World) const;
    /** Room GUID containing the world point; empty outside every room volume. */
    UFUNCTION(BlueprintCallable, Category = "SC|Lights") FString LightRoomAt(const FVector& World) const;
    /** Lighting channel of the world point: 0 outside, else its room's channel (set by FSCLightAreas::Apply). */
    UFUNCTION(BlueprintCallable, Category = "SC|Lights") int32 LightChannelAt(const FVector& World) const;
    /** Room GUID -> lighting channel 1 or 2 (WP-L1), filled by FSCLightAreas::Apply. */
    TMap<FString, int32> LightRoomChannels;
    /** WP-P3: the physics grid's volume (DataCore `physicsGrid.physicsProxy`), a triangle soup in actor space (cm).
     *  Inside it the game's actors belong to the vehicle's grid, where the vehicle's own colliders (HullColliders,
     *  the root entity's game collision) do not block them; the interior's and the items' colliders do. */
    TArray<FVector> PhysicsGridTriangles;
    TArray<int32> PhysicsGridShapeEnds; // per shape, end (exclusive) in PhysicsGridTriangles; empty: one shape
    FBox PhysicsGridBounds = FBox(ForceInit);
    UPROPERTY() TArray<TObjectPtr<UPrimitiveComponent>> HullColliders;
    /** World point inside the physics-grid volume (ray parity); false without a volume. */
    bool IsInPhysicsGrid(const FVector& World) const;
    /** World segment touching the volume: Bottom inside, or the segment crossing its surface. An actor is in the grid
     *  when its upright axis (feet to head) touches it, so a head under a hull overhang already is. */
    bool IsInPhysicsGrid(const FVector& Bottom, const FVector& Top) const;
    UFUNCTION(BlueprintCallable, Category = "SC|Animation") bool PlayClip(FName Clip);
    UFUNCTION(BlueprintCallable, Category = "SC|Animation") bool SnapClip(FName Clip, bool bLast);
    UFUNCTION(BlueprintCallable, Category = "SC|Animation") bool PlayFragment(FName Fragment, FName Tag);
    /** WP-K1: play clip Index of the catalogue on one rig instance (nullptr: every bound rig); Speed < 0 plays it
     *  backwards from its last frame. */
    bool PlayClipAt(int32 Index, USCRigComponent* Target, float Speed);
    /** WP-K1: hold clip Index of the catalogue at its first or last frame on one rig instance (nullptr: every bound rig). */
    bool SnapClipAt(int32 Index, USCRigComponent* Target, bool bLast);
    /** Toggle a caller-selected open/close tag pair; defaults suit the interact key. */
    UFUNCTION(BlueprintCallable, Category = "SC|Animation")
    bool ToggleClip(FName Fragment, FName OpenTag = "Open", FName CloseTag = "Close");
    UFUNCTION(BlueprintCallable, Category = "SC|Animation") void ResetPose();
    void EvaluateClips();
    TArray<FSCClipPlayback> Playback;     // active entries only
    TArray<FSCClipPlayback> HeldPlayback; // bounded by catalogue size; no idle evaluation
    uint64 PoseEvaluations = 0;           // diagnostic: number of rigs refreshed by playback
    /** WP-A1 diagnostics: clip audio events crossed by playback (posted to the audio scene when there is one), and the
     *  last one's trigger. */
    uint64 ClipAudioEvents = 0;
    FString LastClipAudioTrigger;
    /** sc.Doors.Trace: the door logged every tick while a clip plays on its rig (DOOR_TICK, DOOR_AUDIO_EVENT); INDEX_NONE off. */
    int32 TraceDoor = INDEX_NONE;
private:
    bool StartClip(int32 Index, float Speed, bool bSnap, bool bLast, const TArray<FName>& Scopes = {}, USCRigComponent* Target = nullptr,
                   double StartFrame = -1);
    /** Whether later layers on Rig (Playback or HeldPlayback, targeted at Rig or untargeted) drive every channel Entry drives
     *  there, so Entry shows nothing on Rig (its audio events are not Rig's). */
    bool IsCoveredOnRig(const FSCClipPlayback& Entry, const USCRigComponent* Rig) const;
    /** A clip started outside a door interaction (PlayClip / SnapClip / PlayClipAt / SnapClipAt) on a door's channels: the
     *  door's recorded state becomes what the clip ends in when it is the door's own open or close fragment, else unknown. */
    void ReconcileDoorStates(int32 Index, const USCRigComponent* Target, bool bEndsAtLast);
    /** WP-A1: play the audio events of P's clip bindings crossed between Previous and P.Frame. */
    void FireClipAudio(FSCClipPlayback& P, double Previous);
    void MarkDirty(int32 ClipIndex);
    TSet<USkeletalMesh*> DirtyMeshes;
    // Last tag played per (rig instance, fragment); the nullptr rig is the untargeted toggle.
    TMap<TPair<const USCRigComponent*, FName>, FName> FragmentTargets;
    struct FSensorDoorState { double LastInside = -1; float ExitDelay = 0; bool bOpenedBySensor = false; };
    TMap<int32, FSensorDoorState> SensorDoorStates;
    struct FPendingSpawn
    {
        FName Kind;
        FString Label;
        TWeakObjectPtr<USceneComponent> Anchor;
    };
    TArray<FPendingSpawn> PendingItemSpawns;
    uint64 NextOrder = 0;
    FRandomStream FragmentRandom{0x5343};

public:
    UPROPERTY(BlueprintReadOnly, Category = "SC") TArray<TObjectPtr<UStaticMesh>> Meshes;
    /** Transient textures (deduplicated by source path + role) and material instances owned by this import. */
    UPROPERTY(BlueprintReadOnly, Category = "SC") TArray<TObjectPtr<UTexture2D>> Textures;
    /** Source identity of imported meshes and textures (package path + variant/role): the editor cook shares assets
     *  across environments by it. */
    UPROPERTY() TMap<TObjectPtr<UObject>, FString> SourcePaths;
    UPROPERTY(BlueprintReadOnly, Category = "SC") TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;
    /** Instances driven by scene controls.engine_glow (main thrusters). */
    UPROPERTY(BlueprintReadOnly, Category = "SC") TArray<TObjectPtr<UMaterialInstanceDynamic>> EngineGlowMaterials;
    /** Sets the engine-glow control value (package units, clamped to its range); see EngineGlowToEmissive. */
    UFUNCTION(BlueprintCallable, Category = "SC") void SetEngineGlow(float Strength);
    UPROPERTY() TArray<FSCThrusterBinding> Thrusters;
    UPROPERTY(Transient) ESCThrusterPreviewMode ThrusterPreview = ESCThrusterPreviewMode::Minimum;
    UPROPERTY(Transient) bool bThrusterPreviewActive = false;
    bool SetThrusterPreview(ESCThrusterPreviewMode Mode);
    UPROPERTY(BlueprintReadOnly, Category = "SC") TObjectPtr<USCClipCatalog> ClipCatalog;
    UPROPERTY(BlueprintReadOnly, Category = "SC") FSCSceneManifest Manifest;
    /** Anchors of instances that carry an id (player-kit items), by id. */
    UPROPERTY() TMap<FString, TObjectPtr<USceneComponent>> InstanceAnchors;
    /** Helper frames of those instances, by id: port alignment re-parents items at run time (stow <-> hand). */
    UPROPERTY() TMap<FString, FSCInstanceHelpers> InstanceHelpers;
    /** Helper frames of nested-assembly entries (a personal weapon's attachments: optics, magazines), which carry no
     *  id, by their anchor (anchor space, as InstanceHelpers): an optic's sight helper replaces the weapon's own. */
    UPROPERTY() TMap<TObjectPtr<USceneComponent>, FSCInstanceHelpers> AttachmentHelpers;
    /** WP-K1: anchors of placed loadout items by port path (SCCargoRecords::PortKey of `children[].port_path`). */
    UPROPERTY() TMap<FString, TObjectPtr<USceneComponent>> PortAnchors;
    /** WP-K1: per Manifest.InventoryContainers entry (null when unresolved), the container entity's frame: axes X = SC y,
     *  Y = SC x, Z = SC z, cm (the frame placed items use). On its parent item's helper/bone, so it follows the rig. */
    UPROPERTY() TArray<TObjectPtr<USceneComponent>> InventoryAnchors;
    /** WP-K1: the rig of the item owning each inventory container (its parent item; an interior grid: a linked entity's),
     *  whose Mannequin fragments open/close it. Null when the owner has none. */
    UPROPERTY() TArray<TObjectPtr<USCRigComponent>> InventoryOwnerRigs;
    /** Anchor-relative transform that puts Helper (instance Id) on a joint frame plus Offset (native SC, row form). */
    bool PortRelative(const FString& Id, const FString& Helper, const FMatrix& Offset, FTransform& Out) const;
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString SessionPath;
};
