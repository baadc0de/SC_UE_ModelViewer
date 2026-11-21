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

### XL hangar and Hercules C2 integration

The live importer uses the processed cache for both archive-backed hangars and exported folders. The folder
namespace is independent of an open archive. Mesh keys still include GLB content and paths, and texture keys
include source bytes and sampling role. A Garage instance that needs to import an asset also uses the cache.
The editor's Keep as Assets workflow keeps its editable imports.

The environment importer groups repeated rigid components after assigning room channels and dirt data, before
registering instances. The XL hangar consolidated 9,162 parts into 529 registered instanced components.
5,247 of those parts never needed individual scene or physics registration. The same run retained the
hangar bounds, elevator floor and C2 placement. The room containment test uses precomputed segment bounds
to rule out unnecessary polygon and triangle tests and preserves the authored shape test for candidates.

These serialized Win64 D3D12 runs used LOD 0, mip 3 for the C2, the same exported XL hangar folder, and the
same layer-1 C2 tar. The combined column adds the two reported phases and excludes engine startup and the
player kit. The baseline comes from the interactive demo. The two cache measurements use Agent Mode.

| Run | Hangar import | C2 load | Combined |
| --- | ---: | ---: | ---: |
| Before integration | 21.8 s | 34.3 s | 56.1 s |
| Empty processed cache | 24.4 s | 43.4 s | 67.8 s |
| Fresh process, populated processed cache | 14.4 s | 23.5 s | 37.9 s |

The warm run restored 982 hangar and 1,782 C2 static mesh variants with zero static mesh builds or collision
cooks. The cold and warm runs agreed on the ship root position, cargo grids, door states, authored light
counts and environment placement. All 197 streamable Nanite variants in the hangar and all 338 on the C2
restored with the fine bulk pages unloaded. Nanite requests those pages by visibility and projected detail.
The cache still validates the files and imports the GLB metadata for scene assembly. The first import
builds the full exported LOD and writes the processed cache. Deferring source geometry on first visit
requires separate coarse and fine inputs, and a policy that keeps working collision and geometry visible
through open doors and in reflections. Skeletal and non-Nanite detail remains eager.

### Cold-load render previews

`-SCStreamGeometry` is an opt-in Editor game-mode profile. It uses the existing GLB on a cold miss
to make a short-lived, lower-detail render mesh. The reducer collapses connected edges within a grid
cell and protects material seams and sharp normals. It accepts a proxy only when it saves at least
20% of its triangles and preserves every populated material slot. Authored colliders, exact render-derived
collision, and the original bounds and rig binding are ready at assembly. This also applies to eligible
translucent splits and rigid meshes attached to animated rigs. Display panels and deforming skeletal
sections use their full geometry. The XL hangar and C2 have rigid ship parts attached to skeletal rigs
and no deforming ship render sections in this fixture.

The viewer schedules finer meshes for nearby surfaces in the occupied room and recently visible surfaces
whose projected error is large enough. Distant and occluded geometry can remain in its coarse representation.
It allows one fine build in flight, retains promoted meshes until unload, and caps pending fine CPU geometry
at 500,000 triangles per import and 1,200,000 globally. The GLB still needs an initial decode to derive
collision and the proxy. Coarse geometry has hardware ray tracing. The fine mesh commits its source
description on a worker and compiles through Unreal's asset queues. The viewer swaps it in on the game
thread once the static mesh, distance field, and Lumen card work is complete. The fine mesh reuses the
exact Chaos collision already held by the proxy.

The profile stays opt-in because fine geometry can change the lighting as it arrives. `-SCMaxQuality` uses
the exact path. Fixed-step RPC captures require active streamed imports to settle. Once setup is valid,
capture drains the in-flight build and promotes pending geometry before starting. Later loads in that
capture session use exact meshes at import.

On a cold miss, processed packages are written after initial readiness while the camera is steady and
the fine build queue has no active job. Unloading an actor drops its pending upgrades and unwritten
packages before collecting its assets.
The cache contains exact variants. A quick exit retains packages already published and drops the
remaining work. A populated cache uses the direct exact restore path.

The serialized comparison used separate empty processed caches, the same populated StarBreaker export
cache and engine DDC, LOD 0/mip 3 for the C2, and the same XL folder. Both ran in Agent Mode on Win64 D3D12.
First-person ready includes engine startup, hangar and ship placement, and the Brightsky kit.

| Measure | Eager | Cold-load preview |
| --- | ---: | ---: |
| Hangar import | 24.1 s | 18.5 s |
| C2 load | 42.2 s | 32.9 s |
| First-person ready | 88.0 s | 72.1 s |

