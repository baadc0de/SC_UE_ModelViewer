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

## Status (2026-11-13)

- Pipeline in place: export (`scripts/export_hangars.sh`), cook (`ue/scripts/cook_hangars.py` +
  `USCCookLibrary::CookEnvironment`: rigs baked static, repeated parts instanced, meshes/textures pooled under
  `/Game/SC/Hangars/Shared`, materials as constants per hangar, texture sources decoded from the game's DDS blocks by
  `sc_texels_decode_bgra8`), viewer (`USCViewerSubsystem::CookedEnvironmentLevel`: a cooked level is streamed in when one
  exists; `-SCNoCooked` forces the runtime export/import).
- The PNG export was tried first and rejected: its derived-roughness maps and missing layer textures do not match the
  viewer's DDS path (mirror floors, lost projector gobos).
- Verified: modern top-loading hangars (small: look matches the runtime import; ship on the floor).
- Open: front-loading and utility hangars put the origin on a tiled floor; the floor rule (elevator >= 20 m below the
  origin, else a floor tile >= 2 m below a point 2 m above it) gives inconsistent offsets there, and the utility large
  top-loader shows the ship in darkness. Needs a cooked-vs-runtime comparison per type.

## Follow-ups

- **Animatable cooked rigs** (owner: later): doors and the ship elevator are baked static in their presented pose because
  Keep cannot persist runtime skeletal meshes yet (`SCKeepAssets.cpp`: no editor ImportedModel). Building the editor
  source model for rigs would let cooked hangars keep their door/elevator clips.
- Galaxy-class ships as streamed environments (interior object containers as load units); blocked first by StarBreaker's
  CDF reader rejecting binary CryXML (`gdgt_fps_kegr_fire_extinguisher.cdf`, magic `CryX`).
