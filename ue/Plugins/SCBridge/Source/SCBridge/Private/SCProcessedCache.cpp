#include "SCProcessedCache.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Hash/Blake3.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectGlobals.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#else
#include <cstdio>
#endif

namespace
{
constexpr int32 ManifestVersion = 1;
constexpr const TCHAR* ManifestName = TEXT("manifest.json");

bool IsHex(const FString& Value, int32 Length)
{
    if (Value.Len() != Length) return false;
    for (TCHAR C : Value)
        if (!FChar::IsHexDigit(C)) return false;
    return true;
}

bool IsPayloadName(const FString& Name)
{
    if (Name.IsEmpty() || Name.StartsWith(TEXT("."))) return false;
    for (TCHAR C : Name)
        if (!FChar::IsAlnum(C) && C != TEXT('.') && C != TEXT('_') && C != TEXT('-')) return false;
    return true;
}

bool ReplaceManifest(const FString& Destination, const FString& Source)
{
#if PLATFORM_WINDOWS
    return MoveFileExW(*Source, *Destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(TCHAR_TO_UTF8(*Source), TCHAR_TO_UTF8(*Destination)) == 0;
#endif
}
}

FSCProcessedCache::FSCProcessedCache(FString InRoot, int64 InCapBytes)
    : CapBytes(FMath::Max<int64>(0, InCapBytes))
{
    if (InRoot.IsEmpty()) return;
    Root = FPaths::ConvertRelativePathToFull(InRoot);
    FPaths::NormalizeDirectoryName(Root);
    if (!FPaths::CollapseRelativeDirectories(Root)) return;
    // A package can have open streaming readers even after LoadObject returned.
    // Take exclusive ownership of this root for the process, and continue with
    // ordinary imports if another process is already using it.
    FString LockPath = Root;
#if PLATFORM_WINDOWS
    LockPath.ToLowerInline();
#endif
    const FString RootKey = Key(TEXT("root"), LockPath);
    Owner = MakeUnique<FSystemWideCriticalSection>(TEXT("SCProcessedCache-") + RootKey);
    if (!Owner->IsValid() || !IFileManager::Get().MakeDirectory(*Root, true)) return;
    bEnabled = true;
    Mount = TEXT("/SCProcessedCache_") + RootKey.Left(16) + TEXT("/");
    FPackageName::RegisterMountPoint(Mount, Root / TEXT(""));
    LoadIndex();
    Trim();
    PostPurge = FCoreUObjectDelegates::GetPostPurgeGarbageDelegate().AddRaw(this, &FSCProcessedCache::AfterGarbagePurge);
}

FSCProcessedCache::~FSCProcessedCache()
{
    if (bEnabled)
    {
        FCoreUObjectDelegates::GetPostPurgeGarbageDelegate().Remove(PostPurge);
        Flush();
        FPackageName::UnRegisterMountPoint(Mount, Root / TEXT(""));
    }
}

FString FSCProcessedCache::PackageName(const FString& Directory, const FString& Asset) const
{
    return Mount + FPaths::GetCleanFilename(Directory) + TEXT("/") + Asset;
}

FString FSCProcessedCache::Key(const FString& Namespace, const FString& Input)
{
    FTCHARToUTF8 Text(*FString::Printf(TEXT("%d:%s%s"), Namespace.Len(), *Namespace, *Input));
    return LexToString(FBlake3::HashBuffer(Text.Get(), Text.Length()));
}

FString FSCProcessedCache::Digest(TConstArrayView64<uint8> Data)
{
    return LexToString(FBlake3::HashBuffer(Data.GetData(), Data.Num()));
}

bool FSCProcessedCache::FileDigest(const FString& Path, int64 ExpectedSize, FString& Out)
{
    IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
    TUniquePtr<IFileHandle> Handle(Platform.OpenRead(*Path));
    if (!Handle || Handle->Size() != ExpectedSize) return false;
    FBlake3 Hash;
    TArray<uint8> Buffer;
    Buffer.SetNumUninitialized(int32(FMath::Min<int64>(ExpectedSize, 128 * 1024)));
    for (int64 Remaining = ExpectedSize; Remaining > 0;)
    {
        const int64 Count = FMath::Min<int64>(Remaining, Buffer.Num());
        if (!Handle->Read(Buffer.GetData(), Count)) return false;
        Hash.Update(Buffer.GetData(), Count);
        Remaining -= Count;
    }
    Out = LexToString(Hash.Finalize());
    return true;
}

void FSCProcessedCache::LoadIndex()
{
    FString Data;
    TSharedPtr<FJsonObject> Index;
    int32 Version = 0;
    const TSharedPtr<FJsonObject>* Stored = nullptr;
    TSet<FString> Referenced;
    if (FFileHelper::LoadFileToString(Data, *(Root / ManifestName)) &&
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Data), Index) && Index &&
        Index->TryGetNumberField(TEXT("version"), Version) && Version == ManifestVersion &&
        Index->TryGetObjectField(TEXT("entries"), Stored))
    {
        for (const auto& Pair : (*Stored)->Values)
        {
            const FString Key(Pair.Key);
            const TSharedPtr<FJsonObject>* Object;
            const TSharedPtr<FJsonObject>* Files;
            FEntry Entry;
            if (!IsHex(Key, 64) || !Pair.Value->TryGetObject(Object) || !*Object ||
                !(*Object)->TryGetStringField(TEXT("generation"), Entry.Generation) || !IsHex(Entry.Generation, 32) ||
                Referenced.Contains(Entry.Generation) || !(*Object)->TryGetNumberField(TEXT("used"), Entry.LastUse) ||
                Entry.LastUse < 0 || Entry.LastUse >= MAX_int32 || !(*Object)->TryGetObjectField(TEXT("files"), Files))
                continue;
            for (const auto& Saved : (*Files)->Values)
            {
                const FString Name(Saved.Key);
                const TSharedPtr<FJsonObject>* File;
                FFile Info;
                if (!IsPayloadName(Name) || !Saved.Value->TryGetObject(File) || !*File ||
                    !(*File)->TryGetNumberField(TEXT("bytes"), Info.Bytes) || Info.Bytes < 0 ||
                    Info.Bytes > MAX_int64 - Entry.Bytes ||
                    !(*File)->TryGetStringField(TEXT("hash"), Info.Hash) || !IsHex(Info.Hash, 64) ||
                    IFileManager::Get().FileSize(*(Root / Entry.Generation / Name)) != Info.Bytes)
                    break;
                Entry.Bytes += Info.Bytes;
                Entry.Files.Add(Name, MoveTemp(Info));
            }
            if (Entry.Files.IsEmpty() || Entry.Files.Num() != (*Files)->Values.Num() ||
                Entry.Bytes > MAX_int64 - TotalBytes)
                continue;
            Clock = FMath::Max(Clock, Entry.LastUse);
            TotalBytes += Entry.Bytes;
            Referenced.Add(Entry.Generation);
            Entries.Add(Key, MoveTemp(Entry));
        }
        bDirty = Entries.Num() != (*Stored)->Values.Num();
    }
    TArray<FString> Directories;
    IFileManager::Get().FindFiles(Directories, *(Root / TEXT("*")), false, true);
    for (const FString& Dir : Directories)
        if (IsHex(Dir, 32) && !Referenced.Contains(Dir))
            IFileManager::Get().DeleteDirectory(*(Root / Dir), false, true);
}

