# SCViewer — WP-U0 Unreal setup and feasibility spikes

**Engine: the SCViewer fork of UE 5.9, built from source, is the only build** (plan D17, 2026-11-18): branch
`scviewer/5.9` of `C:\Users\bojan\UnrealEngine-5.9-src` (upstream `5.9` plus engine features the viewer needs, WP-E1;
UE's behaviour stays the default), registered as `UE_5.9_src` (`SCViewer.uproject`'s `EngineAssociation`). Build the
engine once in the checkout (`Engine\Build\BatchFiles\Build.bat UnrealEditor Win64 Development -WaitMutex`), then the
project. Scripts (`viewer/scripts/rpc/SCRpc.psm1` `Start-SCViewer`, `package_showcase.ps1`, the `.sh` helpers) take
`-Engine` / `UE_ENGINE_ROOT` and default to `%USERPROFILE%\UnrealEngine-5.9-src` (`~/UnrealEngine-5.9-src`). The
launcher (binary) engine, the Web / Wasm build and the NVIDIA RTX-fork (UE 5.8) build were removed in WP-E1 (E1-0).
Editor-hosted -game sessions skip Python startup by default. Pass -EnablePython when a viewer session needs it.
The game host's command-line flag has priority over the Editor Preferences Python override.
The Lumen World Cache is off for now (plan D13); verification and the World Cache / G-buffer A/B routes:
[verification/WP-U59.md](verification/WP-U59.md). The shader patches of plan D14 (`ue/EnginePatches`, written for the binary
engine) are gone: WP-E1 replaced them with engine features (light falloff E1-2, projector shape E1-3, the game's SSDO E1-7:
`SSDO*` post-process settings and `r.SSDO*`, always on in the look since 2026-11-19; `sc.Look.SSDO game|off` switches it for one session, diagnostics only).
Glass reflectance is a look value too (2026-11-19, [verification/WP-U3b.md](verification/WP-U3b.md) §14.9): every glass
instance reflects `max(F0 × GlassReflectanceScale, GlassReflectanceFloor)` (the floor only where the game's F0 is not 0),
`USCViewerSettings` defaults, not saved; `sc.Look.GlassReflectance <scale> [<floor>]` sets both for one session
(`1 0` = the game's `.mtl` Specular).
Glass refraction is part of the look too (2026-11-19, [verification/WP-U3b.md](verification/WP-U3b.md) §14.10): every
`M_SC_Glass` instance refracts and colour-fringes the scene behind it through UE's distortion pass (`RefractionMethod` 2D offset,
the material's own offset; the fringing is the fork's per-channel distortion, WP-E1 E1-8, `r.Refraction.Dispersion 1`), always on
at every start and not saved, like SSDO; `sc.Look.GlassRefraction 0|1` switches it for one session (diagnostics; no argument logs
the state; key G is gone). Each glass instance takes the owner's profile, chosen from data (`SCLook::GlassProfile*`): glass on the
first-person player kit (weapon optics, visor) strength 0.125 / dispersion 0.25; other glass authored with F0 0 (the C2 canopy)
1 / 0.25; all other glass 5 / 0.5. IOR and thickness are `M_SC_Glass` defaults; any of them is live with
`sc.Look.GlassParam <name> <value> [@<instance name part> | @kit]` (static and skeletal parts). Cooked ships and hangars keep
working without a re-cook: their constant glass instances reference `M_SC_Glass` (the cook never bakes the switch) and each load
puts a dynamic instance with its profile over them; a packaged build has to be re-packaged to carry the new master.
History: the project started on UE 5.8.2 (`++UE5+Release-5.8`, CL 56702186, Linux, WP-U0 2026-11-09), moved to the
UE 5.9.0 preview-1 binary (CL 60351404) on 2026-11-12 and to the source fork on 2026-11-18.
WP-U0 on 5.8.2, `df -h .` on 2026-11-09: **1.6T total, 1.5T used**; available space was
**82G / 95% used** at start and **76G / 96% used** at final verification.
The probe's project build products occupy about 434 MiB, plus 17 MiB under
`./cache/spike/`; no engine source was changed. Logs/build products are ignored.

**Win64** (D3D12 SM6: `Config/Windows/WindowsEngine.ini`; the look is the shared one). The FFI is staged with
`scripts/build_ffi.ps1` (`scbridge.dll` + import library; every build links it) and the build is
`C:\Users\bojan\UnrealEngine-5.9-src\Engine\Build\BatchFiles\Build.bat SCViewerEditor Win64 Development -Project=<abs>\viewer\SCViewer.uproject -WaitMutex`.
`-SCShotDir=<dir>` saves route shots in-engine (no `capture_route.sh` on Windows). NVIDIA DLSS is enabled on Win64
(`.uproject` entry, `Optional`; `r.NGX.DLSS.Enable 1`): DLAA at the default 100 % screen percentage, DLSS Super
Resolution below it (`r.ScreenPercentage 67` = Quality). The plugin is NVIDIA's download (developer.nvidia.com/rtx/dlss,
not redistributable): `powershell -File scripts/install_dlss.ps1 -Zip <zip>` installs DLSS and its dependency
StreamlineNGXCommon into `plugins/NVIDIA/` (gitignored) and applies the casts UE 5.9's compile-time format check
needs in NVIDIA's 8.7.2 (UE 5.8) source; without the plugin the project builds and runs with TSR.
`viewer/scripts/routes/dlss-modes.txt` compares DLAA / TSR / DLSS Quality (Golem, small hightech hangar, RTX 4090, 1080p,
GPU ms p50: 7.22 / 7.39 / 5.22). `install_dlss.ps1` also declares UE 5.9 in the two `.uplugin` files: with NVIDIA's 5.8.0
an unattended cook skips the plugin, and a packaged game then stops at start ("Missing global shader FVelocityCombineCS").
The DLSS plugin is a project plugin, so UBT compiles it against the source fork with the project.

**Win64 showcase (standalone directory):** one cooked ship and one cooked hangar, dynamic lighting:
Nanite, Lumen GI and reflections at Cinematic with hardware ray tracing and hit lighting for GI and reflections
(`-SCMaxQuality`, `SCLook::ApplyMaxQuality`), DLAA. Cook at full resolution (`--lod 0 --mip 0` exports, the hangar-cook
standard), then `powershell -File game/scripts/package_showcase.ps1 -Ship DRAK_Golem -Hangar
hightech_a/hangar_smltop_001 -Config Shipping` writes `out/win64/<ship>`: run `Play_SCViewer.bat` there (a Windows build
does not read the staged `UECommandLine.txt`, so the launcher passes `-SCLoad`, `-SCEnvironment` and `-SCMaxQuality`;
`SCViewer.exe` alone is the plain viewer). Shipping keeps no log: `-SCQualityReport=<file>` writes the effective settings,
and `viewer/scripts/routes/showcase-max-quality.txt` takes a shot with and without hit lighting (`-SCScript=... -SCShotDir=...`).
Golem, small hightech hangar (2026-11-14, RTX 4090, Shipping): 1.7 GB of content; the report reads
`gi=4 reflections=4 lumen_hwrt=1 lighting_mode=1 hit_shadow_mode=2 probe_downsample=8 nanite=1 dlss=1 screen_percentage=100`.

