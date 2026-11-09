# WP-V1 verification — 2026-11-09 (Opus 5.5, RTX 4090 / Vulkan, host B)

App shell and model browser in the Runtime module, driven through the FFI
(`libscbridge.so`, ABI v2) in the chosen host (`SCViewerEditor -game`). No cargo
was run; the staged release `.so` was used as is.

## What was built

| Piece | Files (`ue/Plugins/SCBridge/Source/SCBridge/…`) |
| --- | --- |
| FFI client: one handle, host lock around every call, `UE::Tasks` workers, `FText` errors, progress/cancel thunk | `Public/SCBridgeClient.h`, `Private/SCBridgeClient.cpp` |
| Incremental importer (the WP-U1/U2 `TODO(WP-V1)`): `FSCImportJob` steps Reading → Meshes → Materials → Assembling; manifest + clip sidecars, mesh derive (3-asset look-ahead), prepare (bindings, partition, collision BVH, translucent split) on workers; UObject creation on the game thread within a 30 ms budget per frame, checked after every step (a step is at most one static-mesh variant build since the review round; before it, all variants of one source asset); phase text/fraction; cancel at step boundaries. `FSCImporter::Import` is now a blocking wrapper over the job (same mapping, same logs, plus `IMPORT_PHASES`). | `Public/SCImportJob.h`, `Private/SCImportJob.cpp`, `Private/SCImporter.cpp` |
| `USCViewOptions` (Config, Blueprint/Python bound; defaults from `sc_default_options`, options hash) and `USCViewerSettings` (archive path, cache dir, recents, fly speed) in `Saved/Config/<Platform>/GameUserSettings.ini` | `Public/SCViewOptions.h`, `Public/SCViewerSettings.h` + `.cpp` |
| `USCViewerSubsystem` (`UGameInstanceSubsystem`): archive open (auto-discovery or settings/`-SCArchive`, cache `<launch dir>/cache` resolved with `FPlatformMisc::LaunchDir()`), async search, `LoadEntity`/`CancelLoad`/`Unload`/`WaitForLoad`/`WaitForPhase`/`SetCameraMode`/`FocusComponent`/`IsolateComponent`/`Keep`, reuse on entity + options hash, unload + GC accounting, `CreateHeadless`/`DestroyHeadless` for commandlets | `Public/SCViewerSubsystem.h`, `Private/SCViewerSubsystem.cpp` |
| Game mode, controller (adds the UI, GameAndUI input), camera pawn (orbit with exact box fit + eased focus, free-fly; first person refused until WP-P1) | `Public/SCViewerGameMode.h`, `Public/SCViewerPawn.h` + `.cpp`; `DefaultEngine.ini` `GlobalDefaultGameMode` |
| Slate UI: top bar (archive, camera modes, Keep when the editor module is loaded), model browser (debounced search → `SListView`, recents, options, Load/Cancel, progress), 3D view (player view rect restricted to the gap between panels), hierarchy (`STreeView`, static/skeletal/light + Nanite + triangle badges, select → focus, isolate, frame all), footer (export/parse/mesh build/collision/materials/total, cache hit/miss and size, memory, disk headroom), archive overlay with the `IDesktopPlatform` file dialog (opens by itself when discovery fails) | `Private/UI/*` |
| Automation: `-SCLoad=a,b`, `-SCHold`, `-SCCamera`, `-SCFlyView`, `-SCDefaults`, `-SCLod`, `-SCMip`, `-SCQuitAfterLoad`, `-SCSearch`, `-SCCacheDir`, `-SCArchive`, `-SCNoUI`; console `sc.Load`, `sc.Unload`, `sc.Cancel`, `sc.Camera` | `Private/SCViewerAutomation.*` |
| Editor host Keep (`SCBridgeEditor`): renames the transient objects into `/Game/SC/<Entity>/` packages, MIDs → MICs, mesh slot materials from component overrides, texture sources from mip 0, saves | `SCBridgeEditor/Private/SCKeepAssets.*`, `SCBridgeEditor.cpp` |
| Scripts | `ue/scripts/ue_load_gladius.py`, `test_viewer_cancel.py`, `test_keep.py`, `capture_viewer.sh` |

The `-SCPackage` harness (WP-U1..U3 review runs) skips the archive open and the UI.

