// Cargo scenario (WP-K1, content): ship and environment it is meant for, the stock to put on grids (box entities,
// counts, commodity), drone docks, and the drone definition (tractor-beam record values with provenance, flight limits).
// Loaded from ue/SCViewer/Config/Cargo/<name>.json via USCViewerSettings::CargoScenario / -SCCargoScenario=.
#pragma once
#include "CoreMinimal.h"
#include "Cargo/SCCargoTypes.h"

/** Which grid: the vehicle's or the environment's, by index in export order, or the first whose port path / entity
 *  contains Match (case-insensitive). */
struct SCBRIDGE_API FSCCargoGridSelector
{
    ESCCargoGridKind Kind = ESCCargoGridKind::Vehicle;
    int32 Vehicle = 0; // which of the scenario's vehicles (0: the loaded one, then the extra instances in order)
    int32 Index = 0;
    FString Match;
};

struct SCBRIDGE_API FSCCargoStockEntry
{
    FString Entity; // box entity class (exported as a package for its mesh and occupancy)
    int32 Count = 1;
    FName Commodity;
    FText CommodityLabel;
    /** Optional lattice cell (min corner) and yaw for every box of the entry (content: e.g. a stacked column); without,
     *  the planner's best fit. */
    TOptional<FIntVector> Origin;
    bool bRotated = false;
    FName Owner; // whose boxes; None = the grid's owner (the player on shared grids)
};

struct SCBRIDGE_API FSCCargoStock
{
    FSCCargoGridSelector Grid;
    TArray<FSCCargoStockEntry> Boxes;
};

struct SCBRIDGE_API FSCCargoDockSpec
{
    FSCCargoGridSelector Grid;
    int32 Drones = 0;
};

/** Lift from a DataCore tractor-beam action: read from the record at run time (sc_datacore_record); the scenario's copy
 *  (with its provenance) is the fallback when the archive or the record is
 *  unavailable. Flight limits are viewer choices (no
 *  drone entity exists in the game data). */
struct SCBRIDGE_API FSCCargoDroneDefinition
{
    FString TractorRecord, TractorRecordGuid, TractorAction, Provenance;
    double MaxForceN = 0;
    double MinDistanceM = 0, MaxDistanceM = 0, FullStrengthDistanceM = 0;
    /** maxVolume / volumeForceCoefficient of the action, logged and kept, not applied: their semantics are not
     *  established (see ApplyTractorRecord). */
    double MaxVolume = 0, VolumeForceCoefficient = 0;
    /** maxAngle: the cone (degrees off the emitter's aim) the beam holds within (round 5: how far below level a drone's
     *  hold may point); 90 when the record does not say. */
    double MaxAngleDeg = 90;
    FString TractorSource = TEXT("scenario"); // "datacore" once read from the record
    /** The tractor's effects (WP-K4c), from the same action: its beam (beamGroup.beamEffects[0].particleEffect) and
     *  muzzle (fireEffects[0].firstPerson) particle effect references (`<GUID>|<library>.<effect>`), and its hit effect,
     *  a MaterialEffects custom effect name (beamGroup.hitEffectName). Empty without the record. */
    FString BeamEffect, MuzzleEffect, HitEffectName;
    /** beamStrengthValues: the effects' EmitterStrength by beam state (stable: holding its target, unstable: a target
     *  not under control, noTarget), the impact's (controlling / invalid target), and beamEffectBlendCoeff, the rate
     *  (1/s) at which the shown strength approaches the state's. bSet only once read from the record. */
    struct FStrengths
    {
        double Stable = 0, Unstable = 0, NoTarget = 0, ImpactControlling = 0, ImpactInvalid = 0, BlendCoeff = 0;
        bool bSet = false;
    } Strengths;
    /** Take the tractor values from a DataCore record JSON (sc_datacore_record's shape, `__type`; the CLI's `_Type_`
     *  also accepted): the SWeaponActionFireTractorBeamParams object named TractorAction. False (values unchanged)
     *  with Error when the record has no such action or it lacks a force. */
    bool ApplyTractorRecord(const FString& RecordJson, FString& Error);
    float MaxSpeedMS = 0, MaxAccelMS2 = 0, ClearanceMarginM = 0, FormationSpacingM = 0;
    /** Jerk limit (m/s^3), drone mass (kg), the least acceleration a loaded team must keep (m/s^2), and how far a box is
     *  drawn along its grid's normal (beyond its own height) before it flies free (m). Viewer choices. */
    float MaxJerkMS3 = 0, DroneMassKg = 0, MinLoadAccelMS2 = 0, NormalClearanceM = 0;
    /** Gusts in atmosphere (viewer choices): position wander (cm), wobble (degrees) and base frequency (Hz) of an empty
     *  drone at rest; less under load, a little more at speed. */
    float SwayAmplitudeCm = 4, SwayTiltDeg = 3, SwayFrequencyHz = 0.3f;
};

struct SCBRIDGE_API FSCCargoScenario
{
    FString Path, Name, Ship, Environment;
    /** Air where the drones fly (gusts): no data signal names a room's atmosphere, so the scenario says so; when it does
     *  not, a scenario with an environment (a hangar: pressurised) has air, one without does not (vacuum). */
    bool bAtmosphere = false;
    /** Owners (multi-owner hangars): the player's id, labels of the others, and the owner of each vehicle (index 0 the
     *  loaded ship; more entries spawn more instances of the scenario's ship). */
    FName Player = TEXT("you");
    TArray<TPair<FName, FText>> Owners;
    TArray<FName> VehicleOwners;
    TArray<FSCCargoStock> Stock;
    TArray<FSCCargoDockSpec> Docks;
    FSCCargoDroneDefinition Drone;

    /** Space (WP-K3): no environment (the ships float where they are placed), no gravity, no atmosphere. */
    bool bSpace = false;
    /** The space sun's illuminance in lux (`sun_lux`; viewer choice); unset (< 0): the viewer's default. */
    double SunLux = -1;
    /** Where each vehicle is (index as VehicleOwners, metres and degrees); unset: placed on free floor beside the first. */
    TArray<TOptional<FTransform>> VehiclePoses;
    /** Other entities placed as they are (asteroids around the ships in space): obstacles to the drones and loads. */
    struct FObstacle
    {
        FString Entity;
        FTransform Pose;
    };
    TArray<FObstacle> Obstacles;
    /** Stock put down loose (WP-K2): at a pose, or at free reachable spots drawn from the scenario's seed. */
    struct FLoose
    {
        FString Entity;
        int32 Count = 1;
        FName Commodity, Owner;
        FText CommodityLabel;
        TOptional<FTransform> At;
    };
    TArray<FLoose> Loose;
    int32 Seed = 1;

    bool IsSet() const { return !Path.IsEmpty(); }
    /** Distinct box entities of the stock. */
    TArray<FString> Entities() const;
    static bool Load(const FString& File, FSCCargoScenario& Out, FString& Error);
    static bool Parse(const FString& Json, FSCCargoScenario& Out, FString& Error);
};
