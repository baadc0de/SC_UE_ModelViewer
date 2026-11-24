// WP-O1-4c-3b: the game archive (Data.p4k) as the runtime sees it: open, search, export packages, read DataCore records,
// archive files, effects and music, behind ISCArchive. The archive library's client (FSCBridgeClient, the importer)
// implements it; the runtime never includes scbridge.h, so a build without the importer (a packaged game) has no archive:
// ISCArchive::Create returns null.
#pragma once
#include "CoreMinimal.h"
#include "Tasks/Task.h"
#include <atomic>
#include "SCArchive.generated.h"

/** One entity search hit (core EntityHit JSON). */
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCEntityHit
{
    GENERATED_BODY()
    /** Exact DataCore class name, the export key. */
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString Name;
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString DisplayName;
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString Category;
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString GeometryPath;
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString Guid;
};

/** Export progress shared by the FFI callback (any thread) and the game thread; also carries the cancel request. */
class SCBRIDGE_API FSCExportProgress
{
public:
    void Report(const char* Phase, uint64 Done, uint64 Total);
    FString GetPhase() const;
    float GetFraction() const { return float(Permille.load()) / 1000.f; }
    void RequestCancel() { bCancel = true; }
    bool IsCancelRequested() const { return bCancel; }
    /** sc_progress_cb thunk; User is an FSCExportProgress*. Returns nonzero to cancel. */
    static int Callback(const char* Phase, uint64_t Done, uint64_t Total, void* User);

private:
    mutable FCriticalSection Lock;
    FString Phase;
    std::atomic<uint32> Permille{0};
    std::atomic<bool> bCancel{false};
};

struct SCBRIDGE_API FSCCacheStats
{
    bool bEnabled = false, bLastHit = false;
    uint64 Bytes = 0, Entries = 0, CapBytes = 0;
};
struct SCBRIDGE_API FSCOpenResult
{
    bool bOK = false;
    FText Error;
    FString ArchiveBuildId;
    double Seconds = 0;
    bool bIndexCacheHit = false;
    FSCCacheStats Cache; // read by the open worker under the handle lock (the game thread never waits for it)
};
struct SCBRIDGE_API FSCSearchResult
{
    FString Query;
    TArray<FSCEntityHit> Hits;
    FText Error;
    bool bPanicked = false;
    /** Archive generation the search was issued in (set by the host; results of older generations are dropped). */
    uint32 Generation = 0;
    double Seconds = 0;
};
struct SCBRIDGE_API FSCExportResult
{
    bool bOK = false, bCancelled = false, bPanicked = false;
    FText Error;
    TArray64<uint8> Tar;
    double Seconds = 0;
    FSCCacheStats Cache; // after the export: bLastHit is the package-cache hit of this export
};
struct SCBRIDGE_API FSCTextResult
{
    bool bOK = false, bPanicked = false;
    FString Json;
    FText Error;
};

struct FSCMusicBytesResult
{
    TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Ogg;
    FText Error;
};

/** The export options (mirror of scbridge's sc_options). Defaults: the core's (sc_default_options; the importer's test
 *  SCBridge.Archive.ExportOptionsDefaults keeps them equal). */
struct SCBRIDGE_API FSCExportOptions
{
    enum : uint32 { MaterialTextures = 0, MaterialColors = 1, MaterialNone = 2 };
    uint32 Lod = 0;
    uint32 Mip = 0;
    uint32 MaterialMode = MaterialTextures;
    uint32 Attachments = 1;
    uint32 Interior = 1;
    uint32 Lights = 1;
    uint32 Shields = 0;
    uint32 Animations = 1;
    uint32 DefaultPose = 0;
};

/** The game archive. Calls run on task workers and are serialized by the implementation; results are polled from the
 *  game thread (or delivered there, the OnDone forms). */
class SCBRIDGE_API ISCArchive : public TSharedFromThis<ISCArchive, ESPMode::ThreadSafe>
{
public:
    virtual ~ISCArchive() = default;
    /** A new archive client from the importer (registered by it); null in a build without one. */
    static TSharedPtr<ISCArchive, ESPMode::ThreadSafe> Create();
    /** The importer registers how to make one (its module startup); an empty function unregisters. */
    static void SetFactory(TFunction<TSharedPtr<ISCArchive, ESPMode::ThreadSafe>()> Factory);
    /** The gender ExportPlayer asks for when none is given. */
    static constexpr const TCHAR* DefaultPlayerGender = TEXT("male");

