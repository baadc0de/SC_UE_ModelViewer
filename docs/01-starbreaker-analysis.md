# StarBreaker analysis (as of upstream `7d3f01e`, 2026-10-15)

Analysis written 2026-11-09 against the local install
`~/Games/star-citizen/drive_c/Program Files/Roberts Space Industries/StarCitizen/LIVE`
(build `sc-alpha-4.10.2`, version `4.10.197.2878`, `Data.p4k` = 156 GB).

## What StarBreaker is today

StarBreaker (MIT, github.com/diogotr7/StarBreaker) was rewritten from C# into a
**Rust workspace** (edition 2024). The legacy C# lives on the `master` branch and
is irrelevant for us. The Rust tree is vendored at `vendor/StarBreaker`.

| Layer | Crate / dir | What we get |
| --- | --- | --- |
| Archive | `starbreaker-p4k` | mmap'd `Data.p4k` reader, AES/zstd, case-insensitive lookup, dir listing, search. Install auto-discovery for the Linux Wine path we use. |
| Database | `starbreaker-datacore` | `.dcb` parser, record query by dotted property path, loadout resolution. |
| XML | `starbreaker-cryxml` | CryXmlB → XML (`.mtl`, `.cdf`, `.chrparams`, `.xml`). |
| Geometry | `starbreaker-chunks`, `starbreaker-3d::{ivo,nmc,skeleton,types,dequant}` | IVO/CrCh chunk files: `.cga/.cgam`, `.cgf/.cgfm`, `.skin/.skinm`, `.chr`. Positions, UVs, normals, tangents, colours, BoneMap12 weights, NMC node table, rigid‑vs‑soft submesh split. |
| Materials | `starbreaker-3d::mtl`, `material_slots`, `pipeline/textures`, `starbreaker-dds` | `.mtl` parsing incl. shader family classification (Illum, HardSurface, Glass, Layer, MeshDecal/POM, …), `StringGenMask` decode, tint palettes / paints, DDS (BC1‑7, split mips) → PNG, DDNA normal+smoothness → roughness derivation. |
| Assembly | `starbreaker-3d::pipeline` | `assemble_glb_with_loadout(...)`: entity → full loadout tree (hardpoints, thrusters, seats, weapons), interiors from `.socpak`, lights, helpers; `skin_to_glb` for single files. |
| Animation | `starbreaker-3d::animation`, `chrparams`, `clip` | `.dba`/`.caf` decode (SmallTree48 quats, SNORM positions, keyframe bitmaps), `.chrparams` clip catalogue, Mannequin ADB fragments/tags, `Clip` type, glTF animation emitter (`gltf/animation.rs`). |
| Characters | `starbreaker-3d::character`, `-chf`, `-dna` | `.chf` heads, DNA morphs, armour loadouts, face/body clips. |
| Export | `starbreaker-3d::gltf`, `decomposed.rs`, `starbreaker-blend` | Bundled GLB (textures embedded as PNG) **or** decomposed package: `Packages/<name>/scene.json` + `animations/*.json` + shared `Data/**/*.glb|.blend`, `*.materials.json`, `*.png`. Contract: `vendor/StarBreaker/docs/decomposed-export-contract.md`. |
| Apps | `cli/` (`starbreaker`), `app/` (Tauri 2 + React 19 + three.js 0.186), `mcp/` (MCP server), `blender_addon/` | The Tauri app already has a P4k browser, DataCore browser, a three.js `GeometryPreview` (GLB via `skin_to_glb`), export dialogs, and a character builder. |

Not relevant to us: `starbreaker-ui` (ship MFD screen compositing), `-wwise`/`-wem`
(audio), `-gfx` (SWF).

## Verified on this machine

```
cargo build -p starbreaker            # 30 s wall, debug profile is opt-level 1
starbreaker entity loadout Gladius    # 1.6 s, resolves the full hardpoint tree
starbreaker entity export Gladius gladius.glb --lod 1 --mip 2
                                      # 4.8 s, 67 MB GLB: 1072 nodes, 220 meshes,
                                      # 184 materials, 334 PNG textures, 42 skins,
                                      # KHR_lights_punctual / transmission / ior /
                                      # volume / texture_transform — and 0 animations
```

Toolchain present: rustc 1.99.1, node 26.13, python3 + uv, blender, clang/cmake,
webkit2gtk 2.54 + gtk3 (Tauri deps), Codex CLI 0.167 (`gpt-6-astra`, high).
**Unreal Engine is not installed** on this machine (checked `~/Development`, `/opt`,
`~/UnrealEngine*`). GPU: RTX 4090 (Nanite, Lumen and hardware ray tracing are fine on
Linux/Vulkan). Disk: 84 GB free on `/` — exports and `target/` must stay bounded.