Startup used 50 hangar proxies and 56 C2 proxies, including 26 translucent variants and 57 rigid variants
attached to rigs. At readiness, 102 fine variants were still pending. The camera proved C2 promotion in the
escape pods, the cargo hold, and alongside the hull. The route ended with 86 still pending and zero failed
promotions. All 22 promotions through shutdown took a median 17.8 ms and a maximum 51.5 ms on the game
thread. The ready snapshots agreed on ship position, cargo grids, and door states. Both sessions exited
cleanly. The demand-priority, importer, instancing, and processed-cache tests passed.

#### Fine-build hitch benchmark

The synchronous control ran at `c7f3a1f`. Two runs used the worker path. All three ran serially with separate
empty processed caches, the same populated export cache and DDC, D3D12 HDR, and Lumen Surface Cache.
Each started in first person and waited for the Brightsky kit before sampling. The benchmark then flew
the same camera poses through the C2 escape pod and cargo hold areas and followed the port hull.
Unreal CSV profiling recorded every frame during the two approximately 5.5-second segments.

| Segment and build path | Frames | p50 (ms) | p95 (ms) | p99 (ms) | Max (ms) | Frames over 33.3 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Room transitions, synchronous | 457 | 10.13 | 24.80 | 46.95 | 69.88 | 14 |
| Room transitions, worker run 1 | 521 | 9.97 | 14.83 | 23.43 | 38.54 | 3 |
| Room transitions, worker run 2 | 570 | 9.27 | 13.25 | 19.98 | 39.29 | 1 |
| Close hull pass, synchronous | 614 | 8.73 | 11.19 | 16.66 | 29.98 | 0 |
| Close hull pass, worker run 1 | 612 | 8.70 | 11.62 | 12.60 | 15.06 | 0 |
| Close hull pass, worker run 2 | 644 | 8.43 | 10.51 | 11.42 | 12.50 | 0 |

The synchronous control spent up to 60.60 ms in a single game-thread promotion. Across the worker
runs, the instrumented game-thread work during the route peaked at 0.35 ms for preparation, 2.27 ms
for submission, and 3.49 ms for applying a completed mesh. The longest apply across the full sessions
was 13.29 ms. Median wall time from request to swap was 21–23 ms, while the proxy stayed in use.
The room segment completed 53 fine upgrades in the control and 51 in each worker run. The hull
segment completed 12 in the control and 14–15 in the worker runs. Every worker submission reported
asynchronous compilation, with zero failed promotions. Room-frame spikes remained in the broader
game-thread tick, beyond the instrumented prepare, submit, and apply scopes.

| Cold processed-cache load | Synchronous | Worker run 1 | Worker run 2 |
| --- | ---: | ---: | ---: |
| XL hangar import | 17.99 s | 18.14 s | 17.79 s |
| C2 load | 31.93 s | 33.47 s | 32.03 s |
| First-person ready | 70.55 s | 73.07 s | 71.58 s |

The ready snapshots agreed on the ship, root position, cargo grids, and doors. The Win64 Development
build, geometry-streaming tests, processed-cache tests, and deferred-instancing test passed.
Geometry-streaming automation covered the fine source commit, capture wait, and shared collision.
The route runner, raw CSV captures, and logs are under `out/xl-cold-geometry/`
with prefixes `hitch-control`, `hitch-offthread`, and `hitch-offthread-repeat`.

#### Room audio follow-up

An Unreal Insights trace at `a092a54` put 33.76 ms of a 37.96 ms game-thread frame inside
`USCAudioSubsystem::Tick`. The escape-pod entry logged 46 voice starts in that tick.
Limiting the number of emitter posts reduced the largest burst to 23 voices, because several
authored triggers open multiple voices together.

The audio tick now charges a budget of six newly started placed voices across the active scenes.
It completes each trigger's layers before checking the budget again. Deferred emitters must still
be audible on a later tick, and placed loops retain their scene clock phase. Zero-voice posts
consume no budget and wait half a second before retrying. Explicit audio events
keep their existing timing. The two reruns logged a peak of 14 voice starts in any room frame and
355–357 starts across the segment, compared with 355 in the trace run. Audio tick time peaked
at 12.34 and 13.55 ms, respectively.

| Segment | Baseline p95 / p99 / max (ms) | Rerun 1 p95 / p99 / max (ms) | Rerun 2 p95 / p99 / max (ms) |
| --- | ---: | ---: | ---: |
| Room transitions | 13.25–14.83 / 19.98–23.43 / 38.54–39.29 | 16.03 / 25.17 / 33.90 | 14.02 / 20.00 / 31.44 |
| Close hull pass | 10.51–11.62 / 11.42–12.60 / 12.50–15.06 | 13.11 / 14.10 / 14.75 | 12.58 / 13.35 / 14.96 |

