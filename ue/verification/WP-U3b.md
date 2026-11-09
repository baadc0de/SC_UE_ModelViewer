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

**Mapping (kept).** UE candela = `intensity_raw × 1500` = `intensity_candela_proxy`
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
- TODO(WP-U2): re-tune `GlowToEmissive` for auto exposure (or derive Glow → cd/m²).
- TODO(WP-R5): export `shadowParams` (cast-shadow flag) and `planeWidth`/`planeHeight`; the
  shadow rule and the 64 cm rect default are stand-ins for them.
- TODO(WP-V2): light-state selection (default/auxiliary/emergency/off); the Gladius hangar
  reference likely shows a parked state without the wing Omnis.
- TODO(WP-U3b): MegaLights is "Experimental" in 5.8.2; recheck on engine upgrades and keep the
  fallback rule working (the patch keys it on the same CVar).
