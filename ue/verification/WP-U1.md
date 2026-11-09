# WP-U1 verification — 2026-11-09

Implemented in the assigned working tree without Git writes or Cargo commands.
The orchestrator owns commits and cross-review. Only `ue/**` and the WP-U1 brief
were edited. All runs used UE 5.8.2, CL 56702186. Commands below were run from the
repository root; `$HOME` and `$PWD` replace their expanded machine paths.

## Implementation and decision

Runtime `SCBridge`: `FSCPackageSource`, folder/tar sources, `FSCPackageReader`,
`FSCMeshImporter`, `FSCImporter`, `ASCVehicle`, `USCClipCatalog`, and reflected
`USCImportLibrary`. Editor `SCBridgeEditor`: File-menu folder picker,
`USCInterchangePipeline` and `USCInterchangeBaseline`.

The public C++ entry point is:

```cpp
bool FSCImporter::Import(FSCPackageSource& Source,
                        FSCImportOptions Options,
                        FSCImportResult& Result);
```

`Options` supplies World, optional ScenePath, bBuildCollision, bReplacePrevious,
and ActorTransform. `Result` exposes the actor weak reference, errors, counts,
timings, input hashes and per-mesh build statistics. The folder/Python convenience
entry point positions the bounds above the Viewer floor; the C++ API honors the
specified transform. `Unload(World)` releases the previous actor. Imports only
publish a replacement after assembly succeeds.

The source interface is read-only: `Exists(Path)`,
`ReadBytes(Path, OutBytes, Error)`, `ListFiles(Prefix)`. Paths are relative to the
export root. `FSCTarPackageSource(TArray<uint8>&&)` owns and indexes uncompressed
USTAR/GNU bytes, so the FFI caller can release its original buffer. No extraction
or temporary mesh files occur in the production importer. The folder source also
checks real paths against its root, including symlink targets.

**Select the direct GLB → FMeshDescription path**, retaining Interchange solely
as an editor baseline. The direct path preserves material slot IDs and helper
transforms, controls transient ownership and exposes pure derived data. Editor
`Build()` fixes WP-U0's absent Lumen/RT representation. The default fallback
retains 100% of nondegenerate triangles. The original spike is disabled unless
compiled with `SC_SPIKE=1`.

`Derive` returns the pure mesh description, slot names, helpers, input SHA-1 and
root transforms. `DeriveCollision` builds a Chaos BVH on the worker and serializes
it into pure bytes plus an engine custom-version container. `Instantiate` creates
UObjects and replays the BVH without cooking again. WP-C1 can cache these outputs;
Nanite/card/distance-field processing is still the engine's UObject build phase,
whose processed package WP-C1 can replay. See `ue/README.md` for the complete
mapping, API boundaries and supported GLB/tar features.

## Environment and input revisions

```bash
vulkaninfo --summary
```

Exit 1:

```text
Failed to detect any valid GPUs in the current config
vkEnumeratePhysicalDevices failed with ERROR_INITIALIZATION_FAILED
```

Every engine run here used `-nullrhi`. GPU rendering is not claimed.

The shared input changed during concurrent Rust work. Initial root GLB SHA-1 was
`6A5226CB0298B8D09FE418CBB0017DF2EB3FD2DC` (24 material slots). Final root SHA-1 is
`12521A37B55E390A4CA20CF9FD573F4A5D1E08B6` (27 slots). Both have 274 nodes / 89
meshes and identical hull bounds. The first tar contained 503 files / 99,543,040
bytes; it was replaced with a current snapshot before final acceptance. Final
folder/tar verification compares every referenced GLB hash as well as counts.
The manifest still has 51 children, 3 interiors and 63 clip records. It references
45 meshes and 17 material sidecars; other files in the export root are not loaded.

Sidecars preserve sparse authored indices in addition to array ordinals. The
initial radar GLB's compacted slot ordering differed from its shared sidecar's
array order. No material-name parsing was introduced to work around this. Real
materials are WP-U2; this WP retains primitive slot identity, raw material records,
array ordinal and authored source index separately.

## Final build

```bash
ue/scripts/build.sh SCViewerEditor \
  > ue/SCViewer/Saved/Spikes/build-u1-final-console.log 2>&1
```

