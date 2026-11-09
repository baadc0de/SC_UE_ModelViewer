# Work-package briefs

One file per work package. A brief is handed to its agent **verbatim**; it must be
self-contained (goal, inputs, files, done-criteria, verification). The dispatcher
prepends only: current commit, branch name, and anything learned since the brief
was written. Agent assignment and ordering live in `docs/02-implementation-plan.md`.

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
