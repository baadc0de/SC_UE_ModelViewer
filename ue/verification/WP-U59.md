# WP-U59 — Unreal Engine 5.9 and the Lumen World Cache

Host: UE **5.9.0 preview-1** (`5.9.0-60351404+++UE5+Release-5.9`) at `~/UnrealEngine/5.9`, Vulkan SM6,
RTX 4090, Star Citizen LIVE 4.10.2. It replaces 5.8.2 (plan D9); the 5.8 install was removed after the
checks below passed. Decision: plan D13 — the World Cache is **off** (`r.Lumen.WorldCache.Supported=0`) and the adaptive Substrate
G-buffer stays (see "Decision" at the end).

## Install

Epic's Linux archive (unrealengine.com/linux) lists only `Linux_Unreal_Engine_5.9.0_preview-1.zip` for 5.9
(41,704,882,698 bytes, 315,407 entries, 81.3 GB uncompressed). It was extracted entry by entry from the
end of the zip, checking each entry's CRC-32 and size and truncating the zip behind it, so the peak disk use
was the extracted size instead of zip + extracted. All 315,407 entries verified; 7,578 executables kept
their mode bits.

## Build

`ue/scripts/build.sh SCViewerEditor` — `Result: Succeeded`, no warnings from project or plugin code. Targets
moved to `BuildSettingsVersion.V8` / `EngineIncludeOrderVersion.Unreal5_9` as in the 5.9 templates (V8 only
stops compiling `Tests/` folders; our automation tests live in `Private/`).

## Headless (null RHI)

```bash
E="$HOME/UnrealEngine/5.9/Engine/Binaries/Linux/UnrealEditor-Cmd"; P="$PWD/ue/SCViewer/SCViewer.uproject"
C="-nullrhi -unattended -nop4 -nosplash -ddc=SCViewerDDC"
"$E" "$P" -run=pythonscript -script="$PWD/ue/scripts/test_importer.py" $C
for n in cutlass gladius scorpius; do
  "$E" "$P" -run=pythonscript -script="$PWD/ue/scripts/test_first_person.py" -SCFirstPersonTests -SCPackage="$PWD/out/dec-p3-$n" $C
  "$E" "$P" -run=pythonscript -script="$PWD/ue/scripts/test_rig.py" -SCPackage="$PWD/out/dec-p3-$n" $C
done
uv run python ue/scripts/make_test_tar.py out/dec-p3-cutlass ue/SCViewer/Saved/Import/u59-cutlass.tar
"$E" "$P" -run=pythonscript -script="$PWD/ue/scripts/test_importer.py" -SCPackage="$PWD/out/dec-p3-cutlass" \
  -SCVerifyTar="$PWD/ue/SCViewer/Saved/Import/u59-cutlass.tar" $C
```

| Suite | Result |
|---|---|
| importer unit tests | 35 passed, 0 failed, `UNIT_TESTS_OK` |
| importer + Cutlass package + tar | 37 passed, 0 failed, `UNIT_TESTS_OK` |
| first person ×3 | `WP-P1 HEADLESS_OK`, `FSCFirstPersonCollision passed=1` on Cutlass, Gladius, Scorpius |
| rig ×3 | `WP-U3 RIG_TESTS_OK`, 0 errors; `RIG_VERIFY passed=1` (Cutlass clips 55/bound 53, Gladius 64/63, Scorpius 75/71) |

## GPU

```bash
SC_TIMEOUT=3600 ue/scripts/capture_route.sh u59-wc-warm ue/scripts/routes/cutlass-world-cache.txt \
  -SCDefaults -SCLoad=DRAK_Cutlass_Black -SCCamera=first
SC_TIMEOUT=1800 ue/scripts/capture_route.sh u59-cutlass ue/scripts/routes/cutlass-ramp-to-cockpit.txt \
  -SCDefaults -SCLoad=DRAK_Cutlass_Black -SCCamera=first
```

