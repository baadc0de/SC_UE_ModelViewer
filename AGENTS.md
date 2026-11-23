# UnrealCitizen — agent instructions

Read with `docs/02-implementation-plan.md` (what we are building and in which
order) and `vendor/StarBreaker/AGENTS.md` (upstream engineering rules, which we
inherit for all Rust that touches the vendored crates).

## Layout

- `vendor/StarBreaker/` — upstream toolkit, **git submodule, pinned**. Our patches
  go on branches named `scviewer/<topic>` inside the submodule and are upstreamed
  (WP-D1). Never edit it without a branch; never commit `Co-Authored-By` trailers
  inside the submodule (upstream rule). The superproject follows this repo's rules.
- `crates/` — our Rust (`scviewer-core`, `scviewer-cli`, `scbridge-ffi`).
- The repository (GitHub `baadc0de/UnrealCitizen`, formerly `SC_UE_ModelViewer`) is being split into the
  StarBreaker core, the viewer and the UnrealCitizen game: `docs/work-packages/WP-O1-unrealcitizen-split.md`.
- `plugins/SCBridge/` — the shared Unreal plugin: Runtime module (FFI client, importer, ships and places, doors and
  panels, player, HUD, RPC harness, the viewer shell and its extension points `SCViewerShell.h` / `SCRpcExtension.h`),
  `SCBridgeFx` (effects build, editor), `ThirdParty/SCBridgeLib`. It never depends on game code.
- `viewer/` — the viewer Unreal project (`SCViewer.uproject`; game-mode app, Viewer level), its `scripts/`
  (RPC driver in `scripts/rpc/`, `Start-SCViewer -Game` launches UnrealCitizen) and `verification/` records.
- `game/` — the UnrealCitizen Unreal project (`UnrealCitizen.uproject`): module `UnrealCitizen` (cargo, mobiGlas,
  Ship Link, Garage, music, their RPC routes; registers them with the shell), editor module `UnrealCitizenEditor`
  (cooking, Keep as assets), `Config/Cargo`, `scripts/` (cooks, packaging). Build target `UnrealCitizenEditor`.
- `docs/` — analysis, plan, UE design, `work-packages/` briefs.
- `scripts/` — `starbreaker.sh` (build + run vendored CLI), `glbinfo.py`.
- `out/` — exports (gitignored). Scratch belongs in the session scratchpad.

## Game data

- Star Citizen LIVE `4.10.2` at
  `~/Games/star-citizen/drive_c/Program Files/Roberts Space Industries/StarCitizen/LIVE/Data.p4k` (156 GB).
  StarBreaker auto-discovers it. **Do not set `SC_DATA_P4K`.**
- Game-derived files (`.glb`, `.dds`, `.png` exports, `.p4k`) never enter git.
- Hard-coding a game value or branching on an asset name is a bug: derive from
  DataCore / P4k, fix the category (see upstream AGENTS.md "Coding Practices").

## Building

- One shared `CARGO_TARGET_DIR=./target` for our crates and the vendored CLI.
  **Cargo builds are sequential across agents** — never two at once. UE builds are a
  second lock (one UBT/editor build at a time); cargo and UE builds may overlap.
- Use `cargo build` (debug; opt-level 1 like upstream). Release only for shipping.
- Keep `target/` bounded: incremental compilation is off in the dev profile (its caches
  reached 15 GB); run `cargo sweep --maxsize 10GB .` when `target/` passes ~12 GB and
  delete `target/debug/incremental` if it reappears. Never a blind `cargo clean`.
  `/` is ~97 % full (~55 GB free); the UE project's `Saved/Import/*.tar` test tars
  are regenerable scratch.
- Unreal: **the SCViewer fork of UE 5.9, built from source, is the only engine** (plan D17): checkout
  `C:\Users\bojan\UnrealEngine-5.9-src`, branch `scviewer/5.9`, registered as `UE_5.9_src` (the `.uproject`'s
  `EngineAssociation`). Rendering rules that need engine changes are engine features on that branch (WP-E1), with UE's
  behaviour as the default. Scripts take `-Engine` / `$env:UE_ENGINE_ROOT` and default to `%USERPROFILE%\UnrealEngine-5.9-src`.
  The launcher (binary) engine, the Web / Wasm build, the NVIDIA RTX-fork (UE 5.8) build and the shader text patches of
  D14 (`ue/EnginePatches`, `engine_patches.py`) are gone: WP-E1 carries them as engine features (the SSDO is E1-7,
  `r.SSDO*` and the `SSDO*` post-process settings; `sc.Look.SSDO`). The Lumen World Cache is
  off for now (D13). The deliverable is a standalone game-mode app (plan D2/D3): user-facing code goes in the Runtime
  module; editor-only dependencies stay behind `WITH_EDITOR` with a runtime path kept working.
- Win64, D3D12 SM6 (`viewer/Config/Windows/WindowsEngine.ini`): build with
  `C:\Users\bojan\UnrealEngine-5.9-src\Engine\Build\BatchFiles\Build.bat SCViewerEditor Win64 Development -Project=<abs uproject> -WaitMutex`;
  FFI via `scripts/build_ffi.ps1` (every build links it); NVIDIA DLSS / DLAA via `scripts/install_dlss.ps1` (gitignored
  download, compiled against the fork) — see `viewer/README.md`. Standalone showcase: `game/scripts/package_showcase.ps1
  -Scenario <name>` (one scenario prepared by the prep step, `UnrealCitizenEditor -run=SCPrepare -Scenario=<name>`;
  Shipping, `-SCMaxQuality`). Preparing and packaging happen only on the owner's request.

## Working rules

- Every WP = one branch `wp/<id>-<slug>` → PR → squash into `main`. Cross-review by
  the other model (Codex ⇄ Opus 5.5) before merge.
- Rust files start with a `//!` module header. Tests first for bug fixes.
- Verification commands and their output go in the PR description.
- Docs change in the same PR as the contract they describe.
- Commits in this repo end with the attribution trailer the harness prescribes;
  commits inside `vendor/StarBreaker` carry none.
- Use `uv run python` for project Python; `python3 -I` when reading untrusted/exported data.
