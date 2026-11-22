# WP-P4 — Openings in a hull: collision that follows doors and ramps

## Problem

The player cannot walk back into the C2 Hercules through its open rear ramp. Cargo routing met the same wall (WP-K1,
cross-review item 2) and worked around it: the hulls of the vehicles a move loads or unloads were ignored everywhere
(a move-scoped exception, flagged as a TODO). The Cutlass had the same symptom earlier; there the "upright axis touches
the grid volume" rule of WP-P3 happened to bridge the gap. Wanted: one data-driven rule for what a hull admits when a
door or ramp is open, the same for the walking player, spawn / noclip checks and cargo queries.

## Evidence (LIVE 4.10.2, C2 export of 2026-11-14 and the p4k)

1. **The blocker is static.** The plane that blocks is part of the root entity's `Body` collider
   (`CRUS_Starlifter.cga`, node 0, one trimesh of 3 068 triangles), not of the ramp. The ramp is its own entity
   (`Starlifter_Rear_Ramp.cga`, a `Door` item on `hardpoint_rear_ramp`) with 7 trimesh and 4 box colliders on its own
   nodes (`Rear_Ramp_Door`, `Pavement_Rear_*`, pistons); the importer already attaches those to their rig bones, so
   they ride the animation. Colliders following animated nodes was never the bug.
2. **The grid volume cannot bridge it.** A lengthwise section (x = 0.3 m and 3.1 m) shows the `Body` plane across the
   ramp opening 1 to 2 m *outside* the physics-grid volume (`CRUS_Starlifter_no_collide.cgf`), as `sc.Cargo.Section`
   measured in the app. Anything shorter than that gap is stopped between the two.
3. **No grid extensions here.** Of 2 396 collider records in the C2 package none has class `grid_volume`
   (96 `interaction`, 2 `tires_only`, 2 `no_terrain`). The gap WP-P3 recorded (extensions that ride lifts and ramps)
   is real for other ships but is not what blocks the C2.
4. **The data marks the opening.** A CryPhysics trimesh carries one sub-material index per triangle (decoded since
   WP-P3, never exported). On the C2 `Body` 2 932 triangles have index 0 and 136 have **255** (CryPhysics `char` −1:
   no surface). Clustered by shared vertices, those 136 are exactly the faces over the hull's openings:

   | triangles | area | where (entity frame, m) | what fills it |
   |---|---|---|---|
   | 18 | 181 m², planar | x ±7, y −37.7…−25.9, z −10.7…−2.2, normal aft-down | rear ramp (the blocking plane) |
   | 86 | 347 m² | x ±8.5, y 12.4…27.0, z −9.5…3.5 | front ramp and front doors |
   | 2 × 12 | 27 m² each | x ±14…21, y −35.5 | main engine nozzles |
   | 4 × 2 | 43–44 m² each | x ±10…19, z −11.5…−5.4 | landing-gear bays |

   `Nose_Mesh`: 200 of 482 (the glazed nose). Cutlass Black: 26 of 380 and 32 of 480 (rear ramp plane with normal
   (0, −0.68, −0.74), side doors, cockpit glazing, a 2 × 2 m top hatch).
5. **Survey**, 21 700 `.cga` / `.cgf` under `Objects/Spaceships/Ships` (16 045 trimeshes, 2 355 546 triangles): the
   indices are ordinary sub-material slots (0…86) plus 255 on 32 278 triangles. 697 trimeshes in 275 files mix both,
   nearly all of them a ship's exterior hull `.cga` (Gladius, Cutlass Black / Blue / Red, Hull A, Carrack, Idris, …).
   87 trimeshes have no surface on any triangle: almost all 12-triangle boxes on lifts, elevators, ramps, docking
   collars and cargo racks (helper volumes, the kind the node-name class `grid_volume` already filters).