**World Cache runs but stays empty (blocker).** The config sets `r.Lumen.WorldCache.Supported:1`, the cache
buffers are allocated and `ProfileGPU` shows `LumenWorldCache` with its `TracePaths Bounce0/1`, `LastBounce` and
`Update` passes (no Surface Cache passes) — but the passes take ~0.013 ms for 14,355 paths and the
`r.Lumen.WorldCache.Debug 1` overlay reads `Entries 0, NewThisFrame 0, Samples 0` in the cargo bay, the cockpit and
looking at the stock Viewer hangar, with async compute on or off and with `r.RayTracing.SimplifiedMaterialOverride`
0 or 1. Cause (engine shaders, 5.9.0 preview-1): the path tracer's first bounce reads the G-buffer in a ray
generation shader (`LumenWorldCacheTracePaths.usf`, `ReadMaterialData`); with Substrate's adaptive G-buffer
(`SUBSTRATE_GBUFFER_FORMAT==1`, plan D12) that read is `InternalReadMaterialData_Substrate_Regular`, whose body is
compiled only when `SUBSTRATE_MATERIALCONTAINER_IS_VIEWRESOURCE`, which `Substrate.ush` defines as 0 for ray
generation shaders — so every pixel is invalid and every path returns before tracing. The trace shader special-cases
only the blendable G-buffer (format 0). Consequence, measured with `routes/cutlass-world-cache-control.txt`: clearing
the cache every frame changes nothing (RMSE 1.3–1.5/255, the noise floor), while the Surface Cache
(`r.Lumen.WorldCache.Allowed 0`) is brighter in the indirect-lit areas (aft view: dark band 3.2 → 7.0, shadow band
17.9 → 24.1, mean 38.1 → 44.1 of 255): with this G-buffer the World Cache currently *loses* indirect light.

**MaxBounces sweep** (`routes/cutlass-world-cache-bounces.txt`, 3 / 5 / 10 / 3 at three views): trace-pass cost
0.04 / 0.06 / 0.17 ms, whole-frame GPU unchanged within noise (7.1–7.9 ms); images identical within the 3-vs-3
noise floor in dark, shadow and lit bands. With an empty cache this measures nothing about bounce lighting; it has
to be repeated once the cache fills. `routes/cutlass-world-cache-warmup.txt` (freeze 50 ms after a reset) is the
fill-speed check for the same rerun.

**Regression route.** `cutlass-ramp-to-cockpit` runs all 81 steps; every shot lands within 2.1 cm of the
5.8.2 run of the same route (`p3c-cutlass`, WP-P3), wall/bulkhead blocks, noclip refusal and fall recovery
behave the same. Shots 02/05/08/10/12 side by side (`out/smoke/u59-vs-58-cutlass.jpg`): same geometry,
materials, emissives and lights; mean viewport luminance within 1.5 % (5.9 marginally darker in the darkest
areas). Frame cost, first person / orbit / first person:

| | frame ms (mean) | GPU ms (mean) |
|---|---|---|
| 5.8.2, Surface Cache (`p3c-cutlass`) | 7.80 / 6.30 / 8.04 | 6.12 / 5.71 / 6.10 |
| 5.9.0, World Cache (`u59-cutlass`) | 11.53 / 9.10 / 9.92 | 7.38 / 6.37 / 7.34 |

The 5.9 frame times were taken while an unrelated `rustc` build loaded the CPU (no other GPU client);
the GPU column is the comparable one: +1.2 ms in first person, of which ~0.6 ms is the World Cache.

Known, unchanged from 5.8.2: the transient `Invalid Lod 0 for Rendering Asset: SkeletalMesh` warnings during
assembly (also in the 5.8.2 logs) and the red on-screen message at the top of the viewport (present in the
5.8.2 shots too). MegaLights is still marked Experimental in 5.9.0.

## Blendable G-buffer trial (`r.Substrate.ProjectGBufferFormat=0`)

Routes: `routes/cutlass-gbuffer-ab.txt` (cargo bay, cockpit aft, cockpit forward through the canopy; each through
the World Cache and the Surface Cache) and `routes/cutlass-gbuffer-ab-exterior.txt` (orbit view; Gladius, since the
Cutlass orbit framing at 85 m puts the camera outside the Viewer hangar and renders black in both formats). First
launch after the switch: ~5 min of global/material shader compiles; both formats then start warm from the DDC.

**The cache fills.** Cargo bay overlay: `Entries 45617, UsedThisFrame 8936, NewThisFrame 152, Utilization 4.35 %`;
trace passes 0.15 + 0.19 + 0.04 ms (vs 0.016 ms of early-outs on the adaptive format). Whole-frame GPU in the cargo
bay: World Cache 7.26 ms, Surface Cache 6.57 ms.

**World Cache vs Surface Cache, blendable** (0-255 grey, viewport): dark band +0.5..+1.0, shadow band +0.6..+1.8 in
every view — the extra bounce light the cache is meant to add; lit surfaces within ±2.

