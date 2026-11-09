#include "SCPackageSource.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#if PLATFORM_UNIX
#include <limits.h>
#include <stdlib.h>
#endif

bool FSCPackageSource::Normalize(const FString& Path, FString& Out)
{
    Out = Path.Replace(TEXT("\\"), TEXT("/"));
    if (Out.StartsWith(TEXT("/")) || Out.Contains(TEXT(":"))) return false;
    TArray<FString> Parts;
    Out.ParseIntoArray(Parts, TEXT("/"), true);
    TArray<FString> Clean;
    for (const FString& Part : Parts)
    {
        if (Part == TEXT("..")) return false;
        for (TCHAR C : Part) if (C < 32) return false;
        if (Part != TEXT(".")) Clean.Add(Part);
    }
    Out = FString::Join(Clean, TEXT("/"));
    return !Out.IsEmpty();
}

FSCFolderPackageSource::FSCFolderPackageSource(const FString& InRoot)
    : Root(FPaths::ConvertRelativePathToFull(InRoot))
{
    FPaths::NormalizeDirectoryName(Root);
#if PLATFORM_UNIX
    char Resolved[PATH_MAX];
    if (realpath(TCHAR_TO_UTF8(*Root), Resolved)) Root = UTF8_TO_TCHAR(Resolved);
#endif
}
bool FSCFolderPackageSource::Resolve(const FString& Path, FString& Out) const
{
    FString Normal;
    if (!Normalize(Path, Normal)) return false;
    Out = Root / Normal;
#if PLATFORM_UNIX
    char Resolved[PATH_MAX];
    if (!realpath(TCHAR_TO_UTF8(*Out), Resolved)) return false;
    Out = UTF8_TO_TCHAR(Resolved);
    if (!Out.StartsWith(Root + TEXT("/"), ESearchCase::CaseSensitive)) return false;
#endif
    return true;
}
bool FSCFolderPackageSource::Exists(const FString& Path) const
{
    FString Full;
    return Resolve(Path, Full) && IFileManager::Get().FileExists(*Full);
}
bool FSCFolderPackageSource::ReadBytes(const FString& Path, TArray<uint8>& Out, FString& Error) const
{
    FString Full;
    Out.Reset();
    if (Resolve(Path, Full) && FFileHelper::LoadFileToArray(Out, *Full)) return true;
    Error = TEXT("Cannot read package file: ") + Path;
    return false;
}
TArray<FString> FSCFolderPackageSource::ListFiles(const FString& Prefix) const
{
    TArray<FString> Files, Result;
    FString Normal;
    if (!Prefix.IsEmpty() && !Normalize(Prefix, Normal)) return Result;
    IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*"), true, false);
    for (FString File : Files)
    {
        File.RightChopInline(Root.Len() + 1);
        if ((Normal.IsEmpty() || File.StartsWith(Normal, ESearchCase::CaseSensitive)) && Exists(File)) Result.Add(File);
    }
    Result.Sort();
    return Result;
}

