# WP-R6 — Upstream default-pose export fix and the full-pose acceptance gate

**Agent:** Codex. **Depends on:** WP-U3 landed. **Build lock:** cargo. **Lands in:**
`vendor/StarBreaker` on `scviewer/lights` (stacked) plus `scviewer-core`/`-cli`.

## Why
WP-U3's full-pose check compares the importer's snapped pose against upstream's
"default pose" export (`ExportOptions::apply_default_animation_pose`). The Opus
review of WP-U3 (2026-11-09) found that reference wrong in two ways, so the ≤5°
acceptance gate cannot be passed until upstream is fixed:

1. `animation/pose.rs` writes a rotation for every bone of a clip even when the
   channel has no rotation keys (the unkeyed `BONE_Front_Mid_Arm` got identity
   instead of its bind rotation — a 154.7° error). Per whitepaper §4.4 and §14.6
   unkeyed bones stay at bind; the Blender addon writes only sampled channels.
2. `clip_final_pose` bakes Blender-Z-up values (the sidecar convention, after
   `cry_xyzw_to_blender_wxyz`) into `skeleton::Bone` locals, which are CryEngine
   Y-up — so the baked reference is axis-swapped (`BONE_Front_Base_Arm` T =
   (0, 0.038, 1.0089) instead of (0, 1.0089, −0.038)).

## Tasks
1. `pose.rs`: make `BonePose.rotation` (and position) `Option`s built from
   `ch.rotations.last()` / `ch.positions.last()`; `apply_pose_to_skeleton` sets a
   component only when `Some`. Unit test with a synthetic clip where one bone has
   position keys only.
2. Bake into skeleton bones in native order without the axis swap: rotation
   `[w,x,y,z]` from `[x,y,z,w]`, position unchanged; keep `cry_xyzw_to_blender_wxyz`
   for the sidecar only. Unit test: a bone with a known bind and a known sample ends
   at the sample in Y-up; the sidecar for the same clip still shows the swapped
   values (parity test).
3. Regenerate the reference GLB used by the UE rig check
   (`ue/SCViewer/Saved/Import/u3-scorpius-default.glb` is scratch — produce it with
   `scviewer glb RSI_Scorpius out/scorpius-default.glb --default-pose` or equivalent;
   add the `--default-pose` / `default_animation_tags` options to `scviewer-cli` if
   missing) and document the exact command in the brief for the orchestrator to feed
   `-SCPoseReference`.
4. Contract/whitepaper note: clarify in `docs/decomposed-export-contract.md` that the
   default-pose GLB keeps unkeyed bones at bind.

## Done criteria
- Upstream tests + the two new unit tests green; `cargo test --workspace` green.
- The regenerated Scorpius default-pose GLB has `BONE_Front_Base_Arm` at the swapped-
  back position (0, 1.0089, −0.038) relative to its parent (in CryEngine Y-up) and
  `BONE_Front_Mid_Arm` at its bind rotation.
- Orchestrator step (recorded here, not yours): `ue/scripts/run_import.sh
  out/dec-scorpius -nullrhi -SCVerifyRig -SCPoseReference=<new glb>
  -SCPoseClip=landing_gear_extend` passes within 5° / 1 cm on all bones.

## Implementation and verification (2026-11-09)

`BonePose` now carries optional native Y-up rotation and position. Extraction
reorders `xyzw` to `wxyz` without changing axes; application writes only keyed
components, recomputes world transforms and preserves inverse binds. The entity
exporter uses this corrected shared path. Its existing tag eligibility and
bone-signature clip selection are unchanged. Sidecar serialization is unchanged.

The two synthetic regression tests were written and run before the fix:
`final_pose_preserves_unkeyed_bind_components` failed with identity replacing
the bind rotation; `final_pose_is_native_y_up_while_sidecar_stays_blender_z_up`
failed with `[1, 3, 2]` instead of `[1, 2, -3]`. After the fix both pass. They also
cover rotation-only, empty and absent channels, final-key selection, cached world
transforms, inverse binds, and unchanged sidecar values. The existing DBA fixture
gate now converts its optional native pose to Blender space for comparison with
its recorded Blender reference.

The CLI adds `--default-pose` and repeatable `--default-animation-tag <tag>`.
Tags preserve argument order, replace the default list when supplied, and pass
through `GlbOptions.default_animation_tags` to upstream. The default list comes
from upstream (`landing_gear_extend`). Existing bundled/decomposed defaults and
`--apply-default-pose <bool>` remain; the two pose switches conflict. The FFI ABI
is unchanged and continues to use the default tags. Options serialization and
cache-key tests cover the new field; older JSON receives upstream defaults.

Commands run from the superproject root (debug builds, one cargo process at a
time; `CARGO_INCREMENTAL=0` also disables the upstream workspace's incremental
profile):

