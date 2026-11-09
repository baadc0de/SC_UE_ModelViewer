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

## Implementation evidence and deviations (2026-11-09)

LIVE 4.10.2 does **not** equip a MultiLight item on Gladius's
`hardpoint_lights_ext`. The raw `SEntityComponentDefaultLoadoutParams` contains
`Controller_Light` on `hardpoint_controller_light`, and no MultiLight entry.
Querying `Components[SLightGroupComponentParams]` on the ship returns no value;
`SCItemMultiLightParams` does not exist in this build's schema. The ship's
`VehicleComponentParams.objectContainers` instead references
`objectcontainers/ships/aegs/gladius/base_lightgroups.socpak`.

That SOC contains the actual ship-specific `LightGroup` instances. Their
`PropertiesDataCore/EntityComponentLightGroup` has type
`SLightGroupComponentParams` and authored `helper` values. The separate direct
`EntityComponentLightGroup/BakedInLights/Light` blocks contain
`EntityComponentLight` (`LightComponentParams`) with `LightSizeParams`,
`LightStateParams_*`, `LightProjectorParams`, etc. `TileAttachmentParams` preserves
the attachment-point names inside `ItemPortName` brackets. This is the source
used by the implementation, rather than inventing a missing DataCore light item.
The XML port's five attachment points are exported as metadata and used when a
SOC group lacks an explicit helper. Explicit SOC helpers take precedence: for
example the wing groups author `WingLeft_Tip` / `WingRight_Tip`, and the headlight
point `ext_Headlights_Lights` is absent from the older XML port list while its SOC
helper `Nose` resolves. Cockpit lights use the container's `interior_cockpit`
attachment, not an inferred `helper_light_int_cockpit_lights` binding.

The baseline exporter already parsed 77 Gladius lights (54 in
`base_int_all_main`, 23 in `base_lightgroups`). WP-R5 exposes their provenance and
resolved attachment transforms, moves resolved groups to the owning entity's
`lights[]`, and retains other lights in interior containers without duplication.
The CLI catalogue includes both. Synthetic tests also caught and removed the
fabricated white-light fallback for empty groups, and fixed component-backed
light classes being skipped by a fixed class-name allowlist.

Observed catalogue totals: Gladius **77**, Scorpius **127**, Cutlass Black **225**,
Controller_Light **0**. Resolved per-group NMC attachments: **21**, **94**, **103**
respectively; remaining lights retain their authored container transforms.
Gladius package counts remain **502 files, 45 GLBs, 23 material sidecars,
366 PNGs, 51 child instances, 3 interior containers, 63 animation clips**.
Its lights are now 21 root lights plus 56 interior lights, still 77 total.

The Gladius material evidence is `emissives.color =
[0.063669242, 0.179160565, 0.276474595]`, `glow = 0.300000012`;
`pipe mat 1` and `Headlight_Glow` have zero emission; `pom = relief_only`,
`decals = color_sheet`, `stencil = stencil_palette`. POM retains displacement
`0.031615101` and height bias `0.5`. These observations are acceptance evidence,
not constants or assertions on game-specific counts in the production code.

Scene/material schemas are v2, with raw fields preserved. Core cache format v4
invalidates old package results, including earlier dirty builds of the pinned
revision. Tests and debug builds use the shared target directory. No release
build, UE edit, git metadata write, branch switch or commit was performed; the
orchestrator will branch the existing `scviewer/clip-catalogue` work as
`scviewer/lights`.

TODO(WP-R5): Audit the pre-existing SOC candela-proxy constants, projector inner-angle ratio and sparse-component defaults against source data in a separate lighting-units change; WP-R5 preserves the documented cross-DCC conversion for parity and removes only the invented empty-group light.
TODO(WP-R5): The pre-existing general texture-role classifier consulted for Blender decal parity still uses texture-name heuristics. Replace those with verified family/slot rules in a separate material-contract change; the new emissive selection and engine-glow gate do not use asset names.

### Verification commands and results

Commands run from the superproject root (all generated outputs remain ignored):

```sh
CARGO_TARGET_DIR=./target cargo build
CARGO_TARGET_DIR=./target cargo test --workspace -- --test-threads=1 --nocapture
CARGO_HOME=/tmp/scviewer-r5-cargo CARGO_TARGET_DIR=./target cargo test --manifest-path vendor/StarBreaker/Cargo.toml --workspace -- --test-threads=1
CARGO_HOME=/tmp/scviewer-r5-cargo CARGO_TARGET_DIR=./target cargo build --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker
CARGO_TARGET_DIR=./target cargo fmt -p scviewer-core -p scviewer-cli --check

target/debug/scviewer --no-cache lights Gladius > out/wp-r5/gladius-lights.json
target/debug/scviewer --no-cache lights Scorpius > out/wp-r5/scorpius-lights.json
target/debug/scviewer --no-cache lights Cutlass_Black > out/wp-r5/cutlass-lights.json
target/debug/scviewer --no-cache lights Controller_Light > out/wp-r5/empty-lights.json
target/debug/scviewer --no-cache decomposed Gladius out/wp-r5/after
python3 -I scripts/verify_wp_r5.py out/wp-r5/before out/wp-r5/after
target/debug/scviewer validate out/wp-r5/after
```

`before` was exported with the unchanged binary at the start of the session:
`target/debug/scviewer --no-cache decomposed Gladius out/wp-r5/before`.
The full upstream workspace initially could not unpack `anes` into the read-only
home Cargo registry. The successful rerun used a scratch Cargo home, preserving
the installed linker/profile configuration, linking existing immutable crate
sources, and copying the registry index. It still used the shared `./target`.

Results: viewer workspace **17 passed, 0 failed**; full upstream workspace
**2,129 passed, 0 failed, 61 ignored** across 102 test/doc-test invocations.
The focused `starbreaker-3d --lib` run also passed **777 tests, 3 ignored**.
Live tests verified clip catalogue parity (63 / 69 / 40 clips), light catalogue
parity (77 / 127 / 225 lights), exact-once source IDs, colours/states/radii, and
entity/parent-local positions for attached lights. A dynamically discovered
geometryless entity (`freeze`) returned zero lights as well as zero clips.

The acceptance script checked every exported material's emission against its
own authored attributes, all GLB topology counts against the baseline, the WP's
named material evidence, and light uniqueness/count conservation. Root GLB:
274 nodes, 89 meshes, 27 materials, 395 accessors. Package validation returned
`is_valid: true`, 45 meshes, zero empty meshes, no errors or warnings. That
existing validator counts native mesh-file lights (zero for these GLBs); the
manifest light inventory is checked by the acceptance script and live parity test.

Logs: `out/wp-r5/{build,upstream-build,workspace-tests,upstream-workspace-tests,
upstream-3d,acceptance,validate}.log`. Red-before-green regression evidence is in
`emissive-red.log` and `lights-red.log`: missing Glow previously produced nonzero
emission, empty groups invented a light, and LightGroupItem components were
ignored. Tests use synthetic CryXmlB authored-attribute/component fixtures.
