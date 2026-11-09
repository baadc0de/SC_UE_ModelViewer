# WP-U2 verification — 2026-11-09 (Opus 5.5, RTX 4090 / Vulkan host)

Base: superproject `d39e298` (main). UE 5.8.2 CL 56702186. Test package `out/dec-r1`
(final-exporter Gladius, 23 `*.materials.json`, 366 PNGs). No cargo, no `vendor/` or
`crates/` changes. Commands are from the repository root.

## Substrate vs legacy materials

Owner instruction: choose the material model on evidence before building the master set.

### Facts from the installed engine (5.8.2)

| Topic | Evidence |
| --- | --- |
| Switch | `r.Substrate` — `Engine/Source/Runtime/RenderCore/Private/RenderUtils.cpp:2068`, default **0**, `ECVF_ReadOnly`; project setting `bEnableSubstrate` (`Engine/Classes/Engine/RendererSettings.h:1257`, `ConfigRestartRequired`). **Project-wide**: every material (engine, map, ours) is compiled for one model. |
| Maturity | `r.Substrate` itself carries no Experimental/Beta marker and no `ECVF_Preview`; `GameProjectUtils.cpp:199` `AddSubstrateConfigValues` enables it for **every new 5.7+ project**. Remaining Experimental items: `r.Substrate.EnableLayerSupport` (UE *material layers* UI, `RendererSettings.h:1340`, default 0) and `r.Substrate.OpaqueMaterialRoughRefraction` (`:1312`). The sub-format CVars (`ProjectGBufferFormat`, `BytesPerPixel`, `ClosuresPerPixel`) are `ECVF_Preview`. |
| Our project before WP-U2 | `ue/SCViewer/Config/DefaultEngine.ini` had no `r.Substrate` → legacy (project predates the template default). |
| GBuffer | `r.Substrate.ProjectGBufferFormat` default 1 = Adaptive; `Config/VulkanPC/DataDrivenPlatformInfo.ini:251` sets `bSupportsAdaptiveGBuffer=true` under `[ShaderPlatform VULKAN_SM6]` (our `+TargetedRHIs=SF_VULKAN_SM6`). `Config/Linux/BaseLinuxEngine.ini:22-27` already sets Substrate quality defaults (RoughDiffuse, ClosuresPerPixel=4, Glints). |
| Nanite / Lumen / HWRT / decals | Substrate code paths in `Renderer/Private/Nanite/NaniteShading.cpp`, `NaniteMaterials.cpp`, 159 references under `Renderer/Private/Lumen/`, `RayTracing/RayTracingMaterialHitShaders.cpp`; DBuffer decals via `Substrate::AddSubstrateDBufferBasePass` (`DeferredShadingRenderer.cpp:3177`). VSM is model-independent. |
| Translucency | `BLEND_TranslucentColoredTransmittance` is **Substrate-only** (`Engine/Classes/Engine/EngineTypes.h:254`); legacy has `MSM_ThinTranslucent` (`:720`), which Substrate itself converts to a slab with `SSSMFP` from the transmittance colour (`Shaders/Private/Substrate/SubstrateLegacyConversion.ush:478-509`). |
| Building blocks | `Engine/Public/Materials/MaterialExpressionSubstrate.h`: Slab BSDF inputs **Diffuse Albedo, F0, F90**, Roughness, Normal, SSS MFP, Emissive (`:274-394`); `SubstrateThinFilm` (thickness, IOR → F0/F90, `:1449`); `TransmittanceToMFP` (`:1358`); Vertical Layering / Horizontal Mixing / Weight; Unlit BSDF. Legacy has no thin film and parameterises specular as BaseColor/Metallic/Specular(scalar). |
| Python authoring | `MaterialEditingLibrary.connect_material_property(..., MP_FRONT_MATERIAL)` works (`SceneTypes.h:179`); Substrate nodes are ordinary expressions. |

### Star Citizen families on each model

SC (CryEngine) is a **specular-colour workflow**: authored `Diffuse` + `Specular` (F0 colour,
incl. `_spec` maps on Illum TexSlot4) + gloss from DDNA alpha; layers carry their own
specular/metal snapshot. Counts in `out/dec-r1`: Illum 129, LayerBlend_V2 96, HardSurface 95,
MeshDecal 41, GlassPBR 23, UIPlane 8, DisplayScreen 2, Monitor 1.

