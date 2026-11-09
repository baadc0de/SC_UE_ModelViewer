# Implementation plan — SC_UE_ModelViewer

Goal: a desktop **model browser and viewer** for Star Citizen assets that shows
models with textures and lets the user trigger any discoverable animation, plus an
**Unreal Engine 5 export path** that lands assets correctly set up for Nanite and
Lumen. Built on the vendored StarBreaker crates (`docs/01-starbreaker-analysis.md`).

## Architecture decisions

| # | Decision | Why |
| --- | --- | --- |
| D1 | **Reuse StarBreaker crates as path dependencies** (`vendor/StarBreaker`, git submodule, our fixes on a branch we upstream). No re-implementation of formats. | Formats are reverse-engineered and validated; the value we add is the viewer and the UE bridge. |
| D2 | **Viewer = Tauri 2 + React 19 + three.js**, a new app in `app/`, not a fork of upstream `app/`. | Same stack as upstream, so agents can crib proven patterns (`geometry-preview.tsx`, Tauri commands, zustand stores) without inheriting the ship-UI/audio/character sprawl. three.js `GLTFLoader` + `AnimationMixer` give textured skinned meshes and clip playback for free. |
| D3 | **GLB is the in-memory interchange between Rust and the viewer**; the **decomposed package** (scene.json + Data/ + materials.json + PNG) is the interchange to Unreal. | GLB already works and embeds textures; the decomposed contract is the richer, documented format with material semantics and lights, which UE needs. |
| D4 | **Unreal import is a UE editor plugin (`ue/Plugins/SCBridge`)** in two stages: (M3) import a decomposed package from disk; (M4) link the Rust pipeline as a C-ABI library so the editor reads `Data.p4k` directly. | Direct engine API gives correct Nanite/Lumen settings, materials and actor hierarchy; a file-based first stage is testable without FFI and is what the user asked for as the "intermediate" path. |
| D5 | **No FBX.** GLB → UE Interchange for payloads where convenient, our own `FMeshDescription` builder where we need control. | FBX adds a Blender/Assimp hop and loses metadata; Interchange's glTF translator imports static + skeletal meshes, materials and animations. |
| D6 | **Ships in UE = hybrid actor**: rigid parts → Nanite `StaticMesh` attached to skeleton sockets; soft-skinned parts → `SkeletalMesh` + `AnimSequence`. Glass/decals → non-Nanite translucent/masked components. | Matches StarBreaker's rigid-vs-soft split (whitepaper §5.3). Nanite skeletal meshes are still experimental; static-mesh Nanite is production. Lumen needs opaque Nanite or distance fields; emissive Illum glow feeds Lumen GI directly. |
| D7 | **Linux-first.** UE 5.8.2 Linux editor at `~/UnrealEngine/5.8` (Vulkan RHI). | Already installed on this CachyOS machine with an RTX 4090; Nanite, Lumen and HWRT work on Linux/NVIDIA. WP-U0 only creates the test project and smoke-tests the renderer. |

## Milestones

| Milestone | Deliverable | Verifies |
| --- | --- | --- |
| **M0 Bootstrap** (this commit) | repo, submodule, docs, work-package briefs, helper scripts | `scripts/starbreaker.sh entity loadout Gladius` |
| **M1 Viewer MVP** | search entities (DataCore) + browse P4k geometry, load as textured GLB in three.js, LOD/mip/material controls, orbit + fit camera, hierarchy tree | Gladius renders with textures in < 10 s |
| **M2 Animations** | clip catalogue per entity (name, source, fps, frames, Mannequin tags), play/pause/scrub/loop/speed, snap-first/last, compose several clips, bone overlay | Scorpius: gear, canopy, wings play; Gladius gear; Cutlass doors |
| **M3 UE export (file)** | "Export for Unreal" writes a decomposed package; UE plugin `SCBridge` imports it: Nanite static meshes, Material Instances from a master-material set, skeletal mesh + AnimSequences, actor Blueprint with hierarchy, lights | Gladius in a Lumen test level, gear anim plays, decals/glass correct |
| **M4 UE direct import** | `scbridge-ffi` (Rust cdylib, C ABI) + editor tab "Import from Star Citizen": search, pick LOD/mip, import without leaving the editor | same result as M3 without an intermediate folder |
| **M5 Breadth** | characters (`.chf`/armour), interiors (socpak), paints/palettes switching, batch export | Cutlass interior with lights in UE; a character with face idle |

## Work packages

Each WP has a self-contained brief in `docs/work-packages/` that is handed to the
agent verbatim. Dependencies, agent and parallel-safety are summarised here.

