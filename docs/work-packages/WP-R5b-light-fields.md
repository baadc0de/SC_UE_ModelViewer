# WP-R5b — Light source-of-truth fields and default-state policy (exporter side)

**Agent:** Codex. **Depends on:** WP-R5 landed (`scviewer/lights` at 2b4c8454).
**Build lock:** cargo. **Lands in:** `vendor/StarBreaker` on the existing `scviewer/lights`
branch (already checked out in the submodule) plus `crates/scviewer-*` where the sidecar
contract is mirrored. Follow `docs/work-packages/README.md` (common preamble) and
`vendor/StarBreaker/AGENTS.md`.

## Why

The owner reports that the ships' own lights pool metres across the floor in the UE
viewer, unlike in-game references. The WP-U3b analysis (`ue/verification/WP-U3b.md` §11,
read it first) traced it to two causes. One is the viewer scene (UE side, not this WP).
The other is the exporter: it keeps `sizeParams.lightRadius` and the state intensities but
drops the rest of the authored light component, so the UE importer cannot apply the
CryEngine attenuation model, which depends on the bulb size (`intensity` is the value *at
the bulb*; falloff is `b²/d²` beyond it). The `×1500` candela proxy
(`LIGHT_AUTHORED_INTENSITY_SCALE`, `socpak.rs:24`) ignores the bulb and overstates small
fixture lights by orders of magnitude. The importer will own the physical mapping once the
authored fields exist; this WP exports truth, it does not bake a hypothesis.

Separately, `socpak.rs` picks the active light state as the first with `intensity > 0` in
default → auxiliary → emergency → cinematic order. A light authored *off* in
`defaultState` is therefore shown lit in its auxiliary state (24 Cutlass Black and 3
Scorpius lights). That is a policy bug: the default state is the truth for the default view.

## Tasks

1. **Tests first.** Add unit tests on a small CryXMLB/XML light-component fixture (build it
   from the real `EntityComponentLight` attribute layout: `sizeParams{lightRadius,
   bulbRadius, planeWidth, planeHeight}`, `shadowParams{shadowCasting, maxShadowCastDist}`,
   `fadeParams{minDistance, maxDistance}`, `miscParams{glowMultiplier, attenuationTweak,
   planeSoftness}`, component attributes `importance`, `affectsThisAreaOnly`,
   `useTemperature`, `affectGI`, and the five state children) covering every new field below
   and the state policy, including a light whose `defaultState` intensity is 0 but whose
   `auxiliaryState` is lit.
2. **Export the authored fields** on `LightInfo` (types.rs) and therefore in `scene.json`
   (`root_entity.lights[]`, `children[].lights[]`, `interiors[].lights[]`), the `lights`
   catalogue (`light_catalogue.rs`, `scviewer lights`, `sc_lights`) and the bundled-GLB
   light extras where lights are written. Field names are fixed (the UE importer is being
   written against them): `bulb_radius_m: f32`, `plane_width_m: Option<f32>`,
   `plane_height_m: Option<f32>`, `shadow_casting: bool`, `max_shadow_cast_distance_m:
   Option<f32>`, `affects_this_area_only: bool`, `affect_gi: bool`, `attenuation_tweak:
   f32` (default 1.0), `plane_softness: Option<f32>`, `fade_min_distance_m: Option<f32>`,
   `fade_max_distance_m: Option<f32>`, `importance: Option<i32>`, `glow_multiplier: f32`
   (already parsed; expose it). Missing attributes → the CryEngine defaults where documented
   (`bulbRadius` 0.05, `attenuationTweak` 1.0), otherwise `None`; never invent a value.
3. **Fix the active-state policy.** If `defaultState` is present, it is the active state,
   lit or not (`active_state = "defaultState"`, `intensity_raw` may be 0, colour from that
   state). Fall back to the priority list only when no `defaultState` is authored. Keep all
   states in `states` as today. Document the rule on the field doc comment and in the
   addon-facing docs. Report in the PR how many lights per test ship change (expected: 24
   Cutlass Black, 3 Scorpius, 0 Gladius).
4. **Keep the proxy, label it.** Leave `intensity_candela_proxy` and `intensity_cd` in
   place for compatibility (the UE importer and the Blender addon read them), but make the
   unit labels say what they are (`intensity_unit` stays the raw label; add
   `intensity_candela_proxy_model: "raw_x1500"`), and add a `TODO(WP-R5b)` on
   `LIGHT_AUTHORED_INTENSITY_SCALE` pointing at the UE-side measurement
   (`ue/verification/WP-U3b.md` §11.2–11.3). Do not change the proxy maths in this WP.
5. **Schema/version.** The new fields are additive. Bump the sidecar/scene format version
   constant (or exporter identity, whichever the cache key in `scviewer-core` uses) so a
   layer-1 cache built before this change misses rather than returning stale packages;
   `scviewer-core` tests that assert the version must be updated. Keep the UE reader's
   accepted version range in mind: say in the PR exactly which constant changed and what the
   UE side (`SCPackageReader`) must accept.
6. **Blender addon** (`entity_lights.py`): read the new fields when present — `shadow_casting`
   → light `use_shadow`, `bulb_radius_m` → `shadow_soft_size`, plane size → area light
   dimensions. Optional fields must not break on packages that lack them.
7. **Docs in the same change:** `docs/03-unreal-export-design.md` light rule row and
   sidecar field list; `crates/scbridge-ffi/README.md` if `sc_lights` output changes shape;
   upstream `docs/` for the decomposed format where the lights schema is described.
8. **Verification** (paste commands and output into the PR description):
   `cargo test -p starbreaker-3d` (and the `scviewer-core` tests); `scripts/starbreaker.sh`
   / `scviewer lights AEGS_Gladius`, `RSI_Scorpius`, `DRAK_Cutlass_Black` showing the new
   fields and the active-state counts; `uv run python scripts/verify_wp_r5.py` extended to
   assert the new fields are present and that no light with `defaultState` intensity 0 is
   reported lit.

## Constraints

- One cargo build at a time: you are the only cargo user right now. `CARGO_TARGET_DIR` is
  the shared `./target`; debug profile; no `cargo clean`.
- Do **not** write under `out/` (another agent is reading those packages for GPU
  measurements) and do not touch anything under `ue/`.
- Your sandbox cannot write `.git`: make no commits, create no branches; leave the tree
  ready to stage and list the changed files. Upstream commits carry no `Co-Authored-By`
  trailer; the orchestrator commits.
- No hard-coded game values and no branching on asset names; everything comes from the
  component attributes.
- Rust files start with a `//!` module header.

## Done when

Tests pass; `scene.json` and `scviewer lights` carry the fields above for the three ships;
lights authored off in `defaultState` are exported with `active_state = "defaultState"`
and `intensity_raw = 0`; the format version is bumped; docs updated; PR text with
verification output written to `docs/work-packages/WP-R5b-light-fields.md` under a
"## Report" heading at the end of this file.
