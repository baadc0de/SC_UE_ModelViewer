# RTXPT-1 — NvRTX real-time path tracing instead of MegaLights, Win64 (2026-11-13)

Owner: evaluate NVIDIA's real-time path tracing (NvRTX sampled lighting) with DLSS Ray Reconstruction instead of
MegaLights, on the Drake Golem in the hightech small top hangar (LOOK-8 `golem-htsm`).

## Setup

| | |
|---|---|
| Engine | NvRTX `nvrtx-5.8` (UE 5.8.3, `6f851076`), source build, Win64 Development editor, MSVC 14.50.35739, SDK 10.0.26100 |
| Host | Windows 11, Ryzen 9 9950X3D, RTX 4090 (driver 621.24), D3D12 SM6, 1920×1080 windowed, `-game` |
| Look | `Config/Windows/WindowsEngine.ini` (Win64 layer only; Linux / Vulkan / MegaLights unchanged) |
| Direct | RTXDI (`r.SampledLighting.Direct 1`): all 495 hangar + 39 ship lights sampled, ray-traced visibility |
| Indirect | ReSTIR PT (`r.SampledLighting.Indirect 1`, `Mode 2`): diffuse, rough and mirror indirect; Lumen kept as the GI method for its surface-cache final bounce |
| Denoise | DLSS-RR, DLAA at 100 % (`r.NGX.DLSS.Enable 1`, `DenoiserMode 1`, preset F); NRD plugin loaded (NvRTX routes the sampled signals through it; ReLAX when RR is off) |
| Geometry | RTX Mega Geometry (`r.Nanite.RayTracing 1`): full-detail Nanite clusters in the BLAS, 2.37 GB persistent GPU memory |
| Route | `ue/scripts/routes/golem-rtxpt.txt`, `-SCShotDir` in-engine shots (game-derived, `out/smoke/rtxpt/`, not committed) |

## Requirements found on the way

1. **Substrate blendable G-buffer.** NvRTX disables all sampled lighting under Substrate's adaptive G-buffer
   (`PlatformSupportsSampledLighting`: "complex Substrate"). With `r.Substrate.ProjectGBufferFormat=1` the cvars were
   set but the frame was stock deferred lighting + VSM + Lumen (ProfileGPU: no RTXDI pass, 2.7 ms
   `VirtualShadowMapProjection`). The Win64 layer sets `0` — the WP-U59 trade-off (dark metal floors change).
2. **NRD plugin.** Without the NRD module NvRTX skips the third-party denoiser chain and opens a modal
   "requires the NRD plugin" dialog, even with DLSS-RR active.
3. **Mega Geometry.** On the Nanite fallback proxies (`r.Nanite.RayTracing.Update 0`) ReSTIR PT shows a black hangar
   floor beside the ship, black streaks along hull edges and dark self-shadowing on the fuel tank; in the evaluation
   run Mega Geometry removed all of it. The engine path tracer looked right on the same proxies, so the fault is in
   the real-time sampled-lighting rays. **Correction (follow-up runs, below):** the artefact came back at the door view
   in a later run with Mega Geometry on, so Mega Geometry does not reliably fix it — root cause still open.
4. **5.9-saved assets** (custom versions newer than 5.8): the Viewer map, `Smoke/M_Hangar*` and the SCBridge
   materials were regenerated on 5.8 by `create_viewer.py` and `create_materials.py` (one 5.8 API difference fixed in
   `create_materials.py`: single-input pins are named `""`, not `"Input"`).

## Results (Mega Geometry on; GPU ms p50 over 5 s, RTX 4090, 1920×1080)

| View | ReSTIR PT + RR | proxy BLAS | ReSTIR GI | Standard PT | ReSTIR PT, NRD (no RR) | ReSTIR PT + RR @ 67 % |
|---|---|---|---|---|---|---|
| exterior (noclip 12 m, 4 m up) | **23.0** | 23.3 | 20.7 | 17.0 | 30.9 | 18.3 |
| door close-up | 28.5 | — | — | — | — | — |
| above hull | 26.2 | — | — | — | — | — |

Reference: the stock deferred + VSM + Lumen frame of the same build (adaptive G-buffer, orbit view) was 11.8 ms.

Image (contact sheets of each view against the engine path tracer at 256 spp, 8 bounces):

- All three real-time modes match the reference's shadow shapes, floor brightness and hull shading at the exterior,
  door and top views at contact-sheet scale (no per-pixel error measured yet). NRD without RR is visibly noisier and
  slower than RR (RR replaces the NRD pass and still costs less).
- The ship's floor shadow is soft and multi-lobed (many area lights) where VSM drew one hard key shadow.

## Profile (exterior, ReSTIR PT, one ProfileGPU frame: 15.8 ms)

RTXDI 4.5 ms (initial samples 1.8, spatial 1.3, translucency volume 0.9) · DLSS-RR 1.9 ms · ShadowDepths 4.1 ms
(instance culling; see the follow-up: not VSM and not a real cost).

## Follow-up: VSM and stability (2026-11-13, route `golem-rtxpt-shadows.txt`)

**VSM off saves nothing.** GPU ms p50, VSM on / VSM off (`r.Shadow.Virtual.Enable 0`) / no shadow maps
(`r.ShadowQuality 0`): exterior 24.71 / 24.93 / 24.37, door 21.22 / 21.08 / 22.68 — run-to-run noise. ProfileGPU
still shows `ShadowDepths → CullInstances(UnCulled)` at 4.1 ms with VSM off: its timestamps overlap
`SampledIndirectLighting` (ReSTIR PT, 14.6 ms on async compute), so the 4.1 ms above was an overlap artefact, not
VSM. The real budget at the exterior view is ReSTIR PT ~14.6, RTXDI 4.8–6.7, DLSS-RR 2.2 ms. Images match too (the
exterior's 14 % RMSE between the two shots is the first-person body appearing in frame; noise floor 0.9 %). VSM stays
on: it costs nothing here and still shadows views that sampled lighting does not handle (stateless captures).

**Mega Geometry runs are unstable.** Two of four runs with `r.Nanite.RayTracing 1` lost the GPU about 20 s after the
ship loaded, before any route step: a page fault during RTXDI direct lighting, then `DXGI_ERROR_DEVICE_HUNG` in
`SampledIndirectLighting`; both followed bursts of Mega Geometry persistent-memory reallocations. Fixed CLAS pools
(`r.Nanite.RayTracing.CLAS.Memory.{Root,Streaming}.FixedSizeInMB=1536`) did not prevent the second crash (and doubled
persistent memory to 4.6 GB), so they are not configured. No crash in four runs without Mega Geometry.

## Open

- Mega Geometry GPU crashes (2/4 runs) and the black-floor / edge-streak artefact recurring with Mega Geometry on
  (door view, follow-up run): candidates are the clustered-BLAS build limits (`r.Nanite.RayTracing.BLAS.MaxToBuild`,
  `BLAS.CLASAddresses.Max` — exceeding them silently falls back to proxies; visible only in
  `r.Nanite.RayTracing.Stats.Show`) and NvRTX's Nanite tracing in the sampled GI path.
- The engine path tracer shows saturated blue emissive patches on the hangar's back wall and doors that no real-time
  mode shows (emissive / light-function handling differs between the two); not investigated.
- `Seat` spawns refuse the teleport on the Golem (the route uses `Door`, `Hull` and a noclip placement).
- A MegaLights comparison on the same Windows build was not run (needs the adaptive G-buffer and a second shader set).
- Engine shader patches (D14, SSDO) are not applied on Win64; they target MegaLights / Lumen paths this look replaces.