| WP | Title | Agent | Depends on | Parallel-safe? |
| --- | --- | --- | --- | --- |
| WP-R1 | Rust workspace + `scviewer-core` crate wrapping StarBreaker (open p4k/dcb, search, export GLB in memory, decomposed to disk) | Codex | — | builds: no |
| WP-R2 | Vehicle clips into GLB animations (patch `glb_assembly.rs`, submodule branch) | Codex | R1 | builds: no |
| WP-R3 | Clip catalogue API (`chrparams` ∪ DBA ∪ Mannequin fragments, per skeleton) | Codex | R1 | builds: no |
| WP-A1 | Tauri app scaffold: window, p4k discovery/open, entity search, geometry browser, zustand stores, IPC bindings | Opus 5.5 | R1 | research yes; builds no |
| WP-A2 | three.js viewer: GLB load, textures, lights/env, grid, fit, hierarchy panel, LOD/mip/material controls | Opus 5.5 | A1 | after A1 |
| WP-A3 | Animation UI: catalogue list, play/scrub/loop/speed, snap first/last, multi-clip compose, bone/skeleton overlay | Opus 5.5 | A2, R2, R3 | after A2 |
| WP-U0 | UE 5.8.2 test project `ue/SCTest.uproject` with a Lumen test level, Vulkan/Nanite/Lumen/HWRT smoke test, Interchange GLB smoke import | Codex | — | yes (no cargo) |
| WP-U1 | `SCBridge` plugin: decomposed-package reader (scene.json/materials.json), static meshes from `Data/*.glb` via Interchange or MeshDescription, Nanite settings, UE axis/scale mapping | Codex | U0, R1 (export) | yes vs Rust builds |
| WP-U2 | Materials: master-material set per shader family (Illum, HardSurface, Glass, Layered/Tint, MeshDecal/POM, Emissive) + Material Instance builder from sidecars, texture import with correct sRGB/normal/roughness flags | Opus 5.5 | U1 | after U1 |
| WP-U3 | Rig + animation: SkeletalMesh for soft-skinned parts, skeleton from CHR bones, AnimSequences from clips, sockets for rigid parts, `SCVehicle` actor BP assembly, lights from `interiors[].lights` | Codex | U1, R2, R3 | after U1 |
| WP-U4 | `scbridge-ffi` C-ABI cdylib + UE ThirdParty module + "Import from Star Citizen" editor tab | Codex | R1–R3, U1–U3 | builds: no |
| WP-Q1 | Verification harness: golden entities (Gladius, Scorpius, Cutlass), GLB stats checks (`scripts/glbinfo.py`), screenshot smoke in Tauri, UE commandlet import test | Opus 5.5 | A2, U1 | yes |
| WP-D1 | Docs upkeep + upstream PRs for submodule patches (R2/R3 widenings) | Opus 5.5 | R2, R3 | yes |

Critical path: R1 → R2/R3 → A3 and R1 → U1 → U3 → U4. A1/A2 start as soon as R1
exposes the first two commands; U0 starts immediately.

## Agent orchestration rules

- **Orchestrator:** a Claude Code session in this repo dispatches WPs. Codex via
  the `codex:codex-rescue` subagent / `codex` CLI (configured model `gpt-6-astra`,
  reasoning high, `approvals_reviewer = user`); Opus 5.5 via the `Agent` tool
  (`model: "opus"`). Briefs are the `docs/work-packages/WP-*.md` files; the
  dispatcher adds only the current commit, the branch name and anything newly learned.
- **Builds are sequential.** All Rust (ours + vendored) shares one
  `CARGO_TARGET_DIR=./target`; two agents must never run `cargo build`/`test` at
  once. UE builds (`ue/`) and npm builds (`app/`) are independent of cargo and may
  overlap with it. Read-only research is always parallel-safe.
- **One writer per file set.** Rust WPs and UI WPs touch disjoint trees; a WP that
  needs a change outside its tree writes a `TODO(WP-xx)` note in its report rather
  than editing it.
- **Branching:** one branch per WP (`wp/r2-glb-animations`), PR into `main`,
  squash merge. Submodule patches land on a branch in `vendor/StarBreaker` named
  `scviewer/<topic>` and are pinned by the superproject commit; WP-D1 turns them into
  upstream PRs.
- **Cross-review:** every PR is reviewed by the *other* model before merge
  (Codex work → Opus review, Opus work → Codex review), checking the done-criteria
  in the brief, upstream's rules (root-cause fixes, no per-asset hard-coding,
  `//!` headers), and that verification commands were actually run.
- **Definition of done** for every WP: brief's done-criteria met, verification
  commands pasted into the PR, `cargo build && cargo test --workspace` green
  (debug), `npm run build` green where touched, UE plugin compiles where touched,
  docs updated if a contract changed.
- **Game data rules:** never set `SC_DATA_P4K` (auto-discovery finds LIVE);
  never commit game-derived files (see `.gitignore`); exports go to `./out/`
  (gitignored) or the session scratchpad; keep `target/` bounded with
  `cargo sweep --time 30`, never a blind `cargo clean`.
- **Scope discipline:** fix the category, not the asset (no "if ship == Gladius").
  Hard-coded game values are a bug; derive from DataCore/P4k.

## Risks and fallbacks

| Risk | Mitigation |
| --- | --- |
| Vehicle clip → node-name binding fails for sub-rig duplicates (same bone names front/rear gear) | whitepaper §14.8 routing rules; test on Scorpius; fall back to per-skeleton clips scoped to the child entity node |
| Nanite rejects a mesh (non-manifold, > 64 UV sets, WPO) | import opaque Nanite with fallback mesh; log per-mesh reasons; non-Nanite fallback per component |
| Translucent/decal materials under Nanite | D6: glass/decals as separate non-Nanite components; decals optionally as DBuffer decals |
| UE project DerivedDataCache + Intermediate growth on a 95 % full disk | keep `ue/SCTest` DDC local and bounded; `Saved/` and `Intermediate/` are gitignored; check `df` before Nanite builds of large interiors |
| three.js chokes on 1000+ nodes / 300 textures at mip 0 | default mip 2 / LOD 1 as upstream; lazy texture upload; merge-by-material toggle |
| Upstream StarBreaker moves fast | submodule pinned; patches small and upstreamed early (WP-D1) |