| Family / feature | Legacy (Default Lit) | Substrate |
| --- | --- | --- |
| Illum diffuse + `_spec` colour F0 + DDNA | lossy: needs BaseColor/Metallic/Specular conversion; coloured dielectric F0 not representable; metal classification heuristic (`mtl.rs` `layer_metallic`) in the graph | **native**: Diffuse Albedo = diffuse, F0 = Specular × spec map |
| Roughness (derived `*_roughness`, sqrt(1−s)) | native | native |
| HardSurface / LayerBlend_V2 (≤4 base layers, mask R/G/B, per-layer tiling, palette tint, snapshot metallic) | parameter lerps | same parameter lerps feeding one slab (no extra closures); vertical layering *not* needed — SC layers are a horizontal parameter blend |
| GlassPBR (tint = palette glass, `THIN_FILM`, Thickness, refraction, dirt) | Thin Translucent: coloured transmittance yes, thin film **no** | coloured-transmittance blend + `TransmittanceToMFP` + **`SubstrateThinFilm`**; dirt could become a vertically layered slab |
| HardSurface iridescence (TexSlot10) | no | thin film / specular profile available |
| MeshDecal / POM | translucent or DBuffer decal; BumpOffset | same (DBuffer supported) |
| Emissive / screens | Unlit + emissive | Unlit BSDF |
| Two-sided, masked | material/instance overrides | same |

### Cost / risk, measured

| | Legacy (before) | Substrate (after) |
| --- | --- | --- |
| Viewer + Gladius, Lumen overview | `out/smoke/legacy-before-lumen.png` | `out/smoke/substrate-switch-lumen.png` — identical: Nanite hull, Lumen Scene and HWRT reflection panels present, no black cut-out |
| First start after switch | warm: import at +8 s | engine init **8 min 26 s** (12:53:33 → 13:01:59, global + engine material shaders, 16 local workers), import 2.30 s; later starts warm again |
| Project DDC | 483 MiB | 932 MiB (**+449 MiB**); engine DDC unchanged (1083 MiB) |
| Runtime | — | Adaptive GBuffer (more bytes/pixel than legacy), only multi-closure pixels pay more; our masters are single-slab |
| Risk | none | project-wide; DDC/shader rebuild on engine upgrades; UE *material layers* stay experimental (unused: SC layers are blended in-graph) |

### Decision

**Substrate.** Both models render the layered tint materials equally well (both are an
in-graph parameter blend). Substrate wins on the two places where legacy loses data: the
CryEngine specular-colour workflow maps 1:1 onto the slab's Diffuse Albedo / F0 (no
metallic heuristic, coloured `_spec` maps kept), and GlassPBR gets coloured transmittance
plus a native thin-film node (`THIN_FILM` / `ThinFilmThickness` are authored on the Gladius
canopy). The Viewer map renders unchanged on Vulkan with Nanite + Lumen + HWRT. Cost: one
8.5-minute shader rebuild and +449 MiB DDC.

**Swap cost** (back to legacy): the C++ builder (`FSCMaterialBuilder`) and every parameter
name are model-neutral (`DiffuseColor`, `SpecularColor`, `Layer{i}Tint`, `GlassTint`, …).
A switch is: rewrite the seven graph functions in `ue/scripts/create_materials.py`
(slab → BaseColor/Metallic/Specular conversion, glass → Thin Translucent, drop thin film),
remove `r.Substrate` from `DefaultEngine.ini`, regenerate the assets, and pay the same
shader rebuild. No C++ change.

## What was built

- **Masters** (`ue/Plugins/SCBridge/Content/Materials/`, generated by `ue/scripts/create_materials.py`,
  Substrate slabs, neutral defaults only): `M_SC_Illum`, `M_SC_HardSurface` (1 layer), `M_SC_Layered`
  (4 LayerBlend_V2 base layers, blend-mask R/G/B, per-layer tiling/tint/F0/metallic/gloss),
  `M_SC_Fallback` (each with `MI_*_Masked`, `_TwoSided`, `_Masked_TwoSided` constant variants),
  `M_SC_Glass` (coloured transmittance, `TransmittanceToMFP`, `SubstrateThinFilm`),
  `M_SC_MeshDecal` (DBuffer colour decal via Substrate *Convert To Decal*),
  `M_SC_MeshDecal_Relief` (DBuffer normal + roughness only), `M_SC_Translucent` (lit forward/faded
  surfaces), `M_SC_Emissive_Unlit` (additive screens), plus 4×4 neutral default textures.
- **`FSCMaterialBuilder`** (`Public/SCMaterialBuilder.h`, `Private/SCMaterialDescribe.cpp` pure mapping,
  `Private/SCMaterialBuilder.cpp` MID creation): family → master, roles → texture params, authored
  attributes / public params / palette routing → scalars and colours, blend/two-sided/D6 decision.
  `RequiresNonNanite()` now drives WP-U1's D6 split (replacing the importer's family test).
- **`FSCTextureImporter`** (`SCTextureImporter.h/.cpp`): PNG → BGRA8 + box-filtered CPU mips on worker
  threads (`ParallelFor`), transient `UTexture2D` (sRGB only for colour, `TC_Normalmap` linear for DDNA
  with the glTF→UE green flip, `TC_Masks` linear for roughness/height/masks), deduplicated by (path, role).
