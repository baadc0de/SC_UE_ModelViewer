# LOOK-8 — bloom, crosshatch, tone mapping, exposure, lighting across four stages (2026-11-13)

Owner: the RSI Galaxy lit inside the XL utility hangar did not reach a good visual standard; re-adjudicate bloom,
crosshatch, tone mapping, exposure and lighting factors, balanced across four configurations:

| Config | Ship | Environment |
|---|---|---|
| galaxy-utilxl | RSI_Galaxy | util_a/hangar_xltop_001 |
| galaxy-htxl | RSI_Galaxy | hightech_a/hangar_xltop_001 |
| golem-utilsm | DRAK_Golem | util_a/hangar_smltop_001 (its fitted hangar, WP-C1) |
| golem-htsm | DRAK_Golem | hightech_a/hangar_smltop_001 |

Views per config: orbit, path-traced orbit (128 spp), low exterior, close exterior, and for the Galaxy cockpit and an
interior corridor. Runner: a per-config capture_route script (`-SCDefaults -SCEnvironment=<exact>`), contact sheets of
the views; adjudicator GPT Astra (Codex), three rounds.

## Rounds

| Round | Settings | Astra |
|---|---|---|
| baseline (`l8b`) | star bloom 0.05, bias 0, boost 1.15, game display curve | Galaxy: green-grey hull merges with the floor, perimeter floor pools compete, amber bloom veils panel edges, cockpit starbursts; the canopy shows a bounded milky patch. Golem/util: best balance. Golem/hightech: icy walls and floor pools overpower the small ship |
| 2 (`l8r2`) | standard bloom 0.10, threshold 1.0, bias -0.25 | keep standard bloom: starbursts gone, edges and cockpit rails read, emitters still glow; -0.25 EV too dark for the Galaxy underside and Golem recesses |
| 3a (`l8r3a`) | as 2 with bias -0.10 | **ship**: better than baseline in all four (clearly for both Galaxy stages, modestly for the Golem) |
| 3b (`l8r3b`) | 3a + LumenDiffuseColorBoost 1.25 | small shadow lift, no real hull/floor separation gain: keep 1.15 |

Kept: the game's display curve (UE filmic crushed undersides in LOOK-5), diffuse boost 1.15, sky and wall lights as
authored, every light shadowed. Local exposure is not applied by the game display-curve material, so its sliders were
not tried.

## What shipped

`USCViewerSettings`: `StarBloom` 0 (standard bloom; the 4-point diffraction kernel was the "crosshatch"),
`BloomIntensity` 0.10, `BloomThreshold` 1.0, `ExposureBias` -0.10 on top of the game's exposure-compensation curve,
applied to the unbound post-process volumes at startup (`SCLookLighting::ApplyDefaults`). Saved settings from an
earlier look revision are migrated once (`LookRevision` 9: the saved StarBloom was the old default, not a user
choice). Check: the shipped defaults reproduce round 3a within 0.37-0.47 % RMSE on every view (`l8r6`).

## Experiment not shipped: perimeter floor washes

Astra (round 3) proposed dimming lights that wash the floor outside the ship bay. `sc.Look.PerimeterFloorWash <scale>`
classifies environment spot/rect lights by tracing their emission (cone or cosine hemisphere, the ship ignored): >= 80 %
of the weighted samples on upward-facing floor outside the measured ship bay and <= 10 % inside -> scaled. At 0.6 it
picked 42-78 of 505-938 lights in the XL hangars and 16-22 in the small ones, but changed the frames by 0.35-0.65 %
RMSE (mean luminance -0.1): the competing floor brightness does not come from those fixtures. Kept as a tool, not a
default. The Galaxy-in-utility-XL lighting hierarchy stays as authored (improved by the bloom/exposure change, not
resolved): the hangar is lit for its own play spaces, not as a ship studio.

## Separate

The canopy's bounded milky patch is the canopy glass (gone with `sc.Look.HideGlass 1`, unchanged without front-layer
translucency reflections): a glass-shading investigation, not a post-process setting.
