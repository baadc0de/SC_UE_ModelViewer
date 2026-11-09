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
- Review corrected the clip basis: undo the exporter's native Y-up -> Blender Z-up
  swap before applying the GLB -> UE transform. Sidecar `(bx,by,bz)` becomes
  `100*(by,bx,bz)` cm; sidecar `[w,x,y,z]` becomes UE `FQuat(-y,-x,-z,w)`.
  The GLB wrapper remains a bone and is applied once. The previous default-pose
  export agreement was circular: that exporter made the same basis mistake.
- Non-identity `start_rotation` retains `bind * inverse(start) * sample`;
  identity/absent clip-root anchors use absolute parent-local rotations, matching
  the addon source decoder. Unkeyed channels keep bind (whitepaper §4.4), while
  sparse clips compose onto the current pose (§14.6).
- Cyclic clips use per-bone `bind + (sample - first)` for positions, like the
  addon. A moving clip is cyclic when all authored channels return to their
  first value (0.1 degrees / 0.01 cm); names and clip-root `start_position` are
  not used. Snap-last remains the literal last frame, not a guessed midpoint.
- Full-pose reference acceptance remains pending WP-R6. Source-based tests use
  the original GLBs and sidecars independently of `clip_final_pose`, check
  translations, and separately confirm the stable bind endpoints and CableTop
  rotation. The quarantined reference check also reports translations now.
- Only assets receiving clip channels create rigs and split by bone owner.
  Others retain U1/U2's static meshes. Collision derivation is checked on a
  worker before material splitting; a failure skips that owner with a warning.
- Finished playback leaves the active list; bounded held layers preserve sparse
  overlap order without idle evaluation. Only changed clip receivers refresh.
  Fragment options use their weights and compete for overlapping scopes;
  disjoint scopes can coexist. `ToggleClip(fragment, openTag=Open, closeTag=Close)`
  alternates caller-selected tags for WP-P1's interact action.
- Each catalogue entry has a sequence. Clips with no receiver in the equipped
  package retain an empty metadata-only sequence and a warning, and cannot be
  played. Counts are reported separately from clips bound to actual bones.
- The old empty `Source/SCBridgeLib/SCBridgeLib.Build.cs` stub was removed because
  WP-R4 supplies that module under `Source/ThirdParty/SCBridgeLib`; UBT otherwise
  reports duplicate classes. No writes were made to the WP-R4 folder.

TODO(WP-R6): upstream animation/pose.rs must (a) make BonePose.rotation an Option built from ch.rotations.last() and have apply_pose_to_skeleton set it only when Some, and (b) bake into skeleton bones in native order without the axis swap (rotation [w,x,y,z] from [x,y,z,w], position unchanged), keeping cry_xyzw_to_blender_wxyz for the sidecar only; then regenerate the reference.

TODO(WP-U3): rerun full-pose acceptance after the WP-R6 reference correction; the
current reference command intentionally returns failure rather than hiding the
exporter's basis and unkeyed-channel discrepancies.

TODO(WP-U3): orchestrator must run the GPU checks listed in the verification
record (PIE/game playback, simultaneous clips, socket-following rigid geometry,
soft skin rendering, lit corridors, projector roll/gobos, Nanite/Lumen parity).
