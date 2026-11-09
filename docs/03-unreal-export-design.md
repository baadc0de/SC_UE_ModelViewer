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

## Hosts and the standalone build path (WP-U0: B selected)

| Host | How it runs | Observed result |
| --- | --- | --- |
| Editor PIE / viewer level | `UnrealEditor SCViewer.uproject`, open `Viewer`, Play | development host; map created and verified headlessly |
| **B: Editor-target `-game`** | `UnrealEditor SCViewer.uproject -game -windowed -resx=1600 -resy=900 -ddc=SCViewerDDC` | **selected**; builds Nanite resources and full-resolution complex collision in a game world; cooked-package reload survives restart and an empty DDC |
| A: Development Game target | `SCViewer Linux Development` linking `NaniteBuilder`, `MeshBuilder`, `MeshDescription`, `StaticMeshDescription` | UBT rejects `MeshBuilder -> MeshUtilities -> MaterialUtilities -> UnrealEd`; no executable for runtime comparisons |

Measurements, exact commands, and errors are in `ue/README.md` and
`ue/verification/WP-U0.md`. The measured geometry is synthetic (100,352 triangles),
not a Gladius import. The test environment has no GPU device nodes; Vulkan reports
zero devices. All successful runtime probes used `-nullrhi`. Nanite **data** and
collision are verified; rendered Nanite, Lumen and HWRT remain a WP-U0 gate on the
GPU-equipped host. Shipping is out of scope. The runtime module owns the console
probes and future user-facing code; build/save APIs remain behind `WITH_EDITOR`.

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

## Emissive, decal and light rules (D11 — data-driven, see WP-R5)

| Signal in the sidecar | Rendering rule |
| --- | --- |
| Illum / HardSurface / LayerBlend emission (**final rule, 2026-11-09, amended the same day after a survey of all 391 Gladius submaterials — see `ue/verification/WP-U2.md`**) | strength = `Glow` when `Glow` > 0; else, for the Illum family (Illum/HardSurface) with CryEngine's `MTL_FLAG_REQUIRE_FORWARD_RENDERING` (`MtlFlags & 0x8000`) and a non-black `Emissive`, strength = 1 with `Emissive` as the tint — these are forward-rendered glow cards (`Headlight_Glow`, `glows_a`, `decal_glow`: the strips that visibly glow in the reference); else 1.0 when an emittance map exists (`%EMITTANCE_MAP` token); else **0**. `LayerBlend_V2` and non-forward materials never emit from `Emissive` alone (upstream lighting research §7: `Emissive=1,1,1` is a neutral baseline there; 42 such Gladius materials are pipes and tiles). Emission = surface colour (`Diffuse` × base-colour texture, or the emittance map) × tint × strength, where tint = `Emissive` when non-black (white otherwise) and strength = `Glow` when `Glow` > 0, else **1** for a forward-flag glow card or an emittance map (`%EMITTANCE_MAP` or `%MACRO_EMISSIVE_MAP`), else 0 — the `Emissive` triplet is applied once, as the tint, never also as a strength (a Cutlass `thruster_glow` with Emissive (1, 0.09, 0) is orange at strength 1, not squared). `AlphaGlowCutoff` gates by alpha. Forward-flag glow cards render as **additive/translucent unlit emissive** (not masked at the default clip; their alpha is a soft 0.3–0.4 gradient) and therefore as non-Nanite split meshes like glass (D6). UE strength = one registered host constant × the strength above. Materials with the forward flag but neither `Emissive` nor `Glow` (main-thruster `engine_glow`, glass trims) have no authored emission: thruster glow is runtime-driven and is handled by `controls.engine_glow`, whose data signal is the DataCore `materialEffects` Glow property link on a mesh node, narrowed to the forward-flag Illum-family cards on that node (deferred materials on the same node do not respond in game); the control's range comes from the link's multiplier / min / max modifiers (WP-R5). |
| MeshDecal, `%PARALLAX_OCCLUSION_MAPPING`, no stencil/tint signal (`relief_only`) | base colour untouched; alpha = TexSlot1.a × `DecalAlphaMultiplier`; normal from the DDNA slot blended over the host; height → parallax/normal darkening (panel grooves). Never opaque. |
| MeshDecal with `%DIFFUSE_MAP` colour sheet (`color_sheet`) | masked/blended colour from the sheet where alpha × `DecalAlphaMultiplier` × `DecalDiffuseOpacity` > 0 |
| MeshDecal with `%STENCIL_MAP` + `$TintPaletteDecal` (`stencil_palette`) | colour from the palette `decal` red/green/blue channels selected by the stencil texture, `StencilDiffuseColor*`, `StencilOpacity` |
| Illum with CryEngine forward flag (`MtlFlags & 0x8000`) | diffuse alpha is coverage (masked); upstream should emit this in `alpha_config` (TODO WP-R5). `MtlFlags & 0x2` = two-sided, not yet honoured |
| GlassPBR | translucent; tint = palette `glass` channel × `GlassTintStrength`; `EmissiveOpacity` |
| Vehicle `MultiLight` port lights (`lights[]` from WP-R5) and `interiors[].lights` | UE light components, same mapping as the interior-light row above |

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
| 2 processed data | UE (`FSCProcessedCache`) | per mesh hash: cooked package holding `FStaticMeshRenderData` incl. `Nanite::FResources` + cooked `UBodySetup`; per texture hash: platform data; per entity: skeleton, `UAnimSequence` raw data, `USCClipCatalog`, assembled `scene` | no Nanite build, no collision cook, no texture build — objects are loaded from packages and the actor is assembled |

