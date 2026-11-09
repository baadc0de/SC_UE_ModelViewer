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
| Blue wing-tip / under-wing strips, headlight glow | `.mtl` surface colour × Emissive tint × strength; strength follows the amended Glow / forward Illum / emittance-map priority below. "emissives" has Diffuse 0.212,0.597,0.922 and Glow 0.3; forward "Headlight_Glow" has no Glow and strength 1 | curated `emissive` and `alpha_config` objects, preserving raw attributes |
| Panel-line grooves | MeshDecal "pom": `%PARALLAX_OCCLUSION_MAPPING`, TexSlot1 coverage, TexSlot3 DDNA, TexSlot4 height, `PomDisplacement`, `PomHeightBias` | present (slots + flags + public params) |
| Markings, warning tape | MeshDecal with `%DIFFUSE_MAP` colour sheet (`gladius_text_diff`) | present |
| Roundels / stencils | MeshDecal with `%STENCIL_MAP` + `$TintPaletteDecal` → palette `decal` channels | present |
| Canopy tint | GlassPBR via palette `glass` channel | present |
| Nav lights, headlights, landing lights, cockpit lights | **not materials**: the vehicle XML (`Data/Scripts/Entities/Vehicles/Implementations/Xml/<ship>.xml`) declares an ItemPort `hardpoint_lights_ext` of type `MultiLight` with `<AttachmentPoint name="ext_Body_Lights" bone="Body"/>` etc.; the equipped light item (DataCore `LightGroup*` / light item params) defines colour, intensity, radius, style and which attachment point each light uses | **missing**: StarBreaker exports lights only from socpak interiors |

Shader sources are not shipped in `Data.p4k` (no `.cfx`/`.cfi`), so the material
semantics come from CryEngine's documented Illum/MeshDecal behaviour plus the
authored attributes; executable datamining is not needed for this package.

## Tasks
1. **Amended emission rule** (`mtl.rs` / `material_semantics.rs`): strength is
   positive Glow; otherwise mean(Emissive) for forward (MtlFlags & 0x8000)
   Illum/HardSurface with non-black Emissive; otherwise 1 for an enabled emittance
   map; otherwise 0. `color` is Emissive tint, or white for black Emissive. Final
   emission is surface colour × color × strength. LayerBlend and non-forward
   materials never emit from Emissive alone. Preserve `uses_base_texture`,
   `emittance_texture`, `alpha_glow_cutoff`, raw `glow`, and add
   `forward_rendering`. `alpha_config` exposes forward/two-sided flags and
   authored AlphaTest/Opacity. Unit fixtures must be synthetic.
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
- Gladius sidecar strengths: "emissives" ≈ 0.3, "Headlight_Glow" = 1,
  "pipe mat 1" = 0, "glows" in KLWE_las_rep_s1-3 ≈ 0.35. Tint is white in
  these cases. Effective "emissives" surface RGB before texture remains
  ≈ (0.064, 0.179, 0.276);
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

The amended Gladius material evidence is `emissives.color = [1,1,1]`,
`strength = 0.300000012`, yielding surface RGB before texture
`[0.063669242, 0.179160565, 0.276474595]`. `pipe mat 1` has strength 0;
forward `Headlight_Glow` has strength 1. KLWE `glows` has strength ≈ 0.35; `pom = relief_only`,
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

## Cross-review corrections (2026-11-09)

These corrections supersede the initial task/done-criteria emission formula and
its earlier evidence above. Work continues from `scviewer/lights` at b4c0ebe6;
no git metadata, UE files, or shared design documents were modified by this agent.

