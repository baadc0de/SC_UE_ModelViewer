// Viewer application state: owns the FFI client, the current ASCVehicle and the load pipeline
// (FFI export on a worker -> tar parse on a worker -> FSCImportJob stepped on the game thread), camera mode,
// focus/isolate and object-reuse by entity + options hash. Blueprint/Python-callable; UI polls it every frame.
#pragma once
#include "CoreMinimal.h"
#include "SCImport.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "Tasks/Task.h"
#include "SCArchive.h"
#include "SCViewerCatalog.h"
#include "SCPlayerAnimator.h"
#include "SCVehicleInstances.h"
#include "SCViewerSubsystem.generated.h"

class ASCVehicle;
class ASCViewerPawn;
class ASCFirstPersonCharacter;
class USCViewOptions;
class FSCFolderPackageSource;
class FSCPackageSource;
class FSCTarPackageSource;
class FSCViewerAutomation;

UENUM(BlueprintType)
enum class ESCCameraMode : uint8
{
    Orbit,
    Fly,
    FirstPerson
};
UENUM(BlueprintType)
enum class ESCArchiveState : uint8
{
    Closed,
    Opening,
    Ready,
    Failed
};
UENUM(BlueprintType)
enum class ESCLoadPhase : uint8
{
    Idle,
    Exporting,
    Parsing,
    Importing,
    Succeeded,
    Failed,
    Cancelled
};

/** Outcome and timings of the latest LoadEntity call (seconds are wall time). */
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCLoadReport
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString Entity;
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString Options;
    UPROPERTY(BlueprintReadOnly, Category = "SC") ESCLoadPhase Result = ESCLoadPhase::Idle;
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString Error;
    /** Same entity + options as the loaded actor: nothing was exported or imported. */
    UPROPERTY(BlueprintReadOnly, Category = "SC") bool bReused = false;
    /** Layer-1 package cache hit reported by the core for this export. */
    UPROPERTY(BlueprintReadOnly, Category = "SC") bool bExportCacheHit = false;
    UPROPERTY(BlueprintReadOnly, Category = "SC") double ExportSeconds = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") double ParseSeconds = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") double ImportSeconds = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") double DeriveSeconds = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") double BuildSeconds = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") double CollisionSeconds = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") double MaterialSeconds = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") double AssembleSeconds = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") double TotalSeconds = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") int64 TarBytes = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") int64 CacheBytes = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") int64 CacheEntries = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 CachedMeshes = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 MeshBuilds = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 CollisionBuilds = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 Assets = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 Instances = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 Warnings = 0;
    /** Live objects after the load (whole process). */
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 StaticMeshes = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 SkeletalMeshes = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 Textures = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 MaterialInstances = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 Vehicles = 0;
    /** Objects of the previous vehicle tracked before unload, and how many survived GC (must be 0). */
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 PreviousObjects = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 PreviousObjectsAlive = 0;
    /** Cancelled/failed import: objects it had created (partial actor + session packages) and survivors after GC. */
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 PartialObjects = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 PartialObjectsAlive = 0;
    /** State when CancelLoad was called: static meshes already built, and whether worker tasks were still running. */
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 MeshesBuiltAtCancel = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") bool bWorkersPendingAtCancel = false;
    /** Archive session the load belonged to (OpenArchive increments it). */
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 ArchiveGeneration = 0;
};

/** Outcome of the editor-host Keep. */
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCKeepResult
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category = "SC") bool bOK = false;
    /** Long package path of the kept folder, e.g. /Game/SC/<Entity>. */
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString Root;
    UPROPERTY(BlueprintReadOnly, Category = "SC") int32 Saved = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString Message;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSCOnLoadFinished, const FSCLoadReport&, Report);
DECLARE_MULTICAST_DELEGATE_OneParam(FSCOnComponentFocused, USceneComponent*);
/** Editor-host "Keep": save the current vehicle's transient objects as assets. Bound by SCBridgeEditor only. */
DECLARE_DELEGATE_RetVal_TwoParams(FSCKeepResult, FSCKeepHandler, ASCVehicle*, const FString& /*Entity*/);

