# WP-U2c — Illum vertex colours: verification

Branch `wp/u2c-illum-vertex-colours`. UE 5.9.0 preview-1, Vulkan SM6, RTX 4090; packages `out/dec-p3-*` and the
viewer's own exports.

## Rule (decoded `Illum.cfx`, scratch, not in git)

Under `%VERTCOLORS`: `layer.Albedo *= Vert.Color.rgb` — `.bbb` (the AO channel) under `%USE_DAMAGE_MAP`, `.aaa` under
`WIND_BENDING_MAIN/DETAIL`, `%TRANSMITTANCE_LIGHTING`, `%OCEAN_SWAY`. The glow colour is taken from the albedo before
that multiply and masked by `vertGlowMask` = `.rgb` / `.ggg` (damage map) / `.aaa`. The blending opacity is
`× Vert.Color.a` unless `%BLENDLAYER` (after the alpha test, so it does not move the clip).

The three ships carry 382 Illum submaterials with `%VERTCOLORS` (199 with `%USE_DAMAGE_MAP`, none with the wind /
transmittance / ocean tokens); the masters ignored vertex colour on all of them, and Illum decals took MeshDecal's
vertex wear instead.

## Change

- `FSCMaterialBuilder::Describe`: `VertexColorMode` (0 none, 1 rgb, 2 damage map, 3 alpha) and `UseVertexAlpha` for every
  Illum surface on whichever master it lands (Illum / Translucent / MeshDecal / EmissiveUnlit; not the relief decal);
  Illum decals no longer set MeshDecal's `UseVertexColors`.
- `create_materials.py` `Graph.vertex_illum` (`VERTEX_ALBEDO`, `VERTEX_GLOW`): applied in `M_SC_Illum` / `M_SC_Fallback`,
  `M_SC_Translucent`, `M_SC_MeshDecal` and `M_SC_Emissive_Unlit`. The importer already counters the static-mesh
  builder's sRGB vertex-colour packing, so the node reads the game's raw values.

## Tests

`FSCMaterialsFamilies` (failed first on all six new assertions, including the Illum decal's MeshDecal vertex wear):
mode 1 for `illum_spec`, 2 for `emissives_like` (damage map), 0 without the token, `UseVertexAlpha`, and the Illum
decal's mode with `UseVertexColors` 0. UE unit 37/37; importer + Cutlass package 39/39; rig OK on the Gladius and
Scorpius.

## GPU

`cutlass-ramp-to-cockpit` (`vc-cutlass`, 81 steps) against `l1-cutlass`: `out/smoke/vc-before-after-cutlass.jpg`. The
visible change is the glow cards: the Cutlass `Decals_INT` strip lights (Illum `%DECAL` + `%VERTCOLORS`, mode 1) are
tinted orange by their vertex colours where they were white; surface albedo changes are slight. Mean grey is within
±2 / 255 (the cargo-bay and corridor shots land ~20 cm apart, so their RMSE is framing, not shading). Not compared
with an in-game capture.

## Ambient occlusion end to end (2026-11-12 survey)

- Material AO (GBuffer, darkens all light): HardSurface vertex B × `MacroAOMap` (TexSlot1, `%MACRO_AO_MAP`);
  LayerBlend HAL G × vertex B (`%WEAR_DIRT_AO_VERTEX`); Illum vertex rgb / blue (this WP). Ported as albedo and F0 × AO,
  like the game's `Albedo *= AO; Reflectance *= AO`.
- Screen space: the game's `DirOcclusionPS` (SSDO, bent normal + occlusion in `DirectionalOccRT`), used by
  `TiledShading.cfx` for ambient (`r_ssdoAmountAmbient`), reflections (`r_ssdoAmountReflection`) and the direct light
  of non-shadow-casting lights and the sun (`r_ssdoAmountDirect`). UE: Lumen's short-range AO covers the indirect
  part; MegaLights traces only shadow-casting lights and UE's contact shadows are not on its path, so since WP-L1 the
  unshadowed lights have no occlusion here. Open.