1. **Previous review emission rule (forward-card exception superseded below).** `emissive.color` is tint only: authored Emissive RGB
   unless all zero, then white. `strength` is positive Glow, else 1 for an enabled
   emittance-map permutation, else 0. Surface colour remains Diffuse × base map,
   or the emittance map. Exact tokens `%EMITTANCE_MAP` and `%MACRO_EMISSIVE_MAP`
   enable the map path; a slot name/texture filename alone does not. The latter
   token is verified on live HardSurface materials with TexSlot14 (Scorpius trim
   and thrusters, Cutlass thrusters/headlight). Unit tests cover black/white
   Emissive with Glow, neutral Illum/LayerBlend without Glow, token-only maps,
   and HardSurface map binding/negative filename-only evidence. Synthetic test
   scalars are deliberately not copies of game values.
2. **Thruster control restored from DataCore.** Querying
   `EntityClassDefinition.AEGS_Gladius_Thruster_Main` found
   `SEntityComponentEffects.entityEffects.materialEffects.enabled = true`, with
   two enabled tag effects whose property links write `materialProperty = Glow`
   on `subMeshName = Gladius_Thruster_Main`. Their authored multipliers are 50/100
   and minima 10/20; these values are evidence, not constants added to code.
   `SCItemThrusterParams` and `ThrusterEffectsComponentParams` were also inspected;
   particle effects are separate. The NMC dump identifies that exact name as
   geometry node 0 (parent 1), with only MapChannel 4/5/6 metadata. The material
   `engine_glow` is Illum, `%DECAL`, Emissive zero, no Glow, AlphaGlowCutoff zero,
   DecalDiffuseOpacity zero. No texture-name signal is used. Enabled Glow links
   join exact NMC names and source material IDs. Because the authored effect
   addresses the entire submesh, Gladius has four compatible Illum targets:
   source slots 1/2/4/5 (`engine_a`, `engine_b`, `text_decals`, `engine_glow`).
   Narrowing to the formerly name-selected slot would invent an unsupported
   binding. This is a manual control; it does not simulate effect tags/modifiers
   or change static emission. Tests cover disabled/unrelated effects, exact node
   joins, original material IDs, missing nodes, and zero static emission.
3. **Blender parity.** Manifest parsing and scene orchestration consume root and
   child light arrays as well as interiors. A small helper binds per-instance
   lights to animated helper-local transforms; unresolved helpers fall back to
   the owning instance's entity-local frame. Tests verify all three locations,
   legacy absence, local rotation/direction handling, and fallback placement.
4. **Mixed light/geometry entities.** Removed the early continue after light
   extraction. The regression test failed with zero meshes before the fix and
   now retains both mesh and light. Live placement identities/transforms and all
   GLB topology/file counts are unchanged on all three ships.
5. **Optional vehicle XML.** Shared tolerant base/modification reader now serves
   both invisible-port and light-port discovery. Query/read/parse failures warn
   and allow other sources/export work to continue. A test covers missing and
   corrupt XML followed by a valid modification.
6. **Exact helpers and compatibility.** Removed suffix matching (including the
   unique-suffix case, reproduced red first). Contract documents bundled GLB
   emissive factors and screen-back-colour effects of the shared factor fix.
   Scene/material schema stays v2; validation accepts v1/v2. Core cache is now
   v5 to invalidate packages with superseded semantics. The name-derived texture
   role debt is also marked `TODO(upstream)` at its implementation in `mtl.rs`.

### Review verification

Commands from the superproject root; detailed output is in ignored
`out/wp-r5-review/`. The three `before-*` exports were made with the unchanged
binary at the start of review work, using `--no-cache decomposed` and default
LOD1/mip2. Regression-first logs: `rust-red.log` (six reproduced failures),
`helper-red.log` (unique suffix bound incorrectly), `effect-red.log` (dynamic Glow
binding missing), and `/tmp/r5-blender-red.log` (missing entity-light fields).

