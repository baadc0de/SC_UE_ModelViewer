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

## Cross-review (Opus 5.5)

Reviewed `vendor/StarBreaker` 29bf66fc (on 2b4c8454) and superproject 026fdcf against the brief.
Re-ran: `cargo test -p scviewer-core` (24 passed), `cargo test --manifest-path
vendor/StarBreaker/Cargo.toml -p starbreaker-3d` (865 passed, 31 ignored, incl. the 8 new
`light_field_tests`), the Blender `test_light_fields.py` (3 OK), and
`scviewer --no-cache lights` for the three ships (77 / 127 / 225 lights; every light
`active_state = defaultState`; authored-off defaults 0 / 3 / 24, each of which had a lit
auxiliary/emergency/cinematic state, so these are exactly the lights the old policy flipped).
Codex's before/after scenes in `/tmp/wp-r5b` differ, for lights whose active state did not change,
only by the new `intensity_candela_proxy_model` label: proxy maths unchanged. `importance`
options come from DataCore at run time (`Decorative, Standard, Secondary, Key, Cinematic`); an
absent struct or property returns an empty list, so `importance` stays null and the name is kept.
The cache bump (10 → 11) is in the `package_key` tuple, the entity-index file name and the
light-catalogue identity, so pre-R5b layer-1 entries miss. The submodule commit has no trailer,
no game-derived files, `//!` headers on both new `.rs` files, and red logs for every new test.

### Findings (ranked)

1. **should-fix (follow-up WP): sparse `EntityClass="Light"` components get values the data
   contradicts.** `socpak.rs:2016-2036` (`shadow_casting`, `affects_this_area_only`, `affect_gi`,
   `importance`, and the `None` optionals), `socpak_light_tests.rs:146-165` (enforces `false`).
   Gladius `base_lightgroups` Light-088/-089 (the cockpit projectors) are serialised as *deltas*:
   only `lightType`, `bulbRadius`, plane size, state intensities, `FOV` and fade `maxDistance` are
   authored. The base they override is the DataCore record `EntityClassDefinition.Light` →
   `LightComponentParams`. Its values are `affectsThisAreaOnly=true`, `affectsObjects=true`,
   `affectGI=InheritAffectsObjects`, `importance=Standard`, `maxShadowCastDist=15`,
   `planeSoftness=1`, `fadeParams.minDistance=0`, `lightRadius=10`, `temperature=6600`. The export
   has `affects_this_area_only=false`, `affect_gi=false`, `importance=null`,
   `max_shadow_cast_distance_m=null` and `plane_softness=null`. Two older invented values sit on
   the same path: `radius 5.0` (`socpak.rs:1965`) and state `temperature 6500` (`socpak.rs:1871-1874`).
   The 0.05 and 1.0 defaults happen to match this record. Under the new policy there is also a
   latent bug: a `defaultState` node with no `intensity` attribute is read as an authored 0
   (`socpak.rs:1867-1870`) and would now be exported dark, although a delta means "inherit 1.0".
   This does not occur in the three ships: all 27 off-defaults carry an explicit `intensity="0"`.
   **Fix:** resolve the SOC entity's `EntityClass` to its `EntityClassDefinition`
   `LightComponentParams` and use it as the base for every missing attribute. This replaces the
   hand-written defaults, including the brief's 0.05/1.0, with data. Until then, treat a missing
   `intensity` as unknown rather than 0. Not a blocker: the brief fixed `bool` types and
   "absent → false", and only 2 of 429 lights are affected. Under upstream's self-correcting
   hard-coding rule it needs a tracked `TODO`, though. Codex's report only hints at it
   ("legacy sparse-component defaults").
2. **should-fix (brief-level, owner decision): Blender now turns shadows off for ~97 % of lights.**
   `entity_lights.py:9-10`. `shadow_casting → use_shadow` follows the brief. But Blender has no
   `affectsThisAreaOnly` vis-area containment, so exterior lights now leak through hulls into
   interiors and back. WP-U3b §11.3 makes the same point for UE: without containment, shadows
   are the substitute. **Fix:** keep `use_shadow=True` when `affects_this_area_only` is true
   (or add an import option), and honour `shadow_casting=false` only for unconfined lights.
3. **should-fix: viewer-specific text in the upstream contract.**
   `vendor/StarBreaker/docs/decomposed-export-contract.md:252-299` names `WP-R5b`,
   `scviewer lights` / `sc_lights`, `scviewer-core::CACHE_FORMAT_VERSION` and `SCPackageReader`.
   None of these exist upstream, and WP-D1 will upstream this branch. The same applies to
   `TODO(WP-R5b)` → `ue/verification/WP-U3b.md` at `socpak.rs:31` (the brief asked for it).
   **Fix:** keep the field table, enum and active-state text upstream. Move the cache, FFI and
   UE-reader paragraph to `docs/03-unreal-export-design.md`, which already has it. Reword the TODO
   to name the physical bulb-size calibration without a superproject path.
