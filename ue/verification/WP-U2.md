# WP-U2 verification — 2026-11-09 (Opus 5.5, RTX 4090 / Vulkan host)

Base: superproject `d39e298` (main). UE 5.8.2 CL 56702186. Test package `out/dec-r1`
(final-exporter Gladius, 23 `*.materials.json`, 366 PNGs). No cargo, no `vendor/` or
`crates/` changes. Commands are from the repository root.

> **2026-11-10 — emissive units superseded (WP-U3b §13.3).** `GlowToEmissive` (10 UE units per authored `Glow`),
> named in the dated records below, no longer exists. `EmissiveStrength` is now cd/m² from the game renderer's
> own rule: an authored Glow emits `SceneUnitNits × sqrGlowFactor × Glow²` (`FSCStarEngine::GlowNits`), an
> effect-system Glow (`controls.engine_glow`) is per-instance glow, `SceneUnitNits × amount`
> (`InstanceGlowNits`). The rule tables and the fallback register below are current; the dated round records
> keep the numbers measured at the time.

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
  (4 LayerBlend_V2 base layers, blend-mask R/G/B, per-layer tiling/tint/F0/gloss; the `Metallic` scalars were removed with
  the static-wear change: albedo and F0 are authored directly and split by `Graph.conserve`),
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
  threads (`ParallelFor`), transient `UTexture2D` (colour encoding from the PNG's `sRGB` / `gAMA` chunk, role fallback sRGB only for
  colour — see "Static wear" below; `TC_Normalmap` linear for DDNA
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
| Emission | `Emissive` RGB, `Glow` (or `GlowAmount`), `public_params.AlphaGlowCutoff`, `%EMITTANCE_MAP` + emittance slot | strength in cd/m² from the game's glow rule, `SceneUnitNits × sqrGlowFactor × Glow²` (`FSCStarEngine::GlowNits`, WP-U3b §13.3); a material that emits without a Glow (forward glow card, `%EMITTANCE_MAP`) gets one scene unit of instance glow (fallback register below); **otherwise Glow absent/0 → 0** (no `emissive_factor()` Glow=1 fallback); colour = Emissive × Diffuse × base texture, gated by alpha > cutoff. Deviation: Glow > 0 with Emissive = 0,0,0 (`glows`, `buttons`) is CryEngine diffuse glow → Emissive treated as white |
| Relief decal | MeshDecal + `%PARALLAX_OCCLUSION_MAPPING`, no `%DECAL`/`%STENCIL_MAP`/stencil slot/`tint_palette_decal`; TexSlot1 alpha, TexSlot3 `normal_gloss`, TexSlot4 `height`, `PomDisplacement`, `DecalAlphaMultiplier` | `M_SC_MeshDecal_Relief`: normal + roughness into the DBuffer under TexSlot1 alpha × DecalAlphaMultiplier, POM as bump offset — the dark grooves |
| Colour decal | MeshDecal/`%DECAL` with `base_color` (gladius_text_diff, warningtape) , `DecalAlphaMultiplier`, `DecalDiffuseOpacity` | `M_SC_MeshDecal`: sheet RGB × Diffuse where alpha × DAM × DDO |
| Stencil decal | `%STENCIL_MAP`, `$TintPaletteDecal` | r1's only stencil is inactive and `palettes.json` `decal.export_path` is null → hidden; TODO below |
| Glass | `Diffuse` (transmissive colour), `%USE_TINT_PALETTE` + `palette_routing.material_channel` (glass) → `palettes.json` `glass` by `public_params.GlassTintStrength`, `GlassTiling` / `GlassVStretch`, `%PER_VERTEX_TINT` / `%PER_VERTEX_CURVATURE`, `%TINT_MAP` TexSlot4, `%HEIGHT_MAP` TexSlot8, `%THIN_FILM` + `ThinFilmThickness`, `Specular` (F0), DDNA + derived roughness; `Opacity` is not read (rule: "Glass" section below) | `M_SC_Glass`, non-Nanite |
| Layers | `layer_manifest[]` base layers (name not `Wear*`): `diffuse/normal/roughness_export_path`, `tint_color`, `palette_channel`, `uv_tiling`, `gloss_mult`, `layer_snapshot.{diffuse,specular,shininess}`; `blend_mask` slot + `BlendMapTiling` | tint = snapshot diffuse × tint × palette channel; F0 = snapshot specular (superseded by the palette `TintMode` / finish rule in "Static wear" below; `layer_snapshot.metallic` is no longer read) |
| Roughness | `derived_textures[]` role roughness paired by `source_path`; `Shininess` | roughness = 1 − min(smoothness, 251/255) (since 2026-11-10, see "Macro surface, HAL and the gloss convention"; before: sqrt(1 − smoothness)), smoothness = (1 − r²) × Shininess/255 with r the exporter's sqrt(1 − gloss) map; placeholder Shininess=255 without map → 0.5 (mtl.rs) |
| Specular | `Specular`, `specular_support` map; Diffuse = Specular = 1,1,1 sentinel without map → dielectric default | F0 colour (Substrate slab) |
| Host tint | POM colour decals: dominant `palette_routing.material_channel` of the host mesh's slots by triangle count (Blender add-on rule) | `HostTint` per channel instance |
| Engine glow | `controls.engine_glow` targets (`material_sidecar`, `source_material_index` = authored `index`), `default_strength` | strength from the control (`default_strength`), clamped to its range; `material_property_glow` is an entity-effect Glow amount = per-instance glow, linear: `SceneUnitNits × amount` cd/m² (`FSCStarEngine::InstanceGlowNits`, WP-U3b §13.3) |

### Fallback register (host constants)

| Constant | Value | Why | Trigger to remove / sunset |
| --- | --- | --- | --- |
| Unit instance glow (`Emission()` in `SCMaterialDescribe.cpp`: rule strength > 0 with Glow 0) | one scene unit (`SceneUnitNits` = 1000 cd/m², from the fixture) of instance glow for a material that emits without an authored `Glow` (forward glow card, `%EMITTANCE_MAP`) | the game lights these through the instance-glow term of `CalculateEmissive` (light-linked glow, `glowMultiplier` × the linked light's state; effect links); that amount is not in the export, and material Glow 0 alone would switch them off | derive the amount from the light link (`glow_multiplier` × light state) or the entity-effect link once the exporter carries the link target; retired `GlowToEmissive` (10 units per Glow) was replaced by the game's rule on 2026-11-10 (WP-U3b §13.3) |
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
(TexSlot9/3/12), HardSurface iridescence (TexSlot10),
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
| roughness constant | `Shininess`/255 → smoothness | Substrate default roughness 0.5 (smoothness 0.5 since 2026-11-10, 0.75 under the earlier sqrt convention); TODO(WP-R5) emit gloss when `Shininess` is absent |
| roughness map scale | `Shininess`/255 × layer `gloss_mult` | identity (1) |
| F0 | `Specular` (× `_spec` map), layer snapshot `specular` | Substrate default F0 0.04; TODO(WP-R5) resolve the `Diffuse=Specular=1,1,1` sentinel upstream |
| albedo | `Diffuse` × base texture × palette | texture as-is; with neither, Substrate default albedo 0.18 |
| emission | `Emissive` × strength (cd/m², game glow rule — WP-U3b §13.3) × albedo, alpha-gated by `AlphaGlowCutoff` | none (Glow absent/0 or Emissive 0,0,0 → 0); TODO(WP-R5) confirm Glow>0 with Emissive 0,0,0 (`glows`, `buttons`) |
| mask cutoff | `AlphaTest` | forward cards: UE default clip (`UMaterial` CDO) |
| multipliers (`DecalAlphaMultiplier`, `DecalDiffuseOpacity`, `StencilOpacity`, `uv_tiling`) | public params / manifest | identity (1) |
| glass parameters (`GlassTintStrength`, `GlassTiling`, `GlassVStretch`, `ThinFilmThickness`) | public params | the shader's declared default from `StarEngineRender.json` (since the "Glass" section below) |

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

## Engine-glow control (WP-R5 regenerated dec-r5)

`controls.engine_glow` in dec-r5: range 0–200 `emission_strength`, default 0, four targets on
`AEGS_Gladius_Thruster_Main_TEX2.materials.json`: `engine_a` (Illum, AlphaTest 0.03), `engine_b` (Illum),
`text_decals` (Illum, `MtlFlags` 160, not forward), `engine_glow` (Illum, forward, Diffuse blue, glow atlas).

Implementation: `FSCImportMaterials` applies `ApplyEngineGlow(default_strength)` to each target; the colour stays
the rule's (surface colour × Emissive when non-black — all four have black Emissive, so the surface colour
itself, never a fixed blue); strength is the control value 1:1 in UE emissive units, clamped to min/max by
`ASCVehicle::SetEngineGlow` (`sc.EngineGlow`, harness `-SCEngineGlow=`). A forward-flag Illum target becomes a
glow card (additive, alpha-weighted) and the D6 split now asks `RequiresNonNanite(Sub, bEngineGlowTarget)`, so it
lands in the non-Nanite mesh; non-forward targets stay lit Nanite surfaces whose emissive parameter is driven.
Tests: driven forward target → card, non-Nanite, strength 50, no tint; driven non-forward target stays Illum
Nanite. All three packages: 12/12 passed, `VERIFY passed=1`; GPU log `MATERIALS … engine_glow=4`.

GPU, `SC_ARGS="-SCView=180,3,1.0 -SCEngineGlow=<v>" ue/scripts/capture_views.sh out/dec-r5 …`:
`wp-u2/rear-engine-glow50.jpg`, `wp-u2/rear-engine-glow5-vs-reference.jpg` (ours left, `reference/gladius/rear.webp`
right), `wp-u2/nozzle-engine-glow5.jpg`. **Finding:** driving the targets as linked lights up the whole thruster
housing (`engine_a`/`engine_b` cover it; their dark-grey diffuse × 50 saturates to white, × 5 reads lavender-grey),
while the reference glows blue-white only inside the nozzle oval. The `engine_glow` card inside the oval is not
distinguishable from the housing glow. `text_decals` (alpha-gated glyphs on the housing) shows at most faint light
marks along the housing at strength 5 — it cannot be separated from the `engine_a/b` glow, so the capture neither
confirms nor rules out the link. Conclusion for WP-R5: the `engine_a`/`engine_b` (and probably `text_decals`) links
drive the whole housing's emission, which does not match the reference; the material side follows the data.
TODO(WP-R5): re-check which submaterials the DataCore Glow links should target (only the nozzle glow surface
glows in the reference). TODO(WP-R5): switch to the v2 `emissive` object once it carries `forward_rendering` and the
glow-card strength (dec-r5 still encodes the pre-amendment rule, e.g. Headlight_Glow strength 0).

## v2 emissive / alpha_config, engine-glow units (dec-r5 regenerated by WP-R5)

**Builder.** When a submaterial carries both v2 objects, `Describe` reads `emissive` {strength, color (tint),
emittance_texture, alpha_glow_cutoff, uses_base_texture} and `alpha_config` {alpha_test, opacity, two_sided,
forward_rendering}; v1 sidecars use the attribute path (`Describe(…, bUseV2Objects=false)` forces it for
comparison). Emission = surface colour × tint × strength × `GlowToEmissive`; tint applied once (the v1 path's
glow card now emits strength 1 with Emissive as tint, matching WP-R5's pending correction).
`uses_base_texture = false` is not supported by the masters (warning, TODO(WP-U2)); dec-r5 has none.

**Engine glow.** `controls.engine_glow.units` is read; `material_property_glow` maps the control value onto the
target's Glow, so emissive = value × `GlowToEmissive` (same host constant), clamped to `min_strength`/
`max_strength` (`FSCMaterialBuilder::EngineGlowToEmissive`, used by the import default and
`ASCVehicle::SetEngineGlow` / `-SCEngineGlow=`); legacy `emission_strength` stays 1:1. The per-link
`multiplier`/`minimum`/`maximum` chains are not applied (the control value is the Glow value).

*Superseded 2026-11-10 (WP-U3b §13.3):* the control value is not an authored material Glow. It is the output
of a DataCore entity-effect link with `materialProperty == Glow`, which the game adds as per-instance glow
(`CalculateEmissive`: `instData.glow × glowColour`, no conversion) — so emissive = `SceneUnitNits × value` cd/m²
(`FSCStarEngine::InstanceGlowNits`), still clamped to the control range; legacy `emission_strength` stays 1:1.

**v2 vs attributes** (`PackageRules` on dec-r5, informational `V2_DIFF` lines): 226 v2 submaterials, **68 disagree,
all only in `two_sided`**: v2 takes it from `MtlFlags & 0x2` (e.g. hull `metal_*`, interior trims, `emissives`
two-sided), the attribute path from `mtl.rs is_double_sided` (every MeshDecal / `%DECAL` two-sided; v2 makes all
41 MeshDecals single-sided). Strength, tint, forward flag, blend and master agree for every submaterial
(Headlight_Glow strength 1, emissives 0.3, engine_glow 0 until driven).

**Tests.** New: `card_tinted` (tint once), `v2_object` (v2 wins over attributes; v1 path emits nothing),
units/clamp of the control. dec-r5 / dec-r1 / dec: exit 0, 12/12, `VERIFY passed=1`
(`PACKAGE_RULES … v2=226 v2_disagree=68` on dec-r5, `v2=0` on v1 packages); synthetic only 10/10.

**GPU rear** (`SC_ARGS="-SCView=180,3,1.0 [-SCEngineGlow=50]" ue/scripts/capture_views.sh out/dec-r5 …`):
`wp-u2/rear-engine-glow-default10-vs-reference.jpg` (default 10, ours left / reference right) and
`wp-u2/rear-engine-glow50-v2targets.jpg`. The housing no longer glows (only `engine_glow` is targeted), but **no nozzle
glow is visible at 10 or 50**. Checked: `MATERIALS … engine_glow=1`; the thruster's non-Nanite split holds 331
triangles (272 `engine_glow` + 43 details + 16 text_decals); the card samples a blue atlas region (mean RGBA
33/99/120/156, 3 % alpha-0); it is a 1.0 × 0.35 m plane 2–14 cm inside the nozzle exit facing out (−Y);
an experiment with a two-sided unlit master (reverted) changed nothing, so facing/culling is not the cause.
Remaining suspects: occlusion by the nozzle grille geometry (`engine_a`/`engine_b`, masked at their authored
AlphaTest 0.03/0.02) or the vertex colours the card carries (not used by the masters; CryEngine may modulate
glow cards with them). TODO(WP-U2): resolve the invisible nozzle card (depth/visibility debug of the
`engine_glow` component, vertex-colour semantics).

## Nozzle card fixed: forward Illum `%DECAL` is a glow-only overlay

Cause, confirmed in the builder: thruster `engine_glow` (Illum, `MtlFlags` 50364576 = forward, `StringGenMask`
`%DECAL`, public params `DecalDiffuseOpacity 0`, `DecalAlphaMult 1`) was routed by the `%DECAL` token to the
DBuffer colour-decal master, whose coverage = alpha × Opacity × DecalAlphaMultiplier × **DecalDiffuseOpacity (0)**
— so it drew nothing, at any strength; `ApplyEngineGlow` then saw a MeshDecal master and never made it a card.

Fix (`Describe`): family comes from the shader name; a forward-flag Illum with `%DECAL` is never a DBuffer decal
or lit-translucent surface — it takes the glow-card path (additive unlit, coverage = texture alpha ×
`AlphaGlowCutoff` gate only), even at strength 0 so the engine-glow control can drive it. Decal opacity
parameters never touch its emission or coverage; its non-emissive colour (Diffuse × DecalDiffuseOpacity) is not
drawn by the additive master (0 here). TODO(WP-U2): a forward `%DECAL` Illum with DecalDiffuseOpacity > 0 would
also need its diffuse overlay. Test `decal_glow_overlay` (forward Illum + `%DECAL` + DDO 0 → glow card, driven to
100). `PackageRules` now counts it: `glow_cards=8`.

Diagnostics (game host log, dec-r5):

```text
ENGINE_GLOW_BOUND entity=AEGS_Gladius_Thruster_Main component=Mesh_21 slot=4 nanite=0 visible=1 bounds_cm=X=94.849 Y=80.354 Z=58.557 material=MID_engine_glow_0
ENGINE_GLOW_BOUND entity=AEGS_Gladius_Thruster_Main component=Mesh_23 slot=4 nanite=0 visible=1 bounds_cm=X=94.849 Y=80.354 Z=58.557 material=MID_engine_glow_0
ENGINE_GLOW control=50 emissive=500 instance=MID_engine_glow_0 parent=M_SC_Emissive_Unlit readback=500
```

The card is bound on both thrusters, visible, in the non-Nanite split component (bounds ≈ 1.9 × 1.6 × 1.2 m,
not culled), and the driven strength reaches the instance (control 50 × `GlowToEmissive` = 500, read back).

Tests: dec-r5 / dec-r1 / dec exit 0, 12/12, `VERIFY passed=1`; synthetic only 10/10.
Rear captures (`SC_ARGS="-SCView=180,3,1.0 [-SCEngineGlow=50]" ue/scripts/capture_views.sh out/dec-r5 …`):
`wp-u2/rear-nozzle-glow10-vs-reference.jpg` (control default 10) and `wp-u2/rear-nozzle-glow50-vs-reference.jpg`
(ours left, `reference/gladius/rear.webp` right): only the nozzle interior glows, blue-white with brighter ring
lines, matching the reference; the housing stays unlit; at 50 the glow blooms beyond the nozzle.


## Macro surface, HAL and the gloss convention (2026-11-10, WP-P2 "armour look")

Defect: armour (LayerBlend_V2) read flat and too bright; the layered masters drew only the per-layer textures.
Rules below are read from the game's own shader sources (decoded `Engine/ShaderCache_Vulkan.pak`:
`LayerBlend_V2.cfx`, `HardSurface.cfx`, `layerLib.cfi`, `NormalsCommon.cfi`, `ShadeLib.cfi`; the decoded text is
game-derived scratch and is not in git). No asset names, no values copied from assets or reference images.

| Rule | Game shader | Sidecar field | Master parameter |
| --- | --- | --- | --- |
| Macro normal over the blended layers | `LayerAttribs_AddMacroSurface`: `NormalTS = AddDetailNormals(macro, layer.NormalTS)` | `TexSlot3`, role `normal_gloss`, gated by `%NORMAL_MAP` (LayerBlend_V2) / `%MACRO_NORMAL_MAP` (HardSurface) in `string_gen_mask` | `MacroNormalMap` |
| Macro gloss | same function: `Smoothness *= macroSmoothness` (DDNA alpha) | the slot's `derived_textures[]` roughness | `MacroRoughnessMap`, `UseMacroGloss` |
| Macro UVs | LayerBlend_V2: base UVs; HardSurface: `baseTC × MacroTiling` | `public_params.MacroTiling` | `MacroTiling` (1 for LayerBlend_V2) |
| HardSurface macro AO | `%MACRO_AO_MAP`: `Albedo *= AO; Reflectance *= AO` | `TexSlot1` (exported role `base_color`), loaded linear, red channel | `MacroAOMap` |
| HAL map AO | LayerBlend_V2 `HueAoLumMap` (TexSlot13), G: `Albedo *= AO; Reflectance *= AO` after the blend | `TexSlot13`, role `hal_control` | `HalMap`, `UseHalMap` |
| HAL luminance | `Layer_ApplyLayerEffects` per layer, before accumulation: `saturate(colour × (lumShift × 0.2 + 0.8))`, `lumShift = B × 2` (`HAL_LUM_MAX_* = 0.2`), on albedo and reflectance | same map, B | same |
| Layer blend | `LayerAttribs_MultiplyAdd` accumulates Albedo, Reflectance, NormalTS.xy and **Smoothness**; `LayerAttribs_BlendNormalize` = `ReconstructNormalZ(xy)` | — | graph: smoothness is blended (was roughness), normal XY blended and Z rebuilt (was normalize(xyz)) |
| Gloss → roughness | `GlossToRoughness(g) = (1 − min(g, 251/255))²` is the GGX alpha (`NDF_GGX(alpha)` squares it once more: `alpha2/denom²`) | — | UE GGX alpha = Roughness² ⇒ **Roughness = 1 − min(gloss, 251/255)** in every lit master (`Graph.gloss_to_roughness`); before: `sqrt(1 − gloss)` |

`AddDetailNormals(base, detail)`: `base.z += 1; detail.xy *= −1; normalize(base × dot(base, detail) − detail × base.z)`;
it is equivariant under the importer's green flip applied to both inputs, so no extra handedness correction.
The macro normal is selected by slot number with a role check: 65 armour LayerBlend_V2 submaterials also carry a
POM decal normal in `TexSlot2` (same role), and two Scorpius `%MACRO_NORMAL_MAP` submaterials have a `TexSlot3`
of role `alternate_base_color` (not used). A `TexSlot3` normal without the token is not used (fixture
`layerblend_like`).

**The gloss convention is global.** Every lit master now uses the game-exact mapping, so all surfaces with
authored gloss above 0 are glossier than before (gloss 0.5: roughness 0.5 instead of 0.71; gloss 0.9: 0.1 instead
of 0.32). Constant-smoothness defaults moved accordingly (Substrate default roughness 0.5 = smoothness 0.5).

Not implemented (warnings where the data asks for it): HAL hue shift (R, needs the game's `HueLUT` volume),
`RotateLayerUVs` (0 in every surveyed sidecar), `%USE_UV2_FOR_MACRO_MAPS` and LayerBlend wear (runtime
per-instance; pristine = none). HardSurface static wear and vertex-colour AO were open here and are drawn
since the "Static wear" section below.
TODO(WP-U2): derived roughness maps are mip-mapped and filtered as `sqrt(1 − gloss)` (exporter encoding,
`FSCTextureImporter::BuildMips`); the game filters linear gloss. Minified texels with varying gloss come out
glossier (two texels gloss 0 and 1 average to 0.75 instead of 0.5). Needs a gloss texture role (decode to
linear gloss before mips) and a cache-format bump. Pre-existing; the macro gloss inherits it.
TODO(WP-U2): `MasterParameters` counts 44 (was 52 before static wear removed `Metallic`) descriptor parameters that masters outside the layered family do not
have (shared describe code: `AlphaTest`/`Smoothness`/… on `M_SC_Emissive_Unlit`, and
`EmissiveColor`/`EmissiveStrength`/`DiffuseColor`/`SpecularColor`/`HostTint` on `MI_SC_MeshDecal_Relief`); MIDs
drop them silently. Triage which of them are authored values that should be drawn.

### Tests

`SCBridge.Materials.FamilyMapping` (fixture `synthetic.materials.json`): `hard_primary` (macro normal + gloss,
`MacroTiling` 3, macro AO linear, no HAL), `blend4` (macro normal = TexSlot3 and not the TexSlot2 decal normal,
HAL linear, `UseHalMap` 1), `blend1` / `layerblend_like` (no macro), `glow_zero` (default smoothness 0.5);
deduplicated texture requests 27 → 33. New `SCBridge.Materials.MasterParameters`: every parameter described for a
layered-family fixture material exists in its master (a MID drops unknown names silently); this found
`UseBlendMask` / `BlendMaskTiling` written for the one-layer `M_SC_HardSurface` (now Layered only). The tests
check selection and parameter presence, **not graph output**: the graph math is verified by the GPU captures only.

```bash
ue/scripts/build.sh SCViewerEditor                                             # Result: Succeeded
UnrealEditor-Cmd … -run=pythonscript -script=ue/scripts/create_materials.py …  # WP-U2 MATERIALS_OK
UnrealEditor-Cmd … -script=ue/scripts/test_importer.py -nullrhi …              # exit 0, 17/17, WP-U1 UNIT_TESTS_OK
#   MASTER_PARAMETERS present=383 layered_family=4 dropped_other_families=52
UnrealEditor-Cmd … -script=ue/scripts/test_player.py -SCPlayerTests \
  -SCPlayerPackage="$PWD/out/fps/player-p4-male.tar" -nullrhi …                # exit 0, 5/5
ue/scripts/capture_route.sh p2r3-cutlass ue/scripts/routes/cutlass-armed-walkthrough.txt -SCDefaults \
  -SCLoad=DRAK_Cutlass_Black -SCCamera=first \
  -SCPlayerLoadout=Data/Scripts/Loadouts/Player/DefaultLoadouts/PVE_medium_behr_rifle_gmni_rifle.xml
SC_SETTLE=20 ue/scripts/capture_viewer.sh armour-gladius 'VIEWER_LOAD result' -SCDefaults -SCLoad=AEGS_Gladius
```

### GPU result (captures re-taken after the cross-review fixes)

Route `p2r3-cutlass`: `SCRIPT_DONE steps=149`, 29 shots, 0 material compile errors; every `SCRIPT_SHOT pos=` is
within 7.8 cm of the round-2 run (`p2r2-cutlass`), so the pairs below are the same view. (The first after-run
was perturbed and not position-matched; it was discarded.) Gladius: `VIEWER_LOAD result=Succeeded`,
`AUTOMATION_DONE all_succeeded=1`. Before | after sheets (half size here, full size `out/smoke/armour-r3-*.png`):

- `wp-u2/armour-hangar-before-after.jpg` (shot 04, neutral hangar light): **better**. Plates are darker in
  their recesses and seams (HAL AO), the helmet and limb plates show panel relief and the backpack shows fabric
  folds (macro normal), olive plates are deeper (HAL luminance). White torso and legs stay white: that is the
  loadout variant's authored tint.
- `wp-u2/armour-cockpit-before-after.jpg` (shot 23): helmet seams, strap and collar relief are visible where
  the before image is a smooth shell; the scene is still very bright.
- `wp-u2/armour-bulkhead-before-after.jpg` (shot 20, Cutlass interior): **not fixed**. The armour is blown out
  to white before and after; with the game-exact gloss the metallic plates now mirror the interior lights more
  sharply (the shoulder plate reads as polished metal). The cause is the over-bright ship interior lights
  (open light-intensity defect), not the armour material; wear, which would break the mirror up, is not drawn.
- `wp-u2/gladius-gloss-before-after.jpg`: **ships changed**. The Gladius hull is visibly glossier (sharp
  highlights on the nose, canopy frame and wing edges) than before and than the in-game references, which show
  satin paint broken up by wear and dirt. The mapping is the game's; the difference is the 18 % static wear the
  game always blends in and we do not (TODO above). Kept game-exact rather than tuned by eye.

Frame time, same cockpit view as WP-P2 §4 (single run each, different sessions):

```text
SCRIPT_FRAMETIME tag=body-a       frames=805  frame_ms[mean=7.45 p50=6.49 p95=12.52] gpu_ms[mean=6.17 p50=6.19 p95=6.73]
SCRIPT_FRAMETIME tag=capsule-only frames=1030 frame_ms[mean=5.83 p50=5.76 p95=10.10] gpu_ms[mean=5.54 p50=5.52 p95=6.02]
SCRIPT_FRAMETIME tag=body-b       frames=800  frame_ms[mean=7.50 p50=6.37 p95=12.54] gpu_ms[mean=6.08 p50=6.04 p95=6.63]
```

Against round 2 (body 7.23 / 5.71 GPU, capsule-only 5.43 / 5.13 GPU): about +0.4 ms GPU (+8 %) with and
without the body, i.e. the extra macro/HAL samples cost on the ship's materials too. Not A/B'd inside one
session, so part of it may be run-to-run noise.

### Cross-review (Codex, on the uncommitted diff)

| Finding | Verified | Disposition |
| --- | --- | --- |
| HAL luminance saturated after the blend; the game applies it per layer (`Layer_ApplyLayerEffects` before `LayerAttribs_MultiplyAdd`) | yes (`LayerBlend_V2` layer loop, `Layer_ApplyComplementWearAndHAL`) | **fixed**: per-layer `saturate(colour × lum)` on albedo and F0; AO stays after the blend |
| Blended layer normal was `normalize(lerp xyz)`; the game accumulates XY and rebuilds Z | yes (`LayerAttribs_MultiplyAdd`, `LayerAttribs_BlendNormalize`) | **fixed**: `Graph.reconstruct_z` (pre-existing since the first layered master) |
| Gloss is filtered in the exporter's `sqrt(1 − gloss)` encoding | yes (`BuildMips` averages the encoded bytes) | **open**, TODO above (pre-existing, texture role + cache format) |
| Tests cannot detect a broken graph | yes | **partly**: `MasterParameters` added (presence); graph-output tests not added, stated above |
| Selection logic against 52 ship and 59 player-kit sidecars, token gates, green flip, linear roles, `251/255`, HAL constants | reviewer found no defect | — |

## Static wear, palette finish, colour encoding and the energy split (2026-11-10)

Defect: ship paint read too glossy and too clean against the in-game references. Four causes, all categories
rather than assets; every rule is read from the game's shader sources (decoded `ShaderCache_Vulkan.pak`:
`HardSurface.cfx`, `layerLib.cfi`, `layerLibConstants.cfi`, `paletteLib.cfi`, `TiledShading.cfx`; scratch, not
in git), DataCore or the P4k. Nothing is tuned from the reference images.

### 1. HardSurface static wear

| Rule | Game shader | Sidecar / mesh field | Master (`M_SC_HardSurface`) |
| --- | --- | --- | --- |
| Vertex data | `VertexData_FromVertColor`: damage = R, glow = G, AO = B, wear = A (`LayerBlendVertexData` 0) | glTF `COLOR_0`, used only with `%VERTCOLORS` in `string_gen_mask` | `UseVertexColors`; without it the graph uses glow 1, AO 1, wear 0 |
| Wear amount | `CalculateVertexWear(v) = v × 0.95`; `objectWear = max(WearBlendBase, instWear)`; `wearAmt = saturate(objectWear − wear)` | `public_params.WearBlendBase` (per-instance wear is runtime: 0) | `WearBlendBase` |
| Blend alpha | `CalcAlphas_BlendBased` → `CalcBlendAlpha(blendMap, wearAmt, WearDirtBlendFalloff)` on the Primary layer's TexSlot8 blend map at the Primary tiling | Primary layer `texture_slots[TexSlot8]` (linear), `WearDirtBlendFalloff` | `WearBlendMap`, `WearFalloff`; custom node `WEAR_ALPHA` (the shader's arithmetic verbatim) |
| Wear weight | `Layer_RemapWear`: paint starts the wear layer at `1 − PaintPrimerStart`, cloth at `1 − ClothComplementStart` | Primary layer `ShadingModel` | `WearPaint`, `WearCloth`; `WEAR_REMAP` |
| Paint primer | `Layer_ApplyLayerEffects`, paint branch: primer colour / gloss from the tinted primary between `PaintPrimerStart` and `PaintComplementStart` | — | `WEAR_PRIMER` (on the tinted primary albedo and smoothness) |
| Separate wear layer | `%WEAR_LAYER`: `BlendLayers_SeparatePrimaryAndWear`, second layer with its own maps, tint, F0, gloss | `layer_manifest[]` layer `Wear` | `Wear*` parameters (same set as `Layer0*`) |
| Tinted wear | no `%WEAR_LAYER`: `BlendLayers_TintedPrimaryWear` + `LayerAttribs_ApplyWearTints`: the primary maps with wear tints | `WearDiffuseColor`, `WearSpecularColor`, `WearGlossiness`, palette `wear_tint` / `wear_gloss` | `Wear*` filled from the Layer0 maps × the wear tints |
| Vertex AO | `attribs.Albedo.rgb *= attribs.AO; attribs.Reflectance.rgb *= attribs.AO` | `COLOR_0` B | graph |
| Emission | `GetMaterialAttribs`: `glowColour = attribs.Albedo` (wear-blended, after AO) `× saturate((alpha − AlphaGlowCutoff) / (1 − AlphaGlowCutoff)) × vertData.glow` | `COLOR_0` G | `Graph.glow(..., ramp=True)` × vertex glow (other masters keep their step gate) |

