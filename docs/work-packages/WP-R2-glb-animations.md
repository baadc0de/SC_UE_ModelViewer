# WP-R2 — Vehicle clips into GLB animations *(optional — upstream contribution)*

**Agent:** Codex. **Depends on:** WP-R1. **Build lock:** required.
**Priority:** low. The UE viewer reads clips from the decomposed sidecars, not from
GLB animations; do this only when the critical path (R4, U1, U3, V1, V2) is idle.
It is still a valuable upstream fix for glTF consumers.
**Lands in:** `vendor/StarBreaker` on branch `scviewer/glb-vehicle-animations`
(submodule pointer bumped in the superproject). Keep the patch upstreamable.

## Problem
`vendor/StarBreaker/crates/starbreaker-3d/src/pipeline/glb_assembly.rs` builds
`GlbInput { animations: Vec::new(), .. }` for entity exports (two sites, ~L477 and
~L521). `gltf/animation.rs::add_animations` already emits glTF animations from
`clip::Clip` (bones matched to nodes by lower-cased name, linear sampling); the
character path uses it. Vehicles therefore export 0 animations in bundled GLB while
the decomposed path serialises the same clips to `animations/*.json`.

## Task
1. Factor the clip discovery used by `decomposed.rs` (root skeleton `.chrparams` →
   `$TracksDatabase` `.dba` + standalone `.caf`; child entities' skeletons; interior
   per-entity clips) into a reusable function in `pipeline/` (e.g.
   `collect_entity_clips(...) -> Vec<clip::Clip>`) that returns clips with bone
   tracks **by node name** (`animation/to_clip.rs::shared_clip` already maps CRC32
   hashes to names given the skeleton's bone list).
2. Call it from both GLB sites when `ExportOptions::include_animations` is set
   (make the CLI flag `--animations` default on for `entity export`).
3. Clip naming: `<clip>` for root-skeleton clips, `<entity_stem>/<clip>` for child
   skeleton clips (mirror `decomposed.rs::interior_clip_key`), so the viewer can
   group them. Put `fps`, `frame_count`, source (`chrparams` | `dba` | `caf`) and
   Mannequin fragment tags into the glTF animation `extras`.
4. Coordinates: samples stay in CryEngine Y-up like the geometry under the
   `CryEngine_Z_up` wrapper node — do **not** apply the Blender per-sample swap
   that the decomposed sidecar applies. Hemisphere-align consecutive quaternions.
5. Duplicate bone names across sub-rigs (front/rear gear): a track must bind to the
   node inside the same child entity's subtree, not the first global name match.
   Add a test on `RSI_Scorpius` or a synthetic fixture (whitepaper §14.8).

## Done criteria
- `scripts/starbreaker.sh entity export Gladius out/gladius.glb` →
  `python3 -I scripts/glbinfo.py out/gladius.glb` lists `animations ≥ 4` including a
  landing-gear clip; Scorpius lists gear, canopy and wing clips.
- Loading the GLB with the `gltf` crate and sampling the gear clip at its
  last key moves the gear bones; no node receives tracks from two clips of the same
  name in different children.
- `cargo test -p starbreaker-3d` green; new tests follow upstream's TDD rule
  (failing test first). `//!` headers updated. No `Co-Authored-By` trailers on
  commits inside `vendor/StarBreaker` (upstream rule).
