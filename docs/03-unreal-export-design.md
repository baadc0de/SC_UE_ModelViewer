# Unreal app and importer design

Target: Unreal Engine 5.8.2 (Linux, `~/UnrealEngine/5.8`, Vulkan RHI). Project
`ue/SCViewer` (C++). Plugin `ue/Plugins/SCBridge`:

| Module | Type | Contents |
| --- | --- | --- |
| `SCBridgeLib` | ThirdParty | links `libscbridge.so`, ships `scbridge.h` |
| `SCBridge` | Runtime | FFI client, package source + reader, runtime importers, `ASCVehicle`, `USCClipCatalog`, player pawn, collision, in-game UI (UMG/Slate), app game mode |
| `SCBridgeEditor` | Editor (optional host) | "Keep as assets" (save transient objects to `/Game/SC/<Entity>/`), viewer-level helpers, Interchange baseline tooling |

Everything the user touches lives in the **Runtime** module so it works in the
standalone app. The editor module only adds persistence.

## Hosts and the standalone build path (decision gate, WP-U0)

| Host | How it runs | Status |
| --- | --- | --- |
| Editor PIE / viewer level | `UnrealEditor SCViewer.uproject`, open `Viewer` map, Play | dev host |
| **Editor-target `-game`** | `UnrealEditor SCViewer.uproject -game -windowed -resx=…` (or the project's own `SCViewerEditor` binary) — editor code compiled in, no editor UI, a normal game window | guaranteed fallback for the standalone app |
| **Development game target** | `SCViewer` target (`TargetType.Game`, `Development`) linking the developer modules needed to build Nanite (`NaniteBuilder`, `MeshBuilder`, `MeshDescription`, `StaticMeshDescription`) and cook Chaos trimeshes at runtime | preferred if the spike proves it builds and renders Nanite |

WP-U0 builds both, records what compiles, what builds Nanite at runtime, how long a
Gladius-sized import takes in each, and picks. `Shipping` is out of scope (developer
modules are stripped there). The launcher script hides the choice from the user.

## How a view happens (same in every host)

```
[Model browser UI]  p4k path (auto-discovered or picked) ──► sc_open()
       │ search "gladius" ──► sc_search_entities()  (FFI, worker thread)
       │ pick entity + options (LOD, mip, attachments, interior, lights, animations, collision)
       ▼
sc_export_decomposed_mem()  ──►  in-memory tar: scene.json, Data/**.glb,
       │                          **.materials.json, **.png, animations/*.json
       ▼
FSCPackageSource(memory)  ──►  FSCImporter (worker thread for parsing/decoding, game thread for UObject creation)
       ├─ FSCMeshImporter      GLB → FMeshDescription → transient UStaticMesh (Nanite) + complex collision
       ├─ FSCTextureImporter   PNG → transient UTexture2D (flags by role)
       ├─ FSCMaterialBuilder   materials.json → UMaterialInstanceDynamic on master materials
       ├─ FSCSkeletonBuilder   bones → USkeleton; soft parts → USkeletalMesh
       ├─ FSCAnimImporter      animations/*.json → UAnimSequence + USCClipCatalog
       └─ FSCActorAssembler    scene.json → ASCVehicle (components, sockets, lights)
       ▼
spawn ASCVehicle in the Viewer level (Lumen GI + reflections, Nanite, HWRT)
       ▼
[UI] hierarchy · clip list + transport · camera mode (orbit / fly / first person) · (editor host: Keep)
```

Loading a second entity destroys the previous actor and its transient objects
(explicit `MarkAsGarbage` + GC) so memory stays bounded. The on-disk form of the same
package (`scviewer decomposed <entity> out/dec`) feeds the folder package source for
tests.

## Inputs (decomposed package contract)

`vendor/StarBreaker/docs/decomposed-export-contract.md`:

```
Packages/<Entity>_LOD<n>_TEX<m>/scene.json        # hierarchy, children, interiors, lights, animation index
Packages/<Entity>_LOD<n>_TEX<m>/animations/*.json # per clip; bones keyed by CRC32 hash; Blender Z-up, [w,x,y,z]
Packages/<Entity>_LOD<n>_TEX<m>/palettes.json | liveries.json | paints.json
Data/**/*.glb                                      # one mesh per source geometry (glTF Y-up, metres)
Data/**/*.materials.json                           # per .mtl: submaterials, shader family, texture slots, params
Data/**/*.png                                      # textures (DDNA normal w/ smoothness alpha, derived *_roughness)
```

## Object mapping

| Source | UE object (transient at runtime; asset when kept) | Notes |
| --- | --- | --- |
| mesh `.glb` (rigid, opaque/masked) | `UStaticMesh`, **Nanite on**, fallback relative error 1 % | `FMeshDescription` from GLB primitives: metres → cm (×100); glTF Y-up RH → UE Z-up LH, matrix locked by a unit test against the Interchange baseline and DataCore ship dimensions. Keep normals/tangents, UV0/UV1, vertex colours. |
| same mesh, collision | `UBodySetup` complex trimesh (Chaos, cooked at runtime) or Nanite fallback-mesh collision | per D7; `CollisionTraceFlag = UseComplexAsSimple`; cache cooked data by mesh hash under the app's cache dir |
| soft-skinned submesh set | `USkeletalMesh` + `USkeleton` from CHR bones | Nanite off by default (option; experimental) |
| `.mtl` submaterial | `UMaterialInstanceDynamic` of a master per shader family | Illum, HardSurface, Glass, Layered (tint palette), MeshDecal (POM), Emissive/Unlit (screens, holograms), NoDraw (skipped), Fallback |
| texture slot | `UTexture2D` | sRGB only for colour roles; DDNA → `TC_Normalmap` linear; roughness/metal/masks → linear; dedupe by source path; mip 0 textures built on a worker thread |
| `interiors[].lights` | point / spot / rect light components | `intensity_candela_proxy` (cd), `radius_m` ×100, linear colour, half-angles, gobo → light function; `ambient_proxy` skipped with a log |
| clip sidecar | `UAnimSequence` on the skeleton | Blender Z-up wxyz → UE; linear keys at clip fps; hemisphere-align; `bind · (start⁻¹ · sample)` when `start_rotation` present; hash-collision arrays by provenance (whitepaper §14.8) |
| Mannequin fragments | `USCClipCatalog` | clip id, display name, owner entity, source, fragment/tags/scopes, boomerang flag |
| scene hierarchy | `ASCVehicle` | root scene component; rigid meshes as `UStaticMeshComponent` on sockets named after their owning bone; soft parts as `USkeletalMeshComponent`; children via `local_transform_sc` (`gltf_y_up`); interiors via `container_transform` / placements; `SpawnPoints` derived from seats, entrances (door helpers) and interior floors |

## Nanite / Lumen rules

- Opaque and Masked → Nanite static meshes. Translucent (Glass) and MeshDecal →
  separate non-Nanite components (decals optionally as DBuffer decal components).
- Emissive: Illum glow → emissive colour × strength so Lumen GI sees it; thruster
  targets (`controls.engine_glow`) get an exposed scalar.
- Two-sided from `is_double_sided`; mesh-decal hosts get DBuffer response.
- One `UStaticMesh` per source geometry path, instanced across components.
- `Viewer` map: Lumen GI + reflections, HWRT on, VSM, sky + directional light, neutral
  floor (collision), fixed exposure; a "space" variant with no floor for free-fly.

## Local file cache (D10, WP-R1 + WP-C1)

Root: `./cache/` **relative to the working directory the app or CLI is launched
from** (the repo root in development; gitignored), overridable with `--cache <dir>`
or in settings. Never a global or home-directory location. Key material: archive build id (`BuildId` from
`build_manifest.id`, else `Data.p4k` size + mtime), entity class name, the full
options struct, and a cache-format version per layer. A `manifest.json` per root
tracks entries, sizes and last-use for LRU eviction under a size cap (default 20 GB).

| Layer | Owner | Content | Hit means |
| --- | --- | --- | --- |
| 1a entity index | Rust (`scviewer-core`, dir passed to `sc_open`) | `entities.<buildid>.bin` | model list appears without walking DataCore |
| 1b package | Rust | `<key>.tar` — the decomposed package | no StarBreaker decode/export |
| 2 processed data | UE (`FSCProcessedCache`) | per mesh hash: `FStaticMeshRenderData` incl. `Nanite::FResources` + cooked `UBodySetup`; per texture hash: platform data; per entity: skeleton, `UAnimSequence` raw data, `USCClipCatalog`, assembled `scene` | no Nanite build, no collision cook, no texture build — objects are reconstructed from blobs and the actor is assembled |

Serialisation uses the engine's own runtime `Serialize` paths (the same code cooked
assets load through) via `FMemoryWriter`/`FMemoryReader` with a proxy archive; WP-U0
proves the round-trip for Nanite render data and `UBodySetup` in the chosen host. If
the game target cannot write them, the editor `-game` host caches `.uasset` packages
under a mounted content root (`/SCCache/`) instead — same key, same API. The footer
shows per-layer hit/miss and sizes; settings offer Clear cache and the cap.

