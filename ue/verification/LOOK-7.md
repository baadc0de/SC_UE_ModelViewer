# LOOK-7 — short-range AO by ray-tracing budget; the RSI Galaxy (2026-11-13)

## Finding: traced short-range AO rebuilds the material SBT every frame on Vulkan

LOOK-1 traced Lumen's short-range AO with hardware ray tracing (`r.Lumen.ScreenProbeGather.ShortRangeAO.HardwareRayTracing=1`,
+1.6..1.9 ms GPU on the Cutlass). The pass is a ray-generation pass (`PrepareLumenHardwareRayTracingShortRangeAO` always
registers RayGen shaders, although its dispatch then uses its own minimal binding table). Any registered RayGen shader
makes `SetupRayTracingPipelineStatesAndSBT` build the material ray-tracing pipeline and its shader binding table, and the
Vulkan RHI has no persistent SBTs (`GRHIGlobals.RayTracing.SupportsPersistentSBTs` is set by D3D12 only), so the table
is rebuilt each frame: one record per ray-traced geometry segment, on the render thread and again on the RHI thread.
The engine is a binary build (C++ cannot change).

Found on the Galaxy in the XL hangar (329k segments): render thread ~25 ms in `SetupRayTracingPipelineStatesAndSBT`
(Insights trace), RHI thread ~26 ms translating it. A/B toggles ruled out lights, shadows, VSM, translucency, skeletal
meshes, MegaLights HWRT, the ray-tracing geometry pool and instance culling; Lumen software tracing or screen-space
short-range AO removed it.

| Scene (1600x900) | traced AO frame / render / RHI ms | screen-space AO frame / render / RHI ms |
|---|---|---|
| Scorpius, hightech hangar_lrgtop_001 (115k segments), orbit | 15.7 / 9.0 / 12.2 | 9.4 / 4.4 / 1.5 |
| Scorpius, same, first person | 18.9 / 12.4 / 16.4 | 9.3 / 4.4 / 2.5 |
| Cutlass, same (114k), orbit | 14.6 / 8.4 / 9.6 | 9.6 / 4.6 / 1.5 |
| Cutlass, same, first person | 18.2 / 10.4 / 15.3 | 11.8 / 4.8 / 4.1 |
| Galaxy, hangar_xltop_001 (329k), orbit | 33.2 / 23.5 / 27.8 | 11.3 / 8.8 / 1.3 |
| Galaxy, same, cockpit | 51.2 / 37.8 / 47.8 | 14.9 / 7.0 / 4.1 |

This is also the likeliest cause of the render-thread-bound wide hangar views in LOOK-5 and of the cooked hangars'
unexplained +2..4 ms render thread in WP-C1 (both scale with the scene's primitives).

## Rule now (GPT Astra verdict: adopt)

`SCLook::UpdateShortRangeAO`, after every vehicle or environment load: short-range AO is traced while the world's
ray-traced segments (sections of every primitive in the ray-tracing scene, instanced components once; Lumen proxies
included) stay within `USCViewerSettings::TracedShortRangeAOMaxSegments` (20000, ~2-3 ms CPU: a platform budget, not a
visual threshold), else Lumen's screen-space short-range AO. Logged as `LOOK short_range_ao=... rt_segments=...`;
`sc.Look.ShortRangeAO auto|0|1` overrides. In practice every game hangar and the Galaxy run screen-space AO; the
procedural designed hangar with a small ship keeps traced AO.

Image cost (Cutlass in hangar_lrgtop_001, same frame, `out/smoke/aolook-*`): RMSE 0.48 % orbit, 0.56 % cockpit,
0.68 % cargo. Astra: finishing differences only (cargo-ramp grooves read slightly harder, softer junction transitions
with traced AO), no halos, the GI-driven look is kept. Open: a camera-motion check for screen-edge occlusion; the AO
mode changes only when a load completes, not during motion.

## RSI Galaxy (first load ever; binary-CDF export fix in StarBreaker `scviewer/cdf-cryxmlb`)

| Step | Load (warm DDC) | Frame, XL hangar (orbit / cockpit) |
|---|---|---|
| First run (cold DDC, 16k per-part Lumen proxies) | 492 s | 37 / 51-100 ms |
| Warm DDC | 92 s | — |
| Proxies clustered (2 m cells of mesh space per source section) | 110 s | 21.6 / 34.7 ms (default hangar) |
| Short-range AO by budget | — | 11.7 / 14.4 ms |
| Proxy meshes built as one batch; one opaque base material per master | 57 s | 12.1 / 14.9 ms |
| Mesh derivation look-ahead = worker threads (max 12) | 47 s | — |

- The 2.6 GB package exceeded the importer's 2 GiB tar limit: the tar source and export results are 64-bit arrays now
  (entries stay below 2 GiB each).
- Lumen proxies: 16,077 glowing parts -> 4,056 clusters (3,928 distinct meshes, 4,052 components); cockpit GI within
  0.9 % RMSE of per-part proxies. Build 63 s -> 6.3 s (`FSCMeshImporter::InstantiateBatch`; the per-material opaque
  permutation compile was 29 s of it).
- Interior spawns stand on the floor of the container's largest room volume (was: the bounds of everything that collides,
  which reach the ground through the lifts).
- Interiors (14 containers, 4,838 primitives) streamed by camera position would now save < 1 ms (cockpit 14.9 -> 14.2 ms
  with only the nose and front containers shown): not needed for performance. `sc.Interior.Show|RayTracing <name|*> 0|1`
  stay as measurement tools.