So the hull collider is authored closed, and the faces that close it over an opening carry no surface. What fills the
opening (ramp, door, gear door, thruster, interior) is another part with its own colliders in its own pose.
How Star Engine itself treats a face without a surface is inferred from this correlation, not read from the
executable.

## Options considered

Ideas from Codex Astra (read-only consultation; its ranking was 1, 2, 4, 3, 5, 6, 7), with the critique:

1. *Colliders follow their animated owner.* Already true for the ramp's own colliders (evidence 1); the blocker is
   the stationary `Body`. Moving `Body` would be wrong. Rejected as the fix, kept as the state of affairs.
2. *Union the grid with animated `grid_volume` extensions.* The C2 has none (evidence 3), so it cannot fix the case
   at hand, and extensions grant admission where a lift stands, not where a door is open. Still the right follow-up
   for lifts (WP-P3 gap, unchanged).
3. *Reconstruct native grid transitions and collision classes.* Highest fidelity, but the semantics live in the
   protected executable; `portalExclusiveMode` / `gridPartsOnly` are false on every vehicle. Research, not a fix.
4. *Pose-resolved entrance corridors from portal / room data.* Needs door-to-portal association and invents a
   corridor shape; superseded by 5, which needs neither.
5. *Detect obsolete hull patches by geometry difference.* The right target, the wrong means: the data already names
   the patches (evidence 4), so nothing has to be guessed from render geometry.
6. *Carve the hull with the door's swept volume.* Carves walls beside hinges; needs per-door tuning. Rejected.
7. *Grid membership by contact with hysteresis.* History-dependent: no answer for a spawn check or for a planner
   that tests poses out of order. Rejected.

Also rejected: keeping toggles ("when door X opens, disable collider Y") in any form, and enlarging the grid volume
by a margin (a game value we would invent).

## Chosen design

**A collider's triangles without a surface are not obstacles when the rest of the collider has one.** The exporter
splits them off into their own record (class `no_surface`); the importer builds no collision for that class, exactly
as for `interaction`, `tires_only` and `grid_volume`. A trimesh with no surface on any triangle (the 87 helper boxes
of evidence 5) is left an ordinary collider: nothing in it marks a cap, and what those boxes are was not established.
Nothing else changes:

- "Open" is the pose: a shut ramp blocks with the ramp's own colliders, which ride its rig bones; an open one is out
  of the way. No toggles, no door or ship names, no per-opening state.
- The WP-P3 grid rule stays as it is (the rest of the hull is ignored inside the grid volume) and now only has to
  separate skin from interior, not to admit through openings.
- One rule for every consumer, because it is applied where collision is built: player movement, spawn and
  noclip-exit checks and cargo overlap / sweep queries all see the same components. The WP-K1 move-scoped exception
  is deleted; `FSCCargoHullRule` is by position for every vehicle.

Limits, stated: an opening filled by nothing that collides is open. On the C2 that is the glazed nose seen from
outside (the bridge interior has its own colliders; cargo queries also test render geometry). Engine nozzles and
gear bays are filled by items and gear doors.

## Contract changes

- `vendor/StarBreaker/docs/decomposed-export-contract.md`: in a trimesh with both kinds, the triangles with
  sub-material index 255 become a second `trimesh` record on the same node (`class: "no_surface"`, same `part`),
  after the record of the triangles with a surface. A node whose name gives a class keeps one record, and so does
  a trimesh without a surface on any triangle.
- `physics_class.rs`: `NO_SURFACE`, `NO_SURFACE_CLASS` beside the node-name table.
- `scviewer-core` `CACHE_FORMAT_VERSION` 28.
- UE `FSCMeshImporter::PhysicsBlocksActor`: `no_surface` does not block.
- UE `USCCargoSubsystem::HullRule(Base, HalfExtent, Rotation)`: the `Move` / `Drone` parameters and
  `FSCCargoHullRule::Partners` are gone.

## Verification plan

- Rust: `decomposed_physics` test `triangles_without_a_surface_become_a_no_surface_record`; `starbreaker-3d`
  physics tests; UE `SCBridge.GamePhysics` (class table).
