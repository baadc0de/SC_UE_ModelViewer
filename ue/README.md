# SCViewer — WP-U0 Unreal setup and feasibility spikes

UE **5.8.2**, `++UE5+Release-5.8`, CL **56702186**, installed at
`~/UnrealEngine/5.8`; Linux binaries are in `Engine/Binaries/Linux/`.
`df -h .` on 2026-11-09: **1.6T total, 1.5T used**; available space was
**82G / 95% used** at start and **76G / 96% used** at final verification.
The probe's project build products occupy about 434 MiB, plus 17 MiB under
`./cache/spike/`; no engine source was changed. Logs/build products are ignored.

## Decision

**Choose B: `SCViewerEditor` launched with `-game`, for continued development.**
A fails the installed engine's module rules. B builds Nanite data and cooks complex
collision in a game world, and reloads a cooked package after process restart.
**GPU acceptance (added after the sandbox run):** rendered Nanite, Lumen GI and
HWRT are verified on the RTX 4090 — see *GPU validation* below — with one finding:
runtime-built meshes have no Lumen scene / ray-tracing representation yet.

| Probe | A: Development Game target | B: Editor target with `-game` |
| --- | --- | --- |
| Compile | **No**: `MeshBuilder -> MeshUtilities -> MaterialUtilities -> UnrealEd`; UBT rejects non-editor targets (2.072 s wall). | **Yes**: probe build 7.648 s wall, UBT `Result: Succeeded`. |
| Runtime Nanite | Not runnable; no runtime result. | **Yes, CPU data**: 100,352 input triangles, 1,627 clusters, 1.591184 s mesh build. Cluster visualization is unverified. |
| Runtime complex collision | Not runnable; no runtime result. | **Yes**: 100,352 fallback triangles; 47.181 ms explicit recook; line trace hits the spike at Z=151.527 cm. |
| Direct render-data/body memory-archive round-trip | Not runnable; no runtime result. | **No** in this installed build: link fails on `FStaticMeshRenderData::Serialize` (5.978 s wall). |
| Mounted cooked-package fallback | Not runnable; no runtime result. | **Yes for data and collision**: save 0.994380 s; restart with an empty DDC loads in **13.941 ms**, including reading the Nanite streaming sidecar. Rendering remains unverified. |

The cached object is cooked, has zero source models, 1,633 Nanite clusters,
26,504 root-data bytes, 1,999,926 readable streaming bytes, and one Chaos triangle
mesh. The reload reports `build_calls=0 cook_calls=0`, `TRACE hit_spike=1`, and
`RUN_COMPLETE passed=1`. No `SpikeMesh` rebuild appears in the reload log; unrelated
engine/map meshes do rebuild with the empty DDC. Save prepares Linux platform data,
so its cluster count can differ from the initial editor-platform build.

Use **cooked `.uasset` + `.uexp` + `.ubulk` packages** mounted at `/SCCache/` for
WP-C1's mesh-cache foundation. The spike writes only to `./cache/spike/` relative
to the invoking working directory. It requires an `ICookedPackageWriter` and waits
for distance-field, Lumen-card and Nanite streaming preparation before saving.
The three files total 17,374,663 bytes. These are single-run synthetic measurements,
not Gladius timings; the mesh-build time includes UE's automatic collision work,
and the separately timed recook follows `InvalidatePhysicsData()`.

Exact chosen launch command, from the repository root (GPU validation still needed):

```bash
"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor" \
  "$PWD/ue/SCViewer/SCViewer.uproject" \
  -game -windowed -resx=1600 -resy=900 -vulkan -ddc=SCViewerDDC
```

## GPU validation (2026-11-09, host B on the RTX 4090)

Run by the orchestrator outside the Codex sandbox with the launch command above
(`ue/scripts/run_spike.sh load`, then `-SCSpikeStay` runs with `-ExecCmds` for the
visualisation modes). Evidence: `ue/verification/wp-u0/*.png` (half size).