Exit 0:

```text
Result: Succeeded
Total execution time: 3.12 seconds
```

The installed UBA occasionally reports the pre-existing nonfatal action-result
store warning. No engine source was edited.

## Fresh-DDC game host, folder/tar, collision and GC

```bash
python3 -I ue/scripts/make_test_tar.py out/dec ue/SCViewer/Saved/Import/dec.tar

timeout -k 5 240 ue/scripts/run_import.sh out/dec -nullrhi \
  -SCVerifyTar="$PWD/ue/SCViewer/Saved/Import/dec.tar" \
  -SCViewerDDCPath="$PWD/ue/SCViewer/Saved/Import/ColdFinalDDC" \
  > ue/SCViewer/Saved/Spikes/verify-final-cold-console.log 2>&1
```

Both exit 0. `ColdFinalDDC` did not exist before this run. The launch helper runs
`UnrealEditor SCViewer.uproject -game -windowed -resx=1600 -resy=900 -unattended
-nosplash -nop4 -ddc=SCViewerDDC -SCPackage=<absolute folder>` with the supplied
arguments. Project DDC is overridden only for this cold measurement.

```text
TAR files=502 bytes=101785600
IMPORT_OK seconds=10.0826 assets=45 instances=61 children=51 interiors=3 materials=17 palettes=18 clips=63 game_world=1
TRACE hit_hull=1
IMPORT_OK seconds=1.9439 assets=45 instances=61 children=51 interiors=3 materials=17 palettes=18 clips=63 game_world=1
VERIFY passed=1 folder_assets=45 tar_assets=45 folder_instances=61 tar_instances=61 representations=1 old_live=0 tracked_old=581 baseline=33 loaded=78 reloaded=78 final=33 first_s=10.0826 second_s=1.9439
```

The hull's cold mesh line:

```text
triangles=164881 degenerate=10 nanite=accepted clusters=2941 fallback=164881
cards=10 distance_field=1 rt_geometry=1 collision=1
derive_s=0.6577 build_s=1.8499 representation_wait_s=0.6064 cook_s=0.0481 skins_static=0
```

`derive_s` includes the pure collision work; `cook_s` is that subset, not an
additional cost. `build_s` includes engine representation work initiated inside
`Build()`, and `representation_wait_s` measures the remaining card/DF queue wait.
The second hull build was 0.1177 s with zero representation wait; pure collision
still ran (0.0460 s). This is an engine-DDC-assisted reload, **not** WP-C1's
persistent zero-processing cache.

Forty-four mesh assets contain geometry; one is hierarchy-only and logs
`nanite=disabled:empty_geometry`. Every drawable asset has Nanite, a distance-field
object, built card representation and RT proxy geometry. Eight tiny planar glow
meshes receive zero selected cards; the hull has ten. No asset-name rules select
this behavior. Geometry-only, empty and planar cases are handled by structure.
All 61 drawable placements share the 44 drawable meshes.

The verification executes `obj list class=StaticMesh` before import and after
unload/GC. Both outputs contain:

```text
33 Objects (Total: 0.141M / Max: 0.144M / Res: 4.092M | ResDedSys: 0.193M / ResDedVid: 0.000M / ResUnknown: 3.899M)
```

It also counts loaded/reloaded meshes and tracks 581 weak references covering the
old actor, components, clip catalog, packages, meshes and their subobjects.
All expire after replacement and GC. A world Visibility line trace must hit the
root hull component, not another attachment or the floor.

## Registered automation tests and headless Python

```bash
timeout -k 5 180 "$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/test_importer.py" \
  -SCPackage="$PWD/out/dec" -SCVerifyTar="$PWD/ue/SCViewer/Saved/Import/dec.tar" \
  -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC \
  -abslog="$PWD/ue/SCViewer/Saved/Spikes/unit-integration-final.log" \
  > ue/SCViewer/Saved/Spikes/unit-integration-final-console.log 2>&1

timeout -k 5 180 "$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/import_package.py" -SCPackage="$PWD/out/dec" \
  -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC \
  -abslog="$PWD/ue/SCViewer/Saved/Spikes/python-import-final.log" \
  > ue/SCViewer/Saved/Spikes/python-import-final-console.log 2>&1
```

Both exit 0:

```text
UNIT_TEST name=FSCReaderSparse passed=1
UNIT_TEST name=FSCPaths passed=1
UNIT_TEST name=FSCBasis passed=1
UNIT_TEST name=FSCFolderTarImport passed=1
VERIFY passed=1 folder_assets=45 tar_assets=45 folder_instances=61 tar_instances=61 representations=1 old_live=0 tracked_old=581 baseline=27 loaded=72 reloaded=72 final=27 first_s=8.4805 second_s=2.0638
Success - 0 error(s), 2 warning(s)
```

The independent Python entry point's final run:

```text
IMPORT_OK seconds=1.9750 assets=45 instances=61 children=51 interiors=3 materials=17 palettes=18 clips=63 game_world=0
Success - 0 error(s), 2 warning(s)
```

The commandlet uses an editor world; the separate `-game` run above verifies the
chosen standalone host. Tests cover unsafe paths, malformed tar checksum, sparse
material records, basis/units, authored attributes, helper/interior composition,
hierarchy-only GLBs, truncated GLB headers, real folder/tar hash/count parity,
mesh representations, tracing and lifetime. Fixtures are synthetic and written
only to ignored `Saved/Import` scratch.

## Interchange baseline and regressions

After `test_importer.py` generates the synthetic GLB:

```bash
timeout -k 5 90 "$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/interchange_baseline.py" \
  -SCBaseline="$PWD/ue/SCViewer/Saved/Import/synthetic.glb" \
  -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC \
  -abslog="$PWD/ue/SCViewer/Saved/Spikes/baseline-final.log" \
  > ue/SCViewer/Saved/Spikes/baseline-final-console.log 2>&1
```

Exit 0:

```text
SC_BASELINE synthetic_attributes_match=1 imported_sign=1.0 direct_sign=1.0
SC_BASELINE mesh=synthetic triangles=1 nanite=1 uv_channels=2 colours=3 bounds_cm=X=0.000 Y=200.000 Z=300.000 direct_bounds_match=1 seconds=0.0416
SC_BASELINE complete=1 meshes=1 direct_match=1 error=
```

The comparison checks corner ordering, positions, normals, tangents and their
signs, both UV channels and colours, including non-primary mid-tones. It failed
before the fixes: the first run reported imported sign +1 versus direct −1;
a separate mid-tone regression reported mismatched colours with signs equal.
UE's vertex-colour packing requires the same compensating conversion used by
Interchange. The final implementation matches that baseline.

The real initial hull baseline ran with the same command, timeout 180,
`-SCBaseline="$PWD/out/dec/Data/Objects/Spaceships/Ships/AEGS/Gladius/AEGS_Gladius_LOD1.glb"`,
and log names `baseline-gladius.log` / `baseline-gladius-console.log`:

```text
SC_BASELINE mesh=AEGS_Gladius_LOD1 triangles=164891 nanite=1 uv_channels=2 colours=25163 bounds_cm=X=1968.933 Y=1735.958 Z=512.927 direct_bounds_match=1 seconds=17.2919
SC_BASELINE complete=1 meshes=1 direct_match=1 error=
Failure - 1 error(s), 2 warning(s)
```

The measured bounds are **19.68933 × 17.35958 × 5.12927 m**, with forward +X and
up +Z. The direct path drops ten zero-area triangles; Interchange's source mesh
description retains them. The commandlet exits 1 despite completing the baseline:
UE's GLTFReader treats object-valued `asset.extras.export_options` as a string and
logs `Json Value of type 'Object' used as a 'String'`. The direct importer handles
that valid metadata without errors. No engine/input mutation or error suppression
was used to make this baseline green.

Earlier investigation also found and fixed rejection of hierarchy-only GLBs and
source-root fallback for absent attachment helpers. An attempted full-editor
`-ExecCmds="Automation RunTests SCBridge" -TestExit="Automation Test Queue Empty"`
run stalled trying to write the read-only user settings path and was interrupted;
the Python commandlet runner avoids that UI startup. An initial test-runner call
using beautified names rather than registered test-class names asserted; the
runner now uses the actual registrations and checks that a test started before
stopping it. Intermediate build/test logs are ignored scratch; only the successful
commands above establish final acceptance.

