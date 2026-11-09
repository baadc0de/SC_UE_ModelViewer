// WP-U0 only: synchronous, single-package sink for SavePackage's cooked bytes.
// No geometry is encoded here. UE owns the .uasset/.uexp/.ubulk serialization.
#pragma once
#if WITH_EDITOR
#include "Serialization/BasePackageWriter.h"
#include "Serialization/LargeMemoryWriter.h"
#include "UObject/PackageFileSummary.h"
#include "AssetCompilingManager.h"
#include "DistanceFieldAtlas.h"
#include "MeshCardBuild.h"

class FSCSpikePackageWriter final : public FBaseCookedPackageWriter
{
public:
    bool bWritten = false;

    virtual EPackageWriterResult BeginCacheForCookedPlatformData(FBeginCacheForCookedPlatformDataInfo& Info) override
    {
        for (UObject* Object : Info.SaveableObjects) Object->BeginCacheForCookedPlatformData(Info.TargetPlatform);
        FAssetCompilingManager::Get().FinishAllCompilation();
        for (UObject* Object : Info.SaveableObjects)
        {
            if (UStaticMesh* Mesh = Cast<UStaticMesh>(Object))
            {
                if (GDistanceFieldAsyncQueue) GDistanceFieldAsyncQueue->BlockUntilBuildComplete(Mesh, false);
                if (GCardRepresentationAsyncQueue) GCardRepresentationAsyncQueue->BlockUntilBuildComplete(Mesh, false);
            }
            const double Deadline = FPlatformTime::Seconds() + 10.0;
            while (!Object->IsCachedCookedPlatformDataLoaded(Info.TargetPlatform))
            {
                if (FPlatformTime::Seconds() > Deadline)
                {
                    UE_LOG(LogTemp, Error, TEXT("WP-U0 cooked data not ready: %s"), *Object->GetPathName());
                    return EPackageWriterResult::Error;
                }
                FPlatformProcess::Sleep(0.01f);
            }
        }
        return EPackageWriterResult::Success;
    }

    // This spike writes one package synchronously. It has no cooker server,
    // incremental cook log, DLC, multiprocess protocol, or hash database.
    virtual void SetCooker(UE::PackageWriter::Private::ICookerInterface*) override { checkNoEntry(); }
    virtual void Initialize(const FCookInfo&) override {}
    virtual void BeginCook(const FCookInfo&) override {}
    virtual void EndCook(const FCookInfo&) override {}
    virtual void PopulateOplog(const FAssetRegistryState&, int32& Count) override { Count = 0; }
    virtual void UpdateLastReferenceDateAndPruneStaleOps(UE::Cook::Artifact::FUpdateOplogPackagesContext&) override { checkNoEntry(); }
    virtual TArray<FName> GetOplogPackageNames() override { return {}; }
    virtual FCbObject GetOplogAttachment(FName, FUtf8StringView) override { return {}; }
    virtual void GetOplogAttachments(TArrayView<FName>, TArrayView<FUtf8StringView>, TUniqueFunction<void(FName, FUtf8StringView, FCbObject&&)>&&) override { checkNoEntry(); }
    virtual void GetBaseGameOplogAttachments(TArrayView<FName>, TArrayView<FUtf8StringView>, TUniqueFunction<void(FName, FUtf8StringView, FCbObject&&)>&&) override { checkNoEntry(); }
    virtual ECommitStatus GetCommitStatus(FName) override { return bWritten ? ECommitStatus::Success : ECommitStatus::NotCommitted; }
    virtual void RemoveCookedPackages(TArrayView<const FName>) override { checkNoEntry(); }
    virtual void RemoveCookedPackages() override { checkNoEntry(); }
    virtual TFuture<FCbObject> WriteMPCookMessageForPackage(FName) override { checkNoEntry(); return {}; }
    virtual bool TryReadMPCookMessageForPackage(FName, FCbObjectView) override { checkNoEntry(); return false; }
    virtual TMap<FName, TRefCountPtr<FPackageHashes>>& GetPackageHashes() override { return Hashes; }

    virtual void BeginPackage(const FBeginPackageInfo& Info) override { Filename = Info.LooseFilePath; }
    virtual void WritePackageData(const FPackageInfo& Info, FLargeMemoryWriter& Archive, const TArray<FFileRegion>&) override
    {
        check(Info.MultiOutputIndex == 0);
        HeaderSize = Info.HeaderSize;
        Package.Append(Archive.GetData(), Archive.TotalSize());
    }
    virtual void WriteBulkData(const FBulkDataInfo& Info, const FIoBuffer& Data, const TArray<FFileRegion>&) override
    {
        check(Info.MultiOutputIndex == 0);
        if (Info.BulkDataType == FBulkDataInfo::AppendToExports) Append(InlineBulk, Data);
        else Append(Files.FindOrAdd(Info.LooseFilePath), Data);
    }
    virtual void WriteAdditionalFile(const FAdditionalFileInfo& Info, const FIoBuffer& Data) override
    {
        check(Info.MultiOutputIndex == 0);
        Append(Files.FindOrAdd(Info.Filename), Data);
    }
    virtual void WriteLinkerAdditionalData(const FLinkerAdditionalDataInfo& Info, const FIoBuffer& Data, const TArray<FFileRegion>&) override
    {
        check(Info.MultiOutputIndex == 0);
        Append(Additional, Data);
    }
    virtual void WritePackageTrailer(const FPackageTrailerInfo& Info, const FIoBuffer& Data) override
    {
        check(Info.MultiOutputIndex == 0);
        Append(Trailer, Data);
    }
    virtual void CommitPackage(FCommitPackageInfo&& Info) override
    {
        if (Info.Status != ECommitStatus::Success) return;
        check(HeaderSize > 0 && HeaderSize <= uint64(Package.Num()));
        TArray64<uint8> Exports;
        Exports.Append(Package.GetData() + HeaderSize, Package.Num() - HeaderSize);
        Exports.Append(InlineBulk);
        Exports.Append(Additional);
        const uint32 Tag = PACKAGE_FILE_TAG;
        Exports.Append(reinterpret_cast<const uint8*>(&Tag), sizeof(Tag));
        Exports.Append(Trailer);
        bWritten = FFileHelper::SaveArrayToFile(TArrayView64<const uint8>(Package.GetData(), HeaderSize), *Filename);
        bWritten &= FFileHelper::SaveArrayToFile(Exports, *FPaths::ChangeExtension(Filename, TEXT("uexp")));
        for (const auto& File : Files) bWritten &= FFileHelper::SaveArrayToFile(File.Value, *File.Key);
    }

private:
    static void Append(TArray64<uint8>& Dest, const FIoBuffer& Data) { Dest.Append(Data.Data(), Data.DataSize()); }
    FString Filename;
    uint64 HeaderSize = 0;
    TArray64<uint8> Package, InlineBulk, Additional, Trailer;
    TMap<FString, TArray64<uint8>> Files;
    TMap<FName, TRefCountPtr<FPackageHashes>> Hashes;
};
#endif