| Check | Result |
| --- | --- |
| RHI | Vulkan on `NVIDIA GeForce RTX 4090`, driver 620.70, API 1.4.357; `Ray tracing is enabled (dynamic)`, ray tracing shaders enabled |
| First Vulkan start | ~11 min (10:41:46 → 10:52:55) spent compiling shaders; later starts reach the loaded mesh in well under a minute |
| Cooked-package reload on GPU | `LOAD_PACKAGE seconds=0.021` (then 0.013/0.012/0.012 on three more runs), `nanite=1 clusters=1633`, `build_calls=0 cook_calls=0`, `TRACE hit_spike=1`, `RUN_COMPLETE passed=1` |
| Rendered Nanite | **Yes**: `r.Nanite.Visualize Overview` shows the spike plane and the map cubes in the Triangles / Clusters / Instances / Primitives panels (`nanite.png`) |
| Lumen GI | **Yes** for map assets: the emissive sphere lights the neighbouring cube face and floor; VSM shadows (`lit.png`) |
| HWRT | Enabled; the Lumen overview's "Reflection View, HWRT" panel renders the map assets (`lumen.png`) |
| **Runtime-built mesh in the Lumen scene** | **No.** In both `build` (live) and `load` (cooked) runs the spike mesh appears as a black cut-out in "Lumen Scene" and in "Reflection View, HWRT" (`build-lumen.png`, `lumen.png`), while the map's cubes are present. Black (not pink) means the mesh has no Lumen scene representation at all: `BuildFromMeshDescriptions` produces no mesh cards, no distance field and no ray-tracing geometry. The cooked package therefore has nothing of the kind to carry; the round-trip is not at fault. |

Consequence for WP-U1 and WP-C1: runtime-built ship meshes must also get Lumen mesh
cards (`MeshCardBuild`), a distance field (`MeshUtilities` DF generation) and ray
tracing geometry (`bSupportRayTracing` + RT geometry init) — all available in host B
because the editor modules are linked — or be built through the editor `Build()`
path that produces them; and the cache must store them. Verify with the Lumen
overview: no black cut-out where the ship is.

The `pkill -f` in the screenshot helper killed the helper itself once (exit 144) and
left the game running; stop the game by PID, not by pattern.

## Project and commands

`SCViewer` is a C++ Game target; `SCViewerEditor` is its Editor target. Both use
5.8's `BuildSettingsVersion.V7`. `SCBridge` is Runtime; `SCBridgeEditor` is Editor;
`SCBridgeLib` is an empty external module awaiting WP-R4. The Development Game
build deliberately retains the builder dependencies to reproduce Spike A's failure.
The app-facing probes live in Runtime, with editor-only build/save paths guarded.
No cargo was run, no game data was exported, and nothing was pushed or opened as a PR.

The `Viewer` map contains sky/directional lights, sky atmosphere, a neutral
collision floor, three Nanite cubes, an emissive sphere and a PlayerStart. The
three content files are synthetic UE assets, not game exports.
Nanite, Lumen GI/reflections, HWRT, VSM, distance fields and Vulkan SM6 are configured.
`.claude/launch.json` provides `ue-editor` and `ue-game`.

```bash
ue/scripts/build.sh SCViewerEditor
ue/scripts/build.sh SCViewer                 # expected RulesError for Spike A
SC_SPIKE_RAW_SERIALIZE=1 ue/scripts/build.sh SCViewerEditor -NoUBTMakefiles
                                             # expected link failure for Spike D
ue/scripts/build.sh SCViewerEditor -NoUBTMakefiles  # restore working probe
ue/scripts/run_spike.sh save                  # build, cook, save, then quit
ue/scripts/run_spike.sh load                  # fresh process: load and trace
```

On a GPU-equipped machine, use `-SCSpikeStay` to keep the game window open and
`sc.SpikeBuildMesh`, `sc.SpikeSavePackage`, `sc.SpikeLoadPackage` in the console.
Load must run in a fresh process to measure persistence. Runtime probes otherwise
quit after 20 seconds of frames; that wait is included in process wall times.
Use `viewmode naniteoverview` / `r.Nanite.Visualize clusters` for Nanite inspection,
and the editor viewport's **Lumen Overview / Lumen Scene** for the Lumen check.
These visual checks have **not** passed in this environment.