## Player modes (WP-P1)

| Mode | Pawn | Collision | Notes |
| --- | --- | --- | --- |
| Orbit | camera actor | none | default after load; fit to bounds; double-click component to focus |
| Fly | `ADefaultPawn`-style | optional (noclip toggle) | WASD + mouse, speed wheel |
| First person | `ACharacter` (capsule 34 × 88 cm default, crouch 44) | complex collision of every component | gravity along the ship's up axis (ship is spawned level so world −Z); walk/run/crouch/jump; interact key triggers the nearest door clip (fragments tagged `Door`/`Open`/`Close` or clip names of the owning entity); F toggles flashlight (spot on camera) for unlit interiors; teleport to spawn point list |

Collision is applied to the components of the loaded actor only; the floor of the
Viewer map is the only other collider.

## In-game UI (WP-V1, WP-V2)

- Startup: p4k path (auto-discovered via `sc_open(NULL)`, else a file picker),
  build branch shown; remembered in `Saved/Config` (`USCViewerSettings`).
- Model browser: search box → virtualised list (display name, class name, category),
  recent list; options panel (LOD 0–3, mip 0/2/4, attachments, interior, lights,
  shields, animations, default pose, collision, Nanite skeletal); Load with progress
  (export phase → import phases) and cancel.
- Hierarchy panel: component tree with badges (static/skeletal/light, Nanite yes/no,
  triangles); click focuses / isolates.
