# WP-M2 — Garage (mobiGlas app: the player's ships as property)

**Agents:** Opus 5.5 (app, retrieve and store, RPC), Codex `gpt-6.1-terra` (exporter research for loadout changes,
if taken up), Codex `gpt-6.2-astra` (visual review). Cross-review: Codex ⇄ Opus.
**Depends on:** WP-M1 (pairing, the app rail, the placeholder rule), WP-K1f (vehicle instances), the hangar ship bay
(`FSCEnvironment::FindShipBay`).
**Branch:** `wp/M2-garage`.
**Owner decisions (2026-11-20), binding for this WP:** vehicle configuration lives in the Garage, not in Ship Link;
the Garage replaces the game's kiosk / ASOP-terminal idea (the owner dislikes kiosks); unbuilt features are visible
`NOT INSTALLED` rows, as in WP-M1.

## Why

Star Citizen splits "my ships" across fleet-manager kiosks, ASOP terminals and the vehicle manager app, all tied to
standing at a terminal. The viewer puts it in one mobiGlas app, available anywhere in the hangar: which ships I have,
bring one out, put it away, and configure it. Ship Link (WP-M1) is the ship *in the world*; the Garage is the ship as
*property*. The hand-off between them is pairing: **retrieving a ship pairs it to the player; storing it ends the
pairing.** This replaces WP-M1's stand-in "pair at placement" for the player's own ships, with no change to Ship Link.

## Behaviour

### Fleet

1. **The player's fleet** is a list of vehicle entities. There is no game data for a player's ownership, so the list is
   the viewer's: a setting (`USCViewerSettings`) holding entity names, defaulting to the ships this build can show (the
   cooked ships in a packaged showcase; in a development build, the entity browser's vehicles, searchable, with
   "add to fleet"). No ship names are hard-coded.
2. Each fleet row: display name (DataCore, localised), manufacturer, role / size (as exported), a thumbnail when one
   exists (else none), and its state: `STORED`, `RETRIEVING`, `IN HANGAR` (with distance), `UNAVAILABLE` (failed to
   load, with the reason).

### Retrieve and store

1. **Retrieve** places the ship on the hangar's ship bay (`FindShipBay`, its pad's top centre, landing gear rested)
   through the existing vehicle-instance path (`SpawnVehicleInstance(Entity, World)`; the loaded vehicle when none is
   out), and pairs it to the player as `Owner` (WP-M1 store). Progress shows in the row from the import job.
2. **The bay holds one ship.** Retrieving while another of the player's ships stands on it asks to store that one first
   (one confirm, then both steps); a bay occupied by someone else's ship refuses with the reason. A second ship can be
   retrieved to free hangar floor (`FSCVehiclePlacement`) when the bay is taken, as `sc.Vehicle.Spawn` already does.
3. **Store** removes the ship (`DestroyVehicleInstance`, or unloads the loaded vehicle) and ends its pairing. Refused
   while the player or a lift rider is aboard (the player would lose the floor), or while a cargo transfer uses one of
   its grids; the reason is shown. Loose cargo resting on it falls and settles, as WP-K2 already handles.
4. Retrieve and store are instant placement in this WP; the hangar ship elevator's lowering and raising is a later
   feature (placeholder below).

### Configuration

1. **Paint (live)**: the ship's paints (`Manifest.Paints`) as a list with the active one marked; choosing one calls
   `ASCVehicle::SetPaint` on the retrieved ship and is remembered per fleet entry for the next retrieve.
2. **Loadout (read-only, live)**: items by port, from the manifest's `children[]` (`port_path`, display name, size),
   grouped by item type as exported. The same view Ship Link summarises; Ship Link's `Configure` placeholder becomes a
   link here.
3. **Loadout changes (placeholder)**: swapping an item needs an export with an overridden loadout. Research only in this
   WP: whether StarBreaker can export a vehicle with a loadout override (and how the cache keys it), written up as a
   short note in the brief's Verification section with a recommendation. No code for it here.

### Kiosks

