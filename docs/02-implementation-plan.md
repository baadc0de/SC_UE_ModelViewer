# Implementation plan — SC_UE_ModelViewer

Goal: a **standalone Unreal Engine 5.8 application** that is pointed at a Star
Citizen `Data.p4k`, lists the models it contains, loads and processes them, and
displays them **rendered with Nanite and Lumen** — textures, a list of animations
the user can trigger, and (intermediate goal) **first-person exploration of ships
with collision against the model**. Viewing produces proper UE objects (Nanite
static meshes, skeletal meshes, materials, animation sequences); an editor plugin is
an acceptable development vehicle and lets those objects be kept as project assets,
but the final deliverable does not open the Unreal Editor.

Built on the vendored StarBreaker crates (`docs/01-starbreaker-analysis.md`).

Decision history (2026-11-09): the M0 draft planned a Tauri + three.js viewer with UE
as an export target; the owner then asked for UE (Nanite + Lumen) to be the renderer,
then for a standalone app rather than the editor, plus first-person exploration,
plus a local file cache so repeat loads need no processing.

## Architecture decisions

| # | Decision | Why |
| --- | --- | --- |
| D1 | **Reuse StarBreaker crates as path dependencies** (`vendor/StarBreaker`, git submodule; our fixes on branches we upstream). No format code of our own. | Formats are reverse-engineered and validated; our value is the app and the UE bridge. |
| D2 | **The viewer is a UE game-mode application**: project `ue/SCViewer`, plugin `ue/Plugins/SCBridge`. All viewer UI (model list, options, animation panel, first-person HUD) is in-game UMG/Slate, so the same code runs standalone and inside the editor's PIE/viewer level. | One UI, two hosts. The standalone deliverable is the game-mode app; the editor is only a dev host and an optional "keep as assets" tool. |
| D3 | **Choose B: the Editor target launched with `-game`** (WP-U0, 2026-11-09). It builds Nanite and full-resolution complex collision in a game world. The Development Game target with `MeshBuilder`/`NaniteBuilder` fails UBT because it pulls in `UnrealEd`. UI remains in the Runtime module; Shipping is out of scope. | Actual builds and probes are recorded in `ue/README.md`; rendered Nanite, Lumen GI and HWRT were then verified on the RTX 4090 (screenshots in `ue/verification/wp-u0/`). Finding: `BuildFromMeshDescriptions` yields no Lumen cards, distance field or RT geometry — WP-U1 must add them (host B links the editor modules that build them). |
| D4 | **Rust ↔ UE boundary is a C-ABI library** (`crates/scbridge-ffi`, `libscbridge.so`): open archive, search entities, clip catalogue, export an entity as an in-memory **decomposed package** (same contract as on disk). | One documented contract (`decomposed-export-contract.md`) carries geometry, material semantics, lights, hierarchy, clips. In memory avoids 100 MB folders per view; the on-disk form remains for debugging and CLI tests. |
| D5 | **Importer owns the UE mapping at runtime**: transient `UStaticMesh` built from `FMeshDescription` (Nanite on), `UMaterialInstanceDynamic` from a master-material set per shader family, `USkeleton` + `USkeletalMesh` + `UAnimSequence` built in memory, lights, `ASCVehicle` actor from `scene.json`. In the editor host the same objects can be saved as assets ("Keep"). | Full control of Nanite settings, UV/tangent/colour preservation, axes/units, naming. Interchange is a baseline for validation only. |
| D6 | **Ships = hybrid actor**: rigid parts → Nanite static meshes on skeleton sockets; soft-skinned parts → skeletal mesh; Glass / MeshDecal → non-Nanite components. | Matches StarBreaker's rigid-vs-soft split; production Nanite is static-mesh Nanite; Lumen wants opaque Nanite geometry and reads Illum glow as emissive. |
| D7 | **Per-mesh complex collision in the chosen Editor `-game` host**, with `CTF_UseComplexAsSimple`. WP-U0 cooked and traced a full-resolution 100,352-triangle Nanite fallback mesh at runtime; choose `FallbackTarget=PercentTriangles` explicitly to avoid automatic reduction. A first-person `ACharacter` supplies capsule, gravity, crouch and fly/noclip. | Triangle-accurate ship interiors require retained geometry. Reduced fallback collision is not validated for corridors. Game-target cooking remains untested because that host does not compile with the builder dependencies. |
| D8 | **No FBX, no Interchange in the main path.** | FBX loses metadata; Interchange cannot express our material semantics, sockets or runtime transient objects. |
| D11 | **Emissive, decal and light appearance is derived from game data only** — `.mtl` authored attributes (`Emissive × Glow × Diffuse × texture`, MeshDecal POM/colour/stencil signals, palette channels), vehicle-XML `MultiLight` ports with their DataCore light items, socpak lights — never from mesh or asset names, never tuned per ship. Shader sources are not in the archive, so CryEngine's documented material semantics are the reference; the executable is datamined only if a rule cannot be established from data. | Owner requirement (2026-11-09): hundreds of models carry emissives and decals; the viewer must get them right automatically. |
| D12 | **Substrate materials** (`r.Substrate=True`, adaptive GBuffer on Vulkan SM6), decided in WP-U2 after a comparison recorded in `ue/verification/WP-U2.md`. | In 5.8.2 Substrate is the default for new projects and not experimental; SC authors diffuse + specular colour, which Substrate takes directly; glass gets coloured transmittance and thin film. Cost: one 8.5 min shader rebuild, +449 MiB DDC; Lumen overview identical before/after. Switching back costs only the material graphs in `ue/scripts/create_materials.py`. |
| D9 | **Linux-first.** UE 5.8.2 at `~/UnrealEngine/5.8`, Vulkan RHI. | Installed on this CachyOS machine with an RTX 4090; Nanite, Lumen and HWRT work on Linux/NVIDIA. |
| D10 | **Two-layer local file cache, keyed by archive build id + entity + options + importer version**, in a directory **relative to the working directory** (`./cache/` by default, gitignored; `--cache <dir>` overrides) with a size cap and LRU eviction. No global or home-directory cache. Layer 1 (Rust): the entity index and the exported decomposed package (tar). Layer 2 (UE): processed results in cooked packages under a mounted content root (WP-U0 mesh proof): Nanite render data and cooked collision; built textures and skeleton/anim data follow in WP-C1. A hit skips processing of the cached objects. | Processing is the expensive part (seconds of export, Nanite build, collision cook); the owner requires that a second load of the same model needs none of it. The editor host's DDC does not exist in the standalone app, so we own the cache. |

