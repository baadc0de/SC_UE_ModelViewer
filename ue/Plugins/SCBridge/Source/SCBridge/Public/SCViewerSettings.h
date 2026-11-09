// Persistent viewer settings (archive path, cache dir, recents, camera speed), saved per user in
// Saved/Config/<Platform>/GameUserSettings.ini (Game.ini only saves whitelisted sections).
#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SCViewerSettings.generated.h"

UCLASS(BlueprintType, Config = GameUserSettings)
class SCBRIDGE_API USCViewerSettings : public UObject
{
    GENERATED_BODY()
public:
    /** Data.p4k chosen by the user; empty = auto-discover (sc_open(NULL)). */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") FString ArchivePath;
    /** Cache directory; empty = <launch dir>/cache, relative paths resolve against the launch dir, "none" disables. */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") FString CacheDir;
    /** Most recent first, exact entity class names. */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") TArray<FString> RecentEntities;
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") int32 MaxRecents = 12;
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") int32 SearchLimit = 500;
    /** Free-fly speed in cm/s (mouse wheel changes it while flying). */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") float FlySpeed = 1500.f;

    UFUNCTION(BlueprintPure, Category = "SC|Settings") static USCViewerSettings* Get();
    void AddRecent(const FString& Entity);
    /** Absolute cache dir resolved from the launch directory (UE changes cwd at startup); empty when disabled.
     *  -SCCacheDir=<dir> overrides the setting for one run. */
    FString ResolveCacheDir() const;
    /** -SCArchive=<path> overrides the setting for one run. */
    FString ResolveArchivePath() const;
    UFUNCTION(BlueprintCallable, Category = "SC|Settings") void Save();
};
