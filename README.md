# SC_UE_ModelViewer

A **standalone Unreal Engine 5.9 application** that is pointed at a Star Citizen
`Data.p4k`, lists the models inside, loads and processes them, and displays them
**rendered with Nanite and Lumen** — textures, a list of animations you can trigger,
and first-person exploration of ships with collision. The Unreal Editor is a
development host and an optional "keep as assets" tool, not the deliverable.

Built on the Rust crates of [StarBreaker](https://github.com/diogotr7/StarBreaker)
(MIT), vendored as a git submodule at `vendor/StarBreaker`.

## Status

**M0 — bootstrap.** Analysis and plan are written; no viewer code yet.

| Doc | Purpose |
| --- | --- |
| [docs/01-starbreaker-analysis.md](docs/01-starbreaker-analysis.md) | what StarBreaker provides, what was verified on this machine, the gaps |
| [docs/02-implementation-plan.md](docs/02-implementation-plan.md) | architecture decisions, milestones, work packages, agent orchestration |
| [docs/03-unreal-export-design.md](docs/03-unreal-export-design.md) | UE app design: hosts and the standalone build path, view flow, object mapping, Nanite/Lumen rules, player modes, UI, FFI |
| [docs/work-packages/](docs/work-packages/) | self-contained briefs handed to Codex / Opus 5.5 agents |

## Quick start (today)

```bash
git submodule update --init
scripts/starbreaker.sh entity loadout Gladius
scripts/starbreaker.sh entity export Gladius out/gladius.glb --lod 1 --mip 2
python3 -I scripts/glbinfo.py out/gladius.glb
```

`Data.p4k` is auto-discovered from the standard RSI install locations (on this
machine the LUG Wine prefix under `~/Games/star-citizen`). Do not set `SC_DATA_P4K`.

## Planned layout

```
crates/scviewer-core   Rust: Session over p4k + DataCore, decomposed export (disk + in-memory), clip catalogue
crates/scviewer-cli    thin CLI for verification
crates/scbridge-ffi    C-ABI library (libscbridge.so) the Unreal plugin calls
ue/SCViewer            UE 5.9 project: game-mode app + Viewer level (Lumen, Nanite, HWRT)
ue/Plugins/SCBridge    runtime module (importer, actor, player, UI) + optional editor module (Keep)
vendor/StarBreaker     upstream toolkit (submodule, pinned)
scripts/               helpers (starbreaker.sh, glbinfo.py, golden.sh …)
```

## License

This repository: MIT (see `LICENSE`). StarBreaker: MIT. Star Citizen game data is
© Cloud Imperium Games and is never committed here.