```bash
# Red first (0 passed, 2 failed); green after the fix (2 passed).
CARGO_TARGET_DIR=./target CARGO_INCREMENTAL=0 cargo test --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d --lib animation::pose::tests
CARGO_TARGET_DIR=./target cargo build --workspace
CARGO_TARGET_DIR=./target cargo test --workspace
CARGO_TARGET_DIR=./target CARGO_INCREMENTAL=0 cargo test --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d

./target/debug/scviewer glb RSI_Scorpius out/scorpius-default.glb --default-pose
./target/debug/scviewer glb AEGS_Gladius out/gladius-default.glb --default-pose
python3 -I scripts/verify_pose_reference.py pose out/scorpius-default.glb out/dec-scorpius/Data/Objects/Spaceships/Ships/RSI/LandingGear/Scorpius/RSI_Scorpius_Landinggear_Front_SKIN_LOD1.glb --position-bone BONE_Front_Base_Arm --rotation-bone BONE_Front_Mid_Arm

# Fresh comparison packages; existing UE inputs are not overwritten.
./target/debug/scviewer decomposed-tar RSI_Scorpius out/r6-scorpius-compare.tar
./target/debug/scviewer decomposed-tar AEGS_Gladius out/r6-gladius-compare.tar
./target/debug/scviewer decomposed-tar DRAK_Cutlass_Black out/r6-cutlass-compare.tar
python3 -I scripts/verify_pose_reference.py sidecars out/dec-scorpius out/r6-scorpius-compare.tar
python3 -I scripts/verify_pose_reference.py sidecars out/dec-r5 out/r6-gladius-compare.tar
python3 -I scripts/verify_pose_reference.py sidecars out/dec-cutlass_black out/r6-cutlass-compare.tar
```

Both reference exports reported cache misses. Scorpius: 69,281,972 bytes;
Gladius: 67,251,408 bytes. Read directly from the Scorpius GLB's parent-local
node matrices, before the scene wrapper:

- `BONE_Front_Base_Arm` position: `[-3.4914828e-7, 1.0088683367,
  -0.0380002595]` metres; distance from bind `1.19267483e-7` metres.
- `BONE_Front_Mid_Arm` rotation, native Y-up `wxyz`:
  `[0.2190432486, -0.2190431916, 0.6723240781, 0.6723246756]`;
  identical extracted bind quaternion, numerical angular error `1.70754729e-6°`.

Verification results:

- Debug workspace build succeeded. Workspace tests: **32 passed, 0 failed**
  (FFI 5, C smoke 1, C cancellation 1, CLI 2, core 23).
- Upstream `starbreaker-3d`: **857 passed, 0 failed, 31 ignored** across unit,
  integration and doc tests. Unit suite: **797 passed, 3 ignored**, including
  both new tests. Existing fixture-dependent ignored tests were not enabled;
  some existing integration tests also skip when their fixtures are absent.
- All fresh comparison exports reported cache misses. Byte comparison:
  **Scorpius 69/69, Gladius 63/63, Cutlass Black 40/40 identical; 0 changed**.
  The original 172 files also still match their pre-fix SHA-256 snapshot.
  `out/dec-scorpius`, `out/dec-r5`, and `out/dec-cutlass_black` were not regenerated.
- `git diff --check` and `git -C vendor/StarBreaker diff --check` passed.
  Shared `target/` ended at 11 GiB, below the approximately 12 GiB sweep trigger;
  the empty incremental directory was removed. Build outputs remain gitignored.

Changed files under `vendor/StarBreaker` (complete list):

- `crates/starbreaker-3d/src/animation/pose.rs`
- `crates/starbreaker-3d/src/pipeline/entity_export.rs`
- `crates/starbreaker-3d/tests/dba_animation_test.rs`
- `docs/decomposed-export-contract.md`

Orchestrator handoff (not run by this Rust task):

```bash
ue/scripts/run_import.sh out/dec-scorpius -nullrhi -SCVerifyRig -SCPoseReference="$PWD/out/scorpius-default.glb" -SCPoseClip=landing_gear_extend
```

Scope deviation: per task authorization, work remains uncommitted on main and
the already checked-out `scviewer/lights` submodule branch; the orchestrator
creates/commits the WP branch and runs cross-review. No UE files were touched.
The requested viewer-CLI exports replace upstream's generic Blender re-export
workflow for this work package; the full UE pose gate remains the orchestrator's
step above.

TODO(WP-R6): Orchestrator runs the full UE pose gate with the new reference
(all bones within 5° / 1 cm), obtains cross-review, and commits the changes.

## Cross-review (Opus, 2026-11-09): merge as-is — optional follow-ups
- TODO(WP-R6): `--default-animation-tag` values enter the cache key even when pose baking is off (decomposed default), so identical output is stored under several keys; ignore the tags unless `apply_default_pose` is on, or make the flag require it.
- TODO(WP-R6): `--default-pose` is a no-op on `scviewer glb` (bundled exports already bake the pose by default); say so in the flag's help text.
- TODO(WP-R6): the bake writes the final sample directly and does not apply §14.6 `bind · (start⁻¹ · sample)` for clips with a non-identity `start_rotation` (irrelevant for gear-deploy clips whose first frame is bind; none of the three test ships has such a clip).
- TODO(WP-R6): `dba_animation_test.rs` round-trip test is `#[ignore]` (needs a fixture) so that path never ran in CI; add a synthetic fixture.