Room frames over 33.3 ms fell from 1–3 per run to 0–1. The largest game-thread samples still
included about 13.2 ms of Unreal garbage collection and additional world tick work. The peak
game-thread sample in the repeat was 44.48 ms. The runs used the same route, cold processed
cache, HDR, and Surface Cache. The hangar imported in 18.40–18.42 s, the C2 loaded in 32.40–32.51 s,
and first person was ready in 74.22–75.03 s.

The Win64 Development build and all seven `SCBridge.Audio` tests passed. All Unreal runs and
builds were serialized. The Insights trace and the benchmark captures are in `out/xl-cold-geometry/`
under the `room-profile-before` and `room-audio-layered` prefixes.

After the cross-review fixes, the final `room-audio-final` run measured 16.34 / 19.04 / 33.17 ms
for room p95 / p99 / max and 13.84 / 15.27 / 17.06 ms for the hull. The peak room audio tick was
14.92 ms. The hangar imported in 18.84 s, the C2 loaded in 32.26 s, and first person was ready
in 72.31 s. Unloading the C2 left zero of 49,439 tracked objects alive after collection.
The HDR RPC smoke test produced valid PNGs with and without UI. Recording encoded an MP4 from
the float HDR source with zero dropped frames.

#### Garbage collection and frame pacing

The fresh `f62b128` run brought room p95 back from 16.34 to 12.87 ms and hull p95 from 13.84 to
10.89 ms. The previous slow run had 13.82 ms room GPU p95 and 11.92 ms hull GPU p95. Those fell to
10.67 and 9.70 ms on the fresh run of the same tree. The slowdown tracked run-to-run render and
GPU variation. Room audio also spent more frames on decoder creation after
the start budget was introduced. The earlier room trace had a 5.52 ms audio p95 and a 33.76 ms peak.
The previous slow run had an 8.49 ms audio p95 and a 14.92 ms peak.

| Unreal Insights scope | Main | Main with `-DisablePython` | Patched |
| --- | ---: | ---: | ---: |
| Peak `ConditionalCollectGarbage` | 27.95 ms | 15.29 ms | 15.62 ms |
| Python pre-collection callback in that pass | 12.70 ms | — | — |
| UObject reachability in that pass | 13.45 ms | 13.98 ms | 13.85 ms |

The viewer's editor-hosted `-game` startup now defaults to disabling Python. Explicit Python requests,
the editor, and commandlets retain their Python entry points. The normal Unreal collector still runs.
The trace showed about 14 ms of UObject reachability and subsequent cleanup slices of about 2 ms.
The audio tick now shares the listener transform across emitters in each interior and submits gain
and submix updates when their values change. In equal 11-second windows of the two Python-disabled
traces, the engine processed 263,717 volume and submix commands before the change and 8,235 after.

The serialized reruns used the same escape-pod-to-hold and port-hull route, cold processed caches,
populated export cache and DDC, D3D12 HDR, and Lumen Surface Cache.

| Run | Room p95 / p99 / max (ms) | Hull p95 / p99 / max (ms) |
| --- | ---: | ---: |
| Fresh `main`, lightweight CSV | 12.87 / 20.98 / 34.68 | 10.89 / 12.10 / 13.22 |
| Patched, lightweight CSV | 13.88 / 17.41 / 24.35 | 11.81 / 13.61 / 14.75 |
| Patched repeat, lightweight CSV | 13.01 / 16.92 / 35.39 | 11.79 / 12.79 / 14.08 |
| Patched, Insights trace and CSV | 13.72 / 21.13 / 31.69 | 11.62 / 13.00 / 15.42 |

The lightweight repeats lowered room p99, with room and hull p95 slightly above the fresh control.
The 35.39 ms frame in the repeat coincided with a 34.05 ms RHI sample, a 34.55 ms render-thread sample,
and 23.00 ms of game-thread event wait. Its game-thread work was 11.06 ms.
In the lightweight patched runs, hangar import took 17.90–18.43 s, C2 load took 32.35–32.42 s,
and first-person readiness took 71.65–72.50 s. Each ended the route with 67 successful promotions.

The Win64 Development build, 15 audio and importer tests, and a Python commandlet smoke test passed.
The raw traces, trace exports, and frame captures are in `out/xl-cold-geometry/` with the
`pacing-main`, `pacing-no-python`, and `pacing-fixed` prefixes.

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
