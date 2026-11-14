# SCViewer — WP-U0 Unreal setup and feasibility spikes

UE **5.9.0** preview-1, `++UE5+Release-5.9`, CL **60351404**, installed at
`~/UnrealEngine/5.9`; Linux binaries are in `Engine/Binaries/Linux/`. It replaced 5.8.2
(`++UE5+Release-5.8`, CL 56702186) on 2026-11-12; the Lumen World Cache it was installed for is off for now (plan D13);
verification and the World Cache / G-buffer A/B routes: [verification/WP-U59.md](verification/WP-U59.md).
Shader patches (plan D14) are applied to the installed engine with `uv run python ue/scripts/engine_patches.py apply`
(`check`, `revert`); see [EnginePatches](EnginePatches/README.md).
WP-U0 on 5.8.2, `df -h .` on 2026-11-09: **1.6T total, 1.5T used**; available space was
**82G / 95% used** at start and **76G / 96% used** at final verification.
The probe's project build products occupy about 434 MiB, plus 17 MiB under
`./cache/spike/`; no engine source was changed. Logs/build products are ignored.

**Win64:** the project also builds and runs on Windows (UE 5.9 from the Epic launcher, D3D12 SM6:
`SCViewer/Config/Windows/WindowsEngine.ini`; the look is the shared one). The FFI is staged with
`scripts/build_ffi.ps1` (`scbridge.dll` + import library) and the build is
`<engine>\Engine\Build\BatchFiles\Build.bat SCViewerEditor Win64 Development -Project=<abs>\ue\SCViewer\SCViewer.uproject`.
`-SCShotDir=<dir>` saves route shots in-engine (no `capture_route.sh` on Windows). NVIDIA DLSS is enabled on Win64
(`.uproject` entry, `Optional`; `r.NGX.DLSS.Enable 1`): DLAA at the default 100 % screen percentage, DLSS Super
Resolution below it (`r.ScreenPercentage 67` = Quality). The plugin is NVIDIA's download (developer.nvidia.com/rtx/dlss,
not redistributable) unpacked to `ue/Plugins/NVIDIA/DLSS/` (gitignored); without it the project builds and runs with TSR.
NVIDIA ships it per engine version; for a preview engine the newest release's source is compiled against it.

**Web (planned):** a separate 5.9 Wasm / WebGPU build that bakes one level and one ship as cooked assets — no Data.p4k,
no Rust FFI in the browser (32-bit Wasm, ~2 GB practical memory). Building blocks: cooked hangar levels
(`cook_hangars.py`, `SCCookLibrary`) and Keep-as-assets for a ship (`SCKeepAssets`).

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
"$HOME/UnrealEngine/5.9/Engine/Binaries/Linux/UnrealEditor" \
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
"$HOME/UnrealEngine/5.9/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/headless_smoke.py" \
  -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC
```

`verify_viewer.py` can replace `headless_smoke.py` to verify the hangar's scene actors
(floor, walls, ceiling, 16 ceiling lights, smoke cubes, emissive sphere, start), the Nanite
flags, floor collision and emissive material assignment.
`create_viewer.py` generates the default map (a closed hangar, WP-U3b §11.9) and the untracked
presets `Content/Maps/Presets/Viewer_{Daylight,Space}` through the same commandlet. It refuses
existing maps: for intentional regeneration remove `Content/Maps/Viewer.umap` and
`Content/Maps/Presets/` first; assets under `Content/Smoke/` are reused when present.

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
WP-U2 materials (Substrate decision, masters, builder, textures, macro surface / HAL / gloss convention,
HardSurface static wear, palette finish, PNG colour encoding, energy split):
[verification/WP-U2.md](verification/WP-U2.md).

## WP-V1 viewer app (model browser, load pipeline, cameras)

The game mode `ASCViewerGameMode` (set in `DefaultEngine.ini`) shows the in-game
Slate UI in every host. Run from the repository root (the cache defaults to
`./cache/`, resolved from the launch directory):

```bash
"$HOME/UnrealEngine/5.9/Engine/Binaries/Linux/UnrealEditor" "$PWD/ue/SCViewer/SCViewer.uproject" \
  -game -windowed -resx=1600 -resy=900 -vulkan -ddc=SCViewerDDC
