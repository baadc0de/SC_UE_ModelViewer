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
  nodes / 89 meshes / 27 materials (28 sidecar slots; highest referenced slot 26), 23 sidecars, 366 PNGs, 63 clips, 96 MB, ~10 s.
- **Material mapping contract** (decided 2026-11-09 after Codex found the prototype's
  GLB material list was compacted to the 24 slots the Gladius root uses out of the
  sidecar's 28, so `_NN` was a compact position, not the sidecar index): **preserve
  direct sidecar indices.** A decomposed mesh GLB's `materials[]` has one entry per
  source `.mtl` slot up to the highest slot the mesh references (unused slots are
  plain entries with no primitives); the GLB primitive's `material` index therefore
  equals the zero-based index into the sidecar's `submaterials[]`, and the material
  name is `<mtl stem>_<submaterial name>_<NN>` with that same index. Document the
  corrected Gladius root count (27 materials for 28 sidecar slots, highest referenced
  slot 26) wherever the earlier "24" appears, and add a unit test that checks
  `materials[i].name` ends in `_{i:02}` and that `i` indexes the sidecar's
  `submaterials[]` for the Gladius root mesh.
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

## WP-R1 implementation handoff (2026-11-09)

Implemented `crates/scviewer-core` and the `scviewer` binary in
`crates/scviewer-cli`. Public API details and the material contract are documented
in `crates/scviewer-core/README.md`. All requested Vec-returning methods retain
those signatures; `Session::cache_status()` and `_cached` companions expose
`CacheStatus` without changing them. `ExportResult` carries status directly.
`DecomposedOptions` contains `geometry: GlbOptions` and always includes animations.

Necessary implementation clarifications:

- Upstream `MappedP4k` uses pooled positional reads, not mmap. The wrapper reuses
  that implementation rather than adding archive-format code.
- Cold `open` loads DataCore and builds the index once. A persisted index hit
  defers archive/DataCore initialization until an uncached export or geometry
  preview, allowing process-start cache hits below 200 ms.
- The binary browser index includes geometry paths and parent directories. The
  default local cache is enforced by the CLI; passing `None` to core disables it.
  Cache publication is atomic and process-locked; writes apply a 20 GiB LRU cap.
- Beyond the supplied prototype, native Blend assembly had to be bypassed for
  GLB packages: it otherwise regenerated mesh files as compressed Blend data
  under `.glb` names. GLB material slots also required sidecar-order restoration,
  retention of unused gaps, and the specified two-digit naming convention.
- Upstream validation only accepted Blend exports. It now accepts GLB scene
  manifests, validates GLB payloads, and rejects empty GLB files.
- The sandbox prevented commits/branches as anticipated; no `.git` writes were
  attempted. The orchestrator must branch, commit and pin the submodule.
- Cargo dependency downloads required a writable `CARGO_HOME` copy in `/tmp`;
  build outputs remained in the repository's shared `./target` (6.3 GiB).

### Verification commands and key output

Commands were run from the repository root. Every cargo invocation below used
this environment prefix (including formatting and upstream tests):

```sh
CARGO_HOME=/tmp/wp-r1-cargo-home CARGO_TARGET_DIR=./target
```

```sh
cargo build
cargo test --workspace
cargo test --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d --test decomposed_glb
cargo test --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d --lib gltf::
cargo test --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d --lib validation::
cargo test --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker common::tests
cargo fmt -p scviewer-core -p scviewer-cli -- --check
```

Results: build passed; workspace **7 passed**, live decomposed regression **1
passed**, GLB tests **43 passed**, validation tests **29 passed**, CLI common tests
**15 passed**, formatting passed. Live tests skip if discovery finds no archive.
The empty-GLB regression was observed failing before the mesh fix; material-name
and validator regressions were likewise observed failing before their fixes.

```sh
cargo run -p scviewer-cli -- search gladius
cargo run -p scviewer-cli -- search gladius
cargo run -p scviewer-cli -- glb Gladius out/gladius.glb
python3 -I scripts/glbinfo.py out/gladius.glb
cargo run -p scviewer-cli -- decomposed Gladius out/dec --lod 1 --mip 2
cargo run -p scviewer-cli -- decomposed-tar Gladius out/dec.tar --lod 1 --mip 2
cargo run -p scviewer-cli -- decomposed-tar Gladius out/dec.tar --lod 1 --mip 2
python3 -I scripts/verify_r1_package.py out/dec out/dec.tar
cargo run -p scviewer-cli -- validate out/dec
cargo run -p scviewer-cli -- ls Data/Objects/Spaceships/Ships/AEGS/Gladius
```

Search lists `AEGS_Gladius` / `Aegis Gladius` first. Bundled GLB: **67,189,044
bytes**, 1,072 nodes, **220 meshes, 334 images**, 184 materials, 42 skins; export
4,144.0 ms. Decomposed cold export: **6,919.9 ms**, **502 files**. Tar:
**101,784,064 bytes**, `scene.json` first, all **502 entries byte-identical** to
the folder. An older folder containing a stale `scene.blend` was preserved as
`out/dec-before-final-r1` before repeating the final clean-folder check.

Package counts: **45 non-empty GLBs / 23 material sidecars / 366 PNGs / 63 clip
sidecars**. Root: **274 nodes / 89 meshes / 27 materials**, **28 sidecar slots**,
highest referenced slot **26**, 63 root animations. Validator: `is_valid: true`,
`mesh_count: 45`, `errors: []`, `warnings: []`.

Process timing used Bash's builtin `time` (same cargo environment prefix):

```sh
TIMEFORMAT='wall: %3R s'
time cargo run -p scviewer-cli -- search gladius
time cargo run -p scviewer-cli -- decomposed-tar Gladius out/dec.tar --lod 1 --mip 2
```

Search: **cache: hit (29.8 ms)**, whole Cargo command **0.075 s**.
Tar: **cache: hit (77.2 ms)**, whole Cargo command **0.123 s**.

```sh
cargo run --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker -- entity export Gladius out/wp-r1-upstream --kind decomposed --format glb --lod 1 --mip 2
blender --background --factory-startup --python /tmp/wp-r1-blender.py
```

Real upstream CLI `--format glb` export: **502 files**. The temporary Blender
script imported the root GLB with `bpy.ops.import_scene.gltf`: **274 objects,
89 meshes**, 24 instantiated materials (only slots actually used by primitives).
The GLB itself retains the required 27 slots.

Additional reproducibility probe:

```sh
cargo run -p scviewer-cli -- --no-cache decomposed-tar Gladius out/dec-uncached.tar --lod 1 --mip 2
python3 -I scripts/verify_r1_package.py out/dec out/dec-uncached.tar
```

The independent uncached export took **6,818.0 ms**. Its parity probe found exactly
one differing file: `Data/UI/Generated/.export_stamp.json`, whose upstream build
and export timestamps intentionally change. The regular cached folder/tar parity
check above passes for every file.

### Handoff items

- TODO(WP-R1): Orchestrator to create `scviewer/decomposed-glb`, commit/pin the
  submodule and arrange the required cross-review/PR; this sandbox cannot write git metadata.
- TODO(WP-R1): If independent cache-disabled exports must be byte-reproducible,
  agree on upstream diagnostic `.export_stamp.json` handling. Cached package
  replay is already byte-identical; the stamp is currently preserved verbatim.
- TODO(WP-R1): Carry upstream missing skin-joint/bind-pose fallback warnings from
  the real CLI export into the rig/animation verification work (WP-U3).

## Cross-review corrections (WP-R1c, 2026-11-09)

Applied the review against superproject `df22acc` and submodule
`scviewer/decomposed-glb` at `a3304713`; changes remain uncommitted for the
orchestrator.

1. Cache identity now embeds the vendored revision and dirty flag, exposed as
   `EXPORTER_REVISION`, `EXPORTER_DIRTY`, and `EXPORTER_VERSION`. The build script
   watches vendor sources and git metadata. Git-unavailable fallback was exercised
   directly and matched the SHA-256 of the vendored Cargo.lock. Both package keys
   and index headers include exporter identity.
2. Cache format is now v3; index filenames are `entities.v3.<buildid>.bin`.
   Invalid/outdated index envelopes are removed and rebuilt as misses. Manifest
   corruption triggers reconstruction from cache-owned files. Package reads check
   recorded length and SHA-256 plus tar structure; truncated or same-size damaged
   entries are misses. Atomic writes sync before publication; orphan `.tmp*` files
   are removed under the cache lock. Filesystem access errors still surface.
3. Tar files use explicit Regular typeflag `0`, POSIX ustar name/prefix splitting,
   and per-file PAX `x`/`path` only for unsplittable paths. No GNU LongLink entries.
   The minimal-reader live test checks all Gladius entries without GNU/PAX support;
   a separate synthetic test covers the PAX fallback. Extraction disables mtime
   preservation. The contract is documented in the crate README and design §FFI.
4. The upstream live regression now checks material-name/sidecar/index bounds and
   highest-referenced-slot invariants, without patch-specific counts. Exact counts
   remain in the labelled LIVE 4.10.2 script fixture, including the unchanged
   bundled material baseline.
5. `GlbOptions.preserve_material_slots` is explicit: true in the decomposed
   writer, false in bundled/raw/character callers. The claim that every production
   caller's metadata said Bundled was false: the decomposed writer already said
   Decomposed. Tests now prove slot behavior is independent of that label, and the
   real bundled Gladius baseline stayed at 184 materials. The upstream contract
   documents GLB package dispatch and the Blend-only `__child` rewrite.

Also applied the low-severity fixes: core export requires an exact name (case
insensitive); CLI shorthand retains exact-first substring resolution. A bad
per-record localization query/value warns and omits its display name rather than
aborting the index. Removed the CLI's unused serde_json dependency and added the
requested category-classification TODO.

### Review verification

All cargo commands below ran at the repository root with
`CARGO_HOME=/tmp/wp-r1-cargo-home CARGO_TARGET_DIR=./target`.

```sh
cargo build
cargo test --workspace
cargo test --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d --lib gltf::
cargo test --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d --test decomposed_glb
cargo test --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d --lib validation::
cargo test --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker common::tests
cargo fmt -p scviewer-core -p scviewer-cli -- --check
```

Passed: build, **15 core tests**, **44 GLB tests**, **1 live export test**,
**29 validation tests**, **15 CLI tests**, formatting and diff whitespace checks.
The pre-fix run reproduced five failures covering corrupt index/manifest,
truncated tar acceptance and non-regular/non-ustar headers.

```sh
cargo run -p scviewer-cli -- decomposed-tar Gladius out/dec-r1c.tar --lod 1 --mip 2
cp out/dec-r1c.tar out/dec-r1c-first.tar
cargo run -p scviewer-cli -- decomposed-tar Gladius out/dec-r1c.tar --lod 1 --mip 2
cmp out/dec-r1c-first.tar out/dec-r1c.tar
```

Tar size **101,727,744 bytes**, cold **cache: miss (8548.7 ms)**, second
**cache: hit (133.5 ms)**. `cmp` passed. The size reduction is from replacing GNU
long-name records with ustar prefix splitting.

```sh
python3 -I /tmp/wp-r1c-truncate.py
cargo run -p scviewer-cli -- decomposed-tar Gladius out/dec-r1c-rebuilt.tar --lod 1 --mip 2
cargo run -p scviewer-cli -- decomposed-tar Gladius out/dec-r1c-rebuilt-hit.tar --lod 1 --mip 2
cmp out/dec-r1c-rebuilt.tar out/dec-r1c-rebuilt-hit.tar
cargo run -p scviewer-cli -- decomposed Gladius out/dec-r1c --lod 1 --mip 2
cargo run -p scviewer-cli -- --no-cache glb Gladius out/gladius-r1c-after.glb
python3 -I scripts/verify_r1_package.py out/dec-r1c out/dec-r1c-rebuilt.tar out/gladius-r1c-after.glb
cargo run -p scviewer-cli -- validate out/dec-r1c
```

The truncation script selected the single cache tar whose manifest checksum
matched `out/dec-r1c.tar`, then truncated it to **4,096 bytes** without modifying
the manifest. The next call reported **cache: miss (8007.1 ms)** and restored
**101,727,744 bytes**; the following call reported **cache: hit (112.8 ms)**.
The rebuilt and hit files compared byte-identically.

Fixture check: **45 non-empty GLBs / 23 sidecars / 366 PNGs / 63 clips**, root
**274/89/27**, all **502 entries** identical to the folder. Bundled Gladius stayed
at **184 materials** before and after the explicit-option refactor (220 meshes,
334 images). Validation reported **45 mesh files**, no errors or warnings.

TODO(WP-R1): Replace inherited path-substring categories with DataCore type metadata.
TODO(WP-R1): Orchestrator to commit these review corrections in the existing branches.
The earlier diagnostic-stamp reproducibility and skin-joint followups remain unchanged.
