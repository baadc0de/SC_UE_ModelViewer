# WP-L2 — Screen-space directional occlusion (unshadowed lights and ambient)

Branch `wp/l2-unshadowed-occlusion`. Follows WP-L1 (only the authored `shadow_casting` lights cast shadows) and the
WP-U2c AO survey (`ue/verification/WP-U2c.md`).

## Problem

After WP-L1 most ship lights (Cutlass 222 of 225) cast no shadow, as in the game. The game does not leave them
unoccluded: every light that is not a shadow caster (and the sun) is scaled by its screen-space directional occlusion
(SSDO) cone. In the viewer those lights reach into corners, under seats and behind panels at full strength.

## The game's rule (decoded `Engine/ShaderCache_Vulkan.pak`; hashes in `StarEngineRender.json`)

- `DeferredShading.cfx` `DirOcclusionPS` (default `r_ssdoQuality 0`): horizon search with visibility bitmasks,
  3 slices x 6 samples per direction, radius `GetSSDORadius` = `r_ssdoRadius` metres projected and clamped to
  0.1–1 of the screen height, falloff `1 / radiusWS^2`, thickness `log2(1 + radiusWS) / 2`, 4x4 interleaved noise.
  Output: AO = `saturate(visible / weight) ^ r_ssdoStrength`, bent normal = mean slice tangent occlusion + normal.
- `SpatialDenoisePS`: 2x2 depth-weighted blur, normalised bent normal, w = 1 − AO.
- `ShadeLib.cfi` `CalculateSSDO`: `1 − saturate((sqrt(w) − dot(L, bent)) × r_ssdoAmountDirect ×
  saturate(lightDist / cameraDist × 5))` — a cone around the bent normal whose aperture closes with occlusion.
- `TiledShading.cfx`: `if (!IsLightShadowCaster || sun) lightTerm *= CalculateSSDO(...)` (diffuse and specular).
- `MergedTiledShading.cfi`, after the environment probes: `ambientAcc *= 1 − w × r_ssdoAmountAmbient` and probe
  `specularAcc *= DeriveSpecularOcclusion(NdotV, 1 − w × r_ssdoAmountReflection, gloss)` (`ShadeLib.cfi`:
  `saturate(pow(NdotV + ao, exp2(−16 (1 − gloss) − 1)) − 1 + ao)`). With `r_ssdoColorBleeding` (PostProcessing spec
  3–4) the ambient factor is `pow(AO, saturate(1 − bleed² × 0.75))`, `bleed` from the game's screen-space GI pass.
- Cvar defaults (executable): `r_ssdo 1`, `r_ssdoRadius 4.0`, `r_ssdoStrength 2.0`, `r_ssdoAmountDirect`,
  `r_ssdoAmountAmbient`, `r_ssdoAmountReflection` 1.0. The ray-traced GI (`sys_spec_GlobalIllumination`, default 0 =
  off; the owner's profile does not set it) switches SSDO to a GI variant; the default path is the one ported.

## Implementation

UE 5.9 is a binary install: engine C++ cannot change, shaders can. Lumen's short-range AO cannot feed the lights — the
Lumen indirect pass runs async and is composited after them — so the SSDO is computed in each pass that needs it.
The patch lives in the repo:

- `ue/EnginePatches/MegaLights/SCSSDO.ush`: the port (translated world space, depth at mip 0, the game's 4x4 noise
  rotated per frame so MegaLights' temporal denoiser stands in for the 2x2 blur).
- `ue/scripts/engine_patches.py apply|check|revert`: writes `SCSSDO.ush`, `SCSSDOParams.ush` (constants from the
  fixture's `exe_cvars`) and three anchored insertions into `MegaLights/MegaLightsShading.usf` (original kept as
  `.scviewer-orig`). The SSDO is computed once per pixel, on the first light that needs it.
- Ambient: `DiffuseIndirectComposite.usf` (Substrate path, Lumen screen-probe gather) replaces Lumen's short-range AO
  with the game's terms: Lumen diffuse indirect × `1 − w × r_ssdoAmountAmbient`, Lumen rough specular ×
  `DeriveSpecularOcclusion` (gloss = 1 − UE roughness, `create_materials.py`). Lumen's traced reflections stay
  unoccluded, as the game's SSR does. Colour bleeding's exponent is not ported: its `bleed` is the game's SSGI bounce,
  which Lumen's GI computes itself.
- `r.GeneralPurposeTweak` scales the amounts for A/B in development builds (0 = UE's behaviour: no SSDO on lights,
  Lumen short-range AO on ambient; 1 = the game, default, fixed in shipping).

`projScale` / `invProjScale` / `rcpAspectRatio` are set in the game's C++, not the shader text; read from the executable:
`0.5 × P11`, `2 / P11`, `P00 / P11` (see the verification).

Debug view (development): `r.GeneralPurposeTweak2 2` writes the SSDO AO, `3` the bent normal, as the MegaLights
diffuse light (route `cutlass-ssdo-debug.txt`, lighting-only view mode, Lumen indirect and reflections off).

## Acceptance

- `verify_star_engine_render.py` matches the new cvars and function hashes against the installed game.
- `engine_patches.py check` OK; MegaLights shaders compile; GPU route `cutlass-ssdo.txt` shows the effect, its cost
  and the frame-to-frame noise at three views; recorded in `ue/verification/WP-L2.md`.
