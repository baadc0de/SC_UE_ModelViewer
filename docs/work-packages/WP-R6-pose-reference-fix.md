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
