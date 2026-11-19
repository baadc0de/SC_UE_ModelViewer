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

#### Result (E1-2)
Engine `362370d99`.
- **Contract.** `ULocalLightComponent::RangeFade` / `RangeFadeFraction` (clamped to [0.001, 1]) and `DistanceAttenuation` /
  `AttenuationBulbRadius` / `AttenuationDistanceOffset` (cm), set through `SetRangeFade` / `SetDistanceAttenuation` (render
  state dirty), carried by `FLocalLightSceneProxyDesc` (built and serialized; cooked FastGeo data must be rebuilt) and the
  proxy. A bulb offset law outside its domain (`IsValidBulbOffsetAttenuation`: finite, b >= 0, b + c > 0) logs a warning
  and renders UE's law; the editor greys the law out on rect lights. `SoftSourceRadius` is UE's again.
- **Packing.** One float4 everywhere (`LightDefinitions.h`: x range fade scale 1/F or 0, y model, z b, w c; all-zero and a
  default `FVector4f` (0,0,0,1) both mean UE's falloff), packed by `FLightRenderParameters::GetPackedDistanceFalloff`
  and unpacked by `LocalLightFalloff.ush`: `FLightShaderParameters` (C++ / HLSL / `Light_*` root parameters), the
  forward record's 7th float4 (`LightGridCommon.ush`, `WorldLightGridInjection.usf`), `FLightSceneData` (96 -> 112 B),
  Lumen's packed light (144 -> 160 B), `FPathTracingLight` (128 -> 144 B). Full float precision (no half for c).
- **Exact law, not a mask.** UE's inverse square comes from the capsule integrators (`IntegrateLight`, `CreateAreaLight`:
  `1 / (d^2 + DistBiasSqr)`, or the line integral for capsules). In BulbOffset mode they return `1 / (max(d, b) + c)^2`
  (point law from the centre) in place of it, so the product is exactly the law, flat inside the bulb (WP-L3's
  `d^2 / (...)^2` mask also skipped the bulb clamp at tweak 1). The horizon wrap keeps the geometric sphere size.
- **Consumers changed:** `GetLocalLightAttenuation` (deferred, clustered, forward / translucency, MegaLights shading,
  Lumen surface cache / HWRT / world cache, volumetric fog, heterogeneous volumes, clouds, ray-traced lighting, Substrate
  paths: all via `FDeferredLightData` + `GetCapsule`); the translucency volume's legacy `1/(d^2+1)`; the mobile SH
  translucency term; `EstimateLocalLight` (light grid cell estimate via a new `ViewSpaceDistanceFalloffData` buffer,
  world grid from the forward record, per-pixel `PassesLightGridEnergyCull` in MegaLights / Lumen / RT): both terms
  decrease monotonically, so the cell's nearest point keeps culling conservative with negative c; path tracer
  `ComputeAttenuationFalloff` (range fade times `d^2 / (max(d,b)+c)^2`, PDFs unchanged); reference `IntegrateBxDF`.
