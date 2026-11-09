# scviewer-core

UI-independent wrapper around the pinned StarBreaker crates. No game-format
parsers are implemented here. `scviewer-cli` builds the `scviewer` verification
binary; its default cache is `./cache/` relative to the current working directory.

## Public API

- `Session::open(Option<&Path>, Option<&Path>) -> anyhow::Result<Session>`;
  `None` archive discovers the install, `None` cache disables persistence.
- `p4k_path()`, `build_id()`, `build_branch() -> Option<&str>`.
- `search_entities(&str, usize) -> Vec<EntityHit>`; hits contain `name`, `guid`,
  optional `geometry_path`, `category`, and optional English `display_name`.
  Search matches case-insensitive entity or display-name substrings, ordered by
  shortest entity name then name. Core export accepts exact entity names (case-insensitive); only the CLI resolves substring shorthand.
- `list_dir(&str) -> Vec<DirEntry>` and `search_paths(&str, usize)`;
  entries contain `path`, `is_dir`, and uncompressed `size`. The geometry browser
  includes `.cga/.cgf/.skin/.chr/.cdf` and their parent directories. Paths use `/`;
  input paths accept either separator. Glob matching is case-insensitive.
- `export_entity_glb(&str, &GlbOptions) -> Result<Vec<u8>>` and
  `geometry_file_glb(&str) -> Result<Vec<u8>>`. Raw previews use upstream's
  `skin_to_glb`, with `.skinm/.cgfm/.cgam` companion resolution.
- `export_entity_decomposed(&str, &Path, &DecomposedOptions) -> Result<ExportResult>`;
  result contains written `files: Vec<PathBuf>` and `cache_status`.
- `export_entity_decomposed_mem(&str, &DecomposedOptions) -> Result<Vec<u8>>`;
  uncompressed tar, `Packages/.../scene.json` first, remaining files sorted by path.
  Regular files have explicit typeflag `0`. Paths use POSIX ustar `name` (100 bytes)
  and `prefix` (155 bytes), split at a slash when required. Only paths that cannot
  fit those fields use a per-file PAX `x` header with a UTF-8 `path` key followed
  by a regular file header. No GNU `L`/`@LongLink` entries are emitted. The UE
  reader must support exactly ustar name/prefix and per-file PAX path overrides.
  Unpacking does not preserve the archived zero mtimes.
- `set_progress_callback(Option<ProgressCallback>)`; callback is an
  `Arc<dyn Fn(f32, &str) + Send + Sync>`. Upstream progress is sampled every 20 ms
  on a scoped thread during assembly; final/cache-hit notifications run on the
  calling thread. Callbacks must not panic or block indefinitely.
- `cache_status() -> CacheStatus` describes the latest successful operation.
  `_cached` companions return `Cached<T> { value, cache_status }`, retaining the
  exact `Vec` signatures requested by WP-R1. Uncached operations report `Miss`.
- `cache_evict(u64) -> Result<u64>` removes least recently used files and returns
  bytes removed. `validate_decomposed_export` and `ValidationReport` are re-exported.

`GlbOptions` exposes `lod`, `mip`, `materials: Materials::{Textures,Colors,None}`,
`attachments`, `interior`, `lights`, `shields`, `apply_default_pose`.
`DecomposedOptions { geometry: GlbOptions }` always enables animation sidecars;
its default disables pose baking. Other defaults follow upstream (LOD 1, mip 2).

A cold session reads the archive, parses DataCore and builds the entity index
once. A disk index hit defers archive/DataCore work until an uncached export or
raw geometry preview. This is required for sub-200 ms process-start cache hits.
Upstream's `MappedP4k` now uses pooled positional file reads rather than mmap;
we reuse that implementation. A session can be moved to a worker but is not Sync
because upstream's entity index contains RefCell. Export progress callbacks are
thread-safe; the session's operations must be serialized by its owner.

## Package contract and cache

