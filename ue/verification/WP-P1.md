# WP-P1 — first-person exploration verification

2026-11-09. Superproject `main` at `2994531`; submodule remains on
`scviewer/item-types` at `b8fed48d`, with uncommitted changes. UE 5.8.2,
Star Citizen LIVE 4.10.2. All Unreal runs below used **null RHI**. No GPU
acceptance is claimed. No commits, branches, map edits or light importer edits.

> **Superseded in part** by [GPU acceptance pass](#gpu-acceptance-pass-orchestrator-2026-11-09)
> below: the first headless pass never ran the skinned-collision refit (it is gated on the
> render mesh object, null under `-nullrhi`), a GPU load crashed in it, and interaction /
> spawn categories changed. Statements below that conflict with that section are history.

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

(First pass, superseded:) Cutlass selected `FIRST_PERSON_BEST kind=Entrance` at the
ramp; Gladius started at its weapon rack because every DataCore `Door` was an
entrance. See the GPU acceptance pass for the current kinds and ranking.

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

## GPU acceptance pass (orchestrator, 2026-11-09)

Superproject `main` at `6887d5b` plus the uncommitted changes listed in the report;
UE 5.8.2 Vulkan, RTX 4090, 1600×900 window; Star Citizen LIVE 4.10.2 through the staged
debug FFI (`-SCDefaults -SCLoad=<entity> -SCCamera=first`). Screenshots are game-derived and
stay in `out/smoke/` (gitignored).

### A. Crash on load — root cause and fix

`FSCMeshImporter::DeriveCollision` built the skinned trimesh without an external vertex
map. Chaos serializes a missing map as an empty array and always loads a **non-null empty**
map (`TriangleMeshImplicitObject.h:806-824`); `UpdateVertices` indexes it whenever it is
non-null → `Array index out of bounds: 0 into an array of size 0`. The engine reaches
`UpdateVertices` only through `UpdateKinematicBonesToAnim`'s per-poly branch, which is gated
on the render `MeshObject` (null under `-nullrhi`), so no headless run ever refit the body
and the claim "soft collision follows the animated pose" was untested.

Fix: the skeletal derive passes `bVertexIndexMap` (identity map over the one-vertex-per-corner
expansion; particle *i* is render vertex *i*). `USCRigComponent::RefitCollision` CPU-skins LOD0
(`CacheRefToLocalMatrices` + `ComputeSkinnedPositions` on the asset's render data and skin
weight buffer, both built with CPU access) and calls `BodyInstance.UpdateTriMeshVertices`; it
runs from `RefreshBoneTransforms`, never before the body exists, only when a bone that weights
a vertex moved (`SKIN_COLLISION ... skin_bones=`), and after `OnCreatePhysicsState` catches up
with the current pose. `OnUpdateTransform` moves the single body like any primitive (the stock
skeletal path also skipped that without a mesh object). One code path with and without a GPU.

Test first: `SCFirstPersonTests.cpp` compares the live trimesh particles with
`GetSkinnedVertexPosition` (bind, after a 137 cm root translation, after reset) and probes
sampled triangles (moved surface must hit, vacated surface must miss), plus the same particle
check for every deployed door. With the fix but the old engine refit call restored, Gladius:

```text
Expected 'pose change refits the body' to be true.
Expected 'refit particles follow the CPU skin (error 137.000 cm)' to be true.
Expected 'moved surface is hit (3/61)' to be true.
Expected 'vacated surface is missed (0/61)' to be true.
P1_VERIFY passed=0 soft=3 refitted=3 ...
```

With the fix (final build), all exit 0 with `WP-P1 HEADLESS_OK`:

```text
P1_VERIFY passed=1 soft=3 refitted=3 surface_hits=3 attached_bodies=142 seats=1 spawns=19 doors=13 toggles=13 walk=1 floor_frames=24 hits=135 recovery=1 import_s=2.536
P1_VERIFY passed=1 soft=0 refitted=0 surface_hits=0 attached_bodies=503 seats=2 spawns=22 doors=16 toggles=16 walk=1 floor_frames=24 hits=11 recovery=1 import_s=3.053
P1_VERIFY passed=1 soft=2 refitted=2 surface_hits=2 attached_bodies=299 seats=2 spawns=24 doors=12 toggles=12 walk=1 floor_frames=24 hits=3 recovery=1 import_s=3.580
```

`test_importer.py`: exit 0, 16/16 suites, `WP-U1 UNIT_TESTS_OK`. Folder game host
(`run_import.sh out/dec-r5b-cutlass -nullrhi -SCExplore -SCWalkSeconds=2`): exit 0,
`P1_GAME_WALK passed=1 ... distance_cm=693.62 ... recovery=0`,
`P1_GAME_LIFECYCLE passed=1 switched=1 unloaded=1 reloaded=1 quit_in_first_person=1`.
On the GPU the same Cutlass load that asserted now loads (`VIEWER_LOAD result=Succeeded`,
no ensure/assert in any log below).

### Review findings

1. Vocabulary removed: an interaction is any fragment with an Open/Close tag pair on a rig the
   clip binds. Every former interaction is kept; additions are authored mechanisms that the word
   list missed: Gladius 3→13 (Cooler L/R, Diagnostic, Fuelport, Inventory, Jumpdrive, Power,
   Radar, Shield panels and **Left_Ladder**), Scorpius 5→16 (Avionics, Battery, Coolers,
   Jumpdrive, **Ladder**, Life support, Power L/R, Radar, Shield generator), Cutlass 6→12 (six
   `SCItemJumpSeat` folding seats). Logged per interaction as `EXPLORATION_INTERACTION`.
   E now acts on the rig under the crosshair (one interaction on that rig: it; several: the one
   whose driven joint carries the hit part, else the nearest joint), else the nearest joint
   within 3 m — door hinges/hydraulics can sit > 3 m from the panel you face.
2. Door items and door interactions are kind `Door`; `Entrance` is reserved for data-identified
   hull entrances (none exported yet). Start ranking `Entrance > SeatAccess > Seat > Interior >
   Door > Hull`. Item spawn hints are read after assembly from the bottom of the item's own
   collidable geometry (ports at the vehicle origin carry their offset in the mesh); an item
   at the origin without geometry has no location data and is skipped
   (`EXPLORATION_SPAWN_UNLOCATED`: the Gladius and Scorpius SeatAccess). Seat hints resolve a
   crouched (sitting) capsule within two capsule diameters, never the floor under the ship.
   Starts: Cutlass `SeatAccess` (manned turret access), Gladius `Seat` (pilot), Scorpius
   `Interior` (both seats reject a capsule). Tested: Door never outranks seat hints.
   **Amended 2026-11-10 (seat reach):** a free spot within two diameters is not enough — the
   capsule must touch the seat. Rule, tests and results: "Seat reach" below. Gladius now
   starts at the next kind that fits instead of `Seat`.
4. `SSCViewportInput` resets drag state in `OnMouseCaptureLost` and `OnFocusLost`.
5. `UnloadInternal` only nulls the character/pawn pointers and sets the mode when the world is
   tearing down or exit was requested; the lifecycle check now reloads, re-enters first person and
   quits from there (null RHI above; GPU: Cutlass route ends with `quit` in first person, exit 0).
7. "Spawn where I look" uses kind `Selected` (never ranked), replaces the previous one; the
   teleport menu rebuilds on `ASCVehicle::SpawnRevision`.
Notes: the noclip refusal stays on the HUD for 3 s (tested); door state is keyed by (rig
instance, fragment) from the tag actually played, the untargeted orbit toggle keeps its own key
(tested: one door leaves other doors and the orbit state unchanged). Noclip now brakes
(`NoclipBraking`), it drifted indefinitely before. Leaving first person for Orbit keeps the
viewpoint (pivot = what you looked at); it previously eased out to the old frame-all distance,
outside the hangar (black frame).

### GPU routes

```bash
ue/scripts/capture_route.sh p1-cutlass ue/scripts/routes/cutlass-ramp-to-cockpit.txt -SCDefaults -SCLoad=DRAK_Cutlass_Black -SCCamera=first
ue/scripts/capture_route.sh p1-gladius ue/scripts/routes/gladius-hull-seat.txt -SCDefaults -SCLoad=AEGS_Gladius -SCCamera=first
ue/scripts/capture_route.sh p1-scorpius ue/scripts/routes/scorpius-smoke.txt -SCDefaults -SCLoad=RSI_Scorpius -SCCamera=first
```

Frame cost: `frametime 6 <tag>` samples every frame for 6 s (`FApp::GetDeltaTime`,
`GGameThreadTime`, `GRenderThreadTime`, `RHIGetGPUFrameCycles` — the `stat unit` sources),
`r.VSync 0`, `t.MaxFPS 0`, Cutlass at the first-person start, same eye position and view for
all three samples (Orbit takes over the eye):

```text
SCRIPT_FRAMETIME tag=first-person-a  frames=1009 frame_ms[mean=5.94 p50=5.64 p95=11.91] game_ms[mean=1.71 p50=1.70 p95=2.19] render_ms[mean=3.65 p50=3.26 p95=9.57] gpu_ms[mean=5.34 p50=5.44 p95=5.62]
SCRIPT_FRAMETIME tag=orbit-same-view frames=1004 frame_ms[mean=5.98 p50=5.69 p95=11.96] game_ms[mean=1.52 p50=1.52 p95=2.05] render_ms[mean=3.64 p50=3.23 p95=9.68] gpu_ms[mean=5.39 p50=5.51 p95=5.68]
SCRIPT_FRAMETIME tag=first-person-b  frames=1000 frame_ms[mean=6.01 p50=5.60 p95=12.06] game_ms[mean=1.73 p50=1.73 p95=2.18] render_ms[mean=3.78 p50=3.27 p95=9.81] gpu_ms[mean=5.32 p50=5.44 p95=5.59]
```

| GPU row | Result / evidence |
| --- | --- |
| Cutlass ramp → bay → bulkhead → corridor → cockpit | **PASS with one deviation.** Start `Door:2` (ramp item) resolves to the hangar floor under the closed ramp; E on the ramp (crosshair) opens it (`FIRST_PERSON_DOOR ... Ramp open=1`), `01-ramp-closed-from-outside`, `02-ramp-open-overhead`. The open ramp's lip hangs ~1.5 m above the hangar floor (viewer places the model 1 m above the floor): not boardable from the floor (above jump height) — crouch-walk out under it (low pass, `crouched=1`, 7.4 m) `03-ramp-open-outside`; the route then spawns on the bay floor (`Interior:1`), walks down to the ramp foot and **up the ramp** (`04-ramp-foot-looking-up`, Z −441→−242), `05-cargo-bay`; closed bulkhead blocks at X=76.7 `06-bulkhead-closed-blocks` (prompt "E: Open drak door bulkhead"), E opens it `07-bulkhead-open`, `08-corridor`; wall blocks (two moves end at X=769.0 / 769.0) `09-wall-blocks`; cockpit reached via the step-up to Z −154 `10-cockpit`, `11-cockpit-crouched`, `12-cockpit-looking-aft`. No `FIRST_PERSON_RECOVER` during traversal. Canopy glass renders black from inside (material, not P1). Files `out/smoke/p1-cutlass-01…12-*.png`. |
| Gladius hull / jump / pilot seat / canopy / ladder | **PARTIAL.** Hull spawn `p1-gladius-02-hull.png` (standing on the spine, prompt shown), jump Z 182.9 → 266.0 → 182.9 (`FP_WHERE`), walk along the hull 3.7 m `03-hull-after-walk`. Pilot-seat spawn is accepted crouched (`crouched=1`, eye at crouch height) but 136 cm aft of the seat (ring search); `04-pilot-seat`/`05`/`06` are **black frames** (camera in the enclosed space behind the seat) — sit-height view **not demonstrated, FAIL**. Canopy toggled by E from that spot (`FIRST_PERSON_DOOR fragment=Canopy open=1`) but not visible. Ladder: E from the floor beside it picked the nearest joint (Entrance panel, crosshair missed the hull); the `Left_Ladder` fragment toggled by index (`sc.FP.Door 8`) `08-ladder-fragment-toggled.png` (hull underside visible, ladder motion not identifiable in the still). |
| Scorpius smoke | **PASS.** Start `Interior` on the hull (both seats reject a capsule, SeatAccess unlocated) `p1-scorpius-01-start.png`; E toggles the boarding `Ladder` fragment from the canopy spawn `02-entry-toggled.png` (prompt then offers "Open Cockpit Sliding Panel"); hull spawn, 1 s walk, torch on `03-hull-walk-torch.png` (HUD "torch on"; the cone is not visible in the lit hangar), off `04-torch-off.png`. |
| HUD | **PASS.** Crosshair, prompt with interaction name and Open/Close, mode (Walking/Crouched/Noclip), m/s, torch state, vehicle-relative cm, status line, key help — readable on a dark backing (was white-on-white before this pass) — every `p1-*` FP shot; teleport menu with index · kind · label `p1-cutlass-16-torch-and-teleport-menu.png`. The engine's red sky-light warning line stays on screen (WP-V1 TODO). |
| Frame cost ≤10 % | **PASS.** Same view, frame mean FP 5.94 / 6.01 ms vs Orbit 5.98 ms (−0.1 %); GPU 5.34 / 5.32 vs 5.39 ms; game thread 1.71 / 1.73 vs 1.52 ms (+0.2 ms, +13 %, not on the critical path). |
| Safety | **PASS.** `sc.FP.Embed` into the copilot seat, exit refused `FIRST_PERSON_NOCLIP refused=1`, message visible `p1-cutlass-14-noclip-exit-refused.png`; backed out, exit accepted; noclip 19 m down through the floor, walk mode → `FIRST_PERSON_RECOVER count=1` back at the last spawn `15-recovered`; Fly → Orbit → First person, `sc.Unload`, `sc.Load DRAK_Cutlass_Black`, First person `17-reloaded-first-person`, `quit` in first person → **exit 0**, log ends `Log file closed`, no ensure/assert. |

### Follow-ups

- **Hull entrance (exporter, cargo lock):** export per child `NavigationLinkComponentParams.navLinks[].linkingType`
  (or a boolean `exterior_link` = any link `LinksToExternalZone`/`LinksFromExternalZone`). Read-only
  `target/debug/starbreaker dcb query EntityClassDefinition --filter '*.<name>'`: present on
  `Door_RN_RoomConnector_Breachable_OpenReverse_Cutlass_Ramp` and `..._Cutlass` (side doors),
  absent on `Weapon_Rack_AEGS_Gladius` (also `AttachDef.Type = Door`). UE then emits kind
  `Entrance` for those doors (ranked first).
- **Seat access location:** Gladius/Scorpius SeatAccess ports sit at the vehicle origin and the
  items have no geometry; the location lives in interaction-point helpers
  (`$IP_SeatAccess_Ground_Pilot` in `SInteractionPointParams`). Export that helper position.
- **Pilot vs other seats:** no direct flag; candidates are `SCItemSeatParams.dashboardScopeContext`
  (`SeatDashboard` pilot vs `SeatUser` gunner on the Scorpius) or the vehicle's controller
  assignment. Until then seats are ordered by height.
- **Sitting:** seat buckets do not fit a 34 cm capsule; a seated state (camera at the seat's
  sit helper, collision off until the player stands) is needed for the sit-height check. Gladius
  row above fails on this. Since the seat-reach rule (below) the Gladius and Scorpius seat spawns
  are refused outright rather than placed beside the seat, so no pilot's-eye view exists until this
  state does. Data for it: `SCItemSeatParams.actorAttachment` (bone `Sit_Pos_Pilot`, stance `Seated`).
- **Floor clearance:** the viewer places the lowest point 1 m above the hangar floor, so
  ramps/ladders do not reach the ground (Cutlass ramp lip 1.5 m up). Rest the ship on its
  deployed gear for first person (WP-V1 presentation constant).
- **Interior spawn hints** from containers without a floor (e.g. Gladius `base_lightgroups`)
  resolve to the hangar floor under the ship.

## Seat reach and the Gladius cockpit (2026-11-10)

Part of the cockpit-glass change (`WP-U3b.md` §14). Defect: the Gladius pilot-seat start was accepted
crouched 136 cm aft of the seat, inside the closed bay behind the seat back, and every frame from there
was black (the **FAIL** of the Gladius row above).

Rule. A seat spawn carries the box of the seat item's collidable primitives in the vehicle's frame
(`FSCSpawnPoint::Bounds`, computed with `CalcBounds` in actor space, so it does not grow when the vehicle is
rotated). `ASCFirstPersonCharacter::FindCapsuleSpot` accepts a seated candidate only when the distance from
the capsule's axis segment (centre ± (half height − radius) along Z) to that box, measured in the vehicle's
rigid frame (rotation and translation; the actor's scale is folded into the box), is at most
`Radius + Skin + 0.1` cm — the capsule touches the seat. A seat known only by its hint, or whose box is
invalid, is a zero-size box at the hint. No reachable free spot → `FIRST_PERSON_SPAWN_REJECT … kind=Seat …
reason=no_clear_capsule`, and the start falls to the next kind in `Entrance > SeatAccess > Seat > Interior >
Door > Hull` (ranking unchanged). The capsule numbers are the character's own profile (radius 34, crouched
half height 44, skin 2.5); nothing is ship-specific.

Tests (written first): `FSCFirstPersonSeatReach` — a seat with room accepts; a seat whose only free spot is
behind the back refuses; hint-only and invalid boxes; a vehicle turned 45° (a spot past the seat's corner is
refused although a world-aligned box would contain it); a vehicle turned 45° and stretched 2× along X (a
capsule one diameter out along the vehicle's X sits). `FSCFirstPersonSpawns`: a refused seat never hides a
lower-ranked spawn that fits.

```
$ … test_first_person.py -SCPackage=out/dec-r5b-gladius -SCFirstPersonTests
  FIRST_PERSON_SPAWN_REJECT index=0 kind=Seat pos=V(X=824.86, Y=0.01, Z=-85.50) label="AEGS_Gladius_SCItem_Seat_Pilot" reason=no_clear_capsule
  FIRST_PERSON_BEST kind=Door label=pinventory interiordoor
  WP-P1 HEADLESS_OK
$ … -SCPackage=out/dec-r5b-scorpius …
  FIRST_PERSON_SPAWN_REJECT … label="RSI_Scorpius_SCItem_Seat_Gunner" …   (and …_Seat_Pilot)
  FIRST_PERSON_BEST kind=Interior label=ext_lightning_main
  WP-P1 HEADLESS_OK
$ … -SCPackage=out/dec-r5b-cutlass …
  FIRST_PERSON_BEST kind=SeatAccess label=DRAK_Cutlass_SeatAccess_MannedTurret
  WP-P1 HEADLESS_OK
```

GPU (`routes/gladius-hull-seat.txt`, RTX 4090): the viewer's own load (interior containers present) starts
at `Interior` `base_lightgroups`, which resolves to the hangar floor beside the ship (the "Interior spawn
hints" follow-up above; the headless test world has no hangar floor, so that hint is rejected there and
the `Door` is picked); `spawn Seat` is
refused and the player stays where they were (`04-seat-spawn-refused`); a noclip camera at the pilot's eye
point shows the cockpit and the hangar through the canopy (`05-cockpit-eye`); E on the canopy from the hull
toggles it (`06`). Evidence: `wp-u3b/glass-gladius-seat-before-after.jpg`. The Gladius row's sit-height
check stays **not demonstrated** — it needs the seated state.

### Fixed: Open / Close clips ended in each other's pose (exporter, 2026-11-10)

Seen on the Gladius canopy: "Open canopy" closed it. The first reading recorded here — that
`animation/dba.rs` pairs DBA metadata entries with key blocks in the wrong order — was wrong. Entry `i`
of a `.dba` metadata table does own key block `i`: in all 548 `.dba` files of 4.10.2 (15 910 blocks) the
entry's controller count equals the block's and its end frame equals the block's last key. The clips were
wrong one step earlier, where a `.chrparams` event is paired with a database entry.

**Rule (from the archive: 4151 `.chrparams`, 548 `.dba`).** An event plays the CAF its resolved path
names: the tracks-database entry of that name, else the loose `.caf` at that path. Nothing else decides.
`#filepath` is a running directive — an event resolves under the one in effect where it is declared,
directive names compare case-insensitively, and the prefix is always joined. Events whose resolved path is
an entry of their own tracks database: 3821 under the old reading, 4221 of 4223 under this one.

**Causes** (vendored submodule, branch `scviewer/clip-pairing`, commits `01b9410c`, `9e216d33`):

1. `chrparams.rs` kept one global `#filepath` (the last) and matched directive names case-sensitively. A
   file declaring several sent every event to the last directory; no path matched.
2. `animation/matching.rs` then fell back to name-similarity, bone-subset and animevents heuristics, which
   also overrode valid path matches. Removed (`matching_scores.rs`, `matching_diagnostics.rs` deleted):
   pairing is by path only, and an event whose CAF is stored nowhere gets no clip.
3. `rig_sources.rs` gave a clip merged by name across rigs the first contributor's `frame_count`, cutting
   longer contributors short. It now runs as long as the longest.
4. The default-pose bake took the first block whose bones fit the skeleton; it now resolves its event by
   the same rule (`pipeline/default_pose.rs`).

Causes 1–3 and the review fixes each have a test that failed first (`chrparams.rs`,
`animation/matching_tests.rs`, `rig_sources.rs`, `animation/catalogue_tests.rs`, `animation/dba.rs`,
`animation/extraction.rs`, `pipeline/clip_catalogue.rs`; synthetic fixture
`tests/fixtures/clip_catalogue/rigs.zip`). Cause 4 has no failing-first test of its own: it is covered
by the shared resolver's tests (`clips_for_caf_path`) and by
`default_pose.rs::default_pose_comes_from_a_loose_caf_when_the_event_names_one`. Layer-1 `CACHE_FORMAT_VERSION` is 17.

**Cross-review (Codex) of `01b9410c`:** no blockers, eight findings. Fixed in `9e216d33`: `#filepath` and
path join with exactly one separator; a key block that fails to decode stays in the block list as a gap so
later blocks keep their metadata entry; the default-pose bake reads loose CAFs and rigs without a tracks
database; a loose CAF that is stored but unreadable is a warning, not silence; a skeleton's `dba_path`
comes from any database-backed clip. Recorded as gaps, not worked around (submodule
`docs/decomposed-export-contract.md`, "Known gaps of the pairing"): a merged clip has one length, `fps`,
anchor set and start pose (shorter contributors hold their last key; played backwards they start late —
7 frames on the Gladius gear); a loose CAF with several key blocks merges under one name; seven
`.chrparams` declare names differing only in case and 45 repeat a name (first wins); `Animations.img` is
not parsed (events whose CAF is in neither their own database nor a loose file get no clip); only the
last `$TracksDatabase` of a file is read; wildcard rules are not expanded.

**Verification** (exporter `9e216d332ced42806d7e0fe39d444e7be5035bfb:dirty=false`):

```
$ cd vendor/StarBreaker && CARGO_TARGET_DIR=$PWD/../../target cargo test -p starbreaker-3d --lib
  test result: ok. 873 passed; 0 failed; 3 ignored
$ target/debug/scviewer --no-cache decomposed AEGS_Gladius out/dec-r5b-gladius        (and Scorpius, Cutlass)
$ target/debug/starbreaker p4k extract --filter '**/*.chrparams' -o <extract>          (and '**/*.dba')
$ python3 -I scripts/verify_clip_pairing.py --extract <extract> out/dec-r5b-gladius out/dec-r5b-scorpius out/dec-r5b-cutlass
  out/dec-r5b-gladius rigs 13 event=block 58 wrong 0 loose-caf events 2 events without clip 0
  out/dec-r5b-scorpius rigs 14 event=block 55 wrong 0 loose-caf events 2 events without clip 0
  out/dec-r5b-cutlass rigs 18 event=block 42 wrong 0 loose-caf events 5 events without clip 0
```

The same check on the packages exported before the fix exits 1:

| Package | before | after cause 1 only | final |
| --- | --- | --- | --- |
| Gladius | 37 right, 21 wrong | 58, 0 | 58, 0 |
| Scorpius | 54, 1 | 54, 1 | 55, 0 |
| Cutlass Black | 34, 8 | 36, 6 | 42, 0 |

Clip sets, before → after: Gladius 65 → 64, Scorpius 75 → 75, Cutlass 54 → 55; no event lost its clip.
Gladius `canopy_open` 125 → 260 frames, `canopy_close` 260 → 125, `landing_gear_extend` 75 → 167.
`scripts/verify_wp_p1.py` on the three packages with `--datacore`: exit 0. Headless, each exit 0:
`test_importer.py` (`WP-U1 UNIT_TESTS_OK`), `test_first_person.py` on each package (`WP-P1 HEADLESS_OK`
×3), `test_player.py` (`WP-P2 HEADLESS_OK`).

**GPU (RTX 4090, Vulkan), final library** — `libscbridge.so` built from
`9e216d332ced42806d7e0fe39d444e7be5035bfb:dirty=false` (the identity string in the binary; the ThirdParty
copy is byte-identical to `target/debug/libscbridge.so`), each ship re-processed (`FFI_EXPORT …
cache_hit=0`), all three routes `EXIT=0`:

- **Gladius** (`cp2-gladius`, canopy fragment toggled twice by E): at rest the canopy is closed and the
  prompt reads "E: Open canopy"; `FIRST_PERSON_DOOR fragment=Canopy … open=1` raises it and the prompt
  becomes "E: Close canopy"; the second E (`open=0`) closes it again. Before the fix the first E closed an
  already-raised canopy. Evidence: `wp-u3b/clip-pairing-gladius-canopy.jpg` (rest / after first E / after
  second E).
- **Cutlass Black** (`cutlass-ramp-to-cockpit.txt`): the ramp prompt reads "Open", E logs
  `FIRST_PERSON_DOOR fragment=SCItemDoor owner=Door_RN_RoomConnector_Breachable_OpenReverse_Cutlass_Ramp
  joint=Door_Hydrolic_a_top open=1` and the ramp is down with the bay visible; the closed bulkhead blocks
  until `Door_Bulkhead open=1`; the cockpit is reached. Frame time: first-person 8.19 ms / 8.01 ms, orbit
  at the same view 6.33 ms.
- **Scorpius** (`scorpius-smoke.txt`): `FIRST_PERSON_DOOR fragment=Ladder … joint=Ladder_Top open=1`, the
  hull walk completes.

No "loose CAF", "Failed to parse animation block" or "no default pose" warning in any of the three logs.
Not looked at on the GPU: the landing-gear motion (its clip length changed 75 → 167 frames on the
Gladius), the other Gladius panels whose pairing changed, and the Scorpius ladder's end pose (only the
toggle log line was checked).

Not verified: the default-pose bake on real data (the viewer's "Default pose" option is off by default;
covered by the synthetic fixture only); loose CAFs are not compared by `verify_clip_pairing.py`; ships
other than these three.

### Fixed: a merged clip timed every rig by one length (exporter + importer, 2026-11-10)

The gap the previous section left: a clip merged by name across rigs had one `fps`, one `frame_count`
(the longest contributor's) and one start pose, so every track ran on the clip's clock. The game plays
each entity's own animation of that name, and a Mannequin fragment with negative speed starts each one at
its own end. Played backwards, a shorter contributor therefore held its end pose until the clip clock
reached its length: on the Gladius `Landing_Gear` / `Retract` (`landing_gear_extend` at speed −1) the hull
animation is 167 frames and the three gear rigs' 160, so the gear started 7 frames (0.23 s) after the hull
doors and finished with them instead of 7 frames before.

**Measured on the three packages** (clips whose contributors differ in length; all are 30 fps, and no
merged clip has contributors with different start poses that would change a pose — only lengths differ):

| Package | merged clips | contributors differ in length |
| --- | --- | --- |
| Gladius (64 clips) | 7 | `landing_gear_extend` 167 (hull 167, three gear rigs 160), `landing_gear_retract` 160 (hull 140, gears 160), `open` 67 / `close` 69 (power plant 67 / 69, jump module 40) |
| Scorpius (75) | 6 | `dashboard_deploy` 77 (gunner 45, pilot 77), `dashboard_retract` 77 (gunner 50, pilot 77), `open` 70 / `close` 70 (jump module 40, fuel port 70, power plant 67 / 69) |
| Cutlass Black (55) | 4 | none |

**Fix.** Exporter (submodule branch `scviewer/clip-sources`, `e95e6ca5` + `dfee4503`): a clip lists each distinct
contributing animation in `sources` (`skeleton_path`, `fps`, `frame_count`, start pose) and every channel
or channel variant names its own by `source`; a bone's entry stays one channel or a flat list of variants when a contributor
brings a list of its own; the clip-level fields are unchanged. Importer: a track takes
its rate, length and start-pose anchor from its source (`FSCBoneTrack::Fps`; a hard import error when `sources` is not a list, or a channel of a clip with
`sources` names none, a fraction, text, or a record without a usable rate and length; also when a rate is not a whole number of
frames a second, a channel has keys past its animation's length, or an animation is longer than its clip), and `FSCAnimImporter::TrackFrame` maps the clip frame to
the track's own frame — forward from the common start, backwards from the track's own end — in
`ASCVehicle::EvaluateClips` and the rig verification mirror. A `UAnimSequence` has one timeline, so a
binding holds one per source timing (rate and length) among its tracks (`FSCClipBinding::Sequences`),
each with that animation's keys verbatim; nothing is resampled.
Layer-1 `CACHE_FORMAT_VERSION` 17 → 18. No asset name, bone name or length threshold takes part.

**Tests first.** Rust (`animation/rig_sources.rs` and `decomposed.rs`, 6 tests: a merged clip records each
contributing animation, one animation on two entities is one source, the merged length is the longest
duration at the clip rate, every channel variant names its source, an incoming variant list joins flat —
at the merge function and through a rig that brings one):
`CARGO_TARGET_DIR=$PWD/target cargo test --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d --lib`
→ 879 passed, 0 failed, 3 ignored. UE (`SCRigSourceTimingTests.cpp`): `FSCRigSourceTiming` (import: own
length, own rate, own anchor, error on a missing source) and `FSCRigSourcePlayback` (a 6-frame and a
4-frame rig in one clip: forward both start at once, backwards both start at once and the short one ends
first). The UE tests could not be run before the API they call existed; they were shown to fail by
mutation instead — with `EvaluateClips` back on the clip frame and `DeriveBinding` ignoring `sources`,
both report `passed=0` ("shorter animation keeps its own length" expected 5, was 7; "backward: short
moves at once" expected 30, was 40; "backward starts at own end" expected 4, was 6); restored, both pass.

**Validation on re-exported packages** (`out/dec-r7-*`, `target/debug/scviewer --no-cache decomposed
<entity> <dir>`, exporter `dfee4503`; the clips are identical to the `e95e6ca5` export's — 64 / 75 / 55
clips, same names, 0 bodies differing). `scripts/verify_clip_pairing.py` makes two checks. *Structure*
(no game files needed): `sources` is a list, every channel names a whole listed index, no source is
unused, variant lists are flat, the channels naming a source end on that source's `frame_count`, the
clip's `fps` and start pose are its first source's, and its `frame_count` is the longest source's
duration in clip frames, rounded up. *Pairing* (against the extracted `.chrparams` / `.dba`): the bones
of a rig's event are its own database block's controllers, each with the block's rotation and position
key counts and last key time, and all name one
source whose rig, rate, length and start pose are the block's — the start pose read from the block's
metadata by the script itself, not taken from the export; a clip exported under a database entry's own
path is compared as a whole with that entry (`path=block`). Any `wrong`, `timing wrong`,
`structure wrong` or `missing evidence` (a rig whose `.chrparams` is not in the extract, a tracks database
a `.chrparams` names that is absent or unreadable, a clip of a rig with a database that no entry was
compared with) exits 1. Key values, and the times of the keys before a controller's last, are not
compared with the database:

```
$ python3 -I scripts/verify_clip_pairing.py --chrparams-root <chrparams> --dba-root <dba> out/dec-r7-gladius out/dec-r7-scorpius out/dec-r7-cutlass
out/dec-r7-gladius rigs 13 event=block 58 path=block 16 wrong 0 timing wrong 0 structure wrong 0 loose-caf events 0 events without clip 0 missing evidence 0 unjudged: rigs without database 1 rig-clip pairs 2 of 76
out/dec-r7-scorpius rigs 14 event=block 55 path=block 28 wrong 0 timing wrong 0 structure wrong 0 loose-caf events 0 events without clip 0 missing evidence 0 unjudged: rigs without database 1 rig-clip pairs 2 of 87
out/dec-r7-cutlass rigs 18 event=block 42 path=block 14 wrong 0 timing wrong 0 structure wrong 0 loose-caf events 0 events without clip 0 missing evidence 0 unjudged: rigs without database 1 rig-clip pairs 5 of 61
```

Unjudged, and listed by the script: the rigs whose `.chrparams` names no tracks database, so their events
are loose `.caf` files with no block to compare against — `jmod_tars_s01_pl02.cga` `open` / `close`
(Gladius, Scorpius) and `grin_tractor_beam_s1.cga` `power_down` / `power_up` / `running` / `spin_down` /
`spin_up` (Cutlass). Their structure is checked; their pairing and timing are not.

The validator fails when the package is broken (copies of the Gladius package with `landing_gear_extend`
edited, each exit 1): clip `frame_count` 167 → 160 — `STRUCTURE … 'clip is 160 frames long, its longest
source 167'`; one gear source's start rotation replaced — `TIMING (aegs_gladius_landinggear_backright_chr.chr,
landing_gear_extend) … start rotation [0.5, 0.5, 0.5, 0.5], block (1.0, -0.0, 0.0, -0.0)`; two sources
swapped — `STRUCTURE … 'source 0 is 167 frames long, its channels end on 160.0'` and four `TIMING`
(rig, length); a variant list nested — `STRUCTURE … '0x11ED938E: a variant list inside a variant list'`;
one middle key of `canopy_close` removed (same controllers, same last key) — `WRONG (aegs_gladius.cga,
canopy_close) … 'keys differ on 1 controllers'`; the owner rig of a path-named clip renamed —
`no chrparams … aegs_gladius_renamed.cga`, `MISSING EVIDENCE (aegs_gladius_refueling_port.cga,
…/air_fuel_intake_close.caf)`, `missing evidence 2`. A copy with the time arrays that equal the sample
indices left out (the contract's implicit times) passes, exit 0.
The package of the previous section (no `sources`) fails it too: `out/dec-r5b-gladius … event=block 0 …
timing wrong 58 structure wrong 64 … missing evidence 16`, exit 1.

`scripts/verify_wp_p1.py --datacore`: exit 0 ×3. Headless on `out/dec-r7-*`, each exit 0 (run again after
the second review pass's changes, same results):
`test_importer.py` (31 suites passed, 0 failed), `test_first_person.py` on each package
(`WP-P1 HEADLESS_OK`), `test_player.py` (`WP-P2 HEADLESS_OK`), `test_rig.py` on each package
(`WP-U3 RIG_TESTS_OK`; `RIG_VERIFY passed=1 clips=64 bound=63 sequences=80 tracks=636 rigs=16`,
`clips=75 bound=71 sequences=90 tracks=675 rigs=23`, `clips=55 bound=53 sequences=68 tracks=371 rigs=32`
— 80 / 90 against 78 / 88 before the per-timing sequences: two more on each of the two ships, where a
binding's tracks have more than one timing).

**Cross-review (Codex, on the first version of this change).** Eight findings; five fixed, tests first:

- *The sequence resampled every track on the clip's timeline* (P1) — fixed: one sequence per source timing,
  keys verbatim. Red before the fix: `'one sequence per source timing' to be 4, but it was 1`,
  `'Short: sequence of the animation's length' to be 5, but it was 7`, `'Fast: sequence at the animation's
  rate' to be 60, but it was 30`; green after.
- *A source index was rounded, a `sources` that is not a list was read as absent, a missing rate fell back
  to the clip's* — fixed: all hard import errors. Red: `'fractional source index'`, `'negative source
  index'`, `'source index as text'`, `'source list that is not an array'` expected true; green after.
- *A contributor that brings a variant list nested it inside the merged list* — fixed in the exporter
  (`dfee4503`), two Rust tests red then green. No package of the three contains such a list, so their output
  did not change.
- *The validator took the start pose from the export it was checking, did not check structure, skipped
  path-named clips and counted unjudged rigs as passes* — fixed as described above.
- *Contract wording on when two source records are one* — fixed (`decomposed-export-contract.md`).

Measured, not changed: `bCyclic` is decided for the clip as a whole from its tracks' endpoints; decided
per source instead it differs for 0 clips of the three packages. It remains an endpoint-equality
heuristic (the game's loop flag is not read) and is flagged as such. Flagged, not fixed: reversing a
clip in mid-play restarts it from the opposite end rather than from the current pose (present before
this change); the Blender addon does not read `sources`.

**Second review pass (Codex, over the review fixes).** Five findings, all P2; it reported no defect in
the strict source-index parsing, the sequence grouping, `TrackFrame`, the exporter's flattening and
source re-indexing, `SCKeepAssets.cpp`, or the validator's database offsets and axis conversion, and did
not examine live playback or saved-asset reload.

- *The importer accepted a rate too small for the track's float (it became 0 and reached `TrackFrame`'s
  check) and a fractional rate (the sequence's frame rate is whole)* — fixed: a rate is a whole number of
  frames a second, at least 1, as the contract says.
- *Inconsistent lengths were accepted: a channel with keys past its animation's `frame_count`, an animation
  longer than its clip* — fixed: hard import errors. Red before the fix, seven expectations of
  `FSCRigSourceTiming` (`'source rate too small to store'`, `'fractional source rate'`, `'source shorter than
  its channel'`, `'source shorter than its channel's times'`, `'clip shorter than its source'`, `'clip
  shorter than its faster source'`, `'clip shorter than its channel'` expected true); green after. The
  check also caught a test of ours that bound a three-key channel under a one-frame clip
  (`FSCRigMath`); the test now uses a one-key clip and asserts the three-key one is rejected. No clip of
  the three packages or the player kit is rejected (suites above).
- *The validator read an omitted time array as time 0 and so rejected the contract's implicit times* —
  fixed (last key = sample count − 1 per component); it also checks that an explicit time array is as
  long as its keys. Before: the implicit-times copy failed with `'source 0 is 8 frames long, its channels
  end on 0'`; after: exit 0.
- *The validator exited 0 when evidence was missing* — fixed: `missing evidence` exits 1 (before: the
  renamed-owner copy exit 0, after: exit 1). Rigs whose `.chrparams` names no tracks database stay
  reported as unjudged; the set of rigs is still taken from the clips, not from the scene's rig inventory.
- *The validator certified a pairing by the controller set and one last key time* — narrowed, not closed:
  it now compares every controller's rotation and position key counts and last key time, per variant
  (before: the removed-key copy exit 0, after: exit 1; the three packages match the database on every
  controller). It still does not compare key values or the earlier key times: a clip with the right
  shape and other values would pass.

A `USCClipCatalog` kept as assets before this change has no per-track rate and no per-timing sequences;
keep the vehicle again. The app itself never plays a kept catalog: every load builds the catalog from
the export's clip files (`SCImportJob.cpp`).

**GPU (RTX 4090, Vulkan).** New console command `sc.Fragment <fragment> [tag]`; route
`ue/scripts/routes/gladius-gear-retract.txt` plays `Landing_Gear` / `Retract` at `slomo 0.05` (one
animation frame = 0.67 s) from the front-left quarter, once on the new package and once on the previous
one — the previous package has no `sources`, so the same build times it the old way:

```
ue/scripts/capture_route.sh lg-r6  ue/scripts/routes/gladius-gear-retract.txt -SCPackage="$PWD/out/dec-r6-gladius"  -SCExplore
ue/scripts/capture_route.sh lg-r5b ue/scripts/routes/gladius-gear-retract.txt -SCPackage="$PWD/out/dec-r5b-gladius" -SCExplore
```

Both log `FRAGMENT_CMD fragment=Landing_Gear tag=Retract ok=1`. Frames after the fragment start, from
the log and screenshot times (new / old): 6.6 / 6.0, 40.9 / 41.1, 163.5 / 164.2. Share of pixels in the
gear region differing by more than 40/255 between the two runs: gear down (before the fragment) 0.00 %,
start 0.22 %, mid 1.72 %, near end 0.00 %, retracted 0.00 %. At the start shot the new package's gear
has left its end pose and the old one's has not (difference image: gear outlines only); at the mid shot
the new gear is visibly further folded. Evidence: `wp-u3b/gear-retract-sources.jpg` (rows: start, mid —
new left, old right; bottom: rear gear at the start shot, new / old / difference ×4).

After the review fixes the route was run again on the re-exported package
(`capture_route.sh lg-r7 … -SCPackage="$PWD/out/dec-r7-gladius" -SCExplore`, `FRAGMENT_CMD … ok=1`, exit 0)
and compared with the `lg-r6` shots over the whole frame: 0.07 – 0.08 % of pixels differ by more than
40/255 in every shot, the one before the fragment included — scattered single pixels, the renderer's
frame-to-frame noise. The start and mid shots differ no more than the still one, so the gear poses are
the same as in the evidence image. Run once more with the final build, after the second review's
importer checks (`lg-r8`, same package, `FRAGMENT_CMD … ok=1`, exit 0): 0.01 % of pixels differ from the
`lg-r7` shots in each of the five.

No regression through the app's own load path (cache miss, re-processed with the new library):
`capture_route.sh src-gladius <canopy cycle route> -SCDefaults -SCLoad=AEGS_Gladius -SCCamera=first`
(`export_cache_hit=0`, canopy `open=1` then `open=0`, rest / raised / closed as before) and
`scorpius-smoke.txt` with `-SCLoad=RSI_Scorpius` (`Ladder Top open=1`, hull walk).

Not verified / not done: the Scorpius dashboards and the `open` / `close` item clips were not looked at
on the GPU (covered by the validator and the unit tests only); the Cutlass has no clip this changes and
was not run on the GPU; the app's own load path (`-SCLoad`) was not re-run after the reviews' fixes (the
clips it exports are unchanged; the headless suites and the GPU route import them with the final build); the
second review's fixes were not reviewed again; the
near-end frame shows no difference because the gear is already behind the doors there. The Blender addon
does not read `sources`. A merged clip still keeps only the first contributor's fragments, and clips of
unrelated items still merge by name (contract "Known gaps"). At the start of `Retract` the gear jumps from
its rest pose (compressed, on the floor) to the clip's end pose (extended) — present before this change,
not addressed.