The constants are the game's `#define`s in `layerLibConstants.cfi`: `PaintComplementStart 0.9`,
`PaintPrimerStart 0.5`, `ClothComplementStart 0.75` (the cross-review could not see this file; it was missing
from the review bundle). Albedo, F0, smoothness and normal XY are blended primary(worn) ↔ wear by the wear
weight, then macro normal / gloss / AO as before.

When an input the rule needs is missing (no Primary blend map, no wear layer and no wear tints) the material
imports without wear and warns `WearBlendBase %.3f is not drawn: …` (fixtures `hard_wear_no_blend_map`,
`hard_wear_no_tints`).

### 2. Texture colour encoding from the DDS

The exporter converted every DDS to an untagged PNG and the importer guessed sRGB from the slot role; blend
maps, spec maps and HAL maps in colour slots were therefore decoded as sRGB (or the reverse).

- Exporter (submodule `scviewer/palette-gloss`, `15625916`): `DdsFile::is_srgb()` (CryEngine image flag or an
  `_SRGB` DXGI format) → `tag_png_color_encoding`: an `sRGB` chunk, or `gAMA` 100000 for linear data.
- Importer: `FSCTextureImporter::ResolveSRGB`: `sRGB` chunk → sRGB; `gAMA` 1.0 → linear; neither → the role
  fallback (Color: sRGB, Linear: linear), i.e. the previous behaviour. Normal maps are always linear. The CPU
  mip chain and `MeanColor` linearise only sRGB-resolved texels.