- **`FSCImportMaterials`** (importer phase): describes every (sidecar, palette) pair, decodes textures once,
  creates MIDs owned by `ASCVehicle` (`Textures`, `Materials`, `EngineGlowMaterials`); binds per component and
  slot (instances with different palettes share meshes); split sections without an instance get an invisible
  material, never the opaque placeholder; split components are excluded from the Lumen scene and cast no shadow.
- `scene.json` `controls.engine_glow` parsed (`FSCEngineGlow`); `ASCVehicle::SetEngineGlow`, `sc.EngineGlow <v>`,
  harness `-SCEngineGlow=`, review camera `-SCView=yaw,pitch,dist -SCTarget=x,y,z`, `-SCNoMaterials` baseline.

## Evidence: sidecar fields per rule

| Rule | Fields read | Result |
| --- | --- | --- |
| Family → master | `shader_family` (`mtl.rs` `ShaderFamily` strings) | Illum, HardSurface, LayerBlend_V2/Layer, GlassPBR, MeshDecal, screens (DisplayScreen/UIPlane/Monitor/UIMesh/Hologram*), NoDraw; anything else → Fallback + warning naming family and `Shader` |
| Hidden | `activation_state.state == inactive`, NoDraw | invisible in the split mesh (e.g. `stencil`, `RTT_Text_To_Decal`) |
| Blend | `alpha_config` (null in r1), `authored_attributes` `AlphaTest`, `Opacity`, `MtlFlags & 0x8000` (CryEngine REQUIRE_FORWARD_RENDERING: Headlight_Glow, emissives, glows, engine_glow…), `%DECAL` token | masked / translucent / decal; two-sided = `is_double_sided` (decal) |
| Emission | `Emissive` RGB, `Glow` (or `GlowAmount`), `public_params.AlphaGlowCutoff`, `%EMITTANCE_MAP` + emittance slot | strength = Glow × `GlowToEmissive` (10, host constant below); **Glow absent/0 → 0** (no `emissive_factor()` Glow=1 fallback); colour = Emissive × Diffuse × base texture, gated by alpha > cutoff. Deviation: Glow > 0 with Emissive = 0,0,0 (`glows`, `buttons`) is CryEngine diffuse glow → Emissive treated as white |
| Relief decal | MeshDecal + `%PARALLAX_OCCLUSION_MAPPING`, no `%DECAL`/`%STENCIL_MAP`/stencil slot/`tint_palette_decal`; TexSlot1 alpha, TexSlot3 `normal_gloss`, TexSlot4 `height`, `PomDisplacement`, `DecalAlphaMultiplier` | `M_SC_MeshDecal_Relief`: normal + roughness into the DBuffer under TexSlot1 alpha × DecalAlphaMultiplier, POM as bump offset — the dark grooves |
| Colour decal | MeshDecal/`%DECAL` with `base_color` (gladius_text_diff, warningtape) , `DecalAlphaMultiplier`, `DecalDiffuseOpacity` | `M_SC_MeshDecal`: sheet RGB × Diffuse where alpha × DAM × DDO |
| Stencil decal | `%STENCIL_MAP`, `$TintPaletteDecal` | r1's only stencil is inactive and `palettes.json` `decal.export_path` is null → hidden; TODO below |
| Glass | `palette_routing.material_channel` (glass) → `palettes.json` `glass`, `public_params.GlassTintStrength`, `%THIN_FILM` + `ThinFilmThickness`, `Specular`, DDNA + derived roughness | `M_SC_Glass`, non-Nanite |
| Layers | `layer_manifest[]` base layers (name not `Wear*`): `diffuse/normal/roughness_export_path`, `tint_color`, `palette_channel`, `uv_tiling`, `gloss_mult`, `layer_snapshot.{diffuse,specular,metallic,shininess}`; `blend_mask` slot + `BlendMapTiling` | tint = snapshot diffuse × tint × palette channel; F0 = snapshot specular |
| Roughness | `derived_textures[]` role roughness paired by `source_path`; `Shininess` | roughness = sqrt(1 − smoothness), smoothness = (1 − r²) × Shininess/255; placeholder Shininess=255 without map → 0.5 (mtl.rs) |
| Specular | `Specular`, `specular_support` map; Diffuse = Specular = 1,1,1 sentinel without map → dielectric default | F0 colour (Substrate slab) |
| Host tint | POM colour decals: dominant `palette_routing.material_channel` of the host mesh's slots by triangle count (Blender add-on rule) | `HostTint` per channel instance |
| Engine glow | `controls.engine_glow` targets (`material_sidecar`, `source_material_index` = authored `index`), `default_strength` | strength from the control (default 0) |

### Fallback register (host constants)

| Constant | Value | Why | Trigger to remove / sunset |
| --- | --- | --- | --- |
| `FSCMaterialBuilder::GlowToEmissive` | 10 UE emissive units per authored `Glow` | Glow's luminance unit is not in the exported data; the Viewer uses fixed, non-physical exposure (5 lux sun) | replace with the engine's Glow → cd/m² unit once WP-R5/WP-V1 establish physical light/exposure units |
| Alpha-glow gate slope | 1e4 (step at `AlphaGlowCutoff`) | step function in a material graph | none (numerical) |

