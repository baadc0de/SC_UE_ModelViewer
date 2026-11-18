# WP-E1 — Star Engine rules as engine features of the UE 5.9 source fork

Branch `wp/E1-engine-fork-features` (this repo) and `scviewer/5.9` (engine, `C:\Users\bojan\UnrealEngine-5.9-src`,
upstream `EpicGames/UnrealEngine` branch `5.9` at `32e0d4024` plus our commits; build record in
`docs/engine-source-build.md` on branch `claude/ue59-source-fork-0e4ddb`).

Owner request (2026-11-18): move the lighting, texturing, material, particle, exposure and tonemapping hacks and shader
patches into the local UE 5.9 source where they belong. A new setting where it is a new setting; a toggle or mode where
it is a different behaviour; other content keeps working on this engine, SC content sets the toggles to look exactly
the SC way. Also lift artificial in-code limits (e.g. the light-function atlas).

Inputs: the inventory of today's workarounds (49 items: shader patches, borrowed fields, ini cvars, the replacing
tonemapper material, Niagara limits; summarised per item below) and an engine-design review by Astra
(`out/e1/astra/design-reply.md`, not committed; its corrections are folded in here).

**Owner decision (2026-11-18): the forked UE 5.9 source build is the only build.** The Web / Wasm build, builds
against the launcher (binary) engine and the abandoned build against NVIDIA's RTX fork of UE 5.8 are obsolete; their
scripts, configuration and code paths are removed (E1-0). So the plugin uses the fork's API directly (no binary-engine
fallback), and the shader text patches of plan D14 (`engine_patches.py`) end once their features are engine commits.

## Rules for every engine change

- **UE's behaviour is the default.** Nothing changes for content that does not opt in. Per-object data (a light's
  falloff, a projector's shape) is a component property; a per-view look rule (display curve, metering) is an
  `FPostProcessSettings` field with its `bOverride_` bit and blending; a capacity is a cvar whose default and clamp are
  sane on their own.
- **Every consumer or an explicit fallback.** A light property reaches deferred, clustered / forward, MegaLights
  (sampling estimate and shading), Lumen direct lighting, volumetric fog, translucency volume, GPU scene, ray-traced
  lighting and the path tracer, or the path documents that it ignores it. Mobile gets an explicit fallback, never an
  uninitialised field.
- **Proxy descriptors too:** `FLightSceneProxyDesc` and the point / spot / rect descriptors (`BuildSceneProxyDesc`),
  not only the component constructors; setters mark render state dirty; serialization and editor clamps allow the
  game's value ranges (negative offsets).
- **Shared pack / unpack functions and named bit masks** (`LightDefinitions.h`); no new magic numbers.
- **Constants come from the game via the project** (`StarEngineRender.json` → importer / runtime). The engine carries
  mechanisms and UE-neutral defaults, never SC values.
- **Commits** on `scviewer/5.9`, one feature per commit, subject `SCViewer: <topic>`, body naming this WP, ending with
  the repo's attribution trailer. Not pushed (no fork exists yet).
- **Capabilities header** `Engine/Source/Runtime/Engine/Public/SCViewerEngineFeatures.h`: `SCVIEWER_ENGINE_FEATURES 1`
  plus one `SCVIEWER_ENGINE_<FEATURE> <version>` per feature below, as the fork's record of what it adds. The plugin
  uses the features directly and never writes borrowed fields.

## Features, in order

### E1-0 Obsolete builds removed
The Web / Wasm build (`package_web.ps1`, `SC_WITH_FFI=0` / `SCBridgeNoFFI.cpp`, the Wasm target settings, mobile-path
fallbacks such as the kept UE tonemapper), launcher-engine paths and defaults in scripts and docs, and every vestige of
the NVIDIA RTX-fork (UE 5.8) build. Kept: the Win64 showcase package and DLSS (now on the fork), cooked content that
the showcase uses; `ue/EnginePatches` goes once E1-2, E1-3 and E1-7 carry its patches as engine features.

