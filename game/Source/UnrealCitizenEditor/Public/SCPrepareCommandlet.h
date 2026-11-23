// WP-O1-4: UnrealCitizen's prep step (it replaces game/scripts/cook_ships.py, cook_hangars.py and cook_audio.py).
//
//   UnrealEditor-Cmd game/UnrealCitizen.uproject -run=SCPrepare -Scenario=<name> -AllowCommandletRendering -unattended
//       [-Force]
//
// Reads Config/Scenarios/<name>.json (UCScenario.h), exports its environment and vehicle from the archive (Data.p4k,
// auto-discovered, through the bridge), imports each into a new level, presents it as the game shows it (the hangar's
// mechanisms open and the ship elevator under the origin; the ship on its landing gear on the floor, the scenario's paint
// on), stages its audio beside the level (SCPrepareAudio.h) and makes the level persistent with pooled meshes and textures
// and constant materials (USCCookLibrary::CookEnvironment). Each level is stamped with FUCPrepared::FormatVersion
// (<level>.cookformat); a level already prepared in this format is kept, -Force prepares it again (an older level is
// deleted with its materials first; the shared pools stay). Last it writes the
// scenario's record, Content/SCPrepared/<name>.json, which the game runs from (UUCScenarioSubsystem).
//
// Levels: environments /Game/SC/Hangars/Maps/<style>_<name> (materials /Game/SC/Hangars/<style>_<name>, meshes and
// textures /Game/SC/Hangars/Shared, the ship capacity in the hangar index); vehicles /Game/SC/Ships/Maps/<entity>[__<paint>]
// (<paint>: the paint's palette id, else its subgeometry tag, else its name, and a hash of all three)
// (materials /Game/SC/Ships/<level name>, meshes and textures /Game/SC/Ships/Shared). All game-derived (gitignored).
// Textures are prepared at full resolution (the shared pools key a texture by its source, so one resolution per pool).
// Needs a renderer (-AllowCommandletRendering: runtime textures and Nanite builds). Exit code 0 when the record is written.
#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "SCPrepareCommandlet.generated.h"

UCLASS()
class USCPrepareCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    USCPrepareCommandlet();
    virtual int32 Main(const FString& Params) override;
};
