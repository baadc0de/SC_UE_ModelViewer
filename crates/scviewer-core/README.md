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
  shortest entity name then name. Export resolves exact entity names first.
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

The binary browser index is `entities.<buildid>.bin`; unsafe build-id filenames
use a SHA-256 representation. Build identity comes from `Data.BuildId` in the
adjacent `build_manifest.id`, falling back to archive size and modification time.
Packages are `<sha256>.tar`, hashing cache format version, build id, canonical
entity name and all export options. Index envelopes also check version/build id.
`manifest.json` tracks size and last-use timestamp for both indexes and packages.
Publication is atomic; a process lock serializes cache reads, writes and eviction.
Writes enforce the default 20 GiB cap. Cache failures are reported, not silently
ignored. Existing files at identical output paths are replaced when unpacking;
unrelated files under a shared export root remain intact.

Tests discover game data and skip live checks when none is found. Run cargo from
the repository root with `CARGO_TARGET_DIR=./target`. Format only owned packages
(`cargo fmt -p scviewer-core -p scviewer-cli`); `--all` traverses the vendored
workspace as well.
