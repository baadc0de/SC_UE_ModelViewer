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
