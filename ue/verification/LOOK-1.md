# Look iteration 1 — towards a path-traced sci-fi look (2026-11-12)

Goal (owner, plan D15): a "path-traced sci-fi look" for a modern sci-fi game, GI on; not Star Citizen's current GI-off
renderer. Method: capture fixed Cutlass views in real time and with UE's path tracer (256 spp, 8 bounces, same scene),
have an adjudicator model (GPT-6 Astra through Codex, read-only) judge them, apply its ranked changes, re-capture.
Routes: `ue/scripts/routes/cutlass-look*.txt` (host `-SCDefaults -SCLoad=DRAK_Cutlass_Black -SCCamera=first`).

## Shipped defaults

| Change | Where | Verdict / measurement |
|---|---|---|
| Every ship light casts shadows (MegaLights) | `USCViewerSettings::bShadowAllLights` (default on), applied at import and by `sc.Look.ShadowAll` | Removes light leaking through geometry (the yellow streaks in the cargo bay) and gives the overhead fixtures their cone shadows, as in the path tracer. Cost within noise (−0.2..+0.1 ms). |
| Game SSDO off; Lumen short-range AO traced with HWRT | `ue/EnginePatches/look.json` (`ssdo: false` → patched amounts 0), `DefaultScalability.ini` `[GlobalIlluminationQuality@3]` | Contact occlusion follows geometry; broad SSDO darkening gone. +1.6..1.9 ms. |
| `LumenDiffuseColorBoost` 1.3 (1.15 since LOOK-2) | runtime exposure volume (`SCViewerExposure.cpp`) | Restrained lift of bounce light (cargo +7 % grey mean); no material wash-out. |
| Glowing surfaces in Lumen | `bEmissiveInLumen` (default on), hidden opaque copies per glowing part (`SCEmissiveLights.cpp`) | Strips light their surroundings through GI; 2.9 s at import, ~0.1 ms. |

Rejected after measurement: reflection hit lighting (`r.Lumen.HardwareRayTracing.LightingMode 2`, +1.5..2.7 ms, no
clear gain), Cinematic GI (+1.6..1.9 ms, no brightness recovery), Cinematic reflections (only front-layer translucency
reflections), `r.MegaLights.ScreenTraces 0`, `LumenScene.DirectLighting.MaxLightsPerTile 16`, short-range AO
downsample 2 (savings within timing noise).

## Result (1600x900, GPU ms; viewport grey mean before → after, path tracer)

| View | GPU before → after | Mean before → after | Path traced |
|---|---|---|---|
| Cargo bay | 6.98 → 9.17 | 28.3 → 31.6 | 34.8 |
| Cargo bay, aft | 6.89 → 9.02 | 43.5 → 39.6 | 49.8 |
| Cockpit, looking aft | 7.23 → 10.00 | 43.5 → 41.9 | 46.7 |
| Cockpit | 6.96 → 9.46 | 44.4 → 47.2 | 64.0 |

Exposure is histogram auto exposure (not locked): means move a few units between runs; display-encoded means are not
energy ratios.

## Next (adjudicator's ranking)

1. Reproducible comparisons: identical locked exposure for real time and path tracer per camera; linear luminance in
   lit / shadow regions; log effective shadow flags and cvars after saved settings load (a saved
   `bShadowAllLights=False` silently overrode the default during this iteration).
2. Cargo-aft's large dark region (and the unlit ceiling truss over the cargo bulkhead) that the path tracer lights:
   compare direct-only and full path tracing to tell direct-light visibility / source size from Lumen surface-cache
   coverage.
3. Cockpit: separate canopy glass from the hangar environment (canopy hidden; sky light off / recaptured).