- Animation panel: grouped clip list, Mannequin tag chips, transport, snap first/last,
  compose, bone overlay; fragment mode.
- Footer: timings (export, mesh build, collision cook, total), cache hit/miss per
  layer and cache size, memory, `df` headroom;
  in the editor host a Keep button.

## FFI (`crates/scbridge-ffi`, `scbridge.h`)

```
sc_status  sc_open(const char* p4k_or_null, const char* cache_dir_or_null, sc_handle** out);
sc_status  sc_search_entities(sc_handle*, const char* query, uint32_t limit, char** json);
sc_status  sc_clip_catalogue(sc_handle*, const char* entity, char** json);
sc_status  sc_export_decomposed_mem(sc_handle*, const char* entity, const sc_options*,
                                    sc_progress_cb, void* user, sc_blob* out_tar);
const char* sc_last_error(sc_handle*);
void       sc_free_string(char*);  void sc_free_blob(sc_blob*);  void sc_close(sc_handle*);
```
Every entry point is `catch_unwind`; strings UTF-8; the tar is uncompressed; the
progress callback may be called from worker threads.

## Open questions (answered by WP-U0 / WP-U1)

- Which standalone build path (D3) and its import time for Gladius.
- Whether `FStaticMeshRenderData` (with Nanite) and `UBodySetup` cooked data round-trip
  through `FMemoryWriter`/`FMemoryReader` in that host (layer-2 cache feasibility).
- Runtime Chaos trimesh cooking availability and cost; whether the Nanite fallback
  mesh is an acceptable collider for corridors.
- Exact glTF→UE basis matrix (lock with a synthetic-GLB unit test).
- Nanite skeletal mesh status in 5.8 and its flag.
