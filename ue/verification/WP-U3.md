# WP-U3 verification — 2026-11-09

Implementation is in the Runtime module. Headless checks use UE 5.8.2 and
`-nullrhi`; no rendered appearance is claimed. Full-pose reference acceptance is
**not passed**: the exporter has basis and unkeyed-channel defects. The
**Review fixes** section supersedes the original basis/reference rationale below.
GPU acceptance and cross-model review remain with the orchestrator.

## Implementation

- `FSCSkeletonBuilder` creates transient `USkeleton` / reference skeletons from
  parent-first GLB nodes, with an identity synthetic root and named sockets.
  Skin inverse binds are checked against the node reference pose (matrix
  tolerance 0.002, including translation in metres). These inputs need no extra
  skeleton sidecar. Duplicate names get stable node-index suffixes in UE;
  original names remain available for source CRC32 routing. Identity skin-anchor
  aliases inherit their owner's animation instead of applying it again.
- `FSCSkeletalMeshImporter` builds resident GPU vertex/index/weight buffers in
  memory without editor modules. JOINTS_0/WEIGHTS_0 remap to reference bones;
  normalized four-influence weights use 16-bit indices and weights. Positions,
  normals, tangents, both UV sets, colours and material slots are retained.
  Skeletal Nanite stays off. Socket-only rigs use an empty skeletal mesh.
- Rigid triangles split by their actual GLB node owner; each retains WP-U1
  Nanite, Lumen cards/distance fields/RT resources, complex collision and WP-U2's
  translucent/glass/decal/glow split. World-baked vertices get the inverse bind
  transform on their component, then attach to the owner's socket. Helpers,
  loadout children and lights follow the same animated hierarchy. Logical asset
  and scene-instance counts are unchanged; render meshes/components increase.
- `FSCAnimImporter` reads index + sidecar records, validates times/values, maps
  **case-sensitive UTF-8 CRC32** hashes, handles arrays using source name/path
  token and direction compatibility, and deterministically breaks provenance
  ties. Uniform frames include frame_count (the last frame, not sample count).
  Linear interpolation normalizes hemisphere-aligned quaternions. Each populated
  rig binding gets a `UAnimSequence` at the source frame rate using UE's data
  controller behind `WITH_EDITOR`. Retained transient tracks provide playback
  in the runtime path without requiring an editor data controller.
- `USCClipCatalog` is a DataAsset with id, display name, root catalogue owner,
  sidecar source, contributing skeleton paths, fragment/tags/scopes/speed,
  boomerang flag, canonical sequence and per-rig sequence bindings. An unbound
  clip has an empty metadata-only sequence, a warning, and no playable binding.
- `ASCVehicle::PlayClip`, `SnapClip`, `PlayFragment`, and `ResetPose` are Blueprint
  APIs. `USCRigComponent` is a `USkeletalMeshComponent`; the actor evaluates
  independently for each instance. Only authored position/rotation channels
  override the current pose. Disjoint clips compose; latest playback wins
  overlapping channels, completed clips hold their endpoint. Fragment playback
  respects negative speeds and avoids replaying source-path aliases over an
  already selected canonical clip. There is no AnimBlueprint/editor dependency.
- Entity and interior lights become point/spot/rect components, use candela
  proxy intensity without per-ship scaling, radius ×100, linear colour and
  projector half-angles. Direction and roll come from the exported direction
  and rotation. Attached lights convert their entity-local transform into the
  helper frame once. Container/helper basis follows WP-U1 placement rules.
  `ambient_proxy` is logged and skipped. Lossless state/style metadata remains
  in Manifest.LightJson; style waveforms are not provided by the contract.
  Rectangle dimensions are not exported: the UE component's default source
  width/height are retained (host defaults, not inferred ship data).
- PNG and HDR EXR gobos use the synthetic `M_SC_LightFunction` material with
  linear texture sampling, clamp addressing and texture reuse per source path.
  EXR decode validates image dimensions/bytes before allocation and preserves
  HDR platform pixels. No game-derived texture or geometry is checked in.

## Original axis / anchor decision — superseded by Review fixes

**The following rationale was incorrect.** The default-pose exporter shared the
importer's basis bug, so agreement with it was not independent evidence. Use the
Review fixes section below for the corrected basis and acceptance results.

For **node-local animation samples**, use the same conversion as the exported
GLB node locals. In column-vector notation (metres to centimetres separately):

```text
G = [ 0  0 -1 ]        pUE = 100 * G * pLocal
    [ 1  0  0 ]        RUE = G * RLocal * inverse(G)
    [ 0  1  0 ]
sidecar [w,x,y,z] -> UE FQuat(x,y,z,w) = [z,-x,-y,w]
```

The exported external GLB coordinate wrapper remains a reference bone and is
applied exactly once. Undoing the sidecar basis again produces a different pose
from the default-pose export. Entity-local lights use CryEngine Z-up -> UE
`(x,y,z) -> (y,x,z)`; lights within helper-frame containers use `G`, consistently
with WP-U1's helper-relative interior placement transform.

Non-identity `start_rotation` uses `bind * inverse(start) * sample`. Identity or
absent clip-root metadata uses absolute parent-local rotations, as the current
exporter and Blender action importer do. This deviates from a literal reading
of the brief's “when start_rotation is present”; the source implementation and
reference evidence justify it. Position-only tracks preserve bind rotation;
rotation-only tracks preserve bind translation. No asset-name branches implement
any of these rules.

