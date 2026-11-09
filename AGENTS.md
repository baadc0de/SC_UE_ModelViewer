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
- `app/` — Tauri 2 + React + three.js viewer.
- `ue/` — Unreal test project and the `SCBridge` plugin.
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
  **Cargo builds are sequential across agents** — never two at once. UE and npm
  builds may run alongside cargo.
- Use `cargo build` (debug; opt-level 1 like upstream). Release only for shipping.
- Keep `target/` bounded with `cargo sweep --time 30`; never a blind `cargo clean`.
  `/` has ~84 GB free.
- Unreal: Linux editor, Vulkan RHI (not installed yet — WP-U0 does that).

## Working rules

- Every WP = one branch `wp/<id>-<slug>` → PR → squash into `main`. Cross-review by
  the other model (Codex ⇄ Opus 5.5) before merge.
- Rust files start with a `//!` module header. Tests first for bug fixes.
- Verification commands and their output go in the PR description.
- Docs change in the same PR as the contract they describe.
- Commits in this repo end with the attribution trailer the harness prescribes;
  commits inside `vendor/StarBreaker` carry none.
- Use `uv run python` for project Python; `python3 -I` when reading untrusted/exported data.
