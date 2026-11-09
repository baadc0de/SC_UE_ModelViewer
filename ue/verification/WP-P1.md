# WP-P1 — first-person exploration verification

2026-11-09. Superproject `main` at `2994531`; submodule remains on
`scviewer/item-types` at `b8fed48d`, with uncommitted changes. UE 5.8.2,
Star Citizen LIVE 4.10.2. All Unreal runs below used **null RHI**. No GPU
acceptance is claimed. No commits, branches, map edits or light importer edits.

## Built

- Runtime `ASCFirstPersonCharacter`: 34/88 cm capsule, 44 cm crouch, 400/650 cm/s
  walk/run, 45 cm step, jump, mouse look, camera torch, noclip, teleport menu,
  stuck prompt, safe spawn checks and fall recovery. Slate HUD and existing app
  buttons, possession, restoration and unload are wired.
- Existing worker-cooked static triangle collision retained. Soft skins get a
  per-instance deformable Chaos BVH, refitted with the evaluated CPU skin pose;
  rigid animated geometry retains its bone-attached triangle bodies. Collision
  readiness includes successful soft-body initialization. No second cache.
- `children[].item_type` comes directly from DataCore AttachDef.Type; absent/null
  is supported by the Rust mirror and UE reader. Cache format 13. Debug FFI
  rebuilt/restaged; ABI unchanged.
- Child/interior Mannequin metadata is scoped with compact stable clip IDs and
  matching fragment references. Interior controllers resolve by entity identity
  or an unambiguous exact DataCore geometry relationship. Animated CGA interiors
  retain NMC nodes without double-baking transforms. Static CGFs keep their old
  path. The Cutlass bulkhead now binds and toggles independently.
- Seat/access/door hints, driven fragment joints, interior placement bounds and
  hull fallback populate vehicle-local spawn points. E selects the nearest
  receiving joint within 3 m, targeting one rig instance. No ship/asset-name
  branches or copied game values.

## Rust and contract gates

Commands run from the repository root, sequentially for Cargo:

```bash
CARGO_TARGET_DIR=./target cargo test --workspace
CARGO_INCREMENTAL=0 CARGO_TARGET_DIR=./target cargo test \
  --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d --lib
scripts/build_ffi.sh
ue/scripts/build.sh SCViewerEditor
```

Results: root suites **34 passed** (5 + 1 + 1 + 2 + 25), zero failures;
upstream 3D **811 passed, 3 ignored**, zero failures. Debug FFI build/header
check/staging succeeded; staged `libscbridge.so` is 127809592 bytes and the symbol
check exposed only `sc_*` functions. Final UE build: `Result: Succeeded` (3.15 s, final display-label cleanup).
The last Rust production change was followed by both suites and FFI staging.

Tests were written/run red before fixes: scene child missing `item_type`;
child fragment animation `open` versus `synthetic_door__open`; soft rigs with
`Expected 'soft geometry has per-poly collision' to be true`. The live
catalogue/export parity test caught null fragment insertion during namespacing.
The added package verifier caught `interior controller lost its receiving
hierarchy` before the CGA fix. All of those checks now pass.

```bash
for pair in AEGS_Gladius:gladius RSI_Scorpius:scorpius DRAK_Cutlass_Black:cutlass; do
  entity=${pair%:*}
  package=${pair#*:}
  target/debug/scviewer --no-cache decomposed "$entity" "out/dec-r5b-$package"
done
python3 -I scripts/verify_wp_p1.py \
  out/dec-r5b-gladius out/dec-r5b-scorpius out/dec-r5b-cutlass \
  --datacore target/debug/starbreaker
```

The independent CLI DataCore query matched **48 / 75 / 98 typed children**;
three untyped attachments per ship serialize null. Output:

```text
P1_CONTRACT root=out/dec-r5b-gladius children=51 types={None: 3, 'SeatAccess': 1, 'Seat': 1, 'SeatDashboard': 1, 'PowerPlant': 1, 'Cooler': 2, 'Radar': 1, 'MainThruster': 2, 'ManneuverThruster': 10, 'QuantumDrive': 1, 'JumpDrive': 1, 'Turret': 3, 'WeaponGun': 3, 'WeaponAttachment': 4, 'MissileLauncher': 4, 'Missile': 6, 'WeaponDefensive': 2, 'Door': 1, 'LifeSupportGenerator': 1, 'Misc': 1, 'DockingCollar': 1, 'Relay': 1} scoped_clips=7 interior_receivers=0 compared=48 live=True
P1_CONTRACT root=out/dec-r5b-scorpius children=78 types={None: 3, 'Misc': 4, 'Cooler': 2, 'WeaponDefensive': 2, 'SeatDashboard': 2, 'MissileLauncher': 4, 'Missile': 16, 'QuantumDrive': 1, 'JumpDrive': 1, 'Radar': 1, 'Seat': 2, 'SeatAccess': 1, 'ManneuverThruster': 10, 'MainThruster': 4, 'Turret': 9, 'WeaponGun': 8, 'LifeSupportGenerator': 1, 'Usable': 1, 'DockingCollar': 1, 'Cargo': 2, 'PowerPlant': 2, 'Relay': 1} scoped_clips=11 interior_receivers=0 compared=75 live=True
P1_CONTRACT root=out/dec-r5b-cutlass children=101 types={None: 3, 'LifeSupportGenerator': 1, 'SeatDashboard': 1, 'Relay': 2, 'Misc': 5, 'SeatAccess': 3, 'Seat': 2, 'TurretBase': 1, 'Turret': 7, 'WeaponGun': 6, 'MainThruster': 4, 'ManneuverThruster': 26, 'WeaponDefensive': 2, 'WeaponAttachment': 8, 'MissileLauncher': 6, 'Missile': 16, 'TractorBeam': 1, 'Door': 3, 'Usable': 4} scoped_clips=28 interior_receivers=6 compared=98 live=True
```

A diagnostic comparison of every transformed position in the old flattened and
new hierarchical bulkhead GLBs gave **maximum bounds delta 0.0 m**. It used an
isolated stdlib Python GLB accessor/matrix walker in `/tmp/p1-bounds.py`, run as:

```bash
python3 -I /tmp/p1-bounds.py /tmp/p1-bulkhead-before.glb \
  out/dec-r5b-cutlass/Data/Objects/Spaceships/Ships/DRAK/Cutlass/DRAK_Door_Bulkhead_LOD1.glb
```

Those before/after artifacts are session scratch, not repository fixtures. The
committed verifier checks the ongoing interior receiver contract.

The broader upstream command was attempted:

```bash
CARGO_INCREMENTAL=0 CARGO_TARGET_DIR=./target cargo test \
  --manifest-path vendor/StarBreaker/Cargo.toml --workspace
```

It stopped before compilation with `failed to unpack package anes v0.1.6`,
`failed to create directory $HOME/.cargo/registry/src/.../anes-0.1.6`,
`Read-only file system (os error 30)`. This is **not a passing workspace gate**.
No release build or `cargo clean` was used. `cargo sweep --maxsize 10GB .` ran
when the shared target exceeded 12 GB; the final sweep removed 3.05 GiB,
leaving `du -sh target` at 11G including retained non-sweep artifacts. Incremental
scratch was removed when present. Both `git diff --check` commands pass.

## Real-package physics acceptance

```bash
for package in gladius scorpius cutlass; do
  "$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
    "$PWD/ue/SCViewer/SCViewer.uproject" \
    -run=pythonscript -script="$PWD/ue/scripts/test_first_person.py" \
    -SCPackage="$PWD/out/dec-r5b-$package" -SCFirstPersonTests \
    -nullrhi -unattended -nop4 -nosplash -ddc=SCViewerDDC \
    -abslog="/tmp/p1-$package.log" > "/tmp/p1-$package-stdout.log" 2>&1
done
```

All exit 0 and end with `WP-P1 HEADLESS_OK`. In ship order:

```text
P1_VERIFY passed=1 soft=3 surface_hits=3 attached_bodies=142 seats=1 spawns=9 doors=3 toggles=3 walk=1 floor_frames=24 hits=5 recovery=1 import_s=2.672
P1_VERIFY passed=1 soft=0 surface_hits=0 attached_bodies=503 seats=2 spawns=11 doors=5 toggles=5 walk=1 floor_frames=24 hits=2 recovery=1 import_s=3.050
P1_VERIFY passed=1 soft=2 surface_hits=2 attached_bodies=299 seats=2 spawns=17 doors=6 toggles=6 walk=1 floor_frames=24 hits=2 recovery=1 import_s=4.365
```

`recovery=1` is the **deliberate** final fall-recovery test; the walk separately
asserts recovery zero. Each run checks authored seat counts, fallback hull hint,
soft surface queries and a synthetic pose change, deployed door surface queries,
Open/Close fragment toggles, unchanged other rig instances, E reach/toggle,
24 real character movement steps with floor contact, crouch, upward jump,
blocked-input prompt, torch and noclip. Scorpius has no soft-skinned triangles
in this package; its 503 rigid animated bodies are the relevant collision path.
Cutlass has six interactions (two side doors, ramp, two lockers, bulkhead).
The short component walk is not a claim of completing the entire manual route.

Cutlass selected `FIRST_PERSON_BEST kind=Entrance
label=Door_RN_RoomConnector_Breachable_OpenReverse_Cutlass_Ramp`. Gladius's first
accepted Door-category hint is near its weapon rack; the T menu also exposes
pilot seat and hull choices for the requested hull/ladder check.

### Collision choice and cost

The test constructs a temporary physics asset with one convex hull per receiving
bone, matches the deployed pose, compares 441 grid traces with the exact bodies,
and destroys it. Child door comparisons include panel and linkages; root
fragments compare their driven subtree. Samples report hit/no-hit disagreement
or surface distance >1 cm, not only body existence:

```text
P1_COLLISION_COMPARE owner=Door_RN_RoomConnector_Breachable_OpenReverse_Cutlass_Ramp joint=Door_Hydrolic_a_top exact_surface=1 bodies=7 convex_cook_s=0.010003 queries_s=0.001365 exact_hits=302 convex_hits=339 differences=260
P1_COLLISION_COMPARE owner=Data/Objects/Spaceships/Ships/DRAK/Cutlass/DRAK_Door_Bulkhead.cga joint=Door_Bulkhead exact_surface=1 bodies=1 convex_cook_s=0.003953 queries_s=0.000392 exact_hits=427 convex_hits=437 differences=205
COLLISION_DERIVE mesh=Data/Objects/Spaceships/Ships/DRAK/Cutlass/DRAK_Cutlass_Door_Ramp_LOD1.glb exact_s=0.001461
```

The ramp's convex hulls bridge concavities/openings and differ on 260/441 samples.
Exact triangles are selected for correctness. The measured ramp exact cook was
1.461 ms; the alternative's setup/cook was 10.003 ms (includes temporary component
setup). Combined grid query time was 1.365 ms. These are diagnostic CPU timings,
not a GPU frame-rate benchmark. Soft-skin triangles additionally pass queries
after a pose translation; each component owns its deformable BVH.

Whole-ship worker collision cook sums in fresh processes: Gladius 0.1006 s,
Scorpius 0.1022 s, Cutlass 0.1760 s. Cutlass repeated in the game host at 0.1664 s.
Collision is recooked each import; the brief's persistent cached-cook target is
assigned to WP-C1 by the addendum. These timings do not claim a disk collision
cache hit, nor a cold OS/DDC run. The CPU cook is well below 20 s.

## Game host and FFI lifecycle

```bash
ue/scripts/run_import.sh out/dec-r5b-cutlass \
  -nullrhi -SCExplore -SCWalkSeconds=2 > /tmp/p1-game-stdout.log 2>&1
```

Exit 0. This automation opens authored door fragments, waits one second to settle,
then commands forward movement. Import/initial-physics stalls are excluded from
the walk timer. It requires displacement >20 cm, floor contact and zero recovery,
then switches Orbit → Fly → First person and unloads. Null-RHI frame counts are
uncapped and timing/displacement vary; this is a functional gate, not a benchmark.