## Commands and results

```bash
ue/scripts/build.sh SCViewerEditor                                     # Result: Succeeded
"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" "$PWD/ue/SCViewer/SCViewer.uproject" \
  -run=pythonscript -script="$PWD/ue/scripts/create_materials.py" -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC
                                                                        # LogPython: WP-U2 MATERIALS_OK
"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" "$PWD/ue/SCViewer/SCViewer.uproject" \
  -run=pythonscript -script="$PWD/ue/scripts/test_importer.py" -SCPackage="$PWD/out/dec-r1" \
  -SCVerifyTar="$PWD/ue/SCViewer/Saved/Import/review-dec-r1.tar" -unattended -nop4 -nosplash -nullrhi -ddc=SCViewerDDC
ue/scripts/run_import.sh out/dec-r1 -vulkan [-SCNoMaterials]           # timing, 2 runs each
SC_ARGS="-SCView=35,40,1.2" ue/scripts/capture_views.sh out/dec-r1 u2-3q lit
ue/scripts/capture_views.sh out/dec-r1 u2-after lit lumen
```

Automation (exit 0, `WP-U1 UNIT_TESTS_OK`): all 12 registered tests pass — the 8 WP-U1 tests (now also
asserting that every glass/decal/blended split section carries a decal-domain or translucent material) plus
`SCBridge.Materials.FamilyMapping` (synthetic `Tests/Fixtures/Materials`: every family, palette routing, layer
copy-down, relief vs colour decal, forward glow, Glow rule, blue `emissives`-like glow, dedupe = 24 unique
texture keys), `.MasterAssets` (every parent path loads; variant overrides; decal masters are `MD_DeferredDecal`,
translucent masters translucent), `.TextureDecode` (sRGB/linear/normal flags, green flip, mip chain),
`.PackageRules` on `out/dec-r1` (`PACKAGE_RULES decals=11 glowing=8 no_glow=210`: no MeshDecal on an opaque
master, no emission without Glow) and `FolderTarImport` (`VERIFY passed=1 … old_live=0 tracked_old=1947`).

Import (Vulkan game host, warm DDC, `IMPORT_OK seconds`): without materials 2.87 / 3.06 s, with materials
3.09 / 3.05 s — the material phase is `material_s=0.17–0.23` (250 textures, 190 MB BGRA8 incl. mips:
`decode_s=0.034–0.054` on workers, `create_s=0.035–0.062`; 273 MIDs in 0.04–0.06 s). The difference is within
run-to-run noise. Masters used: Layered 103, Illum 39, HardSurface 44, MeshDecal 35, Translucent 21,
Emissive_Unlit 13, Glass 9, MeshDecal_Relief 7, Fallback 2. First start after regenerating masters: shaders
compile on demand (~40 s).

## Visual checks (GPU, `ue/verification/wp-u2/`, half size)

| File | Shows |
| --- | --- |
| `stripes-before.jpg` / `stripes-after.jpg` | before: POM panel-line decals drawn as opaque white strips (decals in the split mesh with a lit translucent master and `Specular=1,1,1`); after: relief DBuffer decals → dark grooves, colour unchanged |
| `three-quarter-vs-reference.jpg` | our lit view (left) next to `reference/gladius/three-quarter.webp` (right) |
| `side.jpg` | port side: blue emissive strips at the wing edge, small text decals, tan fin stripe |
| `lumen-after.jpg` | Lumen overview: hull textured in Lumen Scene and HWRT reflections, no black cut-out; split glass/decal components no longer show as yellow culled patches |
| `substrate-before-lumen.jpg` / `substrate-after-lumen.jpg` | Viewer + grey Gladius before/after `r.Substrate=1` |

Against the reference — **matches**: olive hull from `palette/aegs_gladius` primary, tan bands on nose, wing
roots, fins and intakes (tertiary), dark grey intakes / engine covers / canopy frame, panel lines as dark
grooves, small stencil text decals, blue edge strips, orange fin-tip points, canopy translucent (the Geometry
Normals panel shows the interior through it). **Differs**: the cockpit reads almost black (interior is unlit:
no interior lights yet), the wing roundels / "362" are not legible at this distance, the sun-facing wing shows
a strong specular sheen (Viewer lighting/exposure, not material data), thruster glow not verified (below).

## Open issues

