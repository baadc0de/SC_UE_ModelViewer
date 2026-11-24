// Hangar waypoints (WP-P1 follow-up): the places of an environment a player walks to, chosen from the package's records by
// their DataCore components, never by names; and the first-person teleport menu's sections (ship spawns, hangar places).
#pragma once
#include "CoreMinimal.h"
#include "SCSceneManifest.h"
#include "SCHangarWaypoints.generated.h"

struct FSCSceneManifest;

/** What makes an environment record a waypoint (the evidence, logged and listed as `source=`). */
UENUM()
enum class ESCWaypointSource : uint8
{
    TransitGateway, // `interiors[].entities[]` kind `transit_gateway` (TransitGatewayParams): where the transit system
                    // (the personnel elevator) lets the player into the hangar
    Panel,          // a placement carrying an operating panel's UI (FSCUiBinding::IsOperatingPanel: ship-selector and
                    // freight-elevator kiosks, transit panels); in a container with one transit gateway and one transit
                    // panel (an operated UI whose owner is not an attachable item) that panel is the gateway's call panel
                    // and is left to the gateway
    Inventory,      // an interior inventory container (`inventory_containers[]`, InventoryContainer: item banks, the
                    // freight elevator's cargo grid)
    ShipPad,        // the ship elevator the ship stands on (FSCEnvironment::FindShipBay), at its top centre
};

/** One waypoint chosen from the manifest (positions are resolved by the importer from the record's anchor). */
struct FSCWaypointCandidate
{
    ESCWaypointSource Source = ESCWaypointSource::Panel;
    int32 Interior = INDEX_NONE; // Manifest.Interiors index (TransitGateway, Panel)
    int32 Index = INDEX_NONE;    // Interiors[Interior].Entities (TransitGateway) / .Placements (Panel) / Manifest.InventoryContainers
    FString Label;               // CleanLabel of the entity name (a placement's authored `entity_name`; Inventory: its entity
                                 // class), before NumberDuplicates
};

namespace SCHangarWaypoints
{
/** The rule, in list order: transit gateways, panels, inventory containers (each in export order). The ship pad is added
 *  by ASCVehicle::BuildWaypoints from the presented geometry. */
SCBRIDGE_API TArray<FSCWaypointCandidate> Select(const FSCSceneManifest& Manifest);
/** An entity name as a label, cleaned like the vehicle spawns' (underscores to spaces), without the editor's instance
 *  counter ("-029") or a mesh LOD suffix ("_LOD0"). */
SCBRIDGE_API FString CleanLabel(const FString& Name);
/** Labels that repeat get " #1", " #2", ... in list order; unique ones stay as they are. */
SCBRIDGE_API void NumberDuplicates(TArray<FString>& Labels);
SCBRIDGE_API const TCHAR* SourceName(ESCWaypointSource Source);
}

/** One row of the first-person teleport menu: a section heading or a destination. */
struct FSCTeleportRow
{
    enum class EType : uint8 { Heading, Ship, Hangar };
    EType Type = EType::Heading;
    int32 Index = INDEX_NONE; // ASCVehicle::SpawnPoints (Ship) / the character's hangar list (Hangar)
    bool IsDestination() const { return Type != EType::Heading; }
};

namespace SCTeleportMenu
{
/** Ship spawns under a "Ship" heading, then hangar places under a "Hangar" heading; a section without entries has no
 *  heading. */
SCBRIDGE_API TArray<FSCTeleportRow> Rows(int32 ShipCount, int32 HangarCount);
/** The destination Delta destination rows away from Current (headings skipped, wrapping across both sections); the
 *  first destination when Current is not one. INDEX_NONE without destinations. */
SCBRIDGE_API int32 Step(const TArray<FSCTeleportRow>& Rows, int32 Current, int32 Delta);
}