**Blendable vs adaptive, both on the Surface Cache** (material encoding only): RMSE 6.7 (cargo bay), 6.8 (aft), 2.5
(canopy), 1.3 (Gladius exterior). The difference sits on the dark metal floors (low diffuse, high F0): the adaptive
format keeps the slab's Diffuse Albedo / F0 / F90 split (WP-U2, D12) and shows a darker plate with a directional
sheen; the blendable format refits it into BaseColor/Metallic/Specular (one closure, 20 B/pixel) and the plate reads
lighter and flatter. Walls, doors, emissives, railings, decals, the Gladius paint/camo and the canopy glass are
unchanged within noise.

**MaxBounces on the blendable format** (`routes/cutlass-world-cache-bounces.txt`): trace passes 0.24 / 0.35 / 0.49 ms
for 3 / 5 / 10 bounces; images identical within the 3-vs-3 noise floor (dark/shadow/lit bands within 0.07/255,
RMSE 1.2–1.9 for every pair). Fill speed (`routes/cutlass-world-cache-warmup.txt`, cache frozen 50 ms after a reset):
95–99 % of the converged shadow level after 5–6 frames at every bounce count. The last bounce reads the previous
frame's cache, so bounces beyond 3 add nothing visible in these interiors; keep the default 3.

Headless on the blendable format: importer 35/35, importer + Cutlass package + tar 37/37.

## Decision (owner, 2026-11-12)

Keep the adaptive Substrate G-buffer (`r.Substrate.ProjectGBufferFormat=1`, D12) and turn the World Cache off
(`r.Lumen.WorldCache.Supported=0`, which also drops its shaders): the adaptive format reproduces the game's own
material split, and the World Cache's gain in these interiors is small. Re-test with every 5.9.x release: set
`r.Lumen.WorldCache.Supported=1`, restart, and `r.Lumen.WorldCache.Debug 1` must show `Entries > 0` on the
adaptive format before it is turned back on.

## MegaLights quality (boiling)

Route `routes/cutlass-megalights-quality.txt`: at two still Cutlass views, per setting two shots 0.2 s apart
(frame-to-frame difference = temporal noise) and a 5 s frame sample; idle machine. `r.MegaLights.NumSamplesPerPixel`
is already at its maximum (4, `MegaLights.cpp` `GetNumSamplesPerPixel2d`); the lever is `r.MegaLights.DownsampleMode`.

| Setting | cargo noise / shadow noise / pixels >4 changed | aft noise / shadow / changed | GPU ms cargo / aft |
|---|---|---|---|
| DownsampleMode 2 (Epic) | 0.91 / 0.47 / 0.59 % | 1.69 / 0.94 / 1.49 % | 6.23 / 6.53 |
| 1 (checkerboard) | 0.91 / 0.45 / 0.44 % | 1.63 / 0.79 / 1.25 % | 6.52 / 6.65 |
| **0 (full resolution)** | **0.59 / 0.33 / 0.23 %** | **1.49 / 0.70 / 0.75 %** | **6.89 / 7.27** |
| 1 + 24 accumulated frames | 0.86 / 0.46 / 0.53 % | 1.47 / 0.67 / 1.29 % | 6.51 / 6.56 |
| 0 + 24 accumulated frames | 0.73 / 0.32 / 0.21 % | 1.12 / 0.56 / 0.73 % | 6.87 / 7.16 |

Decision: `ue/SCViewer/Config/DefaultScalability.ini` sets `r.MegaLights.DownsampleMode=0` at `[ShadowQuality@3]`
(the engine's Cinematic level uses the same value); temporal accumulation stays at 12 (24 helps the cockpit view but
its ghosting under motion is unmeasured). Verified: the log sets `r.MegaLights.DownsampleMode:0`;
`cutlass-ramp-to-cockpit` runs all 81 steps (same shot set), GPU 7.17 / 6.53 / 6.97 ms (first person / orbit / first
person).

**Bias check and the shadow policy.** Without shadow rays MegaLights is within 3–7 % of deferred lighting without
shadows (cargo 46.3 vs 49.9, aft 58.6 vs 60.5 mean grey; part of it is 5.9's energy-based light culling). With every
local light ray-traced-shadowed — the WP-U3b substitute for the game's `affectsThisAreaOnly` vis-area confinement —
the cargo bay drops to 23.3 (aft 44.1): the shadows halve the interior light, while the game shadows only the
authored `shadowCasting=1` lights (3 of 225 on the Cutlass). Confining lights to their rooms (WP-U3b open item) and
then shadowing only the authored lights is the fidelity fix; it is not done here.