TODO(WP-U2): `-SCEngineGlow=50` produced no visible glow in rear close-ups; check the engine_glow slot binding
and its UV region against the alpha gate (texture `gladius_emissives_diff`, Illum + forward flag).
TODO(WP-U2): wear/damage layers (`WearLayer*`, `wear_mask`, vertex colours), Illum secondary blend layer
(TexSlot9/3/12), LayerBlend_V2 direct TexSlot3 macro normal, HardSurface direct TexSlot1 / iridescence (TexSlot10),
glass dirt and `EmissiveOpacity`, TexMod tiling, UV1 (`SECOND_UVS`) are not mapped.
TODO(WP-U2): stencil / `$TintPaletteDecal` decals need the palette decal texture (`palettes.json decal.export_path`
is null in r1); screens need `ui_bindings` images; paints.json variants as an actor dropdown.
TODO(WP-U2): `MtlFlags & 0x8000` → translucent is a host reading of CryEngine's material flag; ask upstream to
emit it in `alpha_config`; `MtlFlags & 0x2` (two-sided) is not honoured (upstream `is_double_sided` = decal only).
TODO(WP-U2): `ThinFilmThickness` is passed straight to Substrate's normalised thickness (1 = 10 µm); unit unverified.
TODO(WP-U2): verify the emissive interior lights the cockpit under Lumen once interior lights exist (WP-R5);
current interior glows are weak (`buttons` Glow 0.04).
TODO(WP-C1): cache decoded textures (BGRA8 + mips) with the processed-cache key.

## Review fixes (Codex cross-review of 04c3c0a)

Supersedes the earlier rows on roughness placeholder, white-specular sentinel, forward → translucent and
screen brightness. Engine facts checked in `~/UnrealEngine/5.8`.

| # | Finding | Fix |
| --- | --- | --- |
| 1 | `MP_Opacity` is only live for AlphaComposite under Substrate (`Material.cpp:7928-7931`) | Glass and `M_SC_Translucent` route coverage through a Substrate **Weight** node (`Graph.weight`, slab → `A`, coverage → `Weight` → Front Material); decals already use Convert-To-Decal `Coverage` |
| 2 | default clip 0.3333 (`Material.cpp:1169`) shifted thresholds | every master sets `opacity_mask_clip_value = 0.5` explicitly; mask graph = alpha − AlphaTest + 0.5, so a texel survives iff alpha ≥ authored AlphaTest. Test: `explicit mask clip 0.5` on every parent |
| 3 | forward (`MtlFlags & 0x8000`) glow cards were translucent and left the Lumen scene | they are now **masked** opaque-family surfaces (Nanite, Lumen cards, emissive feeds GI); cutoff = authored AlphaTest, else UE's default clip read from the `UMaterial` CDO. Masters: Illum 51 (was 39), Translucent 9 (was 21) |
| 4 | invented appearance (D11) | removed: roughness-0.5 placeholder (authored Shininess used), white-specular → 0.04 sentinel (authored Specular used), screen emission without Glow, Emissive=0 → white, and the `Max(1, n)` host-tint weight for empty slots (now actual triangle count, empty = 0). Genuinely missing values use the engine/Substrate default of that input (table below) |
| 5 | single-sided decals | `MI_SC_MeshDecal_TwoSided`, `MI_SC_MeshDecal_Relief_TwoSided`, `MI_SC_Translucent_TwoSided` (two-sided override); `ParentPath` selects them for `is_double_sided`; `M_SC_Translucent` itself is single-sided now |
| 6 | stencil/emittance routing | stencil decals bind `StencilMap` (TexSlot7 stencil export, or `palettes.json decal.export_path` for `$TintPaletteDecal`), `DecalRed/Green/Blue` from the palette decal channels, `StencilTint1..3` from `StencilDiffuseColor{,2,3}`, `StencilOpacity`, `UseStencil`; colour = Σ channel × decal colour × tint, coverage = Σ channels × StencilOpacity. An inactive `missing_base_color_texture` stencil becomes visible when the palette supplies the texture. `%EMITTANCE_MAP` routes the base-colour slot (no emittance role exists in `TextureSemanticRole`) |
| 7 | sRGB mips averaged encoded bytes | colour RGB averaged in linear light (`FLinearColor::sRGBToLinearTable`, IEC sRGB encode); alpha, masks, normals as stored. Test: black/white 2×1 → 188 (colour), 128 (linear) |
| 8 | decode before validation | header probe (PNG IHDR via `SetCompressed`) → RHI limit `GetMax2DTextureDimension()` → per-texture decoded-size budget → aggregate budget in key order → only then `GetRaw`. Rejections are warnings; the material keeps its neutral default. Tests: dimension rejection before decode, aggregate budget |
| 9 | tests | `PackageRules` skips (info) without `-SCPackage`; explicit assertions: Glow = 0 with Emissive 1,1,1 → 0; `emissives`-like → b > g > r; `AlphaGlowCutoff` from public_params (0.25); emittance routing; stencil binding; real-package `emissives` (if present) emits blue. Fixture `emissives_like` now uses arbitrary values (0.11, 0.37, 0.83 / Glow 0.45) |
| 10 | decode blocked the game thread | describe + decode start on a worker before the mesh loop and overlap mesh derive/build; the game thread waits only before creating textures/MIDs (`wait_s=0.0000` on the GPU run). TODO(WP-V1) full async API |
| P3 | `SC_ARGS` under `set -u` | `${SC_ARGS:-}` |