```sh
CARGO_TARGET_DIR=./target cargo build
CARGO_TARGET_DIR=./target cargo test --workspace
CARGO_HOME=/tmp/scviewer-r5-cargo CARGO_TARGET_DIR=./target cargo test --manifest-path vendor/StarBreaker/Cargo.toml --workspace -- --test-threads=1
CARGO_TARGET_DIR=./target cargo fmt -p scviewer-core -p scviewer-cli --check

target/debug/scviewer --no-cache lights Gladius > out/wp-r5-review/gladius-lights.json
target/debug/scviewer --no-cache lights Scorpius > out/wp-r5-review/scorpius-lights.json
target/debug/scviewer --no-cache lights Cutlass_Black > out/wp-r5-review/cutlass-lights.json
target/debug/scviewer --no-cache lights Controller_Light > out/wp-r5-review/empty-lights.json
target/debug/scviewer --no-cache decomposed Gladius out/dec-r5 --lod 1 --mip 2
target/debug/scviewer --no-cache decomposed Scorpius out/wp-r5-review/after-scorpius --lod 1 --mip 2
target/debug/scviewer --no-cache decomposed Cutlass_Black out/wp-r5-review/after-cutlass --lod 1 --mip 2
python3 -I scripts/verify_wp_r5.py out/wp-r5-review/before-gladius out/dec-r5 --expect-engine-glow present
python3 -I scripts/verify_wp_r5.py out/wp-r5-review/before-scorpius out/wp-r5-review/after-scorpius --generic --expect-engine-glow present
python3 -I scripts/verify_wp_r5.py out/wp-r5-review/before-cutlass out/wp-r5-review/after-cutlass --generic --expect-engine-glow present
target/debug/scviewer validate out/dec-r5
```

Results: debug build succeeds, viewer **17 passed**, upstream **2,139 passed,
61 ignored** across 102 invocations, no failures. Gladius validation is valid:
45 meshes, zero empty meshes, no errors/warnings (the validator's GLB light count
is zero; the scene manifest independently contains 77 lights).

| Evidence | Gladius | Scorpius | Cutlass Black |
| --- | ---: | ---: | ---: |
| Lights | 77 | 127 | 225 |
| Root / interior lights | 21 / 56 | 94 / 33 | 103 / 122 |
| Interior placements, before = after | 10 | 14 | 30 |
| Engine-glow control, before → after | absent → 4 targets | absent → 4 targets | absent → 2 targets |
| Files, before = after | 502 | 491 | 747 |

Controller_Light returns zero lights. No child light arrays contain lights for
these three exports. Gladius still has 13 positive-Glow/black-Emissive materials.
Live four-case emission evidence (surface RGB before texture modulation):

| Material | Tint | Strength | Surface RGB × strength |
| --- | --- | ---: | --- |
| weapon `glows` | 1,1,1 | 0.35 | 0.322554, 0.114722, 0 |
| root `emissives` | 1,1,1 | 0.3 | 0.063669, 0.179161, 0.276475 |
| root `Headlight_Glow` | 1,1,1 | 0 | 0,0,0 |
| `pipe mat 1` | 1,1,1 | 0 | 0,0,0 |

Synthetic unit cases use Glow 0.25 / 0.5 and surface colours (1,0.5,0) /
(0.25,0.5,1), respectively: tint stays white, factors are (0.25,0.25,0.25) /
(0.5,0.5,0.5), and final pre-texture surfaces are (0.25,0.125,0) /
(0.125,0.25,0.5). Both no-Glow cases have zero factors. The map-only test has
strength 1 and tint/factor (0.5,1,0.25).

Decals remain: `pom = relief_only`, PomDisplacement 0.031615101, PomHeightBias 0.5,
coverage/normal_gloss/height = TexSlot1/3/4; `decals = color_sheet` with base colour
TexSlot1; `stencil = stencil_palette` with TexSlot7 and numeric Stencil* fields.

Blender's ordinary system-Python discovery first reported three missing legacy
`vendor/ships/...component_master_01_TEX0.materials.json` fixture errors. Using the
freshly exported equivalent TEX2 fixture, without copying game data into git:

```sh
cd vendor/StarBreaker/blender_addon
python3 - <<'PY'
import pathlib, unittest
suite = unittest.defaultTestLoader.discover('tests')
import test_manifest, test_templates
fixture = pathlib.Path('../../../out/dec-r5/Data/Materials/vehicles/components/component_master_01_TEX2.materials.json').resolve()
test_manifest.COMPONENT_MASTER = test_templates.COMPONENT_MASTER = fixture
result = unittest.TextTestRunner(verbosity=1).run(suite)
raise SystemExit(not result.wasSuccessful())
PY
```

**426 tests run, OK, 41 skipped** (385 executed successfully). The two existing
TODO(WP-R5) lines above remain; no engine-control regression TODO is necessary
because a non-name authored binding was found and implemented.

Changed files under `vendor/StarBreaker` (including new files):

```text
blender_addon/starbreaker_addon/manifest.py
blender_addon/starbreaker_addon/runtime/importer/entity_lights.py
blender_addon/starbreaker_addon/runtime/importer/orchestration.py
blender_addon/tests/test_manifest.py
blender_addon/tests/test_scene_instances.py
crates/starbreaker-3d/src/decomposed.rs
crates/starbreaker-3d/src/material_semantics.rs
crates/starbreaker-3d/src/mtl.rs
crates/starbreaker-3d/src/pipeline/engine_glow.rs
crates/starbreaker-3d/src/pipeline/light_catalogue.rs
crates/starbreaker-3d/src/pipeline/mod.rs
crates/starbreaker-3d/src/pipeline/vehicle.rs
crates/starbreaker-3d/src/pipeline/vehicle_lights.rs
crates/starbreaker-3d/src/pipeline/vehicle_xml.rs
crates/starbreaker-3d/src/socpak.rs
docs/decomposed-export-contract.md
```

Superproject files: `crates/scviewer-core/README.md`,
`crates/scviewer-core/src/cache.rs`, `scripts/verify_wp_r5.py`, and this brief.

## Forward-card amendment and Glow join audit (2026-11-09)

This round starts at superproject `5ce4faa`, submodule `scviewer/lights`
`98222214`, and supersedes the earlier review's statement that neutral Emissive
without Glow/map always stays off. The read-only command
`python3 -I ue/scripts/survey_materials.py out/dec-r5` reproduced **395**
submaterials: **11** forward Illum cards with non-black Emissive/no Glow and
**42** LayerBlend_V2 materials with non-black Emissive/no Glow/no forward flag.
The design row's older count of 391 is historical; this agent did not edit it.

The implementation shares the amended strength calculation between the sidecar
and `emissive_factor()`: positive Glow, then mean(Emissive) for forward
Illum/HardSurface cards, then enabled emittance map, then zero. Tint remains
Emissive (white when black); surface colour is applied once by the importer.
The previously verified `%MACRO_EMISSIVE_MAP` permutation remains supported
alongside `%EMITTANCE_MAP`. `forward_rendering` is explicit on `emissive`.
`alpha_config` carries `forward_rendering`, `two_sided`, `alpha_test`, `opacity`.
Scene/material schemas stay v2; cache format v6 prevents stale v2 package hits.

| Acceptance probe | Strength | Forward | Colour/coverage evidence |
| --- | ---: | --- | --- |
| Root `emissives` | ≈ 0.3 | true | Tint white; surface RGB before texture ≈ (0.063669242, 0.179160565, 0.276474595) |
| Root `Headlight_Glow` | 1.0 | true | Emissive (1,1,1), no Glow; soft forward glow card |
| `pipe mat 1` | 0 | false | LayerBlend_V2 neutral Emissive is not energy |
| KLWE_las_rep_s1-3 `glows` | ≈ 0.35 | false | Black Emissive is untinted; positive Glow drives surface emission |

The `glows` selector in the acceptance script includes the source material stem:
a distinct jump-drive material also calls a submaterial `glows` and authors Glow
≈ 0.3. No production rule uses these names. All numeric acceptance values above
come from the supplied brief/live survey; unit tests use synthetic values.

### Enabled DataCore property-link evidence

Reproduced with:

```sh
target/debug/starbreaker dcb query EntityClassDefinition --filter 'EntityClassDefinition.AEGS_Gladius_Thruster_Main' > out/wp-r5-amended/thruster-record.json
target/debug/starbreaker nmc dump 'Data\Objects\Spaceships\Thrusters\AEGS\AEGS_Gladius_Thrusters\AEGS_Gladius_Thruster_Main.cga' > out/wp-r5-amended/thruster-nmc.txt
```

Entity record **5d118473-dd82-4182-81a8-3bcf35b8d969** is
`EntityClassDefinition.AEGS_Gladius_Thruster_Main`.
`Components[SEntityComponentEffects].entityEffects.materialEffects.enabled`
is true, and **both** `tagEffects[0]` and `tagEffects[1]` are enabled.
Each has `propertyLinks[0].materialProperty = Glow` and
`propertyLinks[0].subMeshName = Gladius_Thruster_Main`. Their parameter record is
`25650c41-acb0-4545-ad28-6417928c2a5e`; tag records are respectively
`eb736d5b-c639-48b0-852f-f18d919e1795` and
`f303463a-97cb-4d49-b609-71966d409507`. Neither link contains a material-slot filter.

| Record ID | Property links | Exact submesh / NMC node | Source material slot → exported slot | Material | LOD1 indices |
| --- | --- | --- | --- | --- | ---: |
| 5d118473-dd82-4182-81a8-3bcf35b8d969 | tagEffects[0/1].propertyLinks[0].Glow | Gladius_Thruster_Main / 0 (geometry, parent 1) | 1 → 0 | engine_a | 6354 |
| 5d118473-dd82-4182-81a8-3bcf35b8d969 | tagEffects[0/1].propertyLinks[0].Glow | Gladius_Thruster_Main / 0 (geometry, parent 1) | 2 → 1 | engine_b | 44871 |
| 5d118473-dd82-4182-81a8-3bcf35b8d969 | tagEffects[0/1].propertyLinks[0].Glow | Gladius_Thruster_Main / 0 (geometry, parent 1) | 4 → 3 | text_decals | 48 |
| 5d118473-dd82-4182-81a8-3bcf35b8d969 | tagEffects[0/1].propertyLinks[0].Glow | Gladius_Thruster_Main / 0 (geometry, parent 1) | 5 → 4 | engine_glow | 816 |

The following conclusion was superseded by the focused glow-capability round
below: a node-level write alone is insufficient evidence that a slot responds.
`text_decals` is addressed by the authored **node-level** Glow writes;
its label does not indicate a separate NMC node. The four-target join is retained.
It describes eligibility for the manual control, not a claim that every section
visibly glows in every game state (effect tags, modifiers and shader coverage
still matter). The synthetic regression
`enabled_glow_link_includes_markings_and_retains_exact_source_ids` feeds an enabled
Glow link through the join, includes a marking section on that exact node, and
rejects a similarly named sibling. The acceptance script's `--glow-record` option
also rechecks the live record → node → sidecar source slot → GLB primitive join.

Amendment verification (all commands from the superproject root; logs and raw
evidence in ignored `out/wp-r5-amended/`):

```sh
CARGO_TARGET_DIR=./target cargo build
CARGO_TARGET_DIR=./target cargo test --workspace
CARGO_HOME=/tmp/scviewer-r5-cargo CARGO_TARGET_DIR=./target cargo test --manifest-path vendor/StarBreaker/Cargo.toml --workspace -- --test-threads=1
CARGO_TARGET_DIR=./target cargo fmt -p scviewer-core -p scviewer-cli --check
rustfmt --edition 2024 --check vendor/StarBreaker/crates/starbreaker-3d/src/material_semantics.rs vendor/StarBreaker/crates/starbreaker-3d/src/pipeline/engine_glow.rs
target/debug/scviewer decomposed Gladius out/dec-r5 --lod 1 --mip 2
target/debug/scviewer validate out/dec-r5
python3 -I scripts/verify_wp_r5.py out/wp-r5-amended/before out/dec-r5 --expect-engine-glow present --glow-record out/wp-r5-amended/thruster-record.json
python3 -I ue/scripts/survey_materials.py out/dec-r5
for entity in Gladius Scorpius Cutlass_Black Controller_Light; do
  target/debug/scviewer --no-cache lights "$entity" > "out/wp-r5-amended/${entity}-lights.json" 2> "out/wp-r5-amended/${entity}-lights.log" || exit
done
```

