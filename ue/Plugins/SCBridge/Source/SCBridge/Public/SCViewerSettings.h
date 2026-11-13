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
    /** Look (2026-11-12, GI on): glowing surfaces Lumen cannot see (glow cards, split meshes) still light the scene
     *  through hidden opaque copies (sc.Emissive.Lumen). Not the game's GI-off renderer. */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") bool bEmissiveInLumen = true;
    /** The environment the ship stands in: an archive object container (a game hangar, exported and cached like
     *  ships), or "none" for the map's procedural hangar. A viewer choice (LOOK-3); -SCDefaults uses "none" unless
     *  -SCEnvironment is given. */
    /** Size the chosen hangar to each ship: the smallest cooked hangar of the chosen style whose ship elevator and clear
     *  height hold it (measured at cook time, USCHangarIndex); false keeps exactly Environment. */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") bool bFitHangarToShip = true;
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") FString Environment =
        TEXT("Data/ObjectContainers/PU/loc/mod/common/hangar/hightech_a/hangar_lrgtop_001.socpak");
    /** Look (2026-11-12): every ship light casts shadows (MegaLights ray-traced), not only the authored casters. */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") bool bShadowAllLights = true;
    /** WP-L1 room confinement (area-only lights reach only their room's lighting channel). Off in the look (D15):
     *  every light is shadowed, so walls contain the light; the path tracer ignores channels. */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") bool bConfineLightsToRooms = false;
    // Look (LOOK-6, adjudicated): FFT bloom with a 4-point diffraction kernel at this intensity; 0 = the standard bloom.
    // 0.05 gives small fixtures a restrained glint; 0.15 suits stills. Console: sc.Look.BloomStar.
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") float StarBloom = 0.05f;
    // Look (LOOK-1) vs frame time: Lumen short-range AO is traced with hardware ray tracing while the world's
    // ray-tracing segments stay within this budget (SCLook::UpdateShortRangeAO; a performance budget, not game data);
    // beyond it Lumen's screen-space short-range AO. -1 = always traced, 0 = never. Console: sc.Look.ShortRangeAO.
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") int32 TracedShortRangeAOMaxSegments = 20000;
    /** How much more dirt faces pointing down get than faces pointing up (0 = even, 1 = all below); a viewer
     *  choice for a landed ship (the game's direction-dependent dirt is run-time state). */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Settings") float DirtUnderside = 0.5f;
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
