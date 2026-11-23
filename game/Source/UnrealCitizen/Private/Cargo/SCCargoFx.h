// The game's effects for the cargo drones at run time (WP-K4c hook-up). What plays comes from the data, through the
// archive (FFI), and the Niagara system for an effect reference from the naming rule SCFxBuild built it by
// (SCFxNames.h); no asset or effect name is written here.
//  - The tractor beam, its muzzle and its hit: the tractor record's action (FSCCargoDroneDefinition: beamEffects,
//    fireEffects, hitEffectName). The hit effect is a MaterialEffects custom effect: MaterialEffects.xml gives the FXLib
//    entry for the surface group of what it hits (a box), and that entry (sc_material_effect) its particle effect.
//  - A box hitting a ship: the ship record's CollisionMFXEntityParams.customEffect, resolved the same way.
// Viewer choices (Config/FX/effects.json "runtime"): the boxes' surface group (no box surface reaches the runtime)
// and the contact threshold and rate limit (CryEngine's mfx_* thresholds are engine settings, not data).
// Without the archive, or a system not built on this machine, the effect is missing: the caller falls back (logged once).
#pragma once
#include "CoreMinimal.h"
#include "Cargo/SCCargoScenario.h"
#include "NiagaraSystem.h"
#include "UObject/StrongObjectPtr.h"
class FSCBridgeClient;
struct FSCCargoFxMapping;

class FSCCargoFx : public TSharedFromThis<FSCCargoFx>
{
public:
    /** Effects.json "runtime": the boxes' surface group, the least speed change (cm/s) along the contact normal a box
     *  hit plays its effect at, and the least time (s) between two effects of one box. */
    struct FRuntime
    {
        FString BoxGroup;
        double ContactMinSpeedCmS = 0, ContactMinIntervalS = 0, ContactFullSpeedCmS = 0;
        bool bLoaded = false;
    };
    static const FRuntime& Runtime();

    /** Resolve the tractor's effects (once per definition): Beam / Muzzle at once, Hit through MaterialEffects. */
    void Start(const TSharedPtr<FSCBridgeClient, ESPMode::ThreadSafe>& InClient, const FSCCargoDroneDefinition& Drone);
    /** WP-O1-4b-3b: a prepared scenario's effects: the beam and muzzle as Start sets them, the hit and the contacts from
     *  the mapping the prep step resolved (FSCCargoFxMapping); InClient (may be null) for ships it does not cover. */
    void StartPrepared(const TSharedPtr<FSCBridgeClient, ESPMode::ThreadSafe>& InClient, const FSCCargoDroneDefinition& Drone, const FSCCargoFxMapping& Mapping);
    UNiagaraSystem* Beam() const { return BeamSystem.Get(); }
    UNiagaraSystem* Muzzle() const { return MuzzleSystem.Get(); }
    UNiagaraSystem* Hit() const { return HitSystem.Get(); }
    const FSCCargoDroneDefinition::FStrengths& Strengths() const { return StrengthValues; }
    /** The effect a box plays hitting the ship of entity ShipEntity, and its scale range (the FXLib entry's minscale /
     *  maxscale); null while it is being resolved (started on the first ask) or when there is none. */
    UNiagaraSystem* Contact(const FString& ShipEntity, float& OutMinScale, float& OutMaxScale);
    /** Why the effects are missing, for a once-only log (empty when the tractor's are resolved). */
    FString Missing() const { return Why; }

private:
    TSharedPtr<FSCBridgeClient, ESPMode::ThreadSafe> Client;
    FString StartedFor;
    TStrongObjectPtr<UNiagaraSystem> BeamSystem, MuzzleSystem, HitSystem;
    FSCCargoDroneDefinition::FStrengths StrengthValues;
    FString Why = TEXT("not started");
    // MaterialEffects.xml: custom effect -> surface group -> `<FXLib>:<effect>` (lower-case keys).
    enum class EXml : uint8 { None, Loading, Ready, Failed } Xml = EXml::None;
    TMap<FString, TMap<FString, FString>> Custom;
    TArray<TFunction<void()>> AfterXml;
    struct FContact
    {
        bool bDone = false;
        TStrongObjectPtr<UNiagaraSystem> System;
        float MinScale = 1, MaxScale = 1;
    };
    TMap<FString, FContact> Contacts;

    void WithMaterialEffects(TFunction<void()> Then);
    /** The FXLib entry custom effect CustomName plays on the boxes' surface group (its "default" group otherwise). */
    FString CustomEntry(const FString& CustomName) const;
    /** An FXLib entry's first particle effect and scale range, then Done(reference, min, max) (reference empty: none). */
    void ResolveEntry(const FString& Entry, TFunction<void(const FString&, float, float)> Done);
    /** The built system of an effect reference (SCFxNames rule); null, logged once, when it is not built. */
    static UNiagaraSystem* Load(const FString& Reference, const TCHAR* What);
};
