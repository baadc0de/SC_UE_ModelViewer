// UnrealCitizen's settings (WP-O1-2; the game's part of what USCViewerSettings held before): the cargo scenario and fleet,
// cargo tuning and the Garage fleet. Saved like USCViewerSettings, in Saved/Config/<Platform>/GameUserSettings.ini.
#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UCSettings.generated.h"

UCLASS(BlueprintType, Config = GameUserSettings)
class UNREALCITIZEN_API UUCSettings : public UObject
{
    GENERATED_BODY()
public:
    /** WP-M2: an empty list uses the available vehicle catalog; explicit entries remain available as the player's fleet. */
    UPROPERTY(Config) TArray<FString> GarageFleet;

    /** WP-K1 cargo scenario (stock per grid, drone docks, drone definition): a JSON file, relative to the project's
     *  Config/Cargo directory or absolute; empty = no scenario. -SCCargoScenario=<file> overrides it for one run. */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Cargo") FString CargoScenario;
    /** The mobiGlas lists the grids within this distance of the player, nearest first (metres; a game choice).
     *  0 = every grid in the level: a ship and the freight elevator it loads from can be far apart in a large hangar,
     *  and standing at one must not hide the other. */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Cargo") float CargoGridListRadiusM = 0.f;
    /** A transfer fails after this many consecutive boxes could not be lifted (and no other box could take their place). */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Cargo") int32 CargoPickAttempts = 3;
    /** Absolute path of the scenario file, empty when none is set. */
    FString ResolveCargoScenario() const;
    /** Live cargo spawning without a scenario: the drone fleet (a scenario file's drone definition and docks; its ship,
     *  environment and stock are ignored) a session takes on with its first spawn. Resolved like CargoScenario;
     *  -SCCargoFleet=<file> overrides it, empty or "none" = no drones for spawned cargo. */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Cargo") FString CargoFleet = TEXT("fleet");
    /** The entity-index search that finds the candidate box records for live spawning (a name substring); each hit is
     *  kept only when its DataCore record is a whole-SCU box holding a resource (empty: every entity, one record read
     *  each: slow). A name filter for speed only: what is a box is decided by the record. */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Cargo") FString CargoBoxSearch = TEXT("Carryable_TBO_");
    FString ResolveCargoFleet() const;

    /** WP-O1-4: the scenario a run plays (Config/Scenarios/<name>.json, prepared by -run=SCPrepare; UUCScenarioSubsystem);
     *  empty = none (the command line and the menus decide). -Scenario=<name> overrides it for one run, -Scenario=none
     *  turns it off. */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Scenario") FString Scenario;
    /** The scenario's name for this run (empty: none). */
    FString ResolveScenario() const;
    /** Set while a scenario runs (or was refused): it owns the scene, so the cargo subsystem loads no ship or environment
     *  of its own and takes the scenario's prepared cargo setup (ScenarioCargoJson, none when empty) instead of
     *  CargoScenario. Not saved. */
    bool bScenarioOwnsScene = false;
    FString ScenarioCargoName, ScenarioCargoJson;
    /** The running scenario's vehicle: the ship of its cargo setup when the setup names none. */
    FString ScenarioVehicle;
    /** WP-O1-4b-3: the running scenario's prepared cargo boxes (entity -> level: the box library streams them instead of
     *  exporting) and its tractor record as read at prep (the cargo setup applies it instead of asking DataCore). */
    TMap<FString, FString> ScenarioCargoLevels;
    /** WP-O1-4b-3c: its obstacles' prepared levels (entity -> level), as the record names them. */
    TMap<FString, FString> ScenarioObstacleLevels;
    FString ScenarioTractorRecordJson;
    /** WP-O1-4b-3b: its cargo effects as resolved at prep (FSCCargoFxMapping JSON; empty: resolved from the archive). */
    FString ScenarioCargoFxJson;
    /** Set by the cargo subsystem when a prepared box did not load: the scenario fails with it. */
    FString ScenarioCargoProblem;

    UFUNCTION(BlueprintPure, Category = "SC|Settings") static UUCSettings* Get();
    UFUNCTION(BlueprintCallable, Category = "SC|Settings") void Save();
};
