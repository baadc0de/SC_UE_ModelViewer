# WP-C1 — cooked hangars

Goal (owner, 2026-11-12): the few player hangars are pre-baked as UE maps for load speed and runtime cost, instead of
exporting and importing an object container on every start.

## Scope

- The base hangar of each style and size: modern (`hightech_a`) and utility (`util_a`) x small/medium/large top/front +
  XL top = 14 maps. City and dressing variants (`_newbab`, `_orison`, `_lorville`, ...) are not cooked.
- One plain level per hangar (not World Partition): a hangar is one ~200 m room, always loaded whole, so streaming cells
  and HLODs buy nothing. The viewer loads it as a level instance.
- Cooked content is game-derived: it never enters git (gitignored content folder).

## Pipeline

1. `scripts/export_hangars.sh`: StarBreaker `socpak export` (decomposed GLB + DDS at full resolution, LOD 0: the DDS path the viewer imports; the cook decodes each texture's blocks to an editable source via sc_texels_decode_bgra8) into `out/hangars`,
   one shared `Data/` tree.
2. Editor cook (commandlet): import the package as an environment (effect-only placements hidden, repeated parts
   instanced), present the door fragments' Open end pose (elevator raised), place the ship floor at the map floor,
   **bake rigs static** in that pose, keep meshes/materials/textures as assets, save the level.
3. Viewer: `SetEnvironment` loads the cooked level when one exists for the requested container, else falls back to the
   runtime export/import.
4. Hangar by ship size: the cook records each hangar's ship capacity in `/Game/SC/Hangars/HangarIndex`
   (`USCHangarIndex`): the ship elevator's footprint, the clear height above it (vertical traces; none = open roof) and
   whether ships enter through a roof hatch (its largest moving part above the floor lies flat) or front doors. The
   viewer treats the chosen hangar (settings, environment picker) as a style: after each ship loads it takes the
   smallest cooked hangar of that style whose platform holds the ship's solid footprint (centred on the elevator) and
   height, the chosen entrance kind first; none holds it: the largest. `-SCEnvironment` / `sc.Environment` load exactly
   what they name; `USCViewerSettings::bFitHangarToShip` turns it off. Already-cooked hangars are measured with
   `SC_COOK_MEASURE=1 ue/scripts/cook_hangars.sh` (~30 s each).

## Status (2026-11-13)

- Pipeline in place: export (`scripts/export_hangars.sh`), cook (`ue/scripts/cook_hangars.py` +
  `USCCookLibrary::CookEnvironment`: rigs baked static, repeated parts instanced, meshes/textures pooled under
  `/Game/SC/Hangars/Shared`, materials as constants per hangar, texture sources decoded from the game's DDS blocks by
  `sc_texels_decode_bgra8`), viewer (`USCViewerSubsystem::CookedEnvironmentLevel`: a cooked level is streamed in when one
  exists; `-SCNoCooked` forces the runtime export/import).
- The PNG export was tried first and rejected: its derived-roughness maps and missing layer textures do not match the
  viewer's DDS path (mirror floors, lost projector gobos).
- Verified: modern top-loading hangars (small: look matches the runtime import; ship on the floor).
- Placement: the ship stands on the ship elevator, found by geometry: the largest flat moving part (under a rig,
  >= 20 m across, <= 15 m thick) in the presented pose; of moving parts at least half that area the highest (the
  platform, not the shaft cover doors; roof-hatch leaves and cargo pads are smaller; static ceilings never move). Its
  centre goes under the ship and its top to the map floor. Hangars place their origin differently (modern top-loaders
  on the roof hatch; front-loading and utility hangars on the floor or at a building corner), so the origin cannot be
  used. All 14 pick their ship elevator (cook logs `ENVIRONMENT_PLACE elevator=...`); checked in the viewer: modern
  large top/front, utility large/small top.
- All 14 cooked (`ue/scripts/cook_hangars.sh`: one editor process per hangar in a memory-capped systemd scope; a
  single commandlet cooking all of them grew to ~26 GB and was OOM-killed). Content/SC/Hangars ~ the shared pool plus
  per-hangar materials and levels.
- Validation (modern large top-loader, Scorpius): hangar ready in 7.6-10.3 s cooked vs 47 s runtime; lit captures within
  3 % RMSE (cooked at full texture resolution); frames ~2 ms slower cooked (render thread +2.5-4 ms, GPU -0.7 ms). Likely
  cause (LOOK-7): traced short-range AO rebuilds the material shader binding table every frame on Vulkan, per ray-traced
  segment; game hangars now run screen-space short-range AO by budget, which removes that cost.
- Capacities (2026-11-13): small 48 x 48 m, medium 88 x 56 m, large 160 x 88 m, XL 304 x 184 m platforms, both styles;
  clear height 37-72 m (front-loaders and utility top-loaders), modern top-loaders open; entrance kind derived from
  geometry matches every hangar. Viewer (saved choice util_a large top): Cutlass (35.7 x 26.1 x 10.3 m) -> small top,
  Galaxy (134.2 x 62.5 x 23.1 m) -> large top, Cutlass again -> small top (`ENVIRONMENT_FIT` log lines).
- The runtime path exports utility hangars again: StarBreaker's physics step read character definitions (binary CryXML
  `.cdf`) as chunk files (fixed on `scviewer/cdf-cryxmlb`).

## Follow-ups

- **Animatable cooked rigs** (owner: later): doors and the ship elevator are baked static in their presented pose because
  Keep cannot persist runtime skeletal meshes yet (`SCKeepAssets.cpp`: no editor ImportedModel). Building the editor
  source model for rigs would let cooked hangars keep their door/elevator clips.
- Galaxy-class ships (2026-11-13, `ue/verification/LOOK-7.md`): the RSI Galaxy loads at runtime (47 s warm, 11-15 ms frames
  in the XL hangar). Measured, streaming its 14 interior containers by camera position would save < 1 ms once short-range
  AO follows the ray-tracing budget, so it stays one vehicle. Cooking a ship as a map would cut the load to the cooked
  hangars' ~10 s but bakes its doors, lifts and gear; it waits for animatable cooked rigs above.