Missing-value policy (engine/Substrate defaults, `MaterialExpressionSubstrate.h` slab docs):

| Input | Authored source | When absent |
| --- | --- | --- |
| roughness constant | `Shininess`/255 → smoothness | Substrate default roughness 0.5 (smoothness 0.75); TODO(WP-R5) emit gloss when `Shininess` is absent |
| roughness map scale | `Shininess`/255 × layer `gloss_mult` | identity (1) |
| F0 | `Specular` (× `_spec` map), layer snapshot `specular` | Substrate default F0 0.04; TODO(WP-R5) resolve the `Diffuse=Specular=1,1,1` sentinel upstream |
| albedo | `Diffuse` × base texture × palette | texture as-is; with neither, Substrate default albedo 0.18 |
| emission | `Emissive` × `Glow` × `GlowToEmissive` × albedo, alpha-gated by `AlphaGlowCutoff` | none (Glow absent/0 or Emissive 0,0,0 → 0); TODO(WP-R5) confirm Glow>0 with Emissive 0,0,0 (`glows`, `buttons`) |
| mask cutoff | `AlphaTest` | forward cards: UE default clip (`UMaterial` CDO) |
| multipliers (`DecalAlphaMultiplier`, `DecalDiffuseOpacity`, `GlassTintStrength`, `StencilOpacity`, `uv_tiling`) | public params / manifest | identity (1) |

Fallback register additions: `FSCTextureLimits::MaxTextureBytes` 256 MiB per texture and `BudgetBytes`
2048 MiB per import (decoded BGRA8 incl. mips) — host memory budgets with no data source; sunset when WP-C1's
processed texture cache stores compressed platform data.

Commands (from the repository root; UE invocations sequential):

```bash
ue/scripts/build.sh SCViewerEditor                                                   # Result: Succeeded
UnrealEditor-Cmd … -script=ue/scripts/create_materials.py -nullrhi …                 # WP-U2 MATERIALS_OK (24 materials)
for p in dec-r1 dec; do UnrealEditor-Cmd … -script=ue/scripts/test_importer.py \
  -SCPackage=$PWD/out/$p -SCVerifyTar=$PWD/ue/SCViewer/Saved/Import/review-$p.tar -nullrhi …; done
UnrealEditor-Cmd … -script=ue/scripts/test_importer.py -nullrhi …                    # synthetic only
SC_ARGS="-SCView=35,40,1.2" ue/scripts/capture_views.sh out/dec-r1 u2r-3q lit
SC_ARGS="-SCView=-90,8,1.0" ue/scripts/capture_views.sh out/dec-r1 u2r-side lit
ue/scripts/capture_views.sh out/dec-r1 u2r lumen
```

Both packages: exit 0, all 12 tests `passed=1`, `PACKAGE_RULES decals=11 glowing=5 no_glow=210`,
`VERIFY passed=1 … old_live=0 tracked_old=1939`, `WP-U1 UNIT_TESTS_OK`; synthetic-only run: exit 0, 10 passed.
GPU (dec-r1): `TEXTURES requested=247 created=247 failed=0`, `MATERIALS … overlapped_s=2.41 wait_s=0.0000 masters:
Illum=51 Layered=103 HardSurface=44 MeshDecal=35 MeshDecal_Relief=7 Emissive_Unlit=13 Translucent=9 Glass=9
Fallback=2`, `IMPORT_OK seconds=2.9931`. Screenshots: `wp-u2/review-three-quarter.jpg`, `review-side.jpg`,
`review-lumen.jpg` — livery, grooves and decals unchanged; Lumen Scene/HWRT show the hull with no black cut-out.
Visible consequence of rule 4: the blue wing-edge strips (`Headlight_Glow`: Emissive 1,1,1, no Glow) no longer
emit and read as dim blue paint; only Glow-authored materials (e.g. `emissives`, Glow 0.3) glow.

TODO(WP-R5): sidecar fields named above (gloss when Shininess absent, specular sentinel, emittance role,
forward/coverage in `alpha_config`, Glow without Emissive, palette `decal.export_path`).
TODO(WP-V1): async import API with progress; texture creation and MID binding still run on the game thread.

## Schema v2, final emission rule, wing-strip evidence (after 0158adb / 613ba10)

Primary package is now `out/dec-r5` (scene v2, sidecar v2); `out/dec-r1` and `out/dec` stay as v1 regression inputs.

**1. Schema v2.** `FSCPackageReader` accepts scene `version` 1 or 2 (v2 only adds `lights`, `vehicle_light_ports`,
sidecar `emissive`/`decal` objects) and rejects anything else; sidecar versions other than 1/2 produce a warning.
New fixtures `scene_v2.json` (read) and `scene_v3.json` (rejected) in `FamilyMapping`.