Decomposed meshes use actual GLB geometry, NMC nodes and bones, color materials,
and preserved textureless decal primitives. PNGs and material/animation sidecars
come directly from StarBreaker. A material is named
`<mtl stem>_<submaterial name>_<NN>` with a minimum two-digit decimal suffix.
GLB material index `i` equals the array position `submaterials[i]` in the shared
`<mtl stem>_TEX<m>.materials.json`. It is **not** the sidecar entry's `index` field,
which identifies the original source slot before hidden-material filtering.
Unused slots through the highest referenced sidecar position are retained as
material entries without primitives; trailing unused slots are omitted.

Gladius at LOD 1 / mip 2 has **45 non-empty GLBs, 23 material sidecars, 366 PNGs,
63 clips**. The root has **274 nodes, 89 meshes, 27 GLB materials**; its sidecar
has 28 slots and its highest referenced slot is 26. The prototype compacted the
material table and did not satisfy direct sidecar indexing.

The binary browser index is `entities.v<N>.<buildid>.bin`; unsafe build-id filenames
use a SHA-256 representation. Build identity comes from `Data.BuildId` in the
adjacent `build_manifest.id`, falling back to archive size and modification time.
Packages are `<sha256>.tar`, hashing cache format version, build id, canonical
entity name and all export options, plus `EXPORTER_VERSION`. Index envelopes
also check version/build id/exporter identity. `build.rs` embeds the vendored git
revision and dirty flag in `EXPORTER_REVISION`, `EXPORTER_DIRTY`, and
`EXPORTER_VERSION` via compile-time environment constants. It watches vendored
source and git metadata for changes. Without git, the identity falls back to a
SHA-256 hash of the vendored Cargo.lock.
`manifest.json` tracks size, SHA-256 checksum and last-use timestamp for both indexes and packages.
Publication is atomic; a process lock serializes cache reads, writes and eviction.
Writes enforce the default 20 GiB cap. Corrupt/outdated entries are removed and rebuilt as misses. Malformed manifests
are reconstructed from cache-owned files; orphan `.tmp*` files are removed.
Files are synced before atomic publication. Filesystem access failures are reported. Existing files at identical output paths are replaced when unpacking;
unrelated files under a shared export root remain intact.

Tests discover game data and skip live checks when none is found. Run cargo from
the repository root with `CARGO_TARGET_DIR=./target`. Format only owned packages
(`cargo fmt -p scviewer-core -p scviewer-cli`); `--all` traverses the vendored
workspace as well.

## Clip catalogue (WP-R3)

`Session::clip_catalogue(&str) -> Result<Catalogue>` lists the default decomposed
package's animation IDs before exporting meshes or textures.
`clip_catalogue_cached(&str) -> Result<Cached<Catalogue>>` returns the same value
with `CacheStatus`; `cache_status()` also records the result. Names must match an
entity exactly, ignoring case. The CLI accepts substring shorthand:

```bash
CARGO_TARGET_DIR=./target cargo run -p scviewer-cli -- clips Scorpius
```

The command prints JSON and reports clip/skeleton counts, catalogue-call timing,
cache status and total process timing on stderr. `--cache` and `--no-cache` apply.

`Catalogue { entity, skeletons, clips }` and its member types are re-exported:

- `SkeletonInfo { owner_entity, chr_path, chrparams_path: Option<String>,
  dba_path: Option<String>, bone_count }`. `chr_path` is the source rig path,
  including rigid CGA/NMC rigs; optional paths are absent when undiscovered.
- `ClipInfo { id, display_name, owner_entity, contributors: Vec<String>, source: ClipSource, fps: u16,
  frame_count: u32, duration_s: f32, has_position, has_rotation, bone_count,
  fragments: Vec<Fragment>, boomerang }`.
- `ClipSource::{Chrparams,DbaOnly,Caf}` distinguishes named DBA events, remaining
  DBA metadata entries, and standalone CAFs. `Fragment` holds `fragment`, `tags`,
  `frag_tags`, `scopes`.

