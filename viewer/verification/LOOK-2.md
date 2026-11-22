# Look iteration 2 — reproducible comparisons, the cargo-aft dark region, the canopy (2026-11-12)

Follows [LOOK-1](LOOK-1.md). Comparisons now lock exposure for real time (RT) and path tracer (PT) alike:
`sc.Look.PP AutoExposureBiasCurve none`, `AutoExposureBias 0`, `AutoExposureMinBrightness = AutoExposureMaxBrightness
= 6.7` (EV100; auto exposure settles at 6.6..6.9 in these views). New runtime switches (`SCLook.cpp`): `sc.Look.PP`
(any post-process field, `none` clears an object field), `sc.Look.Channels`, `sc.Look.Sky`, `sc.Look.HideGlass`.
Routes: `cutlass-look-iter3-diag.txt` (diagnostics), `cutlass-look-iter3.txt` (decisions).

## Diagnostics (grey mean, display-encoded, locked exposure)

| View | RT | RT direct only | RT without room confinement | PT | PT direct only |
|---|---|---|---|---|---|
| Cargo bay | 30.5 | 25.7 | 30.5 | 33.4 | 30.3 |
| Cargo bay, aft | 49.9 | 46.6 | **59.3** | 55.1 | 53.0 |

Cockpit: RT 62.4, RT without glass 61.3; PT 84.1, PT without glass 63.0; RT after a fresh sky capture 62.4.

- Cargo-aft's dark region was the WP-L1 room confinement: lights on room lighting channels, which the path tracer
  ignores. With every light shadowed (LOOK-1), geometry already contains the light, so confinement only removed light.
- The cockpit difference is the PT rendering of the canopy glass (and the screen as a solid card), not the hangar
  environment or a stale sky capture: without glass both agree. Target look (adjudicated): clear transmission as RT
  renders it.

## Decisions

| Change | Where | Result |
|---|---|---|
| Room confinement off: every ship light on all lighting channels | `USCViewerSettings::bConfineLightsToRooms` (default off); WP-L1 tests pin it on | Cargo-aft 49.9 → 59.4 (PT 55.1), its upper/right structure lit as in the PT. |
| `LumenDiffuseColorBoost` 1.3 → 1.15 | `SCViewerExposure.cpp` | 1.0 / 1.15 / 1.3: cargo 29.5 / 29.9 / 30.3, cargo-aft 58.9 / 59.2 / 59.4, cockpit-aft 43.7 / 44.6 / 45.7. |
| Front-layer translucency reflections | rejected | No measurable change (62.40 vs 62.41), +0.4 ms. |

GPU after the changes: 8.8–9.9 ms at 1600x900. Unit tests 38/38.

## Next (adjudicator's ranking)

1. A designed hangar as the default environment (shadowed rect-light keys sized to visible fixtures, weaker fill,
   about 3:1, dark structural bays, contrasting panels) instead of the uniform grey box.
2. A space preset: a game skybox (Stanton starfield/nebula or GC14 nebula) as backdrop and a restrained specified
   cubemap sky light, one shadowed directional key.
3. A daylight / dock preset from `Data/Textures/skybox/PBS_Skydome_*`.
Also: emitter sizes and placement in the cargo bay against the fixture geometry (highlights harsher than PT); keep the
hidden Lumen copies of glowing parts out of PT camera / refraction rays.