## Milestones

| Milestone | Deliverable | Verifies |
| --- | --- | --- |
| **M0 Bootstrap** (done) | repo, submodule, docs, briefs, scripts | `scripts/starbreaker.sh entity loadout Gladius` |
| **M1 Rust core + FFI** | `scviewer-core`, `scbridge-ffi` + header, `scviewer-cli` — *R1 done 2026-11-09; R3, R4 open* | `scviewer decomposed-tar Gladius out.tar`; C smoke program |
| **M2 App MVP** *(U0, U1, V1 done 2026-11-09)* | `ue/SCViewer` game-mode app (runs via editor `-game` at least): p4k picker, model list (search), options, load via FFI → Nanite meshes under Lumen, orbit/free-fly camera, hierarchy panel; **standalone build-path decision recorded**; layer-1 cache (entity index + package) | Gladius grey-textured under Lumen, standalone window, < 30 s cold; second load skips the export |
| **M3 Materials** | master materials, dynamic instances from `materials.json`, textures with correct flags, palettes, emissive glow | Gladius hull/glass/decals/thruster glow right; cockpit lit by emissive |
| **M3b Processed-data cache** | layer-2 cache: Nanite render data, cooked collision, textures, skeleton/anims round-tripped to disk; cache hit → no processing; size cap, eviction, Clear cache | second load of Gladius < 3 s with zero mesh builds; cache survives restart |
| **M4 Animations** *(U3 done 2026-11-09; V2 open)* | skeleton + skeletal meshes, sockets, `UAnimSequence` per clip, clip catalogue; panel: list with Mannequin tags, play/snap/loop/speed/compose | Scorpius gear/canopy/wings; Gladius gear + canopy; Cutlass doors |
| **M5 First-person exploration** | walk mode with collision against the imported geometry incl. interiors, doors via clips, spawn at entrance/seat, fly/noclip toggle, HUD | walk through Cutlass Black interior without falling through; ramps and doors work |
| **M6 Standalone deliverable** | launcher (`scripts/run_viewer.sh` → chosen build path), settings persistence (p4k path, options), no editor UI, cold start < 20 s to model list, error dialogs | fresh shell → app → Gladius in under a minute |
| **M7 Breadth** | characters (`.chf`), interiors as root entities (socpak), paints switching, "Keep as assets" in editor host, batch | Cutlass interior as a location; a character with a face idle |

## Work packages

Self-contained briefs in `docs/work-packages/`, handed to agents verbatim.

