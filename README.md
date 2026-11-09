# SC_UE_ModelViewer

A model browser and viewer for Star Citizen assets — ships, items, characters and
interiors straight from `Data.p4k` — with textures and triggerable animations, plus an
export path into Unreal Engine 5 that sets assets up for Nanite and Lumen.

Built on the Rust crates of [StarBreaker](https://github.com/diogotr7/StarBreaker)
(MIT), vendored as a git submodule at `vendor/StarBreaker`.

## Status

**M0 — bootstrap.** Analysis and plan are written; no viewer code yet.

| Doc | Purpose |
| --- | --- |
| [docs/01-starbreaker-analysis.md](docs/01-starbreaker-analysis.md) | what StarBreaker provides, what was verified on this machine, the gaps |
| [docs/02-implementation-plan.md](docs/02-implementation-plan.md) | architecture decisions, milestones, work packages, agent orchestration |
| [docs/03-unreal-export-design.md](docs/03-unreal-export-design.md) | UE plugin design: asset mapping, Nanite/Lumen rules, direct-import FFI |
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
crates/scviewer-core   Rust: Session over p4k + DataCore, GLB/decomposed export, clip catalogue
crates/scviewer-cli    thin CLI for verification
crates/scbridge-ffi    C-ABI library for the Unreal plugin (M4)
app/                   Tauri 2 + React + three.js viewer
ue/SCTest              UE test project with a Lumen level
ue/Plugins/SCBridge    UE editor plugin: package importer, materials, rig/anim, direct import
vendor/StarBreaker     upstream toolkit (submodule, pinned)
scripts/               helpers (starbreaker.sh, glbinfo.py, golden.sh …)
```

## License

This repository: MIT (see `LICENSE`). StarBreaker: MIT. Star Citizen game data is
© Cloud Imperium Games and is never committed here.