Headless commandlet (passed):

```bash
"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/headless_smoke.py" \
  -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC
```

`verify_viewer.py` can replace `headless_smoke.py` to verify all nine scene actors,
the Nanite flags, floor collision and emissive material assignment.
`create_viewer.py` generated the map through the same commandlet; it is a one-time
creator and refuses an existing map. For intentional regeneration, remove only
`Content/Maps/Viewer.umap`, `Content/Smoke/NaniteCube.uasset` and
`Content/Smoke/Emissive.uasset` first.

`SCViewerDDC` is a project-local filesystem DDC, selectable with `-ddc=SCViewerDDC`.
`-SCViewerDDCPath=<empty-dir>` redirects it for cold-cache verification. It is
engine scratch, separate from the D10 cache. `run_spike.sh` passes the caller's
absolute `./cache/spike/` path before UE changes cwd; console-only launches use
the shell's `PWD` when no `-SCSpikeCacheDir` is supplied. `UE_ENGINE_ROOT` overrides
the install path in the helper scripts.

## Observed environment failures and workarounds

Exact commands, timings and key output are retained in [verification/WP-U0.md](verification/WP-U0.md).
Full logs are local in the ignored `SCViewer/Saved/Spikes/` directory.

- UBT initially failed writing its user trace (`Read-only file system`).
  `XDG_CONFIG_HOME` and `-UBARootDir` redirect its scratch to the project. The UBA
  directory failure loop persisted with `-NoUBA -NoUBALocal`; an explicit root
  fixed it. Repeated error logs were trimmed to keep disk usage bounded.
- Plain headless startup stalled on Zen: `Unreal Zen Storage Server is unable to
  determine a valid data path`, followed by read-only writes in the user directory.
  The local filesystem DDC workaround passed in 3.507 s: `HEADLESS_PYTHON_OK`,
  `Python script executed successfully`, zero errors. The final map verification
  also passed; benign read-only editor-preference warnings remain.
- Vulkan: `SelectPhysicalDevice could not find a compatible Vulkan device or
  driver (EnumeratePhysicalDevices returned 'VK_ERROR_INITIALIZATION_FAILED' and
  0 devices)`. `/dev/nvidia*` and `/dev/dri/*` are absent in this tool environment.
  `-nullrhi` permitted CPU experiments; it does not verify rendering.
- Full editor `-ExecutePythonScript` also encounters a native modal because
  `FDesktopPlatformLinux::EnumerateEngineInstallations` cannot write
  `~/.config/Epic/Unreal.uproject`. `-unattended` and redirecting UBT config did not
  bypass that engine behavior. The Python commandlet succeeds without the full UI.
- A plain cooked `SavePackage` asserted `!IsCooking() || PackageWriter`; an ordinary
  package writer then asserted `CookedWriter`. The implemented cooked writer plus
  readiness wait passed. It is a spike sink, not a versioned, atomic production cache.

## Skeletal Nanite in 5.8

