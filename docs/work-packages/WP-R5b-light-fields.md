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

## Report

Implemented WP-R5b. Authored fields reach all scene light arrays, the CLI/FFI catalogue and bundled-GLB light extras. Authored `defaultState` always wins, including zero intensity. Proxy maths is unchanged and labelled `raw_x1500`. Blender consumes authored shadows, bulb radius and plane dimensions while retaining compatibility with older packages.

LIVE uses named enums: `importance` now resolves from the current DataCore property enum's option index, and `affectGI="InheritAffectsObjects"` follows `affectsObjects`. Additional `importance_name` and `affect_gi_mode` fields preserve the source spellings. No game enum ordinal table or asset-specific rule was introduced.

**Version:** `crates/scviewer-core/src/cache.rs::CACHE_FORMAT_VERSION` changed **10 → 11**, invalidating old layer-1 packages. Scene/material schema stays **v2**. UE `SCPackageReader` must continue accepting **scene versions 1 and 2** and consume the additive v2 fields; **11 is not a scene version**.

| Ship | Lights | Active state changed | Off defaults retained |
| --- | ---: | ---: | ---: |
| AEGS_Gladius | 77 | 0 | 0 |
| RSI_Scorpius | 127 | 3 | 3 |
| DRAK_Cutlass_Black | 225 | 24 | 24 |

All 429 lights match the raw SOC authored-field tuples. Catalogue/scene parity passes for every new field. Package file counts remain 502 / 491 / 747; all GLB topology counts and 10 / 14 / 30 interior placements are unchanged.

Verification ran from the repository root, sequentially, using debug builds and scratch outputs in `/tmp/wp-r5b`. Commands:

```sh
export CARGO_TARGET_DIR=./target CARGO_INCREMENTAL=0
cargo build
cargo test --workspace -- --test-threads=1 --nocapture
CARGO_HOME=/tmp/scviewer-r5-cargo cargo test --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d -- --test-threads=1 --nocapture
CARGO_HOME=/tmp/scviewer-r5-cargo cargo test --manifest-path vendor/StarBreaker/Cargo.toml --workspace -- --test-threads=1
cargo fmt -p scviewer-core --check
rustfmt --edition 2024 --check vendor/StarBreaker/crates/starbreaker-3d/src/socpak_light_tests.rs vendor/StarBreaker/crates/starbreaker-3d/src/socpak_light_fields.rs
```

| Verification output | Passed | Failed | Ignored |
| --- | ---: | ---: | ---: |
| Viewer workspace, including scviewer-core | 33 | 0 | 0 |
| scviewer-core portion | 24 | 0 | 0 |
| starbreaker-3d, aggregate across 22 suites | 865 | 0 | 31 |
| Full upstream workspace, aggregate across 102 suites | 2161 | 0 | 61 |

Build output: `Finished dev profile [optimized + debuginfo]`. Formatting checks exit 0. Tests were added and run before fixes: parser regressions reproduced three failures; bundled GLB lacked extras; the pre-R5b cache key was reused; enum parsing lost names/inherited GI. Each regression now passes. Logs: `rust-red.log`, `glb-red.log`, `cache-red.log`, `enum-red.log`, `build-final.log`, `workspace-final.log`, `starbreaker-3d-final.log`, `upstream-final.log` under `/tmp/wp-r5b`.

Baseline exports were captured before implementation with the same decomposed command and destinations `before-$ship`. Final live commands:

```sh
for ship in AEGS_Gladius RSI_Scorpius DRAK_Cutlass_Black; do
  target/debug/scviewer --no-cache lights "$ship" > "/tmp/wp-r5b/after-$ship.json"
  target/debug/scviewer --no-cache decomposed "$ship" "/tmp/wp-r5b/after-$ship"
  UV_CACHE_DIR=/tmp/wp-r5b/uv-cache uv run python -I scripts/verify_wp_r5.py "/tmp/wp-r5b/before-$ship" "/tmp/wp-r5b/after-$ship" --generic --expect-engine-glow present --catalogue "/tmp/wp-r5b/after-$ship.json"
done
python3 -I /tmp/wp-r5b/raw-verify.py
```

Output excerpts:

```text
WP-R5b: AEGS_Gladius active state changed: 0 authored-off defaults: 0
Catalogue/scene authored-field parity: 77 lights
WP-R5b: RSI_Scorpius active state changed: 3 authored-off defaults: 3
Catalogue/scene authored-field parity: 127 lights
WP-R5b: DRAK_Cutlass_Black active state changed: 24 authored-off defaults: 24
Catalogue/scene authored-field parity: 225 lights
EntityClassDefinition.AEGS_Gladius: 77 raw SOC component field tuples match; affect_gi true=75
EntityClassDefinition.DRAK_Cutlass_Black: 225 raw SOC component field tuples match; affect_gi true=223
EntityClassDefinition.RSI_Scorpius: 127 raw SOC component field tuples match; affect_gi true=102
```

The verifier asserts every requested field, proxy labels, named-importance resolution, catalogue parity and zero-default darkness. Float comparisons preserve f32 identity across direct catalogue JSON and scene JSON decimal representations.

Blender and C ABI verification:

```sh
python3 -m unittest discover -s vendor/StarBreaker/blender_addon/tests -p test_light_fields.py
python3 -I /tmp/wp-r5b/blender-suite.py
blender -b --factory-startup --python-exit-code 1 --python /tmp/wp-r5b/blender-live.py
cc -std=c11 -Wall -Wextra -Werror -I crates/scbridge-ffi/include crates/scbridge-ffi/ctest/smoke.c -L target/debug -Wl,-rpath,"$PWD/target/debug" -lscbridge -o /tmp/wp-r5b/ffi-smoke
/tmp/wp-r5b/ffi-smoke /tmp/wp-r5b/ffi - smoke
```

Output: new Blender regressions **3 passed**; full stub suite **429 tests, OK (41 skipped)**; headless Blender **77 / 127 / 225 lights imported**, authored dimensions/shadows passed, **0 / 3 / 24 off defaults stayed off**. The normal full stub-suite invocation initially hit its three documented missing component-master fixture errors. The scratch runner redirects only that fixture to the live Gladius TEX2 sidecar; assertions are unchanged. Headless verification used Blender 5.2.3 LTS and imported light records without gobo graphs.

C output: `C ABI smoke (uncached): PASS`. A JSON comparison also passed: `sc_lights: 77 lights; all authored fields and states match CLI; zero-default policy PASS`.

Final housekeeping:

```sh
CARGO_TARGET_DIR=./target cargo sweep --maxsize 10GB .
rmdir target/debug/incremental
du -sh target
git diff --check
git -C vendor/StarBreaker diff --check
```

Output: sweep cleaned **8.80 GiB**; `target` is **11G**; both diff checks exit 0. No commits or branches were created. This task made no writes under `out/` or `ue/`; unrelated concurrent changes are excluded below.

Changed files, relative to the superproject root:

```text
crates/scbridge-ffi/README.md
crates/scviewer-core/src/cache.rs
crates/scviewer-core/src/tests.rs
docs/03-unreal-export-design.md
docs/work-packages/WP-R5b-light-fields.md
scripts/verify_wp_r5.py
vendor/StarBreaker/blender_addon/starbreaker_addon/runtime/importer/entity_lights.py
vendor/StarBreaker/blender_addon/starbreaker_addon/runtime/importer/orchestration.py
vendor/StarBreaker/blender_addon/tests/test_light_fields.py
vendor/StarBreaker/crates/starbreaker-3d/src/gltf/glb_builder.rs
vendor/StarBreaker/crates/starbreaker-3d/src/pipeline/interiors.rs
vendor/StarBreaker/crates/starbreaker-3d/src/socpak.rs
vendor/StarBreaker/crates/starbreaker-3d/src/socpak_light_fields.rs
vendor/StarBreaker/crates/starbreaker-3d/src/socpak_light_tests.rs
vendor/StarBreaker/crates/starbreaker-3d/src/types.rs
vendor/StarBreaker/docs/decomposed-export-contract.md
```

TODO(WP-R5b): UE-side measurement in `ue/verification/WP-U3b.md` §11.2–11.3 must establish the physical intensity mapping. Legacy sparse-component defaults and old-package Blender sizing remain compatibility behavior; this WP does not recalibrate them.
