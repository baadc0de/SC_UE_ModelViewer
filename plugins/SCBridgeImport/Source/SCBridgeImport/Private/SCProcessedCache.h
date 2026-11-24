// Project-local storage for imported platform representations. An entry's files
// are immutable once its manifest record is published.
#pragma once

#include "CoreMinimal.h"
#include "SCImport.h"
#include "HAL/CriticalSection.h"
#include "UObject/WeakObjectPtrTemplates.h"

class FSCProcessedCache : public ISCProcessedCache
{
public:
    using FStats = FSCProcessedCacheStats;
    static constexpr const TCHAR* PayloadName = TEXT("data.bin");

    FSCProcessedCache(FString InRoot, int64 InCapBytes);
    virtual ~FSCProcessedCache() override;

    virtual bool IsEnabled() const override { return bEnabled && GetCapBytes() > 0; }
    virtual const ISCImporting* GetImporter() const override;
    const FString& GetRoot() const { return Root; }
    FString PackageName(const FString& Directory, const FString& Asset) const;
    static FString Key(const FString& Namespace, const FString& Input);
    static FString Digest(TConstArrayView64<uint8> Data);
    bool ReadBlob(const FString& Key, TArray<uint8>& Out);
    bool WriteBlob(const FString& Key, TConstArrayView<uint8> Data);

    /** Validate all files before giving the package loader a path. */
    bool Find(const FString& Key, FString& Directory);
    /** Protect a validated directory until its load finishes or is cancelled. */
    class FReadPin final
    {
    public:
        ~FReadPin();
        const FString Directory;
        /** Retire this generation if application-level validation fails after checksum validation. */
        void Invalidate() const;
        FReadPin(const FReadPin&) = delete;
        FReadPin& operator=(const FReadPin&) = delete;
    private:
        friend class FSCProcessedCache;
        FReadPin(FSCProcessedCache& InCache, const FString& InKey, FString InDirectory);
        FSCProcessedCache& Cache;
        const FString Key;
    };
    TUniquePtr<FReadPin> PinEntry(const FString& Key);
    /** Keep the files for a loaded asset's bulk-data readers until its UObject is purged. */
    void KeepLoaded(const FString& Directory, UObject* Asset, bool bUsable = true);
    UObject* GetLoaded(const FString& Directory) const;
    FString BeginWrite();
    bool CommitWrite(const FString& Key, const FString& Directory);
    void AbortWrite(const FString& Directory);

    virtual FStats Stats() const override;
    virtual int64 GetCapBytes() const override;
    virtual void SetCapBytes(int64 Bytes) override;
    virtual void Clear() override;
    bool Flush();

private:
    struct FFile
    {
        int64 Bytes = 0;
        FString Hash;
    };
    struct FEntry
    {
        FString Generation;
        int64 LastUse = 0, Bytes = 0;
        TMap<FString, FFile> Files;
    };
    FString Root, Mount;
    int64 CapBytes = 0, TotalBytes = 0, Clock = 0;
    bool bEnabled = false, bDirty = false;
    TUniquePtr<FSystemWideCriticalSection> Owner;
    mutable FCriticalSection Mutex;
    TMap<FString, FEntry> Entries;
    struct FResident
    {
        TWeakObjectPtr<UObject> Asset;
        bool bUsable = false;
    };
    TSet<FString> Staging;
    TMap<FString, int32> Pins;
    TMap<FString, FResident> Resident;
    TMap<FString, int64> Retired;
    FDelegateHandle PostPurge;

    static bool FileDigest(const FString& Path, int64 ExpectedSize, FString& Out);
    TUniquePtr<FReadPin> PinForRead(const FString& Key, FEntry& Snapshot);
    void FinishRead(const FString& Key, const FString& Generation, bool bValid);
    bool IsPinned(const FString& Generation) const;
    void Unpin(const FString& Directory);
    void Reclaim(const FString& Generation);
    void AfterGarbagePurge();
    void LoadIndex();
    bool FlushLocked();
    void Remove(const FString& Key);
    void Trim();
};
