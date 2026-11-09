# WP-Q1 — Verification harness

**Agent:** Opus 5.5. **Depends on:** WP-R1 (exports), WP-U1 (UE importer), WP-V1
(`ViewEntity` Python entry). **Build lock:** cargo lock for Rust tests, UE lock for
the headless editor run.

## Goal
Fast, repeatable checks that an agent can run before claiming a WP done, plus CI
that runs what it can without game data.

## Deliverables
- `scripts/golden.sh`: exports Gladius, Scorpius, Cutlass Black (bundled GLB at
  LOD 1/mip 2 and decomposed) to `out/golden/` and writes a stats JSON per export
  (`scripts/glbinfo.py` counts + animation names + timings + peak RSS). A second run
  diffs against the previous stats and fails on count regressions (> 5 %) or missing
  clip names.
- `scripts/validate_decomposed.sh`: runs upstream `validate_decomposed_export` on the
  decomposed output.
- UE smoke: `UnrealEditor-Cmd ue/SCViewer/SCViewer.uproject -run=pythonscript
  -script=scripts/ue_smoke.py` imports each golden package (folder path, and via
  `ViewEntity` through FFI once WP-V1 lands), asserts object counts, Nanite flags, clip counts and collision presence (a line
  trace hits the hull), renders the `Viewer` level to PNG (`HighResShot`) into `out/smoke/`,
  exits non-zero on any failure. Document how to compare screenshots by eye.
- GitHub Actions `ci.yml`: `cargo build`, `cargo test --workspace` (p4k tests skip),
  `scbridge.h` up to date (`cbindgen` diff), clippy warnings as errors on our crates
  only. UE builds are not run in CI (no engine there); the headless smoke is local.
  No game data in CI.

## Done criteria
- `scripts/golden.sh` runs end-to-end on this machine in < 3 min and produces stats.
- CI green on a PR that touches only docs.
- A standalone-host smoke (`scripts/run_viewer.sh --smoke Gladius`, added once WP-S1
  lands) loads the ship, screenshots, exits 0.