## Commands and key output

```bash
ue/scripts/build.sh SCViewerEditor                       # Result: Succeeded, no warnings

# Standalone host, cold: empty cache dir (index + package miss)
SC_SETTLE=10 ue/scripts/capture_viewer.sh v1-gladius-cold 'VIEWER_LOAD result' \
  -SCDefaults -SCSearch=gladius -SCLoad=AEGS_Gladius -SCCacheDir=<scratch>/coldcache
#  VIEWER_ARCHIVE ok=1 seconds=1.549 index_cache_hit=0
#  VIEWER_PHASE t=0.058 Export: Resolving loadout … t=7.368 Export: Publishing package
#  VIEWER_PHASE t=7.435 Reading package · t=7.606 Import: Deriving mesh 1/45 · … · t=10.239 Import: Assembling actor
#  VIEWER_LOAD result=Succeeded entity=AEGS_Gladius export_s=7.424 export_cache_hit=0 parse_s=0.004
#    import_s=3.149 derive_s=1.701 build_s=0.687 collision_s=0.107 materials_s=0.112 assemble_s=0.345 total_s=10.594
# Warm (same cache dir, fresh process)
#  VIEWER_ARCHIVE ok=1 seconds=0.094 index_cache_hit=1
#  VIEWER_LOAD … export_s=0.148 export_cache_hit=1 parse_s=0.051 import_s=3.098 total_s=3.331

# Standalone host, second ship frees the first (export misses for Scorpius/Cutlass)
ue/scripts/capture_viewer.sh v1-sequence AUTOMATION_DONE -SCDefaults \
  -SCLoad=AEGS_Gladius,RSI_Scorpius,DRAK_Cutlass_Black,AEGS_Gladius -SCHold=3 -SCCacheDir=<scratch>/coldcache
#  Gladius   total_s=3.283  live_static_meshes=320 live_vehicles=1
#  VIEWER_UNLOAD tracked=5704  alive_after_gc=0 live_static_meshes=33
#  Scorpius  export_s=6.051 (miss) import_s=3.693 total_s=9.829  live_static_meshes=791 previous_alive=0
#  VIEWER_UNLOAD tracked=10614 alive_after_gc=0 live_static_meshes=33
#  Cutlass   export_s=6.455 (miss) import_s=4.230 total_s=10.781 live_static_meshes=492 previous_alive=0
#  VIEWER_UNLOAD tracked=8832  alive_after_gc=0 live_static_meshes=33
#  Gladius   total_s=2.989  live_static_meshes=320 (same as the first load)

# Headless (WP-Q1 entry): LoadEntity through the subsystem in a commandlet, exit 0
"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" "$PWD/ue/SCViewer/SCViewer.uproject" \
  -run=pythonscript -script="$PWD/ue/scripts/ue_load_gladius.py" -SCAlso=RSI_Scorpius,DRAK_Cutlass_Black \
  -nullrhi -unattended -nop4 -nosplash -ddc=SCViewerDDC
#  LOAD AEGS_Gladius  total_s=2.593 counts={static_meshes: 317, skeletal_meshes: 13, textures: 330, material_instances: 291, vehicles: 1}
#  VIEWER_UNLOAD tracked=5784 alive_after_gc=0
#  LOAD RSI_Scorpius  total_s=3.135 previous_objects=5784 previous_alive=0 counts={788, 15, 307, 353, 1}
#  VIEWER_UNLOAD tracked=10706 alive_after_gc=0
#  LOAD DRAK_Cutlass_Black total_s=3.747 previous_objects=10706 previous_alive=0 counts={489, 15, 468, 654, 1}
#  LOAD AEGS_Gladius  total_s=2.573 previous_alive=0 counts={317, 13, 330, 291, 1}   (identical to the first)
#  LOAD AEGS_Gladius  reused=True total_s=0.000
#  WP-V1 HEADLESS_LOAD_OK entity=AEGS_Gladius components=567 clips=63 · WP-V1 HEADLESS_TEARDOWN_OK

# Cancel in both phases, handle stays usable (exit 0)
UnrealEditor-Cmd … -script="$PWD/ue/scripts/test_viewer_cancel.py" …
#  FFI_EXPORT status=5 (SC_STATUS_CANCELLED) · CANCEL during_export result=CANCELLED vehicle=None
#  IMPORT_CANCELLED after 0.140 s · CANCEL during_import result=CANCELLED vehicle=None
#  CANCEL after result=SUCCEEDED vehicles=1 · WP-V1 CANCEL_OK

# Editor-host Keep, then a fresh process reloads the saved assets (both exit 0)
UnrealEditor-Cmd … -script="$PWD/ue/scripts/test_keep.py" …
#  KEEP root=/Game/SC/AEGS_Gladius packages=901 saved=901 warnings=13 seconds=1.510
#  kinds={StaticMesh: 287, Texture2D: 253, MaterialInstanceConstant: 267, AnimSequence: 80, Skeleton: 13, SCClipCatalog: 1}
UnrealEditor-Cmd … -script="$PWD/ue/scripts/test_keep.py" -SCKeepReload=/Game/SC/AEGS_Gladius …
#  KEEP_DISK assets=901 failed=0 static_meshes=287 with_kept_materials=287 textures_sized=253 · KEEP_DISK_OK
# (the generated /Game/SC/AEGS_Gladius was deleted afterwards; Content/SC/ is gitignored)

# Importer regressions after the job refactor (all exit 0)
UnrealEditor-Cmd … -script="$PWD/ue/scripts/test_importer.py" -SCPackage="$PWD/out/dec-r5" \
  -SCVerifyTar="$PWD/ue/SCViewer/Saved/Import/v1-dec-r5.tar" …
#  VERIFY passed=1 folder_assets=45 tar_assets=45 old_live=0 baseline=27 loaded=314 reloaded=314 final=27 first_s=2.4740
#  WP-U1 UNIT_TESTS_OK   (first_s was 4.68 before the assembly fix below)
ue/scripts/run_import.sh out/dec-scorpius -nullrhi -SCVerifyRig        # RIG_VERIFY passed=1 clips=69 bound=65 rigs=23
ue/scripts/run_import.sh out/dec-cutlass_black -nullrhi -SCVerifyRig   # RIG_VERIFY passed=1 clips=40 bound=34 lights=225
```

