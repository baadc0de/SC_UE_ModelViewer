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

## Fast feedback on Win64

Run from the repository root. The tools use the D17 source fork, with `-Engine` or
`UE_ENGINE_ROOT` available for another checkout of that fork. Stage the FFI first
if the project has never been built in this worktree.

```powershell
ue/scripts/rpc/automation.ps1 -Build -Filter SCBridge.FloorPanel,SCBridge.UI.Callouts
ue/scripts/rpc/fixture-smoke.ps1 -Fixture Viewer,Cutlass,C2XL
```

Use one automation invocation for several independent groups. It runs with NullRHI
so pure state and parser tests can finish without D3D12, shader compilation, game
archive exports or textures. Run it with an already built target by omitting
`-Build`. NullRHI cannot validate rendering, GPU resource setup or screenshots.

Choose the smallest live fixture that covers the behavior. `Viewer` starts the
procedural room for camera, capture and generic RPC. `Cutlass` loads the Cutlass
Black into that room for ship imports and basic interior and door checks.
`C2XL` is for changes that need the Hercules lift, C2 panels, cargo in tight
spaces, or the XL hangar's authored lighting and sound. The smoke script records
time to RPC health, time to the named ship and environment becoming idle, and
shutdown time as separate values. The full fixture also checks that Unreal
reported a successful environment import or cooked level load. Run warm
comparisons with the same target, graphics settings, archive cache and DDC.
An engine shader change can invalidate the DDC and dominate cold startup.

Feature scripts can use `/wait` for engine-wide `idle`, `settled`, and lift or
cargo completion. When the test needs a specific field, use `Wait-SCRpc` with a
predicate on the existing response. Check concrete readiness after issuing a
request, because global idle alone can also mean that the request failed or was
never accepted. Use fixed delays when elapsed time is part of the behavior the
test is measuring.

Start a folder-based first-person fixture with `-SCPackage=<export root>
-SCExplore -SCEnvironment=<export root>` to reuse prior exports. The harness
waits for the environment load before entering first person. A cargo scenario
started without `-SCLoad` waits after requesting its environment and then
queues its own ship. Once the C2 is idle, use
`ue/scripts/rpc/c2-floor-panel.ps1 -Port <port> -Through 3 -NoShots` for
the floor selection probe in a session already open for another feature.

Both drivers hold the same per-user Unreal lease across worktrees. Keep a build
or a viewer's full lifetime inside a single lease, including shutdown. For
manual runs, use `Invoke-SCUELease` from `SCRpc.psm1` or coordinate the slot
with the person who owns it. Stage FFI before taking the Unreal slot so a Rust
build does not hold it. Compare screenshots only on a rendered fixture with the
same camera, exposure and output settings.

### First measured pass

Measured on Win64 with the UE 5.9 fork and the same RTX workstation. The cold
full-scene baseline used the original project. The smaller fixtures used the
H2 editor build after the engine shader cache was warm, so the table shows the
feedback modes and their observed costs rather than an isolated A/B of asset
size.

| Run | RPC/startup | Usable scene | Evidence |
| --- | ---: | ---: | --- |
| C2 with XL hangar, cold shaders | 335.87 s | 704.01 s | 166.2 s for the XL environment import after a layer-1 cache hit. |
| Procedural Viewer, warm shaders | 13.12 s | 13.20 s | `fixture-smoke.ps1 -Fixture Viewer`; graceful exit. |
| Cutlass in procedural Viewer, warm engine and first ship import | 12.42 s | 100.14 s | `fixture-smoke.ps1 -Fixture Cutlass`; state confirmed the named ship and graceful exit. |
| Cutlass repeat with Ship Link tests | 10.73 s | 42.68 s | One process, then 20.54 s for the state-polled feature route. |
| Cutlass repeat with Garage tests | — | 29.62 s | A further warm run, followed by a 45.59 s lifecycle route. |

The H2 NullRHI runner found five named tests across two requested filters. Its
log measured 0.80 s from queueing to test session start, down from 5.81 s when
the commands were issued separately. M1 ran 88 focused cases in a single
15.46 s NullRHI process. The same driver ran 93 Ship Link/Garage cases in
14.30 s, 12 audio cases in 9.71 s, and three processed-cache cases in 15.60 s,
including launch and exit. Fixture logs write JSON under `out`; the feature
branches carry their own commands and results.