**2. Final emission rule** (`docs/03-unreal-export-design.md`, row "final rule"), computed from authored
attributes in `Emission()`; the v2 `emissive` object is advisory (only its `emittance_texture` is used when
`%EMITTANCE_MAP` is set) — TODO(WP-R5) prefer the object once it encodes this rule:
strength = `Glow` if > 0, else 1 with `%EMITTANCE_MAP`, else 0 (× `GlowToEmissive`); colour = surface colour
(Diffuse × base texture, or the emittance map) × `Emissive` when non-zero, black `Emissive` = no tint.
Synthetic cases (all pass): `glows_like` (Glow 0.35, Emissive 0, orange Diffuse) → orange, 0.35 × k;
`emissives_like` (Glow 0.45, Emissive 1, blue) → b > g > r, 0.45 × k; `forward_glow` / `glow_zero`
(Emissive 1,1,1, no/zero Glow) → 0; `emittance_noglow` → 1 × k tinted by Emissive; `glow_emittance` (Glow 1.5,
`AlphaGlowCutoff` 0.25) → routing and cutoff. `PackageRules` (all three packages): every lit submaterial
without Glow and without `%EMITTANCE_MAP` has zero emission; every emitting one has a non-zero colour.
(The earlier package check matched the v2 `"glow"` key case-insensitively; checks are now case-sensitive.)

**3. Which material the wing-edge strips use.** The coordinator's scan covered the hull GLB only. The strips
are separate child entities (`*_Decal_Glow`, `*_GlowDecal`, `*_DecalGlow` GLBs); every one of their primitives
uses GLB material `AEGS_Gladius_Headlight_Glow_27` → sidecar ordinal 27 → `Headlight_Glow` (Illum, Diffuse
0.212/0.597/0.922, Emissive 1,1,1, **no Glow**, forward flag). UV-centroid samples of
`gladius_emissives_diff` (RGBA, 0–255) per glow GLB:

| GLB | tris | mean RGBA at triangle centroids |
| --- | ---: | --- |
| WingLeft_Decal_Glow / WingRight_Decal_Glow | 38 / 38 | (25, 64, 82, 95) — blue strip |
| WingLeft_Tip_GlowDecal / WingRightTip_GlowDecal | 53 / 51 | (5, 63, 76, 84) — blue |
| Body_DecalGlow_mesh / Nose_DecalGlow | 50 / 12 | (9, 101, 124, 136) / (11, 114, 140, 152) — blue |
| WingfinLeft/Right_DecalGlow | 12 / 12 | (159, 45, 7, 160) — orange (fin-tip points) |
| Headlights_glow | 100 | (252, 252, 252, 113) — white |

