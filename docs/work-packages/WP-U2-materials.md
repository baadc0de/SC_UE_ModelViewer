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
- `FSCMaterialBuilder` (Runtime module, creates `UMaterialInstanceDynamic` at
  runtime; constant instances only in the editor host's Keep): shader family → master; texture slots by `role` → params;
  `public params` → scalars/colours; palette routing → colours from `palettes.json`
  (and `paints.json` variants exposed as a dropdown on the actor later);
  `is_double_sided`, `alpha_config` → blend mode. Unknown family → fallback with a
  warning listing the shader string.
- `FSCTextureImporter`: PNG → `UTexture2D`; sRGB only for colour roles; DDNA →
  `TC_Normalmap`, linear; roughness/metal/mask → linear grayscale/masks; no mip
  generation changes; dedupe by source path.
- Nanite rule: `Glass` and `MeshDecal` instances mark the owning component
  non-Nanite (WP-U1 reads a flag from the builder).

## Schema v2 inputs (WP-R5 landed 2026-11-09)
Sidecars now carry curated per-submaterial `emissive` (`color` = Emissive × Glow ×
Diffuse, `uses_base_texture`, `emittance_texture`, `alpha_glow_cutoff`, `glow`) and
`decal` (`kind` relief_only | color_sheet | stencil_palette | unknown, plus the slots and
public params used) objects; `out/dec-r5` is the Gladius package in that schema. Switch
the builder to read these objects (fall back to attribute parsing only for v1 packages)
so both importers follow one rule; the Headlight_Glow submaterial has no Glow and is
correctly non-emissive in v2.

## Data-driven rules (owner requirement, 2026-11-09)
See `docs/03-unreal-export-design.md` §"Emissive, decal and light rules" and WP-R5.
Until WP-R5 lands, derive the same facts from the sidecar's `authored_attributes`
(`Emissive`, `Glow`, `Diffuse`), `decoded_feature_flags.tokens`, `texture_slots`
roles and `public_params` yourself; do not use upstream's `emissive_factor()` semantics
(missing `Glow` must mean no emission). Evidence table and automation checks required:
no MeshDecal primitive on an opaque material; no emission without `Glow`; the Gladius
"emissives" submaterial yields a blue emissive from its authored attributes.

## Done criteria
- Gladius hull, cockpit glass, warning-tape decals, thruster glow and tint-palette
  panels read correctly against in-game screenshots and the Blender add-on's import; emissive glow lights
  the cockpit interior under Lumen.
- `materials.json` fixtures (synthetic, checked in) drive unit tests of the
  family→master mapping in an automation test (`SCBridge.Materials.*`).

## Decision 2026-11-09 — Substrate (details: `ue/verification/WP-U2.md`)
1. Material model: **Substrate** (`r.Substrate=True`, Adaptive GBuffer, `ue/SCViewer/Config/DefaultEngine.ini`).
2. Installed 5.8.2: `r.Substrate` is not Experimental (default on for new 5.7+ projects); only UE material-layer UI and opaque rough refraction are.
3. VULKAN_SM6 supports the Adaptive GBuffer; Nanite, Lumen, HWRT and DBuffer decals have Substrate paths.
4. Measured on the RTX 4090: Viewer + Gladius render identically (Lumen overview before/after), one 8 min 26 s shader rebuild, project DDC +449 MiB.
5. SC is a specular-colour workflow: Substrate's slab takes Diffuse Albedo + F0 directly (coloured `_spec` maps, no metallic heuristic).
6. GlassPBR gets coloured transmittance and native thin film (`THIN_FILM`/`ThinFilmThickness` are authored on the canopy); legacy has neither in one model.
7. Layered tint materials are an in-graph parameter blend in either model (no Substrate layering needed).
8. Mesh decals use Substrate *Convert To Decal* (DBuffer): relief-only decals change normal/roughness, never base colour.
9. Builder and parameter names are model-neutral; a switch back costs a rewrite of `ue/scripts/create_materials.py` graphs, the ini line and a shader rebuild — no C++.
10. Risk: project-wide setting; shader/DDC rebuild after engine upgrades.

### Deviations (WP-U2)
- Masters added beyond the brief: `M_SC_MeshDecal_Relief` (control-only POM, normal/roughness), `M_SC_Translucent` (forward/faded Illum glow cards); blend/two-sided variants are constant instances (`MI_SC_*`).
- "No mip generation changes": exported PNGs have one level, so the runtime path builds a box-filtered mip chain on the worker (no engine mip settings changed).
- Emission follows the owner's rule (Glow absent/0 → none, strength = Glow × host constant `GlowToEmissive`), not `mtl.rs` `emissive_factor()`; Glow with Emissive = 0 is diffuse glow.
- `MtlFlags & 0x8000` (CryEngine forward-rendering flag) makes Illum diffuse alpha a coverage mask; to be moved into the sidecar's `alpha_config` upstream.
