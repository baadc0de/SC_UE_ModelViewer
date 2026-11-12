# WP-L1 — Room light confinement and authored light shadows

Branch `wp/l1-room-light-confinement`. Follows WP-U3b §12 (two-channel confinement) and WP-U59 (MegaLights quality).

## Problem

The game shades a light flagged `affects_this_area_only` only on pixels of the clip volume (vis area) it sits in, and
casts shadows only from lights authored `shadowParams.shadowCasting=1` (Cutlass 3 of 225, Scorpius 4 of 127, Gladius
2 of 77). WP-U3b §12 approximated the clip volumes with two lighting channels — outside every room (0) and inside any
room (1) — and, because that cannot keep one room's lights out of the next, shadowed every light under MegaLights as
the containment substitute. WP-U59 measured the cost of that substitute: without shadow rays MegaLights is within
3–7 % of deferred lighting, with every light shadowed the Cutlass cargo bay keeps half its light (mean 23.3 vs 46.3).

## Rule (game data only)

- Rooms are the exported `interiors[].room_volumes[]`, grouped by `room` (GUID). Outside every volume is the root
  volume. A light belongs to the room containing its position; a surface to the room most of its sampled vertices
  are in, if more than half of them are inside any room (the WP-U3b vote, unchanged), else outside.
- UE has three lighting channels. Channel 0 is outside. Rooms take channel 1 or 2 so that two rooms never share a
  channel when an area-only light of one reaches the other (its attenuation sphere meets the other room's volume
  bounds): a graph colouring of the rooms by light reach, greedy in room order, preferring the less used channel.
  Reach cycles of odd length cannot be 2-coloured; those pairs are counted and logged (`conflicts=`).
- Area-only lights get their room's channel; lights without the flag (the game's AffectsEverything) and lights that
  belong to no vehicle (hangar, sun, the player torch) get all three.
- Shadows: exactly the authored `shadow_casting` lights (packages without the field keep the radius-quartile rule),
  with or without MegaLights.

Not in scope: per-pixel volumes (UE has none) and splitting meshes that straddle rooms; the log reports how many
sampled vertices fall outside their component's room so the residual error is measured.

## Acceptance

- Unit tests: room colouring (chain, odd cycle, disjoint rooms) and the extended `SCBridge.Lights.AreaConfinement`
  (three rooms, per-room light and mesh channels, all-channel lights); `LIGHT_VERIFY` checks the authored shadows.
- Headless importer and package suites green; GPU routes for the Cutlass, Gladius and Scorpius run; before/after
  brightness and frame cost recorded in `ue/verification/WP-L1.md`, with the reach graph and conflict count per ship.