Responsiveness: during the cold Gladius import the frame counter advanced 372 → 877
(game thread ticking while workers derive). Root-cause fix found on the way: the
assembly re-parsed every submaterial's JSON (`FSCImportMaterials::HostChannel`) per
component variant; memoised per (sidecar, mesh) → Scorpius assembly 7.81 s → 0.38 s,
Gladius 2.33 s → 0.28 s. At commit e90abb8 one step built every variant of a source asset
(the per-mesh `build_s` below is per variant, so a step could be several of them); the
review round splits it to one variant per step. Largest single variant build seen: 0.56 s
on a cold-DDC `TMBL_Storm` (19.5 s summed over its variants).

Settings persistence: `GameUserSettings.ini` gains `[/Script/SCBridge.SCViewerSettings]`
(recents) and `[/Script/SCBridge.SCViewOptions]` (last options); a restart shows the
recents (`cutlass-freefly.jpg`). `Game.ini` was tried first and silently not written
(UE 5.8 saves only `[SectionsToSave]` there).

## Screenshots (`ue/verification/wp-v1/`, half size)

| File | Shows |
| --- | --- |
| `startup-picker.jpg` | `-SCArchive=/nonexistent/Data.p4k`: archive overlay with the error, Choose/Retry, and the native "Open File" dialog that opened by itself |
| `browser-results.jpg` | search "cutlass": 259 results (display name, class, category chip), options panel, idle progress, footer with cache size, memory, disk |
| `gladius-hierarchy.jpg` | Gladius loaded, fitted in the centre view; hierarchy (567 components · 171 Nanite · 76 lights · 473.4k triangles, badges); footer timings |
| `cutlass-freefly.jpg` | Cutlass Black, free-fly inside the cargo bay (`-SCCamera=fly -SCFlyView=-300,0,-60,-3,0`), fly speed in the top bar, recents persisted |

## Open issues

