# WP-K3 — Cargo in space (asteroids, zero g, loose cargo)

## Why

WP-K1 / K2 run in hangars: a floor, gravity, an environment that encloses everything. The vertical slice also moves
cargo between ships in open space, where loose cargo (WP-K2 beacons) is the normal case, not the fallback: there is no
floor to put a box down on, and what is let go stays where it is. This package adds a space scenario and tightens the
cargo code paths that assumed a hangar.

## Behaviour

1. **Space scenarios.** A scenario with `"space": true` loads no environment (the map hangar stays hidden), sets the
   world's gravity to zero and lights the scene with transient lights. Their values are viewer choices, set by the
   owner after an interactive session:
   - a sun: a directional light at 8000 lux with shadows and forward-shading priority;
   - a blue-tinted fill from the opposite side at a tenth of the sun's strength, without shadows. Without it, the
     sides away from the sun are black.

   A scenario's `sun_lux` or `sc.Space.Sun <lux>` sets the sun (the fill follows). Auto exposure absorbs most of the
   absolute level: 10 000 and 50 000 lux look alike, 50 lux is dim. Vehicles float where the scenario puts them (`at` in metres, `yaw` or `rot` [pitch, yaw, roll]) and are
   not set down on a floor.
2. **Obstacles.** `obstacles[]` places game entities (here the mineable asteroids `AsteroidSTypeMineableRock` /
   `AsteroidETypeMineableRock`) as static vehicle instances. They are collision for drones, routes and loose spots like
   any other geometry; nothing in the cargo code knows they are asteroids.
3. **Loose stock.** `loose[]` puts boxes loose at the start (entity, count, commodity, owner, `at` or random). Random
   spots come from the scenario's `seed`. In space a spot is sampled in 3D in a room around the ship (its bounds grown
   by their largest side), with no floor trace; the spot must be free and the box's lift corridor clear.
4. **Zero g.** A box weighs nothing: one drone holds any box its beam can take by volume; team size and loaded
   acceleration follow `min_load_accel` (the team must still accelerate the mass). No gusts in vacuum (as before).
   A box let go in space holds station where it was let go: its speed is taken off while it is loose (viewer choice;
   in vacuum nothing would stop a box let go with the load's speed, and a drifting box would never come to rest in a
   beacon). It comes to rest within half a second and founds or joins a beacon as in K2. Drifting loose cargo (moving
   beacons) is not modelled.
5. **Transfers** from beacons to ship grids, and between the two ships' grids, work as in the hangar; ships' own drone
   docks serve them (no shared hangar pool in space).

## Data

- Scenario JSON: `space`, `seed`, `vehicles[].at` / `yaw` / `rot`, `obstacles[] {entity, at, rot|yaw}`,
  `loose[] {entity, count, commodity, commodity_label, owner, at?}` (`FSCCargoScenario::bSpace`, `VehiclePoses`,
  `Obstacles`, `Loose`, `Seed`).
- `USCViewerSubsystem::SetSpace(bool)` / `IsSpace()`; RPC `/state` `load.space`; ships spawned through the RPC in space
  are not rested on a floor.
- `FSCCargoPlanner::DronesFor`: with g = 0 the lift needs one drone (volume rules still apply).
- New scenario `ue/SCViewer/Config/Cargo/hull_a_space.json`: two MISC Hull A 45 m apart, five asteroids 40–60 m out,
  stock on ship 0's grids, nine boxes loose (six random, three placed in a row).

## Hardening found on the way (exploration batches across all scenarios)

- **Orphaned drone.** A drone flying home after a failed move could be claimed by a new move that was still planning
  its routes; the returning move then counted it as handed over and ended. When the claim was let go before the new
  move flew (cancel, no route, another box swapped in), the drone belonged to no move and hovered for good, holding
  the scene unsettled (seen as 600 s settle timeouts after a cancel). Now a returning move keeps a claimed drone until
  the claiming move actually flies it, and takes it home again if the claim is let go. The state log reports drones
  with `in_move`, and the anomaly `drone_orphaned` flags a drone out of its bay with no move.