## Commands

Run from the repository root. These are the actual commands; `run_import.sh`
writes `ue/SCViewer/Saved/Import/import.log`. Runtime/import build products and
reference GLBs remain ignored scratch; the new light-function `.uasset` contains
only a synthetic material graph.

```bash
ue/scripts/build.sh SCViewerEditor

"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/create_light_material.py" \
  -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC \
  -abslog=/tmp/wp-u3-light-material.log

python3 -I ue/scripts/make_test_tar.py out/dec-r5 \
  ue/SCViewer/Saved/Import/u3-dec-r5.tar

"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/test_importer.py" \
  -SCPackage="$PWD/out/dec-r5" \
  -SCVerifyTar="$PWD/ue/SCViewer/Saved/Import/u3-dec-r5.tar" \
  -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC \
  -abslog=/tmp/wp-u3-dec-r5-regression.log

ue/scripts/run_import.sh out/dec-r5 -nullrhi -SCVerifyRig
ue/scripts/run_import.sh out/dec-scorpius -nullrhi -SCVerifyRig
ue/scripts/run_import.sh out/dec-cutlass_black -nullrhi -SCVerifyRig
```

The existing, already-built CLI generated the reference (subsequently found to
share the basis bug; it is not an independent oracle). No cargo
command or `scripts/starbreaker.sh` was run, and no Rust/vendor source was changed.

```bash
target/debug/scviewer --no-cache \
  --cache ue/SCViewer/Saved/Import/u3-reference-cache \
  glb RSI_Scorpius ue/SCViewer/Saved/Import/u3-scorpius-default.glb \
  --materials none --no-interior --no-lights --apply-default-pose true

ue/scripts/run_import.sh out/dec-scorpius -nullrhi -SCVerifyRig \
  -SCPoseReference="$PWD/ue/SCViewer/Saved/Import/u3-scorpius-default.glb" \
  -SCPoseClip=landing_gear_extend
```

`ue/scripts/test_rig.py` provides the equivalent commandlet package/pose check
using `-SCPackage`, optional `-SCPoseReference` and `-SCPoseClip`. It was also run
successfully (exit 0, `WP-U3 RIG_TESTS_OK`):

```bash
"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/test_rig.py" -SCPackage="$PWD/out/dec-r5" \
  -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC \
  -abslog=/tmp/wp-u3-rig-script.log
```

## Results

Build: `Result: Succeeded`. Material creation: `WP-U3 LIGHT_MATERIAL_OK`.
Tar: `files=502 bytes=101939200`.

The legacy suite has **12 total tests**, including its material tests (not twelve
separate material tests). All twelve pass, plus `FSCRigMath`, `FSCSkinBuffers`, and
`FSCRigLights`: **15/15**. Synthetic coverage includes CRC case, position/rotation
basis agreement, hemisphere continuity, provenance scores, reference hierarchy,
named sockets, non-identity anchors, single-frame sequences, resident skin buffers,
16-bit weight normalization, light kinds/units/angles/direction and shared HDR gobos.

Package and game-host results (source assets count independently of render splits):

| Package | Source assets / instances | Catalogue sequences | Clips with receiver | Per-rig populated sequences | Soft triangles | Light components |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| dec-r5 (Gladius) | 45 / 61 | 63 | 62 | 79 | 35,925 | 76 |
| dec-scorpius | 42 / 92 | 69 | 65 | 88 | 0 | 127 |
| dec-cutlass_black | 53 / 129 | 40 | 34 | 57 | 5,378 | 225 |

Gladius's source has 21 root + 56 interior lights; one interior ambient proxy is
skipped. Scorpius has 94 + 33; Cutlass has 103 + 122. The component counts above
match those inputs. Gladius gear and canopy have disjoint authored tracks; the
runtime compositor keeps both. Cutlass cargo and side-door CAFs bind; the supplied
export has no receiver for its bulkhead/locker CAF channels.

Unbound catalogue entries are retained with a warning, not counted as animated:
Gladius `sdf_temp_anim.caf`; Scorpius `emp_extend.caf`, `qed_idle.caf`,
`qed_off.caf`, `qed_on.caf`; Cutlass `door_locker_close/open.caf`,
`drak_door_bulkhead_close/open.caf`, `nacelles_deploy/retract_galson.caf`.

The folder/tar check retains `assets=45 instances=61`, verifies static collision,
Nanite/Lumen/RT representation, and collects all objects owned by the old import,
including skeletons, sequences, components, textures and materials. Final output:

```text
VERIFY passed=1 folder_assets=45 tar_assets=45 folder_instances=61 tar_instances=61 representations=1 old_live=0 tracked_old=6416 baseline=27 loaded=323 reloaded=323 final=27 first_s=5.5369 second_s=6.9402
WP-U1 UNIT_TESTS_OK
```

All three final game-host checks exited 0:

```text
dec-r5:           RIG_VERIFY passed=1 clips=63 bound=62 sequences=79 tracks=637 rigs=62 soft_triangles=35925 lights=76
dec-scorpius:     RIG_VERIFY passed=1 clips=69 bound=65 sequences=88 tracks=675 rigs=93 soft_triangles=0 lights=127
dec-cutlass_black:RIG_VERIFY passed=1 clips=40 bound=34 sequences=57 tracks=360 rigs=132 soft_triangles=5378 lights=225
```