The synthetic forward-card regression first failed with strength 0 versus
expected 1 (`forward-red.log`), then passed after the amendment. Root workspace:
**17 passed, 0 failed**. Upstream workspace aggregate: **2141 passed, 0 failed,
61 ignored**; the 3D library has **789 passed, 3 ignored**. Formatting checks pass.
The scratch Cargo home uses the existing package cache with writable downloaded
dependencies because the normal registry is read-only in this sandbox; all build
outputs still use `./target`.

Export: **502 files, cache miss**. Validation: **is_valid=true, errors=[],
warnings=[]**. Acceptance: strengths **0.3000000119 / 1 / 0 / 0.3499999940**
for emissives / Headlight_Glow / pipe mat 1 / KLWE glows; every submaterial's
emission and alpha fields match its authored data. `emissives.alpha_config` is
`{forward_rendering:true, two_sided:true, alpha_test:0, opacity:1}`;
`Headlight_Glow.alpha_config` differs only in `two_sided:false`.
The script confirms all four enabled Glow joins, unchanged topology and all ten
interior placements, 45 GLBs / 23 sidecars / 366 PNGs / 51 children / 3 interiors /
63 clips, and **77 lights = 21 root + 56 interiors**. The generic validator's
`light_count:0` is its GLB-light statistic, not the scene-sidecar light total.
CLI counts reconfirm **Gladius 77 / Scorpius 127 / Cutlass_Black 225 /
Controller_Light 0**. Survey reconfirms 395 submaterials, including the 11 forward
Illum cards and 42 non-forward LayerBlend_V2 entries described above.

No new `TODO(WP-R5)` items; the two earlier audit items remain outstanding.

## Focused engine-glow capability and modifier round

This round supersedes the four-target conclusion above. Hypothesis: a runtime
node-level Glow write only produces emission on shader permutations capable of
using it. The structural gate is forward-rendered Illum/HardSurface
(`MtlFlags & 0x8000`) or an emittance feature token (`EMITTANCE_MAP` /
`MACRO_EMISSIVE_MAP`). No material, node, ship or texture name is used to classify
capability. The authored node name still supplies the exact, case-insensitive
DataCore → NMC join; source material IDs survive sidecar filtering.

Evidence: [the in-game rear reference](../../ue/verification/reference/gladius/rear.webp)
shows a blue-white nozzle interior with a dark housing, while
[the UE four-target Glow=50 capture](../../ue/verification/wp-u2/rear-engine-glow50.jpg)
lights the housing. Both images were inspected read-only. On that same node,
`engine_a` / `engine_b` are deferred Illum, `details` is MeshDecal, and
`text_decals` is deferred Illum with `%DECAL%DECAL_OPACITY_MAP`; none has an
emittance source. Only `engine_glow` is a forward Illum card (`%DECAL`), with
Diffuse approximately (0.021, 0.539, 0.939), base texture
`gladius_emissives_diff`, black Emissive and no authored Glow. These facts support
the capability gate rather than a name filter. The regenerated target list is
exactly **[`engine_glow`]**, source slot **5**, exported slot **4**, **816** LOD1
indices. This round verifies exported semantics; a new UE rendering was not
performed.

The same raw DataCore record above supplies both enabled chains:

| Tag effect | Ordered modifiers | Output minimum | Output maximum |
| --- | --- | ---: | ---: |
| 0 | MultiplierModifier 50 → MinMaxModifier | 10 | 10000000 |
| 1 | MultiplierModifier 100 → MinMaxModifier | 20 | 10000000 |