| WP | Title | Agent | Depends on | Parallel-safe? |
| --- | --- | --- | --- | --- |
| WP-R1 | **Done** (df22acc + 1de75d0; submodule `scviewer/decomposed-glb`). `scviewer-core`, `scviewer-cli`, layer-1 cache; upstream fix for empty decomposed GLB meshes; cross-reviewed | Codex | — | — |
| WP-R3 | Clip catalogue API (`chrparams` ∪ DBA ∪ Mannequin, lazy) | Codex | R1 | cargo: no |
| WP-R4 | **Done** (72dc4c5 … latest). `scbridge-ffi` cdylib with 12 `sc_*` entry points, ABI v2 cancellable progress callback, animations toggle, C smoke/cancel tests, ThirdParty module + staged `.so`; cross-reviewed, all findings fixed | Codex | — | — |
| WP-R5 | **Done** (1ad3c48 … latest; submodule `scviewer/lights`). Glow-gated emission with forward glow cards, `emissive`/`decal`/`alpha_config` sidecar objects (schema v2), ship SOC light groups → `lights[]`, engine glow from DataCore Glow links narrowed to forward cards with modifier ranges, Blender addon parity; cross-reviewed twice, all findings fixed | Codex | — | — |
| WP-R6 | **Done** (7ba88b8; submodule `scviewer/lights`). Unkeyed bones keep bind, native-order bake, `scviewer glb --default-pose`; the WP-U3 full-pose gate passes (33 bones, 7e-6°, 0 cm) | Codex | — | — |
| WP-R2 | *(optional, upstream contribution)* vehicle clips into bundled GLB animations | Codex | R1 | cargo: no |
| WP-U0 | **Done.** Project + Viewer map; B selected; runtime Nanite data/collision and cooked-package restart proven, then GPU-validated (Nanite, Lumen, HWRT). Open: Interchange baseline (deferred); runtime meshes lack Lumen/RT representation (carried into U1/C1) | Codex | — | — |
| WP-U1 | **Done** (784f70c + 4a44d2c, GPU-validated). Runtime importer: package source (folder + tar), reader, Nanite meshes with Lumen cards / DF / RT geometry, D6 translucent split, collision BVH, `ASCVehicle`; cross-reviewed | Codex | — | — |
| WP-V1 | **Done** (2026-11-09; `ue/verification/WP-V1.md`). Runtime-module app shell: FFI client, step-wise importer (`FSCImportJob`, 30 ms/frame game-thread budget), `USCViewerSubsystem`, Slate browser/hierarchy/footer, orbit + free-fly, editor-host Keep. Gladius cold 10.6 s, warm 3.3 s; unload leaves 0 objects. Codex cross-review follow-ups pending | Opus 5.5 | U1, R4 | UE lock |
| WP-C1 | Processed-data cache (layer 2): serialise built meshes/Nanite/collision/textures/anims, keying, size cap, eviction, hit/miss UI | Codex | U1 (U3 for anim data), U0 round-trip spike | UE lock |
| WP-U2 | **Done** (Substrate; D11 rules from the v2 sidecar objects; DBuffer decals; forward glow cards additive; engine glow driven from the exporter control, nozzle matches the reference; 12 automation tests on three packages; cross-reviewed, all findings fixed). Open: wear/damage layers, stencil palette decals need an exported decal texture, paints dropdown | Opus 5.5 | — | — |
| WP-U3 | **Done** (6da2de6 … 224999e; GPU-validated). Skeleton, skeletal meshes, sockets, `UAnimSequence` per clip, `USCClipCatalog`, `ASCVehicle` play/snap/fragment/toggle API, lights with shared gobo materials and authored states; cross-reviewed, all findings fixed. Full-pose gate passed after WP-R6 | Codex | — | — |
| WP-U3b | **Done** (7b86de2). MegaLights enabled (Cutlass bay 10.0 → 7.0 ms at 1600×900, 28.8 → 16.9 ms at 4K), auto-exposure with bounded EV100 volume, shared gobo materials, data-derived fallback shadow cap | Opus 5.5 | — | — |
| WP-R5b | Light source-of-truth fields (bulb radius, plane size, shadow flag, area clipping, attenuation tweak, fades, importance) and the default-state policy fix (lights off in `defaultState` stay off); format version bump. Motivated by the owner's light-range report; brief `docs/work-packages/WP-R5b-light-fields.md` | Codex | R5 | cargo |
| WP-V2 | Animation panel: grouped clip list with Mannequin chips, transport, snap, compose, bone overlay | Opus 5.5 | V1, U3 | UE lock |
| WP-P1 | First-person exploration: pawn, runtime complex collision, ship-relative gravity, spawn points, doors via clips, fly/noclip, HUD | Codex | U1, U3, V1 | UE lock |
| WP-S1 | Standalone deliverable: build target per D3 decision, launcher script, settings persistence, startup/error UX, no editor UI | Codex | U0 decision, V1 | UE lock |
| WP-Q1 | Verification: golden exports + stats diff, headless UE smoke (import counts, Nanite flags, clip counts, collision presence, screenshots), CI | Opus 5.5 | R1, U1, V1 | yes |
| WP-D1 | Docs upkeep, upstream PRs for submodule patches, user guide | Opus 5.5 | R3 | yes |

