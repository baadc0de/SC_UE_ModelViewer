# WP-U3b verification — many-light rendering (MegaLights) and light intensity / exposure

Date 2026-11-09. UE 5.8.2 (CL 56702186), Editor `-game` host (D3), Vulkan SM6, RTX 4090,
superproject `58f44c6` plus the uncommitted WP-U3 fix round in the working tree. Packages:
`out/dec-cutlass_black` (225 lights), `out/dec-scorpius` (127), `out/dec-r5` (Gladius, 77 incl.
one skipped ambient proxy). No C++ was edited, no cargo was run; nothing was committed.

## Decision

**Enable MegaLights for the project** (`r.MegaLights.EnableForProject=1`), keep the authored
candela proxy as the UE intensity, and **switch the viewer to auto exposure** (EV100 units, bias 0).
With MegaLights every local light keeps a ray-traced shadow, the VSM one-pass overflow is gone,
and the Cutlass bay costs **7.0 ms GPU at 1600×900 (≈140 fps) and 16.9 ms at 3840×2160
(≈59 fps)** against 10.0 / 28.8 ms on the current path. Gobos need a larger light function atlas
(set) and one material per texture (importer diff below). The data-derived shadow cap stays as
the fallback for hosts without hardware ray tracing (same diff).

> **2026-11-10:** intensity, emissive strength, exposure and the display curve now follow the installed
> game's renderer (§13). The candela proxy and UE's default auto exposure described here are the record of
> the 2026-11-09 build.

## 1. MegaLights in the installed 5.8.2 (engine source, CL 56702186)

| Question | Fact | Reference (`~/UnrealEngine/5.8/Engine/…`) |
| --- | --- | --- |
| Status | Still labelled **"Experimental feature"** in the project-setting tooltip and CVar help; off by default (`r.MegaLights.EnableForProject` default 0). Post-process volumes override it (`bMegaLights`); per light `bAllowMegaLights` (default **true**). | `Source/Runtime/Renderer/Private/MegaLights/MegaLights.cpp:27`, `Source/Runtime/Engine/Classes/Engine/RendererSettings.h:651`, `Engine/Classes/Engine/Scene.h:1881`, `Engine/Private/Components/LightComponent.cpp:474` |
| Read at runtime | `FinalPostProcessSettings.bMegaLights` is refreshed from the CVar every view setup, so it can be toggled from the console. | `Engine/Private/SceneView.cpp:2112` |
| Shader platform | `ShouldCompileMegaLightsShaders`: not mobile, `r.MegaLights.Supported` (default true), **SM6 + wave ops + ray tracing**. VULKAN_SM6: `bSupportsRayTracing`, `bSupportsInlineRayTracing`, `bSupportsRayTracingShaders`, `bSupportsWaveOperations=RuntimeGuaranteed`. | `RenderCore/Private/RenderUtils.cpp:2638`, `Config/VulkanPC/DataDrivenPlatformInfo.ini` `[ShaderPlatform VULKAN_SM6]` |
| Tracing requirement | `IsEnabled = IsRequested && (HWRT || SWRT)`. HWRT needs `r.RayTracing` enabled, `r.MegaLights.HardwareRayTracing` (1), and RT shaders **or** inline RT with `r.MegaLights.HardwareRayTracing.Inline` (1); otherwise an on-screen "no ray tracing data" warning. SWRT fallback = distance fields (`r.MegaLights.SoftwareRayTracing.Allow`). | `MegaLights.cpp:546`, `MegaLightsRayTracing.cpp:361-382`, `MegaLights.cpp:2605` (WriteWarnings) |
| Light types | Point, spot, rect handled; **directional not** unless `r.MegaLights.DirectionalLights=1` (default 0, needs `r.Forward.LightBuffer.Mode>0`). The Viewer sun stays on the classic VSM path. | `MegaLights.cpp:239`, `GetMegaLightsMode` `MegaLights.cpp:645` |
| Shadows | Per light `MegaLightsShadowMethod`: Default → `r.MegaLights.DefaultShadowMethod` (0 = **ray tracing**, fixed cost) or VSM (per-light cost). With MegaLights on, deferred shading, shadow maps, RT shadows and DF shadows are no longer used for those lights. | `MegaLights.cpp:374`, `LightComponent.h:167-179`, `RendererSettings.h:652` |
| Light functions / gobos | Supported (`r.MegaLights.LightFunctions`=1) through the **light function atlas** only: one slot per *material* (`LFMaterialUniqueID`), `r.LightFunctionAtlas.Size`=4 → **16 slots**, 128 px each; lights beyond the slots lose their light function (an out-of-slot screen message exists, `SceneRendering.cpp:4766`, but did not show in our captures). Material must be atlas-compatible (UV-only; `M_SC_LightFunction` is). The deferred path also samples the atlas (`r.Deferred.UsesLightFunctionAtlas`=1). | `MegaLights.cpp:204,1621`, `LightFunctionAtlas.cpp:31,62-77,423-515`, `HLSLMaterialTranslator.cpp:1769` |
| IES / rect textures | Supported (`r.MegaLights.IESProfiles`, `r.MegaLights.TexturedRectLights`, both 1). | `MegaLights.cpp:197,218` |
| Translucency, fog | Translucency lighting volume + volumetric fog via `r.MegaLights.Volume`/`TranslucencyVolume` (1); a dedicated front-layer translucency pass (`r.MegaLights.FrontLayerTranslucency.EnableForProject`=1) is allowed from High shadow scalability up (`ShadowQuality@2`, `@3`, `@Cine`: `FrontLayerTranslucency.Allow=1`). | `MegaLights.cpp:246,322,384-392`, `Config/BaseScalability.ini:232,270,308` |
| Substrate | Supported: sampling/resolve have Substrate tile permutations up to `SHADING_MAX_SUBSTRATE_ADAPTIVE` (our adaptive GBuffer). | `MegaLightsSampling.cpp:92,182`, `MegaLightsResolve.cpp:127,236` |
| Budget | No per-pixel light limit: stochastic sampling, `r.MegaLights.NumSamplesPerPixel` 4 at High and above (2 at Low/Medium, `BaseScalability.ini:156-305`), half-res tracing (`DownsampleMode`=2), temporal + spatial denoiser. Classic path limit for comparison: VSM one-pass projection `r.Shadow.Virtual.OnePassProjection.MaxLightsPerPixel`=16 — the source of the overflow warning. | `MegaLights.cpp:51-66`, `MegaLightsDenoising.cpp`, `VirtualShadowMaps/VirtualShadowMapArray.cpp:483` |
| Spot cone | UE clamps cones to 88.9°: authored 90° half-angle projectors (180° FOV) render as 177.8° cones. | `Engine/Private/Components/SpotLightComponent.cpp:38` |

## 2. Measurement method

`ue/scripts/measure_lights.sh <package> <tag> [host args]` (new): runs `run_import.sh … -vulkan
-SCStay` with a boot CSV capture (`-csvGpuStats`), waits for `IMPORT_OK` and idle shader
workers, settles 20 s, screenshots the window via niri, samples the following frames, stops UE by
PID and reports median/p90 `GPUTime` and the top `GPU/<pass>` timings from the CSV. Console
state per run through `SC_EXEC` (e.g. `r.MegaLights.EnableForProject 1`). The niri tile gives a
1695×953 viewport; `r.ScreenPercentage 94.4` renders **1601×900** and `226.5` renders
**3839×2159** (TSR, Epic scalability, `r.DynamicRes.OperationMode 0`). Camera: Cutlass bay
`-SCView=180,2,0.25`, Cutlass forward `-SCView=0,2,0.12`, Scorpius default exterior, Gladius
top-down `-SCView=0,89,1.3`.

The first matrix ran while another game used ~50 % of the GPU; those samples were noisy
(e.g. Cutlass current path 19.7 ms vs 10.0 ms clean) and are discarded. Every number below comes
from a run logged with no other GPU application (`nvidia-smi` checked per run).

## 3. Frame times (GPU, median of the steady-state window; p90 within ±0.6 ms unless noted)

| Ship / view | Configuration | 1601×900 | 3839×2159 | Light-related passes (900p → 4K) |
| --- | --- | ---: | ---: | --- |
| Cutlass bay (153 lights in the view family) | (a) current: deferred + VSM, every light shadowed | 9.97 | 28.82 | Lights 3.16 → 17.34, ShadowDepths 1.57 → 2.00; VSM overflow warning every frame |
| | (b) MegaLights, light functions on | 7.15 | 16.91 | MegaLights 0.85 → 4.57, ShadowDepths 0.92 → 1.22 (sun VSM only) |
| | (c) MegaLights, `r.MegaLights.LightFunctions 0` | 7.20 | — ¹ | MegaLights 0.85 |
| | (d) current path, `ShowFlag.DynamicShadows 0` (lower bound of any shadow cap) | 6.68 | — | Lights 1.69 |
| | **final ini** (b + auto exposure + atlas 16) | **7.01** | **16.86** | MegaLights 0.84 → 4.28 |
| | final ini with `r.MegaLights.EnableForProject 0` | 9.97 | — | Lights 3.31, ShadowDepths 1.55 |
| Cutlass forward | current path + auto exposure | 10.09 | — | |
| | **final ini** | **6.83** | — | MegaLights 0.82 |
| Scorpius exterior (128) | (a) current | 7.38 | 14.52 | ShadowDepths 1.35, Lights 1.20 → 4.99 |
| | (b) MegaLights | 6.58 | 11.89 | MegaLights 0.57 → 1.61 |
| | (c) MegaLights, no light functions | 6.59 | — | MegaLights 0.57 |
| | (d) no dynamic shadows | 5.21 | — | Lights 0.65 |
| | **final ini** | **6.53** | **11.89** | MegaLights 0.55 → 1.64 |
| Gladius top-down (76) | current | 7.05 | — | ShadowDepths 1.53, Lights 0.75 |
| | **final ini** | **6.48** | **13.56** | MegaLights 0.62 |

¹ The only 4K sample of (c) overlapped the other GPU workload (29.2 ms) and is not used.

Findings: MegaLights' cost is ~0.6–0.9 ms at 900p and ~1.6–4.6 ms at 4K, flat in the number of
lights; on the Cutlass it is within 0.5 ms of rendering **no** local-light shadows at all (d).
The classic path's `Lights` pass grows ~5.5× from 900p to 4K (17.3 ms). Light functions cost
nothing measurable under MegaLights (b vs c). MegaLights' denoiser is included in its pass time.

## 4. Correctness

- **Leaks.** Cutlass exterior with the sun, sky light and atmosphere switched off
  (`ShowFlag.DirectionalLights 0, SkyLighting 0, Atmosphere 0`) shows only the exterior fixtures
  on the hull, identical between the VSM path and MegaLights — none of the 122 interior lights
  shows through the hull (`leakcheck-cutlass-nosun-*.jpg`). Bay and forward interiors show the
  same shadow structure under both paths (`cutlass-bay-*`, `cutlass-forward-*`). Ray-traced
  shadows depend on the WP-U1 ray-tracing geometry; no missing-occluder artefact was seen.
