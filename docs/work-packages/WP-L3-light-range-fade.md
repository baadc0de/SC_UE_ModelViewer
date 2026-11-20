# WP-L3 — Light range window: the game's forced fade instead of UE's radius mask

Branch `wp/L3-light-forced-fade`. Owner question: do the hangar lights' ranges translate correctly (scale, or the
falloff function that decides how far a light reaches)? Compared on the Aeroview high-tech medium hangar
(`Data/ObjectContainers/PU/loc/mod/common/hangar/hightech_a/hangar_medfrnt_001.socpak`, pad "A1") against an Alpha
3.10 marketing screenshot of the same hangar.

## How a game light becomes a UE light (before this WP; part 2 adds the tweak offset)

| Game field (`EntityComponentLight`) | Export (`scene.json` light) | UE (`FSCLightImporter::Build`) |
|---|---|---|
| `sizeParams.lightRadius` (m) | `radius_m` | `AttenuationRadius = radius_m × 100` (cm) |
| `sizeParams.bulbRadius` (m) | `bulb_radius_m` | `SourceRadius = bulb × 100`; candela term |
| `miscParams.attenuationTweak` | `attenuation_tweak` (1 when unset) | candela term |
| state `intensity` | `intensity_raw` | `I_cd = π · 1000 · raw · (1 + bulb / tweak)²` (`FSCStarEngine::PointCandela`), inverse-square falloff |
| `sizeParams.planeWidth/Height` | `plane_width_m/height_m` | rect light size; `I_cd = π · 1000 · raw · w · h` |

Units and scale are right: metres → centimetres once, the radius is the game's cut-off radius, nothing is scaled twice.

## The game's falloff (decoded `Engine/ShaderCache_Vulkan.pak`, read in place from `Data.p4k`)

`ShadeLib.cfi`:

```
GetLightForcedDistanceFade(d, 1/R, F)       = saturate(F · (1 − d / R))²
GetPhysicalLightAttenuation(d, 1/b, tweak)  = 1 / (max(tweak · d / b − tweak, 0) + 1)²
```

`MergedTiledShading.cfi` multiplies every non-ambient light by the forced fade with `F = attenuationParams.z`
(CPU-filled); `lightbeam.cfxb` and `volumetricfog.cfxb` pass `F = 1 / 0.2` literally. So a game light is at full
inverse-square strength until 0.8 R and fades quadratically over the last 20 % of its radius.

UE's inverse-squared local light (`DeferredLightingCommon.ush` `GetLocalLightAttenuation`) uses `(1 − (d / R)⁴)²`
instead. Both reach zero at R, but the shapes differ:

| d / R | 0.5 | 0.6 | 0.7 | 0.73 | 0.77 | 0.8 | 0.9 | 0.95 |
|---|---|---|---|---|---|---|---|---|
| game | 1 | 1 | 1 | 1 | 1 | 1 | 0.25 | 0.0625 |
| UE | 0.88 | 0.76 | 0.58 | 0.51 | 0.42 | 0.35 | 0.12 | 0.03 |

## What it does to the hangar

The pad is lit almost entirely by two ceiling projectors (`lg_hangar_medfrnt_001_light-001-009` / `-157`: raw 2000,
bulb 1.5 m, `lightRadius` 75 m, FOV 100°, 3.9·10⁷ cd) 55–58 m from the pad, at 0.73–0.77 R: UE drew them at 0.42–0.52 of
the game's strength while nearer fixtures were unaffected, so the pad pool lost about half its light against its
surroundings. Floor illuminance from the exported lights (no occlusion, same cone for both, model script in the
session notes): pad centre band UE 8,000–10,000 lux vs game 14,000–16,000 lux.

## Change, part 1: the range window

- Fixture: `shader_constants.forcedFalloffFraction` = 0.2 (`lightbeam.cfxb`) and the token hash of
  `GetLightForcedDistanceFade` (`ShadeLib.cfib`); `FSCStarEngine::ForcedFadeFraction` (domain 0 < f ≤ 1) and
  `LightRangeFade(d, R)`; the light probe (`FSCLightImporter::Probe`) uses it.
- Engine shader patch (plan D14, then `ue/scripts/engine_patches.py`; since WP-E1 E1-2 the fork's `ELightRangeFade::SquaredLinearTail`,
  and the patch mechanism is removed, E1-7): `GetLocalLightAttenuation` and the light grid's
  `EstimateLocalLight` (`LightCulling.ush`, MegaLights sampling / culling) use `saturate((1 − d / R) / f)²`. Every
  shading path that calls `GetLocalLightAttenuation` follows (deferred, MegaLights, Lumen scene lighting,
  translucency, volumetric fog, heterogeneous volumes). Light culling is unchanged: both windows end at R.