namespace
{
FString TarString(const uint8* P, int32 N)
{
    int32 Length = 0;
    while (Length < N && P[Length]) ++Length;
    return FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(P), Length));
}
bool PaxName(const uint8* Data, int32 Size, FString& Name, FString& Error)
{
    int32 Offset = 0;
    while (Offset < Size)
    {
        int32 Space = Offset; uint64 Length = 0;
        while (Space < Size && Data[Space] >= '0' && Data[Space] <= '9')
        {
            Length = Length * 10 + Data[Space++] - '0';
            if (Length > uint64(Size)) { Error = TEXT("Invalid PAX record length"); return false; }
        }
        if (Space == Offset || Space >= Size || Data[Space] != ' ' || Length <= uint64(Space - Offset + 2) || Length > uint64(Size - Offset) || Data[Offset + Length - 1] != '\n')
        { Error = TEXT("Malformed PAX path record"); return false; }
        const FString Record = TarString(Data + Space + 1, int32(Length) - (Space - Offset) - 2);
        if (!Record.StartsWith(TEXT("path="), ESearchCase::CaseSensitive)) { Error = TEXT("Only PAX path overrides are supported"); return false; }
        if (!Name.IsEmpty()) { Error = TEXT("Duplicate PAX path"); return false; }
        Name = Record.Mid(5);
        if (Name.IsEmpty()) { Error = TEXT("Empty PAX path"); return false; }
        Offset += int32(Length);
    }
    return true;
}
bool Octal(const uint8* P, int32 N, uint64& Value)
{
    Value = 0;
    for (int32 I = 0; I < N; ++I)
    {
        if (P[I] == 0 || P[I] == ' ') continue;
        if (P[I] < '0' || P[I] > '7' || Value > (MAX_uint64 >> 3)) return false;
        Value = (Value << 3) + P[I] - '0';
    }
    return true;
}
}
FSCTarPackageSource::FSCTarPackageSource(TArray<uint8>&& InBytes) : Bytes(MoveTemp(InBytes))
{
    FString LongName;
    int64 Offset = 0;
    for (; Offset + 512 <= Bytes.Num();)
    {
        const uint8* H = Bytes.GetData() + Offset;
        bool bZero = true;
        for (int32 I = 0; I < 512; ++I) bZero &= H[I] == 0;
        if (bZero)
        {
            for (int64 I = Offset; I < Bytes.Num(); ++I)
                if (Bytes[I]) { Error = TEXT("Tar contains data after terminator"); return; }
            if (!LongName.IsEmpty()) Error = TEXT("Dangling tar long name");
            return;
        }
        uint64 Size = 0, Sum = 0, Expected = 0;
        for (int32 I = 0; I < 512; ++I) Sum += I >= 148 && I < 156 ? 32 : H[I];
        if (!Octal(H + 148, 8, Expected) || Sum != Expected || !Octal(H + 124, 12, Size))
        { Error = TEXT("Invalid/truncated tar header or checksum"); return; }
        if (Size > uint64(MAX_int32)) { Error = TEXT("Tar entry exceeds the 2 GiB byte-array limit"); return; }
        if (Size > uint64(Bytes.Num() - Offset - 512)) { Error = TEXT("Truncated tar payload"); return; }
        const char Type = H[156];
        const bool bOverride = !LongName.IsEmpty();
        FString Name = LongName.IsEmpty() ? TarString(H, 100) : LongName;
        LongName.Reset();
        if (Type == 'x')
        {
            if (bOverride || !PaxName(H + 512, int32(Size), LongName, Error)) { if (Error.IsEmpty()) Error = TEXT("Stacked tar path overrides"); return; }
        }
        else if (Type == 'L')
        {
            if (bOverride) { Error = TEXT("Stacked tar path overrides"); return; }
            LongName = TarString(H + 512, int32(Size));
        }
        else
        {
            // USTAR prefix; GNU headers use the same bytes for other fields.
            if (!bOverride && TarString(H + 257, 6) == TEXT("ustar") && H[262] == 0)
            {
                const FString Prefix = TarString(H + 345, 155);
                if (!Prefix.IsEmpty()) Name = Prefix / Name;
            }
            FString Normal;
            if (!Normalize(Name, Normal) && !(Type == '5' && (Name == TEXT("./") || Name == TEXT("."))))
            { Error = TEXT("Unsafe tar path: ") + Name; return; }
            if (Type == '0' || Type == 0)
            {
                if (Entries.Contains(Normal)) { Error = TEXT("Duplicate tar file: ") + Normal; return; }
                Entries.Add(Normal, {Offset + 512, int64(Size)});
            }
            else if (Type != '5') { Error = TEXT("Unsupported tar entry (links are not package files): ") + Name; return; }
        }
        Offset += 512 + ((int64(Size) + 511) / 512) * 512;
    }
    Error = TEXT("Tar missing terminator or truncated padding");
}
bool FSCTarPackageSource::Exists(const FString& Path) const
{
    FString Normal;
    return IsValid() && Normalize(Path, Normal) && Entries.Contains(Normal);
}
bool FSCTarPackageSource::ReadBytes(const FString& Path, TArray<uint8>& Out, FString& OutError) const
{
    Out.Reset();
    FString Normal;
    if (IsValid() && Normalize(Path, Normal))
        if (const FEntry* Entry = Entries.Find(Normal))
        { Out.Append(Bytes.GetData() + Entry->Offset, int32(Entry->Size)); return true; }
    OutError = IsValid() ? TEXT("Missing tar file: ") + Path : Error;
    return false;
}
TArray<FString> FSCTarPackageSource::ListFiles(const FString& Prefix) const
{
    TArray<FString> Result;
    FString Normal;
    if (!IsValid() || (!Prefix.IsEmpty() && !Normalize(Prefix, Normal))) return Result;
    for (const auto& Entry : Entries)
        if (Normal.IsEmpty() || Entry.Key.StartsWith(Normal, ESearchCase::CaseSensitive)) Result.Add(Entry.Key);
    Result.Sort();
    return Result;
}