TODO(WP-V1): Build static meshes with asynchronous compilation (`Build` without `FinishCompilation`, poll `IsCompiling`) so Nanite/DF/card builds of several meshes run in parallel off the game thread; today each mesh build is one game-thread step (≤ 0.56 s each on cold DDC).
TODO(WP-V1): Mouse input (drag/pan/zoom, double-click picking, hierarchy click-to-focus) has no scripted test; the only interactive evidence is two UI-initiated loads (TMBL_Storm, RSI_Salvation_PU_AI_CRIM) in `Saved/Viewer/v1-sequence.log`; the editor PIE host and its Keep button were verified only through the commandlet (`test_keep.py`).
TODO(WP-U3): Runtime skeletal meshes have no editor source model, so Keep saves their skeletons and clips but skips the meshes (13 warnings on the Gladius, incl. the soft-skinned hull).
TODO(WP-R4): Expose the archive build id/branch (`Session::build_branch`) through the FFI so the startup screen can show it (design doc §In-game UI).
TODO(WP-P1): First-person camera mode (the enum value, UI button and `SetCameraMode` refusal are in place).
TODO(WP-C1): Layer-2 hit/miss and cache size in the footer once the processed cache exists (layer 1 is shown).

## Review round (Codex cross-review `WP-V1-review.md`, 2026-11-09)

| # | Finding | Fix |
| --- | --- | --- |
| 1 | tar length narrowed into `TArray<uint8>` | `SCBridgeClient.cpp`: `Blob.len > MAX_int32` is refused before allocation with an actionable error ("try a higher LOD or mip"); the blob is freed on every OK path; the log reports the real FFI size (`bytes=`, `accepted=`) |
| 2 | panic left the archive "Ready", search/catalogue kept a poisoned handle, export freed the unspecified blob | one `FailLocked` path for search, catalogue, export and cache stats: on `SC_STATUS_PANIC` the handle is closed under the host lock, outputs are neither read nor freed (the panicked blob is deliberately leaked), and a pending panic is flagged; the subsystem polls it (`PollClientPanic`, also after blocking calls), sets `ArchiveState=Failed` with the error so the overlay offers Choose/Retry, and starts a new archive generation |
| 3 | reuse across archives, stale search results | `OpenArchive` increments `ArchiveGeneration`; the reuse key is `generation|entity|options-hash`; searches carry their generation and older results are dropped; `FSCLoadReport.ArchiveGeneration` |
| 4 | budget/cancel checked only after all variants of an asset | `FSCImportJob` builds one static-mesh variant (part or its translucent split) per step: `BeginBuild` (package + rig) → `BuildNextVariant` × n → `FinishBuild`; session packages are held by the job between steps; phase text "Building mesh i/N (part p/P)"; `build_s` counts game-thread build time only |
| 5 | cancel test did not prove cleanup | the subsystem tracks every object of a cancelled/failed import (partial actor + `/Temp/SC` session packages), collects garbage and reports `PartialObjects`/`PartialObjectsAlive`, plus `MeshesBuiltAtCancel` and `bWorkersPendingAtCancel`; `WaitForImportedMeshes(n)` pumps one job step at a time to a mid-import state with workers pending; `DestroyHeadless` now goes through `UGameInstance::Shutdown` (the real `Deinitialize` path) and returns the survivors after GC; `test_viewer_cancel.py` rewritten (4 cases), `ue_load_gladius.py` asserts 0 survivors |
| 6 | query typed during archive startup was lost | the browser keeps the query ("Waiting for the archive…") and submits it on the transition to Ready; leaving Ready clears the list and re-arms the query |
| 7 | Frame all / F kept isolation | `USCViewerSubsystem::FrameAll()` restores visibility, clears isolation and focus, frames the vehicle; the hierarchy clears selection and its isolate box on the null-focus broadcast |

The new teardown assertion caught a real gap while verifying: with a loaded vehicle and no
import in flight, 5783 of 5784 objects survived teardown. `Deinitialize` now unloads the
vehicle the subsystem owns (skipped while the world is tearing down at engine exit), and the
script drops its Python reference (`del vehicle`) before teardown; rerun below shows 0.

