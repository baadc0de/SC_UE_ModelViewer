# WP-C1 — Processed-data cache (layer 2)

**Agent:** Codex. **Depends on:** WP-U0 Spike D (round-trip findings), WP-U1
(derive/instantiate split, mesh hashes); extends to animation data after WP-U3.
**Build lock:** UE lock.

## Goal
Loading a model the second time performs no processing: no StarBreaker export
(layer 1, Rust — WP-R1), no Nanite build, no collision cook, no texture build. The
actor is reconstructed from cached blobs in a few seconds.

## Read first
`docs/03-unreal-export-design.md` §Local file cache; `ue/README.md` Decision section
(which serialisation path round-tripped in the chosen host).

## Deliverables
- `FSCProcessedCache` (Runtime module): root `./cache/processed/` relative to the
  working directory the app was launched from (never a global/home location;
  `--cache <dir>` and settings override), `manifest.json` (entries, sizes, last use, format version),
  size cap (default 20 GB) with LRU eviction, `Clear()`, `Stats()`.
- Keys: per **mesh** = hash of the GLB bytes + import options that affect geometry
  (Nanite settings, collision mode) + format version; per **texture** = hash of PNG
  bytes + role flags; per **entity load** = archive build id + entity + full options
  → an index blob listing the mesh/texture/anim keys plus the assembled scene
  (`scene.json` copy, skeleton, clip catalogue, anim raw data).
- Serialisation via the engine's runtime `Serialize` paths through
  `FMemoryWriter`/`FMemoryReader` with a name/object proxy archive (or `.uasset`
  packages under a mounted `/SCCache/` root if WP-U0 showed only that works in the
  chosen host). Blobs are written atomically (temp + rename) and validated by a
  trailing checksum; a corrupt blob is a miss, never a crash.
- Integration: the importer checks the entity key first; on a full hit it
  instantiates every object from blobs and assembles the actor; on a partial hit it
  processes only the missing meshes/textures and writes them back. The UI progress
  bar shows "from cache" phases; the footer shows hit/miss per layer and cache size.
- Settings UI: cache path, cap, Clear cache (with size shown), "Rebuild this model"
  (ignores cache once).
- Concurrency: single app instance assumed, but writes are atomic so a crash never
  leaves a half entry; a lock file guards manifest updates.

## Done criteria
- Gladius: first load N s; second load (fresh process) < 3 s to visible model with
  zero `BuildFromMeshDescriptions`/cook calls (assert via counters in a log line) and
  a hull line trace hitting.
- Cutlass Black (interiors, lights, doors): full hit reproduces lights and door clips.
- Changing LOD or mip is a miss for meshes/textures but reuses unchanged ones;
  switching to a different `Data.p4k` build never hits.
- Fill the cache past the cap with three ships → eviction removes the least recently
  used entry; `Clear cache` empties the root; a truncated blob is reported as a miss
  and rebuilt.