    /** Empty P4k auto-discovers the install; empty CacheDir disables the cache. CacheDir must be absolute. */
    virtual UE::Tasks::TTask<FSCOpenResult> Open(const FString& P4k, const FString& CacheDir) = 0;
    virtual UE::Tasks::TTask<FSCSearchResult> Search(const FString& Query, int32 Limit) = 0;
    virtual UE::Tasks::TTask<FSCTextResult> ClipCatalogue(const FString& Entity) = 0;
    /** A vehicle's decomposed package (tar). Cancel through Progress. */
    virtual UE::Tasks::TTask<FSCExportResult> ExportDecomposed(const FString& Entity, const FSCExportOptions& Options,
                                                              TSharedRef<FSCExportProgress, ESPMode::ThreadSafe> Progress) = 0;
    /** A game environment's (object container's) package; same rules as ExportDecomposed. */
    virtual UE::Tasks::TTask<FSCExportResult> ExportEnvironment(const FString& Socpak, const FSCExportOptions& Options,
                                                               TSharedRef<FSCExportProgress, ESPMode::ThreadSafe> Progress) = 0;
    /** JSON array of object-container paths matching every term of Query. */
    virtual UE::Tasks::TTask<FSCTextResult> SearchEnvironments(const FString& Query, int32 Limit) = 0;
    /** The viewer's pick lists, chosen from game data (FSCViewerCatalog::Parse reads the JSON). */
    virtual UE::Tasks::TTask<FSCTextResult> ViewerCatalog() = 0;
    /** One DataCore record as JSON: NameOrGuid is `Type.Name`, a bare record name (exact, case-insensitive) or a GUID. */
    virtual UE::Tasks::TTask<FSCTextResult> DataCoreRecord(const FString& NameOrGuid) = 0;
    /** The same, with OnDone called on the game thread. */
    virtual void DataCoreRecord(const FString& NameOrGuid, TFunction<void(const FSCTextResult&)> OnDone) = 0;
    /** An archive file as UTF-8 XML text (binary CryXML converted); OnDone on the game thread. */
    virtual void P4kReadXml(const FString& Path, TFunction<void(const FSCTextResult&)> OnDone) = 0;
    /** A MaterialEffects FXLib entry `<FXLib>:<effect>` as JSON; OnDone on the game thread. */
    virtual void MaterialEffect(const FString& LibraryAndEffect, TFunction<void(const FSCTextResult&)> OnDone) = 0;
    /** Player kit: empty Gender = DefaultPlayerGender, empty Loadout = the exporter's default loadout, empty Weapons =
     *  every loadout weapon; bSpareAttachments puts each kept weapon's default attachments into the wearer's empty
     *  storage ports. */
    virtual UE::Tasks::TTask<FSCExportResult> ExportPlayer(const FString& Gender, const FString& Loadout, const TArray<FString>& Weapons,
                                                          bool bSpareAttachments, TSharedRef<FSCExportProgress, ESPMode::ThreadSafe> Progress) = 0;
    /** The archive's authored soundtrack catalogue and one selected Ogg cue. */
    virtual UE::Tasks::TTask<FSCTextResult> MusicCatalogue() = 0;
    virtual UE::Tasks::TTask<FSCMusicBytesResult> MusicRead(const FString& CueId) = 0;
    /** Clip-only player actions, including seat transitions. */
    virtual UE::Tasks::TTask<FSCExportResult> ExportPlayerClips(const FString& Gender, const TArray<FString>& Clips,
                                                               TSharedRef<FSCExportProgress, ESPMode::ThreadSafe> Progress) = 0;
    /** Blocking; waits for a running export. */
    virtual FSCCacheStats CacheStats() = 0;
    virtual bool IsOpen() const = 0;
    /** True once after any call hit a panic (the archive is already closed); the host must reopen. */
    virtual bool TakePanic(FText& OutError) = 0;
};
