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

    /** First-person player body (WP-P2): gender ("male"/"female"; empty = exporter default, male). */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") FString PlayerGender;
    /** Player loadout XML in Data.p4k (e.g. Data/Scripts/Loadouts/Player/DefaultLoadouts/<name>.xml); empty = the
     *  exporter's default (first default loadout with a stowed personal weapon). */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") FString PlayerLoadout;

    /** Viewer dirt on exterior surfaces (WP-U2b, FSCDirt): 0 = clean (the game's state for an unflown ship), 1 = full. */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") float DirtAmount = 0.3f;
    /** Linear dirt colour; the default is FSCDirt's "Dust" preset (a viewer choice, not game data). */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") FLinearColor DirtColor = FLinearColor::FromSRGBColor(FColor(118, 108, 92));

    UFUNCTION(BlueprintPure, Category = "SC|Settings") static USCViewerSettings* Get();
    /** -SCPlayerGender=<g> / -SCPlayerLoadout=<p4k path> override the settings for one run. */
    FString ResolvePlayerGender() const;
    FString ResolvePlayerLoadout() const;
    void AddRecent(const FString& Entity);
    /** Absolute cache dir resolved from the launch directory (UE changes cwd at startup); empty when disabled.
     *  -SCCacheDir=<dir> overrides the setting for one run. */
    FString ResolveCacheDir() const;
    /** -SCArchive=<path> overrides the setting for one run. */
    FString ResolveArchivePath() const;
    UFUNCTION(BlueprintCallable, Category = "SC|Settings") void Save();
};