The installed `Engine/Source/Runtime/Engine/Private/Rendering/NaniteResources.cpp`
defines the read-only `r.Nanite.AllowSkinnedMeshes` CVar with default **1**; the
runtime query returned `"1" LastSetBy: Constructor`. Enable Nanite on each skeletal
mesh through its Nanite settings. Epic's current [5.8 overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine)
and [content guide](https://dev.epicgames.com/documentation/unreal-engine/working-with-naniteenabled-content?application_version=5.8)
list skeletal mesh support, including animation LODs and virtual shadow maps,
without an Experimental label in that section. They still label other Nanite
features experimental. This is documentation/source evidence, not a skeletal
rendering test; WP-U3 must validate our soft-skinned import before enabling it by default.

## Open work

TODO(WP-U0): Run Vulkan Nanite cluster visualization, Lumen scene view and HWRT checks on a host exposing the RTX 4090; also render the reloaded cooked package.
TODO(WP-U0): Commit the prepared files when `.git` is writable. Initial branch creation failed; by final verification the checkout was `wp/u0-unreal-spike` at a2e64db, but staging still failed with `.git/index.lock: Read-only file system`. All changes remain uncommitted. Use the requested `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` trailer; do not push or open a PR.
TODO(WP-U0): Interchange Gladius baseline is not done. The export wrapper runs cargo, prohibited for this task; feature preservation, orientation and dimensions remain unmeasured for WP-U1.

## WP-U1 runtime importer

`SCBridge` now imports decomposed GLB packages in the chosen Editor `-game`
host. The runtime entry point is
`FSCImporter::Import(FSCPackageSource&, FSCImportOptions, FSCImportResult&)`.
`FSCFolderPackageSource` and `FSCTarPackageSource` implement the same read-only
`Exists`, `ReadBytes` and `ListFiles` API; paths are relative to the export root.
The tar source owns its buffer and supports regular USTAR/GNU files, including
GNU long names. It rejects traversal, duplicates, links, invalid checksums and
truncation. PAX and compressed archives are outside the WP-R4 package contract.
Select `FSCImportOptions.ScenePath` when a shared export root contains multiple
`scene.json` files. Errors return `false` with `FSCImportResult.Error`.

The reader exposes reflected scene, instance, interior, palette, material and
clip records. Material metadata remains lossless JSON for WP-U2; it retains both
array ordinals and authored (potentially sparse) indices. `ASCVehicle` owns the
transient meshes and `USCClipCatalog`. Repeated paths share one mesh. Helpers,
resolved child transforms and interior placements assemble the component tree;
loading another package replaces the previous vehicle. All drawable components
currently use the engine's grey grid placeholder master, and skins are static.

The **direct GLB / FMeshDescription builder is selected**. The editor-only
`USCInterchangePipeline` / `USCInterchangeBaseline` remains available for
comparison. It sets Nanite before import and applies the same ship-facing yaw.
The direct path preserves authored normals, tangents, UV0/UV1 and vertex colours,
generates missing tangents with MikkTSpace, and preserves material slot numbers.
It supports embedded GLB triangle geometry, normalized/interleaved accessors,
node matrices and TRS, and hierarchy-only assets. Required extensions, sparse
accessors and external buffers fail explicitly. The supplied package needs none
of those unsupported geometry features.

Geometry is glTF metres to UE centimetres, `(x,y,z) -> (-z,x,y) * 100`.
Transform conversion conjugates by that basis. Named helpers retain their source
frame; an attached mesh's external coordinate wrapper is removed once when
composing the resolved local transform. Interior containers and placements use
the same source frame when attached to helpers. Missing helpers fall back to the
source entity root, matching the exporter, with a diagnostic. Repeated entity
names resolve in the manifest's depth-first order. Synthetic tests lock these
compositions; the Gladius hull's 19.689 m bounds match Interchange.

The editor `UStaticMesh::Build()` path builds Nanite, distance fields, Lumen cards
and ray-tracing proxy geometry. `bSupportRayTracing` is enabled before build;
compilation/card/distance-field queues finish before the result is published.
Nanite fallback uses **PercentTriangles=1**, with `FallbackRelativeError=1.0`,
retaining all nondegenerate triangles for collision. Empty assets retain helpers
and log `disabled:empty_geometry`. Tiny planar surfaces can have a built card
representation with zero selected cards; the hull must have actual cards.
All editor-only dependencies are behind the editor target/`WITH_EDITOR` guards.
The non-editor fast-build fallback remains in source; it is not a validated
Nanite/Lumen host.

Cache boundaries for WP-C1:

- `FSCMeshImporter::Derive` returns `FSCDerivedMesh`: mesh description, material
  slots, helper matrices and input SHA-1, without UObjects.
- `DeriveCollision` builds and serializes a Chaos triangle BVH on the worker into
  `CollisionBytes` plus `CollisionVersions`. It does not allocate a UObject.
- `Instantiate` creates the mesh and body setup and deserializes the BVH without
  cooking it again. `bBuildCollision=false` skips collision derivation and replay.
- A processed-cache key must also include `DerivedFormatVersion`, engine version,
  platform and build options. Engine Nanite/card/DF building remains in the
  UObject phase; WP-C1 can replay its cooked package rather than call that phase.
  No persistent processed cache is implemented here.

From the repository root:

```bash
ue/scripts/build.sh SCViewerEditor
ue/scripts/run_import.sh out/dec -vulkan -SCStay
# Headless game-host parity, trace and GC checks:
python3 -I ue/scripts/make_test_tar.py out/dec ue/SCViewer/Saved/Import/dec.tar
ue/scripts/run_import.sh out/dec -nullrhi \
  -SCVerifyTar="$PWD/ue/SCViewer/Saved/Import/dec.tar"
```

The editor File menu contains **Import SC package folder…**. Runtime callers can
also use `sc.Import <folder>` or `USCImportLibrary::ImportFolder` (these convenience
entry points lift the bounds one metre above the Viewer floor); Python exposes
`unreal.SCImportLibrary`. `ue/scripts/import_package.py` is the headless import
entry point. `ue/scripts/test_importer.py` runs the synthetic automation tests;
passing `-SCPackage` and `-SCVerifyTar` also runs the real folder/tar integration
test. `ue/scripts/interchange_baseline.py` accepts `-SCBaseline=<absolute GLB>`.
Use these scripts with `UnrealEditor-Cmd -run=pythonscript -script=<absolute script>
-nullrhi -unattended -nop4 -nosplash -ddc=SCViewerDDC`.

The old WP-U0 spike is quarantined behind `SC_SPIKE` (off by default). To reproduce
it, explicitly rebuild with
`SC_SPIKE=1 ue/scripts/build.sh SCViewerEditor -NoUBTMakefiles`, then use
`run_spike.sh`. Restore the default importer build afterward.

Evidence, exact acceptance commands, input-revision notes and GPU TODOs are in
[verification/WP-U1.md](verification/WP-U1.md). This sandbox has no Vulkan device;
CPU representations are verified, while rendered Nanite/Lumen/HWRT acceptance
must be completed on the orchestrator's GPU.
WP-U2 materials (Substrate decision, masters, builder, textures): [verification/WP-U2.md](verification/WP-U2.md).

## WP-V1 viewer app (model browser, load pipeline, cameras)

The game mode `ASCViewerGameMode` (set in `DefaultEngine.ini`) shows the in-game
Slate UI in every host. Run from the repository root (the cache defaults to
`./cache/`, resolved from the launch directory):

```bash
"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor" "$PWD/ue/SCViewer/SCViewer.uproject" \
  -game -windowed -resx=1600 -resy=900 -vulkan -ddc=SCViewerDDC
```

Search, pick options, Load; drag to orbit, Shift/middle-drag to pan, wheel to zoom,
double-click a part to focus, F to frame all; Fly mode uses WASD/QE with Shift/Ctrl.
Switches: `-SCArchive=<Data.p4k>`, `-SCCacheDir=<dir|none>`, `-SCSearch=<q>`,
`-SCLoad=a,b`, `-SCHold=<s>`, `-SCCamera=fly`, `-SCFlyView=dx,dy,dz,pitch,yaw`,
`-SCDefaults`, `-SCLod=`, `-SCMip=`, `-SCQuitAfterLoad`, `-SCNoUI`; console `sc.Load`,
`sc.Unload`, `sc.Cancel`, `sc.Camera`. `ue/scripts/capture_viewer.sh` screenshots a run.
Headless (WP-Q1): `UnrealEditor-Cmd … -run=pythonscript -script=ue/scripts/ue_load_gladius.py
-nullrhi` drives `USCViewerSubsystem.LoadEntity` through the FFI. Settings persist in
`Saved/Config/<Platform>/GameUserSettings.ini`. Evidence: [verification/WP-V1.md](verification/WP-V1.md).