The hangar's ship-selector kiosk (an operating panel, found as a hangar waypoint of kind `Panel`) keeps its own render
(inherited panel UI). **E on it opens the mobiGlas on the Garage app** as a shortcut; the Garage never requires the
kiosk.

### Placeholders (NOT INSTALLED rows, WP-M1 style)

| Placeholder | Description line (draft) |
|---|---|
| Loadout changes | Swap weapons, shields and components. |
| Ship delivery | Watch the hangar elevator bring the ship up and take it down. |
| Insurance claim | Replace a lost or destroyed ship. |
| Transfer | Move a ship to another hangar or location. |
| Hand over | Give a ship to another player. |

## Data and code

- Fleet setting in `USCViewerSettings` (entity names, remembered paint per entry); a Garage model beside the Ship Link
  model; Slate under `UI/Garage/`; GARAGE on the rail becomes live (it was a `NOT INSTALLED` app in WP-M1).
- Pairing on retrieve and unpairing on store go through the WP-M1 store; WP-M1's placement hook keeps pairing vehicles
  placed by other paths (scenarios, RPC, `-SCExtraVehicles`), so tests and scenarios do not change.
- **RPC**: `/garage/fleet` (rows and states), `/garage/retrieve` `{ entity, at?: bay|floor }`, `/garage/store`
  `{ ship }`, `/garage/paint` `{ ship | entity, paint }`; the state log gains `garage.fleet[]`. Refusals carry their
  reason.

## Tests (automation, failing first)

- Retrieve pairs, store unpairs; a ship placed by another path is still paired (WP-M1 hook).
- Bay rules: occupied by own ship (store-first), by another owner's (refused), floor fallback.
- Store refused with the player aboard, with a transfer running; allowed otherwise.
- Paint remembered per fleet entry and applied on the next retrieve.
- Fleet default: built from the available ships, no names in code.

## Verification

- `Automation RunTests SCBridge.Garage` (new), `SCBridge.ShipLink`, `SCBridge.Cargo` passing.
- RPC run in `hangar_xltop_001`: fleet of two ships; retrieve one to the bay, the second to the floor; store refused
  with the player aboard, then allowed; paint change kept across store and retrieve. State-log excerpts pasted.
- Screenshots: Garage fleet, a ship's configuration page (paint, loadout, placeholders), the kiosk opening the Garage.
  Astra review against the extended mock.
- The loadout-override research note.

**Done means:** from anywhere in the hangar the player opens GARAGE, retrieves a ship onto the bay, sees it appear
and become their paired ship in SHIP LINK, changes its paint, reads its loadout, and stores it again, which removes
it and its pairing. Unbuilt Garage features show as NOT INSTALLED rows.


### Vehicle loadout override research

The public vehicle export options in StarBreaker's `pipeline/mod.rs` cover attached items and the normal vehicle
loadout. The XML resolver applies the entity's authored vehicle and nested item loadouts. The ship exporter has no
public override parameter. The player's kit exporter has a separate loadout XML option and resolves it against the
player template. Ship exports are keyed in `crates/scviewer-core/src/cache.rs` by archive build, entity, exporter,
cache format and serialized decomposed options.

For a future loadout editor, add an explicit canonical set of port-path to entity overrides to the ship resolver and
the decomposed options. Validate against DataCore port compatibility, then key the exported package and any processed
cache by that same normalized selection. This keeps two different configurations of one ship from sharing derived
assemblies. The current Garage uses read-only manifest items and stores paint by palette ID.

### Implementation notes

The primary slot uses the viewer's ordinary prepared ship bay and preserves extra instances on Garage store/retrieve.
An explicit floor retrieval uses the same free-floor solver as existing extra vehicles and excludes the ship bay.
Kiosks are recognized by their operating panel's authored VehicleList and RetrievingVehicle states and UseConsole
interaction. Garage reads the visible rows' authored manufacturer codes by AttachDef.Manufacturer, plus configured type/role and size where present. The examined vehicle catalogue and scene manifests provide no usable ship thumbnails.

## Out of scope

Loadout editing (research only), animated delivery by the ship elevator, insurance, transfers between locations,
handing ships to other players, persistence beyond the fleet setting.