```bash
ue/scripts/build.sh SCViewerEditor                                     # Result: Succeeded, no warnings

UnrealEditor-Cmd "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/test_viewer_cancel.py" -nullrhi -unattended -nop4 -nosplash -ddc=SCViewerDDC   # exit 0
#  FFI_EXPORT entity=AEGS_Gladius status=5 … accepted=0
#  CANCEL during_export result=CANCELLED vehicle=None workers_pending_at_cancel=True vehicles=0
#  VIEWER_IMPORT_CLEANUP stage=6 meshes_built_at_cancel=3 workers_pending_at_cancel=1 tracked=29 alive_after_gc=0
#  CANCEL during_import result=CANCELLED vehicle=None meshes_built_at_cancel=3 partial_objects=29 partial_alive=0 vehicles=0
#  CANCEL after result=SUCCEEDED vehicles=1
#  VIEWER_HEADLESS_TEARDOWN in_flight=1 tracked=29 alive_after_gc=0
#  CANCEL teardown_with_pending_work alive_after_gc=0 · WP-V1 CANCEL_OK

UnrealEditor-Cmd … -script="$PWD/ue/scripts/ue_load_gladius.py" -SCAlso=RSI_Scorpius,DRAK_Cutlass_Black …   # exit 0
#  LOAD AEGS_Gladius        total_s=2.857 build_s=0.614 counts={317, 13, 330, 291, 1}
#  VIEWER_UNLOAD tracked=5784  alive_after_gc=0
#  LOAD RSI_Scorpius        total_s=3.543 previous_objects=5784  previous_alive=0 counts={788, 15, 307, 353, 1}
#  VIEWER_UNLOAD tracked=10706 alive_after_gc=0
#  LOAD DRAK_Cutlass_Black  total_s=4.813 previous_objects=10706 previous_alive=0 counts={489, 15, 468, 654, 1}
#  VIEWER_UNLOAD tracked=8895  alive_after_gc=0
#  LOAD AEGS_Gladius        total_s=4.515 previous_alive=0 counts={317, 13, 330, 291, 1}   (identical to the first)
#  LOAD AEGS_Gladius        reused=True
#  VIEWER_HEADLESS_TEARDOWN in_flight=0 tracked=5784 alive_after_gc=0 · HEADLESS_TEARDOWN_OK alive_after_gc=0

UnrealEditor-Cmd … -script="$PWD/ue/scripts/test_importer.py" -SCPackage="$PWD/out/dec-r5" \
  -SCVerifyTar="$PWD/ue/SCViewer/Saved/Import/v1-dec-r5.tar" …                                           # exit 0
#  VERIFY passed=1 folder_assets=45 tar_assets=45 old_live=0 baseline=27 loaded=314 reloaded=314 final=27
#  WP-U1 UNIT_TESTS_OK
ue/scripts/run_import.sh out/dec-scorpius -nullrhi -SCVerifyRig        # exit 0, RIG_VERIFY passed=1 clips=69 bound=65 rigs=23
ue/scripts/run_import.sh out/dec-cutlass_black -nullrhi -SCVerifyRig   # exit 0, RIG_VERIFY passed=1 clips=40 bound=34 lights=225

SC_SETTLE=20 ue/scripts/capture_viewer.sh v1-review-gladius 'VIEWER_LOAD result' \
  -SCDefaults -SCSearch=gladius -SCLoad=AEGS_Gladius                   # wp-v1/review-gladius.jpg
#  VIEWER_PHASE frame=136  t=1.107 Import: Building mesh 15/45 (part 1/14)
#  VIEWER_PHASE frame=352  t=2.222 Import: Building mesh 19/45 (part 1/89)
#  VIEWER_PHASE frame=355  t=2.349 Import: Building mesh 19/45 (part 22/89)
#  VIEWER_PHASE frame=467  t=3.133 Import: Assembling actor
#  VIEWER_LOAD result=Succeeded export_s=0.068 (hit) import_s=3.251 build_s=0.698 total_s=3.507
```

The 89-variant asset that used to be one game-thread step now advances part by part,
with frames rendered between them (frame 352 → 467 over that asset and the rest). The
largest single variant build in this run was 0.032 s (warm DDC; 287 variants). The
tree also contains the lighting agent's uncommitted `SCLightImporter` change; these runs
include it. The first capture attempt showed the ship framed far too close even though
`VIEWER_FOCUS` logged the same 52.6 m fit, and the second was taken while UE showed
"Preparing Textures"; the third (20 s settle) matches the earlier framing. Possible
external input to the shared window is not excluded.