bool FSCProcessedCache::FlushLocked()
{
    if (!bDirty) return true;
    const TSharedRef<FJsonObject> Index = MakeShared<FJsonObject>();
    const TSharedRef<FJsonObject> Stored = MakeShared<FJsonObject>();
    for (const auto& Pair : Entries)
    {
        const FEntry& Entry = Pair.Value;
        const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        const TSharedRef<FJsonObject> Files = MakeShared<FJsonObject>();
        for (const auto& Saved : Entry.Files)
        {
            const TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
            Info->SetNumberField(TEXT("bytes"), Saved.Value.Bytes);
            Info->SetStringField(TEXT("hash"), Saved.Value.Hash);
            Files->SetObjectField(Saved.Key, Info);
        }
        Object->SetStringField(TEXT("generation"), Entry.Generation);
        Object->SetNumberField(TEXT("used"), Entry.LastUse);
        Object->SetObjectField(TEXT("files"), Files);
        Stored->SetObjectField(Pair.Key, Object);
    }
    Index->SetNumberField(TEXT("version"), ManifestVersion);
    Index->SetObjectField(TEXT("entries"), Stored);
    FString Contents;
    const FString Temp = Root / TEXT("manifest.tmp");
    if (!FJsonSerializer::Serialize(Index, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Contents)) ||
        !FFileHelper::SaveStringToFile(Contents, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
        !ReplaceManifest(Root / ManifestName, Temp))
        return false;
    bDirty = false;
    return true;
}

void FSCProcessedCache::Remove(const FString& Key)
{
    FEntry Entry;
    if (!Entries.RemoveAndCopyValue(Key, Entry)) return;
    Retired.Add(Entry.Generation, Entry.Bytes);
    Reclaim(Entry.Generation);
    bDirty = true;
}

