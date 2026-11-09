# Work-package briefs

One file per work package. A brief is handed to its agent **verbatim**; it must be
self-contained (goal, inputs, files, done-criteria, verification). The dispatcher
prepends only: current commit, branch name, and anything learned since the brief
was written. Agent assignment and ordering live in `docs/02-implementation-plan.md`.

Dropped on 2026-11-09 when the viewer became UE-native: WP-A1..A3 (Tauri/three.js) and
WP-U4 (merged into WP-R4 + WP-V1). Added the same day for the standalone-app and
first-person requirements: WP-P1, WP-S1; WP-U0 gained the build-path spike. Added for
the local-file-cache requirement: WP-C1 (layer 2), with layer 1 folded into WP-R1.

Common preamble every agent receives:

> You are working in `/home/baadc0de/Development/baadc0de/SC_UE_ModelViewer`.
> Read `AGENTS.md`, `docs/01-starbreaker-analysis.md`, `docs/02-implementation-plan.md`
> and `vendor/StarBreaker/AGENTS.md` first. Game data: Star Citizen LIVE 4.10.2 at
> `~/Games/star-citizen/drive_c/Program Files/Roberts Space Industries/StarCitizen/LIVE/Data.p4k`
> (auto-discovered; never set `SC_DATA_P4K`). Only one agent may run `cargo build`
> or `cargo test` at a time; if the orchestrator has not told you the build lock is
> yours, prepare the change and ask. Write exports to `./out/` (gitignored). Report:
> what changed, the exact verification commands you ran and their output, open
> issues as `TODO(WP-xx)` lines.
