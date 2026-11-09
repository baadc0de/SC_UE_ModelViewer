# Unreal Engine export design

Target: Unreal Engine 5.6+ (Linux editor, Vulkan RHI). Plugin: `ue/Plugins/SCBridge`
(editor-only module `SCBridgeEditor`, runtime module `SCBridge` for the actor
classes). Python editor scripts are allowed for glue but the importer core is C++.

## Inputs

StarBreaker decomposed package (contract: `vendor/StarBreaker/docs/decomposed-export-contract.md`):

```
<root>/Packages/<Entity>_LOD<n>_TEX<m>/scene.json        # hierarchy, children, interiors, lights, animation index
<root>/Packages/<Entity>_LOD<n>_TEX<m>/animations/*.json # per clip, bones keyed by CRC32 hash, Blender Z-up wxyz
<root>/Packages/<Entity>_LOD<n>_TEX<m>/palettes.json | liveries.json | paints.json
<root>/Data/**/*.glb                                      # one mesh asset per source geometry
<root>/Data/**/*.materials.json                           # per .mtl: submaterials, shader family, texture slots, params
<root>/Data/**/*.png                                      # textures (DDNA normal w/ smoothness alpha, derived *_roughness)
```

M4 replaces the folder with the same structures produced in memory by `scbridge-ffi`.

## Asset mapping

| Source | UE asset | Notes |
| --- | --- | --- |
| mesh asset `.glb` (rigid, opaque) | `UStaticMesh`, **Nanite enabled**, fallback relative error 1 %, collision complex-as-simple off (use simple boxes) | Build via `FMeshDescription` from the GLB primitives (positions ×100, Y-up RH → Z-up LH: `(x, y, z) → (x, -z, y)` then mirror to LH `(x, y, z) → (x, -y, z)`, verify against Interchange's result on one mesh), UV0/UV1, vertex colours, normals/tangents preserved (do not recompute). |
| soft-skinned submesh set | `USkeletalMesh` + `USkeleton` from CHR bones | Nanite **off** by default (experimental); toggle in import options. |
| `.mtl` submaterial | `UMaterialInstanceConstant` parented to a master material per shader family | Families from `materials.json`: Illum, HardSurface, Glass, Layered (tint palette), MeshDecal (POM), Emissive/Glow, NoDraw (skipped), Hologram/UI (unlit emissive). |
| texture slot | `UTexture2D` | sRGB on for diffuse/albedo; linear + `TC_Normalmap` for DDNA; linear grayscale for derived roughness/metal/masks; `TC_Masks` for packed. |
| `interiors[].lights` | `UPointLightComponent` / `USpotLightComponent` / rect for `area` | use `intensity_candela_proxy` (cd), `radius_m` ×100, `color` linear, half-angles as given, gobo texture → light function. Flag `ambient_proxy` as skipped. |
| clip (`animations/*.json`) | `UAnimSequence` on the skeleton | convert Blender Z-up wxyz back to UE; linear keys at clip fps; hemisphere-align quats; `start_rotation` anchor per whitepaper §14.6. |
| Mannequin fragments | `UDataAsset` (`USCClipCatalog`) listing clips, tags, scopes | lets the actor expose "Landing_Gear/Deploy" instead of raw clip names. |
| scene hierarchy | `ASCVehicle` Blueprint (generated) | root `USceneComponent`; rigid meshes as `UStaticMeshComponent` attached to skeleton sockets named after the owning bone; soft parts as `USkeletalMeshComponent`; children recurse with `local_transform_sc` (`gltf_y_up` basis). |

## Nanite / Lumen rules

- Opaque and Masked materials → Nanite static meshes. Translucent (Glass) and
  MeshDecal materials → separate non-Nanite components with the same transform.
- Emissive: Illum `glow` → emissive colour × strength in the Material Instance so
  Lumen picks it up; thruster glow targets from `controls.engine_glow` get an
  exposed scalar parameter.
- Two-sided from `is_double_sided`; mesh decals use DBuffer decal response on hosts.
- Nanite assembly: one `UStaticMesh` per source geometry, instanced across
  components (same `Data/` asset path → same UE asset) so repeated parts share.
- Lumen test level: `ue/SCTest/Content/Maps/LumenTest` with Lumen GI + reflections,
  HWRT on, a sky light and one directional light. Import smoke = drop `ASCVehicle`,
  take an automated screenshot.

## Direct import (M4)

`crates/scbridge-ffi` builds `libscbridge.so` with a C header (`cbindgen`):

```
sc_open(path | NULL for auto-discover) -> handle
sc_search_entities(handle, query, out_json)
sc_export_glb(handle, entity, lod, mip, out_bytes)          # viewer parity
sc_export_decomposed_mem(handle, entity, opts, out_tar)     # same contract, in memory
sc_free(ptr)
```

UE side: `Source/ThirdParty/SCBridgeLib` module links the `.so`; an `SEditorUtilityWidget`
or Slate tab provides search + options and calls the same importer code as M3 on the
in-memory package. Error strings cross the boundary as UTF-8; no panics across FFI
(`catch_unwind`).

## Open questions to settle in WP-U1

- Interchange glTF vs our own `FMeshDescription` builder: prototype both on
  `AEGS_Gladius.cga`'s GLB; keep whichever preserves tangents/UV1/vertex colours and
  lets us set Nanite settings before build. Expected: own builder.
- UE version pin (5.6 vs 5.7): pick the newest Linux binary that has Nanite skeletal
  mesh behind a flag and a stable Interchange glTF translator.
