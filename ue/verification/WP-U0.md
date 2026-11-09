# WP-U0 verification record — 2026-11-09

All commands ran from the repository root. Paths below use `$HOME`/`$PWD` in
place of their expanded absolute prefixes. Engine version is
`5.8.2-56702186+++UE5+Release-5.8`. No cargo, push, or PR commands ran.
Full logs are in ignored `ue/SCViewer/Saved/Spikes/`. This file retains key output
so the evidence survives deletion of build scratch.

Timing wrapper used for command runs:

```bash
TIMEFORMAT='wall_seconds=%R'
{ time timeout -k 5 90 <command>; } > <console-log> 2>&1
```

Build timeouts were 180 or 240 seconds. Initial probes used `timeout 180` or
`timeout 90` without `-k`. The first attempt to use `/usr/bin/time -p` failed with
`/usr/bin/time: No such file or directory`; all reported wall times use Bash `time`.

## Repository and environment

```bash
pwd
git status --short
git log -1 --oneline
git switch -c wp/u0-unreal-spike
df -h . /tmp
cat "$HOME/UnrealEngine/5.8/Engine/Build/Build.version"
ls -l /dev/nvidia* /dev/dri/*
```

Key output:

```text
a2e64db Cache lives in ./cache relative to the working directory, never a global dir
fatal: cannot lock ref 'refs/heads/wp/u0-unreal-spike': unable to create directory for .git/refs/heads/wp/u0-unreal-spike
/dev/nvme0n1p2  1.6T  1.5T  82G  95%  <repository>
tmpfs           24G  1.9G  22G   9%  /tmp
MajorVersion: 5, MinorVersion: 8, PatchVersion: 2, Changelist: 56702186
BranchName: ++UE5+Release-5.8
ls: cannot access '/dev/nvidia*': No such file or directory
ls: cannot access '/dev/dri/*': No such file or directory
```

The session makes `.git` read-only. The initial branch attempt failed. At final
verification `git branch --show-current` returned `wp/u0-unreal-spike`, still at
a2e64db, and the reflog showed a checkout from main. That change happened outside
this agent's successful tool actions. Staging remained blocked:

```bash
git add -- .claude/launch.json ue docs/02-implementation-plan.md docs/03-unreal-export-design.md
```

```text
fatal: Unable to create '<repository>/.git/index.lock': Read-only file system
```

No commit could be created. All work is uncommitted on `wp/u0-unreal-spike`;
no prohibited build products are tracked.

## A: Development Game target

Initial command:

```bash
"$HOME/UnrealEngine/5.8/Engine/Build/BatchFiles/Linux/Build.sh" \
  SCViewer Linux Development -Project="$PWD/ue/SCViewer/SCViewer.uproject" \
  -WaitMutex -NoHotReload -Log="$PWD/ue/SCViewer/Saved/Spikes/build-game.log"
```

Failed in **0.205 s** before evaluating the target:

```text
Unhandled exception. System.IO.IOException: Read-only file system :
'<home>/.config/Epic/UnrealBuildTool/Trace-backup-2026.10.11-15.27.21.uba'
```

Adding `XDG_CONFIG_HOME="$PWD/ue/SCViewer/Saved/BuildConfig"` fixed the trace
location, but UBA looped on `Failed to create directory
<home>/.epic/UnrealBuildAccelerator/sessions/... (Success)`. Adding
`-NoUBA -NoUBALocal` alone did not fix it. Both runs were interrupted; repeated
logs were trimmed. Adding `-UBARootDir="$PWD/ue/SCViewer/Saved/UBA"` fixed the loop.

The next **0.953 s** run rejected `BuildSettingsVersion.V6` for changing warning
levels in products shared with `UnrealGame`; the project was updated to **V7**.
The decisive invocation was:

```bash
XDG_CONFIG_HOME="$PWD/ue/SCViewer/Saved/BuildConfig" \
  "$HOME/UnrealEngine/5.8/Engine/Build/BatchFiles/Linux/Build.sh" \
  SCViewer Linux Development -Project="$PWD/ue/SCViewer/SCViewer.uproject" \
  -WaitMutex -NoHotReload -NoUBA -NoUBALocal \
  -UBARootDir="$PWD/ue/SCViewer/Saved/UBA" \
  -Log="$PWD/ue/SCViewer/Saved/Spikes/build-game-v7.log"
```

```text
Unable to instantiate module 'UnrealEd': Unable to instantiate UnrealEd module for non-editor targets.
(referenced via SCViewer -> SCViewer.Build.cs -> SCBridge.Build.cs -> MeshBuilder.Build.cs -> MeshUtilities.Build.cs -> MaterialUtilities.Build.cs)
Result: Failed (RulesError)
Total execution time: 1.99 seconds
wall_seconds=2.072
```

