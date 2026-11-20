# WP-C1 — Processed-data cache (layer 2)

**Agent:** Codex. **Depends on:** WP-U0 Spike D (round-trip findings), WP-U1
(derive/instantiate split, mesh hashes); extends to animation data after WP-U3.
**Build lock:** UE lock.

## Win64 implementation and verification

The Editor target running in game mode stores each imported static mesh variant as a cooked loose package under the
launch-relative `cache/processed` directory. The package has the engine's Nanite, Lumen card, distance-field, and
ray-tracing data. A checksummed companion record carries the variant's imported Chaos collision, including triangles
that were split off for translucent rendering. The key combines the archive build, GLB bytes and source path,
render/collision partition, host shader platform, and the engine's static-mesh format. Full rigid-part hits keep the original lighting samples and socket association without copying CPU mesh descriptions.
Source paths preserve each
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

A same-binary A/B on the same warm cache measured **4.698 s** with full CPU descriptions and **3.778 s** with
deferred descriptions. All 467 cached variants restored with zero mesh builds or collision cooks. The new path
deferred 252 rigid parts. Preparation worker time dropped from **1.9245 s** to **0.5633 s**, and preparation wait
dropped from **1.9325 s** to **0.5766 s**. GLB derive worker time stayed at approximately **1.94 s**, with the
import cursor waiting **0.3697 s** as it caught up with the workers. Whole-process time went from **10.29 s** to
**9.59 s**. The grouped NullRHI suite passed eight tests, including the exact lighting-sample comparison.

The recorded LOD 0 Gladius result is **0.778 s** above the under-three-second gate. The entity index,
persistent skeletal data and packaged Shipping validation remain open.

For faster local feedback, the same procedural NullRHI route at LOD 2, mip 4 measured **14.398 s**
with an empty processed cache and **2.653 s** on the warm restart. Whole-process time was **20.330 s**
and **8.434 s**, including launch and exit. Both processes reused the same 49,078,784-byte layer-1 archive,
prepared in **10.704 s** offline through the staged FFI. The cold load built 308 meshes and cooked 229
collision shapes. The warm load hit all 308 processed meshes with zero builds or cooks. Both placed
65 instances from 47 assets and reported 43 warnings. Use `-SCLod=2 -SCMip=4` for this quick fixture and
`-SCQuitAfterLoad` to exit as soon as the load completes. The LOD 0 benchmark above remains the acceptance gate.

### Cutlass GPU cold and warm comparison

Two serialized Win64 Development game-mode processes on the source UE 5.9 fork ran D3D12 SM6 with the
procedural Viewer map. Both used LOD 0, mip 0 and the exact same 1,550,427,648-byte layer-1 archive.
The cold process started with an empty processed cache. The warm process reused it.

| Fresh process | Load | Layer-1 lookup | Static mesh cache hits | Static mesh builds | Collision cooks |
| --- | ---: | ---: | ---: | ---: | ---: |
| Cold processed cache | 17.529 s | 1.016 s | 0 | 886 | 560 |
| Warm processed cache | 8.805 s | 1.025 s | 886 | 0 | 0 |

Both runs restored 225 authored lights across the same eight volumes and three rooms. The ship root was at
Z=5.427 m on the same front gear toe. The first-person pawn found a floor and hit the closed rear ramp
at 363.3 cm with world Z=219.07 cm and normal (0.69, 0.00, 0.73). The same authored ramp clip played and
produced matching closed and open endpoints. Astra passed all three cold/warm screenshot pairs, covering
the overview and the cargo bay with the rear ramp closed and open, at fixed camera poses and EV100 6.7.

The first comparison exposed a warm root displacement of 160.86 cm. Cooked cache packages retain their
Nanite render resources and strip editor settings. Scene assembly and the hierarchy panel now read Nanite
availability from the actual render resources. This restores the same shadow participation and keeps the
gear toe eligible for floor placement. The comparison now asserts the root pose, the exact rest support
and the pawn impact alongside the lighting and ramp checks.

For the warm control, add `-SCDisableDeferredRigidGeometry` to the Gladius command in a development build.
Both passes use the same cache directory. `IMPORT_PREPARE` reports worker and cursor-wait durations.
That Gladius benchmark predates the Cutlass Nanite classification fix.

For the GPU comparison, seed a fresh cache directory with the layer-1 Cutlass tar and its index, then launch
twice with the same directory using `-game -d3d12 -NoSound -SCDefaults -SCLoad=DRAK_Cutlass_Black -SCLod=0 -SCMip=0`,
`-SCCacheDir=<scratch-cache>` and `-SCPlayerPackage=<player-package>`. Use the Q1 RPC runner to wait for load
and settled look. Clamp exposure to EV100 6.7 with both `AutoExposureMinBrightness` and
`AutoExposureMaxBrightness`, set `AutoExposureBias 0` and clear `AutoExposureBiasCurve`. The cargo route is
`sc.FP.Spawn Interior:1`, `sc.FP.Look 180 -5` and the authored rear ramp from `sc.Doors.Pose`. Capture after
the closed and open states settle, copying the cold Fly camera poses to the warm process.

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
