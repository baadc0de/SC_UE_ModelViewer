# WP-Q1 — Verification harness

**Agent:** Opus 5.5. **Depends on:** WP-A2 (viewer), WP-U1 (UE importer) for the UE
part. **Build lock:** needed only when running cargo tests.

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
- Viewer smoke: a Tauri dev-mode `--smoke <entity>` flag that loads the entity,
  waits for first frame, saves a PNG screenshot to `out/smoke/`, and exits non-zero on
  error; document how to compare screenshots by eye.
- UE smoke (once WP-U1 exists): a commandlet or `-ExecutePythonScript` that imports a
  golden decomposed package into `ue/SCTest`, asserts asset counts and Nanite flags,
  and renders the Lumen test level to PNG via `HighResShot`.
- GitHub Actions `ci.yml`: `cargo build`, `cargo test --workspace` (p4k tests skip),
  `npm ci && npm run build`, clippy warnings as errors on our crates only. No game
  data in CI.

## Done criteria
- `scripts/golden.sh` runs end-to-end on this machine in < 3 min and produces stats.
- CI green on a PR that touches only docs.