## Gaps we must close (ordered by how much they block the viewer)

1. **Vehicle GLBs carry no animations.** `pipeline/glb_assembly.rs` passes
   `animations: Vec::new()` to `GlbInput` for entity exports (lines ~477 and ~521).
   The emitter (`gltf/animation.rs::add_animations`) exists and is used by the
   character path; vehicle clips are only serialised into decomposed
   `animations/*.json` sidecars (Blender convention: Z-up, `[w,x,y,z]`, per-sample
   axis-swapped). Fix: resolve clips for the root skeleton + child skeletons
   (`chrparams` ∪ DBA catalogue) into `clip::Clip` (bones **by node name**, which
   `shared_clip()` already does from CRC32 hashes) and pass them into `GlbInput`.
   Keep the glTF Y-up convention (clips are emitted under the `CryEngine_Z_up`
   wrapper node like the geometry).
2. **No "clip catalogue" API.** The viewer needs a list before loading anything:
   clip name, source (`chrparams` event / DBA-only), fps, frame count, Mannequin
   fragment + tags (`animation/mannequin.rs`), and which entity/skeleton owns it.
   Today that information is only assembled inside `decomposed.rs`.
3. **Library surface is `pub(crate)` in places.** `pipeline`, `gltf`, `decomposed`
   are crate-private modules with a curated `pub use` list in `lib.rs`. We can
   call `assemble_glb_with_loadout*`, `skin_to_glb`, `resolve_loadout_meshes`,
   `query_animation_controller_source`, `DecomposedExport`, `ExportOptions`. Anything
   else needs a small upstreamable `pub` widening in our submodule branch.
4. **Primary-state transforms are not in the clips** (whitepaper §15.1). Landing
   gear "deployed" vs "retracted" full travel, canopy slide, wing fold are driven
   by Mannequin + vehicle C++ in the game. The clips carry secondary articulation.
   `ExportOptions::apply_default_animation_pose` bakes the `landing_gear_extend`
   pose into the rest pose. The viewer should expose "snap first / snap last /
   play" exactly as the Blender addon does and not promise more.
5. **Materials in GLB are PBR approximations.** Layered/tint-palette materials are
   flattened to colours + textures; decals, POM and glass become glTF extensions.
   Good enough for the viewer. For Unreal we read the `*.materials.json` sidecars
   (shader family, texture slots with semantic roles, palette routing, public
   params) and build Material Instances, not the GLB materials.
6. **Coordinate conventions.** Source is CryEngine right-handed Y-up on disk with
   `xyzw` quaternions; GLB wraps everything under a `CryEngine_Z_up` node. Unreal is
   left-handed Z-up, X forward, centimetres. The UE importer owns that mapping
   (Interchange handles glTF → UE if we import GLB; our own mesh builder must do
   the swap + ×100 scale itself).

## Reusable assets for our agents

- Formats reference: `vendor/StarBreaker/docs/star-citizen-animation-formats-whitepaper.md`
  (IVO, CHR, SKIN, NMC, CDF, chrparams, DBA/CAF, compression, axis mapping).
- Export contract: `vendor/StarBreaker/docs/decomposed-export-contract.md`.
- Interiors: `vendor/StarBreaker/docs/socpak-structure-export-guide.md`.
- Lighting units: `vendor/StarBreaker/docs/star-citizen-lighting-research.md`.
- Working Tauri + three.js reference: `vendor/StarBreaker/app/src/components/geometry-preview.tsx`,
  `app/src-tauri/src/{commands,state,datacore_commands}.rs`, `app/src/lib/commands.ts`.
- Upstream engineering rules worth inheriting: `vendor/StarBreaker/AGENTS.md`
  (root-cause fixes, no per-asset hard-coding, `//!` module headers, sequential
  cargo builds across agents, debug profile for iteration).
- MCP server (`mcp/`) exposes `search_entities`, `entity_loadout`, `p4k_read`,
  `dba_dump`, `mannequin_dump`, `image_preview`, … — build once and point
  `.mcp.json` at it so agents can research game data without shelling out.

## Test entities

| Entity | Why |
| --- | --- |
| `AEGS_Gladius` | small, fast (5 s), 42 skins, landing gear + canopy clips |
| `RSI_Scorpius` | the whitepaper's worked example: 33‑bone gear rig, wings, canopy, asymmetric keys |
| `DRAK_Cutlass_Black` | interior socpak, lights, doors/ramps (interior clips keyed per entity) |
| `masculinedefault.chf` (via `chf to-glb`) | soft-skinned character path, face clips (later milestone) |