Warm import times: 5.6477 / 12.1903 / 11.6403 seconds respectively (engine DDC
assisted; not a processed-cache performance claim). Final DDC is 1.1 GiB,
project Intermediate 418 MiB, plugin Intermediate 268 KiB; about 48 GiB disk free.
No generated build products are in the change set.

## Original default-pose discrepancy / acceptance gate — superseded

The original, non-oracle export is 25,248,000 bytes. The complete comparison deliberately
fails on one bone:

```text
POSE_DIFFERENCE bone=BONE_Front_Mid_Arm degrees=154.69431
POSE_CHECK passed=0 clip=landing_gear_extend bones=33 max_degrees=154.694311 tolerance_degrees=5
POSE_AUTHORED_ROTATIONS passed=1 bones=26 max_degrees=0.000007 unauthored_reference_differences=1
strict_pose_exit=1
```

Its channel has `has_rotation=false`, an empty `rotation` array and position keys.
The unposed GLB carries a non-identity bind rotation. The default-pose GLB carries
identity, because `clip_final_pose` supplies identity for absent rotation keys and
`apply_pose_to_skeleton` overwrites the reference rotation unconditionally. The
importer preserves the unanimated bind channel. The authored-rotation-only result
is logged separately; it must not be substituted for full-pose acceptance.

TODO(WP-R6): preserve unanimated reference rotations in the default-pose export;
`has_rotation` already carries the needed mask, so no skeleton-sidecar addition is
needed. Regenerate the same reference and rerun the same comparison.

TODO(WP-U3): full-pose <=5-degree acceptance remains pending the WP-R6 correction.
The current reference test returns exit 1 rather than weakening the full-pose gate.

## GPU checks for the orchestrator

TODO(WP-U3): run and record all of these on the GPU; null-RHI is not visual evidence.

1. In PIE and the Editor `-game` host, call `PlayFragment(Landing_Gear, Deploy)` on
   Scorpius. Check front/rear/left/right gear routing, wing motion and canopy, with
   no quaternion flips. Snap the gear and revisit the reference gate after WP-R6.
2. On Gladius, call `PlayClip(canopy_open)` and `PlayClip(landing_gear_extend)`
   together. Confirm both move concurrently, rigid struts/children remain on their
   owning sockets, and soft gear surfaces deform without missing sections or
   detached skin-anchor aliases. Scrub/snap first and last through the Blueprint API.
3. Play Cutlass cargo and side-door clips. Check moving complex collision follows
   rigid parts; skeletal parts have no fabricated physics bodies. Verify the lit
   corridor with the 225 lights, without asset-specific exposure/intensity tuning.
4. Check point/spot/rect positions and orientations, attached lights during wing/
   canopy motion, interior helper-frame placement, projector roll, gobos and falloff.
   Source rectangle sizes are absent, so assess the retained UE source-size default.
5. Recheck WP-U1/U2 lit, Nanite, Lumen Scene and HWRT views: opaque rigid hull present,
   split glass/decals/glow unchanged, blue nozzle control still works. Confirm the
   skeletal material permutations render on the GPU. Save screenshots before/after.
6. Reload/unload repeatedly with animation running; check no old visible components,
   lights or moving collision remain. The headless object leak check already covers
   object lifetime, but not render-thread appearance.

Convenient game-host entry points (omit `-nullrhi` on the GPU):

```bash
ue/scripts/run_import.sh out/dec-scorpius -SCStay -SCFragment=Landing_Gear -SCTag=Deploy
ue/scripts/run_import.sh out/dec-r5 -SCStay -SCPlayClip=canopy_open
ue/scripts/run_import.sh out/dec-cutlass_black -SCStay \
  -SCPlayClip=animations/spaceships/ships/drak/cutlass/cargo_door_open.caf
```

The harness also accepts `-SCSnapClip=<id>`. For simultaneous clip calls use the
Blueprint-callable ASCVehicle API; a UI/transport panel remains WP-V2.

## Changed files

Under `ue/Plugins/SCBridge/Source/SCBridge/`:

- New Public: `SCAnimImporter.h`, `SCClipCatalog.h`, `SCLightImporter.h`,
  `SCRigComponent.h`, `SCSkeletonBuilder.h` (declares both skeleton/skin builders).
- New Private: `SCAnimImporter.cpp`, `SCLightImporter.cpp`, `SCRigComponent.cpp`,
  `SCRigTests.cpp`, `SCRigVerification.cpp`, `SCSkeletonBuilder.cpp`.
- Updated Public: `SCImporter.h`, `SCMeshImporter.h`, `SCPackageReader.h`, `SCVehicle.h`.
- Updated Private: `SCImportHarness.cpp`, `SCImportTests.cpp`, `SCImporter.cpp`,
  `SCMeshImporter.cpp`, `SCPackageReader.cpp`, `SCVehicle.cpp`.

Other changes:

- Added `ue/scripts/create_light_material.py`, `ue/scripts/test_rig.py`.
- Added synthetic `ue/Plugins/SCBridge/Content/Materials/M_SC_LightFunction.uasset`.
- Updated `docs/work-packages/WP-U3-rig-animation.md` and added this record.

