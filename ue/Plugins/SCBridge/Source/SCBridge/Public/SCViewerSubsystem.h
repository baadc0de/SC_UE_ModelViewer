// Viewer application state: owns the FFI client, the current ASCVehicle and the load pipeline
// (FFI export on a worker -> tar parse on a worker -> FSCImportJob stepped on the game thread), camera mode,
// focus/isolate and object-reuse by entity + options hash. Blueprint/Python-callable; UI polls it every frame.
#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "Tasks/Task.h"
#include "SCBridgeClient.h"
#include "SCPlayerAnimator.h"
#include "SCViewerSubsystem.generated.h"

class ASCVehicle;
class ASCViewerPawn;
class ASCFirstPersonCharacter;
class USCViewOptions;
class FSCImportJob;
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
     *  Also -SCEnvironment=<...>, `sc.Environment <...|none>` and USCViewerSettings::Environment. */
    UFUNCTION(BlueprintCallable, Category = "SC|Viewer") void SetEnvironment(const FString& Folder);
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") ASCVehicle* GetEnvironment() const { return Environment.Get(); }
    /** The requested environment (folder, archive path or empty) and the archive's hangars (listed once per archive). */
    FString GetEnvironmentRequest() const { return EnvironmentFolder; }
    const TArray<FString>& GetEnvironmentChoices() const { return EnvironmentChoices; }
    bool IsEnvironmentLoading() const { return bEnvironmentPending || EnvJob.IsValid() || EnvExport.IsValid() || EnvParse.IsValid(); }
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") ESCLoadPhase GetLoadPhase() const { return Phase; }
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") float GetLoadProgress() const;
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") FString GetLoadPhaseText() const;
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") FSCLoadReport GetLastReport() const { return Report; }
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") ASCVehicle* GetVehicle() const;
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") FString GetLoadedEntity() const { return LoadedEntity; }
    UFUNCTION(BlueprintPure, Category = "SC|Viewer") FString GetPendingEntity() const { return PendingEntity; }
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
    /** Cache statistics as of the last export/open (no FFI call). */
    const FSCCacheStats& GetCacheStats() const { return CacheStats; }

    // Player body (WP-P2): the kit is exported, imported and its clips read asynchronously the first time first
    // person is entered, then kept for every later first-person session. Capsule-only first person meanwhile.
    enum class EPlayerKitState : uint8 { None, Exporting, Parsing, Importing, Clips, Ready, Failed };
    /** Starts the player-kit pipeline unless it is running or ready (a failed kit is retried). */
    bool RequestPlayerKit();
    EPlayerKitState GetPlayerKitState() const { return KitState; }
    FString GetPlayerKitStatus() const;
    /** Pumps the kit pipeline until ready or failed (automation, tests). */
    bool WaitForPlayerKit(float TimeoutSeconds = 600.f);
    ASCVehicle* GetPlayerKitActor() const { return KitActor; }
    /** Attach the ready kit to the first-person character (no-op if none, not ready or attached). */
    void AttachPlayerKit();

private:
    bool Tick(float DeltaSeconds);
    void StepLoad(bool bBlocking);
    void StartImport();
    void FinishLoad(ESCLoadPhase Result, const FString& Error);
    void UnloadInternal(bool bCollect);
    void CompleteSearches(bool bBlocking);
    void LogPhaseChange();
    /** Archive lost to an FFI panic: leave Ready so the overlay offers Choose/Retry. */
    void PollClientPanic();
    /** Objects owned by an import session: the actor (and its subobjects) plus its /Temp/SC session packages. */
    static void TrackSession(const UObject* Actor, const FString& Session, TArray<TWeakObjectPtr<UObject>>& Out);
    /** Flush rendering, collect garbage and count tracked objects that still exist (garbage-marked included). */
    static int32 CollectAndCount(const TArray<TWeakObjectPtr<UObject>>& Tracked);
    UWorld* GetViewerWorld() const;

    TSharedPtr<FSCBridgeClient, ESPMode::ThreadSafe> Client;
    FTSTicker::FDelegateHandle TickHandle;
    TSharedPtr<FSCViewerAutomation> Automation; // shared: type-erased deleter, the class stays private

    ESCArchiveState ArchiveState = ESCArchiveState::Closed;
    uint32 ArchiveGeneration = 0;
    FText ArchiveError;
    FString ArchivePath, CacheDir;
    UE::Tasks::TTask<FSCOpenResult> OpenTask;
    double OpenSeconds = 0;
    FSCCacheStats CacheStats;

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
    double LoadStart = 0, ParseStart = 0, ImportStart = 0;
    FString LastPhaseKey;
    TSharedPtr<FSCExportProgress, ESPMode::ThreadSafe> ExportProgress;
    UE::Tasks::TTask<FSCExportResult> ExportTask;
    UE::Tasks::TTask<TSharedPtr<FSCTarPackageSource>> ParseTask;
    TSharedPtr<FSCImportJob> Job;
    TWeakObjectPtr<ASCVehicle> Vehicle;

    UPROPERTY() TObjectPtr<ASCFirstPersonCharacter> FirstPerson;
    void StepEnvironment();
    void BeginEnvironmentImport(TSharedPtr<FSCPackageSource> Source);
    void ShowMapHangar(bool bShow);
    FString EnvironmentFolder; // a package folder, or an archive object-container path (*.socpak)
    bool bEnvironmentPending = false;
    TSharedPtr<FSCPackageSource> EnvSource;
    TSharedPtr<FSCExportProgress, ESPMode::ThreadSafe> EnvProgress;
    UE::Tasks::TTask<FSCExportResult> EnvExport;
    UE::Tasks::TTask<TSharedPtr<FSCTarPackageSource>> EnvParse;
    TArray<FString> EnvironmentChoices;
    UE::Tasks::TTask<FSCTextResult> EnvListTask;
    uint32 EnvListedGeneration = 0;
    TSharedPtr<FSCImportJob> EnvJob;
    double EnvStart = 0;
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
    TSharedPtr<FSCImportJob> KitJob;
    struct FKitClips { TSharedPtr<FSCPlayerKit> Kit; TSharedPtr<FSCPlayerClipMap> Clips; FString Error; int32 Failed = 0; };
    UE::Tasks::TTask<FKitClips> KitClipsTask;
    UPROPERTY() TObjectPtr<ASCVehicle> KitActor;
    TSharedPtr<const FSCPlayerKit> KitData;
    TSharedPtr<const FSCPlayerClipMap> KitClips;
    TWeakObjectPtr<ASCViewerPawn> InspectionPawn;
    ESCCameraMode CameraMode = ESCCameraMode::Orbit;
    TWeakObjectPtr<USceneComponent> Isolated, Focused;
    TMap<TWeakObjectPtr<UPrimitiveComponent>, bool> IsolationRestore;
};
