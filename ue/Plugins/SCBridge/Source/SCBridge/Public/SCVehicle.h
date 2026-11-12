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
#include "SCVehicle.generated.h"
class UTexture2D;
class UMaterialInstanceDynamic;

class USCRigComponent;
class USkeletalMesh;
class ULightComponent;
class UPrimitiveComponent;
/** Named helper frames of one placed instance, in its anchor's space (cm, UE axes). "" is the item's source frame. */
USTRUCT()
struct SCBRIDGE_API FSCInstanceHelpers
{
    GENERATED_BODY()
    UPROPERTY() TMap<FString, FTransform> Frames;
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
    /** Whether the last tag played for Fragment on Rig (nullptr: the untargeted orbit-UI toggle) is Open. */
    bool IsFragmentOpen(FName Fragment, const USCRigComponent* Rig = nullptr) const;
    bool PlayFragmentAt(FName Fragment, FName Tag, USCRigComponent* Target);

    virtual void Tick(float DeltaSeconds) override;
    UPROPERTY(BlueprintReadOnly, Category = "SC") TArray<TObjectPtr<USkeletalMesh>> SkeletalMeshes;
    UPROPERTY(BlueprintReadOnly, Category = "SC") TArray<TObjectPtr<USCRigComponent>> Rigs;
    UPROPERTY(BlueprintReadOnly, Category = "SC") TArray<TObjectPtr<ULightComponent>> Lights;
    UPROPERTY() TMap<FString, TObjectPtr<UTexture2D>> GoboTextures;
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") TArray<FSCImportedLightState> LightStates;
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") bool bMegaLightsActive = false;
    /** Refresh after changing the project/scalability ray-tracing settings at runtime. */
    UFUNCTION(BlueprintCallable, Category = "SC|Lights") void RefreshLightShadowPolicy();
    /** Room volumes of the interior containers (WP-U3b §12); empty when the vehicle authors no rooms. */
    TArray<FSCLightArea> LightAreas;
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
    void MarkDirty(int32 ClipIndex);
    TSet<USkeletalMesh*> DirtyMeshes;
    // Last tag played per (rig instance, fragment); the nullptr rig is the untargeted toggle.
    TMap<TPair<const USCRigComponent*, FName>, FName> FragmentTargets;
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
    /** Anchor-relative transform that puts Helper (instance Id) on a joint frame plus Offset (native SC, row form). */
    bool PortRelative(const FString& Id, const FString& Helper, const FMatrix& Offset, FTransform& Out) const;
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString SessionPath;
};