#### Result (E1-0)
No engine commit (project-side only).
- **Removed:** `package_web.ps1`, `bake_hangar_lighting.py`, `SCBridgeNoFFI.cpp` and every `SC_WITH_FFI` definition and
  branch (the archive picker and overlay no longer test it; `SCBridgeLib` links the library on Linux and Win64 and stops
  the build elsewhere); the Wasm `-ffile-prefix-map` code in `SCViewer.Target.cs`; the Interchange platform list in the
  uproject (it existed to exclude Wasm); the mobile-feature-level branch that kept UE's tonemapper
  (`SCViewerExposure`); `-SCBakedLighting` (`SCViewerSubsystem`) and `USCCookLibrary::PrepareStaticLighting`, which
  existed only for the web build's Lightmass hangars; `-Platform Wasm` and `-BakedLighting` in `package_showcase.ps1`.
- **Fork by default:** `Start-SCViewer` (`SCRpc.psm1`), `package_showcase.ps1`, the `.sh` helpers, `.claude/launch.json`
  and the documented commands (`create_cargo_art.py`, `docs/cargo/art.md`, READMEs) default to
  `%USERPROFILE%\UnrealEngine-5.9-src` (`~/UnrealEngine-5.9-src`), overridable with `-Engine` / `UE_ENGINE_ROOT`; the
  PowerShell entry points fail early when no engine is there.
- **Docs:** plan D17 (D9 and D14 marked superseded), `AGENTS.md`, `README.md`, `ue/README.md` (showcase cooking now
  stands on its own), `docs/03-unreal-export-design.md`, WP-H1 / K1 / K4, the FFI README and the cargo JSON provenance.
  `ue/EnginePatches/README.md` says it is for the obsolete binary engine. No NvRTX / RTX-fork run configuration, ini
  section or script remained; historical records (`ue/verification`, WP-U0 / U2 / U3b, WP-L3 / P4 "not verified" notes)
  are left as they are.
- **Kept:** the cooked-ship and cooked-hangar paths (`cook_ships.py`, `cook_hangars.py`, loading without an archive),
  which the Win64 showcase uses; the `RHI_RAYTRACING` and non-editor Nanite guards (correct for any cooked game);
  the masters' static-lighting usage (`create_materials.py` and the saved assets; harmless, and dropping it means
  regenerating every master); the DLSS installer, whose plugin builds against the fork as a project plugin.
- **Verified:** project build on the fork (`out/e1/E1-0/build-editor.log`, Succeeded; the DLSS modules were up to date, built against the fork earlier today); 81/81
  tests (`SCBridge.StarEngine`, `.Lights`, `.Rig.Lights`, `.ViewerExposure`, `.Cargo`, `.Hangars`,
  `.Materials.MasterAssets`; log `E1-0-tests.log`); a game run through `Start-SCViewer` without `-Engine` started the
  fork's `UnrealEditor.exe`, loaded hangar_medfrnt_001 live from the archive (683 lights, 186 atlas light functions,
  `display_curve=1`), settled, took `out/e1/E1-0/shots/smoke-medfrnt.png` and quit with code 0, no `Error:` lines
  (`out/e1/E1-0/smoke.txt`, log `E1-0-smoke.log`). The showcase package itself was not rebuilt.

### E1-1 Light-function atlas: overflow fix and capacity
Today: `r.LightFunctionAtlas.Size` ≤ 16 (256 material tiles, `LIGHT_FUNCTION_ATLAS_MAX_LIGHT_FUNCTION_COUNT`), slot
resolution 128 px, the per-light atlas index a `uint8` (`FLightSceneProxy::LightFunctionAtlasLightIndex`, extra-data
bits 15..22), and the default `r.LightFunctionAtlas.MaxLightCount=-1` lets indices past 255 wrap onto other lights
(stripes). We set Size 8 and MaxLightCount 255; a hangar has 683 lights, 241 projectors, 15 gobo materials.
- Fix first: allocation is clamped to the representable light-record count before narrowing; the identity entry stays
  reserved; lights past capacity get no light function with a one-time warning (non-shipping) naming the count.
