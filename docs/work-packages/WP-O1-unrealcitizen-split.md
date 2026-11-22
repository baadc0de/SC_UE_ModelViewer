# WP-O1 — Split into StarBreaker core, the viewer and the UnrealCitizen game

The repository was renamed `baadc0de/UnrealCitizen` (2026-11-22). One Unreal project and one plugin
(`ue/SCViewer` + `ue/Plugins/SCBridge`) grew both a model viewer and game-like features (mobiGlas, cargo
operations, music, Ship Link, Garage, cooking). This package separates them into three parts.

## Target

| Part | Where | What it is |
|---|---|---|
| Core | `vendor/StarBreaker/` (submodule), `crates/` | The exporter and our Rust (`scviewer-core`, `scbridge-ffi`, `scviewer-cli`). Unchanged by this WP. |
| Shared UE code | `plugins/SCBridge/` | Everything both apps run: the ship/environment actors and their rendering (materials, lights, look, exposure, SSDO, FX), doors, door and lift panels, lifts, seats, ladders, first person, the interaction popout, the RPC core. Works from UE assets. The runtime importer (FFI client, package reader, import job, processed cache) becomes its own module, `SCBridgeImport`, in O1-4, so a build can leave the p4k reader out. |
| Viewer | `viewer/` (`SCViewer.uproject`) | Loads an environment and a vehicle, from the command line or from a picker. Runtime import with the local cache; no cooking, no mobiGlas, no music, no cargo. |
| Game | `game/` (`UnrealCitizen.uproject`) | A normal, cookable Unreal project. Runs a scenario definition; carries the mobiGlas, cargo, music, Ship Link and Garage. Never reads `Data.p4k`: a prep step (editor) imports what a scenario needs as UE assets first. |

Decisions taken with the owner (2026-11-22):

- **Pure move first.** A move-only change lands quickly; open branches rebase onto it (git follows the renames),
  and the code split comes after in its own changes.
