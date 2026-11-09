# WP-R1 — Rust workspace + `scviewer-core`

**Agent:** Codex. **Depends on:** nothing. **Build lock:** required.

## Goal
Create the Rust workspace for this repo and a `scviewer-core` crate that wraps the
vendored StarBreaker crates behind the handful of operations the viewer and the UE
bridge need. No format code of our own.

## Deliverables
- Root `Cargo.toml` workspace: `members = ["crates/*"]`, `resolver = "2"`, the same
  dev-profile tuning as upstream (`opt-level = 1`, deps at 3), path deps on
  `vendor/StarBreaker/crates/*` declared in `[workspace.dependencies]`.
- `crates/scviewer-core/` with a `//!` header per file and this API (names final):
  - `Session::open(path: Option<&Path>) -> Result<Session>` — mmap `Data.p4k`
    (auto-discover via `starbreaker_common::discover::find_p4k` when `None`), load
    the DataCore, build the entity index once. Expose `p4k_path()`, `build_branch()`
    (from `build_manifest.id` next to the p4k if present).
  - `search_entities(&self, query, limit) -> Vec<EntityHit>` — `EntityClassDefinition`
    records by case-insensitive substring; `EntityHit { name, guid, geometry_path,
    category, display_name }` (display name from `Data/Localization/english/global.ini`
    like upstream `state.rs::parse_localization`).
  - `list_dir(&self, p4k_dir) -> Vec<DirEntry>` and `search_paths(&self, glob, limit)`
    for the raw geometry browser (`.cga/.cgf/.skin/.chr/.cdf`).
  - `export_entity_glb(&self, name, &GlbOptions) -> Result<Vec<u8>>` — wraps
    `starbreaker_3d::assemble_glb_with_loadout_with_progress`; `GlbOptions { lod,
    mip, materials: Textures|Colors|None, attachments, interior, lights, shields,
    apply_default_pose }`; progress via a callback.
  - `geometry_file_glb(&self, p4k_path) -> Result<Vec<u8>>` — wraps `skin_to_glb`
    with the companion-file resolution upstream does in `commands.rs::preview_geometry`.
  - `export_entity_decomposed(&self, name, out_root, &DecomposedOptions) -> Result<ExportResult>`
    — wraps the decomposed `ExportKind` with animations enabled.
  - `scviewer-core` must not depend on Tauri.
- `crates/scviewer-cli/` (binary `scviewer`): `search <q>`, `glb <entity> <out>`,
  `decomposed <entity> <out_root>`, `ls <p4k dir>` — thin, for verification.

## Done criteria
- `cargo build && cargo test --workspace` green (debug).
- `cargo run -p scviewer-cli -- search gladius` lists `AEGS_Gladius` first.
- `cargo run -p scviewer-cli -- glb Gladius out/gladius.glb` then
  `python3 -I scripts/glbinfo.py out/gladius.glb` shows ≥ 200 meshes, ≥ 300 images.
- `cargo run -p scviewer-cli -- decomposed Gladius out/dec` writes
  `out/dec/Packages/*/scene.json` with a non-empty `root_entity.animations`.
- Unit tests that need game data skip cleanly when no p4k is found (copy upstream's
  `test_p4k.rs` pattern).