UCLASS()
class SCBRIDGE_API USCViewerSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintPure, Category = "SC|Viewer", meta = (WorldContext = "WorldContext"))
    static USCViewerSubsystem* Get(const UObject* WorldContext);
    /** Headless/automation host (commandlets): a standalone game instance with a dummy game world, no viewport. */
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") static USCViewerSubsystem* CreateHeadless();
    /** Tear down an instance made by CreateHeadless (game instance shutdown, world destroyed) before the host exits. */
    /** Returns how many objects of the loaded and in-flight imports survived teardown + GC (0 expected; -1 if not headless). */
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") static int32 DestroyHeadless(USCViewerSubsystem* Viewer);

    // Archive
    /** Empty path: settings / -SCArchive, else auto-discovery. A successful explicit path is remembered. */
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") void OpenArchive(const FString& P4kPath);
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") ESCArchiveState GetArchiveState() const { return ArchiveState; }
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") FString GetArchiveError() const { return ArchiveError.ToString(); }
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") FString GetArchiveLabel() const;
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") FString GetCacheDir() const { return CacheDir; }
    /** The open archive's build id (empty before it is open). */
    FString GetArchiveBuildId() const { return ArchiveBuildId; }
    FSCCacheStats GetProcessedCacheStats() const;
    void ConfigureProcessedImport(FSCImportOptions& Options, bool bFolderPackage = false) const;
    bool PrepareGeometryForCapture();
    void ForgetGeometry(ASCVehicle* Owner);
    void LogGeometryStreaming() const;
    void ClearProcessedCache();
    void SetProcessedCacheCap(int32 MiB);
    bool CanRebuildLoadedEntity() const;
    bool RebuildLoadedEntity();
    /** Incremented by every OpenArchive: reuse keys and search results are scoped to one archive session. */
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") int32 GetArchiveGeneration() const { return int32(ArchiveGeneration); }
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") bool WaitForArchive(float TimeoutSeconds = 300.f);

    // Search
    /** Blocking search (scripts); the UI uses SearchAsync. */
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") TArray<FSCEntityHit> SearchEntities(const FString& Query, int32 Limit = 100);
    /** Callback runs on the game thread from the subsystem tick. */
    void SearchAsync(const FString& Query, int32 Limit, TFunction<void(const FSCSearchResult&)> Callback);

    // Load lifecycle
    /** Starts an asynchronous load; returns false if the archive is not ready or a load is running. Same entity and
     *  options hash as the loaded actor completes immediately as "reused". */
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") bool LoadEntity(const FString& Entity, USCViewOptions* Options);
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") void CancelLoad();
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") void Unload();
    /** Garage lifecycle for the primary slot. Extra vehicle instances stay where they are. */
    bool LoadGarageVehicle(const FString& Entity);
    /** Stores the loaded ship; the prepared level it came from (a scenario's paint) is kept as its stored level. */
    bool StoreGaragePrimary();
    /** The prepared level the Garage retrieves an entity from: the one it was stored from (its paint comes back with it;
     *  a package may hold only that level), else the entity's own. Empty: none (the archive exports it). */
    FString GarageShipLevel(const FString& Entity) const;
    /** Whether the Garage can load the entity as the loaded ship now (its GarageShipLevel, or the archive). */
    bool CanLoadGarage(const FString& Entity) const;
    /** Records the prepared level a stored ship came from (StoreGaragePrimary does for the loaded ship). */
    void SetStoredShipLevel(const FString& Entity, const FString& Level);
    /** Entities stored with a recorded level, by their bare names. */
    TArray<FString> GetStoredShipEntities() const;
    int32 SpawnGarageFloorVehicle(const FString& Entity);
    /** Pumps the pipeline on the calling (game) thread until the load finishes; for commandlets and tests. */
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") bool WaitForLoad(float TimeoutSeconds = 600.f);
    /** Pumps until the pipeline reaches Phase (or finishes); lets tests cancel inside a given phase. */
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") bool WaitForPhase(ESCLoadPhase Target, float TimeoutSeconds = 600.f);
    /** Pumps the import one step at a time until at least MinMeshes static meshes exist and a worker is still running
     *  (a mid-import state for cancel/teardown tests). False if the import ends first. */
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") bool WaitForImportedMeshes(int32 MinMeshes, float TimeoutSeconds = 600.f);
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") bool IsBusy() const;
    /** A game environment (e.g. a hangar object container exported as a decomposed package folder) imported beside
     *  the ship, outside the vehicle slot; the map's own hangar geometry and lights are hidden while it is loaded.
     *  Folder: a package folder on disk; a `*.socpak` archive path: exported from the open archive (cached, textures
     *  at least mip 1 so the package fits the 2 GB tar limit). Empty or "none": unload it.
     *  Also -SCEnvironment=<...>, `sc.Environment <...|none>` and USCViewerSettings::Environment.
     *  bFitToShip (the viewer's own choice: settings and the environment picker; USCViewerSettings::bFitHangarToShip):
     *  Folder names the hangar style, and each loaded ship gets the smallest cooked hangar of that style that holds it
     *  (USCHangarIndex, preferring Folder's roof kind); false (-SCEnvironment, sc.Environment) loads exactly Folder. */
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") void SetEnvironment(const FString& Folder, bool bFitToShip = false);
    /** Space (WP-K3): no environment and the map's own hangar hidden, a sun instead of its lights, and no gravity in the
     *  world (what falls in a hangar floats). Off restores the hangar and the project's gravity. */
    void SetSpace(bool bInSpace);
    bool IsSpace() const { return bSpace; }
    /** The space sun's illuminance (lux; viewer choice, `sc.Space.Sun`, a scenario's `sun_lux`). */
    void SetSpaceSunLux(float Lux);
    float GetSpaceSunLux() const { return SpaceSunLux; }
    /** Put paint Index (Manifest.Paints; INDEX_NONE = the vehicle's own) on the loaded vehicle and remember it for the
     *  entity (USCViewerSettings::Paints). Also `sc.Paint [index|default|name part]`. */
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") bool SetPaint(int32 Index);
    /** Custom colours of the active paint (ASCVehicle::SetPaintColors), by palette channel (linear RGB). bSave keeps them
     *  in PaintColorsFile() for this vehicle and paint (an empty map removes them), so they come back with the paint. */
    bool SetPaintColors(const TMap<FString, FLinearColor>& Overrides, bool bSave = true);
    /** The custom colours saved for a vehicle entity and a paint's palette id ("" = the vehicle's own paint). */
    TMap<FString, FLinearColor> SavedPaintColors(const FString& Entity, const FString& PaletteId) const;
    /** <Saved>/PaintColors.json: {"version": 1, "paints": [{"vehicle", "palette", "paint", "colors": {"primary": "#RRGGBB", ...}}]}
     *  (sRGB hex, as a colour picker shows them); read again on every load, so an edit by hand applies on the next one. */
    static FString PaintColorsFile();
    static FString ToHex(const FLinearColor& Linear);
    static bool FromHex(const FString& Hex, FLinearColor& OutLinear);
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") ASCVehicle* GetEnvironment() const { return Environment.Get(); }
    /** The requested environment (folder, archive path or empty) and the catalogue's environments (listed once per
     *  archive session; GetCatalog). */
    FString GetEnvironmentRequest() const { return EnvironmentFolder; }
    /** Pose the environment's mechanisms: "bind" (as exported), "open" or "closed" fragment end poses. */
    void SetEnvironmentState(const FString& State);
    const TArray<FString>& GetEnvironmentChoices() const { return EnvironmentChoices; }
    /** WP-O1-3: the pick lists (environments, vehicles) chosen from game data; bReady once listed for this archive.
     *  Listed on demand: a consumer (the viewer's menu, RPC /catalog) calls RequestCatalog first; hosts that never
     *  ask pay nothing. */
    const FSCViewerCatalog& GetCatalog() const { return Catalog; }
    void RequestCatalog() const { bCatalogWanted = true; }
    /** Why there are no lists for this archive session, final until another archive opens (empty while listing or
     *  ready): the listing failed, the archive did not open or was lost, or none is open (a -SCPackage session). */
    FString GetCatalogError() const;
    /** An environment export, parse or import is under way: RequestEnvironment (SetEnvironment) would wait for its
     *  workers on the game thread. A menu queues its next choice until this is false. */
    bool IsEnvironmentWorking() const { return EnvExport.IsValid() || EnvParse.IsValid() || EnvJob.IsValid(); }
    /** Ask the environment's export or parse to stop, without waiting; its result is then dropped (an import, once
     *  started, finishes). */
    void CancelEnvironmentWork();
    /** The environment in the scene was built in this archive session (an archive opened since keeps the actor, but a
     *  pick from the new archive's lists must load it again). */
    bool IsEnvironmentOfThisArchive() const { return Environment.IsValid() && EnvArchiveGeneration == ArchiveGeneration; }
    /** The vehicle in the scene was loaded in this archive session (LoadedKey starts with its generation); an adopted
     *  -SCPackage vehicle has no archive and counts as this session's. */
    bool IsVehicleOfThisArchive() const
    {
        return Vehicle.IsValid() && (LoadedKey.IsEmpty() || LoadedKey.StartsWith(FString::Printf(TEXT("%u|"), ArchiveGeneration)));
    }
    /** CancelLoad was asked and the load it stops has not finished yet. */
    bool IsLoadCancelling() const { return bLoadCancelRequested; }
    /** CancelEnvironmentWork was asked and the export or parse it stops has not finished yet. */
    bool IsEnvironmentCancelling() const
    {
        return EnvProgress && EnvProgress->IsCancelRequested() && (EnvExport.IsValid() || EnvParse.IsValid());
    }
    bool IsEnvironmentLoading() const
    {
        return bEnvironmentPending || EnvJob.IsValid() || EnvExport.IsValid() || EnvParse.IsValid() || EnvPlaceInTicks > 0 ||
               (EnvLevel && !Environment.IsValid());
    }
    /** The cooked level for an object container path (EnvironmentLevelPath, else a level cooked before WP-O1-4 under the
     *  name without the path hash), empty when none was cooked, the app does not use cooked levels, or -SCNoCooked. */
    static FString CookedEnvironmentLevel(const FString& Folder);
    /** The cooked level of a ship (ShipLevelPath without a variant), empty when none was cooked. LoadEntity streams it
     *  instead of exporting from the archive, so it also works without one (a game build on a machine without Star
     *  Citizen). */
    static FString CookedShipLevel(const FString& Entity);
    /** WP-O1-4: where a prepared (cooked) level lives, whether or not it exists: an object container
     *  …/<style>/<name>.socpak -> /Game/SC/Hangars/Maps/<style>_<name>_<hash of the whole path, case-insensitive> (empty for
     *  anything else); a ship
     *  /Game/SC/Ships/Maps/<entity>, with a variant (a paint baked in) /Game/SC/Ships/Maps/<entity>__<variant>. Characters
     *  outside [A-Za-z0-9_] become '_'. UnrealCitizen's prep step (-run=SCPrepare) writes them. */
    static FString EnvironmentLevelPath(const FString& Folder);
    static FString ShipLevelPath(const FString& Entity, const FString& Variant = FString());
    /** WP-O1-4b-2: the prepared level of a player body (USCViewerSettings::ResolvePlayer*: the gender, the loadout, the weapons
     *  kept, their spares) as a definition names it: readable from the gender, told apart by a hash of the rest (case, the
     *  weapons' order and repetition, the loadout's Data/ prefix and an unnamed gender aside). Not what the export resolves it
     *  to: a named default loadout is another key than an omitted one. */
    static FString PlayerLevelPath(const FString& Gender, const FString& Loadout, const TArray<FString>& Weapons, bool bSpareAttachments);
    /** A ship level with a paint baked in (<entity>__<variant>: an entity's own part never holds "__"), not the entity's own
     *  level: lists of entities (the Garage) leave it out. */
    static bool IsShipPaintLevel(const FString& Level);
    /** Load a prepared ship level (UnrealCitizen's scenario: its paint baked in) as the vehicle, archive or not; false
     *  (nothing started) as LoadEntity, or when the level does not exist. */
    bool LoadPreparedEntity(const FString& Entity, const FString& Level);
    /** The paint a skin names among a vehicle's Paints (Manifest.Paints): "default" or empty (INDEX_NONE), an index, or the first
     *  paint whose label or palette id contains it. False when it names none. (-SCSkin, a scenario's skin.) */
    static bool ResolvePaint(const TArray<struct FSCPaintVariant>& Paints, const FString& Skin, int32& OutIndex);
    /** LoadEntity can run now: the archive is open, or the entity has a cooked level. */
    bool CanLoad(const FString& Entity) const;
    float GetEnvironmentProgress() const;      // 0..1 over export, parse, import and placement
    FString GetEnvironmentPhaseText() const;
    /** -SCShowcase (the standalone showcase build): no viewer chrome (top bar, footer, side panels); the scene, the
     *  first-person HUD and its teleport menu only. Keys switch the view (C first/third person). */
    bool IsShowcase() const { return bShowcase; }
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") ESCLoadPhase GetLoadPhase() const { return Phase; }
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") float GetLoadProgress() const;
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") FString GetLoadPhaseText() const;
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") FSCLoadReport GetLastReport() const { return Report; }
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") ASCVehicle* GetVehicle() const;
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") FString GetLoadedEntity() const { return LoadedEntity; }
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") FString GetPendingEntity() const { return PendingEntity; }
    /** Counts the vehicle loads started (each StartLoad): which load a request or a report belongs to. */
    uint32 GetLoadSerial() const { return LoadSerial; }
    /** The options instance the UI edits; persisted with the settings after each successful load. */
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") USCViewOptions* GetOptions() const { return Options; }

    // Camera / inspection
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") bool SetCameraMode(ESCCameraMode Mode);
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") ESCCameraMode GetCameraMode() const { return CameraMode; }
    /** Orbit camera fits the component's subtree bounds (the vehicle when null). */
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") void FocusComponent(USceneComponent* Component);
    /** Hide every primitive outside Component's subtree; null restores the imported visibility. */
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") void IsolateComponent(USceneComponent* Component);
    /** Restore imported visibility, clear isolation and focus/selection, then frame the whole vehicle (F key). */
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") void FrameAll();
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") USceneComponent* GetIsolated() const { return Isolated.Get(); }
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") USceneComponent* GetFocused() const { return Focused.Get(); }

    // Editor-host Keep
    static FSCKeepHandler& KeepHandler();
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") bool CanKeep() const;
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") FSCKeepResult Keep();

    UPROPERTY(BlueprintAssignable, Category = "SC|Viewer") FSCOnLoadFinished OnLoadFinished;
    FSCOnComponentFocused OnComponentFocused;
    /** The local player's viewer pawn (null headless or when another pawn/view target is active). */
    ASCViewerPawn* GetViewerPawn() const;
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") ASCFirstPersonCharacter* GetFirstPerson() const;
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") bool SpawnWhereLooking();
    /** Folder harness adopts the imported actor without opening an archive. */
    void AdoptPackageVehicle(ASCVehicle* Imported);
    /** The FFI client (null before OpenArchive): WP-K1 exports cargo box packages through it. */
    TSharedPtr<ISCArchive, ESPMode::ThreadSafe> GetClient() const { return Client; }
    /** Whether this build reads Star Citizen's archive at all: the importer plugin (SCBridgeImport) registers the reader;
     *  a packaged game has none and plays prepared content only (WP-O1-4c). */
    bool CanReadArchive() const { return Client.IsValid(); }
    /** A ship load is in flight (exported, parsed, imported or its prepared level streaming); a hangar loading is not one. */
    bool IsShipLoading() const
    {
        return Phase == ESCLoadPhase::Exporting || Phase == ESCLoadPhase::Parsing || Phase == ESCLoadPhase::Importing;
    }
    /** The prepared level the loaded ship was streamed from (a scenario's paint variant too); empty when it was imported.
     *  Extra instances of the loaded ship stream it as well (FSCVehicleInstances). */
    const FString& GetLoadedShipLevel() const { return LoadedShipLevel; }
    /** The prepared level an extra instance of Entity streams (WP-O1-4b-3c): the loaded ship's level for more of the
     *  loaded ship, else the entity's own (CookedShipLevel); empty when it has none (it is imported). */
    FString PreparedShipLevelFor(const FString& Entity) const;
    /** An entity's name as entities compare: a record's name (EntityClassDefinition.<entity>) without its prefix. */
    static FString EntityName(const FString& EntityOrRecord);
    /** A streamed level instance this subsystem or its extra instances no longer want: unloaded and removed; one that failed
     *  to load is removed at once, one still loading is kept until the world removed it (or it failed: then removed). */
    void ReleaseLevel(ULevelStreamingDynamic* Level);
    /** The scene's owner (a game scenario, a cargo setup) failed before its ship came, in a build without an archive
     *  reader: an unstarted -SCScript route is ended, so it does not run against a ship loaded later. */
    /** bSceneGoing: the owner removes its ship too (a failed scenario), so a ship loaded or loading now does not keep the
     *  route (a ship queued by -SCLoad still does). bFinal: the owner ended for good (a game scenario refused or failed:
     *  it owns the scene, nothing loads for it later), so the route ends with an archive reader too. */
    void CancelPendingScript(bool bSceneGoing = false, bool bFinal = false);
    /** Cache statistics as of the last export/open (no FFI call). */
    const FSCCacheStats& GetCacheStats() const { return CacheStats; }

    // Player body (WP-P2): the kit is exported, imported and its clips read asynchronously the first time first
    // person is entered, then kept for every later first-person session. Capsule-only first person meanwhile. A prepared
    // body (WP-O1-4b-2, SetPreparedPlayerLevel) is streamed instead: its level holds the kit actor with its kit and clips.
    enum class EPlayerKitState : uint8 { None, Exporting, Parsing, Importing, Clips, Streaming, Ready, Failed };
    /** The body comes from this prepared level (PlayerLevelPath), not from the archive; empty: exported as before. A run of a
     *  prepared scenario sets it before the body is first requested. */
    void SetPreparedPlayerLevel(const FString& Level) { PreparedPlayerLevel = Level; }
    /** Release a prepared body streaming or ready (detached, its actor and level gone; state None). An export or import in
     *  flight is left to finish. A scenario that fails releases its body with the rest of its scene. */
    void ReleasePreparedPlayerKit();
    const FString& GetPreparedPlayerLevel() const { return PreparedPlayerLevel; }
    /** WP-O1-4b-2b: a prepared scenario's traversal clips (an asset's package name, TraversalClipsPath); seats and ladders take
     *  their clips from it, not the archive. Its load starts now. Empty: they ask the archive as before. */
    void SetPreparedTraversalClips(const FString& Package);
    bool UsesPreparedTraversalClips() const { return !PreparedTraversal.IsEmpty(); }
    /** The prepared traversal clips (loaded now if their load has not finished); null when they do not load, or were read
     *  for another body than the prepared one. */
    TSharedPtr<const FSCPlayerClipMap> GetPreparedTraversalClips();
    /** Where the prep step saves a scenario's traversal clips: one asset per (body, vehicle, environment) level set. */
    static FString TraversalClipsPath(const FString& PlayerLevel, const FString& VehicleLevel, const FString& EnvironmentLevel);
    /** Starts the player-kit pipeline unless it is running or ready (a failed kit is retried). */
    bool RequestPlayerKit();
    EPlayerKitState GetPlayerKitState() const { return KitState; }
    FString GetPlayerKitStatus() const;
    /** Pumps the kit pipeline until ready or failed (automation, tests). */
    bool WaitForPlayerKit(float TimeoutSeconds = 600.f);
    ASCVehicle* GetPlayerKitActor() const { return KitActor; }
    /** Attach the ready kit to the first-person character (no-op if none, not ready or attached). */
    void AttachPlayerKit();

    // Extra vehicle instances (WP-K1 mixed ownership; SCVehicleInstances.h). They start once a vehicle is loaded and
    // no load runs, one at a time; an entity already imported (the loaded one included) is assembled from its
    // retained assets. They go with the loaded vehicle (Unload, the next load). -SCExtraVehicles=<Entity>[*N],... and
    // sc.Vehicle.Spawn / SpawnAt / List / Destroy / Clips / Clip / Focus.
    /** Queue an instance on free floor beside the loaded vehicle (FSCVehiclePlacement). Entity empty or "loaded": the
     *  loaded entity. Returns its id (>= 1; ids are not reused). */
    int32 SpawnVehicleInstance(const FString& Entity);
    /** Queue an instance at World: landing gear rested and its lowest visible point on World's Z (bRest false: World
     *  is the actor transform as given). */
    /** Level: a prepared level to stream for it (an obstacle's, as its record names it); empty: PreparedShipLevelFor. */
    int32 SpawnVehicleInstance(const FString& Entity, const FTransform& World, bool bRest = true, const FString& Level = FString());
    /** Remove an extra instance (queued, loading or ready); id 0 (the loaded vehicle) is refused. */
    bool DestroyVehicleInstance(int32 Id);
    /** The loaded vehicle first, then every ready extra instance by id. */
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") TArray<ASCVehicle*> GetVehicles() const;
    /** Every instance with its state (queued and failed ones too); the loaded vehicle is id 0. */
    TArray<FSCVehicleInstanceInfo> GetVehicleInstances() const;
    /** Id of a vehicle of the set (0 = the loaded one), INDEX_NONE for any other actor. */
    int32 GetVehicleInstanceId(const ASCVehicle* InVehicle) const;
    bool IsSpawningVehicles() const;
    /** Incremented whenever GetVehicles() changes (an instance ready or gone, the loaded vehicle changed, re-placement). */
    int32 GetVehiclesRevision() const;
    /** Broadcast on the same changes (game thread). */
    FSCOnVehiclesChanged OnVehiclesChanged;
    FSCVehicleInstances* GetVehicleInstanceSet() const { return VehicleInstances.Get(); }

