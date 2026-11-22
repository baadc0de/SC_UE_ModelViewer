# WP-O1 — Split into StarBreaker core, the viewer and the UnrealCitizen game

The repository was renamed `baadc0de/UnrealCitizen` (2026-11-22). One Unreal project and one plugin
(`ue/SCViewer` + `ue/Plugins/SCBridge`) grew both a model viewer and game-like features (mobiGlas, cargo
operations, music, Ship Link, Garage, cooking). This package separates them into three parts.

## Target

| Part | Where | What it is |
|---|---|---|
| Core | `vendor/StarBreaker/` (submodule), `crates/` | The exporter and our Rust (`scviewer-core`, `scbridge-ffi`, `scviewer-cli`). Unchanged by this WP. |
| Shared UE code | `plugins/SCBridge/` | Everything both apps run: the ship/environment actors and their rendering (materials, lights, look, exposure, SSDO, FX), doors, door and lift panels, lifts, seats, ladders, first person, the interaction popout, the RPC core. Works from UE assets. The runtime importer (FFI client, package reader, import job, processed cache) is its own module, `SCBridgeImport`, so a build can leave the p4k reader out. |
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

### O1-1 — Move (this PR)

Move only; the contents change only where a path changed.

| From | To |
|---|---|
| `ue/Plugins/SCBridge/` | `plugins/SCBridge/` |
| `ue/Plugins/NVIDIA/` (gitignored download) | `plugins/NVIDIA/` |
| `ue/SCViewer/` | `viewer/` |
| `ue/scripts/` | `viewer/scripts/` (the cook scripts move to `game/` in O1-4) |
| `ue/verification/`, `ue/README.md` | `viewer/verification/`, `viewer/README.md` |

Paths in scripts, `.gitignore`, crates' tests and docs follow. Nothing is renamed: the plugin stays `SCBridge`
and the project `SCViewer`.

Rebasing a branch onto it: `git -c merge.directoryRenames=true rebase main` puts files a branch added under the old
directories into the new ones. A worktree's ignored build output (`Binaries`, `Intermediate`, `DerivedDataCache`,
`Saved`) stays at the old place; move it with the project or let the next build recreate it.

### O1-2 — Split the shared plugin

The game-only code leaves `SCBridge` for the game project's modules (created here as a skeleton that builds):

- Cargo (`Private/Cargo`, `Public/Cargo`), the mobiGlas (`Private/UI/Cargo`), music (`SCMusicSubsystem`,
  `UI/Music`), Ship Link (`SCShipLink*`, `UI/ShipLink`), Garage (`SCGarage*`, `UI/Garage`), the cargo scenario
  (`SCCargoScenario*`) → `game/Source/UnrealCitizen`.
- `SCBridgeEditor` (the cook library) → `game/Source/UnrealCitizenEditor`, the prep step.

What the kept features borrowed from those areas moves into the shared plugin under neutral names first:

- the HUD design tokens and paint helpers (`SCMobiGlasStyle`, `SSCMobiGlasWidgets`) that the interaction popout and
  the world callouts use;
- the viewer-world helper (`SCCargoUI::ViewerWorld`);
- the door names (`ASCVehicle::ShipDoorLabel`, from `SCShipLinkDoors.cpp`) the popout titles with.

Where the shared code calls into game features today (the viewer subsystem's music, cargo and garage state; the RPC
routes for cargo, Ship Link and Garage; the first-person garage-kiosk prompt), it gets an extension point the game
module registers with (an RPC route registry, a prompt provider), not a dependency on the game.

The runtime importer becomes `SCBridgeImport` (FFI client, package reader/source, import job, processed caches,
`SCBridgeLib`), loaded by the viewer and by the game's editor module only.

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

- `game/UnrealCitizen.uproject`: loads `plugins/SCBridge` (without `SCBridgeImport` at runtime) and its own modules.
- **Scenario definition**: an environment, a vehicle (and its skin), the player start, cargo setup and music —
  what `-SCCargoScenario=` and the cooked showcase do today, as data the game runs.
- **Prep step** (`UnrealCitizenEditor -run=SCPrepare -Scenario=<name>`): imports the scenario's environment and
  vehicle from p4k into the project as assets (today's `cook_ships.py` / `cook_hangars.py` / `cook_audio.py`).
  The game refuses to start a scenario whose prepared assets are missing and says to run prep.
- The game is cooked and packaged like any Unreal project (`package_showcase.ps1` moves here). Cooking and
  packaging still happen only on the owner's request.

## Verification

Each stage: `scripts/build_ffi.ps1`, the editor build of each project that exists, the SCBridge automation tests
(baseline 231/234), and for O1-3/O1-4 a launch through the RPC driver (C2 in the XL hangar: doors, panels, popout).