```text
P1_GAME_WALK passed=1 position=X=-137.001 Y=0.089 Z=455.083 distance_cm=1156.81 floor_frames=13084 capsule_hits=2825 recovery=0
P1_GAME_LIFECYCLE passed=1 switched=1 unloaded=1
```

The harness log is `ue/SCViewer/Saved/Import/import.log`. Interactive GPU launches
omit `-SCWalkSeconds`, so they do not auto-open the doors or auto-exit.

```bash
"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/ue_load_gladius.py" \
  -SCAlso=RSI_Scorpius,DRAK_Cutlass_Black -SCCacheDir=/tmp/p1-ffi-cache \
  -nullrhi -unattended -nop4 -nosplash -ddc=SCViewerDDC \
  -abslog=/tmp/p1-ffi-host.log > /tmp/p1-ffi-host-stdout.log 2>&1
```

Exit 0; final staged library used. Fresh isolated cache then reload/reuse:

```text
WP-V1 LOAD entity=AEGS_Gladius result=<SCLoadPhase.SUCCEEDED: 4> reused=False export_s=5.625 cache_hit=False import_s=2.527 build_s=0.542 collision_s=0.101 total_s=8.169 assets=45 previous_objects=0 previous_alive=0 counts={'static_meshes': 317, 'skeletal_meshes': 13, 'textures': 330, 'material_instances': 291, 'vehicles': 1}
WP-V1 LOAD entity=RSI_Scorpius result=<SCLoadPhase.SUCCEEDED: 4> reused=False export_s=4.196 cache_hit=False import_s=3.013 build_s=0.797 collision_s=0.101 total_s=7.268 assets=42 previous_objects=5787 previous_alive=0 counts={'static_meshes': 788, 'skeletal_meshes': 15, 'textures': 307, 'material_instances': 353, 'vehicles': 1}
WP-V1 LOAD entity=DRAK_Cutlass_Black result=<SCLoadPhase.SUCCEEDED: 4> reused=False export_s=6.074 cache_hit=False import_s=3.656 build_s=0.841 collision_s=0.171 total_s=9.808 assets=53 previous_objects=10706 previous_alive=0 counts={'static_meshes': 491, 'skeletal_meshes': 18, 'textures': 468, 'material_instances': 654, 'vehicles': 1}
WP-V1 LOAD entity=AEGS_Gladius result=<SCLoadPhase.SUCCEEDED: 4> reused=False export_s=0.053 cache_hit=True import_s=2.479 build_s=0.526 collision_s=0.099 total_s=2.596 assets=45 previous_objects=9051 previous_alive=0 counts={'static_meshes': 317, 'skeletal_meshes': 13, 'textures': 330, 'material_instances': 291, 'vehicles': 1}
WP-V1 LOAD entity=AEGS_Gladius result=<SCLoadPhase.SUCCEEDED: 4> reused=True export_s=0.000 cache_hit=False import_s=0.000 build_s=0.000 collision_s=0.000 total_s=0.000 assets=0 previous_objects=0 previous_alive=0 counts={'static_meshes': 317, 'skeletal_meshes': 13, 'textures': 330, 'material_instances': 291, 'vehicles': 1}
WP-V1 HEADLESS_LOAD_OK entity=AEGS_Gladius components=567 clips=65
WP-V1 HEADLESS_TEARDOWN_OK alive_after_gc=0
```

## Existing regression checks and remaining failure

```bash
"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/test_importer.py" -nullrhi -unattended -nop4 \
  -nosplash -ddc=SCViewerDDC -abslog=/tmp/p1-regression.log \
  > /tmp/p1-regression-stdout.log 2>&1
```

Exit 0, **16 existing automation suites pass**, `WP-U1 UNIT_TESTS_OK` (package,
mesh/material, rig playback/math/skin and synthetic light checks).

```bash
python3 -I ue/scripts/make_test_tar.py out/dec-r5b-cutlass \
  ue/SCViewer/Saved/Import/p1-cutlass.tar
ue/scripts/run_import.sh out/dec-r5b-cutlass -nullrhi \
  -SCVerifyTar="$PWD/ue/SCViewer/Saved/Import/p1-cutlass.tar" \
  > /tmp/p1-tar-verify-stdout.log 2>&1
```