4. **nit: new code is not rustfmt-clean.** `socpak.rs:2023` and `socpak.rs:2034-2035`
   (`importance_name` chain), `pipeline/interiors.rs:762-767` (the `resolve_importance` closure
   and `} else { Vec::new() }`). Codex checked only the two new files, which are clean; both
   touched files already had format drift. **Fix:** format just these hunks.
5. **nit: the cache test is tautological.** `cache.rs:312-318` copies `package_key`'s tuple
   layout with a literal `10`. It passes for any version ≠ 10 and goes silently stale if the
   tuple changes. **Fix:** call `package_key_for_exporter` with a parameterised version, or simply
   `assert!(CACHE_FORMAT_VERSION >= 11)` next to a comment naming R5b.
6. **nit: unknown `affectGI` → `false`.** `socpak_light_fields.rs:10-13` is a fallback
   silencer under the upstream rules. `affect_gi_mode` keeps the spelling, so the harm is small.
   **Fix:** derive the known option set from the DataCore enum, as importance does, and fail the
   test when LIVE gains a new option.
7. **nit: one warning per light when the importance enum is missing.**
   `socpak_light_fields.rs:39-43` warns per light, not once per export. **Fix:** warn once in
   `importance_options` when it returns empty.
8. **nit: layout and coverage.** `apply_authored_light_fields` lives in `entity_lights.py` but
   serves every light (move it to `utils.py` or a `light_fields.py`). `light_fields` is a
   non-test `#[path]` module at `src/socpak_light_fields.rs` (upstream uses `#[path]` only for
   tests). No fixture has a non-empty `children[].lights` (0 in all three ships); it shares the
   serialisation path, so coverage is structural only.
9. **nit (forward note for WP-C1):** the layer-2 entity-load key in `WP-C1-processed-cache.md`
   ("archive build id + entity + full options") has no exporter or package identity. Exporter
   changes like this one would then return stale UE scenes. **Fix:** key layer 2 on the layer-1
   `package_key` (or `EXPORTER_VERSION` + `CACHE_FORMAT_VERSION`).

Checklist status: (1) exact field names on all three scene arrays, the catalogue, `sc_lights`
and GLB extras: yes; defaults as briefed, but see 1. (2) Policy: correct, counts 0 / 3 / 24
reproduced. (3) Proxy maths unchanged and labelled `raw_x1500`: yes. (4) Cache bump invalidates
and is tested: yes (5). (5) `importance` derived from DataCore and degrades to null + name: yes.
(6) Upstream rules: headers, trailer, files and tests-first all OK; formatting 4. (7) Blender is
backward compatible: absent or null fields leave the legacy mapping untouched (tested); for the
behaviour change see 2. (8) Docs: consistent across `03`, the upstream contract and the FFI
README; scope 3.

**Verdict: merge with follow-ups.** The WP does what the brief asks. All authored fields reach
every output with the fixed names. The active-state fix is exact and minimal (the 27 flipped
lights are precisely those with an explicit zero default). The proxy is untouched and labelled.
Cache invalidation is real, `importance` comes from DataCore, and the test suites pass. Nothing
here blocks the UE importer work. File finding 1 as a follow-up WP: entity-class base
resolution for sparse light components, which also retires the invented 5 m radius and 6500 K.
Decide finding 2 (Blender shadows) with the owner. Fold findings 3-4 into the branch before the
WP-D1 upstream PR.

## Follow-up round

Addressed findings **1–7**; findings 8–9 remain outside this round. Sparse lights now inherit missing attributes and states from their entity class's DataCore `LightComponentParams` at export time. Explicit zero/false overrides survive; missing default intensity inherits the class intensity. Unresolved source values remain nullable, with only the documented bulb-radius 0.05 m and attenuation-tweak 1.0 defaults. The compatibility intensity calculation is unchanged.

Blender keeps shadows enabled for area-contained lights and otherwise honours `shadow_casting`. Its manifest/state consumers preserve unknown values. Unknown GI retains its mode and null value; GI and missing-importance diagnostics each warn once per export, verified across repeated lights and successive exports. Upstream wording is neutral. The parameterised cache-key regression proves version changes alter the key; `CACHE_FORMAT_VERSION` is **12** (previously 11). Scene schema remains **v2**; nullable fields are documented in the contract, UE design and FFI README.

Tests were run before fixes: sparse inheritance, unknown fields, cache-version sensitivity, Blender shadows and nullable state handling reproduced failures. Extracted projector/class fixtures include provenance and a live DataCore/SOC validator. Logs and scratch scripts are under `/tmp/wp-r5b-followup`.

Commands run from the repository root, with sequential Cargo builds:

```sh
export CARGO_TARGET_DIR=./target CARGO_INCREMENTAL=0
cargo test -p scviewer-core
CARGO_HOME=/tmp/scviewer-r5-cargo cargo test --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d
cargo build
cargo test --workspace
python3 -m unittest discover -s vendor/StarBreaker/blender_addon/tests -p test_light_fields.py
python3 -m unittest discover -s vendor/StarBreaker/blender_addon/tests -q
python3 -I /tmp/wp-r5b/blender-suite.py
target/debug/scviewer --no-cache lights AEGS_Gladius > /tmp/wp-r5b-followup/AEGS_Gladius.json
blender -b --factory-startup --python-exit-code 1 --python /tmp/wp-r5b-followup/blender-live.py
cargo fmt -p scviewer-core --check
cargo fmt --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d --check
UV_CACHE_DIR=/tmp/wp-r5b-followup/uv-cache uv run python /tmp/wp-r5b-followup/focus_format.py --check
rustfmt --edition 2024 --config skip_children=true --check \
  vendor/StarBreaker/crates/starbreaker-3d/src/socpak_light_tests.rs \
  vendor/StarBreaker/crates/starbreaker-3d/src/socpak_light_fields.rs \
  vendor/StarBreaker/crates/starbreaker-3d/src/socpak/light_component.rs
cargo sweep --maxsize 10GB .
git diff --check -- crates docs/03-unreal-export-design.md docs/work-packages/WP-R5b-light-fields.md
git -C vendor/StarBreaker diff --check
```

Results:

| Check | Output |
| --- | --- |
| scviewer-core | 24 passed, 0 failed |
| starbreaker-3d | 870 passed, 0 failed, 31 ignored |
| Viewer workspace | 33 passed, 0 failed |
| Debug build | `Finished dev profile [optimized + debuginfo]` |
| Blender light-field tests | 4 tests, OK |
| Standard Blender suite | 432 tests; 3 documented missing-material-fixture errors, 41 skipped |
| Blender suite with scratch material fixture | 432 tests, OK, 41 skipped; assertions unchanged |
| Headless Blender | 77 lights imported; authored dimensions/shadows PASS |
| Scoped formatting and diff checks | Passed |
| Target sweep | Cleaned 1.49 GiB; final `target` size 11G; no incremental directory |

The Blender scratch runner redirects only the missing component-master fixture to the existing Gladius TEX2 material sidecar. The standard suite's three errors are unrelated to these changes.

As requested, formatting is limited to changed files/hunks. **The starbreaker-3d `cargo fmt --check` still fails on pre-existing crate-wide drift: 146 files at baseline, unrelated to this WP; 142 remain after the scoped fixes.** No whole-crate formatting is included.

Live CLI output summary:

```text
AEGS_Gladius: 77 lights
base_lightgroups/Light-088: radius_m=10, temperature=6600, affects_this_area_only=true, affect_gi=true, importance=1 (Standard)
base_lightgroups/Light-089: radius_m=10, temperature=6600, affects_this_area_only=true, affect_gi=true, importance=1 (Standard)
Both: max_shadow_cast_distance_m=15, plane_softness=1, fade_min_distance_m=0
Authored overrides preserved: bulb_radius_m=0.1, intensity_raw=0.2, fade_max_distance_m=75
```

Changed files, relative to the superproject root:

```text
crates/scbridge-ffi/README.md
crates/scviewer-core/src/cache.rs
docs/03-unreal-export-design.md
docs/work-packages/WP-R5b-light-fields.md
vendor/StarBreaker/blender_addon/starbreaker_addon/manifest.py
vendor/StarBreaker/blender_addon/starbreaker_addon/runtime/importer/entity_lights.py
vendor/StarBreaker/blender_addon/starbreaker_addon/runtime/importer/orchestration.py
vendor/StarBreaker/blender_addon/starbreaker_addon/runtime/package_ops.py
vendor/StarBreaker/blender_addon/tests/test_light_fields.py
vendor/StarBreaker/blender_addon/tests/test_manifest.py
vendor/StarBreaker/blender_addon/tests/test_package_ops.py
vendor/StarBreaker/crates/starbreaker-3d/src/decomposed.rs
vendor/StarBreaker/crates/starbreaker-3d/src/gltf/glb_builder.rs
vendor/StarBreaker/crates/starbreaker-3d/src/pipeline/blend_assembly.rs
vendor/StarBreaker/crates/starbreaker-3d/src/pipeline/glb_assembly.rs
vendor/StarBreaker/crates/starbreaker-3d/src/pipeline/interiors.rs
vendor/StarBreaker/crates/starbreaker-3d/src/pipeline/light_catalogue.rs
vendor/StarBreaker/crates/starbreaker-3d/src/pipeline/mod.rs
vendor/StarBreaker/crates/starbreaker-3d/src/socpak.rs
vendor/StarBreaker/crates/starbreaker-3d/src/socpak/light_component.rs
vendor/StarBreaker/crates/starbreaker-3d/src/socpak_light_fields.rs
vendor/StarBreaker/crates/starbreaker-3d/src/socpak_light_tests.rs
vendor/StarBreaker/crates/starbreaker-3d/src/types.rs
vendor/StarBreaker/crates/starbreaker-3d/tests/fixtures/light_class/README.md
vendor/StarBreaker/crates/starbreaker-3d/tests/fixtures/light_class/gladius_projectors.xml
vendor/StarBreaker/crates/starbreaker-3d/tests/fixtures/light_class/light_component.json
vendor/StarBreaker/docs/decomposed-export-contract.md
```

No commits or branches were created. No task writes were made under `out/` or `ue/`; unrelated changes are excluded above. The tree is ready to stage.