- Separate the two capacities: material tiles (`Size²`, `Size` ≤ 32) and light records (12-bit index, 4095). Widen the
  proxy field, getters / setters, counters, per-light records and every packed form; re-lay the extra-data bits with
  named masks.
- Gobo detail is the project's choice (`r.LightFunctionAtlas.SlotResolution` in `DefaultEngine.ini`).

#### Result (E1-1)
Engine `ad613210a` (with the capabilities header `SCViewerEngineFeatures.h`).
- **Layout.** `LightDefinitions.h` holds the light extra-data offsets and masks: the atlas index is 12 bits at 20..31,
  the five flags moved to 15..19, all 32 bits used. It also holds the two capacities: `LIGHT_FUNCTION_ATLAS_MAX_EDGE_SIZE`
  32 (1024 material tiles) and `LIGHT_FUNCTION_ATLAS_MAX_LIGHT_RECORD_COUNT` 4096 (4095 lights plus the reserved record
  0), plus Lumen's packed-word flag offsets. `LightData.ush` and Lumen unpack through these names.
- **Consumers.** Deferred, clustered, MegaLights, volumetric fog, translucency volume, Lumen (surface cache, HWRT, world
  cache) and mobile forward all read the index through the `LightData.ush` unpackers or a `uint32` uniform. Lumen's
  packed word was already wide enough (29 bits), and so was the mobile neutral buffer once sized to every index. GPU
  scene light data and the path tracer / ray-traced lighting carry no atlas index (the latter use their own
  light-function path), so nothing changes there.
- **Overflow.** The record budget is `min(MaxLightCount, 4096)`, and `-1` means 4096. Lights past it get index 0. The
  non-shipping log reports `LightFunctionAtlas: N light(s) with a light function, M sampling the atlas (light records:
  M of C), T of S material tile(s) ..., K with a material that is not atlas compatible` once per change per scene,
  with a warning when lights are skipped.
- **Project.** WP-L3's border node (`TextureCoordinate` + Custom) had taken `M_SC_LightFunction` out of the atlas
  (`bIsLightFunctionAtlasCompatible`), so the first fork run logged 186 lights with light functions and 0 sampling the
  atlas. `create_light_material.py` now sets "Compatible With Light Function Atlas", and the asset is regenerated.
  This affected every engine since WP-L3: MegaLights drew no gobos, and deferred lights used the per-light pass.
- **Runs** (source engine, `out/e1/E1-1/`, log `ue/SCViewer/Saved/Logs/E1-1-run2.log` and `E1-1-lrgtop.log`):

  | Hangar | lights with LF | sampling | material tiles | MaxLightCount 64 | MaxLightCount 255 |
  |---|---|---|---|---|---|
  | hangar_medfrnt_001 | 186 | 186 of 4095 | 14 of 64 | 63 sample, warning "123 light(s) get no light function" | n/a |
  | hangar_lrgtop_001 | 319 | 319 of 4095 | 13 of 64 | n/a | 254 sample, warning "65 light(s) ..." |

  lrgtop's indices reach 319 (the old engine wrapped every index past 255) and show no stripes
  (`shots/lrgtop-default-slot128.png`). Against the 255 cap, 1.2 to 1.6 % of pixels change by more than 8 levels,
  where the 65 farthest lights get their gobos back. Recapturing with identical settings changes 0.00 to 0.01 % of
  pixels by more than 8 levels.
- **SlotResolution.** The game's 15 hangar gobos are 512² BC textures (`Data/Textures/lights/**`). 256 px slots against
  128 px changed medfrnt captures by a mean of 0.5 to 0.6 levels, p99 2 to 3, and 0.00 to 0.07 % of pixels by more than
  8 levels, the same as recapture noise. The project keeps the default of 128.
- **UE default.** Size 4, SlotResolution 128 and MaxLightCount -1 are unchanged. Content under 255 atlas lights gets
  the same indices and records as before; only the bit positions of five flags moved, behind the shared unpackers.