IDs use precisely the export's `root_entity.animations[].name` convention.
Chrparams names containing `*` or `?` are mapping rules, not playable clip names,
and are omitted from the event map. Concrete DBA entries remain discoverable.
The exporter and catalogue share the ordered rig-source list, record-based
geometry fallback, namespace rules and clip merger. Broken rig geometry or
animation data produces a warning and is skipped; healthy rigs still contribute.
Unknown position encodings use the full decoder's zero-value fallback without
removing a DBA block; CAF FPS zero uses the missing-AnimInfo default of 30.
Repeated names merge across rigs; `owner_entity` is the first contributing owner
in export discovery order (root, landing gear, loadout children); landing-gear
owners preserve the exporter's filename including its extension. `contributors`
lists every contributing owner and resolved rig node name, without changing the
merged clip ID. It defaults to empty when deserializing older JSON. `bone_count`
counts merged source-rig/bone channel variants. Frame count is the final key time
rounded up, matching the index record, and duration is `frame_count / fps`.
Boomerang means all available position/rotation endpoints agree (quaternion sign
is immaterial), including constant tracks. No intermediate sample values are
decoded: upstream shares the full export's name matcher and reads controller
headers, time tables and first/final values only. This preserves its motion
ranking while avoiding full keyframe decompression.

Interior keys and labels use the export helpers. The existing exporter only
namespaces children when the package has **no root skeleton**, as in standalone
socpak exports. A Cutlass ship package has a root rig and keeps merged names;
changing its catalogue alone would break the importer join. The Session entity
API does not expose standalone socpak exports.

Catalogue files are `clips.<sha256>.bin`, independently keyed by catalogue schema (now v2),
layer-1 format version, build ID, canonical entity and exporter identity. An
identity-bearing envelope, checksum verification, atomic writes, manifest
recovery and LRU eviction protect them like packages. A hit skips archive,
DataCore, rig and animation reads. Corrupt entries rebuild as misses; cache I/O
errors are reported. With persistence disabled each call recomputes and reports
`Miss`. Entities without discoverable animation clips return an empty clip list.

Live tests discover an entity without a geometry component from DataCore instead
of relying on a build-specific name. Catalogue timings are always reported;
set `SCVIEWER_PERF_TESTS=1` to enforce the 500 ms warm-call budget.

## Lights (WP-R5)

`Session::lights(&str) -> Result<LightCatalogue>` returns authored vehicle and
interior lights without exporting geometry or textures. Exact entity names are
required; `scviewer lights Gladius` accepts the usual CLI shorthand and prints
JSON. This operation reports `CacheStatus::Miss`; package caching remains
available through the decomposed export API. Cache format v5 invalidates packages created before the final emission tint/strength
rule, including earlier dirty exporter builds. Scene/material schemas remain v2;
the validator accepts v1 and v2. Missing vehicle XML warns and leaves that source
empty, so geometry and light discovery can continue.

`LightCatalogue { entity, ports, lights }`, `VehicleLightPort`, `EntityLight`,
`LightInfo`, `LightAttachment` and `LightStateInfo` are re-exported. `ports` describes
vehicle XML MultiLight attachment points. Each `EntityLight` has its owning entity,
source container and source indices, source container parent, the standard light
fields, and an optional resolved NMC node with parent-local position/rotation.
Flat positions are owner-entity-local CryEngine Z-up metres; directions are unit
vectors, rotations are `[w,x,y,z]`. The catalogue keeps source projector paths;
packages replace those with exported texture paths.

The implementation follows DataCore object-container references and the resolved
loadout tree, parsing authored SOC `LightComponentParams` and group metadata.
LIVE 4.10.2's Gladius MultiLight port has no equipped item: its actual light groups
are authored in SOC, including their helper names and baked light states. No item
is inferred from a ship or port name. Empty groups produce no synthetic light.
In schema v2, resolved attached groups occur once in `root_entity.lights` or
`children[].lights`; other lights remain in `interiors[].lights`. Catalogue and
export share discovery and transform resolution, and live tests join both by
`source_container_index` / `source_light_index`.
