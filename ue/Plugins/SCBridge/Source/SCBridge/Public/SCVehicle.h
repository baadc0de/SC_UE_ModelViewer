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
    bool bAudioFresh = true; // WP-A1: the first advance includes the start frame (events sit at t = 0.0001 / 0.9999)
};
UCLASS(BlueprintType)
class SCBRIDGE_API ASCVehicle : public AActor
{
    GENERATED_BODY()
public:
    ASCVehicle();
    UPROPERTY(BlueprintReadOnly, Category = "SC|Exploration") TArray<FSCSpawnPoint> SpawnPoints;
    UPROPERTY(BlueprintReadOnly, Category = "SC|Exploration") TArray<FSCDoorInteraction> Doors;
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
    /** Interaction under the view ray within Reach (rig hit -> its nearest driven joint), else NearestDoor. */
    int32 FindInteraction(FVector Eye, FVector Direction, float Reach = 300, const AActor* Ignore = nullptr) const;
    UFUNCTION(BlueprintCallable, Category = "SC|Exploration") bool ToggleDoor(int32 Index);
    UFUNCTION(BlueprintPure, Category = "SC|Exploration") bool IsDoorOpen(int32 Index) const;
    /** Door control panels (the game's pads): a panel's anchor -> the rig of the door it operates (scene `links`, or
     *  the door whose loadout attached it). FindInteraction resolves a panel under the crosshair to its door. */
    TMap<TWeakObjectPtr<const USceneComponent>, TWeakObjectPtr<USCRigComponent>> DoorControls;
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
    /** SpeedScale multiplies the fragment's own clip speed (a door's DoorAnimationSpeed). */
    bool PlayFragmentAt(FName Fragment, FName Tag, USCRigComponent* Target, float SpeedScale = 1.f);

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

    /** Tick while clips play or a lift has work. */
    void UpdateTickEnabled();

    virtual void Tick(float DeltaSeconds) override;
    UPROPERTY(BlueprintReadOnly, Category = "SC") TArray<TObjectPtr<USkeletalMesh>> SkeletalMeshes;
    UPROPERTY(BlueprintReadOnly, Category = "SC") TArray<TObjectPtr<USCRigComponent>> Rigs;
    UPROPERTY(BlueprintReadOnly, Category = "SC") TArray<TObjectPtr<ULightComponent>> Lights;
    UPROPERTY() TMap<FString, TObjectPtr<UTexture2D>> GoboTextures;
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") TArray<FSCImportedLightState> LightStates;
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
    /** WP-A1: a cooked level's audio (ue/scripts/cook_audio.py): its folder under the content directory and the package
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
private:
    bool StartClip(int32 Index, float Speed, bool bSnap, bool bLast, const TArray<FName>& Scopes = {}, USCRigComponent* Target = nullptr);
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
