// WP-O1-4c-3b: the importer's implementation of ISCImporting (SCImport.h).
#include "SCImport.h"
#include "SCImportJob.h"
#include "SCImporter.h"
#include "SCLightImporter.h"
#include "SCProcessedCache.h"
#include "SCGeometryStreaming.h"

/** The importer behind ISCImporting. */
class FSCImporting final : public ISCImporting
{
public:
    virtual TSharedPtr<ISCImportJob> NewJob(const FSCPackageSource& Source, FSCImportOptions Options, TSharedPtr<const FSCPackageSource> SourceOwner) override
    {
        return MakeShared<FSCImportJob>(Source, MoveTemp(Options), MoveTemp(SourceOwner));
    }
    virtual TSharedPtr<ISCImportJob> NewJob(TSharedRef<FSCImportedAssets> Assets, FSCImportOptions Options) override
    {
        return MakeShared<FSCImportJob>(MoveTemp(Assets), MoveTemp(Options));
    }
    virtual int32 CountRetainedObjects(const FSCImportedAssets& Assets) const override { return FSCImportJob::CountRetainedObjects(Assets); }
    virtual TSharedPtr<ISCProcessedCache, ESPMode::ThreadSafe> NewProcessedCache(const FString& Root, int64 CapBytes) override
    {
        return MakeShared<FSCProcessedCache, ESPMode::ThreadSafe>(Root, CapBytes);
    }
    virtual TSharedPtr<ISCGeometryStreaming> NewGeometryStreaming() override { return MakeShared<FSCGeometryStreaming>(); }
    virtual void Unload(UWorld* World) override { FSCImporter::Unload(World); }
    virtual UTexture2D* LoadGobo(const FSCPackageSource& Source, ASCVehicle* Vehicle, const FString& Path) override
    {
        return FSCLightImporter::LoadGobo(Source, Vehicle, Path);
    }
};
ISCImporting& SCImportingInstance()
{
    static FSCImporting Instance;
    return Instance;
}
const ISCImporting* FSCProcessedCache::GetImporter() const { return &SCImportingInstance(); }
const ISCImporting* FSCGeometryStreaming::GetImporter() const { return &SCImportingInstance(); }