bool FSCProcessedCache::IsPinned(const FString& Generation) const
{
    return Pins.Contains(Generation) || Resident.Contains(Generation);
}

void FSCProcessedCache::Reclaim(const FString& Generation)
{
    const int64* Bytes = Retired.Find(Generation);
    if (!Bytes || IsPinned(Generation)) return;
    if (!IFileManager::Get().DeleteDirectory(*(Root / Generation), false, true)) return;
    TotalBytes -= *Bytes;
    Retired.Remove(Generation);
}

void FSCProcessedCache::AfterGarbagePurge()
{
    FScopeLock Guard(&Mutex);
    for (auto It = Resident.CreateIterator(); It; ++It)
        if (!It.Value().Asset.IsValid()) It.RemoveCurrent();
    TArray<FString> Generations;
    Retired.GetKeys(Generations);
    for (const FString& Generation : Generations) Reclaim(Generation);
    Trim();
}

void FSCProcessedCache::Trim()
{
    while (TotalBytes > CapBytes)
    {
        const TPair<FString, FEntry>* Oldest = nullptr;
        for (const auto& Pair : Entries)
            if (!IsPinned(Pair.Value.Generation) && (!Oldest || Pair.Value.LastUse < Oldest->Value.LastUse))
                Oldest = &Pair;
        if (!Oldest) break;
        const FString Key = Oldest->Key;
        Remove(Key);
    }
}

bool FSCProcessedCache::ReadBlob(const FString& Key, TArray<uint8>& Out)
{
    Out.Reset();
    FEntry Entry;
    auto Pin = PinForRead(Key, Entry);
    if (!Pin || Entry.Files.Num() != 1) return false;
    const FFile* File = Entry.Files.Find(PayloadName);
    if (!File) return false;
    const FString Path = Pin->Directory / PayloadName;
    const int64 Size = IFileManager::Get().FileSize(*Path);
    if (Size < 0 || Size != File->Bytes || Size > MAX_int32 - 2 ||
        !FFileHelper::LoadFileToArray(Out, *Path) || Out.Num() != Size ||
        Digest(TConstArrayView64<uint8>(Out.GetData(), Out.Num())) != File->Hash)
    {
        Out.Reset();
        FinishRead(Key, Entry, false);
        return false;
    }
    FinishRead(Key, Entry, true);
    return true;
}

bool FSCProcessedCache::WriteBlob(const FString& Key, TConstArrayView<uint8> Data)
{
    if (Data.Num() > GetCapBytes()) return false;
    const FString Directory = BeginWrite();
    if (Directory.IsEmpty()) return false;
    if (!FFileHelper::SaveArrayToFile(TConstArrayView64<uint8>(Data.GetData(), Data.Num()), *(Directory / PayloadName)))
    {
        AbortWrite(Directory);
        return false;
    }
    return CommitWrite(Key, Directory);
}

bool FSCProcessedCache::Find(const FString& Key, FString& Directory)
{
    const auto Pin = PinEntry(Key);
    Directory = Pin ? Pin->Directory : FString();
    return Pin.IsValid();
}

FSCProcessedCache::FReadPin::FReadPin(FSCProcessedCache& InCache, FString InDirectory)
    : Directory(MoveTemp(InDirectory)), Cache(InCache)
{}

FSCProcessedCache::FReadPin::~FReadPin()
{
    Cache.Unpin(Directory);
}

TUniquePtr<FSCProcessedCache::FReadPin> FSCProcessedCache::PinEntry(const FString& Key)
{
    FEntry Entry;
    auto Pin = PinForRead(Key, Entry);
    if (!Pin) return nullptr;
    for (const auto& Pair : Entry.Files)
    {
        FString Digest;
        if (!FileDigest(Pin->Directory / Pair.Key, Pair.Value.Bytes, Digest) || Digest != Pair.Value.Hash)
        {
            FinishRead(Key, Entry, false);
            return nullptr;
        }
    }
    FinishRead(Key, Entry, true);
    return Pin;
}

TUniquePtr<FSCProcessedCache::FReadPin> FSCProcessedCache::PinForRead(const FString& Key, FEntry& Snapshot)
{
    FScopeLock Guard(&Mutex);
    const FEntry* Entry = Entries.Find(Key);
    if (!Entry) return nullptr;
    Snapshot = *Entry;
    ++Pins.FindOrAdd(Entry->Generation);
    return TUniquePtr<FReadPin>(new FReadPin(*this, Root / Entry->Generation));
}

