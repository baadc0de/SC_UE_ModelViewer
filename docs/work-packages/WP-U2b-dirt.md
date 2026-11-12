# WP-U2b — Dirt layer

Branch `wp/u2b-dirt`. Follows WP-U2 (static wear) and the WP-L1 finding that dirt is per-instance state.

## The game's rule (decoded `Engine/ShaderCache_Vulkan.pak`; scratch, not in git)

- `vertexLib.cfi` (`ENABLE_DIRT`): the instance dirt is a two-band spherical harmonic over the object-space normal,
  `OUT.dirtAmount = SH2_SampleValue_Clamped(normalize(objectNormal * MeshInvSquareNormalizedBounds), instance.dirtAmountSH)`.
- `HardSurface.cfx` `GetLayerAttribs`, after the macro surface and before `Albedo *= AO; Reflectance *= AO`:
  `if (objectDirt) { dirt = saturate(objectDirt - CalculateVertexDirt(vertex wear)); dirt = CalcDirtBlendAlpha(
  instData.dirtColor, dirt, params.blendMap /* WearBlendMap.r */, vertexAO); LayerAttribs_ApplyDirt(...) }`;
  `GetMaterialAttribs`: `Emissive *= 1 - dirtAmount`.
- `LayerBlend_V2.cfx`: the same with the wear map, the `%WEAR_DIRT_AO_VERTEX` vertex dirt and the blend detail maps
  (`GetAlphaBlendDetailMaps` → `dirtDetail`, `bAddDetails`).
- `layerLib.cfi`: `CalculateVertexDirt(v) = 0.95 v²`; `CalcDirtBlendAlpha`: `mask = blendMap * AO²`,
  `a = CalcBlendAlpha(mask, amount, 1)`, `slope = 1 - (1 - a)²`,
  `alpha = lerp(slope, a, InverseLerpNormalized(0, 0.15, GetLuminance(dirtColor)))`, then `AddBlendAlphaDetails`;
  `LayerAttribs_ApplyDirt`: `Albedo = lerp(Albedo, dirtColor, a)`, `Reflectance = lerp(Reflectance, DIRT_SPECULAR, a)`,
  `Smoothness = lerp(Smoothness, DIRT_GLOSS, a)`.
- `common.cfi`: `DIRT_SPECULAR = 0.03`, `DIRT_GLOSS = 0.0`, `GetLuminance` = Rec.709 (0.2126, 0.7152, 0.0722).

## Data sources (DataCore 4.10.2, all records exported as JSON)

- Amount: every entity's render parameters carry `Dirt` (beside `Interference`, `Damage`). 105 871 are 0.0; the
  non-zero ones (0.1 ×1328, up to 1.0) are carryables, lootables, NPC archetypes, interactables — **no spaceship
  authors dirt**. Loadout `DirtRange` is null in every record. Object-container modifiers carry
  `InteriorDirtFactor` / `ExteriorDirtFactor` = 0.0.
- Colour: **no DataCore field, no P4k config** names a dirt colour; `instData.dirtColor` is supplied by the engine
  at run time (where the ship has been). The direction-dependent amount (`dirtAmountSH`) is also run-time state.

Consequence: a ship exported as data renders clean in the game too. The dirt in in-game references of flown ships
cannot be reproduced from game data: drawing it needs a colour and an amount chosen by the viewer user.

## Decision (owner, 2026-11-12)

Made-up values are allowed; choose them against in-game footage. Implemented:

- `create_materials.py` `Graph.dirt` / `DIRT_ALPHA`: the rule above in `M_SC_HardSurface` (blend map = wear blend
  map, vertex dirt from vertex wear, AO = vertex AO) and `M_SC_Layered` (this master samples no wear map and no vertex
  dirt: blend map 1, vertex dirt 0, AO = HAL AO); identity at amount 0; emission × (1 − dirt).
- `MPC_SCDirt` (DirtAmount, DirtColor) set by `FSCDirt::Push` from `USCViewerSettings::DirtAmount` / `DirtColor`
  (saved per user); the amount is uniform (the SH's constant band only).
- Exterior only: `FSCDirt::MarkExterior` writes custom primitive data slot 0 = 1 on components outside every room
  (WP-L1 lighting channel 0), 0 inside — the game's object-container template sets `InteriorDirtFactor` 0.
- Viewer: options panel "Dirt" slider (0–100 %) and presets Dust / Sand / Mud / Ash; `sc.Dirt <amount> [preset]`.
  Defaults 30 % Dust, picked by eye against the owner's Gladius references (`ue/verification/reference/gladius/`)
  and community footage descriptions of flown ships; not game data.

Open: `M_SC_MeshDecal` has no dirt (the game's `MeshDecal.cfx` has its own), so decals stay crisp on dirty paint;
the Layered master's wear map and vertex dirt are not ported; the game's direction-dependent dirt (SH) is uniform here.