WP-U0 selects the **cooked-package fallback** for the installed 5.8.2 host.
Calling `FStaticMeshRenderData::Serialize` directly from a plugin fails to link
(the installed engine does not export it). `UPackage::SavePackage` with the Linux
cooking target and an `ICookedPackageWriter` produces `.uasset`, `.uexp`, and
`.ubulk` under a mounted `/SCCache/` root. Save must wait for distance-field,
Lumen-card and Nanite streaming preparation. An uncooked asset/DDC hit is not
sufficient evidence of a processed cache.

The spike uses `./cache/spike/` relative to the invoking working directory, passed
explicitly by `ue/scripts/run_spike.sh` because UE changes cwd during startup.
A fresh process with an empty DDC loaded the cooked mesh's Nanite resources and
collision and passed a line trace without rebuilding the spike. GPU rendering of
the reloaded data is still blocked by absent GPU access. The package writer is a
single-package feasibility sink, not WP-C1's production cache: WP-C1 must add
version/key checks, atomic publication, dependency handling, LRU/size limits and
texture/skeleton/animation coverage. The application cache must be independent
of the engine DDC; the latter is project-local build scratch in `DerivedDataCache/`.

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
progress callback may be called from worker threads. The tar uses POSIX ustar headers
with explicit Regular typeflag `0`. Long paths split into the 100-byte `name`
and 155-byte `prefix` fields at a slash. Only paths that cannot fit that scheme
use a per-file PAX `x` header carrying the UTF-8 `path` key for the following
regular entry. The reader supports ustar name/prefix joining and PAX path
overrides; the writer emits no GNU `L` / `@LongLink` records. Gladius's entire
package is representable as plain ustar regular entries.

## Open questions after WP-U0

- ~~GPU validation of path B~~ done 2026-11-09 (`ue/README.md` §GPU validation).
  New: runtime-built meshes must gain Lumen mesh cards, a distance field and RT
  geometry (WP-U1) and the cache must carry them (WP-C1).
- Gladius-sized import timing and Interchange baseline (deferred; no cargo in U0).
- Exact glTF→UE basis matrix (lock with a synthetic-GLB unit test in WP-U1).
- WP-C1 cache format/versioning and processed texture/skeleton/animation coverage.
- Nanite skeletal support is present in 5.8 with `r.Nanite.AllowSkinnedMeshes=1`;
  see U0's source/documentation findings. Soft-skin rendering is not tested here.
