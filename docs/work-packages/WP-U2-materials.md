# WP-U2 — Materials for Unreal

**Agent:** Opus 5.5. **Depends on:** WP-U1. **Build lock:** no cargo.

## Goal
Material Instances that look like the game: per-shader-family master materials in
`ue/Plugins/SCBridge/Content/Materials/`, an instance builder driven by the
`*.materials.json` sidecars, and textures imported with correct flags.

## Read first
`vendor/StarBreaker/docs/decomposed-export-contract.md` §Material Sidecars and
§Texture Export Rules; `vendor/StarBreaker/docs/blender-material-contract-naming-rules.md`
(how the Blender addon reconstructs each family — mirror its slot semantics, not its
node graphs); `vendor/StarBreaker/crates/starbreaker-3d/src/mtl.rs` (`ShaderFamily`,
`TextureSemanticRole`, `AlphaConfig`).

## Deliverables
- Master materials: `M_SC_Illum` (opaque/masked, diffuse, DDNA normal, roughness
  from derived `*_roughness`, metallic, emissive glow with strength param),
  `M_SC_HardSurface`, `M_SC_Layered` (tint palette: up to 4 layers with
  primary/secondary/tertiary/glass colours and per-layer tiling), `M_SC_Glass`
  (translucent, non-Nanite), `M_SC_MeshDecal` (masked/translucent decal response,
  POM as parallax offset from height), `M_SC_Emissive_Unlit` (screens/holograms),
  `M_SC_Fallback`.
- `FSCMaterialBuilder`: shader family → master; texture slots by `role` → params;
  `public params` → scalars/colours; palette routing → colours from `palettes.json`
  (and `paints.json` variants exposed as a dropdown on the actor later);
  `is_double_sided`, `alpha_config` → blend mode. Unknown family → fallback with a
  warning listing the shader string.
- `FSCTextureImporter`: PNG → `UTexture2D`; sRGB only for colour roles; DDNA →
  `TC_Normalmap`, linear; roughness/metal/mask → linear grayscale/masks; no mip
  generation changes; dedupe by source path.
- Nanite rule: `Glass` and `MeshDecal` instances mark the owning component
  non-Nanite (WP-U1 reads a flag from the builder).

## Done criteria
- Gladius hull, cockpit glass, warning-tape decals, thruster glow and tint-palette
  panels read correctly side by side with the three.js viewer; emissive glow lights
  the cockpit interior under Lumen.
- `materials.json` fixtures (synthetic, checked in) drive unit tests of the
  family→master mapping in an automation test (`SCBridge.Materials.*`).