A cannot run B/C/D probes because this dependency experiment produces no executable.
This does not establish that all possible custom runtime builders are impossible.

## B: Editor target

The skeleton used the same bounded command with target `SCViewerEditor` and log
`build-editor-skeleton.log`: **19.693 s wall**, `Result: Succeeded` (UBT 19.60 s).
The checked-in helper preserves those scratch settings:

```bash
ue/scripts/build.sh SCViewerEditor
```

`build-editor-final-console.log`:

```text
[1/3] Compile SCBridge.cpp [NoUba]
[2/3] Compile SCSpike.cpp [NoUba]
[3/3] Link libUnrealEditor-SCBridge.so [NoUba]
Some action result store tasks did not succeed
Result: Succeeded
Total execution time: 7.56 seconds
wall_seconds=7.648
```

The action-result-store warning is nonfatal; linking succeeded. Earlier incremental
builds while implementing the probes passed in 4.762, 3.824, 3.994, 4.194 and
7.139 s wall. Those are iteration timings, not independent performance samples.

## Python commandlet and map

Initial command (no DDC override):

```bash
"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/headless_smoke.py" \
  -unattended -nop4 -nosplash -nullrhi \
  -abslog="$PWD/ue/SCViewer/Saved/Spikes/headless.log"
```

It stalled and was interrupted. Exact errors included:

```text
LogZenServiceInstance: Warning: Unreal Zen Storage Server is unable to determine a valid data path
LogZenServiceInstance: Warning: Unreal Zen Storage Server was unable to uninstall a running system service instance. Zen AutoLaunch settings may fail to function correctly.
LogUnixPlatformFile: Warning: open('<home>/.config/Epic/UnrealEngine/Common/Zen/Install/zen_plugin_versions.json', Flags=0x00080041) failed: errno=30 (Read-only file system)
```

Workaround command:

```bash
"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/headless_smoke.py" \
  -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC \
  -UserDir="$PWD/ue/SCViewer/Saved/User" \
  -abslog="$PWD/ue/SCViewer/Saved/Spikes/headless-local.log"
```

```text
WP-U0 HEADLESS_PYTHON_OK engine=5.8.2-56702186+++UE5+Release-5.8
Python script executed successfully
Success - 0 error(s), 4 warning(s)
wall_seconds=3.507
```

Two warnings concerned the legacy DDC configuration syntax, subsequently migrated
to `[DerivedDataCacheGraphs]` / `[DerivedDataCacheStores]`. The remaining warnings
are writes of editor preferences outside the allowed workspace. `-UserDir` did not
redirect those engine paths; the DDC override was the effective startup workaround.

Map creation and verification used:

```bash
"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/create_viewer.py" \
  -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC \
  -abslog="$PWD/ue/SCViewer/Saved/Spikes/create-viewer-final.log"
"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/verify_viewer.py" \
  -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC \
  -abslog="$PWD/ue/SCViewer/Saved/Spikes/verify-viewer.log"
```

```text
WP-U0 VIEWER_MAP_OK
Success - 0 error(s), 2 warning(s)
wall_seconds=3.707
WP-U0 VIEWER_VERIFY_OK expected_actors=9
Success - 0 error(s), 2 warning(s)
wall_seconds=6.735
```

Creation iterations caught `StaticMeshEditorSubsystem` being unavailable in a
commandlet, refusal to overwrite an existing map, and a changed viewport-camera
API requiring `viewport_config_key`. The final creator edits the mesh's Nanite
property directly and uses a PlayerStart; each failed creator's own partial assets
were removed before retrying. No user assets existed beforehand.

A full-editor workaround was also attempted, both without and with the bounded
`XDG_CONFIG_HOME`:

```bash
XDG_CONFIG_HOME="$PWD/ue/SCViewer/Saved/BuildConfig" \
  "$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor" \
  "$PWD/ue/SCViewer/SCViewer.uproject" \
  -ExecutePythonScript="$PWD/ue/scripts/create_viewer.py" \
  -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC \
  -abslog="$PWD/ue/SCViewer/Saved/Spikes/create-viewer-local-editor.log"
```

It stalled on `open('<home>/.config/Epic/Unreal.uproject', Flags=0x00080041)
failed: errno=30 (Read-only file system)`. Engine source confirms
`FDesktopPlatformLinux::EnumerateEngineInstallations` opens a native SDL error
modal here, even with `-unattended`. Both UI attempts were interrupted.