- `scviewer-core::CACHE_FORMAT_VERSION` 13 → **14**, so cached packages are re-exported with tagged PNGs.
- Limitation (cross-review finding 4, not fixed): `decomposed … --skip-existing-assets` into a directory
  written by an older exporter keeps its untagged PNGs (`textures.rs`, `decomposed.rs`, `entity.rs` skip
  existing files), which then take the role fallback. Re-export into a fresh directory. The viewer's own cache
  is covered by the version bump.

### 3. Palette finish and `TintMode`

`paletteLib.cfi` `LayerAttribs_ApplyTintsAndPalette`: `Albedo *= layerDiffuse; Smoothness *= layerGloss ×
gloss` unconditionally; `if (tintDiffuse) Albedo *= palette diffuse`; `if (tintSpecular) Reflectance *= palette
specular else Reflectance *= layerSpecular`.

| Rule | Source | Sidecar / palette field | Parameter |
| --- | --- | --- | --- |
| Palette specular and gloss per channel | DataCore `TintEntry.specColor`, `TintEntry.glossiness` | `palettes.json` `finish.<channel>.{specular, glossiness}`; glossiness is exported as a 0..1 fraction of the UNorm8 field (submodule `f9ae12dd`; was the raw byte) | `<P>Specular`, `<P>GlossMult` / `<P>Smoothness` |
| Which of diffuse / specular the palette replaces | layer `TintMode` | layer params | `bTintDiffuse = palette && mode ≠ SpecularOnly`, `bTintSpecular = palette && mode ≠ DiffuseOnly` |
| Gloss multiplier without a gloss map or `Shininess` | `Smoothness *= layerGloss × gloss` is unconditional | `gloss_mult`, palette glossiness | `<P>Smoothness = (Shininess or the default 0.5) × GlossMult × palette gloss` (cross-review finding 2) |
| Layer spec map | `LayerAttribs_ReadSpecular`: `Reflectance *= LayerSpecular.Sample` under `%USE_SPECULAR_MAPS`, before the palette step | layer `texture_slots[TexSlot6]` | HardSurface with the token samples `<P>SpecularMap`; otherwise see the fold below |

