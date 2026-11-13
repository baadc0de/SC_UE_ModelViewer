# Unreal app and importer design

Target: Unreal Engine 5.9.0 preview-1 (Linux, `~/UnrealEngine/5.9`, Vulkan RHI; 5.8.2 until 2026-11-12). Project
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
       ├─ FSCTextureImporter   package DDS blocks / PNG → transient UTexture2D (flags by role)
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
Data/**/*.dds                                      # textures: the game's blocks from the requested mip (WP-T1);
                                                   # DDNA normal (BC5), DDNA smoothness alpha (*_smoothness, BC4 / R8)
Data/**/*.png                                      # textures the DDS contract cannot carry (BC6H, cubemaps, ...)
```

## Object mapping

| Source | UE object (transient at runtime; asset when kept) | Notes |
| --- | --- | --- |
| mesh `.glb` (rigid, opaque/masked) | `UStaticMesh`, **Nanite on**, fallback relative error 1 % | `FMeshDescription` from GLB primitives: metres → cm (×100); glTF Y-up RH → UE Z-up LH, matrix locked by a unit test against the Interchange baseline and DataCore ship dimensions. Keep normals/tangents, UV0/UV1, vertex colours. |
| same mesh, collision | `UBodySetup` complex trimesh (Chaos, cooked at runtime) or Nanite fallback-mesh collision | per D7; `CollisionTraceFlag = UseComplexAsSimple`; worker-cooked full-resolution triangles; processed caching is WP-C1 |
| soft-skinned submesh set | `USkeletalMesh` + `USkeleton` from CHR bones | Nanite off by default (option; experimental). One render section per material slot; each section's bone map lists only the joints that weight its vertices (influences index into it); the LOD's required/active bones stay the whole skeleton (sockets and attachments) |
| `.mtl` submaterial | `UMaterialInstanceDynamic` of a master per shader family | Illum, HardSurface, Glass, Layered (tint palette), MeshDecal (POM), Emissive/Unlit (screens, holograms), NoDraw (skipped), Fallback. Character families (WP-P2 round 2) map plainly onto the Illum master: `HumanSkin_V2` and `Eye` = base colour, DDNA normal, spec map, authored F0 (no subsurface/DNA/wrinkles); `HairPBR` = masked cards, coverage from the `opacity` slot's red channel (`OpacityMap`), colour from the authored melanin parameters (UE's `GetHairColorFromMelanin`: `BaseMelanin`, `BaseMelaninRedness`, `DyeColor` × `DyeAmount`, × `BaseTintColor`), authored `Diffuse` when the material has no melanin (hair caps). Every master is flagged for skeletal meshes (`bUsedWithSkeletalMesh`): a standalone `-game` session cannot add a usage flag at runtime and would draw the engine's WorldGrid default material on skinned parts. The layered family (HardSurface, Layered) also draws the material-level macro surface from the game's shaders: `TexSlot3` macro normal + gloss over the blended layers (`%MACRO_NORMAL_MAP` / `%NORMAL_MAP`), HardSurface `TexSlot1` macro AO (`%MACRO_AO_MAP`), LayerBlend_V2 `TexSlot13` HAL map (AO, per-layer luminance; hue shift and wear layers not drawn). All lit masters map gloss as the game does: roughness = 1 − min(gloss, 251/255) (`ue/verification/WP-U2.md`, "Macro surface, HAL and the gloss convention") |
| texture slot | `UTexture2D` | A package DDS (`docs/work-packages/WP-T1-compressed-textures.md`) is uploaded as the game's blocks, never decoded: BC1/BC2/BC3/BC4/BC5/BC7, R8, B8G8R8A8, R8G8B8A8 in the matching `EPixelFormat`, the mips as stored, the colour encoding from its DXGI format (the exporter writes the `_SRGB` variant for the game's sRGB flag). The bridge (`sc_dds_prepare`) re-biases BC4/BC5 SNORM blocks to UNORM (UE has no SNORM block formats) and, for a DDNA normal map, negates green in the blocks (glTF/OpenGL +Y → UE −Y); single-channel formats sample as (R, 0, 0, 1), as in the game. The DDNA smoothness map is the `_ddna` alpha mips as stored, read by the masters as smoothness (`SmoothnessMap`); the exporter's PNG fallback in its place (derived roughness sqrt(1 − s)) is decoded back to smoothness 1 − r² before its mips are built (`ESCTextureRole::DerivedRoughness`). A PNG (exporter fallback, light gobos) keeps the BGRA8 path: encoding from its `sRGB` / `gAMA` chunk, else the slot role. DDNA → `TC_Normalmap` linear; smoothness/masks → linear; dedupe by source path; decode on worker threads. The viewer exports mip 0 (full resolution); the budget counts uploaded bytes |
| `interiors[].lights` | point / spot / rect light components | Authored intensity, bulb/plane size, attenuation and shadow fields below; `radius_m` ×100, linear colour, half-angles, gobo → light function. `intensity_candela_proxy` is the legacy `raw_x1500` proxy, not calibrated cd; physical mapping belongs to UE (the game renderer's light model, WP-U3b §13.2). `ambient_proxy` skipped with a log. Positions are final in the export: a tile-attached light group sits on its container item port (`interactionOffset`, owner frame), not on its `.soc` position (WP-U3b §12.2) |
| `interiors[].room_volumes` | lighting channels (`FSCLightAreas`) | Box / prism / trimesh room extents from `entitycomponentroom.xml` areas. A light with `affects_this_area_only` lights only its own area in the engine (clip volumes); UE maps this onto its three lighting channels (WP-L1): channel 0 = outside every room; each room takes channel 1 or 2, coloured to minimise the area-only lights that reach a room sharing their channel (attenuation sphere vs the room's volume bounds; every colouring tried up to 16 rooms). Area-only lights take their room's channel, other lights all three; primitives take the room most of their rest-pose vertex samples are in; environment lights get all three; the player body follows the capsule. Shadows: only the authored `shadow_casting` lights |
| clip sidecar | `UAnimSequence`s on the skeleton | Blender Z-up wxyz → UE; linear keys, each track at the rate and length of its own source animation (clip-level when the clip lists no `sources`); a binding holds one sequence per source timing (rate and length) with that animation's keys verbatim; hemisphere-align; `bind · (start⁻¹ · sample)` when the source has a `start_rotation`; hash-collision arrays by provenance (whitepaper §14.8) |
| Mannequin fragments | `USCClipCatalog` | clip id, display name, owner entity, source, fragment/tags/scopes, boomerang flag |
| scene hierarchy | `ASCVehicle` | root scene component; rigid meshes as `UStaticMeshComponent` on sockets named after their owning bone; soft parts as `USkeletalMeshComponent`; children via `local_transform_sc` (`gltf_y_up`); interiors via `container_transform` / placements; `SpawnPoints` derived from seats, entrances (door helpers) and interior floors |

## Emissive, decal and light rules (D11 — data-driven, see WP-R5)

| Signal in the sidecar | Rendering rule |
| --- | --- |
| Illum / HardSurface / LayerBlend emission (**final rule, 2026-11-09, amended the same day after a survey of all 391 Gladius submaterials — see `ue/verification/WP-U2.md`**) | strength = `Glow` when `Glow` > 0; else, for the Illum family (Illum/HardSurface) with CryEngine's `MTL_FLAG_REQUIRE_FORWARD_RENDERING` (`MtlFlags & 0x8000`) and a non-black `Emissive`, strength = 1 with `Emissive` as the tint — these are forward-rendered glow cards (`Headlight_Glow`, `glows_a`, `decal_glow`: the strips that visibly glow in the reference); else 1.0 when an emittance map exists (`%EMITTANCE_MAP` token); else **0**. `LayerBlend_V2` and non-forward materials never emit from `Emissive` alone (upstream lighting research §7: `Emissive=1,1,1` is a neutral baseline there; 42 such Gladius materials are pipes and tiles). Emission = surface colour (`Diffuse` × base-colour texture, or the emittance map) × tint × strength, where tint = `Emissive` when non-black (white otherwise) and strength = `Glow` when `Glow` > 0, else **1** for a forward-flag glow card or an emittance map (`%EMITTANCE_MAP` or `%MACRO_EMISSIVE_MAP`), else 0 — the `Emissive` triplet is applied once, as the tint, never also as a strength (a Cutlass `thruster_glow` with Emissive (1, 0.09, 0) is orange at strength 1, not squared). `AlphaGlowCutoff` gates by alpha. Forward-flag glow cards render as **additive/translucent unlit emissive** (not masked at the default clip; their alpha is a soft 0.3–0.4 gradient) and therefore as non-Nanite split meshes like glass (D6). UE strength is cd/m² from the game shader (`FSCStarEngine`, WP-U3b §13.3): an authored `Glow` emits `SceneUnitNits × sqrGlowFactor × Glow²` (quadratic, `MaterialGlowToLinearGlow`); a strength-1 emitter without a Glow gets one scene unit of instance glow (registered placeholder, WP-U2 fallback register). Materials with the forward flag but neither `Emissive` nor `Glow` (main-thruster `engine_glow`, glass trims) have no authored emission: thruster glow is runtime-driven and is handled by `controls.engine_glow`, whose data signal is the DataCore `materialEffects` Glow property link on a mesh node, narrowed to the forward-flag Illum-family cards on that node (deferred materials on the same node do not respond in game); the control's range comes from the link's multiplier / min / max modifiers (WP-R5); its value is a per-instance glow amount, linear in scene units, not a material Glow (WP-U3b §13.3). |
| MeshDecal, `%PARALLAX_OCCLUSION_MAPPING`, no stencil/tint signal (`relief_only`) | base colour untouched; alpha = TexSlot1.a × `DecalAlphaMultiplier`; normal from the DDNA slot blended over the host; height → parallax/normal darkening (panel grooves). Never opaque. |
| MeshDecal with `%DIFFUSE_MAP` colour sheet (`color_sheet`) | masked/blended colour from the sheet where alpha × `DecalAlphaMultiplier` × `DecalDiffuseOpacity` > 0 |
| MeshDecal with `%STENCIL_MAP` + `$TintPaletteDecal` (`stencil_palette`) | colour from the palette `decal` red/green/blue channels selected by the stencil texture, `StencilDiffuseColor*`, `StencilOpacity` |
| HardSurface with `%STENCIL_MAP` (camo liveries) | stencilLib non-sticker tint of the Primary albedo after its primer, before the wear blend: `TexSlot7` (three distance fields) at (`UseUV2ForStencil` ? UV1 : UV0) × `StencilTiling`, anti-aliased by `fwidth`, channel priority R over G over B, × `StencilOpacity`; colours by `StencilTintOverride` = 2 (or `$TintPaletteDecal`): palette decal red/green/blue, = 1: palette primary/secondary/tertiary, else `StencilDiffuseColor*`. The breakup texture (TexSlot8) is not exported and is taken as neutral (`ue/verification/PAINT-1.md`) |
| Illum with CryEngine forward flag (`MtlFlags & 0x8000`) | diffuse alpha is coverage (masked); upstream should emit this in `alpha_config` (TODO WP-R5). `MtlFlags & 0x2` = two-sided, not yet honoured |
| GlassPBR | translucent clear dielectric (F0 = `Specular.r`); colour only as absorption: `Diffuse`, moved to the palette `glass` channel by `GlassTintStrength` under `%USE_TINT_PALETTE`, × vertex colour / tint map; maps at `baseUV × GlassTiling` (v × `GlassVStretch`); `Opacity` unused; `EmissiveOpacity` not mapped (`ue/verification/WP-U3b.md` §14) |
| Vehicle `MultiLight` port lights (`lights[]` from WP-R5) and `interiors[].lights` | Same authored mapping as above. Always use authored `defaultState`, even intensity 0; only an absent default permits the first positive auxiliary → emergency → cinematic fallback. Preserve all states for the UI. |

WP-R5b light sidecar fields apply identically to `root_entity.lights[]`,
`children[].lights[]`, `interiors[].lights[]`, the light catalogue, and bundled
GLB light extras:

- `bulb_radius_m: f32` (documented CryEngine missing-value default 0.05),
  `plane_width_m`, `plane_height_m`: optional metres.
- `shadow_casting: Option<bool>`, `max_shadow_cast_distance_m`: optional metres;
  `affects_this_area_only: Option<bool>`, `affect_gi: Option<bool>` (unknown flags are null).
- `attenuation_tweak: f32` (default 1), `plane_softness: Option<f32>`,
  `fade_min_distance_m`, `fade_max_distance_m`: optional metres.
- `importance: Option<i32>`, `glow_multiplier: Option<f32>`.
- `intensity_candela_proxy_model: "raw_x1500"` on the flat record and states;
  `intensity_unit` remains `"cryengine_authored_intensity"`. The existing ambient
  proxy additionally applies glow multiplier ×0.25; all proxy maths is unchanged.

Sparse SOC light components inherit each missing attribute from their entity
class's `LightComponentParams`, read from DataCore at export time. This includes
nested state intensity/temperature, size, shadows, area/GI flags, fades and
importance. An explicit zero/false/empty value overrides the class. Only when
both sources lack an attribute do the documented bulb-radius 0.05 and attenuation
1 defaults apply; unresolved radius, state temperature/intensity and proxy values
are nullable. Unknown GI retains its mode and null value, with one warning per
export; absent importance enum metadata likewise warns once per export.
Resolved light type, colour, style, state temperature-use flag and preset tag
also remain null if absent from both the component and class record. Consumers
must distinguish unknown values from explicit zero/false values.

Blender keeps shadows enabled for area-contained lights, otherwise honouring
the authored shadow flag.

Unknown optional numeric fields are null; authored zeroes remain zero. Scene and
material schemas stay v2. `scviewer-core::CACHE_FORMAT_VERSION` is **12** (was 11)
so old layer-1 packages miss. `SCPackageReader` continues accepting scene v1/v2;
it must read the additive v2 light fields, not require a scene version 12.

WP-R5b enum detail: LIVE `importance` is often a name. `importance_name` preserves
it and `importance` exports the DataCore property enum's zero-based option index,
resolved from the current database rather than a copied table. `affect_gi_mode`
preserves `On` / `Off` / `InheritAffectsObjects`; `affect_gi` resolves the latter
from the component's `affectsObjects` flag. Numeric importance and boolean GI
spellings remain supported. Missing importance is null; standalone SOC parsing
without DataCore keeps the name with a null index until interior assembly.

## Nanite / Lumen rules

- Opaque and Masked → Nanite static meshes. Translucent (Glass) and MeshDecal →
  separate non-Nanite components (decals optionally as DBuffer decal components).
- Emissive: Illum glow → emissive colour × strength so Lumen GI sees it; thruster
  targets (`controls.engine_glow`) get an exposed scalar.
- Two-sided from `is_double_sided`; mesh-decal hosts get DBuffer response.
- One `UStaticMesh` per source geometry path, instanced across components.
- `Viewer` map: Lumen GI + reflections, HWRT on, VSM, sky + directional light, neutral
  floor (collision); exposure and display mapping are the game renderer's, installed at run time
  (WP-U3b §13.4; the default map is a closed hangar since §11.9); a "space" variant with no floor for free-fly.

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
| First person | `ACharacter` (capsule 34 × 88 cm default, crouch 44) | complex collision of every component | gravity along the ship's up axis (ship is spawned level so world −Z); walk/run/crouch/jump; E toggles the nearest receiving joint within 3 m using Mannequin door/canopy/hatch/entrance/ramp fragments and Open/Close tags, or a DataCore Door owner; F toggles flashlight (spot on camera) for unlit interiors; teleport to spawn point list |

Collision is applied to the components of the loaded actor only; the floor of the
Viewer map is the only other collider.

WP-P3 (game collision): when a mesh asset carries the game's CryPhysics colliders (`extras.sc_physics`, see the
StarBreaker decomposed-export contract), the pawn walks on those instead of render triangles. Each rig owner gets a
hidden collision-only component blocking `ECC_Pawn` only; render components keep Visibility/Camera traces and
ignore the pawn. Every collider blocks except the node-name classes `interaction`, `tires_only`, `grid_volume`.
The vehicle's physics-grid volume (`root_entity.physics_grid`) is kept on `ASCVehicle`; while the capsule's axis
(feet to head) touches it, the root entity's own colliders (tag `SCHullCollision`, the exterior hull) are ignored
by movement, spawn and noclip-exit checks, as the game's grid does. Assets without colliders (`.skin`) keep
render-mesh collision. Debug: `sc.FP.ShowCollision 0|1`, `sc.FP.Blocker [cm]`; logs `GAME_COLLISION`,
`PHYSICS_GRID`, `FP_GRID`.

WP-P1 implements this in the Runtime module with `ASCFirstPersonCharacter` and a
Slate HUD. Static/rigid geometry retains the existing complex-as-simple bodies
attached to receiving bones. Soft skins have a separately owned Chaos triangle
BVH per `USCRigComponent`, cooked in the import worker and refitted with the CPU
skin pose only when animation evaluation changes that rig. UE's per-poly query
body is exposed explicitly; the stock 5.8 skeletal `GetBodyInstance` path only
looks up physics-asset bodies. No second collision cache is introduced.

`children[].item_type` is the authored DataCore
`Components[SAttachableComponentParams].AttachDef.Type` string, or null when
absent; never inferred from entity/asset names. The Rust mirror accepts absent
fields. Layer-1 `CACHE_FORMAT_VERSION` is 13, invalidating older packages and
catalogues. The FFI ABI stays unchanged: the additive field travels in the package.

Static wear (WP-U2, 2026-11-10): generic texture PNGs carry their colour encoding
(`sRGB` chunk, or `gAMA` 100000 for linear data, from the DDS header), and
`palettes.json` `finish.<channel>.glossiness` is the DataCore `TintEntry.glossiness`
UNorm8 as a 0..1 fraction (was the raw byte). Layer-1 `CACHE_FORMAT_VERSION` is 14.
A `--skip-existing-assets` export into a directory written by an older exporter
keeps its untagged PNGs (role fallback); export into a fresh directory instead.
UE reads HardSurface static wear from `public_params.WearBlendBase`, the `Wear`
layer or the wear tints, the Primary layer's `TexSlot8` blend map and glTF
`COLOR_0` (R damage, G glow, B AO, A wear) under `%VERTCOLORS`; rules and sources in
`ue/verification/WP-U2.md`.

Clip pairing (2026-11-10): an exported clip holds the keys of the CAF its `.chrparams`
event names — the `$TracksDatabase` entry of that resolved path (metadata entry `i` owns
key block `i`), else the loose `.caf`; `#filepath` is a running, case-insensitive
directive. No name or bone heuristic takes part, and an event whose CAF is stored nowhere
has no clip. A clip merged by name across rigs has the longest contributor's
`frame_count` and lists every contributing animation in `sources` (rate, length, start
pose); each channel names its own by `source`. UE times and anchors a track by its
source: played forward the animations start together, played backwards (a fragment with
negative speed) each starts at its own end, as each entity does in the game
(`FSCAnimImporter::TrackFrame`). A rate that is not a whole number of frames a second, a
channel with keys past its animation's length and an animation longer than its clip are
import errors. Layer-1 `CACHE_FORMAT_VERSION` is 18 (17: packages
without `sources`, where a shorter animation starts late when its clip runs backwards;
earlier: packages that can hold another clip's keys, e.g. Open and Close exchanged).
Contract and known gaps: submodule
`docs/decomposed-export-contract.md` "Animation Records"; validator
`scripts/verify_clip_pairing.py`; evidence `ue/verification/WP-P1.md`.

Instance palette (2026-11-10): UE resolves an instance's tint palette as the first id that
`palettes.json` carries of: the instance's `palette_id`; its interior container's `palette_id`;
the material sidecar's authored `DefaultPalette` (`palette/<lower-case record file name>`); the
palette of the entity the instance is attached to. With no resolvable candidate the authored id
is kept and the material is untinted. No package or ABI change (`WP-U3b.md` §14.2).

Child controllers use scoped clip IDs and fragment animation references; raw
interior geometry can acquire a controller through an unambiguous exact DataCore
geometry relationship. Animated CGA interior meshes retain their NMC receiving
hierarchy without pre-baking those transforms; static CGF interiors keep their
existing flattened path. UE targets interactions to one receiving rig instance.

`ASCVehicle::SpawnPoints` holds kind, display name and vehicle-local transform.
Seat/SeatAccess/Door/Entrance item types, fragment joints, interior placement
bounds and the hull top produce hints. First-person entry searches entrances,
seat access, seats, interiors, then hull; lower entrance hints are tried first.
Each hint is resolved to a walkable surface with standing-capsule clearance and
a supported short stride. A Seat hint carries the vehicle-local box of the seat item's
collidable geometry (`FSCSpawnPoint::Bounds`); a seat start is accepted only when the
crouched capsule touches that box, else the seat is refused and the next kind is tried. Teleports reject occupied/unsupported locations.
Older packages without item types retain the hull fallback. Orbit's **Spawn
where I look** adds a surface hint. Possession disables the inspection pawn's
tick; exit restores it and unload destroys the character.

Controls: click the viewport to capture mouse; WASD, Shift run (650 cm/s versus
400 walk), Space jump, Ctrl crouch, E interact, F torch, V noclip (Space/Q up/down),
T teleport menu, Esc release mouse. Recovery returns to the last accepted spawn
below the ship/floor; two seconds of blocked movement offers noclip. Exiting
noclip inside blocking geometry is refused. See `ue/verification/WP-P1.md` for
headless physics evidence and GPU acceptance still to be filled by the orchestrator.

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

The generated `crates/scbridge-ffi/include/scbridge.h` is the ABI source of truth.
The twelve production entry points and their value types are:

```c
typedef struct sc_handle sc_handle;
typedef enum sc_status {
    SC_STATUS_OK = 0,
    SC_STATUS_INVALID_ARGUMENT = 1,
    SC_STATUS_ERROR = 2,
    SC_STATUS_PANIC = 3,
    SC_STATUS_UNSUPPORTED = 4,
    SC_STATUS_CANCELLED = 5
} sc_status;
typedef enum sc_material_mode {
    SC_MATERIAL_TEXTURES = 0, SC_MATERIAL_COLORS = 1, SC_MATERIAL_NONE = 2
} sc_material_mode;
typedef struct sc_options {
    uint32_t lod, mip, material_mode;
    uint32_t attachments, interior, lights, shields;
    uint32_t animations, default_pose, collision_reserved;
} sc_options;
typedef struct sc_blob { uint8_t *data; size_t len; } sc_blob;
typedef struct sc_cache_info {
    uint64_t bytes, entries, cap_bytes;
    uint32_t enabled, last_hit;
} sc_cache_info;
typedef int (*sc_progress_cb)(const char *phase, uint64_t done,
                               uint64_t total, void *user);

sc_status sc_open(const char *p4k_or_null, const char *cache_dir_or_null, sc_handle **out);
sc_status sc_default_options(sc_options *out);
sc_status sc_search_entities(sc_handle *, const char *query, uint32_t limit, char **json);
sc_status sc_clip_catalogue(sc_handle *, const char *entity, char **json);
sc_status sc_lights(sc_handle *, const char *entity, char **json);
sc_status sc_cache_stats(sc_handle *, sc_cache_info *out);
sc_status sc_cache_evict(sc_handle *, uint64_t cap_bytes, uint64_t *removed);
sc_status sc_export_decomposed_mem(sc_handle *, const char *entity, const sc_options *,
                                  sc_progress_cb, void *user, sc_blob *out_tar);
const char *sc_last_error(sc_handle *);
void sc_free_string(char *);
void sc_free_blob(sc_blob *);
void sc_close(sc_handle *);
```

All strings, including explicit p4k and cache paths, must be NUL-terminated UTF-8.
NULL p4k auto-discovers the install; NULL cache disables persistence. The host
resolves its launch-directory cache setting to an **absolute path** before UE
changes cwd. Use `sc_default_options` or NULL export options for core defaults;
booleans accept only 0/1, `animations=0` omits clips and animation sidecars, and
`collision_reserved` must be zero. Unknown boolean/material values return
INVALID_ARGUMENT; unsupported options return UNSUPPORTED.

The host serializes all calls on a handle. While an export is busy, its host lock
must block close, last_error and search until export returns or is cancelled. The library does not
provide that host lock; do not call concurrently or reenter from a callback.
Different handles may be used concurrently. Progress may run on worker/rayon
threads; phase is borrowed only during that callback, and done/total is normalized
0..1000 progress, not a file count. The user context must remain live until export
returns. No callback occurs after return; callbacks must not throw or unwind.
The callback returns zero to continue or nonzero to cancel. This is ABI v2;
consumers compiled against the former void callback must rebuild against the new
header. Cancellation returns `SC_STATUS_CANCELLED` (5), leaves an empty blob and
no new package cache entry, and does not poison the handle. `sc_last_error`
reports "export cancelled"; the same handle can search/export again.

The core accepts an optional cloned `CancelToken` on entity export calls.
Checkpoints run before assembly, synchronously at the exporter's checked progress
phases, before tar construction and before cache publication. A token cancelled
from another thread is observed at the next checkpoint; a long decoding phase
is not interrupted mid-operation. The final publication checkpoint is the commit
point: once it accepts continuation, cache publication and any folder unpack
finish without further cancellable callbacks. Thus an export returning Cancelled
never writes a package cache entry or leaves partial output files. Existing cache
entries and unrelated output files are retained. Cache format v9 separates the
new `include_animations` option (default true); CLI `--no-animations` sets it false.


Each JSON output and tar blob is owned and must be released by the corresponding
`sc_free_string` or `sc_free_blob` function. `sc_free_blob` clears its fields.
`sc_last_error` is borrowed and must never be freed: a handle's error remains valid
until its next status-returning call or close. `sc_last_error(NULL)` reports open
failures on the same thread, until the next call other than error retrieval.
Output storage must not alias inputs or contain outstanding allocations.
Every entry point catches Rust unwinding panics. Only PANIC poisons a handle and
requires close/reopen; ordinary failures leave it usable. Ordinary export failures
return an empty blob; outputs after PANIC must not be used. NULL frees/close are
no-ops. Invalid pointers, allocation aborts and foreign exceptions are outside the
panic guarantee.

Cache statistics come from the core under its cache lock. Missing cache/manifest
reports zero, malformed manifests recover through the existing policy, and
last_hit describes the latest successful core operation. Eviction applies a
one-shot cap; future writes still enforce the core's automatic cap.

The tar is uncompressed: **scene.json first, then all other entries sorted
alphabetically**, placing Data/ before Packages/ sidecars. Regular files use
explicit typeflag `0` and POSIX ustar headers. Long paths split into the 100-byte
name and 155-byte prefix at a slash. Only unsplittable paths use a per-file PAX `x`
header carrying the UTF-8 path key for the following regular entry. No GNU
longlink records are emitted. Shared-cache C/CLI exports are byte-identical;
independent exports may differ in upstream export-stamp timestamps.

## Open questions after WP-U0

- ~~GPU validation of path B~~ done 2026-11-09 (`ue/README.md` §GPU validation).
  New: runtime-built meshes must gain Lumen mesh cards, a distance field and RT
  geometry (WP-U1) and the cache must carry them (WP-C1).
- Gladius-sized import timing and Interchange baseline (deferred; no cargo in U0).
- Exact glTF→UE basis matrix (lock with a synthetic-GLB unit test in WP-U1).
- WP-C1 cache format/versioning and processed texture/skeleton/animation coverage.
- Nanite skeletal support is present in 5.8 with `r.Nanite.AllowSkinnedMeshes=1`;
  see U0's source/documentation findings. Soft-skin rendering is not tested here.

## Player package API (WP-P2a)

`Session::export_player(&PlayerRequest { gender, loadout }, cancel)` returns the
contract-v2 character tar. `player_loadouts(gender)` lists default loadout paths
with their stowed weapons. The CLI exposes `player export <out.tar> --gender
male|female [--loadout <p4k path>]` and `player loadouts`. Player raw-cache entries
use a separate player namespace, player format revision, archive identity, global
cache format, gender and loadout request. Entity cache keys and exports are not
changed by this API. Cancellation is checked before/after assembly and before
publication; cached or cancelled exports retain the existing D10 semantics.

The additive FFI function is `scbridge_export_player(handle, gender_or_null,
loadout_or_null, progress, user, out_tar)`. NULL gender chooses male, NULL loadout
chooses the first armed default loadout in name order. Strings and callback
lifetimes follow the existing ABI; release the result using `sc_free_blob`.

See the vendored [player contract](../vendor/StarBreaker/docs/decomposed-export-contract.md#player-kits-wp-p2a)
for the shipped `player`, `port`, motion-layer and blend-space schema. P2b should
bind skinned parts to the root pose by joint name, attach bone items with the port
and inverse item-helper transform, and use each nested weapon's `package_dir`
for its animation sidecars. Body sidecar samples retain the importer convention;
root-motion endpoints and port offsets explicitly use native SC axes. Do not
interpret additive root endpoints as an absolute body pose. Retain source layer
indices and sequence times when evaluating Mannequin layers.

LIVE correction: alphabetical default selection picks Brightsky (sniper, SMG,
pistol), so the P4-AR test uses the explicit PVE loadout. Blend-space effective
bounds include all authored/derived examples while preserving authored grid bounds.
The exporter does not silently clamp examples to their grid.

Player motion layers with `missing: true` retain the authored clip name but have
no sidecar (`additive` is null). `player.diagnostics` identifies the missing clip,
fragment, authored tags/fragment tags and gender. P2b must skip these layers while
retaining the rest of the state. LIVE 4.10.2 lacks the female FP pistol select
additive in the female's registered animation sources; this is a reported sidearm
gap, not a substitute animation. Required unarmed and stocked-rifle states are
checked by `scripts/verify_wp_p2.py` for both genders.

Nested player items namespace their assets and manifests by child ID. Resolve
mesh/material paths from the tar root and weapon animation sidecars from the
child's `package_dir`. Body clip sidecars remain relative to the outer package.
Combined blend spaces retain selectors/components; `scopes` and `move_speed_tag`
retain controller bindings and unarmed gait variants for the runtime evaluator.
The CHF head identity wins over a loadout head subtree, preventing duplicate faces.

P2a validation on LIVE 4.10.2 (2026-11-09, debug, raw cache disabled):

| Package | Wall time | Body clips | Blend spaces | States |
| --- | ---: | ---: | ---: | ---: |
| Default male | 9.184 s | 177 | 37 | 52 |
| Default female | 9.148 s | 174 | 37 | 52 |
| Explicit P4-AR loadout, male | 9.039 s | 177 | 37 | 48 |
| Explicit P4-AR loadout, female | 9.006 s | 174 | 37 | 48 |

These are fresh processes without the application cache, not a flushed OS page
cache. Default loadout enumeration found 79 loadouts, 40 with stowed weapons.
Both female fixtures report the one sidearm diagnostic above. Verification:

```sh
target/debug/scviewer-cli --no-cache player export out/fps/player-male.tar
target/debug/scviewer-cli --no-cache player export out/fps/player-female.tar --gender female
target/debug/scviewer-cli --no-cache player export out/fps/player-p4-male.tar --loadout Data/Scripts/Loadouts/Player/DefaultLoadouts/PVE_medium_behr_rifle_gmni_rifle.xml
target/debug/scviewer-cli --no-cache player export out/fps/player-p4-female.tar --gender female --loadout Data/Scripts/Loadouts/Player/DefaultLoadouts/PVE_medium_behr_rifle_gmni_rifle.xml
python3 -I scripts/verify_wp_p2.py out/fps/player-male.tar out/fps/player-female.tar
python3 -I scripts/verify_wp_p2.py out/fps/player-p4-male.tar out/fps/player-p4-female.tar --require-entity behr_rifle_ballistic_01 --require-entity rrs_combat_heavy_backpack_03_01_01
```

All four packages pass. The core/CLI/FFI workspace passed 38 Rust tests; the
upstream 3D library passed 830 tests (3 ignored), including 19 new player tests.
The generated header check and staged-library comparison pass. Independent
uncached, cached and FFI default-male tars share SHA-256
`a59a5cb692b295e874cdc0903dfab7cc2f97a267fa57d5028a202ea5f4dda275`;
cache hit was 201.4 ms. The C player smoke test verifies cancellation, cleared
output, recovered-handle export and blob release.

The strict whole-Gladius-tar hash differs because the existing provenance stamp
includes all exporter source. Before:
`a3dec8798a3b07fd4e456d066e649fc7fe4cab64aa8df12d1267f13ac918cba7`;
after: `fca239d696e7e1a5ea622ab00accd312b8ab9d6441053a0fb56aa77277406938`.
All 503 non-stamp members are byte-identical. The remaining stamp differs only
in `exporter.source_sha256`; tar framing, member order and all other bytes match.
No provenance or tar is rewritten. The standalone P4-AR comparison against a
scratch build of the pinned pre-P2 source passes 73/73 byte-identical members.
The older supplied `p4-dec` research export predates WP-P1 and is not that baseline.

```sh
python3 -I scripts/verify_wp_p2.py --compare out/fps/gladius-before.tar out/fps/gladius-after.tar
python3 -I scripts/verify_wp_p2.py --compare out/fps/p4-before-p2 out/fps/p4-after.tar
```

P2b still needs runtime verification of scope/layer sequencing, component blend
space selectors, leader-pose clothing, helper-relative weapon attachment and
native-SC root-motion conversion. Aim-pose assets are not part of this ground
motion export; the brief's distributed view-pitch path remains applicable.

P2a armed-locomotion follow-up: the exporter shares the actor motion base across
all participating fragment IDs, fixing rifles that previously selected unarmed
movement despite armed idle. A base association authored in another stance can
establish the same actor base for crouch; actual ADB selection retains the exact
requested stance. Weapons now receive the same untagged/walk/run/sprint query
matrix as unarmed actors, using the authored `moveSpeed` group. The JSON schema
is unchanged; `move_speed_tag` now varies for weapon states as well. Runtime
selection must include it to use speed-specific options. Stow selection remains
independent of the locomotion base.

Follow-up verification: all four tars were re-exported in place. Default packages
now contain 160 states each (72 armed standing states); P4 loadout packages contain
144 each (64 armed standing states). All have 39 blend spaces and 183 body clips.
The stronger verifier passes every armed standing state and requires each weapon's
speed/stance/view matrix to equal the unarmed matrix. The existing female pistol
select diagnostic remains the only missing-clip diagnostic. Before/after comparison
confirms all 16 unarmed states, 15 referenced blend spaces and 67 clip payloads are
unchanged in each package. Player tests: 22 passed; workspace tests: 38 passed.
No FFI staging or UE files were touched for this follow-up.
