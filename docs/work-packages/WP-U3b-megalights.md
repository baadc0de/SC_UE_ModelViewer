# WP-U3b — Many-light rendering: MegaLights investigation and decision

**Agent:** Opus 5.5 (GPU available). **Depends on:** WP-U3 (lights imported).
**Build lock:** UE lock. **Owner request (2026-11-09):** "There are a lot of lights.
Consider MegaLights."

## Why
WP-R5/WP-U3 import every authored light: Gladius 77, Scorpius 127, Cutlass Black 225
(mostly spots, many shadow-casting). Classic per-light shadow maps do not scale to
that; MegaLights renders many dynamic shadowed lights with stochastic sampling on
hardware ray tracing, which the Viewer level already uses (D8/D9, Vulkan HWRT).

## Tasks (evidence-based, like the Substrate decision in WP-U2)
1. Establish MegaLights' status in the installed 5.8.2 from the engine's own sources
   and config (`r.MegaLights*` CVars, Engine/Config/BaseEngine.ini, the renderer
   module, `DataDrivenPlatformInfo.ini` support flags for VulkanPC/SM6,
   Experimental/Beta markers), its requirements (HWRT, inline ray tracing / ray
   queries on Vulkan, Nanite, Substrate compatibility, VSM interplay), and its
   limitations (light types supported — point/spot/rect/directional; light functions
   / gobos; IES; translucency lighting; volumetric fog; max lights per pixel; denoiser
   cost at 1600×900 and at 4K).
2. Measure, on the GPU, the Cutlass Black package (225 lights) and the Scorpius (127)
   in the game host: GPU frame time (`stat GPU`, `stat unit`, or `ProfileGPU`) with
   (a) the current path, (b) MegaLights enabled (`r.MegaLights.Enabled=1` in
   `DefaultEngine.ini` plus whatever per-light settings it needs, e.g. lights
   allowed for MegaLights, shadow settings), (c) MegaLights with light functions
   (gobos) on vs off. Capture the Cutlass interior (corridor + cargo bay) under each.
3. Check correctness: no light leaking through walls (stochastic shadows), gobo
   projection, emissive + Lumen interplay, and the candela-proxy intensities
   (`intensity_candela_proxy`) at the interior views against
   `ue/verification/reference/gladius/*.webp` where applicable.
4. Write the comparison and decision into `ue/verification/WP-U3b.md` (facts with
   file/CVar references, a feature table, measured frame times, screenshots) and a
   10-line summary into this brief under "Decision". If MegaLights wins, enable it
   in the project and set the light importer's per-light flags accordingly
   (`SCLightImporter.cpp`, coordinated with the WP-U3 fix round: do not edit that file
   while a Codex round is running on it — the orchestrator sequences you).
5. Whatever the decision, keep a documented fallback: a cap on simultaneously
   shadow-casting lights derived from the authored data (e.g. shadows only for lights
   whose radius or intensity exceeds a data-derived threshold), never a per-ship list.

## Done criteria
- `ue/verification/WP-U3b.md` has the status facts, the frame-time table for both
  ships under each configuration, the interior screenshots, and the decision.
- With the chosen configuration the Cutlass Black interior renders all its lights
  without leaking at an interactive frame rate on the RTX 4090 (state the number),
  and the Gladius still matches its reference images.
- Any new project setting is in `ue/SCViewer/Config/DefaultEngine.ini` with a comment
  pointing at the verification record.

## Decision (2026-11-09, evidence in `ue/verification/WP-U3b.md`)
1. MegaLights is enabled for the project (`r.MegaLights.EnableForProject=1`). It is still marked
   Experimental in 5.8.2 but supported on VULKAN_SM6 (SM6 + wave ops + HWRT), with Substrate.
2. Cutlass bay, 153 lights: 10.0 → 7.0 ms GPU at 1600×900 and 28.8 → 16.9 ms at 3840×2160
   (RTX 4090). Scorpius 7.4 → 6.5 / 14.5 → 11.9 ms. VSM one-pass overflow gone; all lights shadowed.
3. No leaks: interior lights do not show through the hull with the sun off; shadows match VSM.
4. Gobos were silently dropped on both paths (16-slot light function atlas, one MID per light):
   `r.LightFunctionAtlas.Size=16` now; importer patch shares one MID per texture.
5. Intensity stays `intensity_candela_proxy` (= raw × 1500) in candela; the addon's ×20 gain is a
   uniform exposure, not per-light information. Overexposure came from the fixed exposure.
6. Viewer exposure: auto exposure, EV100 units, bias 0 (measured EV100 −2.0/−1.6 exteriors,
   +0.3 Cutlass bay); a bounded PPV (−3…6) is proposed for the Viewer map.
7. Fallback without MegaLights: per scene instance, only lights with `radius_m` ≥ the instance's
   upper quartile cast shadows (Cutlass 60/225, Scorpius 36/127, Gladius 22/76) — in the patch.
8. MegaLights needs no per-light flags (defaults: allowed, ray-traced shadows).
9. Patch for `SCLightImporter.cpp` is in the record (§8), checked with `git apply --check`, not built.
10. Gladius hull/livery unchanged; the hangar reference shows no ship-light pools (light state, TODO).
