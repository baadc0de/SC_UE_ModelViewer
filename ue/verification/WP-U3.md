# WP-U3 verification — 2026-11-09

Implementation is in the Runtime module. Headless checks use UE 5.8.2 and
`-nullrhi`; no rendered appearance is claimed. Full-pose reference acceptance is
**not passed**: the exporter changes an unanimated rotation (details below).
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

## Axis / anchor decision

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

The existing, already-built CLI generated the independent reference. No cargo
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

## Default-pose discrepancy / acceptance gate

The independent export is 25,248,000 bytes. The complete comparison deliberately
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
