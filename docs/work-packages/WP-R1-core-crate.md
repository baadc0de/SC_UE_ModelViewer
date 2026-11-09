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
  - `Session::open(path: Option<&Path>, cache_dir: Option<&Path>) -> Result<Session>` — mmap `Data.p4k`
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
  - `export_entity_decomposed_mem(&self, name, &DecomposedOptions) -> Result<Vec<u8>>`
    — same package as an **uncompressed tar** in memory (layout identical to the
    folder; `scene.json` first). Implement by giving the exporter a sink abstraction
    if upstream writes straight to disk (`DecomposedExport` / `ExportedFile`); patch
    upstream on a `scviewer/` branch if needed and note it for WP-D1.
  - Layer-1 cache (plan D10) when `cache_dir` is given (the CLI defaults it to
  `./cache/` relative to the current working directory; never a global/home dir): the entity index as
    `entities.<buildid>.bin` (build id from `build_manifest.id` beside the p4k, else
    size + mtime), and every decomposed export as `<key>.tar` where the key hashes
    build id + entity + options + a `CACHE_FORMAT_VERSION`. `manifest.json` records
    size and last use; `Session::cache_evict(cap_bytes)`; every result carries a
    `CacheStatus::{Hit, Miss}`. Never cache across build ids.
  - `scviewer-core` must not depend on any UI framework.
- `crates/scviewer-cli/` (binary `scviewer`): `search <q>`, `glb <entity> <out>`,
  `decomposed <entity> <out_root>`, `decomposed-tar <entity> <out.tar>`,
  `validate <dir>` (upstream `validate_decomposed_export`), `ls <p4k dir>` — thin,
  for verification.

## Findings you must build on (2026-11-09, orchestrator)
- **Upstream's GLB decomposed format writes empty mesh files.** The CLI forces
  `ExportFormat::Blend` whenever `--kind decomposed` is given (`cli/src/common.rs`
  `From<&ExportOpts>`, and `cli/src/entity.rs::export` short-circuits to
  `export_blend`). With `ExportFormat::Glb` the library runs the decomposed path but
  `decomposed.rs::write_mesh_asset` inserts `Vec::new()` for every `.glb` (the Blend
  format fills meshes in a later assembly step; nothing does that for GLB).
- A working prototype fix is in
  `docs/work-packages/wp-r1-prototype-glb-mesh-asset.patch` (against upstream
  `7d3f01e`): `write_mesh_asset` builds each mesh through `gltf::write_glb` with
  `MaterialMode::Colors`, no textures, `preserve_textureless_decal_primitives = true`,
  the NMC node table and the bone table, plus an env-var escape hatch in the CLI.
  Turn that into a proper `scviewer/decomposed-glb` branch in `vendor/StarBreaker`:
  no env var — add a real `--format glb|blend` CLI option (default blend to keep
  upstream behaviour) and make `scviewer-core` request `ExportFormat::Glb` directly.
  Verified output for `Gladius --lod 1 --mip 2`: 45 GLBs none empty, root mesh 274
  nodes / 89 meshes / 24 materials, 23 sidecars, 366 PNGs, 63 clips, 96 MB, ~10 s.
- **Material mapping contract** the UE importer relies on: a GLB material is named
  `<mtl stem>_<submaterial name>_<NN>` where `NN` is the zero-based index into the
  sidecar's `submaterials[]` (`<mtl stem>_TEX<m>.materials.json`); the GLB primitive's
  material index is that same `NN`. Keep this stable and state it in the crate docs.
- The library already returns the whole package in memory
  (`DecomposedExport { files: Vec<ExportedFile { relative_path, bytes, kind }> }`),
  so the in-memory tar is a serialisation of that vector, not a new exporter path.

## Done criteria
- `cargo build && cargo test --workspace` green (debug).
- `cargo run -p scviewer-cli -- search gladius` lists `AEGS_Gladius` first.
- `cargo run -p scviewer-cli -- glb Gladius out/gladius.glb` then
  `python3 -I scripts/glbinfo.py out/gladius.glb` shows ≥ 200 meshes, ≥ 300 images.
- `cargo run -p scviewer-cli -- decomposed Gladius out/dec` writes
  `out/dec/Packages/*/scene.json` with a non-empty `root_entity.animations`;
  `decomposed-tar Gladius out/dec.tar` extracts to a byte-identical tree.
- With the default `./cache/` (or `--cache <dir>`), the second `search` and the second `decomposed-tar` of the
  same entity report `cache: hit` and finish in < 200 ms.
- Unit tests that need game data skip cleanly when no p4k is found (copy upstream's
  `test_p4k.rs` pattern).