One target preserves both chains and their parameter/tag record IDs in
`property_links[]`; its parallel `multiplier`, `minimum`, `maximum` arrays are
respectively `[50,100]`, `[10,20]`, `[10000000,10000000]`. The control uses
`units:"material_property_glow"`, **min_strength 10**, **max_strength 10000000**,
**default_strength 10**. The default is the minimum of the authored output
bounds, an initial manual control value rather than a claim about the active
runtime tag. The authored maximum is preserved without a fabricated display
cap or assumed normalized thrust domain. Unknown bounds remain null; malformed
or unsupported modifier chains fail explicitly. Static-only targets retain
their authored Glow value. The upstream export contract documents these units,
parallel arrays, ordered chains and range derivation. The scviewer package-cache
version advances from 6 to 7 to reject packages with the old control semantics.

TODO(verify): Deferred materials with authored Glow > 0 (buttons, glows) are still treated as statically emissive under the rule; capture such a surface in game to confirm that interpretation. The rear-engine images establish the dynamic card gate, not this separate static-emission hypothesis.

Verification from the superproject root (logs in ignored `out/wp-r5-glow/`):

```sh
CARGO_TARGET_DIR=./target cargo test --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d --lib node_glow_selects_forward_card_not_deferred_materials
CARGO_TARGET_DIR=./target cargo test --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d --lib engine_glow
CARGO_TARGET_DIR=./target cargo build && CARGO_TARGET_DIR=./target cargo test --workspace
CARGO_HOME=/tmp/scviewer-r5-cargo CARGO_TARGET_DIR=./target cargo test --manifest-path vendor/StarBreaker/Cargo.toml --workspace -- --test-threads=1
CARGO_TARGET_DIR=./target cargo fmt -p scviewer-core -p scviewer-cli --check
rustfmt --edition 2024 --check vendor/StarBreaker/crates/starbreaker-3d/src/pipeline/engine_glow.rs
target/debug/scviewer --no-cache decomposed Gladius out/dec-r5
target/debug/scviewer validate out/dec-r5
python3 -I scripts/verify_wp_r5.py out/wp-r5-glow/before out/dec-r5 --expect-engine-glow present --glow-record out/wp-r5-amended/thruster-record.json
for entity in Gladius Scorpius Cutlass_Black Controller_Light; do
  target/debug/scviewer --no-cache lights "$entity" > "out/wp-r5-glow/${entity}-lights.json" 2> "out/wp-r5-glow/${entity}-lights.log" || exit
done
```

The bug regression first failed selecting a deferred slot alongside the card
(`red.log`); the final synthetic case includes a forward card, deferred Illum /
HardSurface / MeshDecal sections, a sibling node and repeated primitives, and
selects only the card with both chains. Targeted engine-glow tests: **8 passed**.
Workspace: **17 passed**. Upstream aggregate: **2144 passed, 0 failed, 61 ignored**.
Build and formatting checks pass. Export: **502 files**, cache miss. Validation:
**is_valid=true, errors=[], warnings=[]**. The acceptance script checks the exact
target list, modifiers, record IDs and source-slot join, unchanged topology and
all ten interior placements: **45 GLBs / 23 sidecars / 366 PNGs / 51 children /
3 interiors / 63 clips**. Static emissive strengths remain **0.3000000119**
(`emissives`, forward), **1** (`Headlight_Glow`, forward), **0** (`pom` and
`pipe mat 1`), and **0.3499999940** (deferred KLWE `glows`); alpha fields and
decal objects still match authored data. Light counts remain **Gladius 77
(21 root + 56 interiors), Scorpius 127, Cutlass_Black 225, Controller_Light 0**.

Changed upstream files in this round:

```text
crates/starbreaker-3d/src/pipeline/engine_glow.rs
crates/starbreaker-3d/src/decomposed.rs
docs/decomposed-export-contract.md
```

The two earlier `TODO(WP-R5)` audit items remain, alongside the `TODO(verify)`
above. No commits or UE changes were made in this round.
