// Integration-test packages: exported from the game archive through the archive library as the viewer exports them
// (its package cache, ResolveCacheDir, serves a warm one), so a test needs no package path on the command line.
#pragma once
#include "CoreMinimal.h"
#include "SCBridgeClient.h"
#include "SCPackageSource.h"
#include "SCViewerSettings.h"

namespace SCTestPackages
{
/** Opens the archive (the viewer's archive and cache settings) and runs Call; the package it exports, or null with
 *  Error (no archive, a failed export). Blocks the calling thread until done. */
inline TSharedPtr<FSCTarPackageSource> Export(TFunctionRef<UE::Tasks::TTask<FSCExportResult>(FSCBridgeClient&)> Call, FString& Error)
{
    const TSharedRef<FSCBridgeClient> Client = MakeShared<FSCBridgeClient>(); // its tasks keep it alive (AsShared)
    const USCViewerSettings* Settings = GetDefault<USCViewerSettings>();
    UE::Tasks::TTask<FSCOpenResult> Open = Client->Open(Settings->ResolveArchivePath(), Settings->ResolveCacheDir());
    Open.Wait();
    if (!Open.GetResult().bOK)
    {
        Error = TEXT("archive: ") + Open.GetResult().Error.ToString();
        return nullptr;
    }
    UE::Tasks::TTask<FSCExportResult> Task = Call(*Client);
    Task.Wait();
    FSCExportResult& Result = Task.GetResult();
    if (!Result.bOK)
    {
        Error = TEXT("export: ") + Result.Error.ToString();
        return nullptr;
    }
    return MakeShared<FSCTarPackageSource>(MoveTemp(Result.Tar));
}

/** A vehicle (DataCore entity class name) package. */
inline TSharedPtr<FSCTarPackageSource> Vehicle(const FString& Entity, FString& Error)
{
    return Export([&](FSCBridgeClient& Client) {
        return Client.ExportDecomposed(Entity, FSCExportOptions(), MakeShared<FSCExportProgress, ESPMode::ThreadSafe>());
    }, Error);
}

/** The default male player kit (its default loadout, every loadout weapon). */
inline TSharedPtr<FSCTarPackageSource> PlayerKit(FString& Error)
{
    return Export([&](FSCBridgeClient& Client) {
        return Client.ExportPlayer(FString(), FString(), {}, false, MakeShared<FSCExportProgress, ESPMode::ThreadSafe>());
    }, Error);
}
} // namespace SCTestPackages
