// RAII client for libscbridge (scbridge.h, ABI v2): one sc_handle, every call serialized by a host lock as the
// header requires, run on UE::Tasks workers; results are polled from the game thread. Errors are FText.
#pragma once
#include "CoreMinimal.h"
#include "Tasks/Task.h"
#include "scbridge.h"
#include <atomic>
#include "SCBridgeClient.generated.h"

/** One sc_search_entities hit (core EntityHit JSON). */
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

struct SCBRIDGE_API FSCOpenResult
{
    bool bOK = false;
    FText Error;
    double Seconds = 0;
    bool bIndexCacheHit = false;
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
struct SCBRIDGE_API FSCCacheStats
{
    bool bEnabled = false, bLastHit = false;
    uint64 Bytes = 0, Entries = 0, CapBytes = 0;
};
struct SCBRIDGE_API FSCExportResult
{
    bool bOK = false, bCancelled = false, bPanicked = false;
    FText Error;
    TArray<uint8> Tar;
    double Seconds = 0;
    FSCCacheStats Cache; // after the export: bLastHit is the package-cache hit of this export
};
struct SCBRIDGE_API FSCTextResult
{
    bool bOK = false, bPanicked = false;
    FString Json;
    FText Error;
};

class SCBRIDGE_API FSCBridgeClient : public TSharedFromThis<FSCBridgeClient, ESPMode::ThreadSafe>
{
public:
    FSCBridgeClient() = default;
    ~FSCBridgeClient();
    FSCBridgeClient(const FSCBridgeClient&) = delete;
    FSCBridgeClient& operator=(const FSCBridgeClient&) = delete;

    /** Empty P4k auto-discovers the install; empty CacheDir disables the cache. CacheDir must be absolute. */
    UE::Tasks::TTask<FSCOpenResult> Open(const FString& P4k, const FString& CacheDir);
    UE::Tasks::TTask<FSCSearchResult> Search(const FString& Query, int32 Limit);
    UE::Tasks::TTask<FSCTextResult> ClipCatalogue(const FString& Entity);
    /** The tar is copied out of the FFI blob (freed before return). Cancel through Progress. */
    UE::Tasks::TTask<FSCExportResult> ExportDecomposed(const FString& Entity, const sc_options& Options,
                                                      TSharedRef<FSCExportProgress, ESPMode::ThreadSafe> Progress);
    /** Blocking; waits for a running export (host lock). */
    FSCCacheStats CacheStats();
    bool IsOpen() const;
    /** True once after any call hit SC_STATUS_PANIC (the handle is already closed); the host must reopen. */
    bool TakePanic(FText& OutError);
    /** Core defaults (sc_default_options); no handle needed. */
    static sc_options DefaultOptions();

private:
    FText LastError(sc_status Status) const; // caller holds HandleLock
    /** Caller holds HandleLock: error text for a failed call; on PANIC closes the poisoned handle and flags it. */
    FText FailLocked(sc_status Status, bool& bOutPanicked);
    FSCCacheStats CacheStatsLocked();
    mutable FCriticalSection HandleLock; // serializes ALL calls on Handle incl. last_error/close (scbridge.h)
    sc_handle* Handle = nullptr;
    FCriticalSection PanicLock;
    bool bPanicPending = false;
    FText PanicError;
};