Inferred, not read from the shader:

- **`TintMode` numbering** (0 diffuse + specular, 1 diffuse only, 2 specular only) comes from the DataCore
  enum's value names; the shader only sees the two booleans.
- **Spec-map mean fold.** A layer whose shader does not sample its TexSlot6 spec map (LayerBlend_V2, and
  HardSurface without `%USE_SPECULAR_MAPS`) gets `F0 × the map's linear-light mean colour` (`VectorMeans` →
  `FSCMaterialBuilder::ApplyTextureMeans`, decoded on the worker with the other textures). This approximates
  what the layer library's sampling variant would produce; it is not a rule of the non-sampling shader. The
  fold also scales a palette-selected specular: the sampling shader multiplies the map before
  `ApplyTintsAndPalette` picks the palette or the layer specular, so the mean stands in for the sample either
  way (cross-review finding 1; kept, pinned by fixture `blend_palette_fold`). A mean that cannot be decoded
  warns and leaves F0 unscaled.
- The CPU side of the wear tints (how `wear_tint` / `wear_gloss` are packed for `LayerAttribs_ApplyWearTints`)
  and the DataCore glossiness field being a UNorm8 fraction.

### 4. Energy split

`TiledShading.cfx`: `Albedo *= saturate(1 − GetLuminance(Reflectance))`. Every lit slab master (surface /
fallback, layered, HardSurface, mesh decal, translucent) now ends in `Graph.conserve(albedo, f0)` with Rec. 709 luminance weights (the coefficients are not in
the shader text; inferred). The `Metallic` scalars and the `layer_snapshot.metallic` read are gone: SC authors
albedo and F0 directly. `r.Shading.EnergyPreservation=0` in `DefaultEngine.ini`, because Substrate's own
preservation would darken the diffuse a second time (energy *conservation*, the multi-scatter term, stays on).