The launcher starts the showcase as a player (`-SCShowcase -SCCamera=first`): no viewer top bar, footer or panels; the
first-person HUD and its teleport menu stay. Keys: WASD/Shift/Space/Ctrl move, `C` first/third person, `Z` held orbits
the third-person camera around the player (double-tap: behind again), `T` teleport menu, `F` torch, `1`.. draw a weapon,
right mouse aims down sights, left mouse fires. The player character is exported from Data.p4k at start (so the build
needs an installed Star Citizen; a cooked player kit is a follow-up): `package_showcase.ps1 -PlayerLoadout <p4k path>
-PlayerWeapons <entity>[,...]` (launcher `-SCPlayerLoadout`, `-SCPlayerWeapons`; settings `PlayerLoadout`,
`PlayerWeapons`, `bPlayerSpareAttachments`) picks the game loadout, keeps only the named weapons and carries their spare
magazines on the armour's storage ports (the exporter's `PlayerRequest.weapons` / `spare_attachments`, FFI
`scbridge_export_player_request`). The showcase uses `StarMarine_Marine.xml` with `behr_rifle_ballistic_01` (P4-AR).
Firing (`FSCPlayerFire`) uses the weapon's first authored fire action: its rate (810 RPM rapid), the fragments of its
enslavement ADB on the weapon's own rig (trigger, looping bolt; stop fragment on release) and its launch helper; flash,
tracer, impact spark and view kick are viewer stand-ins (the game's particles and procedural recoil are not imported; a
ballistic look is the next iteration). The torch is the game's personal light: the kit's `player.flashlights[0]` (the worn
helmet's `SCItemFlashlightParams.localPlayerParams`, its cookie exported as the gobo) mapped by
`FSCLightImporter::ApplyFlashlight` (game light model, square projector of half-angle fov / 2, cookie as light function;
LIVE helmets: diffuseMult 20 ≈ 63 500 cd, 40 m, 90°). Without a body or with a loadout that has no flashlight it is the
viewer's 40 000 cd cone (`FP_TORCH_LIGHT source=fallback`). It is Movable (a Stationary light does not follow the player)
and sits ahead of the helmet (the game's `helperName` / `helperOffset` on `HeadCam` are not used). `viewer/scripts/routes/showcase-player.txt` checks torch, firing, aiming, third person and `Z`.

Preparing the showcase content (WP-O1-4): UnrealCitizen's prep step imports a scenario's hangar and ship from the
archive into the game project as levels: `UnrealEditor-Cmd game/UnrealCitizen.uproject -run=SCPrepare
-Scenario=golem_showcase -AllowCommandletRendering -unattended` (`game/Config/Scenarios/golem_showcase.json`; see
`SCPrepareCommandlet.h`). A prepared ship is `/Game/SC/Ships/Maps/<entity>[__<paint>]` (gear down, on the floor, rigs
baked static, pool `/Game/SC/Ships/Shared`); the game runs the scenario from it (`-Scenario=<name>`) with or without an
archive. The viewer itself never streams prepared levels (WP-O1-3).

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
  "$PWD/viewer/SCViewer.uproject" \
  -game -windowed -resx=1920 -resy=1080 -vulkan -ddc=SCViewerDDC
```

## GPU validation (2026-11-09, host B on the RTX 4090)

Run by the orchestrator outside the Codex sandbox with the launch command above
(`viewer/scripts/run_spike.sh load`, then `-SCSpikeStay` runs with `-ExecCmds` for the
visualisation modes). Evidence: `viewer/verification/wp-u0/*.png` (half size).

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
viewer/scripts/build.sh SCViewerEditor
viewer/scripts/build.sh SCViewer                 # expected RulesError for Spike A
SC_SPIKE_RAW_SERIALIZE=1 viewer/scripts/build.sh SCViewerEditor -NoUBTMakefiles
                                             # expected link failure for Spike D
viewer/scripts/build.sh SCViewerEditor -NoUBTMakefiles  # restore working probe
viewer/scripts/run_spike.sh save                  # build, cook, save, then quit
viewer/scripts/run_spike.sh load                  # fresh process: load and trace
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
  "$PWD/viewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/viewer/scripts/headless_smoke.py" \
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
Full logs are local in the ignored `Saved/Spikes/` directory.

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
- Ships, Garage imports and environments reuse cooked static meshes and Chaos collision
  from the launch-relative `cache/processed`. Entries are keyed by the input geometry,
  archive build or folder namespace, renderer format, and import options. The first
  load builds and saves the cache; a hit restores the cooked mesh while Nanite requests
  fine geometry pages as needed. The importer still places components and binds lights
  and materials on each fresh load. See [WP-C1](../docs/work-packages/WP-C1-processed-cache.md)
  for the XL hangar and C2 measurements.
- `-SCStreamGeometry` enables the cold-load preview in Editor `-game`. Eligible
  cold mesh variants start with smaller render proxies, then build their exact
  meshes when the camera approaches or sees enough projected detail. The source
  collision and rig bindings are installed at assembly. Processed cache hits
  restore the exact geometry directly. The profile uses approximate lighting on
  coarse proxies, and fixed-step render capture prepares exact geometry before frame one.

From the repository root:

```bash
viewer/scripts/build.sh SCViewerEditor
viewer/scripts/run_import.sh out/dec -vulkan -SCStay
# Headless game-host parity, trace and GC checks:
python3 -I viewer/scripts/make_test_tar.py out/dec viewer/Saved/Import/dec.tar
viewer/scripts/run_import.sh out/dec -nullrhi \
  -SCVerifyTar="$PWD/viewer/Saved/Import/dec.tar"
```

The editor File menu contains **Import SC package folder…**. Runtime callers can
also use `sc.Import <folder>` or `USCImportLibrary::ImportFolder` (these convenience
entry points lift the bounds one metre above the Viewer floor); Python exposes
`unreal.SCImportLibrary`. `viewer/scripts/import_package.py` is the headless import
entry point. `viewer/scripts/test_importer.py` runs the synthetic automation tests;
passing `-SCPackage` and `-SCVerifyTar` also runs the real folder/tar integration
test. `game/scripts/interchange_baseline.py` accepts `-SCBaseline=<absolute GLB>`.
Use these scripts with `UnrealEditor-Cmd -run=pythonscript -script=<absolute script>
-nullrhi -unattended -nop4 -nosplash -ddc=SCViewerDDC`.

The old WP-U0 spike is quarantined behind `SC_SPIKE` (off by default). To reproduce
it, explicitly rebuild with
`SC_SPIKE=1 viewer/scripts/build.sh SCViewerEditor -NoUBTMakefiles`, then use
`run_spike.sh`. Restore the default importer build afterward.

Evidence, exact acceptance commands, input-revision notes and GPU TODOs are in
[verification/WP-U1.md](verification/WP-U1.md). This sandbox has no Vulkan device;
CPU representations are verified, while rendered Nanite/Lumen/HWRT acceptance
must be completed on the orchestrator's GPU.
WP-U2 materials (Substrate decision, masters, builder, textures, macro surface / HAL / gloss convention,
HardSurface static wear, palette finish, PNG colour encoding, energy split):
[verification/WP-U2.md](verification/WP-U2.md).

## The viewer's menu (WP-O1-3)

F1 (or the top bar's "Menu (F1)") opens the picker: an environment (the game's player hangars), a vehicle (one row per
ship a player would recognise) and the loaded vehicle's paint. A pick applies at once. The lists come from game data
(`scviewer catalog` prints them; rules in [WP-O1](../docs/work-packages/WP-O1-unrealcitizen-split.md) O1-3). The menu
opens by itself at start when the command line asks for no scene. The command line can choose all three:
`-SCEnvironment=<container path> -SCLoad=<entity> -SCSkin=<index|default|name part>`. The viewer always imports from
the archive through the local cache; it does not cook or stream cooked levels.

## WP-V1 viewer app (load pipeline, cameras)

The game mode `ASCViewerGameMode` (set in `DefaultEngine.ini`) shows the in-game
Slate UI in every host. Run from the repository root (the cache defaults to
`./cache/`, resolved from the launch directory):

```powershell
& "$env:USERPROFILE\UnrealEngine-5.9-src\Engine\Binaries\Win64\UnrealEditor.exe" "$PWD\viewer\SCViewer.uproject" `
  -game -windowed -resx=1920 -resy=1080 -ddc=SCViewerDDC
```

Pick from the menu (F1); drag to orbit, Shift/middle-drag to pan, wheel to zoom,
double-click a part to focus, F to frame all; Fly mode uses WASD/QE with Shift/Ctrl.
Switches: `-SCArchive=<Data.p4k>`, `-SCCacheDir=<dir|none>`, `-SCSearch=<q>` (fills the menu's vehicle search),
`-SCLoad=a,b`, `-SCHold=<s>`, `-SCCamera=fly`, `-SCFlyView=dx,dy,dz,pitch,yaw`,
`-SCDefaults`, `-SCLod=`, `-SCMip=`, `-SCQuitAfterLoad`, `-SCNoUI`, `-SCHostExposure`; console `sc.Load`,
`sc.Unload`, `sc.Cancel`, `sc.Camera`. `viewer/scripts/capture_viewer.sh` screenshots a run.
**Dirt** (WP-U2b, [verification/WP-U2b.md](verification/WP-U2b.md)): `sc.Dirt <amount> [preset]` (Dust / Sand / Mud /
Ash; the options panel with its slider went with the side panels in WP-O1-3) puts the game's dirt rule on exterior
surfaces, saved per user. The game's ships author no dirt; the defaults are viewer choices.

Headless (WP-Q1): `UnrealEditor-Cmd … -run=pythonscript -script=viewer/scripts/ue_load_gladius.py
-nullrhi` drives `USCViewerSubsystem.LoadEntity` through the FFI. Settings persist in
`Saved/Config/<Platform>/GameUserSettings.ini`. Evidence: [verification/WP-V1.md](verification/WP-V1.md).

**Extra vehicle instances** (WP-K1f, [verification/WP-K1.md](verification/WP-K1.md) "K1f"): more vehicles beside the
loaded one, e.g. several Hull A in one hangar. `-SCExtraVehicles=<Entity>[*N][,...]` (an empty entity, `*2`, is the
loaded one), `sc.Vehicle.Spawn [entity|loaded] [count]` (free floor), `sc.Vehicle.SpawnAt <entity|loaded> <x m> <y m>
[floor z m] [yaw]`, `sc.Vehicle.List`, `sc.Vehicle.Destroy <index>`, `sc.Vehicle.Clips <index>`,
`sc.Vehicle.Clip <index> <clip id|part> [speed]`, `sc.Vehicle.Focus <index|all>`; log `VEHICLE_SPAWN idx=.. entity=..
at=..`. Instances of an imported entity are assembled from its retained import (`sc.Vehicle.ShareAssets 1`, default:
shared meshes, rigs, textures and materials; no export, derivation or build), each with its own components, rig pose,
collision, lights and exploration data. Auto placement: the vehicle's envelope (solid bounds at every clip's last
frame) plus a 3 m walkway on flat floor at the loaded vehicle's floor height, nearest its footprint first, clear of
geometry and of the environment's inventory-container access lanes (lattice + the largest permitted item edge);
re-placed when the environment changes. They unload with the loaded vehicle. API: `USCViewerSubsystem::
SpawnVehicleInstance / DestroyVehicleInstance / GetVehicles / GetVehicleInstances / OnVehiclesChanged`.

**Cargo by drones** (WP-K1, [brief](../docs/work-packages/WP-K1-cargo-drones.md),
[verification/WP-K1.md](verification/WP-K1.md)): cargo grids come from the package (`inventory_containers`), boxes are
exported box entities, drones are stand-in art (`/SCBridge/Cargo`, rebuilt by `game/scripts/create_cargo_art.py`).
Scenarios (`game/Config/Cargo/*.json`, `-SCCargoScenario=<name>`) set the stock, docks and owners:

| Scenario | Host switches | Route |
|---|---|---|
| `hull_a_hangar` | `-SCLoad=MISC_Hull_A -SCEnvironment=Data/ObjectContainers/PU/loc/mod/common/hangar/hightech_a/hangar_lrgtop_001.socpak` | `cargo-hull-a.txt`, `mobiglas-live.txt` |
| `hull_a_multi` (3 Hull A, 3 owners) | as above | `cargo-multi.txt`, `mobiglas-multi.txt` |
| `c2_hangar` | `-SCLoad=CRUS_Starlifter_C2 -SCEnvironment=…/hightech_a/hangar_xltop_001.socpak` | `cargo-c2.txt` |

Add `-SCCamera=first` (and `-SCScript=viewer/scripts/routes/<route> -SCShotDir=<dir>` for a route). **F1** opens the
mobiGlas Cargo app (Esc closes): every cargo grid in the level, nearest first (`CargoGridListRadiusM` > 0 limits it),
grid detail, transfer composer (from, to, commodity, amount in whole boxes, fit preview with refusals), transfers in
progress; the view becomes an orbit camera (drag, wheel) around the selected ship or elevator. World callouts label
boxes in flight and drones; grids too while the mobiGlas is open. What moves as one has one callout: a box and the
drones holding it (their names on a third line), a team flying to or from its box, a lone drone.
Console: `sc.Cargo.List`, `sc.Cargo.Preview|Transfer <from> <to> <commodity> <scu> [--owner=<id>]` (grid id,
`ship`/`elevator` or label text), `sc.Cargo.Cancel <job>`, `sc.Cargo.Access <grid|ship> open|close`, `sc.Cargo.Debug
0|1` (lattices, slots, routes), `sc.Cargo.Section <grid>` (what route queries see), `sc.MobiGlas [0|1]` and
`sc.MobiGlas.*` (route stand-ins for clicks), `sc.Env.OpenAccess`. Logs: `CARGO_JOB`, `CARGO_PATH` (found, hits,
clearance, frame time), `CARGO_TEAM`, `CARGO_PICK`, `CARGO_TUG`, `CARGO_GUARD`, `ENV_ACCESS`.

**Live cargo spawning** (a developer feature: console and RPC `/console` only; `Cargo/SCCargoSpawn.h`, `SCCargoSpawnLive.cpp`;
tests `SCBridge.Cargo.Spawn*`): stock without a scenario, while the viewer runs.
- `sc.Cargo.Spawn <grids> <commodity|entity> [<scu>|x<count>] [--size=<scu>] [--owner=<id>]`: grids as for
  `sc.Cargo.Transfer` (id, `ship` / `elevator` (the player's ships, every lift grid), label text; `+` joins); `--owner`
  names whose the new boxes are (else the grid's owner, else the player). `<scu>` is a total in whole boxes (`--size`
  boxes, or from the largest sizes that fit down, then other exact mixes); `x<count>` is a box count (of `--size`, or
  the largest size that fits); neither is one box. Amounts and sizes are whole numbers from 1 (`0`, `x0`, `2.5`,
  `8junk` are refused, never read as one box); at most 256 boxes a spawn.
  Boxes come from the game's box records: the entity-index search `CargoBoxSearch` (setting, default `Carryable_TBO_`;
  a name filter for speed, empty = every entity) and each hit's DataCore record, kept when it is a whole-SCU box
  (`AttachDef.inventoryOccupancyVolume`, `inventoryOccupancyDimensions`) holding a resource (`ResourceContainer`: its
  default composition's `ResourceType`, else its `ResourceTypeGroup`: a mixed box such as the scenarios' `Organic`).
  The commodity is that record's name (`CarinitePure`, `Organic`), matched by key, label (the localised name its boxes
  share; one that several commodities' boxes carry, a placeholder, is no label) or text unique to one; an exact box
  entity name picks that box. Read once per archive, off the game thread, on the first `sc.Cargo.Spawn` /
  `sc.Cargo.Catalog` (1328 records in about 5 s on LIVE 4.10.2; `CARGO_CATALOG`).
  Placement is the planner's packing (largest first, best fit, upright at yaw 0/90, on the floor or on boxes, a hold
  lengthwise to its entrance). Slots reserved for incoming boxes, boxes about to leave and the return slots of boxes held
  aside by a dig-out occupy their cells but bear nothing. The boxes go down once their packages are exported (`SpawnBox`,
  as scenario stock), planned again then, all or none.
  Logs: `CARGO_SPAWN refused=<grid|retracted|full|size|amount|commodity|catalog|package> grid= what= reason="…" [id=]`,
  `CARGO_SPAWN_QUEUED id=`, `CARGO_SPAWN grid= commodity= boxes= scu= owner= id= ids=`.
- `sc.Cargo.Clear <grids>`: removes the boxes on grids (or beacons) that no transfer holds, except those beneath a box of
  a transfer, a reserved incoming slot or a dock, and (off the lattice, by bounds) beneath a held loose box (`CARGO_CLEAR`).
- `sc.Cargo.Catalog [text]`: the commodities and box sizes the records offer, listed once read (`CARGO_CATALOG_COMMODITY`).
- A session without a scenario takes on the default drone fleet with its first valid spawn (`Config/Cargo/fleet.json`,
  setting `CargoFleet`, `-SCCargoFleet=<file|none>`; `CARGO_FLEET`): the scenarios' drone definition and two docks of two
  drones on the environment's first grid (the freight elevator; put down when that grid is there, before the spawn's
  boxes), so spawned cargo can be transferred by the hangar's shared pool.
- Scripts: RPC `/console` (`Invoke-SC /console @{ cmd = 'sc.Cargo.Spawn elevator Organic 96' }`), then `/wait`
  `until = 'idle'` (activities `cargo_catalog`, `cargo_spawns`).

**Loose cargo** (WP-K2, [brief](../docs/work-packages/WP-K2-cargo-beacons.md)): a load let go of in flight is set down on
the nearest free slot it can still reach, else let down where it is. A loose box at rest belongs to a freestyle cargo
beacon: a virtual location with a random name ("Loose cargo Kestrel-86") that remembers where its boxes came from.
Beacons are transfer sources (a beacon id or name, `beacon` / `loose` for all; never a destination, for now), listed
in the mobiGlas with their counts only. Verification hooks: `sc.Cargo.Interrupt [job]` (a load in flight fails as a
lost route would), `sc.Cargo.LoseDrone busy`; the RPC puts boxes down loose (`/cargo/spawn` with `at` or `random`).
Logs: `CARGO_LOOSE`, `CARGO_BEACON`, `CARGO_RETARGET`, `CARGO_LOOSE_SPOT`.

**Cargo in space** (WP-K3, [brief](../docs/work-packages/WP-K3-cargo-in-space.md)): `-SCCargoScenario=hull_a_space`
loads two Hull A among asteroids with no hangar, zero gravity and a sun (8000 lux and a fill light; `sc.Space.Sun <lux>`,
scenario `sun_lux`); `hull_a_space_fleet` has four Hull A for exploring transfers by hand; scenarios gain `space`, `seed`, vehicle poses
(`at`, `yaw` / `rot`), `obstacles` (game entities placed as static instances) and `loose` stock. In zero g one drone
holds any box its beam takes by volume, and loose cargo holds station where it was let go.

How a team carries a box (`Cargo/SCCargoTug.h`, `SCCargoTugStep.cpp`; tests `SCBridge.Cargo.Tug*`): the box in the air
is a simulated body with its mass, and every drone's beam is a spring-damper on its own attach point. The beam's
numbers are the tractor record's (`maxForce` caps it, full up to `fullStrengthDistance`, nothing at `maxDistance`;
stiffness `maxForce / minDistance`, a viewer reading: the record has none; critical damping). Only the force is
shared: what the route asks for (weight, the route's acceleration along it and round its bends, the body's drag) is
split among the attach points with no moment. Each drone then decides for itself, every tick: its beam along its
share, as long as the beam keeps full strength and a sweep along it finds room (so a drone tucks in under a door or a
ceiling and its teammate need not), the rest length of its spring (within the record's `minDistance`..`maxDistance`),
and from that where it hovers. It flies there as a body (`drone_mass_kg`, thrust less the beam's reaction, jerk
limited), keeps its beam on the box on the way by re-aiming it and paying it out, and is pushed about by its own
gusts (each drone its own spectrum and strength, bounded by `sway_amplitude_cm` / `sway_tilt_deg`), which the box
feels through the spring. A load's route reserves its drones at the shortest beam that holds the box
(`minDistance` plus the stretch for its share), and its speed profile keeps acceleration along and across the route
together within what the team can give straight up. The guard sweeps the route ahead along itself as far as the
load needs to brake, and a load that must stop brakes at its own deceleration. On the lift, in a destination's
precise zone and on the descent onto the slot the box is held rigidly (not simulated): it passes boxes at rest 2 cm
from them. `CARGO_TUG` logs each beam's length, rest length and force once a second.
Drones do not collide with each other, with a box or with their surroundings, and no switch turns that off. Every
step a drone takes outside its dock, however it is flown, is its own (`ASCCargoDrone::Step`): a drone flying alone
steps out of the way of a load another team flies (`FSCCargoDroneSteer::Evade`: level, across the load's way), every
drone keeps apart from the others by their positions and velocities (`Separate`), and the step is swept with the
bounds of hull and rotors and slid along what it meets (`Slide`). What a step still ends in is logged
(`CARGO_DRONE_OVERLAP`) and totalled on every `CARGO_JOB` line (`overlaps=N`); the four cargo routes end with
`overlaps=0`. The planner and each drone's flight keep that step from having to act:
- a team's drones look along the route they fly and come in onto a shorter beam before a lintel, as early as their
  bodies need to (`FSCCargoTug::RoomAhead`, the room over the route per 40 cm stretch); one held up all the same
  tucks in to its shortest beam, and the route waits for it;
- route queries keep space for drones (`FSCCargoKeepOut`, `KeptSpace`): the stations of a team from when its move is
  planned, drones hovering attached to a box or holding one, drones over a load that is an obstacle; a lone drone's
  route keeps the distance drones keep from each other, also from its teammates' stations. A plan stopped only by
  that space is tried again (`CARGO_PLAN_WAIT`), not counted as a failure of the box;
- a drone on its way out holds short of its station while a load planned before its own move has yet to fly
  through there, or another team's drone is in its place (`CARGO_HOLD_SHORT`; the flight ends there for now,
  `FSCCargoFlight::HoldAt`);
- a docked drone leaves its bay when its way out (bay, exit, clear of the dock's grid) is free of every drone in the
  air, and a drone in the air stops short of a dock corridor in use (`FSCCargoDroneSteer::FreeAlong`): two bays
  nearer to each other than the drones keep apart launch and dock one at a time. Between bay and exit the dock
  moves the drone; one whose exit is not free (a retracted cargo plate beside the dock) is not sent
  (`CARGO_DRONE_BAY_BLOCKED`);
- teammates leaving one dock fly beside each other: the second's route runs the kept distance to the side of the
  first's (`FSCCargoDroneSteer::Beside`, `CARGO_APART`), swept like any route;
- a box that tilts as it is caught over its slot (onto an upright plate) waits until its drones are out of the
  sphere it sweeps; they fly round it as it turns and come in along the plate's normal as it goes down;
- loads keep apart by their right of way, which reaches as far as the drones over them.
Evidence, job times and what is not covered: [verification/WP-K1.md](verification/WP-K1.md), K1h.

**Game photometry** ([verification/WP-U3b.md](verification/WP-U3b.md) §13). Light candela, emissive cd/m²,
auto exposure and the display curve follow the installed game's renderer. The values the Viewer cannot read
at run time (cvar defaults of the executable, shader constants) live in the extracted fixture
`plugins/SCBridge/Resources/StarEngineRender.json` (provenance `StarEngineRender.notes.md`; no in-code
fallback — a missing or out-of-domain fixture is fatal). Re-check it after every game patch:
`python3 -I scripts/verify_star_engine_render.py` → `STAR_ENGINE_RENDER OK: 47/47 checks match the installed
game`. At `StartPlay` the game mode spawns an unbound post-process volume above the map's own
(`SCViewerExposure::Install`, log line `VIEWER_GAME_EXPOSURE …`): histogram exposure with the game's
EV-compensation curve, multiplier clamp and discard ratios, the game's meter (`r_exposureMeterMode` 3: the reflected meter
blended towards an incident one, clamped; the fork's Meter `Clamped Blend`) and its pupil / pigment adaptation (the fork's
two-stage adaptation; WP-E1 E1-5), the compensation curve baked over its key range
(`r.EyeAdaptation.ExposureCompensationCurveLUT.Domain=1` in `DefaultEngine.ini`), plus the game's display curve as the
engine fork's tone curve (Film > Method `Hue Preserving Shoulder` with the fixture's peak and toe, `FSCDisplayCurve`;
WP-E1 E1-4), neutral grading, no UE lens vignette and local exposure off; `sc.Look.Display ue` switches the volume
to UE's tonemapper as the project sets it, `sc.Look.Exposure ue` (or `ue game`, `game ue`: meter, then adaptation) to UE's
reflected meter and one-stage adaptation, `sc.Look.ExposureLog [seconds]` logs the exposure and both meters.
`SCBridge.ViewerExposure.DisplayCurve` and `.Meter` fail when the game's curve, meter or adaptation functions' hashes in
the fixture differ from the ones the engine ports were written from. `-SCHostExposure` skips the
volume (map exposure, UE tonemapper) for comparison captures.

## RPC harness and state log (WP-H1)

`-SCRpc[=port]` (8770) opens a localhost HTTP/JSON API for driving the viewer from scripts: load, spawn and despawn
ships, add and remove boxes and docks, transfers, grid access, camera, screenshots, video (`/record`, MP4 through a
hidden ffmpeg) and console commands, plus `/wait` until the scene is idle, settled, a job is finished or the lifts are idle. It also
writes `Saved/Logs/SCState.ndjson`: scene state (boxes with pose, velocity and slot error, drones with beam forces,
jobs), idle / settled events, warnings and anomalies, rotated by size (`-SCStateLogMaxMB`, `-SCStateLogFiles`).
Desktop non-Shipping builds only. The routes and records are in `docs/work-packages/WP-H1-rpc-harness.md`.

```powershell
Import-Module viewer/scripts/rpc/SCRpc.psm1
$p = Start-SCViewer -Extra '-SCCargoScenario=c2_hangar'      # off screen (-AgentMode); -Visible for a window
Invoke-SC /wait @{ until = 'settled'; timeout_s = 900 }
Invoke-SC /cargo/transfer @{ from = 'elevator'; to = 'ship'; commodity = 'organic'; scu = 32 }
Invoke-SC /screenshot @{ name = 'after' }
Stop-SCViewer $p
```

`game/scripts/rpc/explore.ps1 -Scenario <name> -Steps N -Seed S [-Video]` is the exploratory stability run (seeded
random scene changes, checked after every step; report in `out/h1/`).

## First-person exploration (WP-P1)

Load a package, then choose **First person** or **Spawn where I look** in Orbit.
Collision readiness includes the soft skin's animated triangle body. Click the
viewport for mouse look; Esc releases capture. WASD walks, Shift runs, Space jumps,
Ctrl crouches, E toggles the interaction under the crosshair (else the nearest
driven joint) within 3 m or presses a lift panel (on a floor panel, the floor under the crosshair, WP-P5), F toggles the camera torch, V toggles noclip (Space/Q
vertical), T opens the teleport list. One key table (`ASCFirstPersonCharacter::KeyTable`)
drives both Slate and native input.

Spawn kinds come from data: `Seat`, `SeatAccess`, `Door` (DataCore `AttachDef.Type`),
`Door` also for every interaction's driven joint, `Interior` (interior container
floors), `Hull` (above the collision bounds) and `Selected` ("spawn where I look",
replaced on each use, never ranked). The first-person start ranks
`Entrance` (only when the data marks a hull entrance — not exported yet) > `SeatAccess`
> `Seat` > `Interior` > `Door` > `Hull`. Interactions are Mannequin fragments with an
Open/Close tag pair on a rig the clip binds; their open state is the tag last played
on that rig instance.

**Teleport menu (T).** Two sections, each a heading over its own scrolling list (both headings stay in view): **Ship**
(the vehicle's spawns above) and **Hangar** (the loaded environment's places, kind `Hangar`). Up / Down (or W / S) move the
highlight across both sections (headings skipped, wrapping), Enter teleports there, a click teleports to the clicked row; T
or Esc closes. Movement keys pressed while the menu is open are ignored (nothing walks after it closes). The highlight and a
clicked row name their destination (section, index, label), so a list that changes while the menu is open does not send
the player elsewhere. Rows carry the `sc.FP.Spawns` numbers (ship spawns first, then the hangar's places).

**Hangar waypoints** (`SCHangarWaypoints.h`; chosen at the environment's import, `FSCImportOptions::bEnvironment`, placed by
`ASCVehicle::BuildWaypoints` in the presented pose). The hangars carry no player spawn, nav or landing-point entities the
export keeps (the `.soc` of `hangar_xltop_001` / `hangar_medfrnt_001` has no SpawnPoint / NavPoint / PlayerSpawn entity; its
`LandingArea` is a geometry-less entity the export drops), so the waypoints are the places a player walks to, chosen by
their DataCore components, never by names:

| Source (`source=`) | Record | Why it is a place |
| --- | --- | --- |
| `transit_gateway` | `interiors[].entities[]` kind `transit_gateway` (`TransitGatewayParams`) | where the personnel elevator lets the player into the hangar (the arrival); it stands for its container's call panel (a transit panel: an operated UI whose owner is not an attachable item); other panels there stay places |
| `panel` | a placement with an operating panel's UI (`FSCUiBinding::IsOperatingPanel`: ship-selector and freight-elevator kiosks, transit panels) | a screen the player walks up to and operates |
| `inventory` | an interior `inventory_containers[]` entry (`InventoryContainer`) | the item banks and the freight elevator's cargo grid (at its floor centre) |
| `ship_pad` | the ship elevator (`FSCEnvironment::FindShipBay`) | the pad the ship stands on, at its top centre |

Labels are the entity names (an inventory container's: its entity class; the pad's: its mesh) with underscores as spaces,
without the editor's instance counter (`-029`) or a mesh LOD suffix; repeated labels are numbered `#1`, `#2`... A waypoint
is listed only where a standing capsule fits on a walkable floor around it (the teleport's own spawn search,
`HANGAR_WAYPOINT_REJECT` otherwise; checked again whenever the menu opens and on `sc.FP.Spawns`, since the ship and cargo
move); the waypoints are rebuilt when the environment's state changes (`sc.Environment.State`); doors are not places of their own here (no hangar placement carries `door` data). A cooked
environment keeps the waypoints its cook saved (`Waypoints` is saved with the actor; `present_environment` builds them
before `cook_environment` bakes the rigs); cook format 8 marks levels cooked before as STALE (re-cook). An export without
`ui_bindings[].interaction_distance` (before cache format 40) warns at import and is refused by the cook scripts.

**Doors (C2 fixes, cache format 39).** A door's own data (`door` on placements and loadout children: `SCItemDoorParams`)
decides how it moves and makes it a door even without a Mannequin Open/Close pair: it spawns in its authored state
(`is_opened`; the C2's doors are all closed) at that state's end pose (the open fragment's end, the close fragment's end,
or a single-procedural door's transition start), an open/close door (`open_tag` / `close_tag`) plays those fragments, a single-procedural door (`transition_tag`, the C2's rear ramp) is its
one transition's time and closes by playing it backwards. Every press (E, a pad, a proximity sensor, a lift, `sc.FP.Door`,
the cargo access rule) goes through `ASCVehicle::PlayDoor`: each clip starts at the frame nearest the rig's current pose,
so a door turned around mid-stroke never snaps (the same clip continues from its exact playhead only while that playhead,
sampled in the new direction, is the pose the rig shows: a held clip a later one covers, such as an open/close door's Open
under its Close, is not resumed); the new direction's start sound (an event within a clip frame of its start of play)
plays even when it starts mid-stroke. A raw clip on a door's channels (`sc.Vehicle.Clip`, a snap) records the door's state
when it is the door's own fragment, else leaves it unknown (read closed), and `ResetPose` clears pending sensor exits and
re-opens the doors of a lift that was opening. And the
door's `item_port_links` are delivered (the C2's front hatch opens the front doors
and the front ramp). A panel in a door's loadout operates that door and its face follows it. A door panel is operated from as far as its own UI
allows: `ui_bindings[].interaction_distance` (cache format 40, the owner's `UIRenderToTextureEntityComponentParams.interactionDistance`,
metres; most door pads 2, the C2's ramp and front-hatch pads 15), never less than the 3 m reach: the C2's exterior pads sit
on the outside of the closed ramp and hatch, 3.3-4.1 m from a standing player's eye, out of the 3 m reach (the ray was cut
short: nothing happened). The view ray's first hit must be the pad (`ASCVehicle::PanelReach`); every E press logs
`FP_INTERACT door via=panel|rig|nearest|none hit dist reach toggled label`. Without door data a door is
the Mannequin Open/Close pair at its bind pose, as before. `sc.Doors.Pose [index]` logs a door's joint, a point on its
frame, its state and the clip playing (`DOOR_POSE`); every door start logs its clip and start frame (`DOOR_CLIP_START …
start=nearest_pose|playhead`); `sc.Doors.Trace <index|-1>` logs one door every tick while it moves (`DOOR_TICK`: state,
clip, playhead, joint) and the audio events its clips cross (`DOOR_AUDIO_EVENT`); `sc.Doors.Sensors` lists the proximity
sensors (`DOOR_SENSOR`: box in the vehicle frame, exit delay, doors). `viewer/scripts/rpc/c2-doors-reopen.ps1` cycles a C2
sensor door and a pad door. A ship's cargo hold is opened (its doors through `PlayDoor`) by each
transfer that needs it, not at load; the transfer waits for those doors' playback, and a hold whose door was closed since
reads closed again.

**Light panels (C2 fixes).** A panel whose render's clickable nodes (`ui_bindings[].buttons[]`) all send a tag a light
group it drives receives (the C2's `ControlPanel_Screen_LightControl`) is operated like the lift floor panel (the same
`FloorPanels` mechanism, `ESCPanelKind::Buttons`): the node under the crosshair is framed, E sends it; the group's lights
take the state its own interaction names (`LIGHT_GROUP_STATE`), and the face shows the exporter's render of the panel's
new state (`LIGHT_PANEL_PRESS`). `sc.Lights.Groups [group state]` lists the light groups or sets one. Unsafe teleport/noclip-exit locations are rejected (the refusal
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

### Door and lift panel screens (WP-I1)

A panel's display face is the game's `DisplayScreen` material with a virtual `$RenderToTexture` slot. The importer
draws it with `M_SC_Screen`: an opaque Nanite surface of dark glass (base colour from the authored `BackColour`,
F0 from `Specular` with the look's glass reflectance, gloss from `Shininess`), whose emission is the panel's image ×
the authored `Glow` (the game's glow rule). A face whose data asks for partial coverage (authored opacity below 1 or a
blended alpha mode) takes `M_SC_Screen_Translucent` (coverage = opacity, in the non-Nanite split). The image is the
exporter's render of the panel's default UI state (`ui_bindings[].generated_image_path`, in the package). It is sampled
at UV0 through `ScreenUVTransformX` / `ScreenUVTransformY` (an affine map, identity by default); a viewer-drawn image
(the lift floor panel, `SetScreenFaceMaterial`) maps the face's own UV window onto itself.

Which face shows which image is decided per binding and per face, from data only:

- **Role.** A binding is an operating panel's when it is `physical`, its owner was resolved, and the owner is not a
  display item or is one the player operates. The exporter writes (cache format 30) `source_item_type` (the UI owner's
  `AttachDef.Type`), `source_owner_resolved` and `source_kiosk_interaction` (the owner has
  `KioskInteractionEntityComponentParams`). Door and light panels (`ControlPanel`), usables, transit panels (no item
  type) and kiosks (an operated `Display`) are panels; vehicle MFDs and physical screens, HUD projectors, radar displays
  and signage screens (a `Display` only shown) are not, whoever carries the binding. Those are left unbound by the owner's
  decision (the viewer's own UI later) and show as clean dark glass, as do screens off by default, unresolved owners and
  bindings of an older export (the import then warns to re-export).
- **Face.** A binding whose `helper_name` names a node of the mesh owns that node's subtree; the entity's other bindings
  own the faces no helper claims, and a binding propagated from another entity whose helper is not on the mesh claims none
  (`FSCSceneInstance::ScreenSlotImage`). Per material slot, using the nodes that draw its
  triangles in the drawn variant or LOD: one image for all of them binds it; a face nobody owns, or faces of different
  outcomes sharing the slot, leave it dark. An unbound screen emits nothing (its strength is 0, so the emitter tools do not
  light it either).
- One material instance per (material, submaterial, image): every C2 door pad shares one.

The import logs `SCREEN_BIND instances=<n> images=<m> panel_images=<k> skipped_mfd=<s> slots_bound=<b> slots_dark=<d>
ambiguous=<a> unclassified=<u>` (only slots with triangles count). The HUD projector glass (`UIPlane`) still draws nothing.
`sc.Screens` lists every screen face with the image it shows (a face whose CPU mesh copy a cook dropped is listed as
`geometry=unavailable`); `sc.Screens <index> [cm]` puts the (fly) camera in front of one and logs the first surface on the
ray to it, so a screen covered by other geometry shows up. `sc.FP.Pads` checks that each door pad is hittable (the first
visibility hit is the pad itself) and resolves to its door; `sc.FP.Pads negative` repeats it with the pads' collision off
and must find none (`FP_PADS_NEGATIVE ok=1`). Prepared levels are stamped with `FUCPrepared::FormatVersion` (WP-O1-4); an
older one is refused by `package_showcase.ps1` and the game and prepared again by the prep step (`-run=SCPrepare`, which
exports through the bridge, so its `ui_bindings` carry the current fields); pooled meshes are keyed by their Nanite build
and section topology.

**Panel states and aspect (cache format 32, I1-5 / I1-3 / I1-2).** The exporter renders an interior panel at its face's
texture-space aspect (`ui_texture_aspect_w_over_h`, measured on its `$RenderToTexture` faces), with the panel owner's user
variables (a C2 door pad hides its lock, sensor and light controls) and state, and writes the owner's other states the
canvas shows as `ui_bindings[].state_images[]` (door pads: `DoorOpenState` opened / opening / closing, `DoorLockState`
locked, `PowerState` low / lost, `OverrideableState` can-override). The importer decodes them with the default image and
makes one instance each per (sidecar, submaterial, image); a face holding state images is an `ASCVehicle::ScreenFaces`
entry. `RefreshPanelScreens` (from the vehicle tick and when a fragment starts) puts on each door pad's face the state of
its door (`DoorControls`): opening or closing while the door moves, opened or closed at rest. The viewer does not model
locks or power, so those states are shown on request: `sc.Screens.States` lists the faces (`SCREEN_STATE`: face, component,
slot, its states, the state shown, the door and its `sc.FP.Door` index); `sc.Screens.States <face|all> <Type.State>` holds
a state, `follow` releases it. `SetScreenFaceMaterial` puts any other material on a face (the hook for screen content the
viewer draws itself). The import logs `SCREEN_STATES faces=<n> state_images=<m> initial_swaps=<k>`; a swap logs
`PANEL_SCREEN face=<i> state=<Type.State|default>`.

### Interaction feedback: hover and the popout (WP-I1-4)

**Hover.** An operating panel's face shows the game's hover look on the button under the crosshair: the exporter renders
each written image (the default and every `state_images[]` entry) once more per button node with the engine's hover tag
(`UI.Style.__Internal.hover`) on that node, so the widget standards' hover entries apply (a large button's background
`Base` 0.3 → 0.6, its foreground 0.4 → 1.0; a light panel row lighter, framed), and writes them as `hover_images[]`
(cache format 43). Door pads, call panels and kiosks swap their face to the render of the state they show with that
button hovered (`ASCVehicle::AimPanelButton` / `SetPanelHover`, `PANEL_HOVER face node`); on a door pad off every button
the door button is hovered (what E presses). Drawn light panels draw the hover render instead of our frame when the
export has one (`FLOOR_PANEL_VIEW … image=…__hover_…`). An export before format 43 keeps the frame and no pad hover.

**Popout.** Pinned to the thing in the world, in the callouts' paper look, a placard lists what E does on the thing
under the crosshair. Its leader starts at a point on the thing, projected every frame, so the popout stays there while
the view moves: the aimed button's centre on a panel's face, a door's opening, a usable's interaction point, else where
the view first met the thing (held while the same thing stays targeted); up-right of it, mirrored left at the view's
edge, gone when the point leaves the view. Seated or on a ladder (nothing to point at) it sits by the crosshair. It is built every tick from what `Interact()` would act on, in its order (`ASCFirstPersonCharacter::BuildPrompt`):
a seat in use, the ladder climbed, the garage kiosk, a drawn panel's row, a lift panel, a seat access, a usable, a
door (title: its display name; the action its verb, unavailable with the refusal when the ship-link rules refuse it).
The E key cap marks the action E performs. With several, the **mouse wheel** moves it (`FP_PROMPT_SELECT`); the choice
is kept while the same thing stays under the crosshair. Usables (`usable_interactions[]`: beds, sofas, chairs) list the
interactions that start a use (a C2 bed: Sit, Lie Down, Sleep), dimmed "not in the viewer yet": uses are WP-P6-4
(`FP_USABLE … performed=0`). `sc.FP.Prompt` logs the current prompt (`FP_PROMPT_NOW key title options`, `>` the
selected, `-` unavailable); `sc.FP.Key MouseScrollDown 1` is the wheel. The prompt left the HUD's status text.

### Lifts (WP-P5)

A package's `interiors[].lifts[]` record (an `SCItemElevatorParams` carriage: speed, easing, dwell and queue waits, stops in
chain order with their landing doors, riders) becomes an `FSCLift` owned by the vehicle (`ASCVehicle::Lifts`, ticked
before the riders move). At import the carriage and every entity riding it (`parent_attachment`: the C2's gate and floor
panel) are re-parented under a moving anchor `Lift_<entity_id>` (tag `SCLift`, left out of instancing and emissive
instancing), keeping their world transforms. The carriage stands at its rest node with its doors closed.

- **Motion.** One leg is a speed profile from `MaxSpeed` and `EasingDistance`. The reading (WP-P5 Q1, not yet compared with
  the game, logged once as `LIFT_PROFILE`): constant acceleration a = MaxSpeed² / (2 · EasingDistance) over the ramp,
  cruise, the mirror ramp down; a leg shorter than two ramps is a triangle. C2: hold ↔ top 4.956 s, ground ↔ hold 4.750 s,
  ground ↔ top 6.957 s.
- **Ground stop.** A leg downward ends on the first blocking surface under the carriage when that lies above the stop: the
  box of the carriage's colliders is swept from where it stands toward the stop against world collision (what blocks the
  player; the vehicle itself, pawns and anything the box already overlaps are not ground). The leg's profile is made for the
  shortened distance, so the carriage ramps down onto the surface, and the arrival is the stop's (doors, `Arrived at <N>`,
  logs); the next leg starts from there. `LIFT_GROUND_CONTACT lift stop authored_z contact_z authored_dist_m dist_m
  authored_s predicted_s by` (z: container frame, cm). Nothing hit: the authored stop. A reading, not game data
  (`SCItemElevatorParams.Collision` is null on every lift class; WP-P5 "Ground stop"): the C2's ground stop (z −14.65 m)
  lies 0.6 m below the hangar pad in the viewer's landed pose.
- **Logic** (`PoweredDown`, `Idle`, `Closing`, `WaitingForGateway`, `Moving`, `Opening`, `Open`): a request where the
  carriage stands only opens its doors. With the doors open, the next leg waits for `OpenWaitTime` since they opened and
  `QueueWaitTime` since the first request; the gate and the stop's landing door close (`CloseAllDoors`; the carriage never
  leaves with one open, and a closing door is not reversed: a call at the stop is served once they have closed). A door
  of the vehicle whose origin lies in the leg's path (the box of the carriage's colliders from start to end, not counting
  the lift's own doors: the C2 belly hatch) is waited for while its clip plays and blocks the leg when closed
  (`LIFT_BLOCKED`), unless the links of the requests for that stop open it again (`LIFT_GATEWAY`, `LINK_REDELIVER`: a
  ground call made while the carriage rose to the top, whose `Arrived at 2` closed the hatch). The carriage moves
  (nearest requested stop in its direction, else the nearest), emits `Arrived at <N>` along its links (the C2 closes the
  hatch) and opens the gate and that stop's landing door. A stop's `DoorOpen` / `DoorClose` with the carriage elsewhere is
  ignored (`LIFT_IGNORED`). A door without the clip asked for is not waited for (`LIFT_DOOR … played=0`); one that stays
  open blocks the leg (`reason=door_did_not_close`). Traversal-tree links (`LIFT_TRAVERSAL`) are reported, not delivered:
  the item port they reach is not exported.
- **Doors** play their own Open/Close fragment at the door's `DoorAnimationSpeed` (a door whose data has no usable speed
  plays at the clip's own rate, with a `LIFT_BUILD` warning); a door rig plays the clips authored for its own geometry
  when it has any (the C2's hold and top lift doors share bone names). The doors a lift drives (gate, landing doors) are
  not toggled by E nor by proximity sensors; `sc.FP.Door` still does. Riders hang on their own parent (the carriage or
  another rider, at its socket when the parent is a rig), and lights whose entity rides the carriage
  (`parent_entity_id`) move with it.
- **Riding** is UE based movement on the carriage's own colliders; the character ticks after the vehicle and its
  movement after the character. While the movement base belongs to a carriage the character logs `LIFT_RIDE lift= base=
  asset= rel_z_cm= floor_dist_cm=` once a second (and on every change). A capsule standing on a moving carriage (its
  movement base is part of the carriage, or a carriage collider lies under its feet within the step height: the stand-in for
  the carriage's own physics grid) is its rider: what the leg passes through
  (`LIFT_PATH`: the colliders in the carriage's swept box, not its own parts, doors or pawns) does not block it, so it is
  carried the whole leg (C2 fixes: the hold's shoulder slab reaches into the shaft and caught riders at the lower deck).
  Per tick while riding `LIFT_RIDE_TICK` (feet over the carriage anchor, floor distance, base) and at the leg's end
  `LIFT_RIDE_LEG samples rel_z_min_cm rel_z_max_cm spread_cm off_carriage`. `sc.Lift.CarryRiders 0` turns the rule off
  (A/B).
- **Press path** (until WP-I1 has its own): a placed entity whose `interaction_links` reach the carriage or one of its
  stops is a lift panel. E on it (the crosshair on its collision, else on its parts' bounds) sends an interaction of its
  interaction point through `ASCVehicle::SendInteraction`, which follows the exported links (`LINK_DELIVER`,
  `LINK_SEND`): a call button its primary interaction (HUD "E: Call Elevator").
- **Floor panel** (WP-P5 "Result (floor panel)"; our own UI, the game draws none on the C2's): a lift panel whose
  interactions ask one carriage for its stops (`From Elevator call <N>`) and that has a blank screen face shows the stops
  there, one row each from the highest down, drawn into a render target on the face (`SCFloorPanel.h`, through
  `SetScreenFaceMaterial` and `M_SC_Screen`'s brightness model). Labels are the stops' names as glyph masks in the door
  pads' label font, colours the pads' palette roles (`lifts[].panel_style`, cache format 35). The row under the
  **crosshair** is highlighted (nothing off the face or between rows, `FP_PANEL_AIM`); **E** sends that row's own
  interaction (`FLOOR_PANEL_PRESS`, the aim taken at the key press), off the rows E does nothing, nor on a floor-select
  panel whose face could not be drawn (`FLOOR_PANEL_BUILD … not drawn, E sends nothing`). A row is labelled with its
  own interaction's display name. A cyan bar marks the current floor, a yellow one a
  called floor (blinking for the leg in progress), a chevron in the bottom bar the direction of travel
  (`FLOOR_PANEL_VIEW`). The interaction popout names the highlighted row (WP-I1-4, above). `sc.FP.Probe` names the focused `panel=` and `choice=` (`None` off the rows).

| Command | Effect |
| --- | --- |
| `sc.Lift.List` | `LIFT_LIST id entity class stops at state gate gateways riders`, one `LIFT_STOP lift index node z_cm s_m name doors` per stop, one `LIFT_PANEL` per panel |
| `sc.Lift.Call <lift> <stop>` | as a call button at that stop (`AscentRequest` / `DescentRequest` on its node) |
| `sc.Lift.Request <lift> <stop>` | as the floor panel (`From Elevator call <stop>`) |
| `sc.Lift.Doors <lift> open\|close` | `OpenInnerDoor` + the stop's `DoorOpen` / `CloseAllDoors` + `DoorClose` |
| `sc.Lift.Send <entity_id> <interaction>` | fire an entity's interaction along its exported links (the press path without a crosshair; the C2's helper-only ground buttons) |
| `sc.Lift.Where` | `LIFT_WHERE lift s_m z_cm v state phase at target queue`, and the player's movement base |
| `sc.Lift.Panels` | `FLOOR_PANEL index kind entity lift face size_cm colours` with the view shown (kind `floor_select`, or `buttons` for a light panel, its rows in the render's image coordinates), `FLOOR_PANEL_AT index centre normal` (vehicle frame), one `FLOOR_PANEL_ROW panel option stop label interaction rect` per row |
| `sc.Lift.PanelAim <panel> <row>` / `<panel> <x> <y>` | turn the first-person view to a row's centre, or to a content point of the face (0..1, y down; outside is off the face) |

Logs: `LIFT_BUILD`, `LIFT_REQUESTED`, `LIFT_STATE`, `LIFT_DEPARTED` (predicted leg time), `LIFT_PHASE`
(`Start`/`RampUp`/`FullSpeed`/`RampDown`/`Stop`/`PowerUp`/`PowerDown`), `LIFT_PASSED`, `LIFT_ARRIVED lift stop t_leg
predicted_s dist_m z_cm contact`, `LIFT_GROUND_CONTACT`, `LIFT_DOOR`, `LIFT_BLOCKED`, `LIFT_GATEWAY`, `LIFT_IGNORED`, `LIFT_TRAVERSAL`. RPC: `/wait until=lift_idle [lift=<id>]` (no request
queued, not moving, no door of the lift in motion, its gateway doors included) and `lifts[]` in `/state` and `SCState.ndjson`. The scripted C2 run is
`viewer/scripts/rpc/c2-lift.ps1` (WP-P5 "Result (P5-1)"); the floor panel's is `viewer/scripts/rpc/c2-floor-panel.ps1`.

### Scripted first person (GPU evidence, reused by WP-P2)

Console commands (game hosts; `sc.FP.*` other than `Spawn` need first-person mode):

| Command | Effect |
| --- | --- |
| `sc.FP.Spawns` | log spawn list (`FP_SPAWN_LIST index kind pos label`; then the hangar's places, `kind=Hangar hangar=<n> source= pos=` (hangar-relative) `world=`) and interactions (`FP_DOOR_LIST`) |
| `sc.FP.Spawn <index\|kind[:n]\|hangar:n>` | enter first person if needed, teleport to spawn `index` (an index past the ship's spawns is a hangar place), the n-th spawn of `kind`, or the n-th hangar place |
| `sc.FP.Look <yaw> <pitch>` | absolute view, degrees, relative to the vehicle |
| `sc.FP.Move <fwd> <right> <seconds> [run 0\|1] [up]` | hold movement input (-1..1) for a duration |
| `sc.FP.Interact` / `sc.FP.Door <index>` | E on the focused interaction / toggle one by index |
| `sc.Paint [index\|default\|name part]` | list the loaded vehicle's paints (`PAINT_CHOICE`), or put one on (also the menu's Paint list; remembered per vehicle) |
| `sc.FP.Crouch 0\|1`, `sc.FP.Jump`, `sc.FP.Torch 0\|1`, `sc.FP.Noclip 0\|1` | as the keys (a refused noclip exit logs `FIRST_PERSON_NOCLIP refused=1`) |
| `sc.FP.TorchLight 1\|0` | the torch is the kit's game flashlight (1, default once the body attaches) or the viewer's fallback beam (0), for A/B; logs `FP_CMD torch_light source=...` |
| `sc.FP.Menu 0\|1` | teleport menu |
| `sc.FP.Tap <key>` | press and release a key of the key table (`Up`, `Down`, `Enter`, `T`...); logs `FP_KEY handled menu selection row` |
| `sc.FP.AimAt <x y z>` | keep the view on a vehicle-relative point (cm) until mouse look |
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

`viewer/scripts/capture_route.sh <tag> <route> [host args]` launches the host like
`capture_viewer.sh`, screenshots the window at every `SCRIPT_SHOT` into
`out/smoke/<tag>-<shot>.png`, stops UE after `SCRIPT_DONE` (or reports the exit status
of a route ending in `quit`) and prints the route log. Routes live in
`viewer/scripts/routes/`. Walking routes use spawn kinds and relative moves only; a route that
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

## Audio (WP-A1)

The viewer plays Star Citizen's own sounds from the package's `audio.json` (D18; contract:
`vendor/StarBreaker/docs/decomposed-export-contract.md` §Audio) through UE audio — no Wwise, no invented sounds, no
trigger name in the code. `FSCAudioScene` holds one imported vehicle's or environment's audio (manifest, Ogg media,
convolution impulse responses, placed spots and areas, entities with sound effects); `USCAudioSubsystem` (a tickable
world subsystem) plays it:

- **Placed audio.** Spots (`interiors[].audio_spots[]`) are voiced while the listener is within their attenuation's
  reach (× `attenuationScalingFactor`); multi-position spots sound at the position nearest the listener; `playRandom`
  spots re-trigger after `minDelay`..`maxDelay` (from the end with `delayIsFromEnd`) within `radiusRandom`; other
  one-shot spots play once each time the listener comes within reach, looping ones while in reach. Areas
  (`audio_areas[]`) play their bed at the listener inside their shape, and outside at the nearest point of the shape
  with the area-fade RTPC falling from 1 to 0 over `fadeDistance`; the spots an area links take that fade and its
  `atsVolumeOffset` while the listener is inside; a spot no area governs is faded by none.
- **Trees.** Random (weights, avoid-repeat, shuffle; history per emitter unless `global`) and sequence containers, step
  or continuous with crossfade / delay / trigger-rate transitions and their random ranges (finite runs scheduled whole,
  infinite ones item by item), blends at their layers' RTPC gains, the default switch branch, initial delays, loops
  (`USCAudioWave` decodes the Ogg on the audio render thread as the mixer pulls it, through `sc_audio_*`: a game-thread
  hitch cannot starve it; loops follow the media's loop region as Wwise does: the intro once, `LoopStart..LoopEnd`
  repeated sample for sample, the tail after the last pass; a media without loop points loops whole). A finite voice
  ends when its audio has played out (the wave reports it), not at a game-clock time. A post returns one handle for all
  its play actions; stop actions honour their object scope, delay and fade. `sc.Audio.Health [reset]` logs
  `AUDIO_HEALTH` (render-side gaps, source changes, restarts, cut voices, start latency, decode load; per voice
  `AUDIO_VOICE_GAP` / `AUDIO_VOICE_RESOURCED` / `AUDIO_VOICE_RESTART` / `AUDIO_VOICE_CUT`); `sc.Audio.Hitch <ms>
  [count] [every_s]` stalls the game thread for measurements; `sc.Audio.Legacy 7` restores the pre-fix whole-file loops,
  clock ends and unfaded stops for A/B runs only (with `au.SetAudioChannelCount 32`, the pre-fix voice limit). The voice
  limit is the audio quality level's `MaxChannels` (`DefaultEngine.ini` `[/Script/Engine.AudioSettings]`, 256, the
  platform's source pool): at UE's default of 32 the ~100 audible voices near the C2's top deck took each other's
  sources, cutting steady beds in and out ~20 times a minute. Measurement switches: `sc.Audio.LimiterTrace start | stop
  [csv]` (the master bus output after its limiter and the main output per 10 ms: peaks over the threshold, samples over
  0 dBFS), `sc.Audio.Limiter 0|1` (limiters bypassed / on), `sc.Audio.MuteScene <name part> 0|1` (silence one scene's
  voices, e.g. the hangar's), `sc.Audio.MixTrace start | stop [csv]` (every bus effect chain per render buffer:
  input / output peaks, the limiter's gain, the meters' values, the side-chain gains, the EQ bands in force;
  `AUDIO_MIX_TRACE` per bus), `sc.Audio.Rtpc` (the meter-written game parameters now, `AUDIO_RTPC`).
- **Gains.** Every node's own volume / make-up / output-bus gain and random draws, the action context, RTPC curves at the
  RTPCs' defaults, the listener-relative `Azimuth` and `Listener_Cone` and the area fade per frame, the bus chain's
  voice volumes and pitch. Dry and wet are two sends of each voice (UE's base submix output is off): the bus gets the
  attenuation's dry curve at the emitter distance, the room's environment the aux gain plus the aux curve. Each Wwise
  bus is a submix (`SCBus_<id|name>`) under its parent at its bus volume + make-up + RTPC defaults + Gain effects
  (auto-disable off: UE never wakes a bus created under an idle parent).
- **Bus effects (A1-2).** Each bus submix runs its own Wwise effect slots in slot order (`SCAudioBusFx.h`, the DSP in
  scbridge `sc_limiter` / `sc_meter` / `sc_eq`, unit-tested in Rust; `AUDIO_BUSFX` lists each bus's chain and what it
  does not run): **Gain** at its slot; **Parametric EQ** bands as RBJ biquads with their parameters at the RTPC values
  (frequency curves log-scaled, gain curves additive); **Meters** (peak / RMS in 10 ms blocks, attack, hold, release,
  range; a meter that applies the downstream volume measures after the bus's and its parents' static levels) that write
  their game parameter (the last write stands); the **Peak Limiter** with the look-ahead as its attack (the gain is held
  at the lowest target over the look-ahead, released with the release time, averaged over the look-ahead, and has
  reached each peak's gain when the peak leaves the delay line), so its output stays within
  `threshold * (peak / threshold)^(1 / ratio)`. A meter-written parameter moves every curve on it: the target buses'
  output-stage gain curves (bus / output-bus volume, make-up) after the chain, relative to their values at the
  parameter's default (the static level carries those) — the game's side-chain ducking — and voices' curves, including
  the buses' voice-stage `volume` curves, which apply per voice before the dry / wet split. Each effect instance has its
  own kernels, built at its Init. A slot that a state group switches is left off (no game state is modelled); bus LPF /
  HPF amounts have no cut-off mapping and are not applied. The LFE of a 5.1 / 7.1 output is left out where a slot does
  not process LFE.
- **Sample-aligned voices (A1-2).** Every voice is scheduled on the mixer's audio clock (its post time plus two render
  buffers): its first pull waits in silence for that time, so voices posted together start sample for sample together
  instead of one render buffer (21.3 ms at 1,024 frames) apart, which combs identical copies. A placed loop keeps its
  scene's timeline: the game posts a zone's spots together and its virtual voices resume from elapsed time
  (`virtual_queue_behavior` 1 in the banks), so a loop that comes into reach seeks (`sc_audio_seek`) to where it would
  be. `sc.Audio.Legacy 8` starts voices at their first pull (A/B); `sc.Audio.Health` lists the triggers audible from
  several posts (`AUDIO_HEALTH_TRIGGER`) and the media audible in several voices with their play positions at one
  audio clock (`AUDIO_HEALTH_MEDIA`). Multi-position spots play one voice at the nearest position (`multiPositionType`
  0 is unset: the engine's `s_DefaultMultiPositionType` 2, MultiDirections, never copies the signal per position).
- **Reverb.** Each room's environment (`rooms[]`) is the aux bus of an `AudioEnvironment` record; its submix runs UE's
  convolution reverb over the game's own impulse response (the bank's Convolution Reverb, wet / dry as authored, the IR
  at its stored level after the authored pre-delay; UE's own reverb send is off). A voice whose node uses game-defined aux sends sends to the environment of its room (a spot's
  `staticApcRoom`, else the room volume it stands in) at its summed aux-send gain plus the attenuation's aux curve.
- **Events.** Doors and every animated part: the clip sources' `audio_events[]` crossed while a clip plays (forward,
  reverse, both; lift-driven doors included). Lifts: each `FSCLift` phase fires the carriage class's trigger effects of
  the same tag name, the `InTransit` tag effect plays from `Start` to `Stop`. Panels: a top-level
  `ASCVehicle::SendInteraction` (a press) fires the source entity's `Activate` effects. Flashlight: the kit item's
  `Light_On_SFX` / `Light_Off_SFX`. Weapon: its `fireAudioTrigger` per shot.
- **Cooked levels.** UnrealCitizen's prep step (`-run=SCPrepare`, `SCPrepareAudio.h`) copies the package's
  audio beside the prepared level (`Content/SCAudio/<level>`, gitignored, staged as non-UFS files; a missing `audio.json`
  or payload stops that level's cook, cook format 7: levels staged before A1-2's bus effects are stale); `ASCVehicle::BeginPlay` loads it (`CookedAudio`). Cooked levels
  bake their rigs, so they carry the placed audio and the reverb; door, lift and panel sounds need a live import.

Console: `sc.Audio.List` (voices: trigger, emitter, distance, gain, reverb send, environment, bus), `sc.Audio.Rooms`
(every room's environment and reverb parameters), `sc.Audio.Play <trigger>` (in front of the listener),
`sc.Audio.Stop`, `sc.Audio.Mute 0|1`, `sc.Audio.Ambience 0|1` (placed spots and areas off for isolated measurements),
`sc.Audio.Log 0|1`, `sc.Audio.Unfocused <0..1>` (UE mutes an unfocused window; off-screen runs need 1),
`sc.Audio.Record start [s]` / `stop <abs path.wav>` (the main submix to a WAV), `sc.Audio.Sends 0|1` (reverb sends off
for dry measurements), `sc.Audio.Meter start` / `stop` (every bus submix's peak and RMS over the window).

Logs: `AUDIO_SCENE` (triggers, media, buses, environments, rooms per scene), `AUDIO_EMITTERS` (spots, extra positions,
areas per interior), `AUDIO_ENVIRONMENT`, `AUDIO_BUS` (each submix's level and its parts), `AUDIO_REVERB` (each
convolution: IR, wet, dry), `AUDIO_PLAY` (trigger, media, emitter, position, distance, gain, bus, environment, send),
`AUDIO_SKIP` (an unresolvable trigger and why), `AUDIO_AREA` (area fades), `AUDIO_ROOM` (the listener's room, its
environment and reverb parameters), `AUDIO_LIMITER`, `AUDIO_BUSFX`, `AUDIO_MIX_TRACE`, `AUDIO_RTPC`, `AUDIO_METER`, `AUDIO_SENDS`, `AUDIO_RECORD`, `AUDIO_COOKED`. `WindowsEngine.ini` raises UE's voice limit to 256
(`AudioMaxChannels`). The scripted C2 run is `viewer/scripts/rpc/c2-audio.ps1` (WP-A1 "Result (A1-1)").

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
| `-SCPlayerDemo[=<entity>]` | runs `viewer/scripts/routes/player-demo.txt` (loads `<entity>` if given, enters first person) |

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
  sharp (UE 5.9's diaphragm DOF has no focal region). `sc.Player.DofSensorWidth` (36 mm). Third person has
  no depth of field (removed 2026-11-19); no player override outside aiming (the level's own look applies).

Console (first person): `sc.Player.Weapon <slot|none>`, `sc.Player.View fp|tp [yaw] [distance]`,
`sc.Player.State` (`PLAYER_STATE` log), `sc.Player.Load` (start/retry the kit), `sc.Player.Body 0|1`
(detach/re-attach — frame-time A/B), `sc.FP.Key <key> 0|1` (press/release a key-table key).
Route pseudo-commands: `waitkit [s]` holds the route until the body is attached; `waitpt [s]` holds it until the
path tracer has accumulated `r.PathTracing.SamplesPerPixel` (put it after `viewmode pathtracing`); `snap <tag>` is
`shot` without clearing held input (moving shots). `slomo <x>` (engine) slows the animation while
route delays stay real time (mid-select frames).

```bash
viewer/scripts/capture_route.sh p2-cutlass viewer/scripts/routes/cutlass-armed-walkthrough.txt -SCDefaults \
  -SCLoad=DRAK_Cutlass_Black -SCCamera=first \
  -SCPlayerLoadout=Data/Scripts/Loadouts/Player/DefaultLoadouts/PVE_medium_behr_rifle_gmni_rifle.xml
```

Null-RHI tests: `-run=pythonscript -script=viewer/scripts/test_player.py -SCPlayerTests
[-SCPlayerPackage=<kit tar>]` (see [WP-P2 verification](verification/WP-P2.md)).

### Offline render RPC (WP-H2)

With the viewer running under the RPC harness, run `viewer/scripts/rpc/render_check.ps1` for a short camera-path test.
Start with `-SCDefaults` and no ship to use the procedural hangar for this quick check.
`-Acceptance` runs the five-second Cutlass orbit with the default 256 warm-up frames. Output goes to
`out/h2/<tag>`.
`report.json` records timing and camera-step checks, any exact RGB frame repeats, and adjacent-image differences
for diagnosis.
`Invoke-SCRender` accepts the request in [the H2 brief](../docs/work-packages/WP-H2-offline-render.md).
For the live repeat check, start the viewer with `-SCDefaults -SCShowcase -SCLoad=DRAK_Cutlass_Black`.
Capture twice in that session with Windows display mode held constant. The scoring command can run after
the viewer exits.

```powershell
./viewer/scripts/rpc/render_check.ps1 -Acceptance -Tag reference
./viewer/scripts/rpc/render_check.ps1 -Acceptance -Tag repeat
./viewer/scripts/rpc/render_repeat.ps1 -Reference out/h2/reference -Candidate out/h2/repeat `
    -Tolerance viewer/scripts/rpc/h2-live-tolerance.json
```

Run `SCBridge.RenderGpu.FrozenOutputDevice` in a separate D3D12 game session to verify exact RGB equality
for the frozen chart across SDR and HDR output-device settings.
Use `/record` for real-time 8-bit previews.
