// libscbridge client: handle lifetime, host lock, worker execution and JSON/blob conversion.
#include "SCBridgeClient.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCBridgeClient, Log, All);

namespace
{
FText StatusText(sc_status Status, const char* Message)
{
    static const TCHAR* Names[] = {TEXT("ok"), TEXT("invalid argument"), TEXT("error"), TEXT("panic"), TEXT("unsupported"), TEXT("cancelled")};
    const TCHAR* Name = int32(Status) >= 0 && int32(Status) < UE_ARRAY_COUNT(Names) ? Names[Status] : TEXT("unknown status");
    const FString Detail = Message ? FString(UTF8_TO_TCHAR(Message)) : FString();
    return FText::FromString(Detail.IsEmpty() ? FString(Name) : FString::Printf(TEXT("%s (%s)"), *Detail, Name));
}
} // namespace

void FSCExportProgress::Report(const char* InPhase, uint64 Done, uint64 Total)
{
    Permille = uint32(Total ? FMath::Min<uint64>(1000, Done * 1000 / Total) : 0);
    FScopeLock Guard(&Lock);
    Phase = UTF8_TO_TCHAR(InPhase ? InPhase : "");
}
FString FSCExportProgress::GetPhase() const
{
    FScopeLock Guard(&Lock);
    return Phase;
}
int FSCExportProgress::Callback(const char* Phase, uint64_t Done, uint64_t Total, void* User)
{
    // May run on rayon threads; must not unwind (no throwing code here).
    auto* Progress = static_cast<FSCExportProgress*>(User);
    Progress->Report(Phase, Done, Total);
    return Progress->IsCancelRequested() ? 1 : 0;
}

FSCBridgeClient::~FSCBridgeClient()
{
    FScopeLock Guard(&HandleLock);
    sc_close(Handle);
    Handle = nullptr;
}
bool FSCBridgeClient::IsOpen() const
{
    FScopeLock Guard(&HandleLock);
    return Handle != nullptr;
}
sc_options FSCBridgeClient::DefaultOptions()
{
    sc_options Options;
    FMemory::Memzero(Options);
    sc_default_options(&Options);
    return Options;
}
FText FSCBridgeClient::LastError(sc_status Status) const
{
    return StatusText(Status, sc_last_error(Handle));
}
FText FSCBridgeClient::FailLocked(sc_status Status, bool& bOutPanicked)
{
    bOutPanicked = Status == SC_STATUS_PANIC;
    const FText Error = LastError(Status);
    if (bOutPanicked)
    {
        // scbridge.h: PANIC poisons the handle and the call's outputs are unspecified (never read or freed);
        // close it here, under the lock, so no other path can use it. The host must reopen the archive.
        sc_close(Handle);
        Handle = nullptr;
        FScopeLock PanicGuard(&PanicLock);
        PanicError = FText::Format(NSLOCTEXT("SCBridge", "Panicked", "The archive session crashed and was closed: {0}"), Error);
        bPanicPending = true;
        UE_LOG(LogSCBridgeClient, Error, TEXT("FFI_PANIC %s"), *Error.ToString());
    }
    return Error;
}
bool FSCBridgeClient::TakePanic(FText& OutError)
{
    FScopeLock PanicGuard(&PanicLock);
    if (!bPanicPending) return false;
    bPanicPending = false;
    OutError = PanicError;
    return true;
}

