# SC_UE_ModelViewer — agent instructions

Read with `docs/02-implementation-plan.md` (what we are building and in which
order) and `vendor/StarBreaker/AGENTS.md` (upstream engineering rules, which we
inherit for all Rust that touches the vendored crates).

## Layout

- `vendor/StarBreaker/` — upstream toolkit, **git submodule, pinned**. Our patches
  go on branches named `scviewer/<topic>` inside the submodule and are upstreamed
  (WP-D1). Never edit it without a branch; never commit `Co-Authored-By` trailers
  inside the submodule (upstream rule). The superproject follows this repo's rules.
- `crates/` — our Rust (`scviewer-core`, `scviewer-cli`, `scbridge-ffi`).
- `ue/SCViewer/` — the Unreal project (game-mode app, Viewer level);
  `ue/Plugins/SCBridge/` — Runtime module (FFI client, importer, actor, player, UI),
  optional Editor module (Keep as assets), `ThirdParty/SCBridgeLib`.
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
- Unreal: 5.9.0 preview-1 (CL 60351404) at `~/UnrealEngine/5.9`, Vulkan RHI; it replaced 5.8.2
  (plan D9); its Lumen World Cache is off for now (D13). Engine shader patches (D14) live in
  `ue/EnginePatches`: run `uv run python ue/scripts/engine_patches.py apply` after installing an engine. The deliverable is a standalone
  game-mode app (plan D2/D3): user-facing code goes in the Runtime module; editor-only
  dependencies stay behind `WITH_EDITOR` with a runtime path kept working.
- Win64: UE 5.9 from the Epic launcher, D3D12 SM6 (`ue/SCViewer/Config/Windows/WindowsEngine.ini`), FFI via
  `scripts/build_ffi.ps1`, NVIDIA DLSS / DLAA via `scripts/install_dlss.ps1` (gitignored download) — see
  `ue/README.md`.
- Web: the 5.9 Wasm / WebGPU build is a separate package of the viewer with one cooked hangar and one cooked ship
  (`ue/scripts/cook_hangars.py`, `cook_ships.py`, `package_web.ps1`; emsdk 6.0.10 via `WASM_EMSDK_ROOT`). No Data.p4k or
  FFI in the browser: Wasm builds compile with `SC_WITH_FFI=0`, so code that needs the archive must also work without it.

## Working rules

- Every WP = one branch `wp/<id>-<slug>` → PR → squash into `main`. Cross-review by
  the other model (Codex ⇄ Opus 5.5) before merge.
- Rust files start with a `//!` module header. Tests first for bug fixes.
- Verification commands and their output go in the PR description.
- Docs change in the same PR as the contract they describe.
- Commits in this repo end with the attribution trailer the harness prescribes;
  commits inside `vendor/StarBreaker` carry none.
- Use `uv run python` for project Python; `python3 -I` when reading untrusted/exported data.
