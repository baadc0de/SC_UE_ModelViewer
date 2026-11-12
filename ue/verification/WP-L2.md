# WP-L2 verification — SSDO for unshadowed lights and ambient light

Brief: [docs/work-packages/WP-L2-unshadowed-occlusion.md](../../docs/work-packages/WP-L2-unshadowed-occlusion.md).
UE 5.9.0 preview-1, Vulkan SM6, MegaLights (DownsampleMode 0), 2026-11-12.

## Game data

```
$ uv run python scripts/verify_star_engine_render.py
STAR_ENGINE_RENDER OK: 73/73 checks match the installed game
```

New entries: `exe_cvars` `r_ssdo` 1, `r_ssdoQuality` 0, `r_ssdoRadius` 4.0, `r_ssdoStrength` 2.0,
`r_ssdoAmountDirect` / `Ambient` / `Reflection` 1.0; `shader_functions` `DirOcclusionPS`, `SpatialDenoisePS`,
`GetSSDORadius` (DeferredShading.cfxb), `CalculateSSDO` (ShadeLib.cfib).

## Engine patch

```
$ uv run python ue/scripts/engine_patches.py apply
WROTE .../Shaders/Private/MegaLights/MegaLightsShading.usf
WROTE .../Shaders/Private/MegaLights/SCSSDO.ush
WROTE .../Shaders/Private/MegaLights/SCSSDOParams.ush
ENGINE_PATCHES applied (3 changed)
$ uv run python ue/scripts/engine_patches.py check
ENGINE_PATCHES OK: 3/3 files current
```

The MegaLights shading shaders recompiled at the next `-game` start with no shader errors in the log.

## GPU route

`ue/scripts/capture_route.sh l2-ssdo ue/scripts/routes/cutlass-ssdo.txt -SCDefaults -SCLoad=DRAK_Cutlass_Black
-SCCamera=first` (1600x900 window, VSync off). Per view: SSDO off (`r.GeneralPurposeTweak 0`) and on (1), two shots
0.2 s apart, 5 s frame sample. Mean = grey mean of the whole window (0–255); noise = pixels differing > 2 % between
the two shots of one setting; change = pixels differing > 2 % between off and on.

| View | GPU ms off → on (mean) | Mean off → on | Noise off / on (px) | Change off→on (px) |
|---|---|---|---|---|
| Cargo bay | 7.06 → 7.41 (+0.35) | 30.44 → 30.13 | 94 / 85 | 1 173 |
| Cockpit, looking aft | 7.39 → 7.52 (+0.13) | 35.03 → 34.87 | 153 / 176 | 2 216 |
| Cockpit, looking forward | 7.28 → 7.40 (+0.12) | 36.87 → 36.86 | 126 / 106 | 49 |

Shots: `out/smoke/l2-ssdo-<view>-<off|on>-<a|b>.png` (game-derived, not in git).

Reading: the patch is active and local — the ×10 difference image lights up at contact edges, crevices, the lower inner
faces of pillars, the undersides of the ceiling truss and door frames; open surfaces facing their lights do not change
(the cone only cuts lights outside the bent-normal cone). The frame-to-frame noise does not grow (the per-frame noise
rotation is absorbed by MegaLights' temporal denoiser). The cockpit forward view barely changes: its lighting is
dominated by Lumen indirect light and emissive screens, which SSDO does not touch in this WP.

## Projection constants (tuning check)

`projScale`, `invProjScale` and `rcpAspectRatio` are not in the shader text. The `r_ssdoRadius` variable (registered
with `lea r8,[rip+…]` → `0x149f4a91c`) is read by one constant fill, StarCitizen.exe 4.10.2:

```
1407eb933  vmovss xmm8,[0x147eb7b7c]        ; 0.5
1407eb951  vmovss xmm0,[r15+0x334]          ; P[1][1] (projection at r15+0x320)
1407eb95a  vmulss xmm2,xmm0,xmm8            ; projScale    = 0.5 * P11      -> [rbp+0x140]
1407eb970  vdivss xmm0,xmm7,xmm0            ; invProjScale = 2.0 / P11      -> [rbp+0x144]  (xmm7 = [0x147eb7d80] = 2.0)
1407eb98c  vdivss xmm2,xmm1,[r15+0x334]     ; rcpAspect    = P00 / P11      -> [rbp+0x148]
1407eb97c  vmovss xmm0,[0x149f4a91c]        ; r_ssdoRadius                  -> [rbp+0x14c]
1407eb9b7  vmovss xmm1,[0x149f4a920]        ; r_ssdoStrength                -> [rbp+0x158]
```

The port's `0.5 × View.ViewToClip[1][1]` and `ViewSize.y / ViewSize.x` (= P00 / P11 for a symmetric projection) are
these values, so every SSDO parameter is the game's; no value is tuned.

## Debug view

`ue/scripts/capture_route.sh l2-ssdo-dbg ue/scripts/routes/cutlass-ssdo-debug.txt -SCDefaults
-SCLoad=DRAK_Cutlass_Black -SCCamera=first`: AO (`r.GeneralPurposeTweak2 2`) and bent normal (3). The AO is dark in
crevices, at the floor–wall and floor–door junctions, under the ceiling truss and along panel seams, and near white on
open walls and floors; the bent normals vary smoothly, without sign flips. The port is behaving; the small change in
the lit image is the game's rule (`CalculateSSDO` only cuts light arriving outside the bent-normal cone, and Lumen
indirect light, which SSDO does not touch here, carries much of the interior).

## Ambient (second patch, same route)

`engine_patches.py apply` → `ENGINE_PATCHES OK: 4/4 files current` (adds `DiffuseIndirectComposite.usf`).
Route `cutlass-ssdo.txt` again (tag `l2-amb`): off = no SSDO, Lumen short-range AO; on = the game's SSDO on unshadowed
lights and on Lumen's diffuse indirect / rough specular. Mean = grey mean of the viewport crop.

| View | GPU ms off → on | Viewport mean off → on | Noise off / on (px) |
|---|---|---|---|
| Cargo bay | 6.86 → 7.29 (+0.43) | 27.54 → 25.49 (−7.5 %) | 76 / 100 |
| Cockpit, looking aft | 7.16 → 7.55 (+0.39) | 44.52 → 43.31 (−2.7 %) | 141 / 142 |
| Cockpit, looking forward | 7.19 → 7.36 (+0.17) | 45.60 → 44.36 (−2.7 %) | 85 / 93 |

The darkening now reaches the ambient-lit structure: the ceiling truss, door frames and seams, the cockpit interior
seen through the door, the handrail and floor next to the walls. The ambient SSDO has no MegaLights temporal denoiser
behind it (only TSR), so the per-frame noise rotation shows as a little more frame-to-frame change in the cargo bay.

Skipping Lumen's short-range AO (`r.Lumen.ScreenProbeGather.ShortRangeAO 0`, route `cutlass-ssdo-cost.txt`) saves
nothing measurable (cargo 7.11 / 7.25 / 7.10 ms with it on / off / on, aft 7.56 / 7.39 / 7.40), so it stays on as the
A/B baseline.

Not covered: the depth mip chain (mip 0 is read); colour bleeding's exponent (`r_ssdoColorBleeding`, from the game's
SSGI pass; Lumen computes the bounce itself); the non-Substrate composite path (the project uses Substrate, D12).