## Vulkan attempt

```bash
ue/scripts/run_spike.sh build
```

This launches `UnrealEditor <project> -game -windowed -resx=1600 -resy=900 -vulkan
-unattended -nosplash -nop4 -ddc=SCViewerDDC -SCSpike=build
-SCSpikeCacheDir=<cwd>/cache/spike -abslog=<project>/Saved/Spikes/build.log`.

```text
Starting Vulkan Profile check for VP_UE_Vulkan_SM6:
SelectPhysicalDevice could not find a compatible Vulkan device or driver (EnumeratePhysicalDevices returned 'VK_ERROR_INITIALIZATION_FAILED' and 0 devices).  Make sure your video card supports Vulkan and try updating your video driver to a more recent version (proceed with any pending reboots).
wall_seconds=1.090
```

The startup returned without reaching `HOST`/`BUILD`. This is a failed renderer
check regardless of its exit status. No Nanite/Lumen visualization or HWRT evidence
was obtained. The installed GPU is not exposed to the tool environment.

## C: actual mesh building and complex collision

```bash
ue/scripts/run_spike.sh build -nullrhi \
  -abslog="$PWD/ue/SCViewer/Saved/Spikes/build-nullrhi.log"
```

Initial CPU run: 100,352 source triangles, Nanite 1,634 clusters, 1.461076 s build,
0.814 ms recook and a spike trace hit. Its fallback had only **1,295** triangles:
setting fallback percentage alone leaves UE's automatic fallback mode in effect.
This was not accepted as a 100k-triangle collision measurement. The probe now sets
`FallbackTarget=PercentTriangles` explicitly and retains 100% of triangles.

The successful full-resolution run used the save command from the next section:

```text
HOST editor=1 game_world=1 mode=save
GENERATE triangles=100352 seconds=0.099004
BUILD seconds=1.591184 editor=1 nanite=1 clusters=1627 input_triangles=100352 fallback_triangles=100352 physics_meshes=1 build_calls=1 cook_calls=0
COOK seconds=0.047181 editor=1 nanite=1 clusters=1627 input_triangles=100352 fallback_triangles=100352 physics_meshes=1 build_calls=1 cook_calls=1
TRACE hit_spike=1 hit_actor=StaticMeshActor_5 impact_z=151.527
```

`InvalidatePhysicsData()` clears cooked data and changes `BodySetupGuid` before the
separate timed `CreatePhysicsMeshes()` call. Earlier full-resolution iterations
measured 44.791, 44.669 and 50.363 ms; these are observations, not a benchmark suite.

## D: memory archive and cooked-package fallback

Raw-memory experiment (same bounded Build.sh arguments as A, target changed):

```bash
SC_SPIKE_RAW_SERIALIZE=1 XDG_CONFIG_HOME="$PWD/ue/SCViewer/Saved/BuildConfig" \
  "$HOME/UnrealEngine/5.8/Engine/Build/BatchFiles/Linux/Build.sh" \
  SCViewerEditor Linux Development -Project="$PWD/ue/SCViewer/SCViewer.uproject" \
  -WaitMutex -NoHotReload -NoUBA -NoUBALocal \
  -UBARootDir="$PWD/ue/SCViewer/Saved/UBA" \
  -Log="$PWD/ue/SCViewer/Saved/Spikes/build-editor-raw.log"
```

```text
ld.lld: error: undefined symbol: FStaticMeshRenderData::Serialize(FArchive&, UStaticMesh*, bool)
>>> referenced by SCSpike.cpp:189
>>> referenced by SCSpike.cpp:204
Result: Failed (OtherCompilationError)
Total execution time: 5.89 seconds
wall_seconds=5.978
```

No raw round-trip ran. The opt-in source keeps the memory writer/reader experiment
reproducible; use `-NoUBTMakefiles` when switching its environment flag to force
rules reevaluation. The normal build leaves this unresolved API disabled.

Package fallback iterations:

```bash
ue/scripts/run_spike.sh save -nullrhi -forcelogflush
ue/scripts/run_spike.sh save -nullrhi
```

- First: `Assertion failed: !IsCooking() || PackageWriter`, `SaveContext.h:625`,
  32.647 s wall. UE 5.8 requires an explicit writer for cooking.
- Ordinary writer: `Assertion failed: CookedWriter`, `SavePackage2.cpp:3532`,
  15.423 s wall. The writer must implement `ICookedPackageWriter`.
- Cooked writer without readiness wait: `SAVE_PACKAGE ok=0 seconds=1.021392`.
  `IsCachedCookedPlatformDataLoaded` was still false while distance-field/card and
  Nanite streaming preparation was pending.
