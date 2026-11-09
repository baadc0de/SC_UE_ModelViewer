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
**GPU acceptance remains open:** Vulkan found zero devices in this session;
successful runtime tests used `-nullrhi`, so rendered Nanite, Lumen and HWRT are
not verified. WP-U0 is not fully complete.

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
