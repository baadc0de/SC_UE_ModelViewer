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
- Lights: `interiors[].lights` → point/spot/rect components with
  `intensity_candela_proxy`, `radius_m`, colour, half-angles, gobo texture as light
  function; `ambient_proxy` skipped with a log line.

## Done criteria
- Scorpius: `PlayFragment(Landing_Gear, Deploy)` animates the gear in PIE; snap-last
  pose matches the deployed-pose export within 5°.
- Gladius gear + canopy play together; rigid gear struts follow their bones.
- Cutlass Black interior lights appear in `LumenTest` with plausible intensity (a
  lit corridor, not a blown-out one); doors animate.