- Cooked writer with the preparation wait: **passed**, `save-ready-console.log`:

```text
PACKAGE_WRITER committed=1
SAVE_PACKAGE ok=1 seconds=0.994380 bytes=2865 file=<cwd>/cache/spike/SpikeMesh.uasset
RUN_COMPLETE build_calls=1 cook_calls=1
wall_seconds=32.224
```

Files: `SpikeMesh.uasset` 2,865 bytes, `SpikeMesh.uexp` 15,368,952 bytes,
`SpikeMesh.ubulk` 2,002,846 bytes; total **17,374,663** bytes. Save builds the Linux
platform representation as needed, distinct from the editor representation.

Fresh-process reload with an initially empty DDC:

```bash
ue/scripts/run_spike.sh load -nullrhi \
  -SCViewerDDCPath="$PWD/ue/SCViewer/Saved/Spikes/ColdDDC" \
  '-ExecCmds=r.Nanite.AllowSkinnedMeshes'
```

`LOAD_PACKAGE seconds=0.023119`, Nanite=1, 1,633 clusters, 100,352 fallback triangles,
one physics mesh, trace hit, `build_calls=0 cook_calls=0`; 29.063 s process wall.
`r.Nanite.AllowSkinnedMeshes = "1" LastSetBy: Constructor` was observed.

The final validation additionally checks cooked flags, absent source models and
forces a read of all streaming bulk data, rather than relying on NullRHI to request
GPU pages. It used another initially empty DDC:

```bash
ue/scripts/run_spike.sh load -nullrhi \
  -SCViewerDDCPath="$PWD/ue/SCViewer/Saved/Spikes/ColdDDC2"
```

```text
HOST editor=1 game_world=1 mode=load
CACHE_OBJECT cooked=1 source_models=0
CACHE_NANITE root_bytes=26504 stream_bytes=1999926 stream_read=1
LOAD_PACKAGE seconds=0.013941 editor=1 nanite=1 clusters=1633 input_triangles=100352 fallback_triangles=100352 physics_meshes=1 build_calls=0 cook_calls=0
TRACE hit_spike=1 hit_actor=StaticMeshActor_5 impact_z=151.527
RUN_COMPLETE passed=1 build_calls=0 cook_calls=0
wall_seconds=27.805
```

Logs confirm the chosen empty DDC path was writable and actually selected. They
contain no `Building static mesh SpikeMesh` during reload. Unrelated engine and
Viewer map assets do rebuild in the empty DDC. The claim is zero processing of the
**cached spike**, not zero processing across the entire editor process. Rendering
of the cached mesh has not been tested because no GPU is available.

## Final source/descriptor verification

After disabling the unrelated default Android file-server plugin (which had added
mobile settings to `DefaultEngine.ini`), the final descriptor build passed:

```bash
ue/scripts/build.sh SCViewerEditor
```

```text
Result: Succeeded
Total execution time: 2.54 seconds
wall_seconds=2.642
```

The final headless invocation passed with the final configuration:

```bash
"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/headless_smoke.py" \
  -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC \
  -abslog="$PWD/ue/SCViewer/Saved/Spikes/headless-final.log"
```

```text
WP-U0 HEADLESS_PYTHON_OK engine=5.8.2-56702186+++UE5+Release-5.8
Success - 0 error(s), 2 warning(s)
wall_seconds=4.114
```

```bash
bash -n ue/scripts/build.sh ue/scripts/run_spike.sh
git diff --check
```

Both passed silently. Final `df -h .` reported 76G available, 96% used;
project build scratch remained about 434 MiB plus the 17 MiB spike cache. An inline `UV_CACHE_DIR=/tmp/scviewer-u0-uv uv run
--no-project python` check parsed the three JSON descriptors and Python scripts,
checked the successful map/save/load log markers, checked for no `SpikeMesh`
rebuild on reload, summed cache file sizes, verified the submodule was untouched,
and audited `git ls-files --cached --others --exclude-standard ue .claude`:

```text
PASS: JSON, Python/shell syntax, cache/log evidence, submodule unchanged, 27 source/content/doc files free of forbidden build/export products
```

## Open work

TODO(WP-U0): Complete Vulkan/Nanite/Lumen/HWRT visual verification, including cache reload, outside the GPU-less tool environment.
TODO(WP-U0): Commit on the now-present `wp/u0-unreal-spike` branch once `.git` is writable; staging is blocked. No push or PR.
TODO(WP-U0): Interchange baseline deferred; the requested export wrapper invokes cargo, which was prohibited. No orientation/size/feature-preservation result is claimed.