- **Ship Link and Garage are game-only.** The viewer keeps door pads and panels (a pad's own lock button too) but
  has no remote door control and no garage kiosk.
- **The game loads only UE assets.** The prep step is an editor step (today's cook path); the packaged game has no
  `scbridge` library and never reads p4k or package tars.
- **One shared plugin.** Both projects load `plugins/SCBridge`; the game adds its own modules on top.

## Stages

### O1-1 — Move (PR #40, merged)

Move only; the contents change only where a path changed.

| From | To |
|---|---|
| `ue/Plugins/SCBridge/` | `plugins/SCBridge/` |
| `ue/Plugins/NVIDIA/` (gitignored download) | `plugins/NVIDIA/` |
| `ue/SCViewer/` | `viewer/` |
| `ue/scripts/` | `viewer/scripts/` (the cook scripts moved on to `game/` in O1-2) |
| `ue/verification/`, `ue/README.md` | `viewer/verification/`, `viewer/README.md` |

Paths in scripts, `.gitignore`, crates' tests and docs follow. Nothing is renamed: the plugin stays `SCBridge`
and the project `SCViewer`.

Rebasing a branch onto it: `git -c merge.directoryRenames=true rebase main` puts files a branch added under the old
directories into the new ones. A worktree's ignored build output (`Binaries`, `Intermediate`, `DerivedDataCache`,
`Saved`) stays at the old place; move it with the project or let the next build recreate it.

### O1-2 — Split the shared plugin (this PR)

The game-only code left `SCBridge` for the new `game/` project (`UnrealCitizen.uproject`, modules `UnrealCitizen`
and `UnrealCitizenEditor`):

- cargo operations (`Cargo/`: drones, tug, jobs, beacons, flight paths, spawning, discovery, scenarios, the cargo
  subsystem and its console commands), the mobiGlas and the world callouts (`UI/Cargo`), Music (`SCMusicSubsystem`,
  `UI/Music`), the Ship Link app and its routes (`UI/ShipLink`, `SCShipLinkRpc`), Garage (`SCGarage*`, `UI/Garage`),
  and their tests → `game/Source/UnrealCitizen`;
- `SCBridgeEditor` (the cook library, Keep as assets, the Interchange baseline) → `game/Source/UnrealCitizenEditor`;
- `Config/Cargo`, the drone art install (`Content/SciFi_Drone_4`, ignored), the cook / packaging / cargo-art
  scripts, the cargo and mobiGlas routes and the cargo, Ship Link and Garage RPC drivers → `game/`;
- the cargo and Garage settings → `UUCSettings` (UnrealCitizen; saved in `GameUserSettings.ini` like
  `USCViewerSettings`, under the new class's section: a fleet saved before this change is not carried over).

Kept in the shared plugin, because the kept features use them:

- the cargo **grid model** (`SCCargoTypes`, `SCCargoRecords`, `SCCargoPlanner` with its dig-out and transfer plans,
  `USCCargoGridComponent`): package data the reader fills and environment access (`FSCEnvironmentAccess`) opens;
- the Ship Link **subsystem** (owners, roles, the door policy, door orders, lights) and `SCShipLinkDoors.cpp`: a door
  pad's lock button and the doors' refusals use them; only the app and its routes are the game's;
- the HUD design tokens and paint helpers, now `SCHudStyle` / `SCHudPaint` (`Public/UI/`), used by the interaction
  popout, the mobiGlas and the callouts;
- which drawn panels are garage kiosks and the aim at one (`ASCVehicle::IsGarageKiosk`, `AimGarageKiosk`): import
  data; what a kiosk does is the game's.

Where the shared code called into game features it now offers an extension point the game registers with at module
startup (`UnrealCitizen.cpp`):

| Extension point | What UnrealCitizen registers |
|---|---|
| `SCViewerShell::SetMenu`, `SetMenuOpen` / `IsMenuOpen` (the F1 menu; `ASCFirstPersonCharacter::bMenuOpen`, `EAction::Menu`, log `FP_MENU` / `VIEWER_MENU`) | the mobiGlas |
| `SCViewerShell::AddOverlay` (layers over the view in `SSCViewerRoot`) | the mobiGlas and the world callouts on one model |
| `SCViewerShell::AddInteraction` (`ISCInteractionProvider`: prompt rows and E, asked before the shared kinds) | the garage kiosk ("Open Garage") |
| `SCRpcExtensions::Add` (`ISCRpcExtension`: routes, state sections, activity, anomalies, catalogue, `/wait` conditions) | cargo (`/cargo/*`, `until=job`), Ship Link (`/ship/state|door|secure|lights|locate`), Garage (`/garage/*`) |
| `ASCVehicle::AddDoorUseQuery` (a reason a door is held) | a cargo job's access doors ("Door in use by cargo") |
| `ASCVehicle::DoorwayBlockerTag` (actors with a box root the doors wait for) | cargo boxes (`ASCCargoContainer`) |

`SCViewerShell::ViewerWorld` replaces `SCCargoUI::ViewerWorld`; the request-field helpers of the RPC harness are
shared in `SCRpcJson.h` (`SCRpcArgs`). Without the game module the viewer has no F1 menu (the help lines leave it
out), no garage prompt and no cargo, Ship Link or Garage routes. `Start-SCViewer -Game` launches UnrealCitizen.

The runtime importer's own module (`SCBridgeImport`) moves to O1-4: until the game runs prepared assets it imports at
run time like the viewer, so the split has nothing to leave out yet, and O1-4 shows which code a cooked game needs.
The plugin's cargo art (`/SCBridge/Cargo`) stays in the plugin until it can move with the editor's redirectors
(O1-4); the game cooks it (`DirectoriesToAlwaysCook`), the viewer no longer does.

### O1-3 — The viewer app (this PR)

**The menu.** The viewer's F1 menu (`viewer/Source/SCViewer/SSCViewerPicker`, registered with `SCViewerShell` as the
menu and a view overlay; also the top bar's "Menu (F1)") lists environments, vehicles and the loaded vehicle's paints in
the HUD look. A pick applies at once (`SetEnvironment`, `LoadEntity`, `SetPaint`), or, while a load runs, as soon as
it lets it: the latest click waits in the menu, marked LOADING. An environment's export or parse is asked to stop and its
result dropped; an import, once started, finishes; nothing is waited for on the game thread. A vehicle waits for the
environment it goes into; clicking the vehicle being loaded drops a later choice (unless its load is being cancelled:
then it is asked again), clicking the one it replaces brings it back after. The decisions are pure functions
(`DecideVehiclePick`, `DecideEnvironmentPick`) walked through these click sequences by `SCBridge.Viewer.PickerQueue`.
The row in the scene is an inverted bar marked IN SCENE; "in the scene" is archive-scoped (an environment or vehicle
built before the archive was reopened is loaded again when picked from the new archive's lists). The environment list starts with the procedural
hangar (the map's own); the environment chosen is saved once it is in the scene (a failed one is not) and restored at
the next start; clicking a failed one requests it again. Both lists have a search box (`-SCSearch=<q>` fills the vehicle one); the menu
opens scrolled to the current environment, vehicle and paint, and by itself at start when the command line asks for no
scene (no `-SCLoad`, `-SCEnvironment`, `-SCScript`, `-SCPackage`, `-SCPlayerDemo`, `-SCShowcase`, `-SCRpc`). While it
is open the engine's on-screen messages (they draw over all UI) are off when they were on, and turned back on when it
closes (a `DisableAllScreenMessages` given while it is open is undone then); however it closes, keys go back to the view. The side panels (model
browser with its options, hierarchy) and `sc.UI.Panels` are gone from the shared root; RPC `/ui` takes
`{menu: true|false}` instead of `panels`. Each app names itself in the top bar (`SCViewerShell::SetAppName`). A paint
row acts on the vehicle whose manifest listed it and is remembered for it (`LoadedEntity`), not for one still loading.

**The lists come from game data** (`crates/scviewer-core/src/viewer_catalog.rs`, `sc_viewer_catalog`,
`scviewer catalog`; `USCViewerSubsystem::GetCatalog`, listed once per archive session on demand — the menu when it opens and RPC
`/catalog` call `RequestCatalog`, runs that never ask (`-SCNoUI`, `-SCLoad` with the menu closed, the game) do not pay
for the scan; a listing finished for an earlier archive session is dropped; `/catalog` answers `environments_ready`
and, once listing or the archive open failed for the session (or the session was lost), `environments_error`; a
saved hangar waiting for the archive does not hold back opening another one — replacing the search for "hangar" in archive paths):

- *Environments*: `EntityClassDefinition` records with `InstancedInteriorComponentParams` and a set
  `SObjectContainerComponentParams.objectContainer` (84 records, 17 of them templates without a container): **61
  hangar containers**, grouped by size (`InstancedInteriorComponentParams.name`: Small / Medium / Large / XL Hangar),
  named by the places that offer them (`InstancedInteriorLocationMap.locationInteriors[]`, not `devOnly`:
  `location.name`, `defaultHangars[]`; records sharing a container merge their places, sorted, and take the smallest
  named size), else by the record (the first by name); independent of record order (`container_rows`, tested). Of the 239 archive paths with "hangar" in them the other
  178 are pieces of these containers (each city's hangar wraps the base room as a child: the Area18 XL hangar exports
  as one 3.9 GB package with it), building parts, test and event content, and legacy hangars nothing references.
- *Vehicles*: `VehicleComponentParams` records (1111) through these rules, in order:

| Rule | Keeps a record when | Left |
|---|---|---|
| R1 | it has `SEntityInsuranceProperties` (salvage hulks have none) | 1104 |
| R2 | `AttachDef.Localization.Name` resolves to non-blank text and is no `@LOC_*` placeholder | 1088 |
| R3 | no tag under `AI.Faction`, `AI.Spawning`, `AI.Ship.Unmanned`, and its crew manifest lists no crew; a record whose tags, crew component or crew manifest do not read (or whose manifest reference is malformed) is left out and listed in the catalogue's `skipped` | 412 |
| R4 | no `Missions.ItemType.Iasi.DisplayItem` tag | 411 |
| R5 | it insures to itself (`shipInsuranceParams.shipEntityClassName`) or `EAEntityDataParams.inclusionMode` is `ReadyToInclude` | 364 |
| R6 | one row per (`vehicleDefinition`, `modification`), split into members connected by shared `Ship.Model.*` tags (untagged members join the largest such group); independent of record order | **229** |

  R6's row is the self-insured member, else an entitled one (`DefaultEntitlementEntityParams`), else any: the one
  most records insure to, then the one whose name begins the others', then the most tagged. 206 spaceships, 23
  ground vehicles (11 hover bikes, `isGravlevVehicle`); the row shows the manufacturer (`SCItemManufacturer`), size,
  role and kind. Special editions (Pirate, Best in Show, Wikelo, Exec, ...) fold into their base; most are also paints of
  it. Known leftovers: a few real ships insure to another record (Idris-M/P, Fortune, Golem OX, Terrapin Medic, Aurora
  Mk II): in two such groups a template member won the row (`ANVL_Terrapin_Medic_AI_Template`,
  `RSI_Aurora_Mk2_MissileModule_TEMP`, identical in data to their bases); the old and new Constellation records both
  list; `DRAK_Caterpillar_PU_Hijacked` and `MISC_Starfarer_NoInterior` pass every rule. The shop and fleet lists that
  would settle "purchasable" are not in DataCore.
- *Paints*: the loaded vehicle's (`Manifest.Paints`). They are also knowable before load
  (`SGeometryResourceParams...SubGeometry[]` tags, paint items with `AttachDef.Type = Paints`); not used yet.

`scviewer catalog` printed the same 229 vehicles and 61 containers as the research scripts (and after the review's rule
fixes the same 229 vehicles; `skipped` holds one record, an AI Gladius whose crew manifest does not resolve).

**Command line.** `-SCEnvironment=<container path>` and `-SCLoad=<entity>` as before (the ids the lists show), and
`-SCSkin=<index|default|name part>` puts a paint on the first vehicle loaded or adopted from `-SCPackage` (as `sc.Paint`).

**No cooking in the viewer.** It never streams cooked levels (`SCViewerShell::SetCookedLevels(false)`): ships and
environments always import from the archive through the local cache. The locally cooked ships and hangars (ignored)
moved to `game/Content/SC`; the built effects (`/Game/SC/FX`, also ignored) are in both projects.

**Open (found by the research, not changed here):**

- `parse_localization` (`crates/scviewer-core/src/index.rs`) keeps the `,P` suffix some `global.ini` keys carry, so
  those keys never resolve in the entity index's display names; the catalogue looks keys up with and without it.
  Fixing the parser changes the cached browser index (a cache-format bump re-exports every cached package).
- `BrowserIndex::build` sets an entity's `category` from its record path (`entities/spaceships`): a path filter;
  `AttachDef.SubType` is the data's answer.
- Hangars without a place in data show their record name; ship fit by size (`sizeId` against `LandingPadSize` and
  `maxBoundingBoxSize`) is likely but not linked in data, so the menu does not filter hangars by the loaded ship.

### O1-4 — The UnrealCitizen game

- `game/UnrealCitizen.uproject` runs prepared assets only: the runtime importer becomes `SCBridgeImport` (FFI client,
  package reader/source, import job, processed caches, `SCBridgeLib`), loaded by the viewer and by the game's editor
  module, not by the packaged game. The plugin's cargo art moves to the game's content.
- **Scenario definition**: an environment, a vehicle (and its skin), the player start, cargo setup and music —
  what `-SCCargoScenario=` and the cooked showcase do today, as data the game runs.
- **Prep step** (`UnrealCitizenEditor -run=SCPrepare -Scenario=<name>`): imports the scenario's environment and
  vehicle from p4k into the project as assets (today's `cook_ships.py` / `cook_hangars.py` / `cook_audio.py`).
  The game refuses to start a scenario whose prepared assets are missing and says to run prep.
- The game is cooked and packaged like any Unreal project (`game/scripts/package_showcase.ps1`). Cooking and
  packaging still happen only on the owner's request.

## Verification

Each stage: `scripts/build_ffi.ps1`, the editor build of each project that exists, the SCBridge automation tests
(baseline 231/234), and for O1-3/O1-4 a launch through the RPC driver (C2 in the XL hangar: doors, panels, popout).
