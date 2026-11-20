# WP-C1 — Processed-data cache (layer 2)

**Agent:** Codex. **Depends on:** WP-U0 Spike D (round-trip findings), WP-U1
(derive/instantiate split, mesh hashes); extends to animation data after WP-U3.
**Build lock:** UE lock.

## Win64 implementation and verification

The Editor target running in game mode stores each imported static mesh variant as a cooked loose package under the
launch-relative `cache/processed` directory. The package has the engine's Nanite, Lumen card, distance-field, and
ray-tracing data. A checksummed companion record carries the variant's imported Chaos collision, including triangles
that were split off for translucent rendering. The key combines the archive build, GLB bytes and source path,
render/collision partition, host shader platform, and the engine's static-mesh format. Source paths preserve each
asset's identity for materials and import diagnostics. PNG mips are cached by bytes and sampling
role. DDS textures enter the importer with their authored mip chains.

The model browser's **Cache options** lets the user set the cap, choose the next launch's folder, clear the processed
cache, or rebuild the loaded model with the same options. The footer reports layer-1 export hits and layer-2 mesh
counts. The manifest publishes atomically while an OS mutex guards the root. Loaded assets keep their bulk files
until Unreal purges them. Retired files still count toward the disk budget during that period. A package whose
files pass disk checksums but cannot restore is retired by generation, leaving a concurrent replacement intact.

The synthetic production-path restart probe on the Win64 fork built a 100,352-triangle mesh in **1.820 s** and
persisted 24.73 MiB in **0.070 s**. A separate process loaded it in **0.031 s**. It restored 3,260 Nanite clusters
and read the streaming bulk data. Cards, distance field, and RT geometry were present on both runs. The same hull
ray hit Z=151.527 cm both times, with zero build/cook calls on restart. A same-process cycle verified shared
collision across two actors, retention during Clear, and reclamation after garbage collection.

The Q1 grouped NullRHI runner passed all three `SCBridge.Import.ProcessedCache` tests (Disk, Pin, Texture) in
**15.60 s** including editor launch and exit. The texture test loaded the persisted mips and handled a corrupt
cache entry. This run was separate from the real restart and live Gladius probes.

The Gladius load used LOD 0, mip 2 and the procedural Viewer map under NullRHI.

| Fresh process | Load | Export | Static mesh cache hits | Static mesh builds | Collision builds |
| --- | ---: | ---: | ---: | ---: | ---: |
| Empty archive and processed caches | 46.161 s | 10.016 s | 0 | 467 | 320 |
| Same archive and processed caches, before indexed material binding | 5.142 s | 0.083 s | 467 | 0 | 0 |
| Same caches, indexed material and paint bindings | 4.640 s | 0.081 s | 467 | 0 | 0 |

All passes produced 47 assets, 65 placements and 43 warnings. Warm actor assembly went from **1.805 s** to
**1.384 s**, and material binding went from **1.120 s** to **0.864 s**. The final warm run reported four PNG cache hits
and took **10.43 s** including editor launch and exit. The grouped NullRHI regressions passed seven tests in
**9.03 s**, including semantic cache retirement, split materials, the paint target and floor-panel paint behavior.

Warm time includes GLB derivation, rig and clip construction and actor assembly. The remaining gap to the
under-three-second gate is **1.640 s**. The entity index, persistent skeletal data, packaged Shipping validation
and GPU/Cutlass acceptance remain open.

### Commands

From the repository root, build once initially and use a fresh scratch cache for each independent comparison.

```powershell
$env:SC_SPIKE = '1'
$engine = Join-Path $env:USERPROFILE 'UnrealEngine-5.9-src'
$project = (Resolve-Path 'ue/SCViewer/SCViewer.uproject').Path
& "$engine/Engine/Build/BatchFiles/Build.bat" SCViewerEditor Win64 Development "-Project=$project" -WaitMutex
$editor = "$engine/Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
$scratch = Join-Path (Get-Location) 'out/c1-probe'
foreach ($phase in 'save', 'load', 'cycle') {
    & $editor $project '/Game/Maps/Viewer' -game -NullRHI -NoSound -unattended -nop4 -nosplash -SCDefaults -SCCacheDir=none -SCSpikeProductionMesh "-SCSpike=$phase" "-SCSpikeCacheDir=$scratch"
}
1..2 | ForEach-Object {
    & $editor $project '/Game/Maps/Viewer' -game -NullRHI -NoSound -unattended -nop4 -nosplash -SCDefaults -SCLoad=AEGS_Gladius -SCMip=2 -SCQuitAfterLoad "-SCCacheDir=$scratch/gladius"
}
& $editor $project '/Game/Maps/Viewer' -game -NullRHI -NoSound -unattended -nop4 -nosplash -SCDefaults -SCCacheDir=none '-ExecCmds=Automation RunTests SCBridge.Import.ProcessedCache' '-TestExit=Automation Test Queue Empty'
```

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
- The per-mesh cooked package must carry the Lumen mesh cards, distance field and
  ray-tracing geometry that WP-U1 builds (WP-U0 found the spike package carried none
  because the build path produced none); a cache hit must render identically in the
  Lumen overview to a fresh build.
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
