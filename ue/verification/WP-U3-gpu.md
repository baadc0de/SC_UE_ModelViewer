# WP-U3 GPU findings — 2026-11-09 (orchestrator, RTX 4090, Vulkan, superproject 6da2de6)

Runs: `ue/scripts/run_import.sh <pkg> -vulkan -SCStay -SCView=35,18,1.4 [-SCSnapClip=…|-SCPlayClip=…]`,
screenshot ~22 s after `IMPORT_OK`. Half-size captures in `ue/verification/wp-u3/`.

| Capture | Package / clip | What it shows |
| --- | --- | --- |
| `scorpius-bind.jpg` | Scorpius, no clip | ship imports with 127 light components; hull blown out by its own blue/white lights, red nav lights flood the floor |
| `scorpius-gear.jpg` | `-SCSnapClip=landing_gear_deploy` | gear snaps, but several parts (canopy halves, a wing section, the nose gun) are displaced far from the ship |
| `scorpius-wings.jpg` | `-SCPlayClip=wings_deploy` | the whole wing/canopy assembly floats above the hull, mirrored; left wing sections detached on the floor |
| `gladius-gear-canopy.jpg` | `-SCSnapClip=landing_gear_extend -SCPlayClip=canopy_open` | a part floats above the ship; gear not visibly deployed; red/blue light floods |
| `cutlass-door.jpg` | `-SCPlayClip=…/cargo_door_open.caf` | **rear ramp lowers correctly**; engines glow; lights overexpose the bay |
| `cutlass-interior.jpg` | same, camera inside | interior entirely white: light intensities far too high |

Findings:

1. **Clip transform composition is wrong for some tracks.** Clips that move a few bones
   (door, gear strut) work; clips driving sub-rigs or helper-parented parts (wings,
   canopy, a Gladius part) send whole assemblies metres away, mirrored. Candidates:
   the Blender-Z-up → UE basis applied to position samples without the handedness
   mirror, local-vs-parent composition for bones whose parent is a rigid part,
   `start_rotation` anchor, or hash-collision routing (§14.8) binding a track to the
   wrong receiver. Must be fixed in the WP-U3 round with a numeric check: snap a clip
   and compare every bone's world transform against the default-pose reference export
   (not only the 26 keyed rotation tracks).
2. **Light intensities are applied unconverted.** `intensity_candela_proxy` (e.g. 690
   for a Cutlass bay light, 75 for cockpit lights) is used as UE candela with
   inverse-square falloff and `radius_m` attenuation, which overexposes everything;
   the Viewer level's fixed exposure was tuned for the sun. The conversion must come
   from upstream's lighting research (`star-citizen-lighting-research.md`: authored
   `intensity`, `glowMultiplier`, the candela proxy's derivation) — i.e. derive UE
   intensity from the raw authored values with the documented engine units, and let
   the viewer use auto-exposure with a bounded range. No per-ship tuning.
3. **Virtual shadow maps overflow with 76–225 local lights** ("One Pass Projection
   max lights overflow" on every frame). WP-U3b (MegaLights) investigates the proper
   many-light path; until then shadow casting should be capped by a data-derived rule
   (the brief's fallback), not left on for every light.

## After the review fixes, WP-U3b settings and the lights round (superproject 224999e)

Same runs, new binaries (`after-*.jpg`): MegaLights on, auto-exposure, sidecar basis fixed.

| Capture | Result |
| --- | --- |
| `after-scorpius-wings.jpg` | `wings_deploy` spreads the wings into the X configuration with every part attached; no mirroring, nothing floating |
| `after-scorpius-gear.jpg` | `landing_gear_deploy` snap-last: struts extended, ship resting on them, all attachments in place |
| `after-cutlass-interior.jpg` | cargo bay reads as a lit interior (grey deck, orange trims, ceiling fixtures with MegaLights shadows); no overexposure |
| all | the VSM "One Pass Projection max lights overflow" message is gone (MegaLights handles the 127–225 local lights) |

Findings 1 (clip basis), 2 (exposure) and 3 (many lights) from the first pass are closed.
Still open: the full-pose numeric gate waits for the upstream reference fix (WP-R6);
the ship's own exterior lights still pool strongly on the floor — their authored
default states are on (Gladius wing lights have positive defaults), so a parked /
hangar light-state selection is a WP-V2 item, not a data error.