## Tests

- Planner (automation): `SCBridge.Cargo.ZeroGravityTeam` (team size at g = 0: one drone for any mass the beam can
  hold, volume rules unchanged, no drones without lift force).
- Exploratory (RPC): `explore.ps1 -Scenario hull_a_space`: ships spawned floating at random places, loose spawns in
  3D, transfers from beacons, cancels and drones hit in flight; asteroids are never despawned.

## Verification (2026-11-16, LIVE 4.10.2, Win64 Development, off screen through the WP-H1 harness)

- `Automation RunTests SCBridge.Cargo`: 45 / 45 Success (new: `ZeroGravityTeam`).
- `hull_a_space`, scripted (`out/k3/smoke/`):
  - Settled in 67 s: `load.space = true`, environment `none`, both ships and five asteroids where the scenario puts
    them.
  - 9 loose boxes found spots in 3D and came to rest in 5 beacons; no anomalies.
  - A transfer from a beacon to ship 0 (16 SCU) was done in 39 s in zero g, by one drone (video `space-transfer.mp4`).
- The orphaned drone found by batch b1: `explore.ps1 -Scenario hull_a_hangar -Seed 11` ran again after the fix and
  went from 66 min with six 600 s settle timeouts to 5 min and failures=[].
- Exploration batches with HitInFlight 0.5 (failures=[], anomalies=[], crash=false in every run):

  | scenario | seed | steps | jobs done / failed / cancelled | overlaps | retargets | let down | loose, beacons at the end |
  |---|---|---|---|---|---|---|---|
  | hull_a_space | 21 | 40 | 3 / 2 / 0 | 0 | 2 | 1 | 11, 8 |
  | hull_a_space | 22 | 40 | 3 / 0 / 0 | 0 | 2 | 1 | 25, 18 |
  | hull_a_space | 23 | 60 | 9 / 1 / 1 | 1 | 6 | 1 | 25, 20 |
  | hull_a_space | 24 | 60 | 7 / 3 / 2 | 2 | 4 | 4 | 26, 22 |
  | hull_a_space | 25 | 60 | 4 / 6 / 0 | 0 | 4 | 4 | 17, 13 |
  | c2_hangar | 13 | 40 | 6 / 9 / 3 | 0 | 3 | 3 | 13, 5 |
  | hull_a_hangar | 13 | 40 | 6 / 4 / 3 | 0 | 3 | 2 | 9, 5 |
  | hull_a_multi | 13 | 40 | 12 / 3 / 1 | 0 | 5 | 3 | 6, 5 |

  Failed jobs are the hits the run makes (interrupted loads, drones lost under a load) and lifts blocked by a box
  on top; each failed with its reason. Runs 23–25 use the explorer's destination choice (mostly grids with room,
  closed grids opened more often when the open ones are nearly full); before it, most transfers in space were
  refused for room (a Hull A takes 64 SCU and starts with half its grids closed).

Known limits:

- In zero g a drone can touch its own load (up to 5 cm) while placing it on a Hull A underside plate. This was seen 3
  times in about 30 space moves and never in a hangar. With no weight the beam does not keep the box away from the
  drone, and the overlap check counts the drone's own load.
- Loose cargo does not drift (see Behaviour 4).
- The sun's direction is fixed (no star from the archive).

## Interactive scenario

`hull_a_space_fleet.json`: four Hull A, each with a dock and two drones, stock on three of them, the five asteroids
and 13 loose boxes in beacons, for exploring transfers through the mobiGlas.

Recording a session with its UI needs a screen capture of the window. The in-engine `/record` takes the scene before
the UI, and GDI capture of a D3D12 window is black. ffmpeg's `ddagrab` (Desktop Duplication) on the window's client
rectangle works.
