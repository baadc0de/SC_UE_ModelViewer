// RAII client for libscbridge (scbridge.h, ABI v2): one sc_handle, every call serialized by a host lock as the
// header requires, run on UE::Tasks workers; results are polled from the game thread. Errors are FText.
#pragma once
#include "CoreMinimal.h"
#include "Tasks/Task.h"
#include "scbridge.h"
#include <atomic>
#include "SCArchive.h"

class FJsonObject;

/** ISCArchive over the archive library (scbridge.h). */
class SCBRIDGEIMPORT_API FSCBridgeClient : public ISCArchive
{
public:
    FSCBridgeClient() = default;
    virtual ~FSCBridgeClient() override;
    FSCBridgeClient(const FSCBridgeClient&) = delete;
    FSCBridgeClient& operator=(const FSCBridgeClient&) = delete;

    /** Empty P4k auto-discovers the install; empty CacheDir disables the cache. CacheDir must be absolute. */
    virtual UE::Tasks::TTask<FSCOpenResult> Open(const FString& P4k, const FString& CacheDir) override;
    virtual UE::Tasks::TTask<FSCSearchResult> Search(const FString& Query, int32 Limit) override;
    virtual UE::Tasks::TTask<FSCTextResult> ClipCatalogue(const FString& Entity) override;
    /** The tar is copied out of the FFI blob (freed before return). Cancel through Progress. */
    virtual UE::Tasks::TTask<FSCExportResult> ExportDecomposed(const FString& Entity, const FSCExportOptions& Options,
                                                      TSharedRef<FSCExportProgress, ESPMode::ThreadSafe> Progress) override;
    /** Game environment (object container) package: sc_export_environment_mem; same rules as ExportDecomposed. */
    virtual UE::Tasks::TTask<FSCExportResult> ExportEnvironment(const FString& Socpak, const FSCExportOptions& Options,
                                                       TSharedRef<FSCExportProgress, ESPMode::ThreadSafe> Progress) override;
    /** JSON array of object-container paths matching every term of Query (sc_search_environments). */
    virtual UE::Tasks::TTask<FSCTextResult> SearchEnvironments(const FString& Query, int32 Limit) override;
    /** The viewer's pick lists, chosen from game data (sc_viewer_catalog; FSCViewerCatalog::Parse reads the JSON). For
     *  the archive session open when called: when another archive has been opened by the time it runs, it does not scan. */
    virtual UE::Tasks::TTask<FSCTextResult> ViewerCatalog() override;
    /** One DataCore record as JSON (sc_datacore_record): NameOrGuid is `Type.Name`, a bare record name (exact,
     *  case-insensitive) or a GUID. Objects carry `__type`; references are record GUID strings (look them up the
     *  same way); `__record_name` / `__record_id` name the record. No match or an ambiguous bare name fails. */
    virtual UE::Tasks::TTask<FSCTextResult> DataCoreRecord(const FString& NameOrGuid) override;
    /** The same, with OnDone called on the game thread. */
    virtual void DataCoreRecord(const FString& NameOrGuid, TFunction<void(const FSCTextResult&)> OnDone) override;
    /** An archive file as UTF-8 XML text (sc_p4k_read_xml: binary CryXML converted, element text kept); in Json.
     *  OnDone on the game thread. */
    virtual void P4kReadXml(const FString& Path, TFunction<void(const FSCTextResult&)> OnDone) override;
    /** A MaterialEffects FXLib entry `<FXLib>:<effect>` as JSON (sc_material_effect: its particle effect references,
     *  audio, random groups). OnDone on the game thread. */
    virtual void MaterialEffect(const FString& LibraryAndEffect, TFunction<void(const FSCTextResult&)> OnDone) override;
    /** Player kit (scbridge_export_player_request, PlayerRequest JSON): empty Gender = male, empty Loadout = the
     *  exporter's default loadout, empty Weapons = every loadout weapon; bSpareAttachments puts each kept weapon's
     *  default attachments (its magazine) into the wearer's empty storage ports. */
    virtual UE::Tasks::TTask<FSCExportResult> ExportPlayer(const FString& Gender, const FString& Loadout, const TArray<FString>& Weapons, bool bSpareAttachments,
                                                  TSharedRef<FSCExportProgress, ESPMode::ThreadSafe> Progress) override;
    /** The archive's authored soundtrack catalogue and one selected Ogg cue. */
    virtual UE::Tasks::TTask<FSCTextResult> MusicCatalogue() override;
    virtual UE::Tasks::TTask<FSCMusicBytesResult> MusicRead(const FString& CueId) override;
    /** Blocking; waits for a running export (host lock). */
    /** Clip-only player actions, including seat transitions. */
    virtual UE::Tasks::TTask<FSCExportResult> ExportPlayerClips(const FString& Gender, const TArray<FString>& Clips,
        TSharedRef<FSCExportProgress, ESPMode::ThreadSafe> Progress) override;
    virtual FSCCacheStats CacheStats() override;
    virtual bool IsOpen() const override;
    /** True once after any call hit SC_STATUS_PANIC (the handle is already closed); the host must reopen. */
    virtual bool TakePanic(FText& OutError) override;
#if WITH_DEV_AUTOMATION_TESTS
    /** Tests: a panic notice as FailLocked leaves it (the handle is not touched). */
    void NotePanicForTest(const FText& Error)
    {
        FScopeLock PanicGuard(&PanicLock);
        PanicError = Error;
        bPanicPending = true;
    }
#endif
    /** Core defaults (sc_default_options); no handle needed. */
    static sc_options DefaultOptions();
    /** Options as the library takes them. */
    static sc_options ToFfi(const FSCExportOptions& Options);

private:
    FText LastError(sc_status Status) const; // caller holds HandleLock
    /** Caller holds HandleLock: error text for a failed call; on PANIC closes the poisoned handle and flags it. */
    FText FailLocked(sc_status Status, bool& bOutPanicked);
    using FPlayerExport = decltype(&scbridge_export_player_request);
    UE::Tasks::TTask<FSCExportResult> ExportPlayerJson(const TSharedRef<FJsonObject>& Request, FPlayerExport Export,
        TSharedRef<FSCExportProgress, ESPMode::ThreadSafe> Progress);
    FSCCacheStats CacheStatsLocked();
    /** A text-returning call (Call(handle, Argument, &out)) off the game thread; OnDone on the game thread. */
    void TextCall(const FString& Argument, sc_status (*Call)(sc_handle*, const char*, char**), TFunction<void(const FSCTextResult&)> OnDone);
    mutable FCriticalSection HandleLock; // serializes ALL calls on Handle incl. last_error/close (scbridge.h)
    sc_handle* Handle = nullptr;
    std::atomic<uint32> Session{0}; // advanced by each Open (under HandleLock): a call can tell its session was replaced
    FCriticalSection PanicLock;
    bool bPanicPending = false;
    FText PanicError;
};
