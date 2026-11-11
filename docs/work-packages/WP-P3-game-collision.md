# WP-P3 — First-person collision from the game's own physics geometry

## Why

The viewer collides the player against render meshes. At LOD 0 the Cutlass ramp's hinge lip stops the player at
the top of the open ramp (`ue/verification/WP-T1.md`). Star Engine never collides movement against render
meshes: every `.cga` / `.cgf` carries CryPhysics geometry in its `StatObjPhysics` chunk, simplified colliders
authored per node (`$proxy_*` hull pieces, a 744-triangle walk proxy for the whole Cutlass cargo bay, the ramp's
`PLAYER_ONLY_COLLISIONS` wedge across its hinge). StarBreaker does not decode the chunk yet.

## The chunk (IVO type `0x58DE1772`, in the `.cga` / `.cgf`, not the `.cgam`)

Little-endian. Measured on 40 094 ship geometry files (every blob decodes with the layout below).

- Header 80 bytes; `u32` at 40 = entry count `n`. Table at 80: `n` × (`u32 size`, `u32 key`). Blobs follow the
  table back to back in table order.
- `key & 0xFFFF` = NMC node index (`0xFFFF` = the whole object: `.cgf` without per-node physics);
  `key >> 16` = physics part: an index among a node's entries, **not a filter**. The Scorpius hull is part 0
  (`Body`, `Tail` 0…25, canopies, ladder: 114 colliders); the Cutlass hull pieces are part 1; damage / SDF blobs are
  part 1. `$IP_*` volumes are part 0 in 721 of 724 cases, but so are 735 ordinary colliders (seat bases, side doors).
- Blob kinds by first `u32`: `2` = CryPhysics `phys_geometry` (decode); `0xCAFEBABE` (v `0x100`) and
  `0xDEADBEB5` (SDF field) = Star Engine damage/destruction physics (render-derived mesh, distance field):
  recorded by kind and size only, not decoded, not collision.

CryPhysics `phys_geometry` (CryEngine `CGeomManager::SavePhysGeometry`), offsets in the blob:

| off | field |
|---|---|
| 0 | `i32` version = 2 |
| 4 | `i32` 0 |
| 8 | `Vec3` Ibody (inertia, unit density) |
| 20 | quat (x, y, z, w) mass frame |
| 36 | `Vec3` origin (mass centre) |
| 48 | `f32` V (volume) |
| 52 | `i32` refcount, 56 `i32` surface_idx, 60 `i32` 0, 64 `i32` nMats |
| 68 | `i32` geometry type: **0 box, 1 trimesh, 4 sphere, 5 cylinder, 6 capsule** (Star Engine numbering) |
| 72 | geometry |

Geometry (all in the node's own frame — the frame of that node's render vertices, CryEngine Z-up metres):

- box (blob 140 bytes): `Matrix33 Basis` (rows = the box's axes in node space: CryPhysics tests
  `Basis · (p − center)`), `i32 bOriented`, `Vec3 center`, `Vec3 half_size`; then a tree tail (ignore).
- sphere (156): `Vec3 center`, `f32 r`; then bbox tail (ignore).
- cylinder (176): `Vec3 center`, `Vec3 axis` (unit), `f32 r`, `f32 half_height`; tail ignored.
- capsule (176): same fields as cylinder (half_height = half the segment between the hemisphere centres).
- trimesh: at 72 `i32 nVerts, nTris, maxValency, flags, nA`; at 92 `u8 bVtxMap`, 93 `u8` (always 1);
  then `nA × u16`; if `bVtxMap` another `nTris × u16`; then `nTris × u8` per-triangle material ids; then
  `nVerts × Vec3` (f32); then `nTris × 3 × u16` indices; then tail (BV tree, ignore).

Checks on real data: box volume 8·hx·hy·hz = V; sphere 4/3πr³ = V; cylinder 2πr²·hh = V; capsule
2πr²·hh + 4/3πr³ = V; trimesh indices < nVerts, floats finite.

## Export contract (StarBreaker, branch `scviewer/physics-proxies`)

`ExportOptions` gains `include_physics: bool` (default false: upstream output byte-identical). With it, each
decomposed mesh-asset GLB carries the CryPhysics geometry of its source file:

- glTF node of an NMC node: `extras.sc_physics` = array of `{ "part": u16, "kind": "box"|"trimesh"|"sphere"|
  "cylinder"|"capsule", ...params }` in the same frame and units as that node's mesh vertices in the GLB (so the
  importer applies exactly the transform it applies to the render mesh). A trimesh references a glTF mesh by
  index (`"mesh": i`): one primitive, POSITION + indices, no material, not instanced by any node. Params: box
  `center, half_size, axes (3 row vectors)`; sphere `center, radius`; cylinder / capsule `center, axis, radius,
  half_height`. Physics nodes without a render mesh (`$proxy_*`, `PLAYER_ONLY_COLLISIONS`) are still emitted as
  glTF nodes with their transform.
- Whole-object entries (`0xFFFF`) go on the node that carries the object's mesh (state in the contract doc
  which one).
- Undecoded kinds are listed as `{ "part", "kind": "damage_mesh"|"sdf", "bytes" }` for diagnostics.
- Viewer: `scviewer-core` sets `include_physics` for its decomposed exports; `CACHE_FORMAT_VERSION` bumps.

## Importer (UE)

First-person movement (Pawn channel) collides with the game's colliders of every part, except the node-name
classes that are no obstacle for an actor (`interaction`, `tires_only`, `grid_volume`): all shapes tessellated into
hidden collision-only components (complex collision), one per rig owner. Render meshes keep visibility /
interaction traces but stop blocking the pawn. Inside the vehicle's physics-grid volume the root entity's own
colliders are ignored. Assets without any `StatObjPhysics` geometry (`.skin`) keep render-mesh collision.

## Status 2026-11-11 (not committed)

Done in the working tree (StarBreaker branch `scviewer/physics-proxies`, superproject uncommitted):
- `ivo/physics.rs` decodes the chunk exactly as tabled above (all 7 500 trimeshes and every box / sphere /
  cylinder / capsule of the 40 094-file survey); live test on the Cutlass ramp and bay passes.
- `decomposed_physics.rs` attaches it to decomposed mesh GLBs (`include_physics`; scviewer sets it, cache v20):
  source NMC indices aligned to the filtered GLB nodes; nodes the export drops (`filter_nmc_hierarchy` drops any
  name with a `proxy` / `nodraw` / `localgrid` segment — a pre-existing upstream name rule, flagged here) are
  recreated as transform-only nodes; whole-object physics rides the object's single geometry node (96 of 111
  sampled `.cga`), else `$physics_object`.
- UE: game colliders become hidden pawn-only parts per rig owner; render meshes keep visibility/camera traces but
  ignore the pawn; `sc.FP.ShowCollision`, `sc.FP.Blocker` debug commands.

Measured in the app (Cutlass, LOD 0): the ramp-hinge stall is gone walking up (X −1336 → past −815). Two semantic
gaps block the walkthrough, both outside the geometry:
1. **Physics grids.** The vehicle's hull parts (`Body`, wings, tail … — damage `Part` joints) each carry a default
   collider; together they are the exterior shell, which blocks the interior at the bulkhead (`sc.FP.Blocker`:
   `DRAK_Cutlass_Black` `Body`, normal −X). The game separates a ship's interior physics grid from its exterior;
   `AEGS_Gladius_Physics_Grid.cgf` holds a single 68-triangle CryPhysics volume, likely the grid boundary. Needed:
   how the grid volume is referenced per ship and which colliders belong to the grid (interior containers, items
   inside it) versus the exterior.
2. **Part-0 filters are names only.** `PLAYER_ONLY_COLLISIONS` (needed to walk *down* the ramp over the hinge),
   `TIRES_ONLY_COLLISION(S)`, `IGNORE_TERRAIN_COLLIDE`, `NO_TERRAIN_COLLISION`, `$IP_*` interaction volumes share
   identical headers; no chunk, `.meshsetup`, NMC property or DataCore record states the filter
   (`EGameCollisionClass` is per entity: Actor_*, Vehicle, Vehicle_Interior, …). The executable is protected, so a
   CRC search of the names is inconclusive.