- **Gobos.** All 184 Cutlass spots (57 Scorpius, 47 Gladius) carry a projector texture, 6–10
  unique per ship, all 128×128 (so the atlas' 128 px slot matches the data). The importer creates
  one MID per light, the atlas keys slots by material, and the default atlas holds 16: the rest
  silently lost their gobo in **both** paths (the deferred path samples the same atlas). With
  `r.LightFunctionAtlas.Size 16` the projected patterns appear (back wall, door) and the image
  differs by a mean 5.3/255 (4.2 % of pixels > 20/255): `cutlass-gobo-atlas4-top-atlas16-bottom.jpg`.
  Cost: 7.16 vs 7.13 ms. The importer diff shares one MID per texture so even the default atlas
  would suffice; the ini setting covers the current importer.
- **Emissive + Lumen.** MegaLights replaces direct lighting only; Lumen GI/reflections and
  emissive materials are unchanged (identical `LumenSceneUpdate`/`LumenReflections` costs).
  WP-U2's `GlowToEmissive` (10 units per Glow) was tuned to the old fixed exposure; under auto
  exposure emissives read 0.6–1 stop brighter on sun-lit exteriors and 1.3 stops dimmer in the Cutlass bay.
  (Superseded by §13.3: emissive strength is cd/m² from the game's glow rule; `GlowToEmissive` is gone.)
- **Spot cones.** Authored 90° half-angles (180° projector FOV) are clamped by UE to 88.9°.
- **Rect lights.** `Planar` lights → rect lights at the UE default 64×64 cm source size; the
  emitter size (`planeWidth`/`planeHeight`) is not exported.
- **Gladius reference.** Hull, livery and decals are unchanged by MegaLights (sun-lit; compare
  `gladius-top-before.jpg` and `final-gladius-*.jpg` with `reference/gladius/*.webp`). The
  in-game `top-hangar.webp` shows no pools from the ship's own lights on the hangar floor, while
  the viewer shows red/blue pools from the authored wing Omnis (raw 1.0 → 1500 cd, 5–7 m radius)
  and blue projectors (300 cd, 10–14 m). That reference cannot calibrate absolute intensity:
  the parked ship's light state and the hangar's own lighting are unknown. It is a
  state/environment question (TODO below), not a per-ship scale.

## 5. Light intensity mapping and exposure (coordinator addendum)

**Authored data.** Each light state carries `intensity_raw` (`intensity_unit =
cryengine_authored_intensity`, a unitless state intensity). The exporter derives
`intensity_candela_proxy = intensity_raw × 1500` (`LIGHT_AUTHORED_INTENSITY_SCALE`,
`vendor/StarBreaker/crates/starbreaker-3d/src/socpak.rs:24,110`, "aligned with the Max script's
PowerModifier"); ambient proxies additionally × `glowMultiplier` × 0.25 (`:114-122`). Colour is
already the temperature colour when `useTemperature` is set. The lighting research
(`docs/star-citizen-lighting-research.md`) establishes semantic classes (ambient proxies are not
punctual lights), that `intensity` is not globally comparable across light *types*, and that
`lightRadius` is attenuation, not emitter size. It does **not** establish a physical unit;
CryEngine's light intensity is an engine-relative HDR multiplier.

**Blender reference** (`blender_addon/.../runtime/importer/utils.py:380-407`, `constants.py:136-154`):
point/spot `W = cd_proxy × 4π/683 × 20` (`LIGHT_VISUAL_GAIN` 20); ambient proxies without the
gain; area `W = intensity_raw / 120`; `cutoff_distance = radius`; soft size `0.05 × radius`
clamped to [0.01, 0.5] m; no exposure setting (Blender exposure 0). Blender displays radiance
in W·m⁻²·sr⁻¹ and the glTF convention maps 683 lm to 1 W, so for point/spot lights the addon is
exactly "candela = cd_proxy, scene-linear = luminance × 20/683": a **uniform** gain, i.e. an
exposure (EV100 ≈ 5.1 in UE's 1 cd/m² → 1.0 convention). A uniform gain carries no per-light
information; what the addon validates is the *relative* mapping (all point/spot ∝ raw × 1500,
ambient proxies separate), which the importer already applies. The area-light branch (lumens/120,
no gain, panel size = attenuation radius) contradicts the research doc's radius rule and is
16 500× dimmer than the point/spot branch for the same raw value; swapping it in changed nothing
visible on the Scorpius (4 wing panels, raw 6.0; `scorpius-area-lights-blender-units.jpg` vs
`final-scorpius.jpg`), so there is no evidence for a separate area unit.

**Mapping (kept for now; superseded by §11 once the bulb radius is exported).** UE candela = `intensity_raw × 1500` = `intensity_candela_proxy`
(`ELightUnits::Candelas`, inverse-square, attenuation radius = `radius_m`); ambient proxies
stay skipped (Lumen provides the indirect fill they stood in for); area lights use the same
candela proxy. No per-ship or per-type constant.

**Why everything was white: exposure, not intensity.** The project had
`r.DefaultFeature.AutoExposure=False`, which pins min = max white point 1 with the default bias
+1 (exposure scale 2, EV100 ≈ −1 equivalent), tuned for the Viewer map's 5 lux sun. Measured
auto-exposure picks (`ShowFlag.VisualizeHDR`, extended range, bias 0): Gladius top-down
**EV100 −1.56**, Scorpius exterior **−2.01**, Cutlass bay **+0.32**. Exteriors stay where the
sun-tuned fixed exposure was (within ~0.5–1 stop); the bay drops 1.3 stops and reads as a lit
bay rather than white (`cutlass-bay-autoexp-hdr.jpg`, `final-cutlass-*.jpg`). The Blender-parity
fixed EV100 ≈ 5.1 would render the 5 lux-sun exteriors black, and a fixed exposure cannot serve
both a 5 lux-sun exterior and ship interiors; a "fixed EV derived from the sun" would only move
the problem.

**Exposure policy.** Auto exposure (histogram), EV100 units, bias 0 — set in `DefaultEngine.ini`.
A bounded range needs a post-process volume (no CVar sets min/max): proposed for the Viewer map
(`ue/scripts/create_viewer.py`, unbound PPV) `AutoExposureMinBrightness=-3`, `MaxBrightness=6`
EV100: 1 stop below the darkest measured exterior, and above the Blender-parity interior EV 5.1
so that brightly lit interiors are never exposed brighter than the validated Blender look.
Until then the engine defaults (−10…20) apply.

## 6. Data-derived fallback (no MegaLights)

Rule: within one scene instance (root entity or interior container — the unit `Build` sees), a
local light casts shadows iff its authored `radius_m` is ≥ the instance's upper quartile of
`radius_m` (ambient proxies excluded). Small-radius fixtures only light their own housing, so
their shadows are invisible; large-radius lights are the ones whose shadows define the space.
Applied only when MegaLights is not active (`r.MegaLights.EnableForProject` off or no HWRT).
Resulting shadowed lights: Cutlass 60 of 225 (16/18/26 per instance, p75 radius 3.0/5.8/6.0 m),
Scorpius 36 of 127 (p75 2.0/8.3 m), Gladius 22 of 76. Measured bounds on the Cutlass bay:
all shadowed 9.97 ms, none 6.68 ms; the rule's cost lies between (not measured: it needs the
importer change). The authored per-light shadow flag (`shadowParams`) is not exported; it would
replace this rule (TODO).

## 7. Project settings (`ue/SCViewer/Config/DefaultEngine.ini`)

```ini
r.DefaultFeature.AutoExposure=True                               ; was False
r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange=True
r.DefaultFeature.AutoExposure.Bias=0
r.MegaLights.EnableForProject=1
r.LightFunctionAtlas.Size=16
```

Each has a comment pointing here. The "final ini" rows above ran with this file and no
overrides. MegaLights' shaders were already compiled (`r.MegaLights.Supported` defaults true);
no additional shader compile wait was observed. DDC 1.3 GiB, Intermediate 423 MiB.

## 8. Proposed `SCLightImporter.cpp` patch (for the orchestrator; not applied, not compiled)

Against the working-tree file at the time of writing (md5 `b917bddabe44340d0e162958b2e6cf3c`,
WP-U3 fix round in progress). MegaLights needs no per-light flags (`bAllowMegaLights` defaults
true, shadow method defaults to ray tracing); the patch adds the gobo-material sharing and the
fallback shadow rule. `RenderCore` is already a dependency.

```diff
--- a/ue/Plugins/SCBridge/Source/SCBridge/Private/SCLightImporter.cpp
+++ b/ue/Plugins/SCBridge/Source/SCBridge/Private/SCLightImporter.cpp
@@ -17,6 +17,8 @@
 #include "IImageWrapper.h"
 #include "Modules/ModuleManager.h"
 #include "Misc/Paths.h"
+#include "HAL/IConsoleManager.h"
+#include "RenderUtils.h"
 namespace
 {
 using FLightJson = TSharedPtr<FJsonObject>;
@@ -47,11 +49,53 @@
 {
     return FVector(V.Y, V.X, V.Z);
 }
+// WP-U3b (ue/verification/WP-U3b.md): MegaLights shades every local light with ray-traced shadows at a fixed
+// cost. Without it (no HWRT, or the project setting off) each shadowed light costs a virtual shadow map and
+// one-pass projection overflows above 16 lights per pixel, so shadow casting is capped by a data rule.
+bool MegaLightsActive()
+{
+    static const auto *Project = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.MegaLights.EnableForProject"));
+    return Project && Project->GetValueOnGameThread() != 0 && IsRayTracingEnabled();
+}
+// Fallback rule: within one scene instance (root entity or interior container) only the lights whose authored
+// attenuation radius is in the upper quartile cast shadows; small-radius fixtures only light their housing.
+double ShadowRadiusThreshold(const TArray<FString> &LightJson)
+{
+    TArray<double> Radii;
+    for (const auto &Text : LightJson)
+    {
+        FLightJson J;
+        if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) && J &&
+            LightString(J, TEXT("semantic_light_kind")) != TEXT("ambient_proxy"))
+            Radii.Add(FMath::Max(0., LightNumber(J, TEXT("radius_m"))));
+    }
+    if (Radii.IsEmpty()) return 0;
+    Radii.Sort();
+    return Radii[(3 * Radii.Num()) / 4];
+}
+// One light-function MID per gobo texture: the light function atlas (deferred lighting, MegaLights, Lumen)
+// de-duplicates slots by material and holds r.LightFunctionAtlas.Size^2 materials; a MID per light exhausts it.
+UMaterialInstanceDynamic *GoboMaterial(ASCVehicle *Vehicle, UMaterialInterface *Master, UTexture2D *Texture)
+{
+    for (UMaterialInstanceDynamic *Existing : Vehicle->Materials)
+    {
+        UTexture *Bound = nullptr;
+        if (Existing && Existing->Parent == Master &&
+            Existing->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Gobo")), Bound, true) && Bound == Texture)
+            return Existing;
+    }
+    auto *MID = UMaterialInstanceDynamic::Create(Master, Vehicle);
+    MID->SetTextureParameterValue(TEXT("Gobo"), Texture);
+    Vehicle->Materials.Add(MID);
+    return MID;
+}
 } // namespace
 void FSCLightImporter::Build(const FSCPackageSource& Source, const FSCSceneInstance& Instance, ASCVehicle* Vehicle, USceneComponent* Parent,
                              TFunction<USceneComponent*(const FString&)> Helper, TArray<FString>& Warnings, bool bHelperFrame)
 {
     auto Basis = [bHelperFrame](FVector V) { return bHelperFrame ? FSCMeshImporter::Position(V) / 100. : LightBasis(V); };
+    const bool bShadowCap = !MegaLightsActive();
+    const double ShadowRadius = bShadowCap ? ShadowRadiusThreshold(Instance.LightJson) : 0;
     for (const auto& Text : Instance.LightJson)
     {
         FLightJson J;
@@ -90,7 +134,9 @@
         if (auto* Local = Cast<ULocalLightComponent>(Light))
         {
             Local->SetIntensityUnits(ELightUnits::Candelas);
-            Local->SetAttenuationRadius(FMath::Max(0., LightNumber(J, TEXT("radius_m"))) * 100);
+            const double Radius = FMath::Max(0., LightNumber(J, TEXT("radius_m")));
+            Local->SetAttenuationRadius(Radius * 100);
+            Local->SetCastShadows(!bShadowCap || Radius >= ShadowRadius);
         }
         Light->SetIntensity(FMath::Max(0., LightNumber(J, TEXT("intensity_candela_proxy"))));
         if (auto* Spot = Cast<USpotLightComponent>(Light))
@@ -165,10 +211,7 @@
             }
             if (Master && Texture)
             {
-                auto* MID = UMaterialInstanceDynamic::Create(Master, Vehicle);
-                MID->SetTextureParameterValue(TEXT("Gobo"), Texture);
-                Vehicle->Materials.Add(MID);
-                Light->SetLightFunctionMaterial(MID);
+                Light->SetLightFunctionMaterial(GoboMaterial(Vehicle, Master, Texture));
             }
             else
                 Warnings.Add(Name + TEXT(": gobo unavailable: ") + TexturePath);
```

Suggested test: `FSCRigLights` with `r.MegaLights.EnableForProject 0` — four synthetic lights
with radii 1, 2, 3, 4 m: only the 4 m light casts shadows; two gobo lights with the same texture
share one `UMaterialInstanceDynamic`.

## 9. Screenshots (`ue/verification/wp-u3b/`, half size, letterbox removed)

| File | Configuration |
| --- | --- |
| `cutlass-bay-before-vsm-fixedexp.jpg` | current path, fixed exposure (white bay, VSM overflow) |
| `cutlass-bay-megalights-fixedexp.jpg` | MegaLights, old exposure (no overflow warning) |
| `cutlass-bay-vsm-autoexp.jpg` / `cutlass-bay-megalights-autoexp-atlas4.jpg` | VSM vs MegaLights under auto exposure |
| `cutlass-bay-megalights-autoexp-atlas16.jpg`, `cutlass-gobo-atlas4-top-atlas16-bottom.jpg` | gobo atlas fix |
| `cutlass-forward-vsm-autoexp.jpg` / `cutlass-forward-megalights-autoexp.jpg` | forward interior, both paths |
| `cutlass-bay-autoexp-hdr.jpg`, `gladius-top-autoexp-hdr.jpg`, `scorpius-autoexp-hdr.jpg` | `VisualizeHDR`: EV100 the auto exposure picks |
| `final-cutlass-bay.jpg`, `final-cutlass-bay-4k.jpg`, `final-cutlass-forward.jpg`, `final-scorpius.jpg`, `final-gladius-top.jpg`, `final-gladius-three-quarter.jpg` | final ini |
| `gladius-top-before.jpg` | Gladius top-down before (for the reference comparison) |
| `leakcheck-cutlass-nosun-vsm.jpg` / `leakcheck-cutlass-nosun-megalights.jpg`, `leakcheck-cutlass-bay-nosun-megalights.jpg` | leak check, sun/sky off |
| `scorpius-area-lights-blender-units.jpg` | area lights in the addon's units (no visible change) |

## 10. Commands

```bash
ue/scripts/measure_lights.sh out/dec-cutlass_black cb-A -SCView=180,2,0.25          # SC_SP=94.4 / 226.5
SC_EXEC="r.MegaLights.EnableForProject 1" ue/scripts/measure_lights.sh out/dec-cutlass_black cb-B -SCView=180,2,0.25
SC_EXEC="r.MegaLights.EnableForProject 1, r.MegaLights.LightFunctions 0" ue/scripts/measure_lights.sh …
SC_EXEC="ShowFlag.DynamicShadows 0" ue/scripts/measure_lights.sh …
SC_EXEC="r.DefaultFeature.AutoExposure 1, ShowFlag.VisualizeHDR 1" ue/scripts/measure_lights.sh … \
  "-ini:Engine:[/Script/Engine.RendererSettings]:r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange=True" \
  "-ini:Engine:[/Script/Engine.RendererSettings]:r.DefaultFeature.AutoExposure.Bias=0"
ue/scripts/measure_lights.sh out/dec-cutlass_black final-cb-bay-900p -SCView=180,2,0.25   # final ini, no overrides
```

Manual-exposure experiments (`r.EyeAdaptation.MethodOverride 3` with `r.ExposureOffset`) did not
match the expected scale and are not used.

## TODO

- TODO(WP-U3b): apply the §8 patch after the WP-U3 round, build, add the suggested test; then
  `r.LightFunctionAtlas.Size` can return to the default if every ship stays ≤ 16 unique gobos.
- TODO(WP-U3b): bounded auto-exposure PPV in the Viewer map (`create_viewer.py`, −3…6 EV100).
- ~~TODO(WP-U2): re-tune `GlowToEmissive` for auto exposure (or derive Glow → cd/m²).~~ Done 2026-11-10,
  derived (§13.3).
- TODO(WP-R5): export `shadowParams` (cast-shadow flag) and `planeWidth`/`planeHeight`; the
  shadow rule and the 64 cm rect default are stand-ins for them.
- TODO(WP-V2): light-state selection (default/auxiliary/emergency/off); the Gladius hangar
  reference likely shows a parked state without the wing Omnis.
- TODO(WP-U3b): MegaLights is "Experimental" in 5.8.2; recheck on engine upgrades and keep the
  fallback rule working (the patch keys it on the same CVar).

## 11. Ranges and intensities (follow-up to owner feedback, analysis only — no GPU runs yet)

> **Superseded in part by §13 (2026-11-10).** The intensity rule of this section (H-CE,
> `I = π·10⁴·intensity·bulb²`, from the public CryEngine 5 source) is replaced by the rule read from the
> installed game's own shaders (H-SE, §13.2); the open `attenuationTweak` question is answered there. The
> attenuation model in UE (§11.3), the hangar (§11.9) and the measurements stand as records of that build.

Owner: the ship's own lights pool metres across the floor (Gladius red/blue wing lights, Scorpius
white/blue), while `reference/gladius/top-hangar.webp` shows none. The UE build lock and GPU
belong to WP-V1; nothing below was rendered. Raw data was read from `Data.p4k` with the existing
`target/debug/starbreaker` binary (`p4k extract`, `cryxml convert`) — no cargo, no exporter change.

### 11.1 Raw authored fields (`EntityComponentLight`, `__type="LightComponentParams"`)

Containers read: Gladius `base_lightgroups` + `base_int_all_main` (77 lights), Scorpius
`ext_lightning_main` + `int_body_main` (127), Cutlass `ext_lg` + `base_int_cockpit_main` +
`base_int_body_main` (225) — the same counts the exporter emits. Per light:
`sizeParams{lightRadius, bulbRadius, planeWidth, planeHeight}`, state blocks
`off/default/auxiliary/emergency/cinematicState{intensity, temperature, color}`,
`projectorParams{texture, FOV, focusedBeam}`, `shadowParams{shadowCasting, maxShadowCastDist,…}`,
`fadeParams{min/maxDistance}`, `miscParams{glowMultiplier, attenuationTweak, planeSoftness,…}`,
component attributes `importance`, `affectsThisAreaOnly`, `useTemperature`, `affectGI`.
The exporter keeps only `lightRadius`, the state intensities/colours/temperature, the projector
texture and FOV, and (for ambient proxies) `glowMultiplier`.

| Field | Gladius | Scorpius | Cutlass |
| --- | --- | --- | --- |
| `bulbRadius` (m), most common | 0.007 ×26, 0.1 ×16, 0.01 ×12, 0.05 ×6, 0.001/0.005 ×6 | 0.05 ×26, 0.007 ×17, 0.02/0.06 ×16, 0 ×14 | 0.05 ×175, **1.0 ×16**, 0.005 ×16 |
| `shadowCasting=1` | **2** of 77 | **4** of 127 | **3** of 225 |
| `attenuationTweak ≠ 1` | 2 headlights (0.007, R 1000 m) | 2 headlights (0.007, R 1000 m) | 4 headlights (0.006, R 1500 m) |

Gladius exterior examples: wing nav `Omni` intensity 1.0, bulb 0.05 m, R 5–7 m; blue `Projector`
0.2, bulb 0.1 m, R 10.5–14 m, FOV 120–160; red body projectors 0.1, bulb **0.01 m**; all
`shadowCasting=0`, `fadeParams.maxDistance` 40–100 m, `affectsThisAreaOnly=1`.

### 11.2 What `intensity` means (CryEngine source; Star Engine assumed to inherit it)

- CryEngine 3 docs (*Light Entities*): since 3.6 the falloff is **inverse square**; the
  attenuation bulb size "fakes the size of the light bulb": full intensity inside the bulb, falloff
  beyond it; the last 20 % of the radius fades to zero
  ([cryengine.com](https://www.cryengine.com/docs/static/engines/cryengine-3/categories/1114113/pages/1048738)).
- CRYENGINE 5 source (public mirror `MergHQ/CRYENGINE`), `Engine/Shaders/HWScripts/CryFX/shadeLib.cfi:251`
  `GetPhysicalLightAttenuation`: `fade = saturate((R−d)/(0.2R))²`, `att = fade / (1 + max(d−b,0)/b)²`.
  For d ≫ b this is **b²/d²**: the authored value is the intensity *at the bulb*, not a candela.
- `GameSDK/Scripts/Entities/Lights/Light.lua:8,31,171`: light colour = `clrDiffuse × fDiffuseMultiplier`
  passed to the renderer unconverted; default `fAttenuationBulbSize = 0.05`.
- Units: `CryCommon/CryRenderer/IRenderer.h` `RENDERER_LIGHT_UNIT_SCALE = 10000`; the sun is set in
  lux (`EnvironmentPreset.cpp`, default **119 000 lux**) and converted as `lux / 10000 / π`
  (`TimeOfDay.cpp:456`, "divide by PI as this is not done in the BRDF"); emissive is kcd/m².
  Shading multiplies colour × albedo × N·L without the Lambert 1/π, so displayed luminance =
  10⁴ × renderer value (the sun's /π restores physical Lambert). A point light of colour C therefore
  gives, beyond its bulb, E(d) = π·10⁴·C·b²/d² lux.

**Derived mapping (hypothesis H-CE):** with the state `intensity` in the role of the diffuse
multiplier, the luminous intensity is **I_cd = π·10⁴ · intensity · bulbRadius²**, with a source of
radius `bulbRadius` (inside it the illuminance saturates at π·10⁴·intensity lux).
`glowMultiplier` has no counterpart in CE5's light entity (SC-specific; affects only the ambient
proxies in the exporter). `attenuationTweak` (headlights only) is not in the CE5 source; its
meaning is unresolved — measure (below), do not guess.

**Is ×1500 right? No.** `LIGHT_AUTHORED_INTENSITY_SCALE = 1500` comes from a 3ds Max import
script, ignores the bulb, and is the same for a 1 mm and a 1 m bulb, whereas H-CE scales with b²
(10⁶ between them). proxy/H-CE ratio (point/spot lights with bulb > 0): Gladius median **477**
(range 0.5 … 5·10⁸), Scorpius median 19, Cutlass median 19 (range 0.05 … 10⁷). The proxy makes
small-bulb fixture lights (housing glows, 1–10 mm bulbs) as bright as floodlights, which is
exactly the "pools everywhere" symptom; H-CE makes them sub-candela.

### 11.3 Attenuation model in UE

| CryEngine | UE (proposed) |
| --- | --- |
| inverse square beyond the bulb, saturated inside it | `bUseInverseSquaredFalloff`, `ELightUnits::Candelas`, I = π·10⁴·intensity·b²; **`SourceRadius = bulbRadius`** (point/spot; also gives the specular highlight and penumbra size the addon approximated as 0.05·radius) |
| zero at `lightRadius`, fade over its last 20 % | `AttenuationRadius = lightRadius` (UE's window (1−(d/R)⁴)² ends at the same R) |
| `Planar`: `planeWidth × planeHeight` | `URectLight` `SourceWidth/Height = planeWidth/Height`; intensity unit for planar lights is unverified (CE area lights take luminance; candidate L = 10⁴·intensity cd/m², I_normal = L·w·h) — measure |
| `shadowCasting` | with MegaLights keep shadows on all lights: SC confines non-shadowing lights with `affectsThisAreaOnly` (vis areas), which UE lacks — ray-traced shadows are the containment substitute; without MegaLights shadow exactly the authored `shadowCasting=1` lights (2–4 per ship) instead of the radius-quartile rule of §6 |
| 1000–1500 m headlights (`attenuationTweak` 0.006–0.007, no shadows, no fade) | keep the authored radius: MegaLights cost was flat (Gladius with both headlights 0.62 ms); revisit only if `attenuationTweak` turns out to scale the falloff distance |

### 11.4 Scene baseline: presets from standard illuminance (not tuned to a ship)

The current Viewer map has a 5 lux sun (horizontal ≈ 3.5 lux at 45°): every authored light, even
under H-CE, outshines it. Proposed presets (created by `ue/scripts/create_viewer.py`, one map per
preset or a preset parameter; each with an unbound post-process volume, auto exposure on):

| Preset | Lights | Horizontal illuminance | Predicted AE pick ¹ |
| --- | --- | --- | --- |
| daylight | directional 100 000 lux at 45° + SkyAtmosphere + real-time sky light | ≈ 86 000 lux (sun 70.7 k + sky ~15 k) | EV100 ≈ +13 |
| hangar | overhead directional 300 lux (70°) + uniform sky light ≈ 100 lux, no atmosphere | ≈ 400 lux (typical hangar/workshop 300–500 lux) | EV100 ≈ +5 |
| space | no sun; uniform sky light ≈ 1 lux (star field) | ≈ 1 lux | ship lights dominate; ≈ −3 or the ship's own level |

¹ Self-calibrated on the measured pick for the current map (−1.6 at ≈3.5 lux): pick ≈ −1.6 +
log2(E/3.5 lux). Proposed PPV bounds = predicted pick ± 3 EV.

**Predicted pool contrast** (pool illuminance / ambient horizontal illuminance; < 0.05 invisible,
0.05–0.3 faint tint, > 0.3 clear pool), floor distance h:

| Light | proxy (×1500) | H-CE | current | daylight | hangar | space |
| --- | ---: | ---: | --- | --- | --- | --- |
| Gladius wing nav Omni, h 2 m | 375 lux | 19.6 lux | 107 / 5.6 | 0.004 / 0.0002 | **0.94 / 0.05** | 375 / 20 |
| Gladius blue projector, h 2 m | 75 lux | 15.7 lux | 21 / 4.5 | ≈0 / ≈0 | 0.19 / 0.04 | 75 / 16 |
| Gladius red body projector (1 cm bulb), h 1.5 m | 67 lux | 0.14 lux | 19 / 0.04 | ≈0 / ≈0 | 0.17 / 0.0003 | 67 / 0.14 |
| Scorpius flood projector, h 2.5 m | 1200 lux | 186 lux | 343 / 53 | 0.014 / 0.002 | 3.0 / **0.47** | ≫1 |
| Cutlass bay flood (1 m bulb), h 3 m | 1067 lux | 22 300 lux | — interior | — | — | — |

(cells: proxy / H-CE). Predictions: under the **current** map pools are visible with either
mapping (the 5 lux sun is the main cause of the owner's observation); under **hangar** the Gladius
pools disappear with H-CE (≤ 0.05, matching `top-hangar.webp`) but stay with the proxy (0.94);
Scorpius floods remain a visible pool (0.47) — expected for a flood light that is on; **daylight**
hides all pools; **space** shows them, correctly. Risk: H-CE makes the Cutlass bay floods (1 m
bulbs) ~20× brighter than the proxy, and the Gladius cockpit lights (median 0.13 cd) very dim;
auto exposure absorbs the absolute level, the measurement checks the balance.

### 11.5 Light state

The exporter picks the first state with intensity > 0 in the order default → auxiliary →
emergency → cinematic (`socpak.rs:1916`). Gladius: all 77 lights are on in `defaultState`
(wing Omnis 1.0 in default, auxiliary and emergency alike). Cutlass 24 and Scorpius 3 lights are
**off** in `defaultState` and get shown in their auxiliary state — a policy error: a light whose
default is 0 should be off in the default state. Lights hang off `SCItemLightGroupParams
powerRequirement` (ship power); a ship parked in a hangar is typically unpowered (off state), and
the reference shows emissive strips but no light pools. This is a **viewer light-state default
question for WP-V2** (default to "powered/default", "off", or "auxiliary"; expose all states), not
a data error; the auxiliary fallback is a WP-R item.

### 11.6 Intended changes (not made; awaiting the go-ahead / lock)

1. **Exporter (WP-R, contract change, not this WP):** export `bulb_radius_m`, `plane_width_m`,
   `plane_height_m`, `shadow_casting`, `affects_this_area_only`, `attenuation_tweak`,
   `fade_min/max_distance_m`, `importance`; stop falling back to auxiliary when `defaultState`
   is 0 (report `active_state: offState`).
2. **Importer (after 1):** `I_cd = π·10⁴·intensity_raw·bulb_radius_m²`, `SourceRadius = bulb_radius_m`,
   rect `SourceWidth/Height` from the plane size, `AttenuationRadius = radius_m`; MegaLights → all
   lights shadowed, fallback → `shadow_casting` only. Replaces the §8 radius-quartile rule.
3. **Viewer map:** the three presets of §11.4 in `create_viewer.py` (default **hangar**), each with
   a PPV: auto exposure, EV100 bounds from §11.4.
4. **ini:** no further change planned (auto exposure, MegaLights, atlas stay).

### 11.7 Measurement plan (when the UE lock is free; `ue/scripts/measure_lights.sh`)

H-CE can be tested before any exporter or C++ change: scratch copies of the three packages (real
copies — the reader does not follow symlinks) whose `intensity_candela_proxy` is replaced by
π·10⁴·intensity·bulb² computed from the raw `.soc` data, matched by container, entity name and
light index (SourceRadius stays default until the importer change). Presets first as console-free
map variants built by `create_viewer.py` into `/Game/Maps/Viewer_<preset>` (map edit at that point).

| Run | Package | Preset | Mapping | Check |
| --- | --- | --- | --- | --- |
| 1–6 | Gladius top-down + three-quarter | current, hangar, daylight | proxy vs H-CE | pool contrast = mean floor luminance under the wing Omni / far floor (same image); expect §11.4 values; compare with `top-hangar.webp` |
| 7–9 | Gladius | space | proxy vs H-CE | pools visible in both (sanity) |
| 10–13 | Scorpius exterior | hangar, daylight | proxy vs H-CE | flood pool ≈ 0.5 under H-CE hangar |
| 14–16 | Cutlass bay + forward | hangar (sun irrelevant inside) | proxy vs H-CE | "stays lit": AE pick inside the bounds, < 5 % saturated floor pixels, corridors not black (mean luminance ≥ 0.1 of the bay) |
| 17 | Gladius headlights | space | H-CE with `attenuationTweak` as (a) ignored, (b) distance scale | which gives a plausible beam at 20–50 m |
| 18 | all three | hangar | H-CE | GPU time vs §3 (expect unchanged; MegaLights cost does not depend on intensity) |

Pass: Gladius hangar pool contrast < 0.1 with H-CE; Cutlass interior criteria met; no per-ship
constants anywhere.

TODO(WP-U3b): run §11.7 after the go-ahead; then hand the exporter field list (§11.6.1) to WP-R
and apply the importer mapping.
TODO(WP-R5): audit `LIGHT_AUTHORED_INTENSITY_SCALE` (×1500) and `AMBIENT_PROXY_DIRECT_FACTOR` against
the CryEngine attenuation model above; export the fields in §11.6.1.
TODO(WP-V2): light-state default for parked ships (§11.5).

### 11.8 Measurements (2026-11-09, RTX 4090; UE lock after WP-V1)

**Setup.** Presets: `ue/scripts/create_viewer_presets.py` (as of `b2b4318`; superseded by `create_viewer.py`, §11.9; run with `UnrealEditor-Cmd … -run=pythonscript
-script=ue/scripts/create_viewer_presets.py -nullrhi`, after deleting `Content/Maps/Presets/`) duplicates
`/Game/Maps/Viewer` into `/Game/Maps/Presets/Viewer_{Daylight,Hangar,Space}` (sun 100 klux at 45° / 300 lux at
70° / no sun and no atmosphere; PPV EV100 bounds 10…16 / 2…8 / −6…6) and two measurement maps
`Viewer_{Daylight,Hangar}_FixedEV` with auto exposure pinned to the predicted EV100 (13.51 / 5.26 =
−1.56 + log2(E / 2.5 lux)). Note: the base map's sun is at **30°** elevation (`Rotator(-45,-30,0)` is
roll −45, pitch −30), so its horizontal illuminance is 2.5 lux, not 3.5 as assumed in §11.4.
`measure_lights.sh` gained `SC_MAP` (map URL as the first host argument). H-CE package copies
(scratch, never under git): all 77/127/225 exported lights matched to the raw `.soc` records by container,
entity name and index, validated against `intensity_raw` and `radius_m` (0 mismatches); every state's
`intensity_candela_proxy`/`intensity_cd` rewritten (the importer reads the state, not the flat field).
Lights-off references: `ShowFlag.PointLights 0, SpotLights 0, RectLights 0`.

**Pool contrast metric.** Same camera, same exposure, on/off pairs: contrast =
mean(on/off | pool) / mean(on/off | far floor) − 1 on sRGB-decoded pixels, per channel (the nav lights
are red); pool mask = lit floor that the proxy brightens > 2× in the current map; peak = 95th percentile
of the strongest channel. Noise floor (daylight, no visible pools): peak ≈ 0.03–0.05.

| Gladius top-down | proxy ×1500: mean R / peak | H-CE recomputed: mean R / peak | H-CE in the built importer, WP-R5b package: mean R / peak |
| --- | --- | --- | --- |
| current map (5 lux sun) | 4.30 / 5.98 (55 % clipped) | 0.77 / 2.31 | 0.79 / 2.36 |
| hangar, EV100 5.26 | 0.138 / **0.50** | 0.001 / **0.043** | 0.001 / **0.042** |
| daylight, EV100 13.51 | 0.000 / 0.027 | 0.000 / 0.026 | 0.000 / 0.047 |
| space (auto exposure) | pools visible | pools visible | — |

**Verdict on H-CE: pass for the reported symptom.** Under the hangar preset the Gladius pools vanish
with the CryEngine model (peak 0.042, at the noise floor; criterion < 0.1) and stay visible with ×1500
(peak 0.50, red tint) — matching `top-hangar.webp`. Under the original 5 lux map the pools remain with
either mapping, so the environment is half of the owner's observation. Daylight hides all pools; space
shows them (correct). Scorpius under the hangar preset: no floor pools visible with either mapping (the
floods do not reach the visible floor; the pool mask has only ~1 000 px, no meaningful number).

**Cutlass interior.** Hangar preset (bounds 2…8), WP-R5b package + importer: bay mean linear luminance
0.078 (proxy 0.176), floor saturation **2.2 %** (< 5 %), forward corridor 0.021 = **0.27 of the bay**
(≥ 0.1): passes the "stays lit" criteria, but the balance changes. 13 exterior-group floods carry
`bulb_radius_m = 1.0` and intensity 6.4 → 201 000 cd each (H-CE) and dominate; most other fixtures drop
~19×. `VisualizeHDR` in the hangar preset: scene average **EV100 −1.11**, clamped by the preset minimum
2 (`r5b-cutlass-bay-hangar-hdr.jpg`); in the unchanged Viewer map (bounds −3…6) the auto exposure follows
to −1.1 and **19.5 % of the bay floor saturates** (`r5b-cutlass-bay-viewer-map.jpg`). Fails the
"AE pick inside the bounds" sub-criterion: the bay is a dark room with very bright flood pools.

**attenuationTweak (headlights).** Gladius headlights in space, camera ahead of the nose
(`hce-gladius-headlights-proxy-ignored-distance.jpg`): ×1500 (750 cd), H-CE ignoring the tweak (39 cd)
and H-CE with the tweak as a distance scale (b/tweak = 7.1 m → 8·10⁵ cd) — none lights the floor ahead
within the 15 m floor; the distance-scale variant only adds a saturated hot spot at the emitter.
**Inconclusive**; the importer does not apply `attenuation_tweak` (documented, TODO).

**Importer (built, against the WP-R5b fields).** `SCLightImporter.cpp`: `FSCLightImporter::StateCandela`
(public, for WP-V2 state switching) — packages with `bulb_radius_m` use I = π·10⁴·`intensity_raw`·b²
(point/spot) and 10⁴·`intensity_raw`·`plane_width_m`·`plane_height_m` (planar); older packages keep
`intensity_candela_proxy`. `SourceRadius = bulb_radius_m`, rect `SourceWidth/Height` = plane size,
fallback shadows (no MegaLights) = `shadow_casting` (radius quartile only for old packages).
`ue/scripts/build.sh SCViewerEditor`: `Result: Succeeded`. New automation test
`SCBridge.Lights.AuthoredCryEngineModel` + `SCBridge.Lights.DefaultsAndShadowBudget`: Success;
full `SCBridge` suite 18/19 (the 19th, `Package.FolderTarImport`, needs `-SCPackage -SCVerifyTar`
arguments and fails without them, unrelated). GPU: Cutlass bay, default Viewer map, 1601×900:
**7.05 ms** (MegaLights 0.83 ms; §3: 7.01 ms) — intensities do not change MegaLights' cost.

Some runs overlapped another GPU application (logged per run in the batch logs); none of the
contrast numbers depend on timing.

**Evidence** (`ue/verification/wp-u3b/`): `hce-gladius-current-hangar-proxy-hce-off.jpg` (rows: current
map, hangar; columns: proxy, H-CE, lights off), `hce-gladius-daylight-space-proxy-hce.jpg`,
`hce-scorpius-hangar-proxy-hce.jpg`, `hce-cutlass-hangar-proxy-hce.jpg`,
`hce-gladius-headlights-proxy-ignored-distance.jpg`, and for the built importer on the WP-R5b packages
`r5b-gladius-top-hangar.jpg`, `r5b-gladius-top-current-map.jpg`, `r5b-gladius-three-quarter-hangar.jpg`,
`r5b-scorpius-hangar.jpg`, `r5b-cutlass-bay-hangar.jpg`, `r5b-cutlass-forward-hangar.jpg`,
`r5b-cutlass-bay-viewer-map.jpg`, `r5b-cutlass-bay-hangar-hdr.jpg`. Full-size captures:
`out/smoke/wp-u3b-r2/`, `out/smoke/wp-u3b-r5b/`.

**Open items.**
- The Viewer's default map: replace the 5 lux sun with the hangar preset (or make the preset the
  default) — only then do the pools disappear; decision for the orchestrator / WP-V2.
- Auto-exposure metering: the presets' floor is a 30 m slab in a black void; the void pulls the
  histogram (Gladius three-quarter view overexposed under the hangar bounds 2…8,
  `r5b-gladius-three-quarter-hangar.jpg`). Enclose the hangar preset (walls/ceiling) or meter centre-weighted,
  and widen the minimum to cover interiors (Cutlass bay EV100 −1.1).
- Cutlass flood balance (**resolved in §12**: placement, not the bulb term): confirm in game whether 1 m-bulb floods really dominate the bay, or whether Star
  Engine bounds the bulb term (the only lights with b ≥ 1 m are these 16 exterior-group projectors).

TODO(WP-U3b): commit decision on `Content/Maps/Presets/*.umap` (generated, untracked) and the default map.
TODO(WP-U3b): `attenuation_tweak` semantics (headlights) — needs a game-side reference.
TODO(WP-V2): reuse `FSCLightImporter::StateCandela` when switching light states.

### 11.9 Default environment: closed hangar (orchestrator decision 2026-11-09)

`ue/scripts/create_viewer.py` now builds `/Game/Maps/Viewer` as a closed hangar and, from the same code,
the selectable presets `/Game/Maps/Presets/Viewer_Daylight` and `Viewer_Space` (generated, untracked; WP-V2
wires the UI). `create_viewer_presets.py` is removed (its `_FixedEV` measurement maps served §11.8 only).
Regeneration: delete `Content/Maps/Viewer.umap` and `Content/Maps/Presets/`, then run
`UnrealEditor-Cmd … -run=pythonscript -script=ue/scripts/create_viewer.py -nullrhi`; `/Game/Smoke` assets
(`NaniteCube`, `Emissive`, new `M_ViewerGrey`) are reused when present. `verify_viewer.py` and the exposure
assertions in `test_importer.py` follow the new contents. Physical reasoning per number:

| Quantity | Value | Reason |
| --- | --- | --- |
| Floor | 80 m × 80 m | largest test ship (Cutlass, `BOUNDS_CM` 35.7 × 26.1 × 10.8 m, bounds radius 22.8 m): ≥ 20 m clear around the footprint; review cameras orbit at ≤ 1.6 radii (≤ 36.5 m) and stay inside in the standard views |
| Clear height | 40 m | the importer lifts models 1 m above the floor; the Cutlass bounds centre is ~6.4 m up, the top-down camera at 1.3 radii is ~36 m up |
| Walls, ceiling, floor | 1 m Nanite slabs, base colour 0.18, roughness 0.8 | photographic middle grey — the reflectance auto exposure targets — and matte so the walls do not mirror the ship; closing the box removes the black void that skewed metering in §11.8 |
| Lighting | 16 rect panels 4 m × 1 m, 0.1 m below the ceiling, 20 m pitch, **98 400 cd** each (normal intensity, ≈ 309 klm) | chosen so the **direct** horizontal illuminance averages **400 lux** over the central 40 m × 40 m: EN 12464-1 industrial work areas 300–500 lux. Far-field Lambert sum E(p) = Σ I₀·cos²θ/d²: 435 lux at the centre, 353 lux at (20 m, 20 m), 170 lux at the wall corner; Lumen inter-reflection off the 18 % box adds on top. Panel size only sets shadow softness. Computed by the script, logged as `VIEWER_HANGAR` |
| Sun, atmosphere | none in the hangar | indoor scene; the sky light captures the enclosure (real-time) |
| Auto exposure (hangar) | histogram, EV100 **−3 … 10**, bias 0 | measured in this hangar (runs below): ship exteriors pick 7.3–7.9, the darkest ship interior (Cutlass bay) −2.1. Lower bound one stop below the darkest interior, upper two stops above the brightest exterior. (First version −2 … 8 came from the prediction −1.56 + log2(400 / 2.5) = 5.8, which the closed box's inter-reflection exceeds by ~2 EV.) |
| Daylight preset | sun 100 klux at 45° + SkyAtmosphere, open floor, EV100 10 … 16 | clear-sky sun; ≈ 86 klux horizontal; predicted pick 13.5 ± 3 |
| Space preset | no sun, no atmosphere, open floor, EV100 −6 … 6 | the ship's own lights set the exposure |
| MegaLights, ini | unchanged | — |

Prediction for the Gladius wing nav light under the hangar (H-CE 78.5 cd, ~2 m above the floor):
19.6 lux of red against ~400 lux → contrast ≈ 0.05 (invisible); the ×1500 proxy would be 0.94.

**Runs (2026-11-09, after the WP-V1 build round).** Maps regenerated: `create_viewer.py` → `VIEWER_HANGAR
panels=16 candela_each=98352 direct_lux centre=435 (20,20)m=353 corner=170`, three `VIEWER_PRESET` lines,
`VIEWER_MAP_OK`; `verify_viewer.py` → `VIEWER_VERIFY_OK expected_actors=28`. Captures on the WP-R5b packages
with the built importer, default map unless noted (`out/smoke/wp-u3b-hangar/`, evidence `wp-u3b/hangar-*.jpg`):

| Check | Result | Criterion |
| --- | --- | --- |
| Gladius top-down pool contrast (lights-off = point+spot off; the hangar panels are rect lights) | mean R/G/B 0.000/0.000/0.000, peak **0.039** (noise floor); only 64 px differ > 20/255, all on the ship's own fixtures | < 0.1 **pass**; matches `top-hangar.webp` (`hangar-gl-top.jpg`) |
| Auto exposure, Gladius top / three-quarter / Scorpius | EV100 **7.89 / 7.44 / 7.30** | inside −2 … 8, but 0.1–0.7 EV under the upper bound |
| Auto exposure, Cutlass bay | EV100 **−2.1**, clamped at the lower bound −2 | **fails** "inside the bounds" by 0.1 EV |
| Cutlass bay floor saturation | **19.5 %** (frame 8.2 %) | < 5 % **fails** — the 1 m-bulb floods (resolved in §12: misplaced exterior floods) |
| Cutlass forward corridor | mean linear luminance 0.151 = 0.56 of the bay (0.268); floor saturation 1.9 % | ≥ 0.1 **pass** |
| Cutlass bay GPU time, 1601×900 | 11.76 ms median, MegaLights pass 1.94 ms (was 7.05 / 0.83 in the open map) | **not clean**: another game used ~40 % of the GPU during the whole batch; the 16 hangar panels do add MegaLights work. Re-measure on an idle GPU |
| Daylight preset | sun-lit, sharp shadows, no pools | sanity ok |
| Space preset | red/blue pools on the floor, ship-lit only | sanity ok (expected) |

Findings: (1) the hangar removes the pools and gives the reference look; (2) the measured hangar picks
(7.3–7.9) sit ~2 EV above the 5.8 prediction — the prediction was calibrated in the old open map with a
lighter floor and no inter-reflection; Lumen bounce in the closed 18 % box raises the scene. Proposed
bounds from these measurements: **EV100 −3 … 10** (one stop below the Cutlass bay −2.1, two above the
hangar 7.9). (3) Cutlass bay saturation is the open 1 m-bulb question, unchanged by the environment.

**Bounds applied (−3 … 10)** in `configure_viewer_exposure.py` (default), `create_viewer.py` and the
`test_importer.py` assertion; maps regenerated (`VIEWER_EXPOSURE … min_ev100=-3 max_ev100=10`,
`VIEWER_VERIFY_OK expected_actors=28`). Re-run on the final WP-R5b packages (regenerated with sparse
components inheriting DataCore class values), `out/smoke/wp-u3b-hangar2/`, evidence `wp-u3b/hangar-bounds-*.jpg`.
`nvidia-smi` before and after every run: **Soulstone-Survivors (another game) at 38–42 % GPU throughout**.

| Check | Result | Criterion |
| --- | --- | --- |
| Gladius top-down auto exposure | EV100 **7.89** (pre-exposure 0.0042) | inside −3 … 10 **pass** |
| Cutlass bay auto exposure | EV100 **−2.14** (pre-exposure 4.40) | inside −3 … 10 **pass** |
| Cutlass bay floor saturation | **19.9 %** (frame 8.4 %), mean linear luminance 0.279 | < 5 % **fails** — 1 m-bulb floods (resolved in §12: 3.7 % after the placement fix) |
| Cutlass bay GPU time, 1601×900 | 10.66 ms median, p90 12.69 (MegaLights pass 1.83 ms) | **contended** (game at ~40 % GPU); clean number still owed |

~~TODO(WP-U3b)~~ **Resolved 2026-11-10, §12** (the floods were exported 5.46 m off, inside the hull; the bulb term
is right). Original note: Cutlass 1 m-bulb floods (13 exterior-group projectors, intensity 6.4, `bulb_radius_m` 1.0,
≈ 201 000 cd each under H-CE) dominate the bay (scene EV100 −1.1, 19.5 % floor saturation in the old
map, §11.8); needs an in-game reference before the bulb term is questioned. Evidence:
`r5b-cutlass-bay-viewer-map.jpg`, `r5b-cutlass-bay-hangar-hdr.jpg`.

## 12. Cutlass interior blow-out: light-group placement and area confinement (2026-11-10)

Owner feedback after the MegaLights demo ("something is wrong with light ranges … or intensities") and the
open §11.9 TODO (bay floor 19.9 % saturated). Result: **neither ranges nor intensities were wrong. 103
exterior lights of the Cutlass were exported 5.46 m from where the game puts them**, which moved the
under-hull floods into the cargo bay and the cockpit. Evidence `wp-u3b/lightanchor-*.jpg`.

### 12.1 Diagnosis

`-SCLightProbe` (new, §12.5) on the armed walkthrough, before any change (`lp0-cutlass`): the player in the
cockpit seat received **26 039 lux unshadowed / 25 320 lux visible**, 25 147 lux of it from one light,
`LightGroup_BL_Body-008` (201 062 cd, 2.81 m away, unblocked). The hangar gives 400–700 lux.

Hypotheses checked against the game shaders (decoded from the P4k shader cache; not in git):

| Hypothesis | Verdict |
| --- | --- |
| The H-CE bulb term (I = π·10⁴·intensity·bulb²) is wrong for 1 m bulbs | **No.** `GetPhysicalLightAttenuation` (ShadeLib) is `bulb² / max(d², bulb²)` × forced falloff, as §11 assumed; a 1 m bulb is that bright in the engine too |
| `projectorNearPlane = 49.18` (authored on exactly the 16 1 m-bulb projectors) cuts them off (`projDepthCutoff` in tiled shading) | **Rejected as a rule.** The CPU-side mapping is not visible in the shaders, the GI path has no such cutoff, and the values across the group (0, 49.18, −60.66, −57.38 = n/61) include Omni lights, where a projector near plane means nothing. Not exported |
| `affectsThisAreaOnly` confines these lights to "outside" | **True and implemented (§12.3), but not the cause**: confinement alone changed nothing measurable (`la1-cutlass` ≈ `lp0-cutlass`) because the lights sat *inside* the room volumes |
| The lights are in the wrong place | **Yes.** The raw `.soc` names them `Light_Flood_Under-*` / `Light_LG*`: under-hull and landing-gear floods |

### 12.2 Root cause and rule

`ext_lg.socpak` is attached to the vehicle at helper `hardpoint_lights_ext` = (0.119, −4.731, 2.720) m. Its
eight `EntityComponentLightGroup` entities carry `.soc` positions that are **already in the vehicle frame**;
the loader composed them with the helper again, shifting every light by |(0.119, −4.731, 2.720)| = 5.4585 m.
Other containers author the same field container-local (Cutlass `base_int_body_main`), so neither reading
of the `.soc` position is right for all of them.

**Rule (data-derived, no asset names):** a light group that names an item port
(`TileAttachmentParams@ItemPortName`) is placed with its origin on that port's `interactionOffset` in the
container descriptor (`<name>.xml`, `TileItemPortEntries/ItemPort`), which is authored in the frame of the
entity that owns the container. The group's lights keep their offsets from the group origin and their
orientation. At load time: `position' = inv(W)·port + (position − group_position)`,
`W = helper_world × container_transform`. Conflicting duplicate port names are dropped with a warning (the
`.soc` placement is kept). Scope: containers listed on the root entity's
`VehicleComponentParams.objectContainers`; nested child socpaks and containers of loadout children are not
anchored.

Effect on the exports (`scene.json` before → after; light counts, rotations and every other field equal):

| Container · group | Lights | Moved |
| --- | --- | --- |
| Cutlass `ext_lg` · 8 groups (`BL_Body` 32, `BL_Tail` 35, wings 9+9, nacelles 5+5, headlights 4+4) | 103 | **5.4585 m** each |
| Cutlass `base_int_body_main` / `base_int_cockpit_main` | 70 / 52 | 10 mm / 11 mm |
| Gladius exterior groups / `base_int_all_main` | 21 / 54 | 2.1 mm / 0.3 mm |
| Scorpius, all 17 groups | 127 | 0 |

So the rule reproduces the previous placement to ≤ 11 mm on four containers where it was already right and
differs only where it was wrong. Independent check — distance from each Cutlass `ext_lg` light to the nearest
exported hull vertex (n = 103): median **1.446 m → 0.054 m**, mean 1.628 → 0.182, p90 3.256 → 0.171: the
lights now sit on the hull fixtures.

The bundled GLB path had the mirror-image defect (Codex review P1): containers were parented to the scene
root, so a helper-relative `container_transform` lost the helper. Containers now hang under their helper node
(or the owner entity node; scene root with a warning when neither exists). Checked on a real export
(`scviewer glb DRAK_Cutlass_Black … --materials none --no-animations --no-cache`): chain
`interior_ext_lg → hardpoint_lights_ext → DRAK_Cutlass_Black`; all 103 `ext_lg` lights sit at the light
catalogue's `parent_local_position` with a maximum residual of 0.0000 m.

### 12.3 Area confinement (`affects_this_area_only`)

The engine shades an area-only light only on pixels of the clip volume it sits in
(`GetClipVolumeWeightBinary`, tiled shading). The exporter now writes `interiors[].room_volumes[]`
(`entitycomponentroom.xml` `AreaComponent` segments: box, extruded polygon, closed triangle soup; Cutlass 8,
Gladius 2, Scorpius 1). UE has no per-pixel clip volumes, so the importer collapses the rule to two lighting
channels: channel 0 = outside every room volume, channel 1 = inside one. Area-only lights get the channel of
their position, other lights both; a primitive is inside when more than half of ≤ 64 vertex samples are
inside; environment lights get both; the player body and kit switch with the capsule every tick.
`LIGHT_AREAS` logs the result:

| Ship | Volumes | Lights inside / outside / everywhere | Meshes inside / outside |
| --- | --- | --- | --- |
| Cutlass, old placement (`la1`) | 8 | 98 / 77 / 50 | — |
| Cutlass (`la2`, `la3`) | 8 | **82 / 93** / 50 | 384 / 305 |
| Gladius | 2 | 53 / 23 / 0 | 34 / 319 |
| Scorpius | 1 | 29 / 80 / 18 | 192 / 727 |

Trimesh containment (Codex review P2): the first version counted a triangle edge or vertex hit by the parity
ray twice or not at all. Both sides (Rust `socpak/room_containment.rs`, UE `SCLightAreas.cpp`) now cast +Z
and decide each projected edge with a sign that never answers "on the edge" (the query is treated as displaced
by an infinitesimal (ε, ε²); edges evaluated in canonical vertex order so the two triangles sharing an edge get
opposite answers). Tests were red first: Rust lattice interior `[0.125, 0.625, 0.5]` and exterior
`[-0.5, -0.5, -1]`; UE 32 + 33 interior lattice points left out of the split/fanned cube and 4 + 4 exterior
points taken in. Cutlass `LIGHT_AREAS` counts are identical before and after this fix.

Known limitations (reviewed, not fixed):
- **Rooms are not confined from each other.** Membership is "inside any room"; the engine matches the
  room's own stencil ID (1–63). An unshadowed area-only light can light the neighbouring room through a wall.
  UE offers three lighting channels, not 63.
- **Channels are assigned once, at the rest pose** (`SCImportJob.cpp`, `SCVehicle.cpp`). A ramp or door that
  animates across a room boundary keeps its side.
- Clip-volume blend distance and portals are not modelled; the test is binary.

### 12.4 Measurements (RTX 4090, default hangar map, final build)

Armed Cutlass walkthrough (`ue/scripts/routes/cutlass-armed-walkthrough.txt`), `lp0` = before, `la1` = area
confinement only, `la2` = with the placement fix. Clipped pixels (all channels ≥ 250) of the whole window:

| Shot | lp0 | la1 | la2 |
| --- | --- | --- | --- |
| 17 first person, cargo bay | 2.5 % | 2.4 % | **0.6 %** |
| 19 first person, bulkhead door | 1.5 % | 1.5 % | 1.3 % |
| 20 third person at the bulkhead | 2.9 % | 2.9 % | 3.4 % |
| 21 first person, corridor | 2.8 % | 2.7 % | 3.2 % |
| 22 first person, cockpit | 0.0 % | 0.0 % | 0.1 % |
| 23 third person, cockpit | 1.9 % | 1.9 % | 1.7 % |
| 01 hangar / 16 ramp foot | 5.7 % / 0.2 % | 5.6 % / 0.2 % | 6.6 % / 0.3 % |
| 05, 08, 12, 15 exterior | 0.0 % | 0.0 % | 0.0 % |

Probe illuminance, lux unshadowed / visible (`LIGHT_PROBE … at=player|view`):

| Shot · point | lp0 | la2 |
| --- | --- | --- |
| 22/23 cockpit · player | 26 039.2 / **25 320.4** | 915.9 / **197.1** |
| 22 cockpit · view | 10 872.4 / 10 151.0 | 722.8 / 1.1 |
| 23 cockpit · view | 15 045.0 / 14 311.1 | 735.3 / 1.9 |
| 17 cargo bay · player | 725.2 / 21.8 | 725.2 / 21.9 |
| 17 cargo bay · view (bay floor ahead) | 785.4 / 21.6 | 771.4 / 7.6 |
| 19 bulkhead · player | 1 039.0 / 188.4 | 1 013.0 / 292.4 |
| 21 corridor · player | 838.7 / 95.6 | 839.4 / 122.8 |
| 16 ramp foot · player | 617.3 / 197.0 | 617.7 / 197.0 |

(The unshadowed totals are dominated by the 16 hangar panels, 98 352 cd each, which the hull blocks.)

§11.9 bay view (`measure_lights.sh out/dec-rv-cutlass anchor-cb-bay -SCView=180,2,0.25`), same metric on
the §11.9 capture (`wp-u3b-hangar2/cb-bay.png`) and the new one, viewport only, all channels ≥ 250:

| | §11.9 capture | Now |
| --- | --- | --- |
| Frame | 8.0 % | **2.3 %** |
| Floor band (lower 42 % of the viewport) | 17.6 % | **3.7 %** (< 5 %) |
| Mean sRGB luma | 0.452 | 0.314 |
| GPU time, 1695×953 | 10.66 ms median (contended) | 7.03 ms median, p90 7.51; MegaLights pass 1.35 ms |

The original floor mask of §11.9 (19.9 %) is not recorded; the band above is a stand-in that gives 17.6 % on
the same capture. The §11.9 capture also predates the static-wear materials (WP-U2), so the pair is not a
single-variable comparison — the walkthrough pairs above are. What still clips in the bay are the emissive
floor strips and wall chevrons. The GPU time is not a clean number either: `nvidia-smi` was read only after
the run (14 %, the other game still open).

Regression, ships whose placement did not move: Gladius orbit clipped 0.0 % and luma 0.238 before and
after; Scorpius walk shots 01–04 luma identical (0.236 / 0.218 / 0.238 / 0.238); Gladius hull-seat route
luma within ±0.016 of the pre-wear baseline, 0.0 % clipped. No visible difference on the contact sheets.

### 12.5 Tooling

- `-SCLightProbe` (host argument): at every route `shot`, logs for the player position and for the surface
  under the crosshair `LIGHT_PROBE tag=… at=player|view point=… channels=… lights=… lux_unshadowed=…
  lux_visible=… gated=… lux_gated=…` and one line per contributing light (lux, cd, distance, radius, cone
  factor, blocking hit). Inverse-square with the importer's candela, radius window and cone; `gated` = lights
  excluded by lighting channel. A line trace stands in for shadowing; gobos and the bulb clamp are ignored.
- `LIGHT_AREAS volumes=… lights_inside=… lights_outside=… lights_everywhere=… meshes_inside=…
  meshes_outside=… environment_lights=…` per import.

### 12.6 Verification

```bash
export CARGO_TARGET_DIR=$PWD/target
cargo test -q --manifest-path vendor/StarBreaker/Cargo.toml -p starbreaker-3d --lib   # 857 passed, 0 failed, 3 ignored
cargo test -q -p scviewer-core                                                         # 26 passed
scripts/build_ffi.sh && cargo build -q -p scviewer-cli && ue/scripts/build.sh SCViewerEditor   # all exit 0
UnrealEditor-Cmd … -run=pythonscript -script=ue/scripts/test_importer.py -nullrhi …     # WP-U1 UNIT_TESTS_OK
UnrealEditor-Cmd … -script=ue/scripts/test_player.py -nullrhi -SCPlayerTests …          # WP-P2 HEADLESS_OK
target/debug/scviewer decomposed DRAK_Cutlass_Black out/dec-rv-cutlass                  # also AEGS_Gladius, RSI_Scorpius
SC_TIMEOUT=2400 ue/scripts/capture_route.sh la2-cutlass ue/scripts/routes/cutlass-armed-walkthrough.txt \
  -SCDefaults -SCLoad=DRAK_Cutlass_Black -SCCamera=first \
  -SCPlayerLoadout=Data/Scripts/Loadouts/Player/DefaultLoadouts/PVE_medium_behr_rifle_gmni_rifle.xml -SCLightProbe
```

New tests: Rust `socpak::light_anchor` (6), `socpak::room_containment` (3), `gltf::tests_interiors` (5),
`pipeline::light_catalogue::item_port_anchored_group_lands_on_its_port_in_the_owner_frame`; UE light tests
for area classification and the trimesh cases. Cache format version 15 → 16.

Cross-review (Codex, read-only, no GPU): P1 bundled-GLB container parenting — fixed; P2 trimesh edge double
count — fixed, tests red first on both sides; P2 duplicate item-port names — fixed; P2 rooms not confined
from each other and P2 stale channels after animation — documented above.

### 12.7 Not verified / still open

- The rule was derived from five containers on three ships, all with identity container rotation and empty
  port helpers. Rotated containers, ports with a helper, `resourceLinkToParent = 1` light ports, nested child
  socpaks, containers of loadout children and standalone tile-attached `Light` entities are untested or
  unanchored.
- The end-to-end catalogue test was written with the fix and was not seen red on its own; the bundled check
  covers the 103 `ext_lg` lights only (the 122 interior-container lights have no catalogue parent-local
  position to compare against).
- Gladius, Scorpius and the Cutlass walkthrough were not re-captured after the trimesh fix (the Cutlass orbit
  was; its area counts are identical).
- Not compared with the game: the small yellow pool at the top of the ramp (a tail-group light at its authored
  position), the white door-frame strips at the bulkhead (shot 19), and the third-person cockpit view, where
  helmet and armour are still very bright. Shots 20 and 21 did not improve. No under-hull view was inspected.
- The hangar floor and the ramp are white before and after — viewer hangar panels and auto exposure, not ship
  lights. Bay auto-exposure EV100 was not re-measured.
- `projectorNearPlane`, `specularMultiplier`, `focusedBeam` are not exported; `attenuationTweak` semantics
  remain open (§11). *(Answered in §13.2.)*

## 13. Game photometry: light, glow, exposure and display curve from the installed renderer (2026-11-10)

Owner request: "continue improving this. visual fidelity is paramount." After §12 every interior was still
blown out or flat: light intensity came from the public CryEngine 5 source (H-CE, §11.2), emissive strength
from a host scale, exposure from UE's defaults and the picture went through UE's filmic tonemapper. All four
are replaced here by the rules of the installed game's renderer (Star Citizen `sc-alpha-4.10.2`, version
`4.10.197.2878`, change `12913470`), read from its shader sources and its executable. Nothing was tuned from
a screenshot and no ship, material or light name is tested anywhere.

RTX 4090, Vulkan SM6, Editor `-game` host, default hangar map. Superproject `ed429a8` plus this change;
submodule unchanged (`1f8993a4`), cache format still 16.

### 13.1 Source, fixture, validator

- The shader sources ship in `Engine\ShaderCache_Vulkan.pak` inside `Data.p4k` (stored, unencrypted;
  2,391,328,688 bytes, crc32 `aea61527`): a zip of FXB1 tokenised `.cfib` / `.cfxb` files. Cvar defaults are
  compiled into `Bin64/StarCitizen.exe` (found by disassembling each cvar's registration call).
- What the Viewer cannot read at run time is checked in as an **extracted fixture**,
  `ue/Plugins/SCBridge/Resources/StarEngineRender.json` (schema `star_engine_render_v1`): 22 cvar defaults,
  7 shader constants and the sha256 of the tokens of 17 shader functions the rules were written from. Shader
  text itself is not checked in. Provenance, method and "what each value drives":
  `StarEngineRender.notes.md` beside it.
- `FSCStarEngine::Load` (`SCStarEngine.cpp`) has **no in-code fallback**: a missing value, another schema or a
  value outside the rules' domain (non-positive units, inverted ranges, anchors not strictly ascending or a
  compensation segment with slope ≥ 1, discard ratios that leave no band, non-finite numbers) is an error
  naming the value, and `Get()` is fatal on it. `SCBridge.Build.cs` stages the fixture as a runtime dependency.
- Validator (run after every game patch; reads the pak in place from `Data.p4k`, so no extracted copy can go
  stale):

  ```
  $ python3 -I scripts/verify_star_engine_render.py
  STAR_ENGINE_RENDER OK: 47/47 checks match the installed game
  ```

  46 fixture values plus one completeness check (the fixture lists exactly the `r_ExposureCompensationEV<n>`
  cvars the executable registers). The validator applies the same domain rules as the loader.

### 13.2 Light intensity (H-SE, replaces H-CE of §11.2)

- One unit of scene colour is `HDR_LUMINANCE_SCALE` = **1000 cd/m²** (`common.cfib`).
- Point and projector lights attenuate with `GetPhysicalLightAttenuation`
  = `1 / (tweak · (d − bulb) / bulb + 1)²`, and the Lambert π is folded into the light colour
  (`DiffuseLambert`). With the authored intensity reached one metre from the bulb surface, the far field is
  `intensity · (1 + bulb/tweak)² / d²` scene units, so

  **`I_cd = π · 1000 · intensity · (1 + bulb/tweak)²`** (`FSCStarEngine::PointCandela`; 0 when tweak ≤ 0).

  This answers the open `attenuationTweak` question of §11/§12.7: it divides the bulb radius in the far
  field. The importer reads `attenuation_tweak` from the light record (1.0 when the record has none, the
  DataCore default).
- Planar (area) lights: the colour is the panel's luminance over π (`PlanarLightDiffuse_LightSpace`), so
  **`I_cd = π · 1000 · intensity · width · height`** (`PlanarCandela`).
- Inferences, not read from the game: the CPU-side normalisation ("reached one metre from the bulb surface",
  including the tweak) — the shader shows only the GPU half; and that planar lights get no CPU normalisation.
- The exported `intensity_candela_proxy` is used only for lights without the authored model
  (`HasAuthoredModel`), as before. The light verifier now derives class and candela through the importer's
  own functions (`ComponentClass`, `StateCandela`, `ShadowRadiusThreshold`) instead of a parallel copy.

### 13.3 Glow

- **Material glow** is quadratic: `MaterialGlowToLinearGlow` gives `sqrGlowFactor · Glow²` scene units with
  `sqrGlowFactor` = 400, so **`EmissiveStrength = 1000 · 400 · Glow²` cd/m²** (`FSCStarEngine::GlowNits`;
  Glow 0.1 → 4,000 cd/m², Glow 0.236 → 22,280 cd/m²). The host scale `GlowToEmissive` is gone.
- **Instance glow** is linear: `CalculateEmissive(glowColour, materialGlowStr, instanceGlowColour)` returns
  `materialGlowStr · glowColour + instanceGlowColour · glowColour`, and the hard-surface shader passes
  `instData.glow · GetPerInstanceGlowFactor()` as the instance term — no conversion, already scene units. So
  **`InstanceGlowNits(amount) = 1000 · amount` cd/m²**.
- The exporter's `controls.engine_glow` (units `material_property_glow`) is the output of an entity-effect
  link (DataCore `SEntityComponentEffects.entityEffects.materialEffects`, `materialProperty == Glow`: parameter
  × multiplier, clamped to min/max). It is read as an **instance-glow amount**, not a material Glow. Evidence:
  (a) the values — a census of 29,693 entities finds 3,049 Glow links whose most common chain is
  `× 100, min 1, max 10⁷` (1,428 links) and whose outputs run from 1 into the thousands, against authored
  material Glow of 0.02–0.5; (b) the executable carries `CRenderProxy::AddGlowForSlot(const SLightAmount&, …)`
  and `CGeomLinkManager::AddGlow(ELightLinkHandle, IRenderNode*, const SLightAmount&, int)` beside separate
  `MATERIAL_PROPERTY_GLOW_AMOUNT` / `MATERIAL_PROPERTY_LINKED_GLOW` names. This is string-level evidence; the
  code path was not disassembled. `GetPerInstanceGlowFactor` (a per-material constant) and any CPU scaling
  between the effect value and `instData.glow` are taken as 1 (not established).
- **Placeholder (registered fallback, WP-U2 register):** a rule strength > 0 with no authored Glow (forward
  glow cards, `%EMITTANCE_MAP`) gets one scene unit of instance glow. Sunset: derive from the light link
  (`glowMultiplier` × light state).

The first version of this change fed the effect value through the quadratic rule. The Scorpius main
thrusters idle at the control minimum 10 (tags `Thrust` × 60 min 10, `AfterBurn` × 120 min 15), which became
4·10⁷ cd/m² and bloomed into teal blobs over the hull (found on the GPU capture, not by a test). Test first:

```
UNIT_TEST name=FSCMaterialsFamilies passed=0
Expected 'effect Glow is linear instance glow' to be 50000.000000, but it was 1000000000.000000
Expected 'clamped to the control minimum' to be 10000.000000, but it was 40000000.000000
Expected 'clamped to the control maximum' to be 1000000.000000, but it was 399999991808.000000
```

then green with `InstanceGlowNits` (idle thrusters 10,000 cd/m²; the build before this change gave 100).

### 13.4 Exposure

The game (`OpticsExposure.cfxb`, `Histogram.cfib`; all numbers from the fixture):

- scene EV = log2(cd/m²) + 3 (`LogLumToEV`);
- `exposure = clamp(0.18 / exp2(logLumMetered − CalculateEVCompensation(EV)), 0.0001, 20)` per scene unit;
- compensation is piecewise linear through EV 0 / 4 / 8 / 12 / 16 = −4.5 / −3.7 / −1.0 / +0.5 / +2.0 stops, end
  segments extended — dark scenes stay dark (a 32 cd/m² interior is exposed one stop under middle grey);
- histogram meter over log2 scene units −14…16, 1 % discarded at each end;
- adaptation half-lives: pigment 5 s (to dark) / 2 s (to bright), pupil 0.4 s over 2 stops.

UE mapping (`FSCExposureSettings::From`, pure and unit-tested; `SCViewerExposure::Install` applies it):

| UE setting | Value | From |
|---|---|---|
| axis offset (scene EV − UE EV100) | 0.526 | `LOG2_LUMINANCE_TO_EV` + log2(0.18 · LuminanceMax), LuminanceMax 1 (extended range) |
| min / max brightness (EV100) | 1.561 / 31.121 | the scene EVs where the multiplier reaches 20 and 0.0001 (2.087 / 31.647; 2.087 ≈ 0.53 cd/m²) |
| low / high percent | 1 / 99 | discard ratios |
| speed up = down | 2.599 stops/s | ln 2 · `r.EyeAdaptation.ExponentialTransitionDistance` (1.5) / pupil half-life |
| histogram log range | −4.034 … 25.966 | `LOG_MIN` / `LOG_MAX` + log2(1000) |
| exposure-compensation curve | 6 linear keys: low clamp, EV 4 / 8 / 12 / 16, high clamp; first (1.561, −4.083), last (31.121, 7.868) | `CalculateEVCompensation` |
| compensation LUT | 1.561 … 31.121, 1893 samples (≤ 1/64 stop apart) | host precision choice, not a game value |
| bias, physical camera exposure | 0, off | — |
| `r.AutoExposure.LuminanceMethod` | 2 | the game's histogram meters Rec.709 luminance (`GetLuminance`) |

- The LUT cvars (`r.EyeAdaptation.ExposureCompensationCurveLUT.MinX/.MaxX/.NumSamples`) are set **before** the
  curve object exists: the renderer rebakes on a curve or sample-count change, not on a domain change. UE's
  stock domain ends below the game's upper clamp, which silently truncated the curve (Codex finding 1).
- The settings live on an unbound transient post-process volume above every volume of the map (priority 11
  in the hangar, whose own volume clamps EV100 to −3…10), spawned by `ASCViewerGameMode::StartPlay`.
  `-SCHostExposure` skips it (map exposure, UE tonemapper) for comparison captures. Log line of every run:

  ```
  VIEWER_GAME_EXPOSURE priority=11 brightness=1.561..31.121 percent=1.0..99.0 speed=2.599
    histogram=-4.034..25.966 keys=6 first=(1.561,-4.083) last=(31.121,7.868) lut=1.561..31.121/1893 display_curve=1
  ```

### 13.5 Display curve

`r_HDRTonemappingCurve` = 1 selects `HuePreservingShoulder`; `r_HDRDisplayOutput` = 0 takes the SDR branch of
`DisplayMapping` with `MaxDisplayNitsLDR` = 80 and toe 1.0. `/SCBridge/Materials/M_SC_DisplayMapping` is a
post-process material at "Replacing the Tonemapper" whose Custom node is a statement-by-statement port of
`HuePreservingShoulder`, `GetLuminance` and `ToSRGB` (scene + bloom, times the eye-adaptation exposure). Its
parameters (`MaxDisplayNits`, `RefWhiteScale`, `ToeScale`) are set at run time from the fixture; the asset's
defaults are deliberately unusable. `ue/scripts/create_display_mapping_material.py` builds it and refuses to
when one of the three function hashes in the fixture differs from the hash its port was written against.

### 13.6 Measurements

Routes as in §12.5, tags `la2` (before: `ed429a8`), `lb3` (this change with the quadratic effect glow),
`lb4` (final). All three routes exit 0; 41 `lb4` screenshots.

Light probe at the player, Cutlass armed walkthrough (lux, unshadowed / visible):

| Shot | before (`la2`) | final (`lb4`) |
|---|---|---|
| 01 hangar, under the ship | 593.0 / 0.0 | 687.3 / 0.0 |
| 16 ramp foot | 617.7 / 197.0 | 743.0 / 308.6 |
| 17 cargo bay | 725.2 / 21.9 | 1809.7 / 967.8 |
| 19 bulkhead | 1013.0 / 292.4 | 5851.0 / 5068.0 |
| 21 corridor | 839.4 / 122.8 | 2491.3 / 1642.8 |
| 22 cockpit | 915.9 / 197.1 | 10044.7 / 9325.8 |
| 24 nose to wall | 764.2 / 45.3 | 2778.1 / 2059.2 |

Ship lights are several times stronger under H-SE (the `(1 + bulb/tweak)²` term against `10 · bulb²`), and the
picture is nevertheless darker and no longer clipped: the game's compensation curve under-exposes interiors
and its shoulder keeps highlights. Gladius probe (`lb4`): hull 766.3 / 758.7, at the ladder 794.9 / 493.7.

What the captures show (half size, viewport crop, before | after):

- `gamephoto-cutlass-bay-before-after.jpg`, `-bulkhead-`, `-corridor-`, `-ramp-`: white, flat interior →
  dark metal with visible light pools, emissive strips and door frames reading as light sources; the
  corridor locker wall keeps detail instead of clipping.
- `gamephoto-cutlass-cockpit-tp-before-after.jpg`: armour was white, is green with its lit visor strips.
- `gamephoto-gladius-hull-before-after.jpg`: hull and hangar no longer clip; paint colours separate.
- `gamephoto-scorpius-hull-before-quadratic-linear.jpg`: before | quadratic effect glow (teal bloom, top
  left) | final. The red cooling emissives (Glow 0.236) are the quadratic material rule.

### 13.7 Verification

```
$ ue/scripts/build.sh SCViewerEditor                                   → BUILD_EXIT=0
$ UnrealEditor-Cmd … -run=pythonscript -script=ue/scripts/test_importer.py -nullrhi …
  WP-U1 UNIT_TESTS_OK                                                  (27 test classes)
$ … test_importer.py -SCPackage=out/dec-rv-cutlass -SCVerifyTar=cache/1272fb0b….tar
  LIGHT_VERIFY passed=1 components=225 enabled=201 disabled=24 projectors=184 gobo_textures=8
    gobo_materials=8 fallback_selected=3 shadow_flags=3 visible_shadowed=3 megalights_active=0 atlas_slots=16
  WP-U1 UNIT_TESTS_OK                                                  (29 test classes)
$ … test_player.py -SCPlayerTests -SCPlayerPackage=out/fps/player-p4-male.tar
  WP-P2 HEADLESS_OK
$ python3 -I scripts/verify_star_engine_render.py
  STAR_ENGINE_RENDER OK: 47/47 checks match the installed game
```

New test classes (added to the explicit list in `USCImportLibrary::RunUnitTests`; they were silently not
running until then): `FSCStarEngineFixture` (the checked-in fixture loads and is in domain),
`FSCStarEngineDomain` (each out-of-domain edit is refused with an error naming the rule),
`FSCStarEngineRules` (glow, instance glow, point, tweak, planar, EV, compensation segments, multiplier — on a
visibly synthetic fixture), `FSCViewerExposureSettings` (every row of the table in §13.4),
`FSCViewerExposureLut` (the baked LUT follows the keys over the whole clamp range),
`FSCViewerExposureInstall` (the volume spawns unbound with the curve and the LUT cvars), plus the material
and light cases that moved to the new units and the verifier negative cases (`FSCLightVerifier`).

Cross-review (Codex, read-only, no GPU), six findings:

1. P1 — the compensation LUT kept UE's stock domain and truncated the curve above it: fixed (domain = key
   range, cvars before the curve), test red first.
2. P1 — the loader accepted fixtures outside the rules' domain: fixed in loader and validator, tests red first.
3. P2 — reflected metering omits the game's dynamic-range rejection (`r_ExposureThresholdRange`): not
   emulated; residual below.
4. P2 — the validator could certify a stale extracted shader copy: fixed, it reads the pak in place and says
   so when it is pointed at a copy.
5. P2 — the light verifier re-implemented the importer's rules and had drifted: fixed, shared functions.
6. P3 — stale documents (host-scale emissive, H-CE, fixed exposure): fixed in this change.

The Scorpius glow regression (§13.3) was not among the findings; it was caught on the GPU capture.

### 13.8 Not verified / still open

- **Not compared with the game.** No in-game capture of these ships under a known light set-up exists here;
  every number above is derived. In particular the 10 klx cockpit, the brightness of the white door-frame
  strips and of the red Scorpius cooling emissives (22,280 cd/m²) are rule outputs, not matched results.
- Meter: UE meters reflected luminance only. The game blends an incident-light meter
  (`r_exposureMeterMode` 3, `r_exposureIncidentBlendAmt` 0.5) and rejects histogram outliers by dynamic
  range; UE's histogram has its own bucket count. An exact port needs a custom metering pass.
- Adaptation: one stage (the pupil half-life both ways); the game's slower pigment stage is not emulated.
- Glow: the per-material instance-glow factor and CPU scaling of the effect value are taken as 1; whether a
  parked ship carries the `Thrust` tag at all was not established (the viewer shows the control minimum,
  "engines on, zero thrust"); glow cards and emittance maps use the registered placeholder.
- Light rule: the CPU normalisation and the planar rule are inferences (§13.2).
- Kept from UE: bloom, lens effects. Not ported: the game's vignette, colour-grading LUT, sun shafts. The
  search for `.cfg` overrides of the cvar defaults was done once by hand and is not part of the validator.
- Views: the Gladius pilot-seat shots (01, 04–06) are black before and after; the Cutlass cockpit still
  looks out at a black hangar wall region (shot 22); the orbit camera of large ships ends outside the hangar
  enclosure; UE's red on-screen sky-light warning is in every capture. None is new. (The first two are
  fixed in §14; the other two remain.)
- Frame time was not re-measured (§3 numbers predate this change; the tonemapper replacement is one
  full-screen pass). Only the three regression ships were captured.
## 14. Cockpit glass, instance palettes and the seat start (2026-11-10)

Four defects of the views around a cockpit, all present before and after §13 (listed there under "Views"):

| Defect (before) | Cause | After |
|---|---|---|
| Gladius canopy opaque black from outside | `M_SC_Glass` used the glass colour itself as the slab's transmittance, on a slab of the default sub-surface type, which has no transmittance in a translucent blend mode | see-through; the cockpit is visible through it (`wp-u3b/glass-gladius-canopy-before-after.jpg`) |
| Opaque white cover on the Gladius spine (refuelling port) | the cover's instance names a palette the package does not carry, so no tint was applied | hull-coloured: the palette chain of §14.2 ends at the host's palette (same image) |
| Gladius pilot-seat start: black frames | the seat spawn accepted a crouched capsule 136 cm behind the seat, in the closed bay aft of the seat back | the seat spawn is refused (§14.3); the start falls to the next kind (`wp-u3b/glass-gladius-seat-before-after.jpg`) |
| Cutlass cockpit looks out at black | same glass defect seen from inside | the hangar is visible through the windscreen (`wp-u3b/glass-cutlass-cockpit-before-after.jpg`) |

### 14.1 Glass: the game's rule and the port

Source: `glassLib.cfib` `GlassShared` (called by `GlassPBR.cfxb` `GlassPBR_PS`), `paletteLib.cfib`
`LayerAttribs_ApplyGlassTint`, read from the extracted shader cache of §13.1. The base layer of a glass
surface is a clear dielectric — albedo 0, opacity 0, reflectance `MatSpecColor.rrr`, smoothness
`MatSpecColor.a` — and the only colour is absorption along the path through the layer:

```
glassUV       = baseUV × GlassTiling;  glassUV.y ×= GlassVStretch        (normal, smoothness, height and tint maps)
transmissive  = MatDifColor.rgb
%USE_TINT_PALETTE, instance palette present:  transmissive = lerp(transmissive, palette glass, GlassTintStrength)
%PER_VERTEX_TINT:                              transmissive *= vertex colour
  #elif %PER_VERTEX_CURVATURE:                 height *= saturate((vertex blue − 0.25) × 3)
%TINT_MAP (TexSlot4):                          transmissive *= tint map
%HEIGHT_MAP (TexSlot8):                        height *= height map;  clip(height map − 0.01)
density       = (1 / max(transmissive, 0.01) − 1) × 0.1
transmittance = exp(−density × path through the layer)
```

The authored `Opacity` (`MatDifColor.a`) is not read by the glass shader. What the old master did instead:
transmittance = `lerp(1, palette glass or Diffuse, GlassTintStrength)`, coverage = `Opacity`. A dark canopy
colour therefore became a dark transmittance, and the slab drew opaque on top of that.

Port:

- `FSCMaterialBuilder` (`SCMaterialDescribe.cpp` `Glass`) resolves the per-material part: `GlassColor` =
  Diffuse moved to the palette's glass entry by `GlassTintStrength` (only under `%USE_TINT_PALETTE` and when
  the instance has a palette), `SpecularColor` = `Specular.rrr`, `GlassTiling` / `GlassVStretch`, the
  vertex / map switches from the shader tokens, the TexSlot4 / TexSlot8 maps when the export carries them.
  A parameter the material does not author takes the shader's declared default from the fixture
  (`GlassTiling`, `GlassVStretch`, `GlassTintStrength`, and `ThinFilmThickness` under `%THIN_FILM`). What
  cannot be applied is reported as an import warning instead of silently dropped: `%HEIGHT_MAP` /
  `%TINT_MAP` without an exported TexSlot8 / TexSlot4, and a material without `Specular` (§14.7).
- `M_SC_Glass` (`create_materials.py` `glass_master`) reads every glass map at `glassUV`, evaluates the
  per-pixel part in two Custom nodes (transmittance at normal incidence; coverage from the height-map clip)
  and feeds
  `SubstrateTransmittanceToMFP`. The slab is a Substrate **simple volume** (`MSS_SimpleVolume`) in the
  `TranslucentColoredTransmittance` blend mode: UE refracts the view ray with the IOR of F0, applies
  Beer-Lambert along the refracted path and multiplies by (1 − Fresnel) — the same three steps as
  `GlassShared`. Coverage is 1 except where the height map clips.
- The nine constants (`0.1`, `0.01`, `0.25`, `3`, `0.01`, and the declared defaults of `GlassTintStrength`,
  `GlassTiling`, `GlassVStretch`, `ThinFilmThickness`, all `1`) are in `StarEngineRender.json`
  (`shader_constants`), reach the material as instance parameters from
  `FSCStarEngine`, and the master's own defaults are inert. `GlassShared`, `GlassPBR_PS`,
  `LayerAttribs_ApplyGlassTint` and `ReflectanceToIOR` are pinned by hash; `glass_master` refuses to build
  when `GlassShared` or `ReflectanceToIOR` differs from the hash its graph was written from.

### 14.2 Instance palette

An instance's tint palette is the first id of this chain that the package's `palettes.json` carries:

1. the instance's own `palette_id`;
2. for an interior placement, its container's `palette_id`;
3. the `DefaultPalette` attribute of the instance's material set (`authored_material_set.attributes[]`, name
   compared without case; id = `palette/<record file name, lower case>`);