## Result of part 1 (Win64, D3D12 SM6, RPC harness, no ship, auto exposure; images under `out/hangar-light/`)

Same three camera poses before and after (`shots/before_*.png`, `shots/after_*.png`; sheets `before_after.png`,
`before_after_crops.png`). Mean linear luminance of screen regions under the viewer's own exposure:

| View, region | before | after | change |
|---|---|---|---|
| top, pad centre | 0.0839 | 0.1304 | +55 % |
| top, pad sides | 0.0926–0.1008 | 0.1009–0.1103 | 0 to +19 % |
| top, side walkways | 0.0818–0.0839 | 0.0737–0.0860 | −10 to +3 % |
| reference pose, mid pad | 0.1089 | 0.1524 | +40 % |
| reference pose, near pad | 0.1426 | 0.1742 | +22 % |
| reference pose, far pad / upper walls / right walkway | 0.1006 / 0.1104 / 0.3175 | 0.0971 / 0.1056 / 0.3012 | −3 / −4 / −5 % |

Centre-to-walkway ratio in the top view: 1.03 → 1.77. Astra (GPT Astra, `astra/reply_*_raw.txt`) on reference vs
before: the pad pool is "less pronounced", with too broad a floor wash on the side walkways; on reference vs after:
"closer in relative pad brightness", and the walkway wash is now the largest remaining difference (part 2),
followed by exposure / post-processing and GI. The reference is Alpha 3.10 marketing art with redesigned wall
modules, haze and a darker grade, so it can't be matched pixel for pixel.

## Change, part 2: the attenuation tweak's distance offset

For a point or spot light the game's curve, normalised as `PointCandela` assumes (the authored intensity 1 m from the
bulb surface), is exactly

```
E(d) = I_cd / (max(d, bulb) + c)²,   c = bulb · (1 − tweak) / tweak        (FSCStarEngine::AttenuationOffset)
```

Inside the bulb that is the game's flat `I_cd · tweak² / bulb²`. `SCBridge.StarEngine.Rules` checks the identity
against the decoded `GetPhysicalLightAttenuation` for bulbs 0.05 / 0.5 / 8 m, tweaks 0.05 / 0.3 / 1 / 2, and distances
inside and outside the bulb. UE has `I_cd / d²`: right for tweak 1, wrong wherever the tweak is not 1.
`hangar_medfrnt_001` has 62 such lights, with c from −0.25 m (tweak 2) to +18.7 m. The six fill lights (bulb 8 m,
tweak 0.3, R 55 m, 9.4 m above the floor by the side walls) were 2.6× (30 m) to 8× (10 m) too bright near themselves.

Carrier: UE has no free per-light shader field. `SoftSourceRadius` comes closest:

- it reaches every light path: deferred uniforms, the light grid (f16), Lumen's packed lights, GPU scene;
- it is never used for shadows (no reference in `Renderer/Private` beyond the packing);
- shading reads it in one place only: `GetCapsule` → `SphereSinAlphaSoft`, which widens sphere-light specular;
- the importer never set it.

So:

- **Importer:** `SetSoftSourceRadius(AttenuationOffset(bulb, tweak) × 100)` for authored point and spot lights. It is
  0 for tweak 1 and negative for tweak > 1. The runtime setter does not clamp; only the editor's
  `PostEditChangeProperty` would.
- **Engine patch:**
  - `GetCapsule` sets `SoftRadius = 0`, so UE no longer reads the field as a soft specular radius.
  - `GetLocalLightAttenuation` multiplies the mask by `d² / (max(d, SourceRadius) + SoftSourceRadius)²` for non-rect
    lights with a non-zero offset.
  - `EstimateLocalLight` (`LightCulling.ush`) scales the light-grid power estimate the same way.
- **Probe:** uses `(max(d, bulb) + offset)` as the distance.
- **Not covered:**
  - The mobile shading path zeroes `SoftSourceRadius` when capsule lights are off (`MobileOverrideDeferredLightParameters`):
    there the falloff degrades to plain inverse square, i.e. the pre-WP-L3 behaviour. Whether the Web build takes
    that path was not checked.
  - Editing such a light's Soft Source Radius in the editor would clamp a negative offset to 0.

Test first: with `AttenuationOffset` in place and the importer unchanged, `SCBridge.Lights.AuthoredCryEngineModel`
failed (`Expected 'tweak offset is carried (cm)' to be 30.000000, but it was 0.000000`). It passed after the importer
change. 9/9 SCBridge.StarEngine / SCBridge.Lights tests green.

