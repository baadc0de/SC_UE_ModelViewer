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
| E1-2 | `362370d99`, review fixes `dd77a0317` (E1-3 section) | importer: range fade (all local lights) + bulb offset law (authored point / spot), no `SoftSourceRadius`; probe; `sc.Lights.FalloffModels`; tests | engine + project build, 11/11 tests, medfrnt run with UE-default A/B (Result (E1-2)) |
| E1-3 | `20605cb76` (+ `dd77a0317` E1-2 review fixes), review fixes `4470238ea` | importer `MakeProjector` (SquareFrustum, half-FOV, Border; no corner cone / lateral scale), `M_SC_LightFunction` without border node (regenerated), square-aware probe, `sc.Lights.ProjectorShape 0\|1\|2`, tests | engine + project build, tests first, 12/12 tests, medfrnt MegaLights + deferred runs, Astra (Result (E1-3)); review fixes: tests first, 17/17, medfrnt rerun unchanged (E1-3 review fixes) |
| E1-4 | `3079e8bc3` | `FSCDisplayCurve` (curve + fidelity baseline on the viewer volume), `sc.Look.Display` switches the method, `M_SC_DisplayMapping` + its script + `KEEP` entry removed, hash guard moved to a test, notes / README | engine + project build, 17/17 tests, LUT vs analytic at 32³ / 64³ / 128³, medfrnt runs material vs engine vs UE Filmic, Astra (Result (E1-4)) |
| E1-5 | — | — | — |
| E1-6 | — | — | — |
| E1-7 | — | — | — |