The obsolete `Source/SCBridgeLib/SCBridgeLib.Build.cs` stub was removed during
integration to coexist with the new ThirdParty module. That deletion is now
included in the other agent's WP-R4 commit `72dc4c5` (HEAD advanced from `b1d92c6`
during this work), so it is not an outstanding WP-U3 diff. No writes to `ThirdParty/SCBridgeLib`, `crates`, `vendor`, Cargo
files or `.git`; no branch/commit/PR was created because `.git` is read-only.
Other agents' changes in the working tree are excluded from this file list.

## Review fixes

This section supersedes the original axis rationale and pose-reference evidence
above. Opus identified that the original importer and `clip_final_pose` shared
an axis error; agreement between them was circular. The GPU review in
`WP-U3-gpu.md` showed the consequence: mirrored, detached wings/canopy/gear parts.

1. **Sidecar basis corrected.** Undo the exporter's per-sample swap before G:
   native Y-up `(cx,cy,cz)` was exported as `(cx,-cz,cy)`. The composed sidecar
   conversion is `pUE = 100*(by,bx,bz)` and `[w,x,y,z] -> FQuat(-y,-x,-z,w)`.
   In column-vector notation, `B = [[0,1,0],[1,0,0],[0,0,1]]`,
   `RUE = B * Rsidecar * inverse(B)`. The GLB wrapper stays in the skeleton.
   Tests added before the fix failed on both translation and rotation
   (`/tmp/wp-u3-review-red.log`, `FSCRigMath passed=0`); they pass after it.
   New package checks use original GLB binds and raw sidecars, never the posed
   export. They check all 15 Scorpius position-keyed receivers against native
   endpoints reconstructed as `GLB bind + G*(native sample - native bind)`, and
   additionally check the seven endpoints that actually equal bind. The other
   eight have authored offsets (up to 52.398571 cm from bind); requiring all 15
   to equal bind would incorrectly erase animation. Front_Base_Arm has its own
   strict 0.01 cm bind assertion. CableTop frame zero has a 0.05-degree assertion
   directly against its original GLB rotation.
2. **Reference comparison quarantined.** It now checks local translations as
   well as rotations, with 0.01 cm / 5-degree tolerances. Its log explicitly says
   `pending=WP-R6 oracle=0`; invoking it still returns failure until corrected
   reference data passes. The source-based regression checks run first and are
   independent acceptance evidence. The old authored-rotation agreement is
   withdrawn, not treated as proof of correct playback.
3. **Unkeyed channels preserved.** Bind is the initial pose (§4.4); clips write
   only sampled channels and compose onto current state (§14.6). No importer
   workaround overwrites an unkeyed rotation. The exact two-part WP-R6 request is
   recorded below and in the brief. No new skeleton-sidecar field is needed.
4. **Playback lifecycle fixed.** Direction determines the endpoint; zero-delta
   ticks cannot stop a forward clip at frame zero or a reverse clip at its last
   frame. Finished entries leave the active list. Held layers, at most one per
   clip, preserve sparse overlap order without enabling tick. Only rigs bound
   by changed clips are evaluated, and unchanged composed poses do not refresh
   sockets/collision. Tests cover both directions, disjoint rigs, a newer held
   layer over an older active layer, and 120 idle ticks with zero rig refreshes.
5. **Rig creation follows binding.** CPU channel derivation precedes skeleton
   and sequence creation. Only receiving assets create skeletal components and
   split rigid geometry by owner; others keep U1/U2's flattened static path.
   Static attachment helpers remain available to lights and child entities.
   Owner collision BVHs are derived on a worker, checked before proceeding, and
   retained across translucent splitting. A derivation failure warns and skips
   the owner instead of silently importing collisionless geometry.
6. **Other review fixes.** Fragment options retain GUID/weight/scopes, use a
   reproducible actor-local weighted draw, and compete for overlapping scopes.
   Disjoint scopes can run together; aliases still play once. Tests cover zero
   and relative weights, broad-tag arbitration, disjoint scopes, and toggling.
   `ToggleClip(fragment, openTag=Open, closeTag=Close)` is now Blueprint-callable;
   callers can supply `Deploy/Retract` or another exported pair. Cyclic positions
   use `bind + (sample-first)`, while noncyclic positions remain absolute. A clip
   is cyclic when at least one channel moves and all authored channels return
   to their first sample (0.1 degrees / 0.01 cm); snap-last remains literal.
   EXR and PNG clamp addressing is set before the final resource update. The
   regenerated synthetic light-function material uses Rec.709 RGB luminance
   `(0.2126, 0.7152, 0.0722)`. Mesh import is split into GLB reading, derivation,
   and static build/collision files; U3 files have purpose comments and separate
   statements.
7. **Light intensity mapping unchanged.** The candela-proxy mapping remains
   exactly as before; intensity conversion and overexposure are WP-U3b's work.
   No WP-U3b file, DefaultEngine.ini, Rust/vendor source, ThirdParty file, or git
   metadata was written by this review-fix work.

Running the legacy package tests on Scorpius exposed an existing test assumption:
`Trim_Core_Plating` and `Glow_Thrusters` carry v2 emittance-map strengths despite
having no `Glow` attribute. The package test now checks the explicit v2 strength
for that category, instead of incorrectly requiring zero. No material builder,
material intensity, or lighting behavior was changed for this correction.
Cutlass also revealed a valid black palette/base colour: the old test required
the product of emission tint and every palette to be nonblack. It now checks
the v2 authored emission tint directly; the original material fixture tests
still cover the palette multiplication behavior.