UE::Tasks::TTask<FSCOpenResult> FSCBridgeClient::Open(const FString& P4k, const FString& CacheDir)
{
    return UE::Tasks::Launch(UE_SOURCE_LOCATION, [Self = AsShared(), P4k, CacheDir] {
        FSCOpenResult Result;
        const double Start = FPlatformTime::Seconds();
        FScopeLock Guard(&Self->HandleLock);
        sc_close(Self->Handle);
        Self->Handle = nullptr;
        const FTCHARToUTF8 P4kUtf8(*P4k), CacheUtf8(*CacheDir);
        sc_handle* Handle = nullptr;
        const sc_status Status = sc_open(P4k.IsEmpty() ? nullptr : P4kUtf8.Get(), CacheDir.IsEmpty() ? nullptr : CacheUtf8.Get(), &Handle);
        Result.Seconds = FPlatformTime::Seconds() - Start;
        if (Status != SC_STATUS_OK)
        {
            Result.Error = StatusText(Status, sc_last_error(nullptr)); // same thread as sc_open
            return Result;
        }
        Self->Handle = Handle;
        {
            FScopeLock PanicGuard(&Self->PanicLock);
            Self->bPanicPending = false; // a fresh session supersedes an unreported crash of the old one
        }
        Result.bOK = true;
        Result.bIndexCacheHit = Self->CacheStatsLocked().bLastHit;
        return Result;
    }, UE::Tasks::ETaskPriority::BackgroundNormal);
}

UE::Tasks::TTask<FSCSearchResult> FSCBridgeClient::Search(const FString& Query, int32 Limit)
{
    return UE::Tasks::Launch(UE_SOURCE_LOCATION, [Self = AsShared(), Query, Limit] {
        FSCSearchResult Result;
        Result.Query = Query;
        char* Json = nullptr;
        double Start = 0;
        {
            FScopeLock Guard(&Self->HandleLock); // blocks while an export runs (header contract)
            Start = FPlatformTime::Seconds();      // report the search itself, not the wait for the lock
            if (!Self->Handle)
            {
                Result.Error = NSLOCTEXT("SCBridge", "NoArchive", "No archive is open");
                return Result;
            }
            const sc_status Status = sc_search_entities(Self->Handle, TCHAR_TO_UTF8(*Query), uint32(FMath::Max(1, Limit)), &Json);
            if (Status != SC_STATUS_OK)
            {
                Result.Error = Self->FailLocked(Status, Result.bPanicked); // *out is NULL or unspecified: not freed
                return Result;
            }
        }
        const FString Text = UTF8_TO_TCHAR(Json);
        sc_free_string(Json);
        TArray<TSharedPtr<FJsonValue>> Values;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Values))
        {
            Result.Error = NSLOCTEXT("SCBridge", "BadSearchJson", "Malformed search result");
            return Result;
        }
        for (const auto& Value : Values)
        {
            const TSharedPtr<FJsonObject>* Object;
            if (!Value->TryGetObject(Object)) continue;
            FSCEntityHit& Hit = Result.Hits.AddDefaulted_GetRef();
            (*Object)->TryGetStringField(TEXT("name"), Hit.Name);
            (*Object)->TryGetStringField(TEXT("display_name"), Hit.DisplayName);
            (*Object)->TryGetStringField(TEXT("category"), Hit.Category);
            (*Object)->TryGetStringField(TEXT("geometry_path"), Hit.GeometryPath);
            (*Object)->TryGetStringField(TEXT("guid"), Hit.Guid);
        }
        Result.Seconds = FPlatformTime::Seconds() - Start;
        return Result;
    });
}

UE::Tasks::TTask<FSCTextResult> FSCBridgeClient::ClipCatalogue(const FString& Entity)
{
    return UE::Tasks::Launch(UE_SOURCE_LOCATION, [Self = AsShared(), Entity] {
        FSCTextResult Result;
        FScopeLock Guard(&Self->HandleLock);
        char* Json = nullptr;
        const sc_status Status = Self->Handle ? sc_clip_catalogue(Self->Handle, TCHAR_TO_UTF8(*Entity), &Json) : SC_STATUS_INVALID_ARGUMENT;
        if (Status != SC_STATUS_OK)
        {
            Result.Error = Self->Handle ? Self->FailLocked(Status, Result.bPanicked) : NSLOCTEXT("SCBridge", "NoArchive", "No archive is open");
            return Result;
        }
        Result.Json = UTF8_TO_TCHAR(Json);
        sc_free_string(Json);
        Result.bOK = true;
        return Result;
    });
}

