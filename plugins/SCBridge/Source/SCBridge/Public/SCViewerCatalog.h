// WP-O1-3: the pick lists of the viewer (SSCViewerPicker): environments worth standing in and one row per vehicle a
// player would recognise, both chosen by the core from game data (sc_viewer_catalog, scviewer-core catalog.rs), never
// by asset names. Listed once per archive session by USCViewerSubsystem.
#pragma once
#include "CoreMinimal.h"
#include "SCViewerCatalog.generated.h"

/** One pick-list row: what to load (Id: an object-container path or an entity name), how to show it. */
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCCatalogEntry
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString Id;
    /** The game's name for it (localized display name), else Id's last part. */
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString Label;
    /** The list's grouping: an environment's kind, a vehicle's manufacturer. */
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString Group;
    /** A short second line: size, role. */
    UPROPERTY(BlueprintReadOnly, Category = "SC") FString Detail;
};

struct SCBRIDGE_API FSCViewerCatalog
{
    TArray<FSCCatalogEntry> Environments;
    TArray<FSCCatalogEntry> Vehicles;
    /** Changes whenever the lists are replaced (a new archive session). */
    uint32 Revision = 0;
    bool bReady = false;
    FString Error;
    /** The core's JSON ({"environments": [...], "vehicles": [...]}, rows {id, label, group, detail}). */
    static bool Parse(const FString& Json, FSCViewerCatalog& Out, FString& OutError);
};