`emissives` (slot 13, Glow 0.3) sits on Body, Belly, Rudder_Left/Right, Ladder_Bottom, Moving_rack_shield,
door and gunrack primitives of the hull GLB and samples the **orange** regions of the same atlas (e.g. rudders
(170, 49, 0, 174), belly (159, 97, 0, 156)); with Diffuse blue its emission is a dim orange-brown
(≈ (0.13, 0.09, 0) × 0.3 × k), visible as the orange points on the rudder tips in `wp-u2/r5-rear.jpg`.
Conclusion: the blue strips, orange fin-tip points and headlights in the reference are **`Headlight_Glow`**,
which the final rule classifies as non-emissive (the "residual uncertainty" case the rule names); the
reference shows them glowing. Not changed here (rule is owner-decided); evidence for the D11 owner.
Also relevant: forward glow cards are masked at UE's default clip 0.3333 and these strips' coverage alpha
averages 84–96/255 (0.33–0.38), so about half their texels are clipped — a masked threshold is a poor fit
for soft glow cards. `GlowToEmissive` was not changed: `emissives` at 0.3 × 10 is visible; the remaining
dimness comes from the data/rule (no Glow on the strips; blue Diffuse × orange texels), not the constant.
No `controls.engine_glow` in dec-r5 (no target meets the exporter's rule), so the thruster test is moot there.

Commands and results:

```bash
python3 -I ue/scripts/make_test_tar.py out/dec-r5 ue/SCViewer/Saved/Import/review-dec-r5.tar   # files=502
ue/scripts/build.sh SCViewerEditor                                                           # Succeeded
for p in dec-r5 dec-r1 dec; do UnrealEditor-Cmd … test_importer.py -SCPackage=out/$p -SCVerifyTar=…/review-$p.tar …; done
UnrealEditor-Cmd … test_importer.py …                                                        # synthetic only
SC_ARGS="-SCView=35,40,1.2" ue/scripts/capture_views.sh out/dec-r5 u2c-3q lit
SC_ARGS="-SCView=-90,8,1.0" ue/scripts/capture_views.sh out/dec-r5 u2c-side lit
ue/scripts/capture_views.sh out/dec-r5 u2c lumen
SC_ARGS="-SCView=180,8,0.9" ue/scripts/capture_views.sh out/dec-r5 u2c-rear lit
```

dec-r5 / dec-r1 / dec: exit 0, 12/12 `passed=1`, `PACKAGE_RULES decals=11 glowing=8 no_glow=210`,
`VERIFY passed=1 … old_live=0`; synthetic only: exit 0, 10/10. GPU dec-r5: `TEXTURES requested=247 created=247
failed=0`, `MATERIALS sets=19 instances=273 engine_glow=0 … wait_s=0.0000`, `IMPORT_OK seconds=3.0076
warnings=4`. Screenshots `wp-u2/r5-three-quarter.jpg`, `r5-side.jpg`, `r5-lumen.jpg` (hull in Lumen Scene and
HWRT reflections, no black cut-out), `r5-rear.jpg`.

TODO(WP-R5): decide `Headlight_Glow`-type materials (Emissive only, forward flag, glow atlas) — the strips the
reference shows glowing; prefer the v2 `emissive` object once it encodes the final rule.
TODO(WP-U2): soft glow cards are masked at the default clip (≈ half of the strip texels clipped); revisit once
`alpha_config` carries the forward/coverage decision.

## Amended emission rule: forward glow cards (after e8337ed)

**Survey** (all sidecars of the export, not only referenced ones):

```bash
python3 -I ue/scripts/survey_materials.py out/dec-r5     # total 395 submaterial records
```

| family | forward (`MtlFlags & 0x8000`) | Emissive > 0 | Glow > 0 | count | examples |
| --- | --- | --- | --- | ---: | --- |
| Illum | yes | yes | no | 11 | Headlight_Glow, glows_a, decal_glow, Glow_Linked, buttons_linked |
| Illum | yes | yes | yes | 7 | emissives |
| Illum | yes | no | yes | 1 | glows |
| Illum | yes | no | no | 4 | glass, trim_02, glows, engine_glow (runtime-driven) |
| Illum | no | yes | yes | 4 | Decal_Glow_Unlinked, Gladius_Emissive, engine_glow, glows_a |
| Illum | no | no | yes | 2 | buttons, glows |
| Illum | no | yes | no | 1 | decal_pom |
| LayerBlend_V2 | no | yes | no | 42 | pipes / metals (never forward, never glow) |
| UIPlane / DisplayScreen | no | no | yes | 8 / 2 | UI_Graphics, hud_material |

**1. Rule** (`Emission()`): strength = Glow if > 0; else for Illum/HardSurface with the forward flag and a
non-black Emissive, mean(Emissive); else 1 with `%EMITTANCE_MAP`; else 0 (× `GlowToEmissive`). Colour = surface
colour (or emittance map) × Emissive when non-black. Tests (synthetic, all pass): `glows_like` orange 0.35k,
`emissives_like` blue 0.45k, `glow_zero` 0, `emittance_noglow` 1k, **`forward_glow`** (Headlight_Glow-like: forward
+ Emissive 1,1,1, no Glow → glow card, strength 1k, colour from the base texture), **`layerblend_like`** (Emissive
1,1,1, no Glow, not forward → 0), `forward_trim` (forward, non-emitting → masked at the engine default clip).

**2. Glow cards**: an emitting forward Illum surface becomes `M_SC_Emissive_Unlit` (additive, Substrate unlit),
`AlphaWeight = 1`: emission × texture alpha × step(alpha > `AlphaGlowCutoff`), no clip, so the 0.3–0.4 gradients
survive; `bNonNanite` puts it in WP-U1's split mesh (split criterion now: Glass | MeshDecal | glow card,
through `FSCMaterialBuilder::RequiresNonNanite`). Non-emitting forward materials keep the masked path.
`PackageRules` checks every card is additive, alpha-weighted, emitting and non-Nanite.

**3. Results.** dec-r5 / dec-r1 / dec: exit 0, 12/12 passed,
`PACKAGE_RULES decals=11 glow_cards=7 glowing=6 no_glow=205`, `VERIFY passed=1`; synthetic-only 10/10.
GPU dec-r5: `TEXTURES created=247 failed=0`, masters `Illum=42 Emissive_Unlit=22` (9 cards moved from masked
Illum), `IMPORT_OK seconds=6.56` (first start after regenerating the unlit master: on-demand shader compile ran
concurrently; earlier warm runs 2.9–3.1 s). Screenshots `wp-u2/r5-three-quarter.jpg`, `r5-side.jpg`,
`r5-rear.jpg`, `r5-lumen.jpg`, refreshed `three-quarter-vs-reference.jpg`: blue strips glow at the wing tips,
wing leading edges, fin roots and under the rear module; orange fin-tip points; Lumen Scene / HWRT unchanged
(hull present, no cut-out; additive cards are, like glass, not in the surface cache).