## Remaining acceptance

TODO(WP-U1): On the GPU host, run `ue/scripts/run_import.sh out/dec -vulkan -SCStay`; inspect all attachments, grey surfaces and orientation in the Viewer level. The editor folder-picker menu also needs interactive smoke validation there.
TODO(WP-U1): Capture `r.Nanite.Visualize Overview`, then `ShowFlag.VisualizeLumen 1` and `r.Lumen.Visualize 1`; require visible hull clusters, hull in Lumen Scene and Reflection View/HWRT with no black cut-out, and GI response when the hull receives an emissive test material. CPU representation checks cannot establish these visual results.
TODO(WP-U1): Reconcile the direct-builder decision from `ue/README.md` into `docs/03-unreal-export-design.md`; that file was outside this task's write scope.
TODO(WP-U1): Resolve the final exporter material-slot/sidecar-index contract with WP-U2 before real-material binding. The initial and regenerated inputs differ; this WP deliberately uses a grey placeholder and retains both index forms.
TODO(WP-U1): The real-GLB Interchange baseline emits the installed engine's object-valued-extras JSON error; synthetic baseline is green and real hull dimensions match. Keep Interchange outside the production path.
TODO(WP-U1): Orchestrator to stage these paths, cross-review and commit on the work-package branch; `.git` is read-only here.

## Final source audit and changed files

```bash
bash -n ue/scripts/build.sh ue/scripts/run_import.sh ue/scripts/run_spike.sh
git diff --check -- ue docs/work-packages/WP-U1-scbridge-importer.md
```

Both passed. A `UV_CACHE_DIR=/tmp/scviewer-u1-uv uv run --no-project python`
AST/JSON check parsed all `ue/scripts/*.py` and the plugin descriptor successfully.
A Git-path audit confirmed the changed/untracked paths contain no build outputs or
game-derived files. Final observed project DDC was 453 MiB, Intermediate 396 MiB;
two isolated cold-DDC directories totalled 80 MiB. Disk had 69 GiB available.
The final source-only helper rename prevents anonymous helper collisions in UE
unity builds; the final build passed after it.

Changed paths (27); these are the orchestrator's staging list:

```text
docs/work-packages/WP-U1-scbridge-importer.md
ue/Plugins/SCBridge/SCBridge.uplugin
ue/Plugins/SCBridge/Source/SCBridge/Private/SCImportTests.cpp
ue/Plugins/SCBridge/Source/SCBridge/Private/SCImporter.cpp
ue/Plugins/SCBridge/Source/SCBridge/Private/SCMeshImporter.cpp
ue/Plugins/SCBridge/Source/SCBridge/Private/SCPackageReader.cpp
ue/Plugins/SCBridge/Source/SCBridge/Private/SCPackageSource.cpp
ue/Plugins/SCBridge/Source/SCBridge/Private/SCVehicle.cpp
ue/Plugins/SCBridge/Source/SCBridge/Public/SCImporter.h
ue/Plugins/SCBridge/Source/SCBridge/Public/SCMeshImporter.h
ue/Plugins/SCBridge/Source/SCBridge/Public/SCPackageReader.h
ue/Plugins/SCBridge/Source/SCBridge/Public/SCPackageSource.h
ue/Plugins/SCBridge/Source/SCBridge/Public/SCVehicle.h
ue/Plugins/SCBridge/Source/SCBridge/SCBridge.Build.cs
ue/Plugins/SCBridge/Source/SCBridge/SCBridge.cpp
ue/Plugins/SCBridge/Source/SCBridge/SCSpike.cpp
ue/Plugins/SCBridge/Source/SCBridgeEditor/Private/SCInterchangeBaseline.cpp
ue/Plugins/SCBridge/Source/SCBridgeEditor/Public/SCInterchangeBaseline.h
ue/Plugins/SCBridge/Source/SCBridgeEditor/SCBridgeEditor.Build.cs
ue/Plugins/SCBridge/Source/SCBridgeEditor/SCBridgeEditor.cpp
ue/README.md
ue/scripts/import_package.py
ue/scripts/interchange_baseline.py
ue/scripts/make_test_tar.py
ue/scripts/run_import.sh
ue/scripts/test_importer.py
ue/verification/WP-U1.md
```
