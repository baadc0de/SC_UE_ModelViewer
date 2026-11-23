// WP-O1-4: UnrealCitizen's scenario — what a run puts in the world (an environment, a vehicle and its paint, where the
// player starts, the cargo setup and the music) — and the record the prep step (-run=SCPrepare) writes once the
// scenario's levels are prepared as assets. The game runs a scenario only from its record (UUCScenarioSubsystem).
#pragma once
#include "CoreMinimal.h"

/** A scenario definition: Config/Scenarios/<id>.json. */
struct UNREALCITIZEN_API FUCScenario
{
    FString Id;          // the file's base name
    FString Title;       // "name"
    /** "environment": an object container path (*.socpak); empty: the map's own hangar (or none, when the cargo setup
     *  is in space). */
    FString Environment;
    FString Vehicle;     // "vehicle": the entity class (required)
    /** "skin": "default" or empty, an index into the vehicle's paints, or a part of a paint's name or palette id
     *  (USCViewerSubsystem::ResolvePaint). The prep step bakes it into the vehicle's level. */
    FString Skin;
    /** "player": {"camera": "first" | "orbit" | "fly" (default first), "start": a part of a teleport destination's label
     *  (first person; empty: where first person puts the player by default)}. */
    FString Camera = TEXT("first");
    FString Start;
    FString Cargo;       // "cargo": a cargo setup under Config/Cargo (its name); empty: none
    FString Music;       // "music": a soundtrack cue (its id or title); empty: none
    FString Json;        // the definition as read
    FString CargoJson;   // the cargo setup's file as read (empty without one)

    /** <project>/Config/Scenarios. */
    static FString Directory();
    /** Parse a definition (Id names it); CargoJson, when the definition names a cargo setup, is that setup's file: its ship
     *  and environment, when it names them, must be the scenario's, and a setup in space takes no environment. */
    static bool Parse(const FString& Id, const FString& Json, const FString& CargoJson, FUCScenario& Out, FString& Error);
    /** Read Config/Scenarios/<Id>.json and the cargo setup it names (Config/Cargo/<cargo>.json). */
    static bool Load(const FString& Id, FUCScenario& Out, FString& Error);
};

/** What the prep step prepared for a scenario: Content/SCPrepared/<id>.json, staged with a packaged game. */
struct UNREALCITIZEN_API FUCPrepared
{
    /** Version of what the prep step writes: its levels (pooled meshes and textures, constant materials, the staged audio
     *  beside them) and this record. Each prepared level carries it too (<level>.cookformat beside the .umap); a level or
     *  a record with another version is stale. History (it was USCCookLibrary::FormatVersion):
     *  2: WP-I1 review (screens: M_SC_Screen / _Translucent masters and per-face panel images; pooled meshes keyed by
     *  their section topology). 3: WP-I1 I1-5 (panel renders at their face aspect with owner values). 4, 5: WP-I1 fix
     *  passes (screen emission; panel renders' layout, glyphs, styles). 6: WP-A1 (the level's package audio staged beside
     *  it: Content/SCAudio/<level>). 7: WP-A1 A1-2 (the staged audio.json carries the bus effects). 8: hangar waypoints
     *  (ASCVehicle::Waypoints). 9: WP-I1-4 (a large button's background at its styled alpha; generated UI textures pooled
     *  under a key carrying this version). The hover renders, panel aims and usables are import-time records a prepared
     *  level does not keep yet. 10: WP-O1-4 (the prep step; levels and records written by -run=SCPrepare). */
    static constexpr int32 FormatVersion = 10;

    int32 Format = 0;
    FString Scenario;        // the scenario's id
    FString Definition;      // the scenario's definition as prepared (the run plays this copy)
    FString CargoDefinition; // its cargo setup as prepared (empty without one)
    FString EnvironmentLevel; // long package name; empty when the scenario has no environment
    FString VehicleLevel;     // long package name (the paint baked in)
    FString Archive;          // the archive build the levels were prepared from (information)
    FString PreparedAt;       // UTC, ISO 8601

    /** <project>/Content/SCPrepared, and the record of a scenario in it. */
    static FString Directory();
    static FString File(const FString& Id);
    static bool Parse(const FString& Json, FUCPrepared& Out, FString& Error);
    FString ToJson() const;
    /** False with Error when the scenario has no record (not prepared) or it does not read. */
    static bool Load(const FString& Id, FUCPrepared& Out, FString& Error);
    bool Save(FString& Error) const;
    /** Why the game cannot run the scenario from this record (empty: it can): another format, a level that does not
     *  exist, or — when Source (the definition on disk) is given — a definition or cargo setup changed since it was
     *  prepared. Each answer ends with what to do. */
    FString Problem(const FUCScenario* Source, TFunctionRef<bool(const FString& Level)> LevelExists) const;
    FString Problem(const FUCScenario* Source) const;
    /** The command that prepares the scenario. */
    static FString PrepareCommand(const FString& Id);
};