UE::Tasks::TTask<FSCExportResult> FSCBridgeClient::ExportDecomposed(const FString& Entity, const sc_options& Options,
                                                                   TSharedRef<FSCExportProgress, ESPMode::ThreadSafe> Progress)
{
    return UE::Tasks::Launch(UE_SOURCE_LOCATION, [Self = AsShared(), Entity, Options, Progress] {
        FSCExportResult Result;
        const double Start = FPlatformTime::Seconds();
        FScopeLock Guard(&Self->HandleLock); // held for the whole export: search/close/last_error wait
        if (!Self->Handle)
        {
            Result.Error = NSLOCTEXT("SCBridge", "NoArchive", "No archive is open");
            return Result;
        }
        sc_blob Blob{nullptr, 0};
        // Progress (the callback's user context) is kept alive by this lambda until the call returns.
        const sc_status Status = sc_export_decomposed_mem(Self->Handle, TCHAR_TO_UTF8(*Entity), &Options, &FSCExportProgress::Callback,
                                                          &Progress.Get(), &Blob);
        Result.Seconds = FPlatformTime::Seconds() - Start;
        const uint64 BlobBytes = Status == SC_STATUS_OK ? uint64(Blob.len) : 0; // sc_free_blob zeroes len
        if (Status == SC_STATUS_OK)
        {
            // FSCTarPackageSource holds a TArray<uint8> (int32 count): refuse what it cannot index instead of narrowing.
            if (Blob.len > size_t(MAX_int32))
                Result.Error = FText::Format(NSLOCTEXT("SCBridge", "TarTooLarge",
                                                       "The exported package is {0} bytes; the importer accepts at most {1}. Try a higher LOD or mip."),
                                             FText::AsNumber(uint64(Blob.len)), FText::AsNumber(MAX_int32));
            else
            {
                Result.Tar.SetNumUninitialized(int32(Blob.len));
                FMemory::Memcpy(Result.Tar.GetData(), Blob.data, Blob.len);
                Result.bOK = true;
            }
            sc_free_blob(&Blob); // freed on every OK path, including the refused size
        }
        else
        {
            Result.bCancelled = Status == SC_STATUS_CANCELLED;
            Result.Error = Self->FailLocked(Status, Result.bPanicked);
            // Ordinary failures return an empty blob (freeing it is a no-op); after PANIC the blob is unspecified and
            // must not be touched, so it is left alone (a bounded leak in a crashed session).
            if (!Result.bPanicked) sc_free_blob(&Blob);
        }
        if (Self->Handle) Result.Cache = Self->CacheStatsLocked();
        UE_LOG(LogSCBridgeClient, Display, TEXT("FFI_EXPORT entity=%s status=%d seconds=%.3f bytes=%llu accepted=%d cache_hit=%d cache_bytes=%llu"),
               *Entity, int32(Status), Result.Seconds, BlobBytes, Result.bOK, Result.Cache.bLastHit, Result.Cache.Bytes);
        return Result;
    }, UE::Tasks::ETaskPriority::BackgroundNormal);
}

FSCCacheStats FSCBridgeClient::CacheStats()
{
    FScopeLock Guard(&HandleLock);
    return CacheStatsLocked();
}
FSCCacheStats FSCBridgeClient::CacheStatsLocked()
{
    FSCCacheStats Stats;
    sc_cache_info Info;
    FMemory::Memzero(Info);
    const sc_status Status = Handle ? sc_cache_stats(Handle, &Info) : SC_STATUS_INVALID_ARGUMENT;
    if (Status == SC_STATUS_PANIC)
    {
        bool bPanicked;
        FailLocked(Status, bPanicked);
    }
    else if (Status == SC_STATUS_OK)
    {
        Stats.bEnabled = Info.enabled != 0;
        Stats.bLastHit = Info.last_hit != 0;
        Stats.Bytes = Info.bytes;
        Stats.Entries = Info.entries;
        Stats.CapBytes = Info.cap_bytes;
    }
    return Stats;
}