Critical path: U0 (spike) → U1 → V1 → P1 → S1, with R1 → R4 feeding V1 and C1 landing
before S1. U0 and R1 start immediately in parallel; U2, U3 and C1 run in parallel
after U1 (disjoint files).

## Agent orchestration rules

- **Orchestrator:** a Claude Code session in this repo dispatches WPs. Codex via the
  `codex:codex-rescue` subagent / `codex` CLI (model `gpt-6-astra`, reasoning high,
  `approvals_reviewer = user`); Opus 5.5 via the `Agent` tool (`model: "opus"`). The
  dispatcher adds only the current commit, the branch name and anything learned since
  the brief was written.
- **Two build locks.** Rust: one `CARGO_TARGET_DIR=./target`, one cargo build at a
  time. Unreal: one UBT/editor build at a time. The two may overlap. Read-only
  research is always parallel-safe.
- **One writer per file set.** Rust WPs, importer (`SCBridge/Import*`), UI
  (`SCBridge/UI*`), materials (`Content/Materials`), gameplay (`SCBridge/Player*`)
  touch disjoint trees; cross-tree needs are reported as `TODO(WP-xx)`.
- **Branching:** one branch per WP (`wp/u1-runtime-importer`), PR into `main`, squash
  merge. Submodule patches on `scviewer/<topic>` branches in `vendor/StarBreaker`,
  pinned by the superproject; WP-D1 upstreams them.
- **Cross-review** by the other model before merge (Codex ⇄ Opus 5.5): done-criteria
  met, upstream rules kept (root-cause fixes, no per-asset hard-coding, `//!` headers),
  verification commands actually run and pasted.
- **Definition of done:** brief's criteria met; `cargo build && cargo test --workspace`
  green where Rust changed; the UE target(s) compile and the headless smoke (WP-Q1)
  passes where UE changed; docs updated when a contract changed.
- **Game data rules:** never set `SC_DATA_P4K`; never commit game-derived files;
  exports to `./out/` or the scratchpad; keep `target/` and the UE `DerivedDataCache/`
  bounded (`cargo sweep --time 30`, no blind `cargo clean`).
- **Scope discipline:** fix the category, not the asset; hard-coded game values are
  bugs — derive from DataCore / P4k.

## Risks and fallbacks

| Risk | Mitigation |
| --- | --- |
| Runtime Nanite build is not available in a game target in 5.8 | D3 fallback: Editor target with `-game`; the user accepted bundling editor functionality. The spike settles it in week one. |
| Runtime complex-collision cooking unavailable or too slow for 1000 meshes | cook on a worker thread with progress; cache cooked trimeshes keyed by mesh hash on disk; fall back to Nanite fallback mesh collision or simplified convex hulls per part |
| Nanite build time per view | background task + progress on first load; layer-2 cache (D10) makes later loads processing-free; WP-U0 proves render-data round-trip in the chosen host |
| Render-data serialisation is not round-trippable in a game target | fall back to the editor `-game` host where `SavePackage` is available and cache as `.uasset` packages under a mounted cache content root |
| Runtime-built meshes invisible to Lumen GI/reflections and HWRT (WP-U0 finding) | WP-U1 builds mesh cards, distance field and RT geometry explicitly (or uses the editor `Build()` path); WP-Q1 asserts no Lumen-scene cut-out; WP-C1 caches those representations |
| Nanite rejects a mesh | per-mesh non-Nanite fallback with a logged reason |
| Translucent / decal materials under Nanite | D6: separate non-Nanite components |
| FFI crashes take the app down | `catch_unwind` on every entry; worker thread; fuzz the tar reader |
| Duplicate bone names across sub-rigs misroute clips | whitepaper §14.8 provenance routing; Scorpius regression |
| Walking inside ships: interiors are separate socpak placements with their own transforms | include_interior on; verify container transforms on Cutlass; door clips via Mannequin fragments |
| Disk at 95 % | cache size cap (default 20 GB) with LRU eviction, Clear cache button, `df` check with a clear error |
| Upstream StarBreaker moves fast | submodule pinned; patches small and upstreamed early |
