# WP-L1 — Room light confinement: verification

Host: UE 5.9.0 preview-1, Vulkan SM6, RTX 4090, MegaLights at full resolution (WP-U59), Star Citizen LIVE 4.10.2,
packages `out/dec-p3-{cutlass,gladius,scorpius}`. Brief: `docs/work-packages/WP-L1-room-light-confinement.md`.

## Tests

- `SCBridge.Lights.RoomChannels` (new): chain a-b-c → 1/2/1 without conflict; odd cycle → one conflict; weighted odd
  cycle → the pair with the fewest reaching lights shares (failed against the first, greedy colouring before the
  exhaustive one was written); disjoint rooms still spread over both channels; a room reaching itself is no conflict.
- `SCBridge.Lights.AreaConfinement` (extended): three adjacent rooms; area-only lights and meshes take their own room's
  channel (a, c = 1, b = 2), unflagged, environment and torch-style lights all three, world queries name the room and
  its channel.
- `LIGHT_VERIFY` now requires `CastShadows == authored rule` with or without MegaLights.

Headless: importer 36/36; importer + Cutlass package + tar 38/38; `test_first_person.py` HEADLESS_OK on all three
ships.

## Room graphs (import log `LIGHT_AREAS`)

| Ship | Rooms → channel | Reach (lights, deepest overlap) | Conflicts | Samples in another channel |
|---|---|---|---|---|
| Cutlass | Cockpit 1, Neck 1, Body 2 | Cockpit→Neck (4, 3.63 m, shared), Neck→Cockpit (7, 8.89 m, shared), Body→Cockpit (19, 6.42 m), Body→Neck (8, 9.15 m), Neck→Body (4, 4.12 m) | 1 | 1089 / 54101 (2.0 %) |
| Gladius | Room_Gladius 1 | — | 0 | 228 / 24857 (0.9 %) |
| Scorpius | Room-cockpit 1 | — | 0 | 314 / 53032 (0.6 %) |

The Cutlass rooms reach each other in a triangle, so one pair must share a channel; the first, greedy colouring put
Cockpit and Body together (19 cargo-bay lights reaching the cockpit through the neck and the bulkhead). The colouring
now minimises reaching lights: Cockpit and Neck share (11 lights, across the open doorway), the cargo bay is apart.
Reach uses each room volume's bounds in its own segment space, so it can over-state reach, never miss it.

## GPU

```bash
SC_TIMEOUT=1800 ue/scripts/capture_route.sh l1-cutlass ue/scripts/routes/cutlass-ramp-to-cockpit.txt \
  -SCDefaults -SCLoad=DRAK_Cutlass_Black -SCCamera=first
SC_TIMEOUT=1200 ue/scripts/capture_route.sh l1-gladius-hull-seat ue/scripts/routes/gladius-hull-seat.txt \
  -SCDefaults -SCLoad=AEGS_Gladius -SCCamera=first
SC_TIMEOUT=1200 ue/scripts/capture_route.sh l1-scorpius-smoke ue/scripts/routes/scorpius-smoke.txt \
  -SCDefaults -SCLoad=RSI_Scorpius -SCCamera=first
```

All three routes complete (81, 35 and 18 steps). Cutlass against the same route just before WP-L1 (`mlq-cutlass`,
identical settings): GPU 7.17 / 6.53 / 6.97 → 6.84 / 6.25 / 6.82 ms (fewer shadow rays). Mean viewport grey after
the game's auto exposure: cargo bay 27.6 → 32.1, cockpit crouched 15.0 → 19.0, cockpit looking aft 35.1 → 42.1,
corridor and cockpit forward unchanged; the bulkhead close-ups differ in framing (the camera lands ~10 cm apart at a
wall), not in light. Side by side (`out/smoke/l1-before-after-cutlass.jpg`): the cargo-bay ceiling structure and the
cockpit seats are lit instead of shadowed; no light reaches the wrong room visibly.

## Limits

- Channels are per component: a mesh straddling rooms takes the majority room (2.0 % of Cutlass samples land in
  another channel); splitting such meshes per room would close the gap.
- Doors and ramps keep their rest-pose classification when they move.
- No interior reference images exist, so the comparison is against the game's rules, not against captures.

## Follow-ups on the same branch (2026-11-12)

**Hangar sky light.** The Viewer hangar's sky light used real-time capture, which records only sky components
(SkyAtmosphere, clouds, IsSky meshes); the hangar has none, so it captured black and UE drew its red on-screen
warning ("A sky light with real-time capture enabled is in the scene...") in every frame of the app. It now uses a
captured scene (`/Game/Maps/Viewer` edited in place; `create_viewer.py` keeps real-time capture only for presets with an
atmosphere). The warning's red pixels in the viewport band: 1089 → 0. Lighting is unchanged in the closed hangar
(Gladius orbit view mean 85.89 → 85.88): Lumen rays hit the walls before they could miss to the sky.

**Lumen ray-hit lighting — measured, not adopted** (`routes/cutlass-lumen-hit-lighting.txt`,
`r.Lumen.HardwareRayTracing.LightingMode`):

| View | Mode 0 (Surface Cache) GPU | Mode 2 (reflections) | Mode 1 (GI + reflections) | RMSE vs mode 0 (2 / 1) |
|---|---|---|---|---|
| Cargo bay | 6.65 ms | 9.62 ms | 12.68 ms | 2.06 / 2.28 |
| Cockpit aft | 6.92 ms | 11.72 ms | 17.52 ms | 2.37 / 2.76 |
| Canopy | 6.99 ms | 9.33 ms | 11.70 ms | 1.18 / 2.13 |

The images differ at the level of frame-to-frame noise (means within 1 %): these mostly rough interiors gain nothing
visible for +2.3..10.6 ms.

**Dirt is per-instance state, not material data.** The game's `HardSurface.cfx` and `LayerBlend_V2.cfx` apply dirt
only `if (objectDirt)`, where `objectDirt` is the instance's dirt amount (vertex input from instance data) and the
colour is `instData.dirtColor`: `dirtAmount = CalcDirtBlendAlpha(dirtColor, saturate(objectDirt − (vertexWear²·0.95)),
blendMap, AO)`, then `LayerAttribs_ApplyDirt` lerps albedo to the dirt colour and reflectance / smoothness to
`DIRT_SPECULAR` / `DIRT_GLOSS`, and emission × (1 − dirt) (`layerLib.cfi`). A freshly spawned ship has no dirt, so the
clean render is the game's own for that state; the dirt in the in-game references is a flown ship's. Drawing it
needs a viewer setting for the instance's dirt amount and colour feeding this rule.

**Deferred.** 182 small opaque Nanite meshes build no Lumen cards (the engine's voxel-density card builder, not a
size cut-off: a 608-triangle missile rack has none); room-straddling meshes stay one channel because a built mesh is
shared by all its placements.