### Tests

`SCBridge.Materials.FamilyMapping` (fixture `synthetic.materials.json`, palette fixture `palettes.json`):
`hard_primary` (separate wear layer, palette secondary finish, sampled spec maps are not folded),
`hard_wear_tinted` (tinted wear, two folds), `hard_wear_no_blend_map` / `hard_wear_no_tints` (wear off +
warning), `blend4` (`TintMode` 1 and 2), a palette without `finish`, `blend1` (fold and repeat),
`blend_palette_fold` (palette gloss without `Shininess`; mean × palette-selected F0), the
`ApplyTextureMeans` unit block; deduplicated texture requests 33 → 42. `SCBridge.Materials.TextureDecode`:
an `sRGB`-tagged PNG in a Linear slot, a `gAMA` 1.0 PNG in a Color slot, Normal always linear, untagged →
role fallback, `MeanColor`. Rust: palette glossiness fraction,
`generic_texture_png_records_its_color_encoding`, `srgb_comes_from_the_cry_image_flag_or_the_dxgi_format`.

Not tested: the `WEAR_*` custom nodes and the graph output (GPU captures only, as before); PNG chunk traversal
on malformed files beyond the length guard; a texture requested both sampled and averaged.

```bash
cargo test -q --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d --lib palette   # 13 passed
cargo test -q --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d --lib \
  generic_texture_png_records_its_color_encoding                                              # 1 passed
cargo test -q --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-dds --lib srgb_comes  # 1 passed
cargo test -q -p scviewer-core                                                                # 26 passed
ue/scripts/build.sh SCViewerEditor                                             # Result: Succeeded
UnrealEditor-Cmd … -run=pythonscript -script=ue/scripts/create_materials.py …  # WP-U2 MATERIALS_OK
UnrealEditor-Cmd … -script=ue/scripts/test_importer.py -nullrhi …              # 17/17 passed=1, WP-U1 UNIT_TESTS_OK
#   MASTER_PARAMETERS present=473 layered_family=8 dropped_other_families=44
UnrealEditor-Cmd … -script=ue/scripts/test_player.py -SCPlayerTests \
  -SCPlayerPackage="$PWD/out/fps/player-p4-male.tar" -nullrhi …                # 5/5, WP-P2 HEADLESS_OK
SC_SETTLE=20 ue/scripts/capture_viewer.sh wear4-gladius 'VIEWER_LOAD result' -SCDefaults -SCLoad=AEGS_Gladius
ue/scripts/capture_route.sh wear4-scorpius ue/scripts/routes/scorpius-smoke.txt -SCDefaults -SCLoad=RSI_Scorpius -SCCamera=first
ue/scripts/capture_route.sh wear4-cutlass ue/scripts/routes/cutlass-armed-walkthrough.txt -SCDefaults \
  -SCLoad=DRAK_Cutlass_Black -SCCamera=first \
  -SCPlayerLoadout=Data/Scripts/Loadouts/Player/DefaultLoadouts/PVE_medium_behr_rifle_gmni_rifle.xml
```