Exact verification commands, from the repository root:

```bash
ue/scripts/build.sh SCViewerEditor

"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/create_light_material.py" \
  -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC \
  -abslog=/tmp/wp-u3-review-material.log

for ship in dec-r5 dec-scorpius dec-cutlass_black; do
  python3 -I ue/scripts/make_test_tar.py "out/$ship" \
    "ue/SCViewer/Saved/Import/u3-review-$ship.tar"
  "$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
    "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
    -script="$PWD/ue/scripts/test_importer.py" \
    -SCPackage="$PWD/out/$ship" \
    -SCVerifyTar="$PWD/ue/SCViewer/Saved/Import/u3-review-$ship.tar" \
    -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC \
    -abslog="/tmp/wp-u3-review-$ship-tests.log"
done

for ship in dec-r5 dec-scorpius dec-cutlass_black; do
  ue/scripts/run_import.sh "out/$ship" -nullrhi -SCVerifyRig \
    > "/tmp/wp-u3-review-$ship-rig.console.log" 2>&1
done

# Deliberate pending-WP-R6 failure, not the pose oracle:
ue/scripts/run_import.sh out/dec-scorpius -nullrhi -SCVerifyRig \
  -SCPoseReference="$PWD/ue/SCViewer/Saved/Import/u3-scorpius-default.glb" \
  -SCPoseClip=landing_gear_extend \
  > /tmp/wp-u3-review-reference.console.log 2>&1
```

The game-host evidence is captured from each process's console stream; the
shared `Saved/Import/import.log` was stale and is not the review-fix evidence.
All UE processes launched by this agent were sequential under the build lock. A scratch launcher used
`UV_CACHE_DIR=/tmp/scviewer-uv uv run --no-project python /tmp/u3_review_verify.py`
to execute the same commands and retain exit codes in
`/tmp/wp-u3-review-results.json`. No cargo or StarBreaker command was run.
The old reference was read, not regenerated.

Review-fix results: build `Result: Succeeded`; material generation
`WP-U3 LIGHT_MATERIAL_OK`. All three final `test_importer.py` invocations exit 0,
with **17/17 tests** each: the same 12 legacy tests plus five rig tests. The
legacy count is twelve total, including material tests, not twelve distinct
material tests. All three `-SCVerifyRig` game-host runs exit 0.

| Package | Assets / instances | Clips / bound | Binding sequences / tracks | Rigs before → after | Soft triangles | Lights | Static render meshes / components |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| dec-r5 | 45 / 61 | 63 / 62 | 79 / 637 | 62 → 16 | 35,925 | 76 | 287 / 337 |
| dec-scorpius | 42 / 92 | 69 / 65 | 88 / 675 | 93 → 23 | 0 | 127 | 758 / 896 |
| dec-cutlass_black | 53 / 129 | 40 / 34 | 57 / 360 | 132 → 23 | 5,378 | 225 | 459 / 645 |

The 1 / 4 / 6 unbound catalogue entries are unchanged. No source clip, authored
track, soft triangle, or light component was lost by limiting rig creation.

Source-based checks (centimetres, UE world axes):

```text
SOURCE_FRONT_BASE_ARM local_cm=(3.800025955,-0.000034915,100.886833668)
  world_cm=(274.235940374,-0.000024050,426.523390375) bind_error_cm=0.000011927
SOURCE_POSE_CHECK passed=1 positions=15 max_position_cm=0.000000000 tolerance_cm=0.01
  bind_endpoints=7 max_bind_cm=0.000092858
SOURCE_CABLE_FRAME0 bone=LG_L_Back_CableTop_Skin_bn degrees=0.005756902 tolerance_degrees=0.05
SOURCE_POSE_CHECK passed=1 cable_checks=1 max_cable_degrees=0.005756902
PLAYBACK_REGRESSION idle_ticks=120 idle_evaluations=0 active_after_finish=0 weighted_wins=32/32
```

The weighted fixture uses weights 1000:1 over 32 deterministic draws. Separate
assertions cover zero-weight exclusion and concurrent disjoint scopes. The
unchanged-composed-pose assertion confirms an older active layer cannot trigger
socket/collision refresh beneath a newer held override.

Folder/tar reload and unload checks include all old import-owned UObjects
(skeletons, sequences, components, textures, materials, and meshes). All three
retain collision hits, matching content hashes, and Nanite/Lumen representations:

```text
dec-r5: VERIFY passed=1 folder_assets=45 tar_assets=45 folder_instances=61 tar_instances=61 representations=1 old_live=0 tracked_old=5942 baseline=27 loaded=314 reloaded=314 final=27 first_s=6.5439 second_s=6.7082
dec-scorpius: VERIFY passed=1 folder_assets=42 tar_assets=42 folder_instances=92 tar_instances=92 representations=1 old_live=0 tracked_old=10927 baseline=27 loaded=785 reloaded=785 final=27 first_s=12.7856 second_s=12.2525
dec-cutlass_black: VERIFY passed=1 folder_assets=53 tar_assets=53 folder_instances=129 tar_instances=129 representations=1 old_live=0 tracked_old=9472 baseline=27 loaded=486 reloaded=486 final=27 first_s=11.2116 second_s=11.7233
```

