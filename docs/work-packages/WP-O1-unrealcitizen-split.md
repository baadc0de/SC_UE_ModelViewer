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

### O1-3 — The viewer app

- **Picker.** First an environment from a filtered list (hangars and other spaces worth standing in), then a vehicle,
  then its skin. The filters come from the data (DataCore categories, the hangar index, what an environment
  contains), never from asset names.
- **Vehicle list.** One row per ship a player would recognise: duplicates, aliases, AI/template/unmanned variants
  and entities sharing one vehicle definition and geometry collapse into one row (category rules derived from
  DataCore; the rules are listed in the WP's result with the counts they remove).
- **F1 menu** changes environment, vehicle or skin at any time. The hierarchy and model-browser panels go.
- **Command line:** `-SCEnv=<id> -SCVehicle=<id> -SCSkin=<id>` loads directly (no picker).
- Keeps the local processed cache, every rendering feature, doors/panels/lifts/seats/ladders, first person, the
  interaction popout and the RPC routes for what it supports (no cargo, music, Ship Link or Garage routes).

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