The test-first run for finding 2 failed as expected before the fix (`Expected 'no Shininess: default
smoothness x layer gloss x palette gloss' to be 0.200000, but it was 0.500000`).

### GPU result (run `wear4`, after the cross-review fixes)

All three loads `VIEWER_LOAD result=Succeeded`. Cutlass `SCRIPT_DONE steps=149`, 29 shots, each within 7.4 cm
of the `p2r3-cutlass` run (the "before"); Scorpius `SCRIPT_DONE steps=18`. Before | after, half size:

- `wp-u2/wear-gladius-orbit-before-after.jpg`: the hull reads **satin instead of glossy**; mean absolute
  difference over the ship region 3.9 / 255, 12 % of its pixels change by more than 8. (Sheet built from the
  pre-fix run `wear3`; `wear4` differs from it by 0.44 / 255, edges only.)
- `wp-u2/wear-gladius-closeup-nowear-specfold-wear.jpg`: three build states (no wear → palette finish and
  spec fold → wear). **Most of the Gladius change is the palette finish and the energy split, not wear**: the
  hull authors `WearBlendBase` 0.18 and its vertex wear only brings a few trim panels above the blend
  threshold. The in-game references show more dirt and scuffing than this; that is the dirt layer and
  per-instance wear, which are not drawn.