## Result of part 2 (same poses; `shots/offset_*.png`, sheet `three_stages.png`: before / + fade / + offset)

| View, region | before | + fade | + offset |
|---|---|---|---|
| top, pad centre | 0.0839 | 0.1304 | 0.1459 |
| top, side walkways (L / R) | 0.0818 / 0.0839 | 0.0737 / 0.0860 | 0.0438 / 0.0761 |
| top, centre / left walkway | 1.03 | 1.77 | 3.33 |
| along the hangar, right floor / right wall | 0.4579 / 0.1309 | 0.4416 / 0.1252 | 0.2302 / 0.0702 |
| reference pose, mid pad / far pad / upper walls | 0.1089 / 0.1006 / 0.1104 | 0.1524 / 0.0971 / 0.1056 | 0.1721 / 0.0548 / 0.0676 |

Astra on reference vs part 2 (`astra/reply_offset_raw.txt`): "closer, especially along the back-left border"; the pad
now reads as a lit area separated from the walls by a darker strip. Still different, ranked:

1. Too little brightness variation within the pad. This is the projectors' footprint (cone / gobo shape), a separate
   question.
2. A pale right perimeter that follows the redesigned panels: set dressing and material.
3. Harsher wall bases: exposure, post-processing and GI.

## Part 3: light shape, white balance, tone mapping, GI (owner follow-up)

### Light shape: projectors are their gobo over a square frustum

`MergedTiledShading.cfi` `TiledLight_ProcessLight` starts every light at `attenuation = 1`. Only planar lights get
`GetSpotAttenuation`. A projector (`IsLightProjector`) is multiplied by
`SpotTexArray.SampleLevel(SampStateTrilinearBorder, projectorMatrix · position)`, plus the forced fade and the physical
attenuation: there is no cone falloff, and nothing outside the texture's square (border sampler).

The viewer instead gave every projector a round UE cone. The outer angle was FOV / 2, and the inner angle was
0.8 · FOV / 2, a factor the exporter invents (`socpak.rs`, `fov * 0.5 * 0.8`; not a game value; flagged for
upstream). For the hangar's square gobos (`rect_full_*`, `Square_Light_Large`, `light_square_soft_edge`; all 241
spots here have a gobo) that cut the corners and started dimming at 64 % of the frustum for a 120° FOV.