```

Search, pick options, Load; drag to orbit, Shift/middle-drag to pan, wheel to zoom,
double-click a part to focus, F to frame all; Fly mode uses WASD/QE with Shift/Ctrl.
Switches: `-SCArchive=<Data.p4k>`, `-SCCacheDir=<dir|none>`, `-SCSearch=<q>`,
`-SCLoad=a,b`, `-SCHold=<s>`, `-SCCamera=fly`, `-SCFlyView=dx,dy,dz,pitch,yaw`,
`-SCDefaults`, `-SCLod=`, `-SCMip=`, `-SCQuitAfterLoad`, `-SCNoUI`, `-SCHostExposure`; console `sc.Load`,
`sc.Unload`, `sc.Cancel`, `sc.Camera`. `ue/scripts/capture_viewer.sh` screenshots a run.
**Dirt** (WP-U2b, [verification/WP-U2b.md](verification/WP-U2b.md)): the options panel's Dirt slider and presets
(Dust / Sand / Mud / Ash) put the game's dirt rule on exterior surfaces, live and saved per user; `sc.Dirt <amount>
[preset]` does the same from the console or a route. The game's ships author no dirt; the defaults are viewer choices.

Headless (WP-Q1): `UnrealEditor-Cmd … -run=pythonscript -script=ue/scripts/ue_load_gladius.py
-nullrhi` drives `USCViewerSubsystem.LoadEntity` through the FFI. Settings persist in
`Saved/Config/<Platform>/GameUserSettings.ini`. Evidence: [verification/WP-V1.md](verification/WP-V1.md).

**Game photometry** ([verification/WP-U3b.md](verification/WP-U3b.md) §13). Light candela, emissive cd/m²,
auto exposure and the display curve follow the installed game's renderer. The values the Viewer cannot read
at run time (cvar defaults of the executable, shader constants) live in the extracted fixture
`Plugins/SCBridge/Resources/StarEngineRender.json` (provenance `StarEngineRender.notes.md`; no in-code
fallback — a missing or out-of-domain fixture is fatal). Re-check it after every game patch:
`python3 -I scripts/verify_star_engine_render.py` → `STAR_ENGINE_RENDER OK: 47/47 checks match the installed
game`. At `StartPlay` the game mode spawns an unbound post-process volume above the map's own
(`SCViewerExposure::Install`, log line `VIEWER_GAME_EXPOSURE …`): histogram exposure with the game's
EV-compensation curve, multiplier clamp, discard ratios and pupil half-life, plus
`/SCBridge/Materials/M_SC_DisplayMapping` (a replacing-tonemapper port of the game's `HuePreservingShoulder`,
generated by `ue/scripts/create_display_mapping_material.py`, which refuses to build when the ported shader
functions' hashes differ from the fixture). `-SCHostExposure` skips the volume (map exposure, UE tonemapper)
for comparison captures.

## First-person exploration (WP-P1)

Load a package, then choose **First person** or **Spawn where I look** in Orbit.
Collision readiness includes the soft skin's animated triangle body. Click the
viewport for mouse look; Esc releases capture. WASD walks, Shift runs, Space jumps,
Ctrl crouches, E toggles the interaction under the crosshair (else the nearest
driven joint) within 3 m, F toggles the camera torch, V toggles noclip (Space/Q
vertical), T opens the teleport list. One key table (`ASCFirstPersonCharacter::KeyTable`)
drives both Slate and native input.

Spawn kinds come from data: `Seat`, `SeatAccess`, `Door` (DataCore `AttachDef.Type`),
`Door` also for every interaction's driven joint, `Interior` (interior container
floors), `Hull` (above the collision bounds) and `Selected` ("spawn where I look",
replaced on each use, never ranked). The first-person start ranks
`Entrance` (only when the data marks a hull entrance — not exported yet) > `SeatAccess`
> `Seat` > `Interior` > `Door` > `Hull`. Interactions are Mannequin fragments with an
Open/Close tag pair on a rig the clip binds; their open state is the tag last played
on that rig instance. Unsafe teleport/noclip-exit locations are rejected (the refusal
stays on the HUD for 3 s); falling recovers to the last spawn and blocked movement
offers noclip after two seconds. Soft-skinned collision is CPU-skinned and refit by
`USCRigComponent::RefitCollision` whenever a weighting bone moves (same path with and
without a GPU).

The folder harness accepts `-SCPackage=<export-root> -SCExplore` to use the app UI
and first-person possession without opening Data.p4k. `-SCWalkSeconds=2` adds a
scripted forward walk, a mode/unload/reload cycle and a quit while in first person,
and exits with `P1_GAME_WALK` / `P1_GAME_LIFECYCLE`. The archive-backed host accepts
`-SCCamera=first` after `-SCLoad=<entity>`. Rebuild/restage the debug FFI library
with `scripts/build_ffi.sh` after exporter changes.

### Scripted first person (GPU evidence, reused by WP-P2)

Console commands (game hosts; `sc.FP.*` other than `Spawn` need first-person mode):

| Command | Effect |
| --- | --- |
| `sc.FP.Spawns` | log spawn list (`FP_SPAWN_LIST index kind pos label`) and interactions (`FP_DOOR_LIST`) |
| `sc.FP.Spawn <index\|kind[:n]>` | enter first person if needed, teleport to spawn `index` or the n-th spawn of `kind` |
| `sc.FP.Look <yaw> <pitch>` | absolute view, degrees, relative to the vehicle |
| `sc.FP.Move <fwd> <right> <seconds> [run 0\|1] [up]` | hold movement input (-1..1) for a duration |
| `sc.FP.Interact` / `sc.FP.Door <index>` | E on the focused interaction / toggle one by index |
| `sc.Paint [index\|default\|name part]` | list the loaded vehicle's paints (`PAINT_CHOICE`), or put one on (also the side panel's Paint row; remembered per vehicle) |
| `sc.FP.Crouch 0\|1`, `sc.FP.Jump`, `sc.FP.Torch 0\|1`, `sc.FP.Noclip 0\|1` | as the keys (a refused noclip exit logs `FIRST_PERSON_NOCLIP refused=1`) |
| `sc.FP.Menu 0\|1` | teleport menu |
| `sc.FP.Teleport <x y z>` / `sc.FP.Place <x y z>` | vehicle-relative floor hint (checked) / raw capsule placement (noclip tests) |
| `sc.FP.Embed [cm]` | noclip on, capsule centre pushed this far into the surface under the crosshair (noclip-exit refusal test) |
| `sc.FP.Where`, `sc.FP.Probe [cm]` | log state (`FP_WHERE`) / what the view ray hits and the focused interaction (`FP_PROBE`) |
| `sc.SpawnWhereLooking` | the Orbit button |
| `sc.Fragment <fragment> [tag]` | play a Mannequin fragment of the loaded vehicle (any camera mode); logs `FRAGMENT_CMD fragment tag ok` |

`-SCScript=<file>` runs a route once a load has finished (after `-SCCamera`): each
non-comment line is `<delay seconds> <console command>`, delays count after the
previous step (and after a timed `sc.FP.Move` ends). Every step logs
`SCRIPT step=<n> cmd="..." mode=... pos=<vehicle-relative cm> yaw pitch speed floor crouched noclip stuck recover`,
the end logs `SCRIPT_DONE`. Pseudo-commands: `shot <tag> [hold s]` logs
`SCRIPT_SHOT tag=<tag>` and holds still (default 3 s) for an external screenshot;
`frametime <seconds> <tag>` logs `SCRIPT_FRAMETIME` (frame, game-thread, render-thread
and GPU ms: mean/p50/p95, the `stat unit` sources). `quit` ends the app from the route.

`ue/scripts/capture_route.sh <tag> <route> [host args]` launches the host like
`capture_viewer.sh`, screenshots the window at every `SCRIPT_SHOT` into
`out/smoke/<tag>-<shot>.png`, stops UE after `SCRIPT_DONE` (or reports the exit status
of a route ending in `quit`) and prints the route log. Routes live in
`ue/scripts/routes/`. Walking routes use spawn kinds and relative moves only; a route that
only frames a shot may put the noclip camera at a vehicle-relative point with `sc.FP.Place`
(`gladius-hull-seat.txt`, and `gladius-gear-retract.txt`, which plays the landing-gear
`Retract` fragment at 1/20 speed with `sc.Fragment` and `slomo`).

`-SCLightProbe` adds a light audit to every `shot`: for the player position and for the
surface under the crosshair it logs `LIGHT_PROBE tag=<shot> at=player|view point=...
channels=<bits> lights=<n> lux_unshadowed=... lux_visible=... gated=<n> lux_gated=...`
followed by one line per contributing light (lux, candela, distance, radius, cone factor,
the actor blocking it). It is inverse-square with the importer's candela, attenuation
radius and cone; a line trace stands in for shadows, gobos and the bulb clamp are
ignored; `gated` counts lights excluded by lighting channel. Every import also logs
`LIGHT_AREAS volumes=... rooms=... room_channels=<room>:<channel>,... reach=<A>><B>(lights,deepest m[,shared]),...
conflicts=... lights_inside=... lights_outside=... lights_everywhere=... meshes_inside=... meshes_outside=...
samples_other_channel=... environment_lights=...`: how the package's room volumes split lights and meshes between
lighting channel 0 (outside) and the rooms' channels 1 and 2, which room pairs an area-only light reaches, and which
of those had to share a channel. See [WP-U3b verification §12](verification/WP-U3b.md) and
[WP-L1](verification/WP-L1.md).

See [WP-P1 verification](verification/WP-P1.md) for exact null-RHI commands,
results, collision comparison, limitations, and the orchestrator's GPU checklist.

## Armed first person: the real player body (WP-P2)

Entering first person the first time starts the **player kit** in the background:
`scbridge_export_player` (D10 cache in `./cache/`; ~9 s cold, <1 s cached) → tar parse →
import (unpublished actor, no collision, every skinned part a rig) → body-clip sidecars read on
workers (~3 s). Until it is ready — and if it fails — first person is the WP-P1 capsule; the HUD
shows the progress or the failure. The kit is kept for later first-person sessions.

Options (settings in `GameUserSettings.ini`, command line overrides one run):

| Setting / switch | Meaning |
| --- | --- |
| `PlayerGender` / `-SCPlayerGender=male\|female` | body; empty = the exporter default (male) |
| `PlayerLoadout` / `-SCPlayerLoadout=<p4k xml>` | e.g. `Data/Scripts/Loadouts/Player/DefaultLoadouts/PVE_medium_behr_rifle_gmni_rifle.xml` (P4-AR); empty = the exporter's default loadout |
| `-SCPlayerPackage=<kit tar>` | read the kit from disk instead of exporting (no archive needed) |
| `-SCPlayerWeapon=<slot\|none>`, `-SCPlayerView=fp\|tp[,yaw[,distance]]` | applied once the body is attached (for `capture_viewer.sh`) |
| `-SCPlayerDemo[=<entity>]` | runs `ue/scripts/routes/player-demo.txt` (loads `<entity>` if given, enters first person) |

Keys (same `KeyTable` for Slate and native input): `1`–`9` draw the stowed weapon in that slot
(slots: stowed weapons with playable select/deselect, ordered by size, then stow-port name — the
P4-AR loadout gives 1 = P4-AR, 2 = S71), the active slot's key or `H` holsters, another slot's key
swaps (deselect, then select), keys are ignored mid-transition; holding the right mouse button aims
down the sights; `C` toggles a third-person orbit.
E, F, V, T work as in WP-P1 in every weapon state (in third person E reaches from the character's
eye — the camera joint — along its view, not from the orbit camera).

What drives the body (all names from the scene's `player` block; Mannequin vocabulary only:
fragment ids `MotionIdle`/`MotionMovement`/`select`/`deselect`, stance tags `stand`/`crouch`,
blend-space parameters `MoveSpeed`/`TravelAngle`):

- `USCPlayerBodyComponent` (on `ASCFirstPersonCharacter`): leader rig = the root part's rig, every
  other rig on the body skeleton follows by joint name; feet on the capsule floor, facing the
  root-motion joint's native +Y; items on their port joints aligned by `item_offset_helper`
  (stow frames come from the port owner's same-named helper when the owner item is placed, e.g.
  the backpack's `wep_stocked_attach_*_override`). Parts with port flags `invis_p1`/`HeadWear` or
  skinned mostly below the camera joint's parent are hidden in first person and keep casting
  shadows (`bCastHiddenShadow`); package-root identity does not imply "head". While head wear is
  worn, parts whose submaterials are all `HairPBR` (hair cards, scalp cap, brows, lashes, beard) are
  hidden in both views (they are authored for the bare head and pass through helmets). Camera at
  `camera_bone`, orientation = control rotation, near plane 2 cm (`USCEyeCameraComponent`).
- **Walls (round 2)**: each first-person frame traces from the eye to the held weapon's box corners
  (all of its primitives, in its anchor frame) and to the hand joints. When a probe reaches into
  the world, the engine's first-person rendering takes over: every visible kit primitive becomes a
  `FirstPerson` primitive and the camera's `FirstPersonScale` = the largest scale that keeps every
  probe 1.5 cm in front of the surface (min 0.04; near plane follows, floor 0.2 cm). The image is
  unchanged (scaling toward the eye keeps the projection), only depth: the weapon never cuts into a
  wall or door. The scale returns to 1 over ~0.3 s; first-person primitives cast no shadows (engine
  rule), so the body's shadow (and the hidden head's) is off only while a wall is in reach; the
  primitives switch back 0.5 s after the scale is 1 again. Third person never uses it. Log:
  `PLAYER_FP_PRIMITIVES`.
- `FSCPlayerAnimator` (plain C++): state = (weapon, stance, move-speed tag, fp/tp); idle/movement
  = the fragment option with the fewest query fragment tags; movement blend spaces sampled at
  (ground speed, travel angle) as the closest point of the authored faces, examples phase-locked;
  cross-fades with the fragment's `blend_in`; additive layers `local = delta * base` (parent
  space), translation added; select/deselect one-shots on top; root-motion joint pinned; view pitch
  spread over the camera joint's ancestors below the pelvis (interim: aim poses are not exported).
  Walk / run speeds = nominal forward example speeds of the slowest / fastest move-speed tag
  (P4-AR: 357 / 601 cm/s, unarmed 345 / 600). Running is allowed only where the fast tag's movement
  space covers the intended direction (movement input); the same decision picks the animation tag
  and the capsule's `MaxWalkSpeed` (Shift + strafe moves at walk speed). Combined (`.comb`) spaces
  evaluate their choosing selector first (the first component, in exported order, whose examples
  span the value), then interpolate that component only; an unevaluable selector makes the
  fragment unplayable (`PLAYER_KIT_DIAGNOSTIC`). Additive layers run on their own clock (start
  time + seconds × speed, looped or held). TravelAngle is sticky at ±π (some unarmed/crouch spaces
  author different clips at +π and −π). A queued weapon swap starts the new draw with the time
  left in the frame on both the animation and state-machine clocks. A weapon whose draw, holster
  or armed idle references an unreadable clip is not given a slot (`PLAYER_SLOT_DISABLED`); an
  unreadable unarmed idle fails the kit (capsule only; the kit actor and data are destroyed). Off-hand two-bone IK to the weapon's off-hand grip
  helper is implemented but off by default (`sc.Player.OffHandIK 1`): the game's clips author their
  left-weapon IK blend at 0 for these states.

- **Aim down sights (hold the right mouse button, `sc.FP.Aim 1|0`)**: first person with an armed
  weapon. The sight line is the game's `ADS_align` helper (the helper the camera aligns to when a
  weapon's DataCore `useAdsHelper` is set), read from the weapon GLB; an attachment that carries one
  (an optic on the sight rail, a nested assembly entry: `ASCVehicle::AttachmentHelpers`) replaces the
  weapon's own. Its axes are the game's (+Y along the line, +Z up) carried through the importer's
  basis (`USCPlayerBodyComponent::SightAxes`). The player package has no ADS clips or pose, so over
  0.25 s (viewer constant; DataCore `aimAction.zoomTime` is not exported) the arm roots — per hand
  joint, the first joint not on the camera joint's chain (`LeftShoulder`/`RightShoulder`) — take the
  world delta that puts the sight origin on the view axis and its line along the view (no cant); the
  head and eye do not move. The eye sits 2 cm (the near plane) behind the helper, or behind the rear
  end of an optic's geometry (DataCore `adsCameraOffset` is not exported either). Released, holstering
  or third person: back to the clip's pose. Log: `PLAYER_SIGHT`, `PLAYER_AIM`; `sc.Player.State`
  reports `sight_axis_cm` / `sight_angle_deg` (0 when aimed).
- **Depth of field (camera post-process override, viewer look)**: aiming focuses on what the view axis
  hits (trace ignoring the player) and blurs only the far field — the f-stop is solved so infinity
  blurs by `sc.Player.AimFarBlur` of the image width (default 0.012), and `r.DOF.Kernel.MaxForegroundRadius`
  is 0 while aiming (restored after) so the weapon, sights and anything nearer than the focus stay
  sharp (UE 5.9's diaphragm DOF has no focal region). Third person always focuses on the character's
  camera joint at `sc.Player.ThirdPersonFStop` (default 0.5), closed down as needed so the nearest
  visible part of the character blurs at most 1/750 of the width. `sc.Player.DofSensorWidth` (36 mm).
  No player override otherwise (the level's own look applies).

Console (first person): `sc.Player.Weapon <slot|none>`, `sc.Player.View fp|tp [yaw] [distance]`,
`sc.Player.State` (`PLAYER_STATE` log), `sc.Player.Load` (start/retry the kit), `sc.Player.Body 0|1`
(detach/re-attach — frame-time A/B), `sc.FP.Key <key> 0|1` (press/release a key-table key).
Route pseudo-commands: `waitkit [s]` holds the route until the body is attached; `waitpt [s]` holds it until the
path tracer has accumulated `r.PathTracing.SamplesPerPixel` (put it after `viewmode pathtracing`); `snap <tag>` is
`shot` without clearing held input (moving shots). `slomo <x>` (engine) slows the animation while
route delays stay real time (mid-select frames).

```bash
ue/scripts/capture_route.sh p2-cutlass ue/scripts/routes/cutlass-armed-walkthrough.txt -SCDefaults \
  -SCLoad=DRAK_Cutlass_Black -SCCamera=first \
  -SCPlayerLoadout=Data/Scripts/Loadouts/Player/DefaultLoadouts/PVE_medium_behr_rifle_gmni_rifle.xml
```

Null-RHI tests: `-run=pythonscript -script=ue/scripts/test_player.py -SCPlayerTests
[-SCPlayerPackage=<kit tar>]` (see [WP-P2 verification](verification/WP-P2.md)).