### Update 2026-11-11 (later, still not committed)

**Physics grid — resolved from data.** Every vehicle record names its grid volume:
`VehicleComponentParams.physicsGrid.physicsProxy.path` (1 100 of 1 104 spaceship records; `gridPartsOnly` and
`portalExclusiveMode` false everywhere). The `.cgf` (Cutlass `DRAK_Cutlass_Black_No_Collide.cgf`, Gladius
`AEGS_Gladius_Physics_Grid.cgf`, Scorpius `RSI_Scorpius_nocollide.cgf`) holds one whole-object CryPhysics collider
in the entity frame (66 of 69 local proxies a trimesh, 3 a box): a closed volume around the hull — coarse, it also
encloses antennas, gear doors, thrusters on many ships. The ship's own `.cga` colliders are the exterior hull (the
Cutlass `.cga` has exactly 11, all hull pieces); its walkable interior comes from the object-container socpaks
(`Int_*.cgf`) and items. Rule implemented: an actor whose upright axis (feet to head) touches the volume is in the
grid, and there the root entity's own colliders do not block it; interior and item colliders always do.
- Export: `physics_grid.rs` → `scene.json` `root_entity.physics_grid = { source_path, shapes: [...] }` (shapes in
  the entity frame, trimesh vertices / triangles inline). Cache v21.
- UE: `ASCVehicle::PhysicsGridTriangles` / `HullColliders` (root game-collision components, tag `SCHullCollision`),
  `IsInPhysicsGrid` (ray parity / segment crossing); the first-person capsule toggles `IgnoreComponentWhenMoving`
  on them (log `FP_GRID inside=…`); spawn and noclip-exit checks use the same rule.
- Measured: Cutlass `cutlass-ramp-to-cockpit` passes end to end at LOD 0 with game collision (ramp foot X −1697 →
  bay → bulkhead → corridor → cockpit X 1009); centre-point membership failed at the ramp (head under the `Tail`
  overhang, centre outside), hence the axis. Gladius `gladius-hull-seat` unchanged (hull walk stays on top).

**Part-0 filtering was wrong.** Treating part 0 as filtered drops real geometry: the whole Scorpius hull (its hull
spawn is refused, `scorpius-smoke` falls off), and on the Cutlass the right side door, control-door walls, locker
door, seat bases, bunk / weapon-rack `$proxy_*` colliders and the ramp's `PLAYER_ONLY_COLLISIONS` wedge. Every part
must collide. The only colliders that should not block movement are identified by node name alone: `$IP_*`
interaction volumes (95 on the Cutlass) — structurally identical to `$proxy_*` colliders (NMC type 1, no render
mesh, physics only) — and `TIRES_ONLY_COLLISION(S)`. Open decision: accept the engine's node-naming convention as
the data's collision class (one documented table, like the existing `proxy` / `nodraw` / `localgrid` helper rule),
or collide with everything.

**Decision 2026-11-11 (owner): use the engine's node-naming convention.** `physics_class.rs` is the single table
(`class` on each record; contract updated). Importer: every part collides except `interaction` / `tires_only` /
`grid_volume`. Grid extensions (`grid_volume`) do not yet extend the grid volume itself (they ride moving lifts and
ramps); recorded as a gap. Cache v22. Verified:
- Rust: `starbreaker-3d` lib 909 passed (17 physics), `scviewer-core` / `scbridge-ffi` green, header check OK.
- UE headless: importer units 35/35 (new `FSCGamePhysicsClasses`, `FSCGamePhysicsGrid`); importer with the
  Cutlass package 37/37; first-person and rig suites pass on Cutlass, Gladius, Scorpius.
- GPU routes (LOD 0): Cutlass `cutlass-ramp-to-cockpit` end to end (ramp 7 colliders, 1 `tires_only` filtered;
  cockpit stop X 886 behind the pilot seat base); Scorpius `scorpius-smoke` hull spawn and hull walk restored
  (X 58, Z 310; baseline X 37, Z 303), its cockpit seats refuse a standing capsule now that the cockpit colliders
  count; Gladius `gladius-hull-seat` unchanged.