- **Documented as unchanged:** simple / Niagara lights and directional / sky lights (packed as UE's falloff); mobile
  (explicit fallback: `UnpackLightDistanceFalloff` returns UE's falloff under `SHADING_PATH_MOBILE`, and
  `MobileOverrideDeferredLightParameters` resets it); GPU Lightmass (static bakes, its lights are zero = UE's);
  VSM / shadow / distance-field / capsule-shadow code (geometry only); rect lights drawn as spots (`RectAsSpotLight`,
  exponent falloff).
- **Project.** The importer sets every local light's range fade to SquaredLinearTail with F =
  `FSCStarEngine::ForcedFadeFraction` (0.2) and authored point / spot lights' law to BulbOffset with b = bulb (cm) and
  c = `AttenuationOffset(bulb, tweak)` (cm) when valid (no bulb: UE's law); it no longer writes `SoftSourceRadius`. The
  probe evaluates the lights' own properties. `sc.Lights.FalloffModels 0|1` swaps every light to UE's falloff and back
  (A/B). Tests first: with the new expectations and the old importer `SCBridge.Lights.AuthoredCryEngineModel` failed
  (`Expected 'tweak offset is carried (cm)' to be 30 ... was 0`, `'soft source radius untouched' ... was 30`,
  `'forced fade: the fixture's fraction' to be 0.2 ... was 1`); after the importer change 11/11
  (`SCBridge.StarEngine`, `.Lights`, `.Rig.Lights`; log `E1-2-tests.log`).
- **Run** (fork, medfrnt, WP-L3's poses, one session; `out/e1/E1-2/run1.txt`, `regions.txt`, `shots/`, log
  `E1-2-run1.log`; WP-L3's region boxes and method):

  | View, region | WP-L3 before | WP-L3 + offset | fork, UE default | fork, models | fork, models again |
  |---|---|---|---|---|---|
  | top, pad centre | 0.0839 | 0.1459 | 0.0842 | 0.1455 | 0.1457 |
  | top, side walkways L / R | 0.0818 / 0.0839 | 0.0438 / 0.0761 | 0.0824 / 0.0840 | 0.0446 / 0.0760 | 0.0447 / 0.0761 |
  | top, centre / left walkway | 1.03 | 3.33 | 1.02 | 3.26 | 3.26 |
  | ref, near / mid / far pad | 0.1426 / 0.1089 / 0.1006 | 0.2201 / 0.1721 / 0.0548 | 0.1422 / 0.1088 / 0.1013 | 0.2189 / 0.1711 / 0.0557 | 0.2187 / 0.1711 / 0.0559 |
  | ref, upper walls / elevator | 0.1104 / 0.1050 | 0.0676 / 0.0795 | 0.1109 / 0.1054 | 0.0684 / 0.0801 | 0.0684 / 0.0801 |
  | long, right floor / right wall | 0.4579 / 0.1309 | 0.2302 / 0.0702 | 0.4568 / 0.1304 | 0.2290 / 0.0701 | 0.2289 / 0.0701 |

  The engine feature reproduces the patched law (fade + offset) within 1-2 %, and with the properties at Default the
  fork renders UE's falloff again (matches WP-L3's unpatched "before" within 1 %); toggling back reproduces the first
  capture to 0.0002. The fork's images match WP-L3's "+ offset" stage, not its part 3 ("+ projector frustum") numbers
  (top walkways 0.210 / 0.190): those were taken while `M_SC_LightFunction` was outside the atlas, so MegaLights drew
  the opened frustum without its gobos (E1-1); with the gobos back the square textures cut the light to about the
  earlier footprint. Not separately verified.
- **Open.** Mobile does not render the models (explicit fallback). A capsule light (SourceLength > 0) in BulbOffset mode
  falls off from its centre. The atlas border (E1-3) still wraps gobos outside the square on the fork. The path tracer
  and Lumen paths were not exercised in a run (compile and code review only).

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

#### Result (E1-3)
Engine `20605cb76` (with the E1-1 review fixes), after `dd77a0317` (E1-2 review fixes, below).
- **Contract.** `USpotLightComponent::Shape` { `Cone`, `SquareFrustum` } and `ProjectorNearClip` (cm, >= 0), set through
  `SetShape` / `SetProjectorNearClip`; `ULightComponent::LightFunctionAddress` { `Wrap`, `Border` } (`SetLightFunctionAddress`);
  all three carried by the proxy descriptors (`BuildSceneProxyDesc`, versioned serialization) and the proxies. In
  `SquareFrustum` the outer angle is the half-FOV h, clamped like UE's outer cone (at most 88.9° + 0.001 rad, independent
  of the inner angle; 90° is an infinite frustum); the lit region is max(|y|, |z|) <= x tan(h) in light space, hard edged,
  without cone falloff. y and z are the light-space axes the light function's UVs use (`.zyx` of `WorldToLight`), so with a
  uniform `LightFunctionScale` (UE's default 1024³) the gobo spans the square, u, v = 0 / 1 at its edges; the importer's
  orientation (`MakeFromXZ(direction, up)`) is unchanged.
- **Bounds and culling: the enclosing cone** atan(√2 tan h). The spot proxy's outer cone (and its cos / sin / tan) is that
  cone in this shape, so every cone consumer covers the square's corners unchanged: CPU bounding sphere and `AffectsBounds`
  (component and proxy), light grid culling (view and world grid), the deferred stencil cone, the whole-scene shadow
  projection (`InvTanOuterCone`, so the spot shadow frustum and VSM contain the corners), Lumen's light culling, ray-traced
  shadow rays, the path tracer's light bounds. Lumens convert over the square's solid angle 4 asin(sin² h).
- **Shading: the square.** `GetSpotLightShapeAttenuation` (`LocalLightFalloff.ush`) replaces the cone term in
  `GetLocalLightAttenuation`, so deferred, clustered / forward, MegaLights (sampling and shading), Lumen (surface cache,
  HWRT, world cache), volumetric fog, the translucency volume, heterogeneous volumes, clouds, Substrate forward and
  ray-traced lighting evaluate it; the path tracer calls the same function in its hit, sample and estimate paths (its
  volume sampler keeps the enclosing cone's importance, since the square's corners can be lit where the cone term at the
  closest point is not); the reflection light-shape pass (`SceneVisibility.cpp`) tests the square. The projector near
  clip zeroes the light where the axial light-space depth is below it, in either shape; it multiplies the attenuation, so
  no light function or light function fade restores it.
- **Packing (no new lanes).** The packed spot angles become (cos of the enclosing cone, tan h) in this shape; a consumer
  that still reads them as a cone sees a falloff that is non-zero exactly inside the enclosing cone, i.e. a conservative
  cone test. The shape and the near clip go into E1-2's falloff float4, whose `.y` is now a model word stored bit for bit
  (`LightDefinitions.h`): bits 0..3 distance attenuation model, 4..7 spot shape, 16..31 near clip as a half. All-zero and
  a default `FVector4f` still mean UE's behaviour, and every packed form E1-2 extended (forward record, light uniforms,
  GPU scene, Lumen, path tracer) carries them without growing. The axes come from the light's `Tangent` (light-space z),
  which every packed form already carries.
- **Light function border.** `Border` is tested on the unwrapped projected UVs (outside [0, 1] or behind the light gives
  0) before `frac` in `LightFunctionAtlasCommon.usf` (light record parameter bit 24) and before the material in the per-
  light passes (`LightFunctionCommon.ush`; `LightFunctionParameters.z` = 2 from `FLightFunctionSharedParameters`, the
  ray tracing, path tracing, Lumen and mobile setups). Inside the square the atlas sample is UE's: the UV is saturated
  and the slot's half-texel inset keeps bilinear taps in the tile, so the edge texels end the gobo; with `Border` the
  UVs are no longer wrapped (u = 1 is not folded onto u = 0). The light function's projection angle is now a proxy
  virtual (`GetLightFunctionTanOuterAngle`: the half-FOV for a square frustum) used by the atlas and all per-light passes.
- **E1-1 review fixes** (`out/e1/followups.md`). HIGH: the gobo no longer repeats outside its square (the border above).
  LOW: `LightFunctionAtlas.cpp` checks material compatibility before the record budget, counts incompatible lights in
  every non-shipping build, and words the warnings as "not sampling the atlas", naming the fallback (deferred lighting's
  per-light light function pass; the atlas-only paths draw no light function).
- **Mobile** keeps the shape and the near clip (and the light's axes) while it still drops E1-2's falloff. Per-vertex
  forward translucency ignores spot shapes as UE ignores spot cones there. **Not covered:** GPU / CPU Lightmass bakes
  (static lights; GPU Lightmass would read tan h as a cone falloff), the editor's spot light visualizer (draws a cone).
- **Project.** `FSCLightImporter::MakeProjector`: a projector whose gobo binds gets `SquareFrustum`, outer = inner = the
  game's half-FOV, `Border`; `FProjectorCone` and the lateral `LightFunctionScale` rescale are gone (the scale stays UE's
  uniform default). `ProjectorNearClip` stays 0: the export carries no field that can be shown to map to the shader's
  `projDepthCutoff` (WP-L3 part 3). `create_light_material.py` lost its border node (luminance of the gobo only, still atlas
  compatible); `M_SC_LightFunction` regenerated with the fork's editor. The probe evaluates the square;
  `sc.Lights.ProjectorShape 0|1|2` switches every imported projector to UE's cone + wrap at the same angles (0), back to
  the import (1), or to the E1-2 importer's emulation (2: corner cone, lateral light function scale, wrap). The look's floor
  wash samples the bounding cone (`GetHalfConeAngle`).
- **Tests** (tests first): with the new expectations and the old importer, `SCBridge.Rig.Lights` failed on exactly the
  four projector checks (`'projector: square frustum'`, `'outer angle is the game's half-FOV' to be 30 ... was 39.23`,
  `'gobo black outside its square (border)'`, `'uniform light function scale'`; `out/e1/E1-3/tests-first.log`) while the
  engine-contract tests passed; after the importer change 12/12 (`SCBridge.StarEngine`, `.Lights`, `.Rig.Lights`, log
  `E1-3-tests.log`). New: `SCBridge.Lights.ProjectorShape` (UE defaults; enclosing cone on component and proxy; light
  function tangent = tan h; packed spot angles and model word incl. the near clip half; corner inside / beyond-corner
  outside `AffectsBounds`; the 88.9° clamp; the solid angle) replaces `ProjectorCone`; `SCBridge.Lights.ProxyDescArchive`.
- **Runs** (fork, medfrnt, WP-L3's poses and region boxes, `out/e1/E1-3/run1.txt` MegaLights, `run2.txt` deferred via
  `r.MegaLights.Allowed 0`; `regions-run1.txt`, `regions-run2.txt`, `signed.txt`, `shots/`; logs `E1-3-run1.log`,
  `E1-3-run2.log`). The atlas log reads `186 light(s) with a light function, 186 sampling the atlas ...; not sampling the
  atlas: 0 with a material that is not atlas compatible, 0 over the light records, 0 over the material tiles`; each toggle
  logged `LIGHT_PROJECTOR_SHAPE ... lights=241`. "E1-2 emulation" is `sc.Lights.ProjectorShape 2` (it reproduces E1-2's
  own captures within 0.4 %):

  | View, region | WP-L3 part 3 | E1-2 (fork) | square + border, ML | cone + wrap, ML | E1-2 emulation, ML | square + border, deferred | cone + wrap, deferred |
  |---|---|---|---|---|---|---|---|
  | top, pad centre | n/a | 0.1455 | 0.1455 | 0.1456 | 0.1457 | 0.1474 | 0.1473 |
  | top, side walkways L / R | 0.210 / 0.190 | 0.0446 / 0.0760 | 0.0446 / 0.0760 | 0.0449 / 0.0763 | 0.0447 / 0.0761 | 0.0456 / 0.0758 | 0.0459 / 0.0759 |
  | top, centre / left walkway | 0.62 | 3.26 | 3.26 | 3.24 | 3.26 | 3.23 | 3.21 |
  | ref, near / mid / far pad | n/a / n/a / 0.114 | 0.2189 / 0.1711 / 0.0557 | 0.2191 / 0.1714 / 0.0554 | 0.2188 / 0.1715 / 0.0555 | 0.2187 / 0.1712 / 0.0559 | 0.2186 / 0.1722 / 0.0565 | 0.2185 / 0.1722 / 0.0565 |
  | ref, upper walls / elevator | 0.097 / 0.133 | 0.0684 / 0.0801 | 0.0679 / 0.0796 | 0.0680 / 0.0796 | 0.0685 / 0.0802 | 0.0691 / 0.0824 | 0.0691 / 0.0823 |
  | long, right floor / right wall | n/a | 0.2290 / 0.0701 | 0.2295 / 0.0701 | 0.2298 / 0.0701 | 0.2289 / 0.0701 | 0.2291 / 0.0707 | 0.2295 / 0.0707 |

  The square changes region means by under 1 %: of the hangar's four gobos (`out/hangar-light/gobos.png`) three lie inside
  the square's inscribed circle and the fourth only softly reaches its corners, so square and inscribed cone differ on
  ≤ 0.03 % of pixels (signed, > 8 levels). What the border removes is the E1-2 state's wrapped gobo between the square
  and the corner cone (the E1-1 review's HIGH item): against the square the E1-2 emulation is brighter on 0.011 % (top),
  0.18 % (ref) and 0.54 % (long) of pixels and darker on at most 0.01 %, the same in MegaLights and deferred
  (0.011 / 0.19 / 0.54 %); recapturing the square changes ≤ 0.001 %. The light it removes is the elevator canopy's top
  and the upper wall bulges' edges (`shots/border_long_ml.png`, `border_long_def.png`: E1-2 emulation, square, their
  difference ×8; `crop_ref_header_*.png`, `crop_long_wall_*.png`). The deferred column is ~1 % brighter than MegaLights
  (renderer difference, the same for every shape); the first deferred capture after the switch was still adapting and is
  not in the table.
- **WP-L3 part 3 re-judged.** Its "walkways 5x brighter" (0.210 / 0.190) does not reproduce: with the gobos in the atlas
  and the border, the corrected footprint keeps the walkways at 0.045 / 0.076 and the pad centre at 3.3× the left
  walkway, as E1-2 found. Astra (`out/e1/E1-3/astra/`, gpt-6.2-astra, reference `out/hangar-light/ref/reference.jpg`
  against the reference-pose captures): square and cone are "a visual tie on both surfaces", 5/10 each; what still differs
  is light range / overlap (the reference's A1 pool is separated from the foreground by a darker band through the hatch,
  ours fills it; the right walkway is too evenly lit between the amber fixtures) and softer highlight shoulders
  (post-processing), not the projector shape. So the game's square frustum stays (it is the game's rule and costs nothing
  visible); the remaining pool mismatch is a range / placement question (the 2020 light set predates 4.10.2's).
- **UE default unchanged.** Cone + Wrap packs UE's spot angles, a zero model word and `LightFunctionParameters.z` 1, and
  the atlas border bit is 0 (`ProjectorShape` test); the cone term is UE's expression; the deferred / MegaLights region
  means with the falloff models on match E1-2's run to 0.1 %.
- **Open.** Mobile, the path tracer, Lumen HWRT and ray-traced lighting were not run (compile and review only). The near
  clip is implemented and packed but not set by the importer (no export field proven to map to `projDepthCutoff`).

#### E1-2 review fixes
Engine `dd77a0317` (`SCViewer: falloff models review fixes`, `out/e1/E1-2/terra-review.md`):
1. `EstimateLocalLight` (light grid cell and per-pixel energy culling) evaluates the bulb offset law and its range window at
   the true distance; the 0.01 cm² clamp stays for UE's inverse square and exponent branches only.
2. Per-vertex forward translucency: `FSimpleDeferredLightData` carries the unpacked models (`ConvertToSimpleLight`), and
   `GetSimpleDynamicLighting` uses the shared law and window; zero-initialised simple lights evaluate UE's
   1 / (d² + 1) · (1 − (d/R)⁴)² as before.
3. Path tracer capsules: the ratio d² / (max(d, b) + c)² assumes a sphere emitter's geometric 1 / d², which an extended capsule
   does not produce near the light. Normalising the capsule's sampled contribution would need its irradiance integral per
   sample, out of proportion for a light shape SC content does not use (`SourceLength` 0), so capsule point / spot lights
   (`SourceLength` > 0) keep UE's distance law in the path tracer (explicit fallback; range window still applied).
4. FastGeo: a custom version `FSCViewerEngineObjectVersion` (`SCViewerEngineObjectVersion.h`) guards the E1-2 fields in
   `FLocalLightSceneProxyDesc::Serialize` (and E1-3's in the light and spot descriptors): an archive written without them
   loads UE's defaults and reads every later field at the right offset (`SCBridge.Lights.ProxyDescArchive` writes UE's,
   E1-2's and the current layout and reads all three back). Archives written by `362370d99` alone (unversioned, never
   shipped) are not readable.
`SCVIEWER_ENGINE_LIGHT_FALLOFF` 2.

#### E1-3 review fixes
Engine `4470238ea` (`SCViewer: spot shape review fixes`, `out/e1/E1-3/terra-review.md`; `SCVIEWER_ENGINE_SPOT_SHAPE` 2):
1. **Border after the fades.** The out-of-square predicate is kept and applied after the light function's distance and shadow
   fades (which lerp towards the disabled brightness, 0.5 by default): in the atlas sampler and, through
   `GetLightFunctionBorderMask` (`LightFunctionCommon.ush`), in the deferred light function pass, Lumen direct lighting, mobile
   deferred, volumetric fog and ray tracing / the path tracer's miss shader. The translucency volume injection does not fade,
   so its early black suffices.
2. **Atlas capacity fallback.** `GatherAndSortLights` treats a light as atlas compatible only when it also holds an atlas record
   (index ≠ 0; the atlas update is a prerequisite of the sort in deferred and runs before it in mobile), so a compatible light
   skipped for record or tile capacity takes the unbatched path with the per-light light function pass, as the atlas warning
   says, instead of the clustered / atlas path without its gobo. The atlas-only paths (MegaLights, Lumen, fog) still draw it
   without a light function.
3. **Near clip range.** The honest option, not more bits: the model word has no free 32-bit lane (E1-2's float4 is full), so
   `LIGHT_PROJECTOR_NEAR_CLIP_MAX` (65504 cm, the half's range; step 2⁻¹¹ of the depth, 0.5 cm at 10 m) is named in
   `LightDefinitions.h`, the property has `ClampMax` 65504, and `SetProjectorNearClip` and `BuildSceneProxyDesc` clamp to it
   with a warning (`LogSpotLight`). Tests first: with the old engine `SCBridge.Lights.ProjectorShape` failed on the two new
   checks (`'near clip: the setter clamps to the packed range' to be 65504 ... was 100000`, same for the proxy descriptor;
   `out/e1/E1-3/review-fixes-testsfirst.log`).
4. **Per-vertex forward translucency** applies the near clip: `FSimpleDeferredLightData` carries the light direction
   (`ConvertToSimpleLight`; zero for real simple lights, whose near clip is 0) and `GetSimpleDynamicLighting` zeroes the light in
   front of the plane. Spot cones and shapes stay ignored there, as UE ignores cones.
5. **Reflection-capture light shapes** (`SceneVisibility.cpp`) apply the near clip for cone spot lights too.

Verified: engine + project build, 17/17 tests (`SCBridge.ViewerExposure`, `.StarEngine`, `.Lights`, `.Rig.Lights`,
`.Materials.MasterAssets`; `out/e1/E1-3/review-fixes-tests.log`); a medfrnt run on the fixed engine (MegaLights, then
`r.MegaLights.Allowed 0`; `out/e1/E1-3/review-fixes-run.txt`, `review-fixes-regions.txt`, `shots-review/`; this run already has
E1-4's engine tone curve). The atlas logs 186 lights sampling it and no warning. The square changes ≤ 0.008 % of pixels by more
than 8 levels against the same view before the fixes (MegaLights), the E1-2 emulation is brighter than the square on 0.019 /
0.195 / 0.544 % of pixels (top / ref / long, MegaLights) and 0.018 / 0.225 / 0.547 % (deferred), against E1-3's 0.011 / 0.18 /
0.54 % and 0.011 / 0.19 / 0.54 %, and the deferred square's region means are E1-3's within 0.3 % (the 32³ display-curve LUT's
bias, Result (E1-4)). As expected: the hangar's projectors are within their fade distances, under the atlas capacity and use no
near clip, so the fixes change nothing here.

### E1-4 Display curve in the engine tonemapper
Today: `M_SC_DisplayMapping`, a post-process material at `BL_REPLACING_TONEMAPPER`, ports the game's
`HuePreservingShoulder` (it compresses **max RGB** and desaturates by channel ratio; luminance only drives reference
white), bypassing UE's grading and local exposure; it drew black on the mobile path (the Web build that met it is gone, E1-0).
- `FPostProcessSettings`: `ToneCurve` { `Filmic` (default), `HuePreservingShoulder` } plus its parameters
  (max display nits, reference-white scale, toe scale), blended and overridable like the other film settings, included
  in the LUT cache key.
- Implemented in `CombineLUTs.usf` in place of the film curve and its ACES / gamut / blue-correction branch for that
  mode, in linear; output encoding stays UE's. Checked against an analytic port at 32³ and 64³ (neutral ramps, dark
  colours, saturated emissives, the shoulder); if the LUT error is visible, an analytic branch in the tonemap pass with
  a grading-only LUT. Mobile is audited separately.
- Fidelity baseline for SC content: neutral grading, local exposure off.

#### Result (E1-4)
Engine `3079e8bc3` (`SCVIEWER_ENGINE_TONE_CURVE` 1).
- **Contract.** UE 5.9 already has a tone curve switch, `FPostProcessSettings::TonemappingMethod` (Film > Method:
  Project Default, Filmic, Standard ACES), so the curve is a fourth method, `ETonemappingMethod::HuePreservingShoulder`,
  rather than a second, overlapping `ToneCurve` enum. It is also a valid project default (`r.DefaultFeature.TonemappingMethod`
  3) and `r.LUT.TonemappingMethod 3`. Parameters `HuePreservingShoulderMaxDisplayNits` (default 80, the sRGB reference
  display white), `HuePreservingShoulderRefWhiteScale` (1) and `HuePreservingShoulderToeScale` (0), each with an override
  bit, interpolated when volumes blend (`SceneView.cpp`, `PostProcessUtils.cpp`); the method switches (`SET_PP`) like
  UE's. Tagged properties, so no custom version.
- **Shader** (`PostProcessCombineLUTs.usf`). `HuePreservingShoulder` is the game's function (`Tonemapping.cfib`) statement
  for statement, checked against the p4k's shader source (`out/e1/E1-4/game-tonemapping.txt`): the largest channel is
  compressed (linear to MaxDisplayLum − 0.5, exponential shoulder towards MaxDisplayLum = nits / 80 / adjusted reference
  white, power toe below 0.15), the channel ratios are desaturated towards white with hue-dependent exponents, and luminance
  only drives the reference white scale. It runs in linear sRGB primaries, is saturated (the game's SDR `DisplayMapping`
  saturates before `ToSRGB`), and takes the place of the film curve and blue correction; Expand Gamut is skipped. White
  balance and colour correction before it, the legacy LDR LUTs, `r.Color.*`, tint / fade, display gamma, output gamut and
  device encoding after it are UE's (Filmic's code path); `ToneCurveAmount` blends from the linear colour as for Filmic.
  The LUT cache key holds the method and the three parameters (taken from the view, like the method, even with colour
  grading off); LUTMax is 1. HDR output devices remap the SDR result through ACES like Filmic ("Automatic HDR", inner LUT).
- **Mobile.** The mobile renderer uses the same LUT pass (`AddCombineLUTPass` from `MobileShadingRenderer` and the mobile
  post-process chain), so the method works there; mobile LDR (gamma only) has no tone curve for any method. Not changed:
  exposure-fusion local exposure and the HDR visualisation keep their internal film curve (as for Standard ACES).
- **LUT against the analytic curve** (`out/e1/E1-4/lutcheck.txt`, `lutcheck128.txt`; the bake and the tonemap lookup
  emulated in float64: log shaper, texel-centre trilinear, 10-bit storage; error in 8-bit output levels, max over channels;
  the game's parameters 80 / 1 / 1):

  | Set | 32³ p99 / max | 64³ p99 / max | 128³ p99 / max |
  |---|---|---|---|
  | neutral ramp 2⁻¹²…2⁵ | 6.5 / 6.8 | 3.2 / 3.4 | 1.6 / 1.7 |
  | dark colours (max channel 10⁻⁴…0.05) | 0.5 / 0.9 | 0.25 / 0.37 | 0.25 / 0.25 |
  | shoulder (max channel 0.3…4) | 6.1 / 6.8 | 3.0 / 3.4 | 1.5 / 1.7 |
  | saturated emissives (0.1…40) | 2.2 / 4.6 | 0.9 / 2.6 | 0.3 / 1.1 |
  | general (10⁻³…40, log) | 4.7 / 6.8 | 2.3 / 3.4 | 1.1 / 1.7 |

  The error falls as 1/N, not 1/N²: max-RGB has a crease along the neutral axis, which trilinear cells straddle, so the
  worst case is near-neutral highlights on the shoulder (display 0.85…0.95). Above the LUT's log domain (linear 43.7) each
  channel is clamped (p99 10, max 16 levels at any size, UE's limit for every method).
- **Runs** (fork, medfrnt, WP-L3's ref / top / long poses plus `up` at the ceiling grilles, a lit doorway and a window;
  `out/e1/E1-4/run{A,B,C}.txt`, `regions.txt`, `diff-bands.txt`, `diff-crosssession.txt`, `shots/`; logs `E1-4-a.log`,
  `E1-4-b.log`, `E1-4-c.log`). A: the material (before its removal) and `sc.Look.Display ue`; B and C: the engine curve
  (`VIEWER_GAME_EXPOSURE ... tone_curve=HuePreservingShoulder max_nits=80.0 ref_white=1.00 toe=1.00`), `r.LUT.Size`
  32 / 64 / 128, `sc.Look.Display ue`. Region means (sRGB-decoded luminance):

  | View, region | material | engine 32³ | engine 64³ | engine 128³ | UE Filmic, before | UE Filmic, fork |
  |---|---|---|---|---|---|---|
  | top, pad centre / walkway L / R | 0.1455 / 0.0446 / 0.0760 | 0.1460 / 0.0448 / 0.0766 | 0.1457 / 0.0448 / 0.0764 | 0.1456 / 0.0449 / 0.0765 | 0.1312 / 0.0235 / 0.0455 | 0.1312 / 0.0235 / 0.0455 |
  | ref, near pad / elevator / walkway R | 0.2192 / 0.0796 / 0.2512 | 0.2200 / 0.0798 / 0.2518 | 0.2193 / 0.0796 / 0.2513 | 0.2192 / 0.0796 / 0.2512 | 0.1780 / 0.0535 / 0.1968 | 0.1779 / 0.0535 / 0.1968 |
  | long, right floor / right wall | 0.2294 / 0.0701 | 0.2299 / 0.0702 | 0.2293 / 0.0700 | 0.2293 / 0.0700 | 0.1742 / 0.0446 | 0.1742 / 0.0446 |
  | up, grilles / doorway / window | 0.0556 / 0.2483 / 0.1884 | 0.0557 / 0.2485 / 0.1886 | 0.0556 / 0.2482 / 0.1883 | 0.0556 / 0.2483 / 0.1882 | 0.0371 / 0.2319 / 0.1397 | 0.0371 / 0.2319 / 0.1397 |

  Per pixel (max channel, 8-bit levels): engine 32³ against the material mean 0.8…1.3, p99 4…6, > 8 levels on 0.12…0.27 %
  of pixels; recapturing the material in its session mean 0.4…0.5, p99 2, 0.00 %; the engine curve across sessions (B
  against C) mean 0.35…0.67, p99 2, ≤ 0.03 %. The LUT shows as a bias in bright pixels (sRGB 200…244: −0.4…−0.5 levels
  at 32³, −0.2 at 64³, ≈ 0 at 128³) and in region means (+0.3…0.6 % at 32³, ≤ 0.1 % at 64³ / 128³). The rest of the
  difference does not shrink with the LUT size: it lies on geometric edges (`shots/diff8_*_32.png`, `diff16_up_128.png`),
  is neither a pixel offset (best alignment 0, 0) nor sharpening or blur (edge regression slope 0.00); its cause between
  the material path and the tonemap pass was not identified and is not pursued (the material path is gone).
- **Astra** (`out/e1/E1-4/astra/`, gpt-6.2-astra, material against engine 32³, `up` and `ref` views, ×8 differences, crops
  of the doorway, window and lit walls): "No visible tonal difference at normal scale"; the 32³ LUT "is sufficient for a
  faithful look in these views"; the edge differences "look like sampling or anti-aliasing variation". So no analytic
  branch in the tonemap pass, and the project keeps UE's `r.LUT.Size` 32 (64 halves the LUT error if it is ever wanted).
- **UE default unchanged.** `TonemappingMethod` defaults to Project Default and the project default to Filmic; the Filmic
  and ACES permutations compile the same code (the only shared edit is an `#if` around Expand Gamut that they take). In the
  runs, UE's Filmic through `sc.Look.Display ue` matches the pre-fork engine's capture region for region to 0.0001, per pixel
  mean 0.51…0.66, p99 2, ≤ 0.04 % over 8 levels, i.e. cross-session noise.
- **Project.** `FSCDisplayCurve` (`SCViewerExposure.h`): `From(FSCStarEngine)` derives the method from
  `r_HDRTonemappingCurve` (1 only; 2, `HuePreservingShoulder2023`, has no engine curve and keeps UE's tonemapper with an
  error), the peak from `MaxDisplayNitsLDR`, the toe from `r_HDRTonemappingCurveToe`; reference white stays the curve's
  neutral 1 (the SDR branch's argument is a renderer constant the fixture does not carry, as for the material). `Apply`
  sets the curve and the fidelity baseline on the viewer volume (neutral grading over any map grading, `ToneCurveAmount` 1,
  no UE lens vignette (the material had none; the game's vignetting map is not modelled), local exposure off), or clears
  every one of those overrides (`sc.Look.Display ue`: UE's tonemapper as the project and map set it, Filmic with UE's
  vignette). `Install` tags the volume (`SCViewerExposure::VolumeTag`) and no longer adds a blendable; the log line names
  the curve. Removed: `M_SC_DisplayMapping.uasset`, `ue/scripts/create_display_mapping_material.py`, its `KEEP` entry in
  `create_materials.py`; the port's hash guard moved to `SCBridge.ViewerExposure.DisplayCurve` (fails when the fixture's
  `HuePreservingShoulder` / `GetLuminance` / `ToSRGB` hashes differ from the ones the engine port was written from);
  `StarEngineRender.notes.md` and `ue/README.md` updated (WP-U3b §13.5 and WP-L3 are historical records and keep their
  wording). Tests: new `SCBridge.ViewerExposure.DisplayCurve` (parameter derivation, unsupported curve, game / UE settings,
  hash guard), `.Install` checks the tag, the method, the peak and no blendable; 17/17 with `SCBridge.ViewerExposure`,
  `.StarEngine`, `.Lights`, `.Rig.Lights`, `.Materials.MasterAssets` (log `E1-4-tests.log`).
- **Open.** Mobile and HDR output were not run (code review only). UE grading in this mode (non-neutral white balance /
  colour wheels) is implemented but not exercised.

#### E1-4 review fixes
Engine `c1e0610d4` (`SCViewer: tone curve and light-function review fixes`, `out/e1/E1-4/terra-review.md`; it also covers the
E1-3 review fixes): `SCVIEWER_ENGINE_LIGHT_FUNCTION_ATLAS` 2, `SCVIEWER_ENGINE_SPOT_SHAPE` 3, `SCVIEWER_ENGINE_TONE_CURVE` 2.
1. **HIGH, simple / per-vertex lights.** `GetSimpleDynamicLighting` (`DeferredLightingCommon.ush`): the E1-3 review's near clip
   test sat between the inverse-squared branch and its `else`, so every inverse-squared light there (per-vertex forward
   translucency, clustered and mobile deferred simple lights, i.e. Niagara light renderers) got the exponent falloff
   `RadialAttenuation(.., 0)` in place of its distance law, UE content included. The pair is restored; the near clip applies after
   it. No test reaches the shader (no CPU path evaluates it); checked by reading and compiling.
2. / 3. **Inherited LUT and local exposure curves.** UE's blending ignores an overridden None asset (`IF_PP`), so a map volume's
   grading LUT and local exposure curves survived the viewer's neutral baseline. New `FPostProcessSettings::bOverriddenNoneAssetsClear`
   (default off = UE): an overridden None Color Grading LUT blends towards the identity LUT (`FFinalPostProcessSettings::LerpTo`
   takes the neutral entry) by weight × intensity, and an overridden None local exposure curve, exposure compensation curve or
   metering mask clears the inherited one (view blending and `FPostProcessUtils`). `FSCDisplayCurve::Apply` sets it in game display.
   Test `SCBridge.ViewerExposure.BaselineBlend` blends a real `FSceneView`: a lower-priority volume with a LUT and both curves, the
   viewer volume over it: no map LUT, identity weight 1, no curves; at viewer weight 0.5 the LUT is half map, half identity; with
   `sc.Look.Display ue` (flag off) and UE's own None overrides the map's LUT and curves stay (UE behaviour unchanged).
4. **Tone curve domain.** The peak is raised on the render thread, after blending, to
   `FPostProcessSettings::GetHuePreservingShoulderMinMaxDisplayNits(RefWhite)` = 52 · max(1, RefWhite) (peak ≥ 0.5 + 0.15 in display
   units: knee at or above the toe range, desaturation denominator positive); UI ClampMin 52. Below it the curve jumped at the knee,
   crushed the darks (negative knee) and, under a peak of 0.2, took a negative pow base (NaN; 8 nits did). Test
   `SCBridge.ViewerExposure.ToneCurveDomain` (a C++ port of the engine function): NaN at 8 nits; finite and continuous at the
   domain edge for reference whites 0.5 / 1 / 2 and toes 0 / 1 / 10 over neutral and saturated ramps; the game's 80 nits inside.
5. **Atlas overflow under MegaLights.** A stochastic light whose atlas-compatible light function got no record (record or tile
   capacity) now leaves MegaLights / LumenPT for the unbatched deferred path (`GatherAndSortLights`), whose per-light light function
   pass draws its gobo; it used to stay stochastic and sample record 0 (white). Shadow setup has already treated it as a MegaLights
   light, so on that path it has no shadow maps (unshadowed unless MegaLights uses VSM); the atlas warnings say so. Run: see
   Result (E1-5).

#### E1-1..E1-4 cumulative review fixes
Engine `9e2a37dd8` (`SCViewer: light function, falloff and tone curve review fixes`; Astra `out/e1/E1-1to4/astra-code-review.md`,
12 items): `SCVIEWER_ENGINE_LIGHT_FUNCTION_ATLAS` 3, `SCVIEWER_ENGINE_LIGHT_FALLOFF` 3, `SCVIEWER_ENGINE_SPOT_SHAPE` 4,
`SCVIEWER_ENGINE_TONE_CURVE` 3.
1. **Atlas overflow under LumenPT.** The E1-4 review's fallback (a stochastic light whose atlas-compatible light function got no record
   moves to the unbatched deferred path) asks the active stochastic renderer: MegaLights' atlas flag or, under LumenPT direct lighting
   (which turns MegaLights off), the new `ELightFunctionAtlasSystem::LumenPT`. LumenPT's candidate loop takes only the cells' stochastic
   range (`NumMegaLights` / `MegaLightsDataStartIndex` and the stochastic directional lights), as MegaLights does, so the rerouted light
   is lit once, by deferred lighting; with LumenPT every other light is stochastic, so the loop is UE's otherwise. LumenPT's indirect
   bounces (`SampleDirectLighting`, world cache) keep every light and sample record 0 (white) for a rerouted one (documented, not changed).
2. **Border when faded by screen size.** `CheckForLightFunction` and the per-light pass's early-out keep a Border light function with a
   perspective projection (`FLightFunctionSharedParameters::KeepsLightFunctionBorderWhenFaded`): the pass fades it to its disabled
   brightness and still masks it black outside the square (before, the light lost its light function and lit its whole cone). The atlas
   has no screen-size fade (UE's), so there such a light keeps its gobo.
3. **One light function parameter builder.** `FLightFunctionSharedParameters::GetLightFunctionSharedParameters` (with the shared
   `GetLightFunctionPerspectiveProjection`, also the atlas's tangent) feeds the path tracer, ray-traced lighting, Lumen and mobile deferred:
   a rect light with a light function cone angle gets the perspective projection and its Border everywhere. *Corrected by review fixes 3
   (Terra T1):* this changed UE content using the default Wrap address in those four passes, which UE projects planar, so it was not an
   acceptable exception to "UE's behaviour is the default"; they keep UE's planar projection for Wrap and take the perspective builder only
   for a Border light function (E1-1..E1-4 review fixes 3 below).
4. **Light grid estimate.** The squared-linear-tail window is evaluated at the true distance; only the inverse square's denominator keeps
   the 0.01 cm^2 clamp (a light under 0.1 cm radius had a zero estimate in its own cell). UE's window keeps UE's clamped evaluation.
5. **Clear intent per asset.** `FPostProcessSettings::OverriddenNoneAssetsClear` is a bitmask (`EPostProcessOverriddenNoneAsset`; default 0 =
   UE) replacing the E1-4 review's bool; `FPostProcessUtils::OverridePostProcessSettings` / `BlendPostProcessSettings` carry each asset's
   bit with the asset (none for an unoverridden default that blending flips to). `FSCDisplayCurve::Apply` sets every asset.
6. **Monotonic tone curve.** Toe Scale >= `GetHuePreservingShoulderMinToeScale()` (-2) and Reference White Scale >=
   `GetHuePreservingShoulderMinRefWhiteScale()` (0.25), UI ClampMin and render-thread clamp. Below about -3.5 the toe lifts the darkest
   tones above brighter ones (-9: neutral 0.02 -> 0.64, 0.10 -> 0.28); with a negative toe a reference white below 1 adds a falling factor,
   and below about 0.2 the shoulder reverses for any toe; -2 / 0.25 keep a margin (scan over neutral and saturated ramps, peaks to 10000 nits).
7. **Atlas slot origins as integer texels** (16 bits each; `FLightFunctionAtlasGlobalParameters::Slot_InvAtlasResolution`): the sampler
   rebuilds the texel centre. Over every Size 2..32 and SlotResolution 32..1024 within 16K, the old normalised packing missed the first
   texel centre of 1007 of 2770 tile columns towards the left neighbour, worst 0.117 texels (Size 31 / 512, column 5: 2560.406 for 2560.5);
   now worst 7.7e-4 texels, fp32 rounding (`out/e1/E1-1to4/fix/atlas-decode.txt`).
8. **Volumetric soft fading of a square frustum** fades towards the square's faces (lateral distance max(|y|, |z|) against x tan h), not
   the enclosing cone (`GetSpotLightVolumetricSoftFading`: volumetric fog and MegaLights volume).
9. **Reflection-capture light shapes** evaluate the light's range window (squared linear tail) and its bulb offset law relative to the
   emitter sphere's own 1 / d^2.
10. **Atlas size clamp** before the multiply (Size 32 x SlotResolution 2^27 wrapped to 0 in uint32 and passed the check).
11. **Probe** (`SCLightVerification.cpp`): the near clip in both shapes, lumens over the square's solid angle (a cone over UE's clamped half
   angle), UE's regularised 1 / (d^2 + 1 cm^2).
12. **Per-world A/B restore.** `sc.Lights.ProjectorShape 1` / `FalloffModels 1` forget only that world's saved lights (and expired ones).
   New `sc.Lights.ProjectorShape 3`: the corner cone with the Border address (the light function alone cuts the square), for the checks.
- **Tests first** (`out/e1/E1-1to4/fix/testsfirst.log`, old engine and plugin): `SCBridge.Lights.LightGridEstimate` (the port of
  `LightCulling.ush`: tiny tail light estimate 0 at and inside its radius, below shading), `.ProbeFollowsRenderer` (1e6 for 5e5 lux at
  1 cm; 2500 lux in front of a cone's near clip; 543.3 for 477.3 lux for 1000 lm over a 45-degree square), `.ComparisonRestorePerWorld`
  (the second world stayed on the comparison), `SCBridge.ViewerExposure.ComposedClear` (a UE-style None metering mask composed with a LUT
  clear, both orders and a blend, over a map with a mask and a LUT: the bool cleared the mask in all three), `.ToneCurveOrder` (ClampMin
  -9 / 0.01). After the fixes all pass: `Automation RunTests SCBridge` 135 of 139 (`out/e1/E1-1to4/fix/tests.log`), every
  `.ViewerExposure` / `.StarEngine` / `.Lights` / `.Rig.Lights` / `.Materials.MasterAssets` / `.Fx` test (30); the four failures are
  outside these features and need inputs this run does not give (`Package.FolderTarImport` and `Player.KitRigAndWeapons` need
  `-SCPackage` / `-SCPlayerPackage`; `Materials.FamilyMapping` rejects a scene v3 package, `FirstPerson.SkeletalCollision` fails without a
  message).
- **Runs** (fork, medfrnt; `out/e1/E1-1to4/fix/`: `run.txt`, `run2.txt`, `rvfix1.txt`, `regions*.txt`, `border-fade.txt`, `shots*/`; logs
  `E1-1to4-fix.log`, `E1-1to4-fix2.log`, `E1-5-rvfix1.log`). The atlas logs `186 light(s) with a light function, 186 sampling the atlas`
  at Size 8 / 128 and at Size 31 / 512 (961 tiles, 15872 px), no warning, no error.
  - *Unchanged:* E1-5's review script (`out/e1/E1-5/review/scripts/run.ps1`) on this engine gives the steady exposures of `rv1` to 0.0001
    stops at all six poses for both chains (game ref -9.0972 / -9.0971, floor -10.0551 / -10.0552; UE ref -8.4843 / -8.4844) and the WP-L3
    region means of its shots to 0.0001 (game ref padNear 0.1532 / 0.1532, walkR 0.1786 / 0.1787, top padC 0.1014 / 0.1014, long floorR
    0.1853 / 0.1854; `regions-rv.txt`). E1-3's and E1-4's tables predate E1-5's exposure chain, so their absolute means are not comparable;
    E1-5's run had matched them. In the case run (look defaults on load: the look's -0.1 exposure bias, so 0.095 stops below the
    explicit chain), square + border and cone + wrap still differ by under 1 % per region under MegaLights (ref padNear 0.1432 / 0.1421,
    long floorR 0.1728 / 0.1731) and MegaLights / deferred by the renderer difference E1-3 saw.
  - *Border when faded (item 2):* deferred with the per-light pass (`r.MegaLights.Allowed 0`, `r.Deferred.UsesLightFunctionAtlas 0`),
    every light function faded by screen size (`r.Shadow.TexelsPerPixel 0.0001`). The corner cone with Border (mode 3) against the square:
    brighter on 0.024 % (ref) / 0.014 % (long) of pixels by > 8 levels, frame mean 0.0956 / 0.0957; what the old early-out drew (no light
    function: mode 2 faded, the light filling its corner cone): brighter on 65.6 % / 52.3 % of pixels, frame mean 0.1244 at 0.50 stops less
    exposure. Under MegaLights the faded and unfaded mode 3 differ on 0.002 % / 0.004 % (the atlas kept the border; before, the faded light
    had no atlas light function at all, `m3_ml_nolf`: brighter on 41 % / 44 %). Unfaded, mode 3 against the square: 0.26 % / 0.024 % per-light
    pass, 0.010 % MegaLights.
  - *Non-power-of-two atlas (item 7):* Size 31 / SlotResolution 512 against Size 8 / 128: 0.012 % / 0.004 % / 0.005 % / 0.003 % of pixels
    (ref / top / long / up) by > 8 levels, as two captures at one size (0.006 / 0.003 / 0.004 / 0.002 %): no seam. The hangar's 14 gobos do
    not include a black tile next to a white one, so the bleed itself is shown by the decode table above, not by a capture.
- **UE default unchanged.** Wrap lights, Default falloff windows and `OverriddenNoneAssetsClear` 0 take UE's code paths; LumenPT's loop is
  UE's when every light is stochastic; the per-light pass's early-out and UE's histogram path are unchanged for non-Border lights; the
  exception is the tone curve's new clamps (Hue Preserving Shoulder only). Item 3 had changed Wrap rect lights with a light function cone
  angle in four passes; review fixes 3 restores UE's projection there.
- **Not covered.** LumenPT, the path tracer, ray-traced lighting, mobile and reflection captures were compiled, not run; no rect light
  with a light function cone angle exists in SC content.

#### E1-1..E1-4 review fixes 3
Engine `1840a16a1` (`SCViewer: light function and post-process review fixes`; Terra `out/e1/fixes2/terra-review.md` T1, Astra
`out/e1/fixes2/astra-code-review.md` 13; triage `out/e1/fixes3-brief.md`): `SCVIEWER_ENGINE_SPOT_SHAPE` 5, `SCVIEWER_ENGINE_TONE_CURVE` 4.
- **T1, UE's projection restored.** The path tracer, ray-traced lighting, Lumen direct lighting and mobile deferred build their
  `LightFunctionParameters` with `FLightFunctionSharedParameters::GetLightFunctionSharedParametersPlanarRect`: a rect light gets UE's planar
  parameters there (1, fade, 0, 0; UE's `bIsSpotLight` false) unless it asks for a Border light function with a cone angle, which takes the
  shared perspective builder and its border. Every other light, and deferred / the atlas / the translucency volume / volumetric fog (UE already
  projected cone-angled rect lights in perspective there), are unchanged. Checked against the pre-fork code (`32e0d4024`: `SetupLightFunctionParameters`,
  `CreateLightFunctionParameters` (RT / PT), `RenderLocalLight` (mobile) all pass `bIsSpotLight ? 1 : 0` with `bIsSpotLight` = spot only); the
  earlier note calling the Wrap change an intentional exception is corrected above.
- **A13, saved clears migrate.** `FPostProcessSettings::bOverriddenNoneAssetsClear_DEPRECATED` (editor data, `UPROPERTY()`, never saved) receives
  a saved `bOverriddenNoneAssetsClear` by name (UHT strips the suffix), and `PostSerialize` turns a true into `OverriddenNoneAssetsClear |= All`.
  Test `SCBridge.ViewerExposure.LegacyClearMigration` (tests first: on the old engine the bool was an unknown property and the mask stayed 0).
- **A16, probe** (`SCLightVerification.cpp`, repo): the cone term uses the proxy's unfloored `1 / (cos inner - cos outer)` in float, and lumens
  convert through the component's own `ComputeLightBrightness` (no cosine clamp). `SCBridge.Lights.ProbeFollowsRenderer` (a 0.5° cone, 1000 lm):
  tests first: 230644 lux where the renderer gives 4177427 (18x low: 0.145 cone factor and the clamped lumens conversion); after the fix within 0.1 %.

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

#### Result (E1-5)
Engine `cfe9d5032` (LUT key, key-range domain, conversion API), `cfc636921` (meter), `03d211bcb` (two-stage adaptation), after
`c1e0610d4` (E1-4 review fixes, above); `SCVIEWER_ENGINE_EXPOSURE` 3. Game data read from the installed 4.10.0 build (change
13165946) with `scripts/verify_star_engine_render.py` (MSVC `dumpbin` standing in for `objdump`): the game's `OpticsExposure` /
`exposureincident` / `Histogram` sources (`out/e1/E1-5/game-exposure-shaders.txt`, not committed) and the cvars; the fixture
gained `r_exposureIncidentSmoothingTime` (0.5), `meterMaxCompensationPerBlend` (3), `meterMinCompensationPerBlend` (−2),
`incidentMiddleGray` (0.18) and the hashes of `CalcUpdatedFrameState`, `ApplyEyeAdaptation`, `ExposureAdaptationCS`
(`STAR_ENGINE_RENDER OK: 87/87`).
- **LUT fix (a).** The compensation curve LUT's cache key is the curve, its baked domain (MinX, MaxX, sample count) and a hash of
  its contents; UE keyed it on the curve pointer and sample count only. Opt-in `r.EyeAdaptation.ExposureCompensationCurveLUT.Domain 1`
  bakes over the curve's key range at ends with Constant / None extrapolation (UE's [MinX, MaxX] kept at other ends), with
  `...MaxStep` the largest spacing; the shader's scale / bias comes from the same pure function (`UE::AutoExposure::
  GetCompensationCurveLUTDomain`), so bake and lookup cannot disagree. Public API `AutoExposureConversion.h` (`UE::AutoExposure`:
  luminance max, middle grey, extended range, curve input, transition distance, LUT domain); the renderer's
  `LuminanceMaxFromLensAttenuation` calls it. Tests first, as a run (baseline, old engine): setting MinX / MaxX after the curve
  existed moved the exposure by +4.21 stops (stale LUT addressed with the new domain; `baseB.txt`). Fixed engine: the same and two
  other domains give the same exposure to 0.0003 stops (`runC.txt` lutA–D).
- **Meter (b).** `AutoExposureMeter` { Reflected (default), Incident, ClampedBlend } with `AutoExposureIncidentBlend`, `...ClampDown`,
  `...ClampUp`, `...SmoothingHalfLife`, orthogonal to the method. Incident signal: UE's ignore-materials white-diffuse luminance
  (the existing illuminance passes, run for views that need it; unlit pixels written as no sample) reduced by a second histogram to
  its arithmetic mean and rescaled to UE's middle grey (the game scales by its `incidentMiddleGray` 0.18, checked equal by
  `SCBridge.ViewerExposure.Meter`). A proxy for `exposureincident.cfib` (128 scene positions × 64 directions of virtual Lambert
  receivers), documented in `StarEngineRender.notes.md`. ClampedBlend: log2 M = log2 R + clamp(b (log2 I − log2 R), −down, +up) on
  the two reduced meters, the incident one smoothed first, before compensation and adaptation. Explicit fallback to Reflected:
  Basic / Manual, mobile / forward, path tracer, debug views, fixed exposure, frames without the illuminance texture. The eye
  adaptation buffer gained a third float4 (log2 R, log2 I, slow, fast), read back as `GetLastAutoExposureMeterState`.
- **Two-stage adaptation (c).** `AutoExposureAdaptation` { Default, TwoStage } with slow half-lives brighter / darker, fast
  half-life and range: T = log2(clamped M) − log2(compensation) (the game's `logLumTarget` = metered − EV compensation); slow → T,
  fast → clamp(T − slow, ±range), scale = 0.18 / 2^(slow + fast); state in the buffer's z / w, reset on cuts, forced targets and a
  missing / foreign history. Histogram and Basic methods.
- **Cost** (`ProfileGPU`, 1920×1080, medfrnt ref view; `gpu-profile.txt`): Reflected runs one histogram (0.016 ms) and no illuminance
  pass; ClampedBlend adds SetupExposureIlluminance 0.007–0.009 ms, CalculateExposureIlluminance 0.033–0.034 ms and the incident
  histogram 0.015–0.017 ms, ≈ 0.057 ms per frame in all. `HistogramEyeAdaptation` stays 0.004–0.005 ms.
- **Project.** `FSCExposureSettings::From` derives the meter (`r_exposureMeterMode` 3 → ClampedBlend, blend 0.5, clamp −1.5 / +1.0 =
  3 × / 2 × blend, smoothing 0.5 s; mode 2 an unclamped blend, 1 incident, 0 reflected) and the adaptation (pigment bright 2 s /
  dark 5 s as the slow stage, pupil 0.4 s and 2 stops as the fast stage) from the fixture; `Apply(S, bGameMeter, bGameAdaptation)`;
  `GameMeteredLog` is the C++ port of `CalcLogLumMetered` the test compares the engine rule with (all four modes, worst < 1e-5 stops).
  Removed: the LUT cvar re-domain before the curve existed and the plugin's copy of the lens-attenuation rule (`FSCEngineExposure::
  FromEngine` reads the API). `DefaultEngine.ini` sets `Domain=1`, `MaxStep=0.015625` (1/64 stop, a host precision choice).
  `sc.Look.Exposure <game|ue> [<game|ue>]` (meter, adaptation) is the A/B; `sc.Look.ExposureLog [s]` logs exposure, both meters and
  the stages (per frame for traces). Tests: new `.CompensationLutPolicy`, `.Meter`; `.CompensationLut`, `.Install`, `.Settings`
  rewritten; `SCBridge.StarEngine.*` read the new fixture values; 21/21 with `.StarEngine`, `.Lights`, `.Rig.Lights`,
  `.Materials.MasterAssets` (`out/e1/E1-5/tests.log`).
- **Runs** (fork, medfrnt; `out/e1/E1-5/`: `baseA.txt` / `baseB.txt` before any change, `runC.txt`, `runD.txt`, `regions-*.txt`,
  `shots/`, logs `E1-5-*.log`). Steady-state log2 exposure (and metered luminance, UE units):

  | View | (1) before E1-5 | (2) Reflected, UE adaptation (`ue`) | (3) ClampedBlend + two-stage (`game`) | log2 R / log2 I in (3) |
  |---|---|---|---|---|
  | ref | −8.583 (32.3) | −8.484 (32.4) | −9.098 (63.9) | 5.00 / 7.24 |
  | top | −8.742 (38.6) | −8.644 (38.7) | −9.258 (76.4) | 5.26 / 7.56 |
  | long | −8.497 (27.2) | −8.398 (27.2) | −8.808 (46.3) | 4.75 / 6.32 |
  | up | −7.919 (7.92) | −7.821 (7.94) | −8.033 (13.0) | 2.98 / 4.41 |
  | ceil | −7.672 (4.68) | −7.574 (4.69) | −7.823 (7.99) | 2.22 / 3.78 |
  | floor | −9.531 (92.4) | −9.430 (92.5) | −10.055 (184.9) | 6.53 / 8.62 |

  Region means (full frame, sRGB-decoded): ref 0.1307 / 0.1398 / 0.0916, top 0.1124 / 0.1201 / 0.0787, long 0.1218 / 0.1302 /
  0.0985. Against the game's own rule (compensation curve and clamp from the fixture, at the measured meter) (2) and (3) are exact to
  ±0.0006 stops at every settled view; (1) was 0.100 stops darker at all six views, uniformly (not slope dependent, so a
  multiplicative error of the old LUT path, not a domain shift; the old engine is gone, so its cause was not isolated further).
  The incident proxy reads 1.4–2.2 stops above the reflected meter, so the clamp's +1 stop binds in most views: the game meter
  exposes 0.2–0.6 stops darker than the reflected one.
- **Adaptation step** (`step.py`, Star Engine's two-stage model with the fixture's half-lives against the traced exposure; ceiling ↔
  floor sweep in five frames under the 75° camera-reset threshold, then a 3-stop compensation step with the scene fixed;
  `step-*.txt`, `step-*.csv`): with the game chain (3) and the model driven by the traced meters, RMS 0.001 stops (worst 0.024) on
  both camera steps; on the ±3-stop steps (beyond the 2-stop fast range) RMS 0.011 stops, t50 / t90 0.61 / 2.19 s measured against
  0.59 / 2.17 s modelled (darker target: the 5 s slow half-life) and 0.45 / 1.12 s against 0.44 / 1.11 s (brighter, 2 s). Fed a
  plain step target instead, the brightening camera step lags the model (t50 0.68 against 0.33 s): the incident meter's 0.5 s
  smoothing, which the game has too. UE's adaptation (2) is unchanged by E1-5: its camera steps match the baseline (1) to 0.01 s
  (t50 0.41 / 0.40 s, t90 1.32 / 1.28 s, RMS against the game model 0.106 / 0.067 stops in both).
- **UE default unchanged.** Meter Reflected and adaptation Default select UE's shader paths (`AUTO_EXPOSURE_METER 0`, no
  `TWO_STAGE_ADAPTATION`), write UE's [0] / [1] lanes, skip the illuminance passes and the second histogram (profile), and the
  LUT keeps UE's domain with `Domain 0`; in the runs, `sc.Look.Exposure ue` matched the rule to 0.0005 stops and its adaptation the
  baseline's; the settings default to Reflected / Default with UE-neutral parameters (not SC's).
- **E1-4 review fix 5 run** (`overflow-diff.txt`, `shots/diff4_overflow_*`): `r.LightFunctionAtlas.MaxLightCount 64` under
  MegaLights (123 of 186 gobo lights over the records). Old engine: against the uncapped view 69.8 / 55.4 / 0.5 % of pixels > 8
  levels (ref / long / top), the ref frame 37 % brighter (gobos lost). Fixed engine: 1.9 / 0.9 / 0.02 %, mean 1.4 / 1.0 / 0.5
  levels: the gobos stay; what differs is the overflowed lights' MegaLights shadows (the elevator canopy) and a 0.03-stop exposure
  shift.
- **Astra** (`out/e1/E1-5/astra/`, gpt-6.2-astra; the 2020 reference against (2) and (3) at the ref pose): (2) is "narrowly"
  closer; (3) "looks mildly underexposed ... by a few tenths of a stop"; the remaining differences are the pad's light pool (light
  intensity / range), shadow depth in the wall recesses (tone curve / GI) and the changed set dressing. The project keeps the
  game's chain (it is the game's rule; the reference predates the current light set); the open question is the incident proxy,
  which may read higher than the game's virtual receivers and so darken (3) through the clamp.
- **Open.** The incident meter is a proxy, not a port of `exposureincident.cfib`'s sampling (positions, 64 directions, the sun
  term). The game's histogram `r_ExposureThresholdRange` rejection stays unmodelled (as before). Mobile keeps UE's meter and
  adaptation. Fix 1 of the E1-4 review was not exercised in a run (the hangar has no simple lights or per-vertex translucency).

#### E1-5 review fixes
Engine `0d847e519` (`SCViewer: auto exposure review fixes`; Terra `out/e1/E1-5/terra-review.md` items T1–T7, Astra
`out/e1/E1-5/astra-code-review.md` items A1–A18, A1/A4/A6/A7/A8/A14 being Terra's T2/T1/T6/T5/T3/T7): `SCVIEWER_ENGINE_EXPOSURE` 4.
- **Two-stage adaptation (T1, A4, A13).** The stages adapt to the unclamped target and the brightness bounds limit the output
  scale (0.18 × the curve's compensation at the bound / the bound; the user's bias is not in the limit), as Star Engine's
  `CalcExposureAndNoise` clamps its final exposure; the scale is 0.18 / 2^(slow + fast) with no re-clamp in UE's uncompensated
  space. Steady state is unchanged inside the bounds and beyond them wherever the curve holds its end value (the SC mapping's curve
  ends at the bounds). Luminances are floored (2^−60) so a zero target cannot write a non-finite history.
- **Curve snapshot (T2, A1, A2, A12, A15).** `FSceneView::AutoExposureCompensationCurve` (`UE::AutoExposure::
  FCompensationCurveSnapshot`, made in `EndFinalPostprocessSettings` on the game thread: keys, LUT domain, contents hash) is all the
  renderer reads of the curve: LUT key, bake, shader scale / bias and the CPU curve without a LUT. The illuminance passes (before
  the LUT task is waited on) bind a dummy LUT and do not read the view state's. Only `RCCE_Constant` ends are held (`RCCE_None`
  cycles in `FRichCurve::RemapTimeValue`); every domain has ≥ 2 samples and MaxX > MinX.
- **Incident meter (T4, T5, T6, A7, A9, A10, A11).** A linear-space reduction of the illuminance texture (`IncidentMeterReduceCS`,
  64 × 64-texel tiles of Σ luminance × meter weight and Σ weight; the eye adaptation pass sums the tiles) replaces the second
  histogram, so `HistogramLogMax` no longer clips it; unlit pixels are written as −1 (no sample), so a dark lit surface counts as 0
  with any `BlackHistogramBucketInfluence`. Every non-Reflected meter keeps the scene colour histogram (reflected meter, Incident's
  fallback, local exposure). A view takes the family illuminance texture only where its own passes wrote it; the illuminance
  shaders stop at their view rect; `bEyeAdaptationAllViewPixels` reduces the union of the family's illuminance rects.
- **Fallbacks and state (T3, A3, A5, T7, A14).** Desktop forward (`IsForwardShadingEnabled`) meters Reflected. Basic eye adaptation
  reads the view state's three-record history, not the one-record system fallback. An absent meter state is `MeterStateNone` in all
  four lanes (`MeterStateNoneVector`; `FVector4f(x)` had left 0, 0, 1); mobile writes it. `GetLastAutoExposureReadback` returns the
  last readback whole under a lock; `sc.Look.ExposureLog` uses it.
- **Project (A16–A18).** `sc.Look.Exposure ue` overrides the meter and adaptation to Reflected / Default instead of clearing the
  override (test `SCBridge.ViewerExposure.UeSelection` blends a map volume with Incident / TwoStage under it; `.Install` updated);
  the tests' scoped cvar restores the set-by priority; `StarEngineRender.json` `extracted_from.additions` records change 13165946
  for the E1-5 entries (notes updated).
- **Runs** (medfrnt, `out/e1/E1-5/review/`: `rv0.txt` on the E1-5 engine, `rv1.txt` with the fixes; `steps-rv*.txt` from
  `scripts/steps.py`; logs `E1-5-rv0.log`, `E1-5-rv1.log`). Steady state unchanged: `sc.Look.Exposure ue` equal to ±0.0001 stops at
  all six poses, the game chain ref −9.0971 / −9.0971, floor −10.0551 / −10.0552. The incident meter no longer depends on the
  histogram range: with `HistogramLogMax` 25.966 → 12 → 10 → 8 it read 7.240 → 7.220 → 7.211 → 6.977 (ref) and 8.624 → 7.916 (floor)
  before, 7.2264 / 8.6110 at every maximum after (the linear mean is 0.014 stops below the old bucket mean at the default range).
  Compensation +3 with the target beyond a bound: before, 99.5 % of the change within 0.1 s at the upper bound (and −3 at the lower
  bound 99 % within 0.25 s); after, none within 0.5 s, t50 1.5 s, t90 2.6 s, and only +1.23 stops (the limit holds the other 1.77 the
  scene lies beyond it); at the lower bound (6 stops of saturation) the output stays at its limit, as the game's clamp would. Released
  after 30 s of saturation, the exposure takes the adapted state within 0.1 s (the state kept adapting; before, it re-adapted from the
  bound). Incident alone at ref: −9.8655 → −9.8567 (its reflected lane is now measured: 4.94).
- **Tests** (tests first where the rule is pure): `.CompensationLutPolicy` (None ends keep UE's domain, held ends match
  `GetFloatValue` outside the keys, degenerate domains valid, the scoped cvar keeps its priority), `.UeSelection`, `.Install`;
  25/25 with `SCBridge.Fx`, `.StarEngine`, `.Lights`, `.Rig.Lights`, `.Materials.MasterAssets` (`E1-6-tests.log`).
- **Not covered.** No run reached desktop forward, split screen, panoramic / cube captures, mobile or the Basic method (the fixes
  there are by reading and compiling); A13's zero target was not provoked. The GPU cost of the linear reduction was not profiled
  separately (it replaces the second histogram, one 64 × 64-texel tile per group).

#### E1-5 review fixes 2
Engine `3808c1160` (`SCViewer: auto exposure review fixes 2`; Terra `out/e1/E1-6/terra-review.md` 1-3, Astra
`out/e1/E1-6/astra-code-review.md` E1-5 table): `SCVIEWER_ENGINE_EXPOSURE` 5.
- **LUT task race (A, High).** `GetEyeAdaptationFixedExposure` (from `UpdatePreExposure`, while the curve LUT task may still replace the
  view state's LUT) builds its scalars with `bCurveLUTReady` false: it ignores the curve and now never reads the LUT pointer.
- **Curve snapshot after view extensions.** `FSceneRenderer`'s constructor (the game-thread handoff) retakes `FViewInfo::
  AutoExposureCompensationCurve` from the final settings, after `ULocalPlayer`'s extension `SetupView`; `r.SplitScreenDebug` clones copy
  the snapshot with their settings.
- **CPU-curve output limits.** On the path without a LUT the two-stage limits take the curve at the brightness bounds from the snapshot
  (`ExposureCompensationCurveAtMin / Max`); they had used the factor at the last metered luminance, so the limits moved with the scene.
- **Incident meter.** Reconstruction is unfloored linear luminance (the histogram's 2^HistogramLogMin floor brightened a dark room); the
  tile sum carries its weight to the decision (`ComputeIncidentMeanLuminance` returns mean and weight): only zero weight falls back to the
  reflected meter, a valid black reading is floored at 2^-60 for its logarithm; the illuminance texture is `PF_R32_FLOAT` when a view
  meters incident light (a pre-exposed reconstruction over a small albedo could pass half float's 65504, and the infinite sample dropped
  out with its weight); UE's ignore-materials path alone keeps `PF_R16F` and its floored luminance.
- **Multi-view (explicit fallbacks, per the coordinator's guidance).** With several views the illuminance rects are `GetDownscaledViewRect`
  for every pass that writes or reads them (no column shared by adjacent views; a single view keeps UE's rect). An all-view-pixels incident
  meter (cube captures, panoramas) needs every family view to write its rect, the rects to tile their union, and one pre-exposure; any
  other family (mixed meters, gaps, differing pre-exposures) meters reflected light, warned once (non-shipping), rather than a per-rect
  reduction. The HDR visualizer reads the illuminance texture only where the view's own passes wrote it (scene colour otherwise).
- **Trace.** `sc.Look.ExposureLog` drops `pre_exposure` (a render-thread value read unsynchronised, possibly from another frame).
- **LUT domain.** `UE::AutoExposure::FCompensationCurveLUTPolicy` holds the policy's inputs (`FromConsoleVariables`), and
  `GetCompensationCurveLUTDomain(Curve, Policy)` evaluates it without console variables (the cvar entry point is that with
  `FromConsoleVariables`). A degenerate width falls back to [MinX, MinX + 1] and, where MinX + 1 is not above MinX (|MinX| >= 2^24) or MinX
  is not finite, to UE's [-10, 20]; Domain 0's sample count is limited by the texture (`GetMax2DTextureDimension`, a 16384-sample D3D12 LUT
  stays 16384), the 8192 cap applies to the key-range policy only.
- **Tests.** `SCBridge.ViewerExposure.CompensationLutPolicy` runs on `FCompensationCurveLUTPolicy` and sets no console variable (the
  scoped override and its history residue are gone): degenerate widths at MinX = MaxX = 3, +-2^24 and 3e38 in both policies give two
  samples and a positive, finite width; Domain 0 keeps 16384 samples (4096 with a 4096 texture), the key range keeps its cap; the cvar
  entry point equals `FromConsoleVariables`. These need the new API, so they are not tests first; the rest of the batch is GPU-side.
- **Runs** (medfrnt, E1-5's review script; `out/e1/E1-5/review/rv2.txt`, `steps-rv2.txt`, log `E1-5-rv2.log`): steady exposure against `rv1`
  within 0.0005 stops at every pose and case (game ref -9.0972 / -9.0971, floor -10.0551 / -10.0552; UE unchanged to 0.0001; incident
  alone ref -9.8572 / -9.8567); the incident meter reads 0.0005 stops above `rv1` (ref 7.2269 / 7.2264, floor 8.6115 / 8.6110: the
  unfloored, FP32 reconstruction) and stays independent of `HistogramLogMax` (7.2269 / 7.2268 at 25.966 / 8). Compensation steps at the
  bounds as `rv1` (upper +3: t50 1.49 s, t90 2.63 s, +1.231 stops; -3: t50 0.16 s; release within 0.1 s). No error lines.
- **Not covered.** No run reached split screen, `r.SplitScreenDebug`, cube captures, a view extension that swaps the curve, or the HDR
  visualizer (by reading and compiling); the half-float overflow was not provoked in a scene (the hangar's reconstructions stay far below
  it).

#### E1-5 review fixes 3
Engine `9303fe332` (`SCViewer: auto exposure review fixes 3`; Terra `out/e1/fixes2/terra-review.md` T2, T6, Astra
`out/e1/fixes2/astra-code-review.md` 10-12, 14, 15; triage `out/e1/fixes3-brief.md`): `SCVIEWER_ENGINE_EXPOSURE` 6.
- **T2 / A10, family incident reduction.** Every view of an all-view-pixels family must meter incident light (`GetAutoExposureMeter` not
  Reflected; `NeedsAutoExposureIlluminance` is also true for UE's ignore-materials Reflected view, which writes floored luminance and keeps unlit
  pixels), share the pre-exposure, and the rects must tile their union: `UE::AutoExposure::DoRectsTileTheirBounds` (each non-empty, pairwise
  disjoint, summed area = bounding area; strips [0,2) [1,3) [4,6) passed the area test). Otherwise the documented per-view Reflected fallback.
- **A14, collapsed rects.** In a family of several views, if any view that writes the illuminance texture has a downscaled view rect that
  collapses (`UE::AutoExposure::IsDownscaledViewRectCollapsed`: `GetDownscaledViewRect` would expand it into its neighbour's texel; downscale 3,
  [0,1) and [1,12)), `GetAutoExposureMeter` returns Reflected for the family's views (warned once, non-shipping), so no view writes a texel
  another reads.
- **T6, UE's rects.** `GetAutoExposureIlluminanceRect` uses `GetDownscaledViewRect` only for multi-view families with an incident producer;
  single views and every family without one (UE's ignore-materials path included) keep UE's `GetDownscaledRect(ViewRect)`, as in `32e0d4024`
  (`GetDownscaledViewport` in both passes, `GetDownscaledRect(PrimaryViewRect)` in post-processing; `PrimaryViewRect` is `View.ViewRect`).
- **A11, fallback histogram source.** The histogram's luminance source follows the bound texture: where the fork binds scene colour in
  place of a valid illuminance texture (a sibling's texture, or the incident fallback) it requests `EHistogramLuminanceSource::SceneColor`
  (UE's ignore-materials permutation read `.r` as precalculated luminance). UE's own bindings keep `ViewDefault`. Not changed (UE behaviour, a
  further low finding): UE's own all-view-pixels path with `r.AutoExposure.IgnoreMaterials` and precalculation binds scene colour with the
  precalculated permutation too.
- **A12, crossing output limits.** The two-stage clamp takes the two bounds' scales ordered (`min` / `max`; `UE::AutoExposure::
  GetTwoStageOutputScaleLimits` is the C++ rule): bounds 1 and 2 with a curve rising 2 stops between them gave `clamp(x, 0.36, 0.18)`.
- **A15, LUT addressing.** `UE::AutoExposure::GetCompensationCurveLUTScaleBias` computes UE's scale `(N - 1) / (N (MaxX - MinX))` and bias in
  double, rounded once; the domain validation also requires a normal, finite FP32 scale, a finite bias and a finite, positive bake spacing (else
  [MinX, MinX + 1], then UE's [-10, 20]). The renderer's shader coefficients come from it.
- **Tests** (`SCBridge.ViewerExposure.ReviewRules3`; tests first on the old engine through the rules it applied, transcribed in the test under
  `SCVIEWER_ENGINE_EXPOSURE < 6`; `out/e1/fixes3/testsfirst.log`): A15 3e34-wide Domain 0 (old scale 0, MaxX at texel 0.00003; new MaxX at
  0.99997) and a 1e38-wide domain (old subnormal scale); A12 limits on the CPU curve and through the half-float LUT (old (0.36, 0.18) on both, new
  (0.18, 0.36), CPU and LUT within 0.2 %); T2 tiling (old: the equal-area overlap and gap passed); A14 collapse (old: none detected).
- **Runs** (medfrnt, E1-5's review script, `out/e1/fixes3/rv3.txt`, `steps-rv3.txt`, `scripts/compare_rv.py`; log `E1-5-rv3.log`): every steady
  exposure of `rv2` (UE chain, game chain, the histogram-maximum and Incident cases, the bounds, saturation and release; 27 cases) within
  0.0004 stops (game ref -9.0971, floor -10.0551; UE ref -8.4844; incident meter ref 7.2269, floor 8.6115 as in `rv2`); the compensation steps
  as `rv2` (upper +3: t50 1.49 s, t90 2.63 s, +1.231 stops; -3: t50 0.16 s; release within 0.1 s). No error lines. The hangar is a single view, so
  this shows UE's and the fork's single-view paths unchanged; the multi-view rules are the tests'.
- **UE default.** Single views and families without an incident meter take UE's rects and histogram sources; the A12 / A15 changes act only on
  the two-stage path (opt-in) and on the scale / bias rounding (double then float: at most one float ulp from UE's float expression).
- **Not covered.** No run reached a multi-view family (split screen, cube captures, panoramas): T2, T6, A11 and A14 are by reading, compiling and
  the pure-rule tests.

### E1-6 Niagara lightweight emitters
Today: `NiagaraStateless::MaxGpuSpawnInfos` = 8 (overflow drops live loops; our FX builder stretches loop durations);
spawn-rate bindings cannot evaluate expressions; expressions have add / multiply only; sprite size ignores component
scale for local-space emitters; the FX commandlet needs `bTreatAsEngineModule` to reach `Niagara/Internal/Stateless`.
- Raise the spawn-info capacity with a measured cost (not to be confused with `NIAGARA_MAX_GPU_SPAWN_INFOS`), keeping
  the builder's loop guard derived from the engine constant.
- Export a narrow stateless authoring API so the commandlet is an ordinary module.
- Then, separately: spawn-rate expression evaluation; clamp / min / max / lerp expressions; an explicit sprite-size
  scale policy (UE default unchanged).

#### Result (E1-6)
Engine `ad5c402d9` (capacity), `05559963d` (expression exports), `9f382da2b` (spawn-info expressions), `c7a0e82f7` (min / max /
clamp / lerp); `SCVIEWER_ENGINE_NIAGARA_STATELESS` 4.
- **Capacity (a).** `NiagaraStateless::MaxGpuSpawnInfos` is 32 (UE 5.9: 8), one constant in the new public header
  `NiagaraShader/Public/Stateless/NiagaraStatelessSpawnInfoCapacity.h` (no cvar: it sizes the shader parameter arrays and the
  simulation shader's define). Overflows are logged (non-shipping, `LogNiagara` "has overflowed spawn infos (N of C)", once per
  emitter when its worst overflow grows; UE drew it on screen in the editor only). Cost: 8 scalar arrays × 4 B × 24 = 768 B more
  shader parameters per stateless emitter instance (per dispatch upload); GPU (`ProfileGPU`, 40 steam jets, the stateless
  dispatches `FNiagaraStatelessComputeManager_OnPreRender`, 40 dispatches): 0.035 / 0.033 ms at 8, 0.034 / 0.034 ms at 32 with the
  same 1 s loops (8 intervals each), 0.036 / 0.035 ms at 32 with 0.25 s loops (29 intervals each): within run-to-run noise.
  Builder: the loop guard is `SCFxRules::LoopDuration(min(lifetime, 0.25 s), lifetime, NiagaraStateless::MaxGpuSpawnInfos)`
  (the engine's constant; tests `SCBridge.Fx.LoopGuard`, `.EngineCapacity`): a loop is stretched only where 1 + ceil(lifetime /
  loop) would not fit, now for lifetimes over 7.75 s instead of 1.75 s. The project's tractor / impact set never needed the stretch
  (lifetimes ≤ 1 s, ≤ 5 live loops) and builds unchanged loops; a pulse burst whose bursts would not fit is reported (none in the
  data). `-LoopCapacity=<n>` builds for another capacity (verification aid).
- **Narrow API (b), at a documented boundary.** Exported: the capacity (public header) and the expression structs (NIAGARA_API), so
  the commandlet builds expressions typed instead of through reflection. Not done: dropping `bTreatAsEngineModule`. The commandlet
  configures the emitter, its spawn infos, the emitter state (`FNiagaraEmitterStateData`, `Niagara/Internal`) and 16 module
  classes with their settings and enums, all in `Niagara/Internal/Stateless`, whose headers include the build / simulation contexts
  and shader-parameter builders: a public header set means moving most of that tree (the whole authoring surface, and merge
  conflicts with every upstream change to it), and a facade without those types means reflection by property name (losing the type
  checking the commandlet has). The editor-only `SCBridgeFx` module keeps the flag; the rest of the plugin is an ordinary module.
- **Spawn-rate expressions (c1).** Spawn rate, spawn probability, loop duration and loop delay in Expression mode are built against
  the emitter's bindings and evaluated per loop (UE 5.9 evaluated the unbuilt expression: a binding inside it read offset −1).
  Builder: a beam emitter's rate is `PerCm × SC.BeamLength` (the runtime no longer sets `SC.BR<i>` / reads `SC.BK<i>`); a constant
  rate with a count strength curve is `rate × curve(SC.Strength)` (none in the data).
- **Expressions (c2).** `Min`, `Max`, `Clamp`, `Lerp` (float). Builder: every EmitterStrength curve is the exact piecewise-linear
  curve held at its ends, `Base + Σ Rise × clamp((s − From) × Scale, 0, 1)` (`SCFxRules::ClampSum`, test `SCBridge.Fx.ClampSum`):
  the runtime's `SC.SV<i>` / `SC.SK<i>` evaluation (`SCFxStrength.h`) and the > 4-key polynomial are gone.
- **Sprite size and scale (c3), no engine change.** UE 5.9 already has the policy: the stateless template's Apply Owner Scale To
  Attributes module (`FNiagaraStatelessSystemScaleBuildData`: initial sprite size off by default = ignores the component's scale;
  by the largest axis or one axis, with an amount; also light radius, ribbon width, decal size, velocity, forces, camera offset),
  applied in `InitializeParticle`'s shader parameters, with fixed bounds that follow the component transform. A second enum would
  duplicate it. Builder: sprite-like emitters (not guided beams, not lights) enable it (largest axis, amount 1); `SC.Scale2` and the
  runtime's `SetVariableVec2("SC.Scale2")` (cargo contacts, `sc.Fx.Spawn scale=`) are gone.
- **Runs** (medfrnt, `out/e1/E1-6/`: `run-*.txt`, `summary-*.txt`, `shots/`, logs `E1-6-*.log`; FX builds `E1-6-fxbuild-*.log`, all
  `SC_FX_DONE systems=11 textures=10 materials=13 errors=0`; the set plus the game's steam jet `Generic_Steam_FX`
  `steam_jet_4x8m_loop_1_gpu_decoupled`, 7 s lifetime, Count Random 0.125, so its loops never coalesce). "overflowed spawn infos":
  (a) engine at 8 (a local measurement build of these commits), old builder: 0 (the steam's loop stretched to 1.001 s, 8 intervals);
  (b) engine at 8, new builder sized for 32 (`-LoopCapacity=32`: the steam at 0.25 s, 29 intervals): the steam overflows, 21
  warnings up to "29 of 8"; (c) the fork at 32, default build: 0. Tractor beam (strength 0.5, 6 m, bent 15°) and metal impact
  (scale 2) before (a) and after (c): PSNR 51.0 dB (55.1 dB for the beam at strength 1) against 34.9 dB between consecutive frames
  of one run. Tests: `SCBridge.Fx.LoopGuard`, `.EngineCapacity` (failed on the 8 build, as it should), `.ClampSum`; 25/25 on the
  fork (`E1-6-tests.log`).
- **Astra** (`out/e1/E1-6/astra/fx.md`, gpt-6.2-astra, frame sheets `shots/sheet_before.png`, `sheet_after.png`,
  `sheet_steam_after.png`): beam "no visible regression"; impact "same effect", the fuller AFTER burst put down to randomness and
  re-trigger phase, "no consistent enlargement or shrinkage suggesting a sprite-scale regression"; steam "visually continuous";
  verdict: acceptable as a drop-in replacement.
- **UE default.** Non-expression spawn-info and emitter-state distributions take the unchanged code paths; constant expressions
  evaluate as before; the new expression structs are additions; the scale policy is UE's own module, off by default; the overflow
  log is a diagnostic. Run (a) above is UE-default content for these paths (bound and ranged rates only): the old builder's systems
  ran unchanged on the new engine (0 overflows; (b) against (c) and (a) against (c) visually equal). The capacity itself is a
  deliberate change for all content (more intervals drawn, never fewer).
- **Not covered.** The steam jet sits at the floor in the first runs (its soft-particle fade, half of a 14 m radius, hid it); the
  visible steam frames are the fork's only (no 8-capacity steam frames). `SC.BeamLength2` stays (a float → vec2 conversion was not
  needed for the data). Cooked showcase FX were not rebuilt; systems built before E1-6 need `-run=SCFxBuild` (the runtime no longer
  sets `SC.SV` / `SC.BR` / `SC.Scale2`).

#### E1-6 review fixes
Engine `e2ec17572` (`SCViewer: stateless spawn review fixes`; Terra `out/e1/E1-6/terra-review.md` 4-5, Astra
`out/e1/E1-6/astra-code-review.md` E1-6 table): `SCVIEWER_ENGINE_NIAGARA_STATELESS` 5.
- **Capacity per the WP rule.** `NiagaraStateless::MaxGpuSpawnInfos` (32) is the compiled storage; the effective limit is
  `NiagaraStateless::GetMaxSpawnInfos()`, the cvar `fx.Niagara.Stateless.MaxSpawnInfos` (default `DefaultMaxSpawnInfos` = UE 5.9's 8,
  clamped to [1, 32]; the overflow log and the on-screen message name it). The project sets 32 in `DefaultEngine.ini` `[SystemSettings]`,
  which the editor, the game and the SCFxBuild commandlet all read, so the builder's loop guard (`SpawnInfoCapacity()`) is the limit the
  project ships with (`SC_FX build ... spawn_infos=32`). UE content that does not opt in draws what UE draws.
- **Expression validation (High).** `FNiagaraStatelessExpression::ValidateTree`: a set, concrete root of the distribution's type, set
  operands of the struct their property declares (`BaseStruct`, where metadata exists), binding leaves that name a parameter. The emitter
  build validates spawn rate / probability and loop duration / delay expressions before building them; an invalid one is logged
  (`LogNiagara` warning naming the emitter, the distribution and the problem) and becomes the distribution's constant (its Min). Every
  binding leaf whose name resolves to no renderer binding builds as its type's default constant instead of a binding at offset -4 (Float,
  Vec2, Vec3, Vec4, Color). Unbuilt, only a valid constant expression is evaluated.
- **Expression-aware filtering.** An Expression spawn rate is kept at build whatever its literal Min / Max (`FNiagaraStatelessSpawnInfo::
  IsValid`); with an Expression loop duration a burst's time is checked per loop against the evaluated duration only.
- **Parameter collections.** `CacheParameterCollectionReferences` also walks the emitter state's and the spawn infos' distributions
  (`FNiagaraDistributionBase::ForEachParameterBinding` over `FNiagaraEmitterStateData` / `FNiagaraStatelessSpawnInfo`), so a collection that
  only a spawn or loop expression binds is bound at run time and referenced for cook.
- **Rate change.** The residual between loops is the fraction of a particle the last loop left (`ResidualSpawnTime` x `ResidualSpawnRate`);
  at a new rate it is that fraction's time at the new rate, so a rate change places the next birth and backdates none (1 / s to 100 / s
  after a 0.25 s residual made 25 particles appear already aged); at an unchanged rate it is UE's residual time, bit for bit.
- **Builder** (`SCFxBuildCommandlet.cpp`, `SCFxRules.h`): strength curves are classified over the configured strengths, 0, 1 and every
  authored key in [0, 1] (`ClassificationStrengths`), so a curve peaking between the knots is neither constant nor invisible; a light's
  intensity curve and its colour's luminance curve (relative to strength 1, at the colour curve's own keys) are two exact expressions,
  multiplied (no resampling of their product at the knots); a zero-width key interval is a strict step (`StepScale`: the later value from
  the next float above the key; the float expression is tested, `EvalFloat`); pulse bursts are counted with an exclusive expiry
  (`LiveBursts`: 8 s at 0.25 s keeps 32, not 33) and the warning says the newest are not drawn; a rate loop is stretched only where its live
  loops do not fit (7.75 s at 0.25 s fits 32 exactly and keeps 0.25 s).
- **Stale FX.** The generated contract is versioned: SCFxBuild writes the user parameter `SC.FxContract` = `SCFxRules::FxContractVersion` (2);
  the runtime (`SCFxContract::Check`, from `SCFxStrength::Set`, non-shipping) warns `SC_FX_STALE` once per system without it or with an older
  one; `-run=SCFxBuild -VerifyContract` lists every system under `/Game/SC/FX` and fails on a stale one, and `ue/scripts/package_showcase.ps1`
  runs it before cooking and refuses stale systems. The legacy setters (`SC.SV`, `SC.BR`, `SC.Scale2`) stay removed.
- **Tests** (tests first for the builder rules: `out/e1/E1-6/review/testsfirst.log`, the new `SCBridge.Fx.ReviewRules` against the E1-6
  rules: the exact fit stretched to 0.25025 s, 33 / 33 / 34 bursts where 32 / 32 / 33 are alive, the step's later value
  0.218 / 0.060 / 0.25 at the next float, the peak at 0.25 classified as 0). After the fixes: `.ReviewRules`, `.ClampSum` (no skipped
  ramp), `.LoopGuard`, `.EngineCapacity` (storage 32, UE default 8, the project's effective 32), `.ExpressionValidation` (engine API: a
  bound rate x 2 valid; an empty root, an unnamed binding, an empty operand, a vec3 root, a vec3 operand of a float multiply rejected);
  `Automation RunTests SCBridge` 137 of 141 (`out/e1/E1-6/review/tests.log`; the same four failures outside these features as above).
- **Runs** (`out/e1/E1-6/`: `fxbuild-rv-stdout.log`, `review/verify-stdout.log`, `review/run-rv.txt`, `review/psnr.txt`, `review/shots/`;
  logs `E1-6-fxbuild-rv.log`, `E1-6-verify.log`, `E1-6-rv.log`). FX build `SC_FX build ... spawn_infos=32 loop_capacity=32`, `SC_FX_DONE
  systems=11 textures=10 materials=13 errors=0`; the steam jet keeps 0.25 s loops (live_loops=29/32), nothing stretched. `-VerifyContract`:
  `SC_FX_CONTRACT systems=7 stale=0 version=2` after the rebuild. Game run (beam, impact, steam): 0 overflowing emitters, no
  `SC_FX_STALE`; PSNR against E1-6's `c32` frames: visual 39.8 / 42.8 / 49.9 / 52.3 dB (E1-6's own before / after 41.3 / 42.9 / 51.0 /
  52.8), beam 55.1 / 55.1 dB (55.1 / 55.0), steam 44.5 to 45.6 dB; consecutive frames of one run 34.7 dB.
- **Not covered.** A cooked showcase package was not built (the verify step was run on its own); a Niagara parameter collection driving a
  spawn rate was not exercised in a run (code and the reference walk only); the rate-change residual was checked by reading (the project's
  beams change length continuously).

#### E1-6 review fixes 3
Engine `41df91ebe` (`SCViewer: stateless spawn review fixes 2`; Astra `out/e1/fixes2/astra-code-review.md` 1-5, 8; Terra
`out/e1/fixes2/terra-review.md` T3-T5 and Astra 6, 7, 9 for the builder; triage `out/e1/fixes3-brief.md`): `SCVIEWER_ENGINE_NIAGARA_STATELESS` 6.
- **A1, one captured limit.** `fx.Niagara.Stateless.MaxSpawnInfos` is `ECVF_RenderThreadSafe`; `GetDataBuffer` reads the limit once and passes
  it to the count (`CalculateActiveParticles(..., MaxSpawnInfos)`), the allocation and the CPU generation (`SimulateGPU`), so a cvar change
  between them cannot make the generation exceed the allocation.
- **A2, runtime validation.** `ValidateTree` holds in every build: a typed base (`FNiagaraStatelessExpressionFloat` / `Vec2` / ...: no evaluator,
  `checkNoEntry`) is rejected as root or operand, and an operand must be of its parent's typed base (every engine operation's operands are; the
  editor's `BaseStruct` check stays too). The emitter build validates before `Build` (which folds), and an invalid tree becomes the constant.
- **A3, unset loop duration.** `FNiagaraStatelessSpawnInfo::IsValid` tests `!LoopDuration.IsSet() || SpawnTime < LoopDuration` (UE's
  `SpawnTime + UE_SMALL_NUMBER` stand-in rounds to `SpawnTime` from about 0.1 s, so a burst at 2 s under an expression loop was dropped). UE's
  callers always pass a set duration.
- **A4, UE timing for stock emitters.** The fractional-particle residual applies only where the rate is an expression; ranged and bound rates
  keep UE's residual time and birth timing.
- **A5, collections.** `CacheParameterCollectionReferences` resolves a collection that is not loaded through the asset registry (its
  `Namespace` tag, then a load), so the reference is cached and saved; the editor's `GatherSimulationAttributeReferences` walks the emitter
  state's distributions too.
- **A8, counter.** Every accepted spawn info counts against the limit also when only counting (`GetNumParticles`); UE counted only with shader
  parameters (nine one-particle bursts at a limit of eight reported nine).
- **New expression** `FNiagaraStatelessExpressionFloatGreaterThan` (A > B ? 1 : 0) for the builder's steps (T5).
- **Builder and verifier (repo, `SCFxRules.h`, `SCFxBuildCommandlet.cpp`).** T3 / A6: strength curves are classified on the classification
  strengths and every key's own value in [0, 1] (`ClassificationValues`: both sides of a step); the light's luminance keys come from each colour
  key's own channels (`RelativeLuminanceKeys`). T4: the light's reference colour is the colour at strength 1 or, black there, the brightest of
  the curve's strengths and keys (`LightColourReference`; the relative luminance curve was 0 everywhere). T5 / A7: a zero-width key interval is
  the comparison expression `x > From` (`FClampTerm::bStep`), exact at 0 and at its next float (a denormal). A8: `-LoopCapacity=1` is honoured
  (`LoopDuration` keeps the wanted loop below 2, and the builder reports a loop that cannot fit; it used to stretch to an infinite loop). A9:
  `-VerifyContract` counts an identified system that does not load as an error (`load_failures=`). The generated contract is version 3
  (`SCFxRules::FxContractVersion`): systems built by the E1-6 builder are stale.
- **Tests** (tests first on the old engine: `SCBridge.Fx.ReviewRules3` failed on T3, A6, T4, T5 / A7 and A8, `out/e1/fixes3/testsfirst.log`):
  `.ReviewRules3`, `.StepExpression` (the builder's tree evaluated by the engine at 0 / 0.25 / 0.5 and the next float; GreaterThan),
  `.ExpressionValidation` (bare float base as root and operand, the vec3 operand outside editor-only data). All batch-3 and E1-7 review tests together: `Automation RunTests SCBridge` 148 of 152 on the final engine (`out/e1/fixes3/tests-final.log`; the four known failures outside these features); tests first on the old engine 140 of 148 (the four known plus `.Fx.ReviewRules3`, `.Lights.ProbeFollowsRenderer`, `.ViewerExposure.ReviewRules3`, `.ViewerExposure.LegacyClearMigration`).
- **Runs** (`out/e1/fixes3/`: `fxbuild-rv3-stdout.log`, `verify-stdout.log`, `fxrun-rv3.txt`, `fx-psnr.txt`, `shots/`; logs
  `fixes3-fxbuild-rv3.log`, `fixes3-verify.log`, `fixes3-rv3.log`). FX build `spawn_infos=32 loop_capacity=32`, `SC_FX_DONE systems=11 textures=10
  materials=13 errors=0` (the steam jet keeps 0.25 s loops, live_loops=29/32); `-VerifyContract`: `SC_FX_CONTRACT systems=7 stale=0
  load_failures=0 version=3`. Game run (beam, impact, steam): 0 overflowing emitters, no `SC_FX_STALE`; PSNR against E1-6's `c32` frames: visual
  51.6 / 44.7 / 41.9 / 51.8 dB, beam 54.1 / 54.1 dB, steam 44.2 to 44.7 dB (the E1-6 review run: 49.9 / 42.8 / 39.8 / 52.3, 55.1 / 55.1, 44.5 to
  45.6 dB; consecutive frames of one run 34.7 dB). The builder's per-emitter mapping report is identical to the E1-6 review build's (sorted
  `SC_FX emitter=` lines: the classification, light reference and loop of every emitter are unchanged).
- **UE default.** Stock emitters: UE's residual timing (A4), UE's burst filter for set durations (A3); the counter (A8) is a UE bug fix that
  changes `GetNumParticles` only where spawn infos overflow the limit; validation rejects only trees UE could not evaluate (checkNoEntry or a
  mistyped operand).
- **Not covered.** A cold collection rebuild (A5) and a cvar change mid-frame (A1) were not provoked (reading and compiling).
- **Further low findings (not fixed, loop end).** The builder's age curves (`RichCurve`, colour / size / alpha over particle age) still take
  each key's value as `Eval` at its time, so two coincident age keys collapse to the earlier value (the strength-curve fix does not reach
  them; no effect of the set has such keys: the mapping report is unchanged). A2's runtime operand rule assumes every operation reads operands
  of its own type (true for all engine expressions); a future mixed-type operation would need its own rule.

### E1-7 SSDO as an engine feature
Today: patches in `MegaLightsShading.usf` and `DiffuseIndirectComposite.usf`, constants in a generated header,
switched by `r.GeneralPurposeTweak`; off in the current look (D15).
- Post-process settings for radius / strength / amounts, cvars for force-enable, quality and debug views; shader
  parameters instead of the generated header; one RDG pass producing occlusion and bent direction consumed by
  MegaLights and the indirect composite. Defined interplay with Lumen short-range AO (no double darkening). Never
  injected into world-space caches or the path tracer.

#### Result (E1-7)
Engine `6f5b30b63` (`SCViewer: screen-space directional occlusion as an engine feature`), superseding `b65d09ad1` / `9cad77abb` (their
shader edits, `SCSSDO.ush` and the generated `SCSSDOParams.ush` are removed; no history rewrite); `SCVIEWER_ENGINE_SSDO` 1.
- **Settings.** `FPostProcessSettings` `SSDORadius` (cm, default 200), `SSDOStrength` (1), `SSDODirectAmount`, `SSDOAmbientAmount`,
  `SSDOReflectionAmount` (default 0: UE's behaviour, no pass), override bits, interpolated in `SceneView` and `PostProcessUtils`. Console:
  `r.SSDO` 0 off / 1 as the settings ask (default) / 2 forced on (amounts left at 0 count as 1); `r.SSDO.Quality` 0..2 = 2 x 4, **3 x 6**
  (default, the game's `r_ssdoQuality` 0), 4 x 8 slices x samples per side; `r.SSDO.TemporalNoise` (per-frame rotation of the 4x4 noise, the
  port's stand-in for `SpatialDenoisePS`'s blur); `r.SSDO.Debug` 1 AO / 2 bent normal over the lit scene colour (also `vis SSDO`). Clamps:
  amounts >= 0, non-finite = 0, radius [1, 100000] cm, strength >= 0. `UE::SSDO` (`ScreenSpaceDirectionalOcclusionSettings.h`, Engine) is
  the rule the renderer applies: `ResolveViewSettings`, `DirectLightFactor` (`CalculateSSDO`), `DeriveSpecularOcclusion`,
  `ComposeIndirectOcclusion`, `GetLumenShortRangeAOIntensity`.
- **One pass.** An `SSDO(Quality=n)` compute pass per view after the base pass and decals (`DeferredShadingRenderer.cpp`, before the
  indirect composite and the lights): the WP-L2 port of `DirOcclusionPS` (same search; radius in cm projected with `0.5 P[1][1]`, thickness in
  metres) on the scene depth and the top-layer / GBuffer normal, writing bent normal and occlusion (`PF_FloatRGBA`). It runs only where an
  amount is positive or a debug view is on, never for forward shading, the path tracer, ray-traced overlays or reflection captures (mobile
  has its own renderer and never resolves it). Consumers bind `FSSDOConsumerParameters` (texture and amounts; a dummy and 0 where off) and read
  it through `ScreenSpaceDirectionalOcclusionCommon.ush`: MegaLights shading (GBuffer input) and the per-light deferred pass (standard and
  simple lights; new, the patches had no deferred path) scale the light term of lights without dynamic shadows and of directional lights; the
  Lumen indirect composite (screen probe gather, Substrate and GBuffer paths, in `GetShadingOcclusion`) takes its occlusion from the SSDO.
  Explicit fallbacks (no SSDO): mobile, forward, clustered deferred, translucency (forward, volume, front layer), hair strands, volumetric fog
  and MegaLights volumes, LumenPT, SSGI and the deferred sky light, ray-traced lighting, the path tracer, reflection captures. Nothing writes it
  into the Lumen surface cache, radiance cache or world cache (all consumers are screen-space passes).
- **Lumen short-range AO interplay (rule).** Term by term, the SSDO's or Lumen's, never their product: diffuse indirect occlusion =
  `SSDOAmbientAmount > 0 ? saturate(1 - w x amount) : Lumen's`; rough specular occlusion = `SSDOReflectionAmount > 0 ?
  DeriveSpecularOcclusion(NoV, saturate(1 - w x amount), 1 - roughness) : Lumen's`. When both terms are the SSDO's, Lumen's short-range AO
  intensity is 0 for the view (`SSDO::GetLumenShortRangeAOIntensity` at every Lumen call site), so it is neither computed nor applied during
  integration (`...ShortRangeAO.ApplyDuringIntegration`). Lumen's material AO (in its integration) and short-range GI stay. Tested as a pure
  rule (`SCBridge.ViewerExposure.SSDO.LumenInterplay`) and in the run below.
- **Project.** Fixture: `DeriveSpecularOcclusion` hash added (`TiledShading.cfxb`, not `ShadeLib.cfi` as WP-L2 said; read from the installed
  `Data.p4k` pak, crc32 8e895d78; the four other SSDO hashes re-checked equal); `FSCStarEngine` loads `r_ssdo*` with domain checks;
  `FSCSSDOSettings::From / Apply` (radius m -> cm, strength, amounts; `r_ssdo` 0 -> amounts 0; `r_ssdoQuality` != 0 unsupported); the viewer
  volume applies it from `USCViewerSettings::bStarEngineSSDO` (**default off**, the look D15; off = amounts overridden to 0, so a map volume
  cannot turn it on); `sc.Look.SSDO <game|off>`. Tests: new `SCBridge.ViewerExposure.SSDO.Mapping` (fixture mapping, override / clear,
  ported hashes, UE default = no pass), `.Blend` (viewer over an opt-in map volume, half-weight lerp, install follows the setting), `.Rules`
  (`r.SSDO` modes, debug, unsupported view, clamps), `.LumenInterplay` (the rule; `DirectLightFactor` = the game's `CalculateSSDO` to
  < 1e-5 over 360 cases); `SCBridge.StarEngine.*` read and range-check the new fixture values. Removed: `ue/EnginePatches/`,
  `ue/scripts/engine_patches.py`, every `r.GeneralPurposeTweak` use (routes `cutlass-ssdo*.txt`, `cutlass-look-iter1*.txt` now use
  `sc.Look.SSDO` / `r.SSDO.Debug`); AGENTS.md, README, `ue/README.md`, plan D14, WP-L2, WP-L3, LOOK-1, the WP-L2 verification, the fixture
  notes and `DefaultScalability.ini` updated.
- **Runs** (fork, medfrnt, 1920x1080, E1-5's poses; `out/e1/E1-7/`: `run-old.txt`, `run-new.txt`, `run-def.txt`, `shots/`,
  `pairstats*.txt`; logs `E1-7-old.log`, `E1-7-new.log`, `E1-7-def.log`). Baseline "old" = engine `e2ec17572` with the patch header's amounts
  set to the fixture's (1, 1, 1; the committed header had 0, the look) and `r.GeneralPurposeTweak` 0 / 1. PSNR (dB) and pixels > 8 levels:

  | Pair | ref | long | up | top |
  |---|---|---|---|---|
  | noise floor: two shots of one setting in one run (off / on) | 53.2 / 54.0 | | | |
  | **UE unchanged:** old off vs new off | 52.7, 0.02 % | 52.9, 0.01 % | 53.1, 0.01 % | 53.2, 0.02 % |
  | **equivalence:** old on vs new on | 52.6, 0.02 % | 52.4, 0.01 % | 53.7, 0.01 % | 53.3, 0.02 % |
  | equivalence with unshadowed lights (`sc.Look.ShadowAll 0`): old on vs new on | 54.6 | 52.7 | 53.9 | |
  | effect: new off vs new on | 49.8, 0.07 % | 47.6, 0.18 % | 42.9, 0.84 % | 53.6 |
  | effect with unshadowed lights: off vs on | 46.5, 0.40 % | 43.0, 0.99 % | 40.9, 2.12 % | |
  | **interplay:** SSDO on, Lumen SR-AO on vs off | 52.8, 0.02 % | 52.9, 0.01 % | 53.6, 0.01 % | |
  | reference: SSDO off, Lumen SR-AO on vs off | 49.0, 0.14 % | 47.7, 0.26 % | 43.3, 1.31 % | |
  | `r.SSDO 0` with the game's settings vs off | 52.1 | 52.5 | | |
  | deferred lights (`r.MegaLights.Allowed 0`, unshadowed): off vs on (noise 52.6 / 53.9 / 58.2) | 46.3 | 43.2 | 40.9 | |

  Old and new, off and on, are within the frame-to-frame noise of one run at every view (full-frame means equal to 0.0006), while the effect
  is 4–12 dB above it: UE's behaviour is unchanged with the feature off and the engine path reproduces the patch path. With the SSDO on,
  turning Lumen's short-range AO off changes nothing (noise level), where without the SSDO it changes the `up` view on 1.3 % of the pixels:
  the SSDO replaces it, no double darkening. The deferred consumer, first exercised here, changes the image as much as the MegaLights one.
  The debug views (`new_dbgao_ref`, `new_dbgbn_ref`) show the AO dark in crevices and smooth bent normals.
- **Cost** (`ProfileGPU`, ref view, every light shadowed): the pass is 0.53 ms (`SSDO(Quality=1) 1920x1023`); Lumen's short-range AO
  (screen space in this hangar, 0.14–0.17 ms on async compute) is skipped while the SSDO replaces it; the composite stays 0.068 ms (0.062
  off). Frame 6.98 -> 7.18 ms (+0.20). The patch path cost 6.71 -> 7.44 ms: its composite recomputed the SSDO inline (0.53 ms) and Lumen's
  AO still ran. With the SSDO on, `r.Lumen.ScreenProbeGather.ShortRangeAO 0` leaves the frame at 7.22 ms.
- **Astra** (`out/e1/E1-7/astra/`, gpt-6.2-astra, high): old vs new (up; long with unshadowed lights): "Differences within frame-to-frame
  noise" for both, no halo, grain or banding. Off vs on (long, up): "pass" for both: darker under the ledge, in door-frame recesses and
  ceiling grille frames, "no obvious double darkening, added grain, or visible smearing", no halo at the sign's or the beam's silhouette.
  Interplay (up): SSDO on with Lumen SR-AO on vs off "look the same ... This view passes the SSDO replacement check", while without the SSDO
  the SR-AO pair differs (deeper grilles with SR-AO); no halos or spatial noise.
- **Tests** (`out/e1/E1-7/tests.log`): `Automation RunTests SCBridge` 141/145; the four failures are the known ones
  (Package.FolderTarImport, Player.KitRigAndWeapons, Materials.FamilyMapping, FirstPerson.SkeletalCollision); `.Lights`, `.ViewerExposure`
  (with the four new `.SSDO.*`) and `.StarEngine` pass. Engine and project builds clean (`engine-build.log`, `project-build*.log`).
- **Not covered.** Mobile, forward, the path tracer and LumenPT were compiled, not run; clustered deferred has no consumer (explicit
  fallback); temporal stability under camera motion was not measured (stills only, as in WP-L2); `r.SSDO.Quality` 0 / 2 and `r.SSDO 2` are
  covered by the rule tests only; `scripts/verify_star_engine_render.py` was not run in full (no `objdump`): the new hash and the four SSDO
  hashes were computed with its `Shaders` reader from the installed pak.

#### E1-7 review fixes
Engine `500fca6c0` (`SCViewer: SSDO review fixes`; Terra `out/e1/E1-7/terra-review.md` 1-3, Astra `out/e1/E1-7/astra-code-review.md`; the
coordinator's triage): `SCVIEWER_ENGINE_SSDO` 2.
- **Partial replacement, no double darkening (Terra 1 / Astra H1).** With `r.Lumen.ScreenProbeGather.ShortRangeAO.ApplyDuringIntegration 1`
  Lumen occludes diffuse and rough specular together during probe integration, before the composite replaces one term.
  `UE::SSDO::AppliesLumenShortRangeAODuringIntegration`: a view whose SSDO replaces either term does not apply it during integration; its AO
  texture (and mode) is kept for the composite, which applies Lumen's AO to the term the SSDO leaves (UE's `ApplyDuringIntegration 0` path for
  that view: the integrate permutation, the SH3 irradiance format, the downsample check, the composite reset and the temporal bent normal all
  take the view's decision, `LumenScreenProbeGather::GetScreenProbeIrradianceFormat(View)`).
- **Short-range GI kept (Terra 2 / Astra H2).** Produce and consume are separate: `UE::SSDO::RunsLumenShortRangeTrace` runs the shared screen-space
  trace wherever Lumen would run it, unless the SSDO replaces both AO terms and no short-range GI (`r.Lumen.ScreenProbeGather.ShortRangeGI`,
  `LumenShortRangeAO::UseShortRangeGI`) is requested; the AO's consumption stays suppressed (the composite's terms are the SSDO's). Screen probe
  gather and irradiance-field gather.
- **Classic deferred lights (Astra M).** Where the SSDO owns a light's direct occlusion (`SSDODirectAmount > 0`), the per-light pass passes
  neutral screen-space AO to `GetDynamicLighting` (MegaLights passes 1), so SSAO and the SSDO cone do not both occlude it.
- **DFAO in mixed-GI families (Astra M).** `FSceneRenderer::ViewHasGIMethodSupportingDFAO` (new virtual; the deferred renderer: not Lumen /
  LumenPT): a view with an active SSDO takes DFAO's scene-colour modulation only if its own GI method supports DFAO; views without SSDO keep UE's
  family-wide decision, and every view still produces its bent normal (output indexing unchanged).
- **Orthographic views (Terra 3 / Astra M).** `SSDO::ResolveViewSettings` supports `View.IsPerspectiveProjection()` only: an orthographic view has
  no SSDO and keeps Lumen's AO. The horizontal search radius uses `P[0][0] / P[1][1]`, which equals `ViewSize.y / ViewSize.x` for UE's symmetric
  perspective projections (the game's inverse aspect ratio; old vs new frames are within the frame-to-frame noise, below).
- **Game parity of the horizon angles (Astra Low).** `ScreenSpaceDirectionalOcclusion.usf` uses Star Engine's `ACosFast` (DeferredShading.cfxb: a
  linear term times the bit-trick square root `asfloat(0x1FBD1DF5 + (asint(1 - |x|) >> 1))`) for the front / back horizon angles and
  `ACosEst2` (CIGMathLib.cfib: pi / 2 - `ASinEst`, a cubic times `sqrt(1 - |x|)`) for the projected normal, read from the installed pak
  (`out/e1/E1-7/review/game-ssdo-functions.txt`; fixture hashes `ACosFast`, `ACosEst2`, `ASinEst` added, crc32 8e895d78), in place of UE's
  `acosFast` for both; `UE::SSDO::AcosFastGame` / `AcosEst2Game` / `SampleVisibilityBits` are the C++ rule. Astra's case (front cosine 0.34, normal
  angle 0) now starts at bit 31 as in the game (30 before).
- **Unified toon diffuse.** Trivial, so done: with Substrate and `r.Substrate.Experimental.ToonUnifiedDiffuse` the direct amount is 0 (explicit
  fallback; the toon resolve restored the unscaled diffuse).
- **Project.** `sc.Look.SSDO` without a world changes only the setting (no `TActorIterator` on null); `r_ssdoQuality` must be an integer in int
  range (`FSCStarEngine::Load` refuses 0.5, which had become the supported quality 0); `.SSDO.Blend` restores and checks the developer's setting,
  the shipped default is its own test (`.SSDO.ShippedDefault`: `USCViewerSettings::bStarEngineSSDOShipped` and the shipped
  `DefaultGameUserSettings.ini`, read in isolation from the user's config).
- **Explicit fallbacks, documented (no code).** Layered Substrate materials: the producer reads the top layer's normal, so a stochastic
  MegaLights closure with a different normal is evaluated against the top-layer cone (single-normal approximation, as before for the indirect
  composite's first closure). Terrain puddles: the game returns specular occlusion 1 where `TerrainPuddle > 0`; the importer cannot identify
  that material property yet, so puddles take the SSDO's specular occlusion.
- **Tests** (`SCBridge.ViewerExposure.SSDO.LumenTrace`: ambient-only, reflection-only and both, integration on / off, GI on / off;
  `.SSDO.HorizonBits`: the engine's bits against the game source transcribed in the test over 58,290 sweep cases including every sector boundary,
  Astra's 0.34 case, `ACosEst2`; `.SSDO.ShippedDefault`; `.SSDO.Mapping` checks the three new hashes). 148 of 152 `SCBridge` with the four known failures (`out/e1/fixes3/tests-final.log`).
- **Runs**: (fork, medfrnt, E1-7's run script and poses; `out/e1/E1-7/review/`: `cases-rv.json`, `run-pre.txt` on the batch-3 engine before these
  fixes, `run-post.txt` after, `pairstats-pre.txt`, `pairstats-post.txt`, `pairstats-cross.txt`, `shots/`, `pairs.sh`). PSNR (dB) ref / long / up:

  | Pair | before (`pre`) | after (`post`) |
  |---|---|---|
  | ambient-only SSDO, integration off vs on (equal = no double darkening) | 51.5 / 50.0 / 48.5 (up 0.5 % darker with it on) | 53.3 / 54.7 / 54.2 |
  | reflection-only SSDO, integration off vs on | 51.6 / 50.9 / 48.5 | 53.0 / 52.9 / 54.2 |
  | both terms, integration off vs on | 53.0 / 52.8 / 53.1 | 53.0 / 52.6 / 53.1 |
  | reference: SSDO off, Lumen AO in the composite vs in integration | 51.4 / 49.8 / 48.1 | 52.5 / 50.6 / 48.1 |
  | SSDO on, short-range GI off vs on (differs = GI kept) | 53.5 / 53.7 / 54.6 (GI lost) | 51.5 / 49.6 / 48.5 |
  | reference: SSDO off, short-range GI off vs on | 51.6 / 49.2 / 48.7 | 51.1 / 49.7 / 48.6 |
  | SSDO on, Lumen SR-AO on vs off (interplay) | 52.3 / 52.6 / 53.5 | 54.8 / 52.5 / 53.1 |
  | effect: SSDO off vs on (E1-7: 49.8 / 47.6 / 42.9) | 49.5 / 47.7 / 42.9 | 49.5 / 47.8 / 43.0 |

  Old vs new (E1-7's `new_*` frames against `post`, four poses): off 53.4 / 52.8 / 56.5 / 53.6 dB, on 52.6 / 56.0 / 53.2 / 52.9 dB, full-frame
  means equal to 0.0007: UE's behaviour unchanged, and the game-parity arc cosines and the P[0][0] radius change nothing visible (E1-7's noise
  floor 53-54 dB). With the fixes, a partial replacement gives the same frame with integration on as off (noise level; before, the replaced
  term carried both occlusions), and short-range GI changes an SSDO view as much as a view without SSDO (before, the SSDO dropped it). The
  batch-3 engine alone (`out/e1/fixes3/ssdo-b3-pairstats.txt`) gave the same on / off effect (49.5 / 47.6 / 43.0 / 57.3) and matched E1-7's frames
  at noise level. Astra (`out/e1/E1-7/review/astra/off-vs-on.md`, gpt-6.2-astra, high; long and up, off vs on): long "pass, subtle", up "pass, more
  pronounced": contact occlusion under the ledge, in the grille slats and the beam channel, "no obvious crushed crevices, silhouette halos, added
  grain, or visible smearing".
- **UE default.** Views without SSDO: `RunsLumenShortRangeTrace` and `AppliesLumenShortRangeAODuringIntegration` return UE's decisions,
  `GetScreenProbeIrradianceFormat(View)` is UE's format, the deferred light's AO input and DFAO's modulation are UE's.
- **Further low findings (not fixed, loop end).** The game's `ACosFast` bit trick is undefined outside [-1, 1]; the port clamps (identical inside). With
  `ApplyDuringIntegration 1` a partially replaced view takes UE's composite path, whose AO differs from integration-applied AO at noise to
  1-2 dB level (the reference row) — inherent to that path. Temporal behaviour across an SSDO volume boundary and multi-view / orthographic
  captures were not run (stills; orthographic by the support predicate).

#### Review fixes 4 (E1-1..E1-7, last batch)
Engine `0fa9c692a` (`SCViewer: review fixes 4`; Terra `out/e1/fixes3/terra-review.md`, Astra `out/e1/fixes3/astra-code-review.md`; the
coordinator's triage): `SCVIEWER_ENGINE_TONE_CURVE` 5, `SCVIEWER_ENGINE_EXPOSURE` 7, `SCVIEWER_ENGINE_NIAGARA_STATELESS` 7, `SCVIEWER_ENGINE_SSDO` 3.
1. **HIGH, unproduced short-range GI with hardware ray traced AO.** `ComputeScreenSpaceShortRangeAO` publishes `ShortRangeGITexture` only where
   the screen-space pass wrote it (the hardware AO branch writes none; UE published it whenever `r.Lumen.ScreenProbeGather.ShortRangeGI` was
   set, a UE defect the E1-7 fix newly reached). And with `r.Lumen.ScreenProbeGather.ShortRangeAO.HardwareRayTracing` in effect the SSDO does
   not take over Lumen's AO: its ambient and reflection amounts are 0 there (explicit fallback; Lumen's AO path, trace and downsample stay
   UE's), `UE::SSDO::ApplyRendererRestrictions`.
2. **Resolution.** A view's short-range AO downsample follows its own integration decision (`LumenShortRangeAO::GetDownsampleFactor(bool)` with
   `SSDO::AppliesLumenShortRangeAODuringIntegration`: the trace, the temporal permutation, the upsample buffer size and the downsampled depth /
   normal request), so a partially replaced view with temporal AO off gets full-resolution AO for the composite. The composite samples the GI
   texture at its own resolution (`FLumenScreenSpaceBentNormalParameters::ShortRangeGIDownsampleFactor`: the trace's factor, 1 after the screen
   probe gather's temporal history); the irradiance field gather's GI is the downsampled trace output (also UE's path with short-range GI).
3. **Exposure fallback.** A family view in a per-view Reflected fallback carries the reason to the final input (`UE::AutoExposure::EMeterFallback`:
   `FamilyRejected` from the all-view check, `CollapsedRect` from `IsAutoExposureMeterCollapsedFallback`) and histograms its own scene colour
   slice with the `SceneColor` permutation, never the `bEyeAdaptationAllViewPixels` union nor a shared illuminance texel
   (`UE::AutoExposure::ChooseHistogramInput`, which also states UE's choices for every other view).
4. **Niagara collections.** The cold lookup queries subclasses of `UNiagaraParameterCollection`, and a name unresolved while the asset registry
   is still discovering keeps the previously cached references (no emptier list is saved before discovery has completed).
5. **A13.** `bOverriddenNoneAssetsClear_DEPRECATED` is a byte (a byte property takes the old bool tag as 0 / 1) initialised to
   `OverriddenNoneAssetsClearLegacyAbsent`; `PostSerialize` migrates on the tag's presence, false to 0 and true to `All`, so an instance's saved
   false over an archetype already migrated to `All` clears. **Cooked data:** the migration is editor data (`PostSerialize` is editor-only);
   packages cooked by earlier fork builds are not migrated at run time and must be recooked from migrated source assets (the fork is
   unreleased; no such cooked data ships).
6. **SSDO enabled flag.** `bEnabled` is recomputed after the renderer's restrictions (unified toon diffuse, hardware ray traced AO): a remaining
   amount or a debug view still runs the pass, otherwise the view runs no SSDO (and no longer withholds DFAO's modulation).
- **Repo lows.** `r_ssdoQuality` uses explicit signed int32 bounds; the horizon-mask rule rounds the span ties-to-even like HLSL `round()`
  (`UE::SSDO::SectorBits`; the oracle in `.SSDO.HorizonBits` too, with the 2.5 / 3.5 sector cases).
- **Tests**: `Automation RunTests SCBridge` 150 of 154 (`out/e1/fixes4/tests.log`; the four known failures). New: `.ViewerExposure.ReviewRules4`
  (a fallback view, either reason, with or without ignore-materials and all-view-pixels: own slice and `SceneColor`; UE's four choices
  unchanged), `.ViewerExposure.SSDO.Restrictions` (direct-only under toon: disabled; debug still runs; hardware AO: no indirect term),
  `.LegacyClearMigration` rewritten on tagged data in the old layout (`FSCLegacyOverriddenNoneClear`, a BoolProperty tag): saved true -> All, an
  instance's saved false delta-loaded over the migrated archetype -> 0, no tag -> inherited; `.SSDO.HorizonBits` with the 2.5 / 3.5 spans.
- **Runs**: (medfrnt; `out/e1/fixes4/`: `rv4.txt`, `steps-rv4.txt`, `run-post2.txt`, `pairstats-post2.txt`, `run-rf4.txt`, `pairstats-rf4.txt`,
  `run-hw2.txt`, `pairstats-hw2.txt`, `cases-*.json`, `shots/`; logs `E1-5-rv4.log`, `E1-7-post2.log`, `E1-7-rf4.log`, `E1-7-hw2.log`).
  - Exposure against `rv3`: 27 steady cases within 0.0002 stops; steps as before (upper +3: t50 1.49 s, t90 2.63 s, +1.231; -3: t50 0.16 s;
    release within 0.1 s); no error lines.
  - SSDO pairs against `out/e1/E1-7/review/pairstats-post.txt` (ref / long / up, dB): effect 49.8 / 47.6 / 42.9 (49.5 / 47.8 / 43.0);
    ambient-only integration off vs on 52.9 / 52.9 / 53.7; reflection-only 53.0 / 52.9 / 53.6; short-range GI with the SSDO on 51.3 / 49.9 /
    48.5 (SSDO off 51.2 / 49.6 / 49.1); unchanged.
  - Downsample 2, temporal AO off, integrate downsample 2 (`r.Lumen.ScreenProbeGather.ShortRangeAO.DownsampleFactor 2`, `.Temporal 0`,
    `IntegrateDownsampleFactor 2`): ambient-only integration off vs on 55.1 / 53.0 / 53.5 dB, reflection-only 53.3 / 53.0 / 53.7 dB (noise:
    the partially replaced view takes its own full-resolution AO), full-frame means within 0.02 %; Lumen alone 48.3 / 45.9 / 41.4 dB (its own
    integration path, the reference).
  - Irradiance field gather (`r.Lumen.FinalGatherMethod 0`), short-range GI on vs off with both SSDO terms: 52.1 / 49.6 / 49.8 dB, means
    -0.04 / +0.15 / +0.32 %; with the SSDO off 51.4 / 49.9 / 49.8 dB, -0.07 / +0.05 / +0.42 %: the GI arrives at its own resolution like
    Lumen's own.
  - Hardware ray traced AO (`ShortRangeAO_HWRT` pass in `ProfileGPU`), `r.RDG.Validation 1`, short-range GI on, both SSDO terms on: no
    error, ensure or RDG validation message; GI cvar on vs off 52.3 / 54.2 / 53.8 dB (no GI texture is produced, so none is read); SSDO
    off vs on 52.7 / 52.3 / 53.6 dB (direct SSDO only, the explicit fallback). The first run of this case raised one development ensure,
    a composite permutation not requested for PSO precaching (the non-GI permutation with the GI cvar on); the precache rule was widened
    and the second run (`hw2`) is clean.
  - FX: `-VerifyContract` `SC_FX_CONTRACT systems=7 stale=0 load_failures=0 version=3`.
- **Further low findings (not fixed).** `RenderHardwareRayTracingShortRangeAO` still reads the global downsample factor (consistent only because the
  hardware path keeps UE's decisions, item 1). The composite point-samples a downsampled GI texture (no bilateral upsample; UE's irradiance
  field path did not upsample it either). A collection referenced for the first time while the registry is still discovering has no earlier
  reference to keep; the cook (after discovery) resolves it. With `r.Lumen.ScreenProbeGather.ShortRangeAO.HardwareRayTracing` in effect (the
  project's Epic scalability default for scenes whose ray-tracing load allows it, `SCLook::UpdateShortRangeAO`), `sc.Look.SSDO game` applies
  only the direct term.

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
| E1-2 | `362370d99`, review fixes `dd77a0317` (E1-3 section) | importer: range fade (all local lights) + bulb offset law (authored point / spot), no `SoftSourceRadius`; probe; `sc.Lights.FalloffModels`; tests | engine + project build, 11/11 tests, medfrnt run with UE-default A/B (Result (E1-2)) |
| E1-3 | `20605cb76` (+ `dd77a0317` E1-2 review fixes), review fixes `4470238ea` | importer `MakeProjector` (SquareFrustum, half-FOV, Border; no corner cone / lateral scale), `M_SC_LightFunction` without border node (regenerated), square-aware probe, `sc.Lights.ProjectorShape 0\|1\|2`, tests | engine + project build, tests first, 12/12 tests, medfrnt MegaLights + deferred runs, Astra (Result (E1-3)); review fixes: tests first, 17/17, medfrnt rerun unchanged (E1-3 review fixes) |
| E1-4 | `3079e8bc3`, review fixes `c1e0610d4`; E1-1..E1-4 cumulative review fixes `9e2a37dd8`; review fixes 3 `1840a16a1`; review fixes 4 `0fa9c692a` | `FSCDisplayCurve` (curve + fidelity baseline on the viewer volume), `sc.Look.Display` switches the method, `M_SC_DisplayMapping` + its script + `KEEP` entry removed, hash guard moved to a test, notes / README; review: `bOverriddenNoneAssetsClear` in the baseline, tests `.BaselineBlend`, `.ToneCurveDomain` | engine + project build, 17/17 tests, LUT vs analytic at 32³ / 64³ / 128³, medfrnt runs material vs engine vs UE Filmic, Astra (Result (E1-4)); review fixes: 21/21 tests, MegaLights overflow run (E1-4 review fixes, Result (E1-5)); cumulative fixes (12 items, incl. probe and per-world A/B): tests first, 135/139 `SCBridge` (the 4 outside these features), medfrnt runs: E1-5 exposures and regions to 0.0001, border when faded, non-power-of-two atlas (E1-1..E1-4 cumulative review fixes); review fixes 3 (T1 UE planar rect projection restored in PT / RT / Lumen / mobile, A13 saved-clear migration, A16 probe): tests first, 148/152 `SCBridge` (E1-1..E1-4 review fixes 3); review fixes 4 (A13 presence-based migration, recook note): tagged delta-load test, 150/154 (Review fixes 4) |
| E1-5 | `cfe9d5032`, `cfc636921`, `03d211bcb`; review fixes 3 `9303fe332`; review fixes 4 `0fa9c692a` | fixture (+4 values, +3 hashes), `FSCExposureSettings` (game meter + two-stage adaptation, no LUT cvars, API instead of the copied rule), `DefaultEngine.ini` LUT domain policy, `sc.Look.Exposure`, `sc.Look.ExposureLog`, tests, notes / README | engine + project build, 21/21 tests, medfrnt runs before / UE / game with GPU cost, adaptation step vs the game model, LUT stale-domain run, Astra (Result (E1-5)); review fixes `0d847e519` (Terra T1–T7, Astra A1–A18; `sc.Look.Exposure ue` overrides, scoped cvar priority, fixture provenance): 25/25 tests, medfrnt before / after runs (E1-5 review fixes); review fixes 2 `3808c1160` (LUT task race, snapshot after view extensions, CPU-curve limits, incident weight / FP32 / unfloored, multi-view fallbacks, LUT policy API; trace without pre-exposure, isolated policy test): 137/141 `SCBridge`, E1-5 steady state and steps within 0.0005 stops (E1-5 review fixes 2); review fixes 3 (T2 / A10 incident producers and exact tiling, A14 collapsed rects, T6 UE rects, A11 histogram source, A12 ordered limits, A15 double LUT scale): tests first (`.ReviewRules3`), 148/152, steady state and steps within 0.0004 stops of `rv2` (E1-5 review fixes 3); review fixes 4 (per-view Reflected fallback input): rule test, 150/154, within 0.0002 stops of `rv3` (Review fixes 4) |
| E1-6 | `ad5c402d9`, `05559963d`, `9f382da2b`, `c7a0e82f7`; review fixes 3 `41df91ebe`; review fixes 4 `0fa9c692a` | SCFxBuild: loop guard from the engine constant (`SCFxRules.h`), typed expressions, beam spawn-rate and count expressions, clamp-sum strength curves, owner-scale sprite sizes, `-LoopCapacity`; runtime contract reduced (`SCFxStrength.h`, `SCFxBeam.h`, cargo contacts, `sc.Fx.Spawn`); `bTreatAsEngineModule` kept (boundary); tests `SCBridge.Fx.*`; WP-K4 contract note | engine + project build, FX build errors=0, 25/25 tests, medfrnt overflow runs at 8 / 32, GPU cost, before / after frames, Astra (Result (E1-6)); review fixes `e2ec17572` (effective limit cvar, default 8, 32 in `DefaultEngine.ini`; expression validation; expression-aware filtering; collections; rate residual) and the builder / contract fixes (`SCFxContract.h`, `-VerifyContract`, package check): tests first, 137/141, FX build errors=0, verify stale=0, 0 overflows, PSNR as E1-6 (E1-6 review fixes); review fixes 3 (A1 captured limit, A2 runtime validation, A3 burst predicate, A4 UE timing, A5 collections, A8 counter, GreaterThan; builder T3-T5, A8, A9, contract 3): tests first, 148/152, FX build errors=0, verify stale=0 load_failures=0, 0 overflows, PSNR as before (E1-6 review fixes 3); review fixes 4 (collection lookup subclasses / discovery): 150/154, verify stale=0 (Review fixes 4) |
| E1-7 | `6f5b30b63` (supersedes `b65d09ad1`, `9cad77abb`); review fixes `500fca6c0`; review fixes 4 `0fa9c692a` | fixture (+`DeriveSpecularOcclusion`), `FSCStarEngine` SSDO values, `FSCSSDOSettings`, `bStarEngineSSDO` (off, D15), `sc.Look.SSDO`, tests `SCBridge.ViewerExposure.SSDO.*`; `ue/EnginePatches`, `engine_patches.py`, `r.GeneralPurposeTweak` removed (routes, docs, AGENTS, READMEs) | engine + project build, 141/145 `SCBridge` (the 4 known), medfrnt old patch vs new feature off / on, SR-AO interplay, deferred-light run, GPU cost, Astra (Result (E1-7)); review fixes (partial replacement, short-range GI, deferred SSAO, DFAO, orthographic, game arc cosines, toon fallback; project null world, integral quality, shipped-default test): 148/152, pre / post interplay and GI runs, old vs new at noise, Astra pass (E1-7 review fixes); review fixes 4 (GI only where written, HWRT fallback, per-view AO downsample, GI resolution, bEnabled, ties-to-even): 150/154, post pairs unchanged, downsample-2 / irradiance-field / HWRT + RDG validation runs (Review fixes 4) |