Change:
- **Importer.** A spot whose gobo binds gets outer = inner = atan(√2 · tan(FOV / 2)), the frustum corners, capped at
  UE's 88.9° inner-cone limit (`USpotLightComponent::GetClampedConeAngles`). `LightFunctionScale` lateral / axial =
  tan(FOV / 2) / tan(rendered outer) keeps the gobo on the FOV frustum, since UE maps light-function UVs by the tangent
  of the outer angle it renders (`LightFunctionAtlasCommon.usf`, `LightFunctionAtlas.cpp`; the renderer swaps the
  scale's X and Z, so `.Z` is along the light and `.X/.Y` are lateral). The rendered outer angle is the component's
  clamped one, at least 0.001 rad wider than the inner, so the importer reads it back with `GetClampedConeAngles()`
  rather than using the angle it set (a 0.2 % gobo shrink at FOV 100° otherwise). The rule is
  `FSCLightImporter::ProjectorCone`.
- **Not modelled:** the game also zeroes a projector in front of its near depth (`projDepthCutoff`,
  `attenuationParams.w`, CPU-filled); UE has no such plane.
- **Border.** `M_SC_LightFunction` returns 0 outside UV [0, 1] (`create_light_material.py`; needs
  `-AllowCommandletRendering`). The light-function atlas, which MegaLights uses and which wraps UVs with `frac`, is
  engine-patched to zero spot light functions outside [0, 1].
- **Superseded (WP-E1 E1-3).** The engine fork renders the square frustum itself: the importer sets `Shape` SquareFrustum
  with the half-FOV as outer angle and `LightFunctionAddress` Border (`FSCLightImporter::MakeProjector`); the corner cone,
  the lateral `LightFunctionScale`, the material's border node and the atlas patch are gone. See WP-E1 "Result (E1-3)".
- **Tests.** `SCBridge.Lights.ProjectorCone` covers the corner angle, the UV edge at the FOV for the angle UE renders, the corners inside the rendered cone, and the 88.9° limit.
  `SCBridge.Rig.Lights` checks that a projector with a bound gobo gets the frustum and that one without keeps its
  cone.

Result (`shots/p3_shape_*`, sheet `shape_compare.png`):

| View, region | before part 3 | after |
|---|---|---|
| top, side walkways | 0.044 / 0.076 | 0.210 / 0.190 |
| top, pad centre / left walkway | 3.33 | 0.62 |
| reference pose, far pad / upper walls / elevator surround | 0.055 / 0.068 / 0.080 | 0.114 / 0.097 / 0.133 |

Wall washes, the elevator surround and the walkways now take the light of the square gobos' full frustum. Against
the 2020 reference this moves away: Astra calls it "broader and flatter", with the pad pool losing its separation.
The game shader leaves no room for a cone, so the change stays; the owner confirmed keeping it (2026-11-18). The 2020 shot predates the current light set
(Alpha 3.10 against 4.10.2 data, modules redesigned). **Not verified against a current in-game capture of this
hangar; that is the evidence that would settle it.**

### White balance and grading

- The hangar's `ColorGradient` entity (`cg_hngr_medfrnt_ht-001`, priority 40) uses
  `textures/colorcharts/generic/default_cch.dds`. That is a 16³ colour chart and it is exactly the identity (all
  4,096 entries, 0 of 255 deviation). So the game applies no grade or white-balance shift here; its `GradingLutGen`
  blends up to eight such layers.
- The viewer is neutral too. The Viewer map's only post-process volume overrides exposure only, and
  `M_SC_DisplayMapping` (the game's `HuePreservingShoulder`) replaces UE's tonemapper and its grading.
- **Light colours (open).** 465 of the hangar's 683 lights are temperature-driven (2100 K ×94, 15000 K ×61,
  1000 K ×47, …). The exporter converts with Tanner Helland's approximation (`socpak.rs` `kelvin_to_rgb`), which is
  display-encoded RGB used as linear light colour: a 2100 K light is (1, 0.56, 0.11) as exported against
  (1, 0.27, 0.01) linearised. The game's conversion is CPU-side (`colorTemperature` in `StarCitizen.exe`): neither
  Helland's nor Krystek's (UE's) constants occur in the executable, and no shader converts temperatures. Settling
  it needs a disassembly of the executable, and the validator's `objdump` is not installed on this host. Unchanged
  until then.

### Tone mapping and exposure

The display curve, the compensation curve and adaptation are the game's (WP-U3b §13). One remaining difference is
decoded here: `OpticsExposure.cfxb` `CalcLogLumMetered` with `r_exposureMeterMode` 3 (`MeterMode_ClampedBlend`) and
`r_exposureIncidentBlendAmt` 0.5 meters `clamp(lerp(logLumReflected, logLumIncident, 0.5), reflected − 1.5,
reflected + 1.0)`. The incident meter samples the light arriving at the camera and scene (`exposureincident.cfib`).
UE meters reflected luminance (current) or, experimentally, albedo-free (`r.AutoExposure.IgnoreMaterials`); it
cannot blend the two without renderer changes.

Bracket (`shots/p4_meter_*`, with part 3): albedo-free metering darkens the displayed mean from 0.179 to 0.118
(reference pose) and from 0.148 to 0.066 (top view), about 0.6 and 1.2 stops. The game's meter is halfway in log
between the two, clamped to +1 / −1.5 stops of reflected. So our image is likely about 0.3–0.6 stops brighter than
the game would expose it, which also lifts the dark surroundings that separate the pools. UE's albedo-free meter is
not the game's incident meter (which samples light arriving at the camera and scene), so this brackets the error and
does not reproduce it. Doing it properly needs a change to UE's eye-adaptation pass: the reason for the source-built
engine (`docs/engine-source-build.md`, in progress in a separate session).

### GI

The hangar's ambient in the game is `EnvironmentLight` probes (`cm_*`, about 20; the main one box-projected over
140 × 80 m) captured at run time from the lit scene (`runtimeGBuffer=1`, no authored cubemap, intensity 1,
`numDiffuseBounce` 0). That is the role Lumen plays here, and there is no authored GI data to import.

Measured in the viewer: Lumen GI off lowers the left wall base 0.0197 → 0.0138 and the upper walls 0.068 → 0.058.
`r.LumenScene.DirectLighting.MaxLightsPerTile` 8 → 32 changes nothing (identical to four digits), so Lumen is not
starved of lights. No GI change was made: there is no game value to move it to. The game's probe ambient is spatially
uniform within a box (darkened only by SSDO, which the look D15 replaced with Lumen's AO), while Lumen is locally
occluded. That is the likely reason for the deeper recesses Astra notes.

## Owner decision (2026-11-20)

The square projector frustum (WP-E1 E1-3) stays as it is: with the gobos back in the atlas it reads correctly. Fog
(volumetric) will be brought in later and may revisit how projector beams read.
