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

/** Owns the uncompressed tar bytes (safe after sc_free_blob); indexes regular USTAR/GNU entries. */
class SCBRIDGE_API FSCTarPackageSource final : public FSCPackageSource
{
public:
    explicit FSCTarPackageSource(TArray<uint8>&& InBytes);
    bool IsValid() const { return Error.IsEmpty(); }
    const FString& GetError() const { return Error; }
    bool Exists(const FString& Path) const override;
    bool ReadBytes(const FString& Path, TArray<uint8>& Out, FString& OutError) const override;
    TArray<FString> ListFiles(const FString& Prefix = TEXT("")) const override;
private:
    struct FEntry { int64 Offset; int64 Size; };
    TArray<uint8> Bytes;
    TMap<FString, FEntry> Entries;
    FString Error;
};
