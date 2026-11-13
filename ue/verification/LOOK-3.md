# Look iteration 3 — the ship's environment: designed hangar, then the game's own hangars (2026-11-12)

Follows [LOOK-2](LOOK-2.md), whose adjudicator ranked a real environment first.

## Designed hangar (the map, now the fallback)

`ue/scripts/create_viewer.py` rebuilds `/Game/Maps/Viewer` as a designed hangar instead of the uniform 18 % grey box:
dark floor, lighter 30 m landing pad with paint stripes, wall pillars every 10 m, a skirt band, a 10 m ceiling truss,
three 24 m key bars over the pad (shadowed rect lights, 5500 K, 146 234 cd each for 350 lux on the pad) with
fixture faces of matching luminance kept out of Lumen, eight 7000 K wall fill panels (126 267 cd each, a third of
the key on a vertical subject), amber accent strips. `verify_viewer.py` OK (24 expected actors, all key/fill
shadowed). Viewer choices, not game data.

## The game's hangars (the default environment)

The owner asked why the viewer does not use the game's hangars. The missing pieces were built:

- StarBreaker (`scviewer/socpak-glb`): socpak packages in the GLB decomposed format (the guard demanded Blend; the
  shared writer already produced GLB), CLI `--dds` (the game's DDS blocks instead of full-size PNGs). Unit test
  `socpak_packages_accept_glb_and_blend_but_not_bundled`.
- `scviewer-core`: the browser index lists every object container (`environments`, cache v25);
  `search_environments`, `export_environment_decomposed_mem_cached` (cached like entity packages, key `socpak:`).
  Tests `environment_search_matches_every_term_case_insensitively`, `environment_cache_key_is_separate_from_entities_and_stable`.
- `scbridge-ffi`: `sc_search_environments`, `sc_export_environment_mem` (header regenerated and installed).
- UE: `USCViewerSubsystem::SetEnvironment` — an archive `*.socpak` path is exported (textures at least mip 1, so the
  tar stays under the 2 GB package limit), parsed and imported beside the ship outside the vehicle slot (routes, first
  person and the emissive pass keep the ship); a package folder is imported directly. The map's own hangar is hidden
  and the sky light recaptured. Placement: the first upward-facing surface at least 20 m across below the
  environment's origin is its floor (a top-loading hangar's origin is on its roof doors), moved to the map floor.
  Options panel "Environment": the archive's 238 hangars or the procedural hangar; saved in
  `USCViewerSettings::Environment` (default `hightech_a/hangar_lrgtop_001`); `-SCEnvironment=<...>`, `sc.Environment`;
  `-SCDefaults` keeps the procedural hangar unless `-SCEnvironment` is given (reproducible review routes).
- Budgets: the per-import texture budget is a third of the GPU's dedicated memory (at least 2 GB);
  `r.Streaming.PoolSize=8000` at Epic texture quality.

Measured (Cutlass in `hangar_lrgtop_001`, 1600x900): export 26 s (1.04 GB tar, cached afterwards), 874 / 876
textures, import 24 s with warm derived data (84 s cold; the very first cold run spent ~5 min building derived data
with the viewer still responsive), floor found at the ship elevator (`ht_hangar_ship_elev_lrg_a`, 160 x 88 m, 64 m
below the roof doors), 1064 hangar lights shadowed through MegaLights, GPU 8.7–9.3 ms. Unit tests 38/38;
`scviewer-core` 41, `scbridge-ffi` green, `starbreaker-3d` socpak guard test green.

Not done: path-traced comparison and adjudication in the game hangar; per-ship hangar size choice; the small
on-screen warning text still shown at the top left.