private:
    bool Tick(float DeltaSeconds);
    /** LoadEntity's body: Cooked, when set, is the level streamed instead of an export from the archive. */
    bool StartLoad(const FString& Entity, USCViewOptions* InOptions, const FString& Cooked);
    void StepLoad(bool bBlocking);
    void StartImport();
    void FinishLoad(ESCLoadPhase Result, const FString& Error);
    /** bForPendingLoad: the previous ship goes for the load in progress (requests waiting for that load stay queued). */
    void UnloadInternal(bool bCollect, bool bKeepExtras = false, bool bForPendingLoad = false);
    bool bGarageLoad = false;
    void CompleteSearches(bool bBlocking);
    void LogPhaseChange();
    /** Archive lost to an FFI panic: leave Ready so the overlay offers Choose/Retry. */
    void PollClientPanic();
    /** Objects owned by an import session: the actor (and its subobjects) plus its /Temp/SC session packages. */
    static void TrackSession(const UObject* Actor, const FString& Session, TArray<TWeakObjectPtr<UObject>>& Out);
    /** Flush rendering, collect garbage and count tracked objects that still exist (garbage-marked included). */
    static int32 CollectAndCount(const TArray<TWeakObjectPtr<UObject>>& Tracked);
    UWorld* GetViewerWorld() const;

    TSharedPtr<ISCArchive, ESPMode::ThreadSafe> Client;
    FTSTicker::FDelegateHandle TickHandle;
    TSharedPtr<FSCViewerAutomation> Automation; // shared: type-erased deleter, the class stays private
    TSharedPtr<FSCVehicleInstances> VehicleInstances;

    ESCArchiveState ArchiveState = ESCArchiveState::Closed;
    uint32 ArchiveGeneration = 0;
    uint32 LoadSerial = 0;
    FText ArchiveError;
    FString ArchivePath, CacheDir;
    FString ArchiveBuildId;
    UE::Tasks::TTask<FSCOpenResult> OpenTask;
    double OpenSeconds = 0;
    FSCCacheStats CacheStats;
    TSharedPtr<ISCProcessedCache, ESPMode::ThreadSafe> ProcessedCache;
    TSharedPtr<ISCGeometryStreaming> GeometryStreaming;
    bool bGeometryCapturePrepared = false;

    struct FPendingSearch
    {
        UE::Tasks::TTask<FSCSearchResult> Task;
        TFunction<void(const FSCSearchResult&)> Callback;
        uint32 Generation = 0;
    };
    TArray<FPendingSearch> Searches;

    UPROPERTY() TObjectPtr<USCViewOptions> Options;
    /** Snapshot of the options of the running/last load. */
    UPROPERTY() TObjectPtr<USCViewOptions> LoadOptions;
    ESCLoadPhase Phase = ESCLoadPhase::Idle;
    FSCLoadReport Report;
    FString PendingEntity, PendingKey, LoadedEntity, LoadedKey;
    /** The prepared level the pending / loaded ship streams from (empty: imported). */
    FString PendingShipLevel, LoadedShipLevel;
    struct FStoredShip { FString Entity, Level; };
    TMap<FString, FStoredShip> StoredShipLevels; // lower-case bare entity -> it and the prepared level it was stored from
    bool bRebuildProcessed = false;
    double LoadStart = 0, ParseStart = 0, ImportStart = 0;
    FString LastPhaseKey;
    TSharedPtr<FSCExportProgress, ESPMode::ThreadSafe> ExportProgress;
    UE::Tasks::TTask<FSCExportResult> ExportTask;
    UE::Tasks::TTask<TSharedPtr<FSCTarPackageSource>> ParseTask;
    TSharedPtr<ISCImportJob> Job;
    TWeakObjectPtr<ASCVehicle> Vehicle;

    UPROPERTY() TObjectPtr<ASCFirstPersonCharacter> FirstPerson;
    void StepEnvironment();
    void BeginEnvironmentImport(TSharedPtr<FSCPackageSource> Source);
    void PlaceEnvironment();
    int32 EnvPlaceInTicks = 0;
    bool bShowcase = false;
    bool bStartSkinApplied = false; // -SCSkin is applied to the first loaded vehicle only
    void ApplyStartSkin();
    bool bBasicLightingPending = true; // SCLook::ApplyBasicLighting on the first tick with a world
    bool bMaxQualityPending = false; // -SCMaxQuality: SCLook::ApplyMaxQuality on the first tick with a world
    FString EnvironmentState = TEXT("open"); // door fragments' Open end pose: the ship elevator raised flush, ship delivered
    void ShowMapHangar(bool bShow);
    bool bSpace = false;
    TWeakObjectPtr<class ADirectionalLight> SpaceSun, SpaceFill;
    // Viewer choices (WP-K3): bright enough that auto exposure shows the sunlit side clearly; the fill lights the rest.
    float SpaceSunLux = 8000.f;
    static constexpr float SpaceFillShare = 0.1f;
    FString EnvironmentFolder; // a package folder, or an archive object-container path (*.socpak)
    FString PreferredEnvironment; // what SetEnvironment asked for; EnvironmentFolder may be a larger hangar of its style
    bool bFitEnvironment = false;
    void RequestEnvironment(const FString& Folder);
    /** The hangar for the loaded ship (see SetEnvironment); requests it when it differs from the current one. */
    void FitEnvironmentToShip();
    bool bEnvironmentPending = false;
    TSharedPtr<FSCPackageSource> EnvSource;
    TSharedPtr<FSCExportProgress, ESPMode::ThreadSafe> EnvProgress;
    UE::Tasks::TTask<FSCExportResult> EnvExport;
    UE::Tasks::TTask<TSharedPtr<FSCTarPackageSource>> EnvParse;
    TArray<FString> EnvironmentChoices;
    FSCViewerCatalog Catalog;
    UE::Tasks::TTask<FSCTextResult> EnvListTask;
    uint32 EnvListedGeneration = 0; // the archive session the catalogue was listed for
    uint32 EnvListIssued = 0;       // the archive session EnvListTask was issued against
    uint32 EnvArchiveGeneration = 0; // the archive session the environment's build started in
    bool bLoadCancelRequested = false;
    /** A saved hangar waiting for an archive that is not there (failed or closed): no work under way, so not busy
     *  (Retry, automation's archive-failure exit); it starts when an archive is ready. */
    bool EnvironmentAwaitsArchive() const;
    mutable bool bCatalogWanted = false;
    TSharedPtr<ISCImportJob> EnvJob;
    // WP-C1: a cooked environment level (Content/SC/Hangars/Maps) streamed in instead of the export/import pipeline.
    UPROPERTY() TObjectPtr<class ULevelStreamingDynamic> EnvLevel;
    void UnloadEnvironmentLevel();
    double EnvStart = 0;
    // A cooked ship level streamed in by LoadEntity (Phase Importing until its vehicle actor is visible).
    UPROPERTY() TObjectPtr<class ULevelStreamingDynamic> ShipLevel;
    void StepCookedShip();
    void UnloadShipLevel();
    TWeakObjectPtr<ASCVehicle> Environment;
    void StepPlayerKit(bool bBlocking);
    void FailPlayerKit(const FString& Error);
    EPlayerKitState KitState = EPlayerKitState::None;
    FString KitError;
    double KitStart = 0;
    TSharedPtr<FSCExportProgress, ESPMode::ThreadSafe> KitProgress;
    UE::Tasks::TTask<FSCExportResult> KitExport;
    UE::Tasks::TTask<TSharedPtr<FSCTarPackageSource>> KitParse;
    TSharedPtr<FSCTarPackageSource> KitSource;
    TSharedPtr<ISCImportJob> KitJob;
    struct FKitClips { TSharedPtr<FSCPlayerKit> Kit; TSharedPtr<FSCPlayerClipMap> Clips; FString Error; int32 Failed = 0; };
    UE::Tasks::TTask<FKitClips> KitClipsTask;
    UPROPERTY() TObjectPtr<ASCVehicle> KitActor;
    FString PreparedPlayerLevel;
    FString PreparedTraversal;
    UPROPERTY() TObjectPtr<class USCTraversalClipsData> TraversalData;
    bool bTraversalWarned = false;
    /** The prepared body's level instance while it streams and once it holds the body (KitActor is its actor). */
    UPROPERTY() TObjectPtr<class ULevelStreamingDynamic> KitLevel;
    void StepPreparedPlayerKit(bool bBlocking);
    void UnloadKitLevel();
    /** Released body levels the world has not removed yet; one whose load failed meanwhile is removed here. */
    UPROPERTY() TArray<TObjectPtr<ULevelStreamingDynamic>> ReleasedKitLevels;
    void SweepReleasedKitLevels();
    /** The kit's actor, level and data released (FailPlayerKit, ReleasePreparedPlayerKit). */
    void ReleaseKitObjects();
    TSharedPtr<const FSCPlayerKit> KitData;
    TSharedPtr<const FSCPlayerClipMap> KitClips;
    TWeakObjectPtr<ASCViewerPawn> InspectionPawn;
    ESCCameraMode CameraMode = ESCCameraMode::Orbit;
    TWeakObjectPtr<USceneComponent> Isolated, Focused;
    TMap<TWeakObjectPtr<UPrimitiveComponent>, bool> IsolationRestore;
};
