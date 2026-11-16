# SC_UE_ModelViewer

An **Unreal Engine 5.9 viewer for Star Citizen game data**. Point it at a `Data.p4k` and it lists the entities inside
and exports the chosen one in memory through Rust. It then builds real UE objects at run time and shows them under Nanite, Lumen
and MegaLights:
- Nanite meshes with Lumen cards, distance fields and ray-tracing geometry;
- skeletal meshes and animation sequences;
- Substrate materials and lights.

You can then explore ships in first person or move cargo with drones. The app runs as a game-mode application (the editor binary
launched with `-game`); the Unreal Editor is a development host, not the deliverable.

The archive formats are read by the Rust crates of [StarBreaker](https://github.com/diogotr7/StarBreaker) (MIT),
vendored as a git submodule at `vendor/StarBreaker` (our patches live on `scviewer/<topic>` branches of the
[public fork](https://github.com/baadc0de/StarBreaker) and go upstream from there).

## What it does

- **Model browser.** Search the archive, pick level of detail and texture mip, load. The model comes through the FFI
  library (`scbridge-ffi`) as a decomposed package (geometry, material semantics, lights, hierarchy, clips). The
  runtime importer builds it step by step within a frame budget. Orbit and free-fly cameras, a hierarchy panel,
  focus and framing.
- **Game-derived appearance.**
  - Materials follow the game's shader families: HardSurface, Layered, Illum, glass, decals, stencils, paints from
    the tint system, dirt.
  - Lights use the game's photometry and room confinement.
  - Exposure and the display curve are taken from the installed game's renderer
    (`StarEngineRender.json`, checked by `scripts/verify_star_engine_render.py`).
  - Nothing is tuned per asset (plan D11). The look target is a modern path-traced sci-fi look with GI on (D15).
- **Environments.** Game hangars and other object containers (`.socpak`) load as levels around the ship, either live
  from the archive or as cooked levels (WP-C1). Ships come out of the hangar elevator, and their doors open from
  their pads and on approach.
- **Animations.** A clip catalogue (`chrparams`, DBA, Mannequin) with play, snap, loop and compose on the vehicle
  rig: landing gear, canopies, wings, doors.
- **First person.**
  - Walk ships with collision against the game's own colliders.
  - Use doors and ramps through their animation fragments; teleport, torch, noclip.
  - The armed player body is exported from a game loadout: blend-space locomotion, weapons that fire, aim down
    sights, third person.
- **Cargo by drones.** Cargo grids, boxes and tractor-beam values come from the package and DataCore:
  - Drones lift boxes physically along the grid normal and fly them on swept, obstacle-aware routes. They work in
    teams sized by beam force and volume (WP-K1).
  - Transfers are ordered from an in-game mobiGlas Cargo app.
  - Failed loads fall back to the nearest free slot or to loose cargo in named beacons (WP-K2).
  - Scenarios run in hangars or in open space among asteroids at zero g (WP-K3). Several vehicle instances and
    owners can share a level.
- **Scripted and exploratory testing.** `-SCScript` routes with `-SCShotDir` screenshots. A localhost RPC server
  (`-SCRpc`) drives the running app: load, spawn, cargo, camera, screenshots, MP4 recording. An NDJSON state log
  reports positions, forces, idle and settled events, and anomalies. `explore.ps1` makes seeded random scene changes
  and checks stability after each (WP-H1).
- **Local cache.** Exports and the entity index are cached under `./cache/` of the working directory: a size cap,
  keyed by archive build, entity and options. A repeat load skips the export.

## Platforms and builds

| Target | Engine / RHI | Notes |
| --- | --- | --- |
| Linux (development host) | UE 5.9.0 preview-1 at `~/UnrealEngine/5.9`, Vulkan SM6 | Reads `Data.p4k` from the LUG Wine prefix; engine shader patches in `ue/EnginePatches` (plan D14). |
| Windows | UE 5.9 from the Epic launcher, D3D12 SM6 | Optional NVIDIA DLSS (`scripts/install_dlss.ps1`, DLAA by default, TSR without it). |
| Win64 showcase | Shipping package | One cooked ship and hangar at maximum Lumen quality with hardware ray tracing and DLAA, started as a player (`ue/scripts/package_showcase.ps1 -Platform Win64`). |
| Web | Wasm / WebGPU (mobile renderer) | One cooked ship and hangar, optional baked lighting; no archive and no Rust library in the browser (`SC_WITH_FFI=0`; `ue/scripts/package_web.ps1`). |

## Quick start

Star Citizen must be installed: `Data.p4k` is found in the standard RSI install locations. **Do not set
`SC_DATA_P4K`.** Rust (stable) and the engine for your platform are required.

```bash
git submodule update --init
```

Linux:

```bash
scripts/build_ffi.sh                                  # libscbridge.so + header into the plugin's ThirdParty module
uv run python ue/scripts/engine_patches.py apply      # once per engine install
ue/scripts/build.sh SCViewerEditor
"$HOME/UnrealEngine/5.9/Engine/Binaries/Linux/UnrealEditor" "$PWD/ue/SCViewer/SCViewer.uproject" \
  -game -windowed -resx=1600 -resy=900 -vulkan -ddc=SCViewerDDC
```

Windows (PowerShell):

```powershell
powershell -File scripts/build_ffi.ps1               # scbridge.dll + import library
& "C:\Program Files\Epic Games\UE_5.9\Engine\Build\BatchFiles\Build.bat" SCViewerEditor Win64 Development `
  "-Project=$PWD\ue\SCViewer\SCViewer.uproject" -WaitMutex
& "C:\Program Files\Epic Games\UE_5.9\Engine\Binaries\Win64\UnrealEditor.exe" "$PWD\ue\SCViewer\SCViewer.uproject" `
  -game -windowed -resx=1600 -resy=900
```

Start the viewer from the repository root, so its cache goes to `./cache/`. Useful switches:
- `-SCLoad=<entity>`;
- `-SCEnvironment=<socpak>`;
- `-SCCamera=first`;
- `-SCCargoScenario=hull_a_hangar` (or `c2_hangar`, `hull_a_multi`, `hull_a_space`);
- `-SCScript=ue/scripts/routes/<route> -SCShotDir=<dir>`;
- `-SCRpc`.

The full list, keys and console commands are in [ue/README.md](ue/README.md).

Without Unreal, the CLI exercises the same Rust core:

```bash
cargo run -p scviewer-cli -- search Gladius
cargo run -p scviewer-cli -- decomposed-tar Gladius out/gladius.tar
scripts/starbreaker.sh entity export Gladius out/gladius.glb --lod 1 --mip 2   # the upstream StarBreaker CLI
```

Driving the app from scripts (Windows):

```powershell
Import-Module ue/scripts/rpc/SCRpc.psm1
$p = Start-SCViewer -Extra '-SCCargoScenario=hull_a_space'   # off screen; -Visible for a window
Invoke-SC /wait @{ until = 'settled'; timeout_s = 900 }
Invoke-SC /screenshot @{ name = 'space' }
Stop-SCViewer $p
ue/scripts/rpc/explore.ps1 -Scenario hull_a_hangar -Steps 40 -Seed 1   # exploratory stability run
```

## Layout

```
crates/scviewer-core    Rust: session over p4k + DataCore, decomposed export (disk and in memory), clip catalogue, cache
crates/scviewer-cli     CLI over the core, for verification and exports
crates/scbridge-ffi     C-ABI library the Unreal plugin calls (scbridge.dll / libscbridge.so)
ue/SCViewer             UE 5.9 project: game-mode app, Viewer map, cargo scenarios (Config/Cargo/*.json)
ue/Plugins/SCBridge     Runtime module (FFI client, importer, vehicles, player, cargo, mobiGlas, RPC, UI),
                        optional Editor module (cooking, keep as assets), ThirdParty/SCBridgeLib
ue/EnginePatches        engine shader patches (applied by ue/scripts/engine_patches.py)
ue/scripts              builds, cooks, packaging, material generation, routes/, rpc/ (RPC module, explorer)
ue/verification         verification records and evidence per work package
scripts/                FFI and StarBreaker builds, DLSS install, export and verification helpers
docs/                   analysis, plan, UE design, cargo research, work-package briefs
vendor/StarBreaker      upstream toolkit (submodule, pinned)
out/, cache/            exports and the local cache (gitignored)
```

## Documentation

| Doc | Purpose |
| --- | --- |
| [docs/01-starbreaker-analysis.md](docs/01-starbreaker-analysis.md) | what StarBreaker provides and the gaps it left |
| [docs/02-implementation-plan.md](docs/02-implementation-plan.md) | architecture decisions (D1–D16), milestones, work packages and their status, orchestration rules |
| [docs/03-unreal-export-design.md](docs/03-unreal-export-design.md) | UE app design: hosts, view flow, object mapping, Nanite / Lumen rules, player modes, UI, FFI |
| [docs/work-packages/](docs/work-packages/) | one brief per work package (goal, behaviour, data, tests, verification) |
| [docs/cargo/](docs/cargo/) | cargo datamining and UI research |
| [ue/README.md](ue/README.md) | engine setup, every build / package / cook command, switches, console commands, logs |
| [AGENTS.md](AGENTS.md) | working rules for contributors and agents (branches, build locks, game-data rules) |

Open work is listed in the plan. It includes:
- the animation panel (WP-V2);
- the processed-data cache for meshes, collision and textures (WP-C1);
- collision that follows opening doors and ramps (WP-P4);
- the standalone launcher and settings UX (WP-S1);
- CI (WP-Q1).

## Rules for game data

Game-derived files (exports, textures, `.glb`, `.dds`, `.png`, `.mp4`, the archive itself) never enter git; they live
in `out/` and `cache/`. Game values are derived from DataCore and the archive, never hard-coded or keyed on asset names.

## License

This repository: MIT (see `LICENSE`). StarBreaker: MIT. Star Citizen game data is © Cloud Imperium Games and is never
committed here.
