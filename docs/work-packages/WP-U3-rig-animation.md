# WP-U3 — Rig, animation and lights in Unreal

**Agent:** Codex. **Depends on:** WP-U1 (importer), WP-R2/WP-R3 (clip semantics),
decomposed `animations/*.json` sidecars. **Build lock:** no cargo.

## Goal
Animated ships in UE: skeleton from the CHR bones, soft-skinned parts as a
skeletal mesh, rigid parts attached to sockets, every clip as an `UAnimSequence`,
Mannequin tags in a catalogue asset, interior lights as light components.

## Read first
`vendor/StarBreaker/docs/star-citizen-animation-formats-whitepaper.md` §13–§14
(axis mapping, LINEAR keys, hemisphere alignment, `start_rotation` anchor,
duplicate-hash routing) and §Animation Records of the export contract (sidecar JSON is
**Blender Z-up, `[w,x,y,z]`**, bone keys are CRC32 hex, `source_node_name` present
when resolved, arrays for hash collisions).

All builders live in the **Runtime** module and create transient objects (the
editor host's Keep persists them); nothing may require editor-only modules unless
guarded by `WITH_EDITOR` with a runtime path kept working.

## Deliverables
- `FSCSkeletonBuilder`: bones from the exported skeleton (scene.json / mesh GLB
  skins) → `USkeleton` + reference pose (bind pose per §4.4). Decide and document
  whether to read bones from the GLB `skins[]` or add a `skeleton.json` sidecar
  (if the latter, file `TODO(WP-R1)` with the exact shape).
- `FSCSkeletalMeshImporter`: soft-skinned submeshes (`JOINTS_0/WEIGHTS_0` in the
  GLB) → `USkeletalMesh`; Nanite optional (experimental) behind an import option.
- Rigid parts: keep WP-U1 static meshes, attach each to a socket named after its
  owning bone (`node_parent_index` / NMC owner) so clips move them.
- `FSCAnimImporter`: sidecar → `UAnimSequence` at clip fps, linear keys, convert
  Blender Z-up wxyz → UE (document the exact matrix; verify a deploy clip ends in
  the same pose as `apply_default_animation_pose` export), hemisphere-align,
  compose `bind · (start⁻¹ · sample)` when `start_rotation` is present, resolve
  hash-collision arrays by provenance (§14.8).
- `USCClipCatalog` DataAsset: clip id, display name, owner, source, fragments/tags,
  boomerang flag; `ASCVehicle` gets `PlayClip(FName)`, `SnapClip(FName, bLast)`,
  `PlayFragment(FName fragment, FName tag)` Blueprint-callable functions.
- Lights (scene schema v2 from WP-R5): `root_entity.lights[]` and `children[].lights[]`
  (the ship's own SOC light groups resolved to attachment helpers; Gladius 21 root +
  56 interior lights, Cutlass Black 225 total) **and** `interiors[].lights` → point/spot/rect components with
  `intensity_candela_proxy`, `radius_m`, colour, half-angles, gobo texture as light
  function; `ambient_proxy` skipped with a log line.

## Done criteria
- Scorpius: `PlayFragment(Landing_Gear, Deploy)` animates the gear in PIE; snap-last
  pose matches the deployed-pose export within 5°.
- Gladius gear + canopy play together; rigid gear struts follow their bones.
- Cutlass Black interior lights appear in the `Viewer` level with plausible intensity (a
  lit corridor, not a blown-out one); doors animate.

## Implementation findings / deviations (2026-11-09)

See `ue/verification/WP-U3.md` for commands, measured results, and remaining gates.

- GLB node hierarchies plus skins are sufficient for the supplied packages. The
  importer verifies that each skin inverse bind composed with its joint's node
  world transform equals the mesh node's bind transform. No skeleton sidecar is
  needed for these inputs. Duplicate identity children emitted as skin anchors
  retain distinct UE bones but do not receive the owner's animation twice.
- The whitepaper and current implementation disagree about animation locals.
  `animation/pose.rs::clip_final_pose` writes the sidecar-convention quaternion
  directly into exported node locals, beneath the existing GLB wrapper. The
  Blender addon likewise assigns absolute sampled rotations. Applying another
  Y-up conversion or multiplying identity-root-metadata samples by every bone's
  bind rotation does not reproduce the supplied/default-pose exports. U3 converts
  node-local samples with the same matrix as GLB node locals; non-identity
  `start_rotation` uses `bind * inverse(start) * sample`, while identity or absent
  clip-root anchors use absolute samples. This is a documented deviation from
  the brief's blanket “when present” wording, backed by the reference comparison.
- **Full-pose reference acceptance is blocked by an exporter discrepancy.** The
  Scorpius gear clip has position-only `BONE_Front_Mid_Arm`; the default-pose
  exporter replaces its unanimated rotation with identity. The importer keeps
  its bind rotation. All authored rotation tracks match, but the complete
  compared pose differs by 154.694311 degrees on that bone. Do not describe the
  full 5-degree acceptance gate as passed.
- Each catalogue entry has a sequence. Clips with no receiver in the equipped
  package retain an empty metadata-only sequence and a warning, and cannot be
  played. Counts are reported separately from clips bound to actual bones.
- The old empty `Source/SCBridgeLib/SCBridgeLib.Build.cs` stub was removed because
  WP-R4 supplies that module under `Source/ThirdParty/SCBridgeLib`; UBT otherwise
  reports duplicate classes. No writes were made to the WP-R4 folder.

TODO(WP-R6): in `animation/pose.rs`, preserve the reference rotation for channels
without rotation keys instead of synthesizing identity in `clip_final_pose` and
applying it unconditionally in `apply_pose_to_skeleton`; regenerate the Scorpius
`apply_default_animation_pose=true` reference, then rerun the unmodified full-pose
comparison. No new sidecar field is necessary: `has_rotation` already expresses
this distinction.

TODO(WP-U3): rerun full-pose acceptance after the WP-R6 reference correction; the
current reference command intentionally returns failure rather than hiding the
position-only bone's rotation mismatch.

TODO(WP-U3): orchestrator must run the GPU checks listed in the verification
record (PIE/game playback, simultaneous clips, socket-following rigid geometry,
soft skin rendering, lit corridors, projector roll/gobos, Nanite/Lumen parity).