Tar: 797 files, 202956800 bytes (existing export-root leftovers are unreferenced).
**Exit 1** from the existing light verifier; not a passing aggregate check:

```text
LIGHT_VERIFY passed=0 components=225 enabled=197 disabled=28 projectors=184 gobo_textures=8 gobo_materials=8 fallback_selected=60 shadow_flags=3 visible_shadowed=3 megalights_active=0 atlas_slots=16
TRACE hit_hull=1
LIGHT_VERIFY passed=0 components=225 enabled=197 disabled=28 projectors=184 gobo_textures=8 gobo_materials=8 fallback_selected=60 shadow_flags=3 visible_shadowed=3 megalights_active=0 atlas_slots=16
VERIFY passed=0 folder_assets=53 tar_assets=53 folder_instances=129 tar_instances=129 representations=1 old_live=0 tracked_old=8982 baseline=33 loaded=494 reloaded=494 final=33 first_s=3.5686 second_s=3.5473
```

`SCLightVerification.cpp` still derives radius-quartile shadow expectations
(`fallback_selected=60`), whereas `SCLightImporter.cpp:199` honors authored
`shadow_casting` (`shadow_flags=3`) for the current exporter. The map and light
implementation/verifier were left untouched. Geometry representations, hull hit,
folder/tar counts and reclamation return to baseline in this run. This mismatch
needs a separate lighting verification follow-up; it must not be concealed as a
P1 success or assumed to be fixed simply by using a GPU.

## GPU acceptance — orchestrator to fill

Run from the repository root, one host at a time:

```bash
ue/scripts/run_import.sh out/dec-r5b-cutlass -SCExplore
ue/scripts/run_import.sh out/dec-r5b-gladius -SCExplore
ue/scripts/run_import.sh out/dec-r5b-scorpius -SCExplore
```

For archive-backed UI validation with the staged FFI:

```bash
"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -game -windowed -resx=1600 -resy=900 \
  -nosplash -nop4 -ddc=SCViewerDDC -SCLoad=DRAK_Cutlass_Black \
  -SCCamera=first -SCHold -abslog="$PWD/ue/SCViewer/Saved/Import/p1-gpu-host.log"
```

Click viewport to capture mouse, Esc to release. WASD, Shift, Space, Ctrl,
E, F, V, T; noclip vertical Space/Q. Expected logs: `EXPLORATION ...
collision_ready=1`, `FIRST_PERSON_MODE mode=2`, `FIRST_PERSON_SPAWN`, and
`FIRST_PERSON_DOOR fragment=SCItemDoor ... open=1/0`. Intentional fall recovery
logs `FIRST_PERSON_RECOVER`; ordinary traversal must not produce it.

| GPU row | Exact action after launch | Result / evidence |
| --- | --- | --- |
| Cutlass rear ramp → interior → cockpit | First command above; E opens ramp, walk up, E opens bulkhead, traverse corridor/cockpit and check wall blocking; Ctrl under a low pass | **PENDING orchestrator** |
| Gladius hull/ladder/seat | Second command; T → hull, jump around ladder region, T → pilot seat, crouch eye-height check | **PENDING orchestrator** |
| Scorpius smoke | Third command; door/entry toggle, walk, torch, T/V, mode buttons | **PENDING orchestrator** |
| HUD/input/lighting | Confirm crosshair, prompt/name, relative coordinates, speed, torch, mouse capture/release, teleport menu | **PENDING orchestrator** |
| Frame cost ≤10% | Same ship, settled pose, resolution and comparable view; use `stat unit`/`stat fps`, record repeated Orbit vs First-person samples, including a moving ramp separately | **PENDING orchestrator** |
| Safety/ownership | Noclip then try exit inside a wall (refused); fall/recover; switch modes and unload/reload via UI | **PENDING orchestrator** |

Save screenshots/timing logs outside git if they contain game-derived imagery.
Persistent collision cache acceptance belongs to WP-C1, not this WP's addendum.