- **Open.** The atlas border (black outside the projected square) is E1-3: the fork's atlas still wraps with `frac`
  where the binary engine was patched. This was not verified on mobile forward (`r.Mobile.LightFunctionAtlas`, default
  off): its neutral buffer is now 4096 records, 320 KiB per graph when that path is in use.

### E1-2 Local-light distance falloff models
Today (WP-L3): unconditional shader patches put the game's range window in `GetLocalLightAttenuation` /
`EstimateLocalLight`, and the attenuation-tweak offset rides in `SoftSourceRadius` (with `GetCapsule` patched to ignore
it as a soft radius).
- `ULocalLightComponent`: `RangeFade` { `Default` (UE `(1 − (d/R)⁴)²`), `SquaredLinearTail` (`saturate((1 − d/R)/F)²`) }
  with `RangeFadeFraction` F (0 < F ≤ 1); applies to every inverse-squared local light, rect included.
- `DistanceAttenuation` { `Default`, `BulbOffset` } with `AttenuationBulbRadius` b and `AttenuationDistanceOffset` c
  (cm, c may be negative, b + c > 0 enforced): intensity ∝ 1/(max(d, b) + c)², point and spot. `SourceRadius` keeps
  its UE meaning (specular / penumbra size); `SoftSourceRadius` is UE's again.
- One shared shader function used by every path; the light-grid estimate stays conservative (negative offsets); the
  path tracer and ray-traced lighting evaluate the same law without changing their sampling PDFs.
- Packing: a seventh float4 in the forward light record (+16 B per light); no rect-field reuse.

### E1-3 Spot projector shape
Today: the importer opens inner = outer = atan(√2 tan(FOV/2)) and rescales `LightFunctionScale`; the atlas shader is
patched to zero spot light functions outside UV [0, 1]; the non-atlas pass relies on a Custom node in
`M_SC_LightFunction`; the game's near-depth cut is not modelled; UE's cone limit is 88.9°.
- `USpotLightComponent::Shape` { `Cone`, `SquareFrustum` }: in `SquareFrustum` the outer angle is the frustum's
  half-FOV, the shading mask is the square (hard edge, no cone falloff), light-function UVs span the square, and CPU
  bounds, light-grid culling, shadow projection and ray-traced / path-traced evaluation use the enclosing cone.
- `ProjectorNearClip` (axial light-space depth, cm; 0 = off), applied last.
- `ULightComponent::LightFunctionAddress` { `Wrap` (UE), `Border` }: the projected domain outside the canonical square
  is black, in the atlas and in the legacy light-function pass.
- The importer then sets the FOV directly; the CPU mapping of the game's `projectorNearPlane` stays unverified and is
  not guessed.

### E1-4 Display curve in the engine tonemapper
Today: `M_SC_DisplayMapping`, a post-process material at `BL_REPLACING_TONEMAPPER`, ports the game's
`HuePreservingShoulder` (it compresses **max RGB** and desaturates by channel ratio; luminance only drives reference
white), bypassing UE's grading and local exposure; it drew black on the mobile path (Web keeps UE's tonemapper).
- `FPostProcessSettings`: `ToneCurve` { `Filmic` (default), `HuePreservingShoulder` } plus its parameters
  (max display nits, reference-white scale, toe scale), blended and overridable like the other film settings, included
  in the LUT cache key.
- Implemented in `CombineLUTs.usf` in place of the film curve and its ACES / gamut / blue-correction branch for that
  mode, in linear; output encoding stays UE's. Checked against an analytic port at 32³ and 64³ (neutral ramps, dark
  colours, saturated emissives, the shoulder); if the LUT error is visible, an analytic branch in the tonemap pass with
  a grading-only LUT. Mobile is audited separately.
- Fidelity baseline for SC content: neutral grading, local exposure off.