- `wp-u2/wear-cutlass-cockpit-before-after.jpg` (shot 22): wear is **strong** on the Cutlass interior paints
  (`WearBlendBase` 0.60–0.62). The cockpit wall turns from neutral grey to a mottled cream: that is the data,
  not a colour error — the material blends to a separate wear layer whose diffuse is cream (texture mean,
  sRGB-encoded, about 0.80 / 0.77 / 0.72 against the primary's neutral 0.78), and the bulkhead mesh's vertex
  wear alpha (p10 0.31, median 0.75) is below the threshold `WearBlendBase / 0.95` ≈ 0.66 on a sizeable
  minority of its vertices.
- `wp-u2/wear-cutlass-cargo-before-after.jpg` (shot 17): floor and wall panels gain wear break-up; the bay is
  **still blown out** by the interior lights (light-intensity defect, WP-U3b), as are the bulkhead (20) and
  third-person cockpit (23) shots.
- `wp-u2/wear-scorpius-hull-before-after.jpg` (shot 03): paint less mirror-like; the emissive chevrons still
  glow after the emission change (`wear3` → `wear4`: 0.80 / 255, edges only).

Frame time, cockpit view (single runs; a `cargo test` ran alongside `wear4` and disturbed its CPU times, so
the pre-fix `wear3` run is the cleaner one):

```text
wear3 body-a       frame_ms[mean=7.35 p50=6.03 p95=12.41] gpu_ms[mean=5.73 p50=5.78 p95=6.13]
wear3 capsule-only frame_ms[mean=5.45 p50=5.51 p95=9.55]  gpu_ms[mean=5.15 p50=5.28 p95=5.42]
wear3 body-b       frame_ms[mean=7.33 p50=5.90 p95=12.43] gpu_ms[mean=5.64 p50=5.69 p95=5.98]
wear4 body-a       frame_ms[mean=7.05 p50=6.21 p95=12.45] gpu_ms[mean=5.87 p50=5.93 p95=6.26]
wear4 capsule-only frame_ms[mean=11.32 p50=8.32 p95=27.39] gpu_ms[mean=5.44 p50=5.43 p95=6.13]
wear4 body-b       frame_ms[mean=6.40 p50=6.19 p95=8.07]  gpu_ms[mean=5.88 p50=5.94 p95=6.19]
```

Against round 3 (6.17 / 5.54 / 6.08 GPU) the wear graph costs nothing measurable; not A/B'd in one session.

Known and not fixed here: the Scorpius orbit view frames the ship from outside the hangar (focus distance
6349 cm against a 4000 cm half-span) and shows a black viewport; pre-existing framing defect of the default
orbit, unrelated to materials.

TODO(WP-U2): complement-hue wear (`Layer_ApplyComplementWearAndHAL`, needs the game's `HueLUT`), dirt layer,
damage map, `WearMasksSecondaryLayer`, per-instance wear; numeric tests of the `WEAR_*` nodes.

### Cross-review (Codex, on the uncommitted diff)

| Finding | Verified | Disposition |
| --- | --- | --- |
| 1. The spec-map mean also scales a palette-selected F0 | yes; consistent with the sampling shader's order (`ReadSpecular` before `ApplyTintsAndPalette`) | **kept**, documented as part of the inferred fold, pinned by `blend_palette_fold` |
| 2. Palette gloss and `GlossMult` ignored when a layer has neither `Shininess` nor a gloss map | yes (`paletteLib` multiplies unconditionally) | **fixed**, test first |
| 3. HardSurface emission used the unworn primary colour and a step gate | yes (`GetMaterialAttribs`) | **fixed**: wear-blended albedo × AO, alpha ramp; masters regenerated, captures re-taken |
| 4. `--skip-existing-assets` keeps untagged PNGs from an older export | yes | **documented limitation** (role fallback = previous behaviour) |
| Primer / remap constants unverified | source located afterwards: `layerLibConstants.cfi` | documented above |
| No graph-output tests; malformed-PNG and sampled-and-averaged cases untested | yes | **open**, stated under Tests |

## Glass: the game's `GlassShared` rule (2026-11-10)

`M_SC_Glass` and `FSCMaterialBuilder` `Glass` now follow `glassLib.cfib` `GlassShared` instead of the earlier
"palette colour as transmittance, `Opacity` as coverage" reading. Rule, sources, port, cross-review and
GPU evidence: `WP-U3b.md` §14. In short:

| Game (`GlassShared`) | UE |
| --- | --- |
| `glassUV = baseUV × GlassTiling`, `v ×= GlassVStretch`; normal, smoothness, height and tint maps read there | `M_SC_Glass` scalar parameters `GlassTiling`, `GlassVStretch` (no generic `Tiling`) |
| base layer: albedo 0, F0 `MatSpecColor.rrr` | Substrate simple-volume slab, albedo 0, F0 = `SpecularColor` through `SubstrateThinFilm` |
| `transmissive = MatDifColor.rgb`, moved to the palette's glass colour by `GlassTintStrength` under `%USE_TINT_PALETTE` | `GlassColor` vector resolved by the builder (instance palette of `WP-U3b.md` §14.2) |
| × vertex colour (`%PER_VERTEX_TINT`), else height × `saturate((blue − 0.25) × 3)` (`%PER_VERTEX_CURVATURE`); × tint map; height map, `clip(height − 0.01)` | switches `UseVertexTint` / `UseVertexHeight` / `UseTintMap` / `UseHeightMap`, Custom nodes; coverage only from the clip |
| `density = (1 / max(transmissive, 0.01) − 1) × 0.1`, `exp(−density × path)` | Custom node → `SubstrateTransmittanceToMFP`, blend mode `TranslucentColoredTransmittance` |

The nine constants and four function hashes are in `StarEngineRender.json` (validator 60/60). The synthetic
fixture `synthetic.materials.json` has 35 submaterials; the glass ones are `glass_ext` (defaults, no
warnings), `glass_thickness` (every feature, authored tiling), `glass_vertex_tint` (token precedence) and
`glass_bare` (no `Specular`, missing map slots, `%THIN_FILM` without a thickness → fixture default and three
import warnings).

Not mapped: see `WP-U3b.md` §14.7 (dirt, wear smoothness, the thin-film LUT and `ThinFilmStrength`,
refraction offset / curvature / glare, light amplification, `EmissiveOpacity`, cracks / frost / condensation /
glass decals).

TODO(WP-U2): the 0.04 reflectance for a glass material without `Specular`, the gloss cap 251/255, the
Rec.709 weights in `conserve` and the host `Shininess` default are host values outside the fixture
(cross-review finding 5 in `WP-U3b.md` §14.6): derive them or register them as host fallbacks.

## Full-detail geometry by default (2026-11-10)

**Found.** Every package the viewer built — the app's own cache and the `scviewer` CLI's — was exported at
`lod_level 1`, `texture_mip 2` (`export_options` in `scene.json`; mesh files `*_LOD1.glb`, packages
`<Entity>_LOD1_TEX2`). `USCViewOptions` declares `Lod = 0` / `Mip = 0`, but its constructor seeds both
from the core (`FSCBridgeClient::DefaultOptions()`), and the core copied StarBreaker's
`ExportOptions::default()` — a coarser mesh LOD and a quarter-resolution texture mip chosen for Blender.
So the viewer showed the game's second mesh LOD at every distance, Nanite included.

**Fix.** `GlbOptions::default().lod` is 0 (`crates/scviewer-core/src/options.rs`); the CLI's `--lod` /
`--mip` defaults now read the core's instead of repeating literals, and the app, the FFI
(`sc_default_options`) and the CLI all follow it. A different options value is a different package key,
so existing cache entries are not reused; no cache-format bump. The options panel still offers LOD 0–3.

**Test first.** `options::tests::viewer_defaults_to_full_detail_geometry` failed before the change
(`left: 1, right: 0`) and passes after; `cargo test -p scviewer-core -p scviewer-cli -p scbridge-ffi`:
all green (27 + 6 + 2 + 2 + 1 + 1 passed). `scripts/build_ffi.sh` and `--check-header`: exit 0.

**Measured** (`target/debug/scviewer --no-cache decomposed <entity> out/dec-l0-<ship>`, triangles of every
GLB of the package):

| Package | LOD 1 (`out/dec-r7-*`) | LOD 0 (`out/dec-l0-*`) |
| --- | --- | --- |
| Gladius | 392 684 | 908 814 |
| Scorpius | 416 041 | 893 276 |
| Cutlass Black | 669 120 | 1 546 021 |

Clips are unchanged by the geometry LOD: `verify_clip_pairing.py` exit 0 on each `out/dec-l0-*` package.
Headless on `out/dec-l0-*`, each exit 0: `verify_wp_p1.py --datacore`; `test_rig.py` (`WP-U3 RIG_TESTS_OK`,
`RIG_VERIFY passed=1` with the same counts as at LOD 1 — `clips=64 bound=63 sequences=80 tracks=636
rigs=16`, `clips=75 bound=71 sequences=90 tracks=675 rigs=23`, `clips=55 bound=53 sequences=68 tracks=371
rigs=32`); `test_first_person.py` (`WP-P1 HEADLESS_OK`); `test_importer.py` (31 suites passed). Gladius
import (`IMPORT_OK seconds`): 24.8 s for the first import of the new meshes (engine DDC without their
Nanite data — the layer-2 processed cache is not persistent yet, so collision is derived on every import),
4.7 s for the next import in the same DDC, against 4.3 – 5.6 s at LOD 1 with a DDC already holding those
meshes; commandlet peak memory 4.3 – 4.7 GB against 3.5 – 3.6 GB.

**GPU (RTX 4090, Vulkan), the app's own load path.** `gladius-hull-seat.txt` twice, both re-exporting
(`export_cache_hit=0`):

```
ue/scripts/capture_route.sh lod1-gladius ue/scripts/routes/gladius-hull-seat.txt -SCDefaults -SCLoad=AEGS_Gladius -SCCamera=first -SCLod=1
ue/scripts/capture_route.sh lod0-gladius ue/scripts/routes/gladius-hull-seat.txt -SCDefaults -SCLoad=AEGS_Gladius -SCCamera=first
```

The status bar reads `473.4k triangles` / `total 9.81 s` at LOD 1 and `1.09M triangles` / `total 13.58 s`
at the new default (`Rig AEGS_Gladius_LOD0`). Evidence `wp-u2/lod0-gladius.jpg` (left LOD 1, right LOD 0;
top: on the hull looking forward, bottom: the cockpit eye point): at LOD 0 the access panel around the Aegis
logo has its outline and its small light (the in-game `top-hangar.webp` shows that panel outlined), and the
canopy frame detail and the dashboard's button icons appear. 0.40 % / 1.39 % of pixels differ by more than
40/255.

**Not done: texture resolution.** Textures stay at StarBreaker's mip 2 (a quarter of the game's resolution
in each dimension). Measured on the Gladius, the textures decode to 326 MiB at mip 2 and 4 223 MiB at mip
0 (181 of them 2048²), the Cutlass 637 MiB at mip 2 — so mip 0, and on the Cutlass even mip 1, exceeds the
importer's 2 GiB per-import budget (`FSCTextureLimits::BudgetBytes`) while textures are decoded to
uncompressed BGRA8. Full resolution needs the game's block-compressed data kept compressed through to
the GPU; the options panel still offers mip 0 for one-off use on small models.

Cross-review (Codex, on the uncommitted diff): `ue/scripts/ue_load_gladius.py` still forced LOD 1 when no
`-SCLod` was given (and a successful load saves the options) — fixed, it now overrides the core's defaults
only for the arguments given; the timing labels above were ambiguous about the cache state — fixed. Nothing
else found in default propagation (core, CLI, FFI incl. null options, UE reset), cache keys and package
names (both carry LOD and mip; no format bump), or tests. A user's saved options from before this change
keep their LOD until "Core defaults" is pressed.