void FSCProcessedCache::FinishRead(const FString& Key, const FEntry& Snapshot, bool bValid)
{
    FScopeLock Guard(&Mutex);
    FEntry* Entry = Entries.Find(Key);
    if (!Entry || Entry->Generation != Snapshot.Generation) return;
    if (!bValid)
        Remove(Key);
    else
    {
        Entry->LastUse = ++Clock;
        bDirty = true;
    }
}

void FSCProcessedCache::Unpin(const FString& Directory)
{
    FScopeLock Guard(&Mutex);
    const FString Generation = FPaths::GetCleanFilename(Directory);
    int32* Count = Pins.Find(Generation);
    if (Count && --*Count == 0)
    {
        Pins.Remove(Generation);
        Reclaim(Generation);
    }
}

void FSCProcessedCache::KeepLoaded(const FString& Directory, UObject* Asset, bool bUsable)
{
    check(IsInGameThread());
    FScopeLock Guard(&Mutex);
    Resident.Add(FPaths::GetCleanFilename(Directory), {Asset, bUsable});
}

UObject* FSCProcessedCache::GetLoaded(const FString& Directory) const
{
    check(IsInGameThread());
    FScopeLock Guard(&Mutex);
    const FResident* Object = Resident.Find(FPaths::GetCleanFilename(Directory));
    return Object && Object->bUsable ? Object->Asset.Get() : nullptr;
}

FString FSCProcessedCache::BeginWrite()
{
    if (!bEnabled) return {};
    FScopeLock Guard(&Mutex);
    if (CapBytes == 0) return {};
    const FString Path = Root / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    if (!IFileManager::Get().MakeDirectory(*Path, false)) return {};
    Staging.Add(Path);
    return Path;
}

bool FSCProcessedCache::CommitWrite(const FString& Key, const FString& Directory)
{
    int64 Budget = 0;
    {
        FScopeLock Guard(&Mutex);
        if (!Staging.Remove(Directory)) return false;
        Budget = CapBytes;
    }
    FEntry Entry;
    Entry.Generation = FPaths::GetCleanFilename(Directory);
    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *(Directory / TEXT("*")), true, false);
    if (IsHex(Key, 64))
        for (const FString& Name : Files)
        {
            FFile File;
            const FString Path = Directory / Name;
            File.Bytes = IFileManager::Get().FileSize(*Path);
            if (!IsPayloadName(Name) || File.Bytes < 0 || File.Bytes > Budget - Entry.Bytes ||
                !FileDigest(Path, File.Bytes, File.Hash)) break;
            Entry.Bytes += File.Bytes;
            Entry.Files.Add(Name, MoveTemp(File));
        }
    FScopeLock Guard(&Mutex);
    if (Entry.Files.IsEmpty() || Entry.Files.Num() != Files.Num() || Entry.Bytes > CapBytes)
    {
        IFileManager::Get().DeleteDirectory(*Directory, false, true);
        return false;
    }
    Remove(Key);
    Entry.LastUse = ++Clock;
    TotalBytes += Entry.Bytes;
    Entries.Add(Key, MoveTemp(Entry));
    bDirty = true;
    Trim();
    return Entries.Contains(Key);
}

void FSCProcessedCache::AbortWrite(const FString& Directory)
{
    FScopeLock Guard(&Mutex);
    if (Staging.Remove(Directory)) IFileManager::Get().DeleteDirectory(*Directory, false, true);
}

FSCProcessedCache::FStats FSCProcessedCache::Stats() const
{
    FScopeLock Guard(&Mutex);
    return {TotalBytes, Entries.Num()};
}

int64 FSCProcessedCache::GetCapBytes() const
{
    FScopeLock Guard(&Mutex);
    return CapBytes;
}

void FSCProcessedCache::SetCapBytes(int64 Bytes)
{
    FScopeLock Guard(&Mutex);
    CapBytes = FMath::Max<int64>(0, Bytes);
    if (CapBytes == 0)
    {
        TArray<FString> Keys;
        Entries.GetKeys(Keys);
        for (const FString& Key : Keys) Remove(Key);
    }
    Trim();
    FlushLocked();
}

void FSCProcessedCache::Clear()
{
    FScopeLock Guard(&Mutex);
    TArray<FString> Keys;
    Entries.GetKeys(Keys);
    for (const FString& Key : Keys) Remove(Key);
    FlushLocked();
}

bool FSCProcessedCache::Flush()
{
    FScopeLock Guard(&Mutex);
    return FlushLocked();
}