4. the palette of the entity the instance is attached to (a nested child: its assembly's resolved palette;
   otherwise the root's).

This is the chain of the contract's other consumers (`vendor/StarBreaker` `blender_addon/…/importer/
orchestration.py`, `starbreaker-3d/src/pipeline/blend_assembly.rs`, `docs/decomposed-export-contract.md`
"palette resolution"). Before, the reader used step 1 only, so an instance naming an unexported palette got
none. With no resolvable candidate the authored id is kept (and the material is untinted, as before).

Gladius: the refuelling-port cover (`component_master_01`) names no exported palette and its material set's
`DefaultPalette` is `gen_glossy_dark_a`, which the exporter does not write into this package's
`palettes.json`; step 4 gives it `palette/aegs_gladius`. The game would use step 3's palette here — see
§14.7.

### 14.3 Seat start

DataCore gives a seat an attachment (`SCItemSeatParams.actorAttachment`: bone `Sit_Pos_Pilot`, stance
`Seated`), not a place to stand. The first-person start may put a (crouched) capsule at a seat only when
the capsule touches the seat: the distance from the capsule's axis segment to the seat's box is at most
radius + skin (+0.1 cm trace tolerance). The box is that of the seat item's collidable geometry, taken in the
vehicle's frame (`FSCSpawnPoint::Bounds`, vehicle-local) and tested there, so a turned or stretched vehicle
gives the same answer as an axis-aligned one; a seat with a hint but no geometry gets a zero-size box at
the hint. A free spot that does not touch the box is another place, not the seat, and the seat spawn is
refused
(`FIRST_PERSON_SPAWN_REJECT … kind=Seat … reason=no_clear_capsule`). Gladius: seat box
(772, −33, −86)…(878, 33, 56) cm, the only free spot within reach of the old ring search was 83 cm behind it.
The start ranking is unchanged (`Entrance > SeatAccess > Seat > Interior > Door > Hull`); what changed is
which seat spawns succeed. Details and results: `WP-P1.md`.

### 14.4 Front-layer translucency (A/B, left off)

`r.Lumen.TranslucencyReflections.FrontLayer.EnableForProject` and
`r.MegaLights.FrontLayerTranslucency.EnableForProject` were switched on together for one run and compared
with the committed configuration on the same camera positions. Gladius exterior: the difference is confined
to the canopy glass, at most 35/255 per channel, mean 0.66/255 over the frame; Cutlass cockpit: no visible
difference. In the uniformly grey hangar the canopy has nothing distinct to reflect, so the result says
little about a scene with bright surroundings. Both stay off (`DefaultEngine.ini` unchanged).

### 14.5 Verification

```
$ ue/scripts/build.sh SCViewerEditor                                   → BUILD_EXIT=0
$ UnrealEditor-Cmd … -script=ue/scripts/create_materials.py -nullrhi … → WP-U2 MATERIALS_OK
$ … test_importer.py -nullrhi …
  WP-U1 UNIT_TESTS_OK                                                  (29 test classes)
$ … test_importer.py -SCPackage=out/dec-rv-cutlass -SCVerifyTar=cache/1272fb0b….tar
  LIGHT_VERIFY passed=1 components=225 enabled=201 disabled=24 projectors=184 gobo_textures=8 …
  WP-U1 UNIT_TESTS_OK                                                  (31 test classes)
$ … test_first_person.py -SCPackage=out/dec-r5b-{gladius,scorpius,cutlass} -SCFirstPersonTests
  WP-P1 HEADLESS_OK   ×3
$ … test_player.py -SCPlayerTests -SCPlayerPackage=out/fps/player-p4-male.tar
  WP-P2 HEADLESS_OK
$ python3 -I scripts/verify_star_engine_render.py
  STAR_ENGINE_RENDER OK: 60/60 checks match the installed game
```

Tests written first (red on the old code): glass cases in `FSCMaterialDescribe*` on three new synthetic
submaterials (`glass_thickness`: palette + tint strength + thin film + height / tint maps + authored
`GlassTiling` / `GlassVStretch`; `glass_vertex_tint`: `%PER_VERTEX_TINT` wins over `%PER_VERTEX_CURVATURE`;
`glass_bare`: no `Specular`, `%THIN_FILM` without a thickness, `%HEIGHT_MAP` / `%TINT_MAP` without exported
slots — fixture defaults and three warnings) and the master's parameter set (`GlassTiling`, `GlassVStretch`,
no generic `Tiling`); `FSCStarEngineFixture` / `Domain` for the nine glass constants; `FSCReaderPalette`
(each step of §14.2, an unknown id at every step, nested children); `FSCFirstPersonSeatReach` (a seat with
room accepts, a seat whose only free spot is behind the back refuses, a hint-only seat, an invalid box, a
vehicle turned 45°, a vehicle turned and stretched 2×) and the start-ranking invariant in
`FSCFirstPersonSpawns` (a refused seat never hides a lower-ranked spawn that fits).

GPU (RTX 4090, default hangar, `-SCDefaults`), before → after on the three regression ships:

| Ship | Route | Result |
|---|---|---|
| Gladius | noclip stations (front of the canopy, cockpit eye point 830, 0, −70) | canopy black → see-through with the seat, dashboard and coolers visible; spine cover white → hull olive; views from inside unchanged |
| Gladius | `routes/gladius-hull-seat.txt` | `spawn Seat … ok=0`; start `Interior`; cockpit-eye shot shows the hangar through the canopy; canopy and ladder fragments toggle (end pose: §14.7) |
| Cutlass | `routes/cutlass-armed-walkthrough.txt` (29 shots) | cockpit shot 22: black → hangar wall and floor through the windscreen; the other 28 shots show no difference beyond frame noise |
| Scorpius | `routes/scorpius-smoke.txt` (4 shots) | shot 02: the opening behind the canopy shows the lit cockpit instead of black; hull shots unchanged (`wp-u3b/glass-scorpius-entry-before-after.jpg`) |

The GPU routes were run twice: once on the first implementation (the before → after of the table and of the
four evidence images) and again after the cross-review fixes of §14.6 (`out/smoke/gl3*`, 58 shots). The
second run reproduces the first: Gladius and Scorpius differ by 0.2–0.9/255 mean per frame (temporal noise),
the Cutlass walkthrough by 0.4–5.6/255, all of it at silhouette edges of the walking player and of geometry
seen from a position a few centimetres off (third-person and mid-animation shots highest); no change inside
any glass area is visible, although 67 of the 145 glass submaterials of the three ships (Gladius 7 of 23, Scorpius 1 of 33,
Cutlass 59 of 89) author a `GlassTiling` or `GlassVStretch` other than 1 — in these views the glass maps
carry too little detail for the tiling to show. Start spots and log lines are identical (Gladius seat
refused → `Interior`; Scorpius both seats refused → `Interior`; Cutlass `SeatAccess`), 0 `Error:` lines.

### 14.6 Cross-review (Codex)

Reviewed on the uncommitted change set; six findings, five fixed with tests first, one recorded.

| # | Finding | Disposition |
|---|---|---|
| 1 | P2 — the seat box was a world box carried back into the vehicle, so it grew under a rotated vehicle and a spot beside the seat counted as on it | fixed: box computed in the vehicle's frame, reach = capsule-axis-to-box distance in that frame (§14.3) |
| 2 | P2 — glass maps ignored `GlassTiling` / `GlassVStretch` | fixed: `glassUV` in the master, both parameters from the material or the fixture default (§14.1) |
| 3 | P2 — `%HEIGHT_MAP` / `%TINT_MAP` without an exported slot switched the feature off silently | fixed: import warning naming the slot |
| 4 | P2 — `%THIN_FILM` without `ThinFilmThickness` used 0; the shader declares 1 | fixed: `glassThinFilmThicknessDefault` in the fixture |
| 5 | P2 — literals outside the fixture: the 0.04 reflectance for a material without `Specular`, and (older, shared by every master) the gloss cap 251/255, the Rec.709 weights in `conserve`, the host `Shininess` default | not fixed here: the 0.04 case now raises a warning and carries a `TODO(WP-U2)`; the three shared ones are a cross-master change, listed in §14.7 |
| 6 | P3 — an invalid seat box reached the reach test | fixed: normalised to the zero-size box at the hint |

On the three regression ships none of 3, 4 or the `Specular` half of 5 occurs (all 145 glass submaterials
author `Specular`; all 61 `%THIN_FILM` ones author a thickness; all 6 `%HEIGHT_MAP` and 2 `%TINT_MAP` ones
have their slot), so those fixes change no pixel there.

### 14.7 Not mapped / still open

- **Not compared with the game.** The glass rule is ported; no in-game capture under a known light set-up
  was matched.
- Glass features of `GlassShared` that are not mapped: dirt layer and wear smoothness, the game's thin-film
  LUT (approximated by `SubstrateThinFilm`: the game's `ThinFilmThickness` scales a LUT coordinate, it is
  not a physical thickness, and `ThinFilmStrength` is not mapped), screen-space refraction offset, curvature
  and glare, light amplification, emissive glass (`EmissiveOpacity`), cracks, frost, condensation and the
  glass decal layer.
- **Host values still outside the fixture** (cross-review finding 5): F0 0.04 for a glass material without
  `Specular` (Substrate's default dielectric, not the engine's unauthored `MatSpecColor`; warned at import),
  the gloss cap 251/255, the Rec.709 weights in `conserve` and the host `Shininess` default. To be derived
  or registered as host fallbacks in WP-U2.
- UE blends a simple-volume slab's diffuse towards the volume for very dark glass; not measured against the
  game's result for the same colour.
- Front-layer Lumen reflections / MegaLights on translucency are off (§14.4).
- **Palettes the exporter does not write.** A material set's `DefaultPalette` that no entity of the package
  references is missing from `palettes.json` (Gladius `gen_glossy_dark_a`), so step 3 of §14.2 cannot
  resolve and the instance takes its host's palette. Exporter fix (vendored submodule): write every
  palette a sidecar names.
- **Seated state.** No seat of the Gladius or the Scorpius accepts a capsule; the pilot's eye point needs the
  seat's idle clip and the rig's camera joint with collision off while seated (`WP-P1.md` follow-ups).
- **Open / Close clips end in each other's pose** on the Gladius canopy and several panels: an exporter
  defect in the pairing of DBA clip names with key blocks, recorded with its evidence in `WP-P1.md`
  ("Follow-ups"). The viewer plays what the package says.
- UE's red on-screen warning about the real-time-capture sky light is still in every capture.
- Frame time was not re-measured; only the three regression ships were captured.
