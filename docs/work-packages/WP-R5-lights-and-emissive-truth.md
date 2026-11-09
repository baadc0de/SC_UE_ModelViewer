# WP-R5 — Vehicle lights and emissive source of truth (exporter side)

**Agent:** Codex. **Depends on:** WP-R1 (exporter path, sidecars), WP-R3 landed.
**Build lock:** cargo. **Lands in:** `vendor/StarBreaker` on a `scviewer/lights` branch
(stacked on `scviewer/clip-catalogue`) plus `scviewer-core`.

## Why
The owner requires that emissive surfaces, decals and lights are derived from the
game's own data for every model, never from mesh names or per-asset tweaks. The
orchestrator verified on 2026-11-09 (Gladius, LIVE 4.10.2) where that data is:

| Visible thing | Source of truth | Status in the decomposed package |
| --- | --- | --- |
| Blue wing-tip / under-wing strips, headlight glow | `.mtl` Illum submaterial attributes `Emissive` (RGB) × `Glow` (scalar) × (`Diffuse` × base-colour texture); e.g. "emissives": Diffuse 0.212,0.597,0.922, Emissive 1,1,1, Glow 0.3, `gladius_emissives_diff` | present in `authored_attributes`, but the curated `emissive_factor()` in `mtl.rs` is wrong (see below) |
| Panel-line grooves | MeshDecal "pom": `%PARALLAX_OCCLUSION_MAPPING`, TexSlot1 coverage, TexSlot3 DDNA, TexSlot4 height, `PomDisplacement`, `PomHeightBias` | present (slots + flags + public params) |
| Markings, warning tape | MeshDecal with `%DIFFUSE_MAP` colour sheet (`gladius_text_diff`) | present |
| Roundels / stencils | MeshDecal with `%STENCIL_MAP` + `$TintPaletteDecal` → palette `decal` channels | present |
| Canopy tint | GlassPBR via palette `glass` channel | present |
| Nav lights, headlights, landing lights, cockpit lights | **not materials**: the vehicle XML (`Data/Scripts/Entities/Vehicles/Implementations/Xml/<ship>.xml`) declares an ItemPort `hardpoint_lights_ext` of type `MultiLight` with `<AttachmentPoint name="ext_Body_Lights" bone="Body"/>` etc.; the equipped light item (DataCore `LightGroup*` / light item params) defines colour, intensity, radius, style and which attachment point each light uses | **missing**: StarBreaker exports lights only from socpak interiors |

Shader sources are not shipped in `Data.p4k` (no `.cfx`/`.cfi`), so the material
semantics come from CryEngine's documented Illum/MeshDecal behaviour plus the
authored attributes; executable datamining is not needed for this package.

## Tasks
1. **Fix `SubMaterial::emissive_factor` upstream** (`crates/starbreaker-3d/src/mtl.rs`):
   emission is `Emissive × Glow`; a missing or zero `Glow` means no emission (today a
   missing Glow falls back to 1.0, which would make every `LayerBlend_V2` submaterial
   with `Emissive=1,1,1` glow white). Keep `is_engine_glow_material` consistent. Add
   a unit test with an authored-attribute fixture. Expose in the sidecar a curated
   `emissive` object per submaterial: `color` (Emissive×Glow×Diffuse), `uses_base_texture`
   (true unless an emittance map exists), `emittance_texture` (slot ref or null),
   `alpha_glow_cutoff`, `glow` (raw scalar) — so importers need no attribute parsing.
2. **Decal semantics in the sidecar**: add `decal` object per MeshDecal submaterial:
   `kind` = `relief_only` (POM, no colour signal) | `color_sheet` | `stencil_palette`
   | `unknown`, with the slot refs it uses and the relevant public params
   (`DecalAlphaMultiplier`, `DecalDiffuseOpacity`, `DecalFalloff`, `PomDisplacement`,
   `PomHeightBias`, `Stencil*`). Mirror the rules the Blender addon already applies
   (`docs/blender-material-contract-naming-rules.md`, decomposed contract "Blender
   importer note") so the two importers agree.
3. **Vehicle lights export**: resolve the `MultiLight` port(s) from the vehicle XML
   and the entity's loadout (which light item is equipped; DataCore `LightGroup`,
   `LightGroupItem`, `LightGroupPoweredItem` classes of type `Light` are the generic
   ones — find the ship-specific one and its params struct), the attachment points
   (`AttachmentPoint name/bone` → the NMC helper nodes of the same name in the
   `.cga`), and emit `root_entity.lights[]` / `children[].lights[]` using the same
   light record shape as `interiors[].lights` (semantic kind, `direction_sc`,
   `intensity_candela_proxy`, `radius_m`, colour, states, light_style). Expose
   `scviewer-core` and `scviewer-cli lights <entity>`.
4. Contract doc: document the `emissive` and `decal` objects and the vehicle
   `lights[]`; bump the sidecar/scene version field as the contract requires.

## Done criteria
- Gladius sidecar: "emissives" → `emissive.color` ≈ (0.064, 0.179, 0.277) (blue × 0.3),
  "pipe mat 1" (LayerBlend_V2, Emissive 1,1,1, no Glow) → `emissive.color` = 0;
  "pom" → `decal.kind = relief_only`; "decals" → `color_sheet`; "stencil" →
  `stencil_palette`. Unit tests on synthetic attribute fixtures, not game counts.
- `scviewer lights Gladius` lists the external light groups with positions resolved
  to the attachment-point helpers (nose, body, wings, cockpit) and their colours;
  Cutlass Black and Scorpius too; an entity without lights → empty list.
- `cargo test --workspace` + upstream tests green; decomposed counts unchanged
  except the new fields; parity between exporter and catalogue preserved.
