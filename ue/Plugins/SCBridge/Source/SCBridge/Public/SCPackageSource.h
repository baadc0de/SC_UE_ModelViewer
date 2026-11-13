#pragma once
#include "CoreMinimal.h"

/** Export-root-relative, case-preserving read-only package. No importer opens files directly. */
class SCBRIDGE_API FSCPackageSource
{
public:
    virtual ~FSCPackageSource() = default;
    virtual bool Exists(const FString& Path) const = 0;
    virtual bool ReadBytes(const FString& Path, TArray<uint8>& Out, FString& Error) const = 0;
    virtual TArray<FString> ListFiles(const FString& Prefix = TEXT("")) const = 0;
    // Reject absolute paths, traversal, drive prefixes and embedded NULs; accept slash/backslash.
    static bool Normalize(const FString& Path, FString& Out);
};

class SCBRIDGE_API FSCFolderPackageSource final : public FSCPackageSource
{
public:
    explicit FSCFolderPackageSource(const FString& InRoot);
    bool Exists(const FString& Path) const override;
    bool ReadBytes(const FString& Path, TArray<uint8>& Out, FString& Error) const override;
    TArray<FString> ListFiles(const FString& Prefix = TEXT("")) const override;
private:
    FString Root;
    bool Resolve(const FString& Path, FString& Out) const;
};

/** Owns the uncompressed tar bytes (safe after sc_free_blob); indexes USTAR, PAX path overrides and GNU long names. */
class SCBRIDGE_API FSCTarPackageSource final : public FSCPackageSource
{
public:
    explicit FSCTarPackageSource(TArray64<uint8>&& InBytes);
    explicit FSCTarPackageSource(TArray<uint8>&& InBytes) : FSCTarPackageSource(TArray64<uint8>(MoveTemp(InBytes))) {}
    bool IsValid() const { return Error.IsEmpty(); }
    const FString& GetError() const { return Error; }
    bool Exists(const FString& Path) const override;
    bool ReadBytes(const FString& Path, TArray<uint8>& Out, FString& OutError) const override;
    TArray<FString> ListFiles(const FString& Prefix = TEXT("")) const override;
private:
    struct FEntry { int64 Offset; int64 Size; };
    TArray64<uint8> Bytes; // a whole ship at LOD 0 exceeds 2 GiB; each entry stays below it (ReadBytes)
    struct FExactPathKeys : BaseKeyFuncs<TPair<FString, FEntry>, FString, false>
    {
        static const FString& GetSetKey(const TPair<FString, FEntry>& Entry) { return Entry.Key; }
        static bool Matches(const FString& A, const FString& B) { return A.Equals(B, ESearchCase::CaseSensitive); }
        static uint32 GetKeyHash(const FString& Path) { return FCrc::StrCrc32(*Path); }
    };
    TMap<FString, FEntry, FDefaultSetAllocator, FExactPathKeys> Entries;
    FString Error;
};