The old reference comparison is deliberately still a failure (exit 1):

```text
POSE_CHECK pending=WP-R6 oracle=0 passed=0 clip=landing_gear_extend bones=33
  max_degrees=174.917697 max_centimetres=631.834068 tolerance_degrees=5 tolerance_cm=0.01
POSE_AUTHORED_ROTATIONS passed=0 bones=26 max_degrees=174.917697 unauthored_reference_differences=1
```

This is expected evidence of the stale exporter defects, not a new acceptance
oracle or a reason to revert the corrected basis.

TODO(WP-R6): upstream animation/pose.rs must (a) make BonePose.rotation an Option built from ch.rotations.last() and have apply_pose_to_skeleton set it only when Some, and (b) bake into skeleton bones in native order without the axis swap (rotation [w,x,y,z] from [x,y,z,w], position unchanged), keeping cry_xyzw_to_blender_wxyz for the sidecar only; then regenerate the reference.

TODO(WP-U3): rerun the full-pose 5-degree / 0.01 cm comparison after WP-R6 regenerates the reference.

TODO(WP-U3): orchestrator must rerun the following GPU checks; null-RHI cannot establish visual acceptance.

- Scorpius gear Deploy/Retract, wings, canopy, nose gun and attachments: first,
  intermediate, last and reverse poses. Confirm no mirrored travel, metre-scale
  detachment or quaternion flips; compare the prior failing GPU captures.
- Gladius gear and canopy concurrently: CableTop frame zero, rigid struts, soft
  gear skin, all attached parts, and first/last snaps. Confirm skin sections and
  materials still render correctly.
- Cutlass ramp, gear strut and side doors: preserve the previously correct motion,
  collision following, and attachment placement. Confirm completed clips leave
  the pose held with no recurring rig/socket movement in profiling.
- Fragment scope arbitration and `ToggleClip`: broad tags choose one option per
  conflicting scope, disjoint scopes coexist, and the interact action alternates
  the caller's open/close (or deploy/retract) pair, including negative speed.
- Projector clamp edges and RGB luminance using red, green and blue PNG/EXR gobos;
  verify HDR range, roll and attached-light following. Reassess corridor exposure
  only with WP-U3b's separately reviewed intensity conversion; U3 does not claim
  the overexposure is fixed.
- WP-U1/U2 Nanite, Lumen Scene, HWRT, glass/decals and engine-glow views, plus
  repeated unload/reload while animating: no leftover visible geometry, light or
  collision. Save before/after screenshots and a short playback capture.

