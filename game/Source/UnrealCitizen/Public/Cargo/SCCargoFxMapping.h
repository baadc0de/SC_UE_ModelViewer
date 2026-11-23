// WP-O1-4b-3b: the cargo effects a prepared scenario keeps. The tractor's hit and a box's contact with a ship are
// MaterialEffects custom effects: MaterialEffects.xml names the FXLib entry for the boxes' surface group, the entry
// (sc_material_effect) its particle effect and scale range, and a ship's record its custom effect
// (CollisionMFXEntityParams.customEffect). The prep step resolves them once from the archive; a run plays them
// (FSCCargoFx::StartPrepared) without asking it. The pure steps are shared with FSCCargoFx's run-time resolution.
#pragma once
#include "CoreMinimal.h"

class FSCBridgeClient;
struct FSCCargoDroneDefinition;
struct FSCTextResult;

struct UNREALCITIZEN_API FSCCargoFxMapping
{
    /** An effect reference (SCFxNames: the built system's name) and its FXLib entry's scale range; Reference empty: none. */
    struct FEffect
    {
        FString Reference;
        float MinScale = 1, MaxScale = 1;
    };
    /** The boxes' surface group it was resolved for (Config/FX/effects.json "runtime"): another one makes it stale. */
    FString BoxGroup;
    /** The tractor's hit on a box. */
    FEffect Hit;
    /** Ship or obstacle entity -> a box hitting it (Reference empty: it has none). */
    TMap<FString, FEffect> Contacts;

    FString ToJson() const;
    static bool Parse(const FString& Json, FSCCargoFxMapping& Out);
    /** At prep (a commandlet: blocks, pumping the game thread for the client's answers): the hit of Drone and the contacts
     *  of Entities. False with Error when MaterialEffects.xml cannot be read. */
    static bool Resolve(FSCBridgeClient& Client, const FSCCargoDroneDefinition& Drone, const TArray<FString>& Entities, FSCCargoFxMapping& Out,
                        FString& Error);

    // The pure steps (shared with FSCCargoFx).
    /** MaterialEffects.xml's CustomEffects: custom effect -> surface group -> `<FXLib>:<effect>` (lower-case keys). */
    static bool ParseCustomEffects(const FString& Xml, TMap<FString, TMap<FString, FString>>& Out);
    /** The FXLib entry CustomName plays on surface group Group (its "default" group otherwise); empty: none. */
    static FString EntryFor(const TMap<FString, TMap<FString, FString>>& Custom, const FString& CustomName, const FString& Group);
    /** An FXLib entry's (sc_material_effect JSON) first particle effect and its scale range. */
    static FEffect ParseEntry(const FString& EntryJson);
    /** A ship record's (DataCore JSON) CollisionMFXEntityParams.customEffect; empty: none. */
    static FString ContactCustomEffect(const FString& RecordJson);
};