### E1-5 Exposure: LUT fix, dual meter, two-stage adaptation
Today: the game's chain is mapped onto UE's histogram exposure by a runtime volume; the compensation-curve LUT must be
re-domained by cvars before the curve exists (it rebakes only on a curve or sample-count change); the lens-attenuation
luminance helper is renderer-private and re-implemented; UE meters reflected luminance only (the game blends an
incident meter: `M = R + clamp(0.5 (I − R), −1.5, +1.0)`, `r_exposureMeterMode` 3); one adaptation stage (the game has
pupil 0.4 s plus pigment 5 s / 2 s).
- Small fix first: the curve LUT cache key includes domain, sample count and curve contents; an opt-in policy domains
  it on the curve's key range (UE's extrapolation semantics preserved); a public exposure-conversion API.
- `AutoExposureMeter` { `Reflected` (default), `Incident`, `ClampedBlend` } with blend amount and down / up clamps,
  orthogonal to the existing method; the blend happens on the two reduced meters in calibrated log units, before
  compensation and adaptation. First incident signal: UE's white-diffuse (`IgnoreMaterials`) path as a second
  reduction, documented as a proxy for the game's `exposureincident.cfib` sampling.
- Optional second adaptation stage ported from the game's `PupilAdaptation` / `PigmentAdaptation` coupling
  (separate view states, reset on cuts).

### E1-6 Niagara lightweight emitters
Today: `NiagaraStateless::MaxGpuSpawnInfos` = 8 (overflow drops live loops; our FX builder stretches loop durations);
spawn-rate bindings cannot evaluate expressions; expressions have add / multiply only; sprite size ignores component
scale for local-space emitters; the FX commandlet needs `bTreatAsEngineModule` to reach `Niagara/Internal/Stateless`.
- Raise the spawn-info capacity with a measured cost (not to be confused with `NIAGARA_MAX_GPU_SPAWN_INFOS`), keeping
  the builder's loop guard derived from the engine constant.
- Export a narrow stateless authoring API so the commandlet is an ordinary module.
- Then, separately: spawn-rate expression evaluation; clamp / min / max / lerp expressions; an explicit sprite-size
  scale policy (UE default unchanged).

### E1-7 SSDO as an engine feature
Today: patches in `MegaLightsShading.usf` and `DiffuseIndirectComposite.usf`, constants in a generated header,
switched by `r.GeneralPurposeTweak`; off in the current look (D15).
- Post-process settings for radius / strength / amounts, cvars for force-enable, quality and debug views; shader
  parameters instead of the generated header; one RDG pass producing occlusion and bent direction consumed by
  MegaLights and the indirect composite. Defined interplay with Lumen short-range AO (no double darkening). Never
  injected into world-space caches or the path tracer.

### Later / investigations
- Lumen World Cache with the adaptive Substrate G-buffer (`LumenWorldCacheTracePaths.usf ReadMaterialData`; D13).
- Native `ProjectorTexture` on spot lights through the rect-light texture atlas (phase two of E1-3).
- BC4 / BC5 SNORM pixel formats (today re-biased in place).
- Deferred-decal world→local transform.
- Clip-volume light linking (the game's 63 room stencils) — not more lighting channels.
- Staying project-side: game constants and unit conversions, fixture validation, import mappings, look presets, the
  colour-temperature conversion question, thin-film bypass.

## Status

| Feature | Engine commit | Project side | Verified |
|---|---|---|---|
| E1-0 | none (project only) | Web / Wasm build, `SC_WITH_FFI`, baked hangar lighting, Wasm target code removed; scripts and docs default to the fork; plan D17 | project build, 81/81 tests, medfrnt game run (Result (E1-0)) |
| E1-1 | `ad613210a` | `DefaultEngine.ini` (MaxLightCount=255 dropped), `SCLightVerification` (`atlas_light_records`), `M_SC_LightFunction` atlas-compatible, uproject `UE_5.9_src` | engine + project build, 11/11 tests, medfrnt + lrgtop runs (Result (E1-1)) |
| E1-2 | — | — | — |
| E1-3 | — | — | — |
| E1-4 | — | — | — |
| E1-5 | — | — | — |
| E1-6 | — | — | — |
| E1-7 | — | — | — |