- C2 rear ramp, first person: `viewer/scripts/routes/c2-ramp-walk-in.txt` (ramp open, start on the hangar floor behind the
  ramp, walk up into the hold; `FP_GRID inside=1` and a position inside the hold).
- Cutlass: `cutlass-ramp-to-cockpit` unchanged end to end.
- Cargo: `cargo-c2.txt` with scenario `c2_hangar`: both jobs Done, no `CARGO_PATH ... found=0` on load legs, with
  the exception removed.
- Hull walks that rest on hull colliders: `gladius-hull-seat`, `scorpius-smoke` (their hulls have capped openings).

## Results 2026-11-15 (Win64, D3D12, not committed)

Positions are vehicle-relative centimetres (X forward), capsule centre.

- Rust: `starbreaker-3d` lib 945 passed (the new test included), `scviewer-core` 42 passed. `scbridge-ffi --test
  shell` fails on this Windows host (a shell-runner test; not looked into). The new test was written before the
  exporter change but not run against the old code.
- UE: `SCBridge.GamePhysics` and `SCBridge.Cargo` (25 of 25) pass.
- `c2-ramp-walk-in` (`P4_c2walkb.log`): floor X −6500 → ramp foot −4797 → on the ramp −3093 → hold **−1388**
  (`FP_GRID inside=1`); back out to −6887; in again to −1387; ramp shut, walking aft stops at −3144 against the ramp's
  own collider (`Pavement_Rear_Top`). **Baseline** (same binary and export, `no_surface` made blocking for one run,
  `P4_c2walk-base.log`): both walk-ins stop on the ramp at **X −2941**.
- `cargo-c2` / `c2_hangar` with the exception removed (`P4_cargoc2b.log`): jobs 1 and 2 Done, 3 of 3 moves each
  (89.4 s, 59.4 s), 160 SCU in the hold, 21 routes found with `hits=0`, none not found, longest search step 3.64 ms.
  Four `stalled` replans (one before this WP). The first run (`P4_cargoc2.log`) ended job 1 at 2 of 3: move 1 stalled
  where the load leaves the lift room, 170 m from the ship, and nine replans from there found the start enclosed by
  hangar colliders. The hangar has no hull colliders and no `no_surface` record, so this WP does not change what is
  tested there; the same build also carries other uncommitted work (cargo subsystem edits, the C2 port-geometry
  exporter fix, which lowers the hold's goal by 72 cm). Cause found afterwards (`viewer/verification/WP-K1.md`, K1g): the
  guard's look-ahead became a 5.6 m chord on the long frame of the route's camera cut; with this WP the searches are
  slower, so the first load is still among the stock at that moment. Fixed there, in the guard and the carry.
- `cutlass-ramp-to-cockpit` (`P4_cutlassb.log`): end to end, cockpit stop X 1064.77 as in the baseline run
  (`P4_cutlass-base.log`); the bulkhead stop is X −109 (baseline −89) and the embed point 34 cm further forward (a
  cap is no longer the surface under the crosshair). The second noclip exit of that route is refused in both.
- `gladius-hull-seat`: hull walk stays on the hull (Z 187…190), last step X 970, Y −73 (as in WP-P3).
  `scorpius-smoke`: hull walk X 57.59, Z 309.89 (WP-P3: 57.45, 310).
- `FP_GRID` flips on every few centimetres between X −2780 and −2630 on the C2 ramp (the grid volume's boundary
  runs along the path there). Harmless now that nothing on the way depends on it, but it is log noise and a sign the
  axis test sits on a surface: not addressed.

Not verified: the C2's glazed nose and the Cutlass canopy from outside (reachable only in noclip or from the hull);
any ship beyond the C2, Cutlass Black, Gladius and Scorpius; the Web (cooked) build, whose cooked ships need
re-cooking to pick the class up.