Files changed by this review-fix work (excluding other agents' changes):

- `docs/work-packages/WP-U3-rig-animation.md`
- `ue/Plugins/SCBridge/Content/Materials/M_SC_LightFunction.uasset`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCAnimImporter.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCGlbReader.h`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCImportTests.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCImporter.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCLightImporter.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCMaterialTests.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCMeshBuild.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCMeshImporter.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCRigComponent.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCRigPlaybackTests.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCRigSourceVerification.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCRigTests.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCRigVerification.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCSkeletonBuilder.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCVehicle.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Public/SCAnimImporter.h`
- `ue/Plugins/SCBridge/Source/SCBridge/Public/SCClipCatalog.h`
- `ue/Plugins/SCBridge/Source/SCBridge/Public/SCLightImporter.h`
- `ue/Plugins/SCBridge/Source/SCBridge/Public/SCRigComponent.h`
- `ue/Plugins/SCBridge/Source/SCBridge/Public/SCSkeletonBuilder.h`
- `ue/Plugins/SCBridge/Source/SCBridge/Public/SCVehicle.h`
- `ue/scripts/create_light_material.py`
- `ue/verification/WP-U3.md`

`git diff --check` is clean for these files. No build products or game-derived
exports are in the change set. Ignored DDC is 1.2 GiB, project Intermediate
423 MiB, plugin Intermediate 276 KiB; no cache purge was needed. Build outputs
remain ignored for the orchestrator's GPU run. The tree is left uncommitted;
`.git` is read-only and other agents' files are untouched.

## WP-U3b importer follow-through — 2026-11-09

Applied §8 of `WP-U3b.md` after `git apply --check`, then completed its state,
exposure and verification items. No Rust/vendor/ThirdParty code or `docs/0*.md`
was changed; no cargo command, commit or git-metadata write was performed.

- **Gobo sharing:** one light-function MID per texture and master, owned by the
  vehicle and reused across its ship/child/interior instances. Cutlass's 184
  projectors use eight MIDs; Scorpius uses ten, Gladius six. The initial Cutlass
  game-host check confirmed eight before removing `r.LightFunctionAtlas.Size=16`.
  Final package tests observe the engine default **Size=4, 16 slots**. All three
  fit; there is no atlas-size override left in `DefaultEngine.ini`.
- **Fallback shadows:** local lights are selected iff their radius is at least
  the instance's sorted radius at index `floor(3*N/4)`. Ambient proxies and
  directional lights are excluded from that population; the directional sun
  keeps its normal shadow policy. Ties at the threshold are retained. Selection
  includes authored-but-disabled fixtures so changing the default state does
  not silently change the documented radius budget. Actual visible shadowed
  counts are reported separately. Thresholds are retained per component.
- **Runtime gating:** the supplied project/HWRT check is extended to account for
  scalability, supported shader platform, enabled HWRT/inline tracing or the
  engine's `DoesProjectSupportDistanceFields()` SWRT gate. Null-RHI always uses
  the fallback. The policy is computed at import; after a runtime renderer
  setting change, call Blueprint `ASCVehicle.RefreshLightShadowPolicy()` or
  reimport. This avoids polling every frame. The Viewer PPV does not override
  MegaLights; per-view debug overrides are not a separate global shadow policy.
- **Authored viewer defaults:** an explicit `offState` imports invisible with
  zero component intensity. Otherwise, `states.defaultState` is selected when
  present, including a zero-intensity default; missing state maps retain the
  legacy flat data. This distinction matters: the export contract says
  `active_state` is the first nonzero state in default/auxiliary/emergency/
  cinematic order, not a running ship's actual state. Twenty-four Cutlass and
  three Scorpius records export `auxiliaryState` only because their authored
  defaults are zero. They now import disabled, while all components remain
  available for later state selection. Positive intensity still maps 1:1 from
  the selected state's candela proxy; no intensity conversion was changed.
- **State properties:** `ASCVehicle.LightStates` exposes each component, source
  name/owner, `ExportActiveState`, viewer `ActiveState`, `bEnabled`, and lossless
  `StatesJson`, plus radius/shadow metadata. This preserves auxiliary/emergency/
  cinematic/off choices for WP-V2 without guessing a parked state from names.
- **Gladius wing evidence:** all eight records in `LightGroup-ext_wing_right`
  and `LightGroup-ext_wing_left` have `active_state=defaultState` and positive
  default intensity. Four projectors are 300 cd; the four Omnis are three at
  1500 cd and one at approximately 450 cd. Their `offState` entries are zero,
  but are not selected. They therefore remain enabled. The hangar screenshot
  does not justify an importer override.
- **Bounded exposure:** `Viewer.umap` now has one enabled, unbound post-process
  volume, blend weight 1, priority 10, histogram auto exposure, bias 0 and
  min/max **−3 / +6 EV100**. `configure_viewer_exposure.py` updates the existing
  map without replacing its actors; `create_viewer.py` invokes the same helper
  when creating the map. The helper was run twice; it updates the same volume.
  Every package test reloads the saved map and checks its persisted bounds.

**Rendered exposure readout is unavailable under null-RHI.** Installed engine
`PostProcessEyeAdaptation.cpp:95` defines `r.EyeAdaptation.VisualizeDebugType` as a
visualization-mode selector, not a log/readback switch. Its exposure result is
computed by render-graph GPU passes and read by
`FEyeAdaptationManager::UpdateLastExposureFromBuffer` via `FRHIGPUBufferReadback`
(around line 1460). A headless placeholder is not a measured scene exposure.

| View after these changes | Measured adapted EV100 | Verified bounds |
| --- | --- | --- |
| Cutlass bay (`-SCView=180,2,0.25`) | Pending orchestrator GPU readout | −3…+6 |
| Scorpius exterior (default camera) | Pending orchestrator GPU readout | −3…+6 |

Do not substitute WP-U3b's earlier +0.32 / −2.01 readings for new measurements:
the default light states and projector material allocation have changed.

Tests were added before the importer changes: shared-gobo identity failed in
`FSCRigLights`, and `FSCLightDefaults` failed its quartile/off-state assertions
(`/tmp/wp-u3b-import-red.log`, exit 255). Final tests cover radii 1/2/3/4 selecting
only 4, an independent instance with radius 0.5, excluded ambient proxies,
explicit off even with a positive supplied intensity, zero default with an
auxiliary fallback, missing active state, state metadata, and null-RHI fallback
with the project preference both off and on. Package verification independently
reads the manifest, asserts per-instance selection and state invariants, and
asserts shared gobo material count equals distinct texture count. Ship-specific
counts below are informational; no importer rule branches on a ship name.

Exact commands from the repository root:

```bash
ue/scripts/build.sh SCViewerEditor

"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
  -script="$PWD/ue/scripts/configure_viewer_exposure.py" \
  -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC \
  -abslog=/tmp/wp-u3b-viewer-exposure.log
# Repeated with -abslog=/tmp/wp-u3b-viewer-exposure-idempotent.log.

# Initial shared-material measurement before removing the atlas override:
ue/scripts/run_import.sh out/dec-cutlass_black -nullrhi -SCVerifyRig \
  > /tmp/wp-u3b-cutlass-atlas-check.log 2>&1

for ship in dec-r5 dec-scorpius dec-cutlass_black; do
  python3 -I ue/scripts/make_test_tar.py "out/$ship" \
    "ue/SCViewer/Saved/Import/u3b-$ship.tar"
  "$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" \
    "$PWD/ue/SCViewer/SCViewer.uproject" -run=pythonscript \
    -script="$PWD/ue/scripts/test_importer.py" \
    -SCPackage="$PWD/out/$ship" \
    -SCVerifyTar="$PWD/ue/SCViewer/Saved/Import/u3b-$ship.tar" \
    -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC \
    -abslog="/tmp/wp-u3b-$ship-tests.log"
done
```

All UE processes launched by this agent were sequential. The scratch launcher
`UV_CACHE_DIR=/tmp/scviewer-uv uv run --no-project python /tmp/u3b_verify.py`
ran the package commands and recorded exits in `/tmp/wp-u3b-import-results.json`.

Final results: **build succeeded; all three package suites exit 0 with 18/18
unit tests** (12 legacy, five rig, one new light-default/shadow-budget test).
The saved map check logs
`VIEWER_EXPOSURE_VERIFY passed=1 unbound=1 min_ev100=-3 max_ev100=6 bias=0 method=histogram`.
The initial Cutlass game-host rig/light check also exits 0.

| Package | Assets / instances | Light components | Enabled / disabled | Projectors | Gobo textures = MIDs | Fallback selected | Visible shadowed in fallback |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Gladius | 45 / 61 | 76 | 76 / 0 | 47 | 6 = 6 | 22 | 22 |
| Scorpius | 42 / 92 | 127 | 124 / 3 | 57 | 10 = 10 | 36 | 33 |
| Cutlass | 53 / 129 | 225 | 201 / 24 | 184 | 8 = 8 | 60 | 58 |

Each package's folder and tar light checks pass with `megalights_active=0`
under null-RHI and `atlas_slots=16`. The fallback counts match the WP-U3b
prediction without hard-coded ship expectations. On a MegaLights GPU host,
all local components retain their shadow flag; disabled lights still emit
nothing. That branch needs the GPU verification below.

Exact final leak/representation evidence:

```text
dec-r5: [2026.11.09-17.13.25:339][  0]LogPython: VIEWER_EXPOSURE_VERIFY passed=1 unbound=1 min_ev100=-3 max_ev100=6 bias=0 method=histogram
dec-scorpius: [2026.11.09-17.13.39:698][  0]LogPython: VIEWER_EXPOSURE_VERIFY passed=1 unbound=1 min_ev100=-3 max_ev100=6 bias=0 method=histogram
dec-cutlass_black: [2026.11.09-17.14.05:969][  0]LogPython: VIEWER_EXPOSURE_VERIFY passed=1 unbound=1 min_ev100=-3 max_ev100=6 bias=0 method=histogram
```

Both imports independently satisfy light-state, shadow and sharing invariants.
All old import-owned UObjects, including shared MIDs and the state component
references, are covered by the leak check. Static meshes return to baseline 27.
`git diff --check` is clean. Ignored DDC is 1.4 GiB, project Intermediate 426 MiB,
plugin Intermediate 300 KiB. Test tars remain regenerable ignored scratch; no
build products or game-derived exports are part of the change set.

TODO(WP-V2): add named light-state selection/reapplication and style waveform playback using `LightStates`; preserve the exported state map and distinguish the authored default from the exporter's first-nonzero fallback.

TODO(WP-U3): orchestrator must measure post-change adapted EV100 in the Cutlass bay and Scorpius exterior and record GPU acceptance for the checks below.

GPU checks for the orchestrator:

1. With the final ini, measure settled adapted EV100 using `ShowFlag.VisualizeHDR 1`
   (optionally `r.EyeAdaptation.VisualizeDebugType 1`) in the Cutlass bay
   (`-SCView=180,2,0.25`) and Scorpius exterior (default view). Record numeric
   readouts and captures, verify −3…+6 bounds, then traverse exterior/interior
   views to check adaptation. These must be new measurements.
2. Confirm default atlas Size=4 retains all visible gobo patterns, with 8/10/6
   distinct shared materials, no overflow or dropped patterns; check HDR/PNG,
   luminance, clamp edges and projector orientation/roll against WP-U3b captures.
3. Run fresh imports with MegaLights active on HWRT: `-SCVerifyRig -SCStay`
   should log `megalights_active=1`, with shadow flags 225/127/76 for
   Cutlass/Scorpius/Gladius. Compare bay/hull occlusion and frame times; no light
   leaks or VSM one-pass overflow. Enabled counts remain 201/124/76.
4. Disable MegaLights before import, or call `RefreshLightShadowPolicy()` after
   toggling it at runtime. Verify the fallback selects 60/36/22 flags (58/33/22
   visible shadow casters), compare shadow quality and VSM cost, then restore
   MegaLights and refresh. Test a host without HWRT if available.
5. Check the disabled default fixtures on Cutlass/Scorpius and the still-enabled
   Gladius wing lights. The latter follow their positive authored default, not
   the unknown parked state in the reference photograph. Recheck lit materials,
   emissives, Nanite/Lumen/HWRT and animated light attachments under auto exposure.
6. Reload/unload while viewing interiors: no leftover projected gobos, visible
   lights or shadowing from a previous vehicle. The headless GC checks pass;
   render-thread appearance remains a GPU check.

Files changed in this follow-through:

- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCImportTests.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCImporter.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCLightImporter.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCLightTests.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCLightVerification.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCRigTests.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCRigVerification.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCVehicle.cpp`
- `ue/Plugins/SCBridge/Source/SCBridge/Public/SCLightImporter.h`
- `ue/Plugins/SCBridge/Source/SCBridge/Public/SCLightState.h`
- `ue/Plugins/SCBridge/Source/SCBridge/Public/SCVehicle.h`
- `ue/SCViewer/Config/DefaultEngine.ini`
- `ue/SCViewer/Content/Maps/Viewer.umap`
- `ue/scripts/configure_viewer_exposure.py`
- `ue/scripts/create_viewer.py`
- `ue/scripts/test_importer.py`
- `ue/verification/WP-U3.md`
