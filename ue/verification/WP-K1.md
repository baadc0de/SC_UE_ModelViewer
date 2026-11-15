# WP-K1 verification

## K1b: cargo domain, grids, boxes, drones (Win64, UE 5.9, RTX, 2026-11-14)

Scenario `ue/SCViewer/Config/Cargo/hull_a_hangar.json`, route `ue/scripts/routes/cargo-hull-a.txt`. Host:

```powershell
UnrealEditor.exe <abs>\ue\SCViewer\SCViewer.uproject -game -windowed -resx=1600 -resy=900 -SCCamera=first -SCLoad=MISC_Hull_A `
  -SCEnvironment=Data/ObjectContainers/PU/loc/mod/common/hangar/hightech_a/hangar_lrgtop_001.socpak `
  -SCCargoScenario=hull_a_hangar -SCScript=<abs>\ue\scripts\routes\cargo-hull-a.txt -SCShotDir=<abs>\out\k1\shots23
```

### Grid origin convention (for the K1a contract doc)

**The container entity's transform is the centre of the lattice floor.** The lattice spans
`[-interior.x/2, +interior.x/2] x [-interior.y/2, +interior.y/2] x [0, interior.z]` in the entity's own axes, its floor
on the entity's XY plane and its +Z (the grid normal) the direction items are drawn off along; `grid_pos_offset` (zero on
every grid here) adds to the entity origin. Checked with `sc.Cargo.Debug 1` and `sc.Cargo.Origin corner|bottom|centre`:

- Hull A cargo plates, extended (`plate3-*.png`, `plate5-*.png`): with *bottom centre* the 2.5 x 5 x 2.5 m lattice covers
  the plate face exactly and stands out of it (the plates are vertical when extended, so the grid normal is horizontal,
  pointing away from the spindle); with *corner* it sits a quadrant off the plate, with *centre* half of it is inside the
  ship. The grid entity sits at the plate's `hardpoint_cargo_grid` bone with K1a's `local_transform_sc` (0.25 m out).
- Freight elevator (Large, `elevator-*.png`): with *bottom centre* the 17.5 x 20 x 5 m lattice is the floor of the
  freight elevator's room behind the CARGO wall; with *corner* it reaches out through the wall into the hangar (boxes
  placed by it intersect the wall: `CARGO_PICK ... blocked by SCVehicle_0 (0.0 m of headroom)` in the corner-rule runs).

Lattice axes in UE: the grid component's X = SC y, Y = SC x, Z = SC z (the frame placed items already use); the cell is
the container's `min_permitted_item_size` per axis (1.25 m), cells = interior / cell (Hull A 4x2x2, Large elevator 14x16x4).

### Automation tests

`UnrealEditor-Cmd.exe <uproject> -ExecCmds="Automation RunTests SCBridge.Cargo" -TestExit="Automation Test Queue Empty" -unattended -nullrhi`:

```
Test Completed. Result={Success} Name={BestFitPack}        Name={DigHold}          Name={DigImpossible}
Test Completed. Result={Success} Name={DigRelocate}        Name={FlightProfile}    Name={FlightRoute}
Test Completed. Result={Success} Name={FlightTeam}         Name={GravitySupport}   Name={LatticeFromContainer}
Test Completed. Result={Success} Name={MaxItemSize}        Name={PickFallback}     Name={PlacementYaw}
Test Completed. Result={Success} Name={ReaderFields}       Name={StackedRemoval}   Name={TransferPlan}
**** TEST COMPLETE. EXIT CODE: 0 ****   (20 of 20 Success, K1bTests26.log; TeamByVolume and Sway since run 25; new: DigCancelReturn,
MoverDirection, TractorRecord; extended: PlacementYaw, FlightRoute, PickFallback)
```

`FlightRoute` plans around a synthetic wall between start and goal and asserts the spline's minimum clearance > 0 with
0 sweep hits (`FSCCargoRoute::MinClearance`), that the straight line through the wall measures as hitting it, and that a
goal inside the obstacle is refused without a search. `FlightProfile` includes the run-12 crash regression (a one-point
route, `Points.Add(Points[0])` aliasing). `PickFallback` covers the covered-box substitute and that a substitute is never
larger than the box it replaces. `DigHold` / `DigRelocate` / `DigImpossible` are the temporaries cases (a), (b), (c).

### Run evidence (run 19, `K1bRun19.log`, shots `out/k1/shots19`)

Route `cargo-hull-a.txt` with four jobs, all `Done` with every move done and no failure:

```
CARGO_JOB id=1 state=Done t=16.9 moves=1 done=1   16 SCU ship bottom-left plate -> bottom-right plate (around the hull)
CARGO_JOB id=2 state=Done t=42.0 moves=2 done=2   32 SCU elevator -> ship; refusals: 32 / 24 SCU boxes longer than 5 m
CARGO_JOB id=3 state=Done t=65.4 moves=2 done=2   16 SCU elevator grid 1 -> grid 2, dug out (2 SCU box held aside, put back)
CARGO_JOB id=4 state=Done t=39.1 moves=2 done=2   32 SCU ship -> elevator
```

**Obstacle avoidance.** Every route a load or a drone flew was swept free: 25 of 25 `CARGO_PATH ... found=1` lines show
`hits=0` (the 2 `found=0` first attempts were for the elevator legs, which then reel in and plan again), for example:

```
CARGO_PATH job=1 move=1 box=12 leg=load found=1 direct=0 waypoints=5 expansions=205 length_m=23.6 minClearanceCm=25 hits=0
CARGO_PATH job=2 move=3 box=6 leg=load found=1 direct=0 waypoints=76 expansions=97 length_m=100.5 minClearanceCm=0 hits=0
CARGO_PATH job=3 move=5 box=1 leg=load found=1 direct=0 waypoints=6 expansions=253 length_m=180.5 minClearanceCm=0 hits=0
```

`minClearanceCm` is the clearance beyond the swept shape, which already includes a 20 cm margin around the box and the
beam/drone column. It reads 0 when a route uses a tight passage: the elevator doors, where the beam reels in to its 0.5 m
minimum (`reel_in hang_m=0.50`). The runtime guard sweeps the stretch ahead every frame. Loads in the air give way by box id:
at lift-off it is first come, first served, and in transit the later load waits (`CARGO_YIELD`). A blocked stretch replans
from where the load is (`CARGO_REPLAN ... found=1`). Without a route the load holds position (`bStalled`), retries with a
backoff and fails after 10 tries with `no free route from where the load stopped`. Run 14 showed that path firing; run 15
removed the deadlock behind it. A neighbour placed after planning moves the hover point out along the normal.

**Physical carry (inbound and outbound).** A `CARGO_CARRY` sample every second shows the box on its physics handle following
the flight (`sim=1`, lag 4-48 cm at up to 10 m/s). The lift and the placement are kinematic along the grid normal (`sim=0`):

```
CARGO_MOVE  job=2 move=3 box=6 team=1 load_kg=15000 loaded_accel_m_s2=7.28 empty_accel_m_s2=12.00 lift_m=2.80 hang_m=0.50
CARGO_CARRY job=2 move=3 box=6 step=Transit box_at=V(X=-3708.68, Y=3917.48, Z=302.61) target=V(X=-3668.76, Y=3893.76, Z=299.90) lag_cm=47 speed_m_s=10.30 sim=1
```

**Dig-out.** `CARGO_HOLD job=3 move=4 box=2 point=...` sets a hover point beside the lift corridor of the box beneath, and
outside it. The 16 SCU box waits until the held box hovers there, is carried, then the held box is put back
(`CARGO_REPLAN reason=put_back found=1`) on the slot the planner recomputed with the target gone (`2:2SCU@X=0 Y=2 Z=0`).

**Extend / retract.** The ship's plates extend on request (`CARGO_GRID_ACCESS id=3 request=extend clips_played=2`). A retract
of loaded plates is refused with its reason:
`CARGO_ACCESS grid=3 request=close ok=0 reason="Grid holds 16 SCU; unload it before retracting"` (the shared hull clip refuses
the other plates too). The elevator grids open through K1e's `FSCEnvironmentAccess::OpenGrid`
(`CARGO_GRID_ENV_ACCESS id=1 rigs_tested=21 rigs_moved=1 seconds=2.0`).

Shots checked: `plate3/5-*` and `elevator-*` (origin rules), `around-1..6` (16 SCU flown over the hull, the drone over
the box), `inbound-1..11` (elevator to ship, the drones and boxes in the elevator room), `dig-1..6`, `dig-restored`,
`loaded`, `outbound-*`, `back-1/2` (boxes back in elevator grid 1), `mobiglas`.

### Cross-review fixes (Codex review `out/k1/review-k1b.md`)

| Finding | Fix | Evidence |
|---|---|---|
| P1-1 unload mid-job | `ForgetGrids` ends each affected job's open moves properly. It lets go of the box, releases the reservation on the grid that stays and the box state, and sends the surviving drones home (the job ends once they're back). It destroys only boxes on the lost grids and the lost jobs' boxes in the air. Other transfers keep theirs. | `CARGO_JOB id=N grid_gone=1 drones_returning=...` |
| P1-2 drone lost mid-move | The team is validated every tick. A lost drone's flight and formation slot are dropped with it, and a new lead takes over the beam. A team below the drones needed to hold the box's weight fails the move ("N drone(s) left cannot hold the box", or "the drone team is gone"), and the box is let down under gravity. A team that can still hold it goes on with the smaller team's loaded acceleration. | route step `sc.Cargo.LoseDrone busy`, `CARGO_TEAM ... lost_drones` |
| P1-3 cancel during dig-out | The held box's slot is decided when it goes back (`FSCCargoPlanner::ReturnSlot`), against the grid as it is then. It keeps the planned slot while that is free and supported, else takes the best free slot. | test `SCBridge.Cargo.DigCancelReturn` (target still in place: the held box goes on top of it, not into it) |
| P1-4 unswept turn | The load arrives at its pickup rotation at a turn point and turns onto the lattice in free air there (`TurnClear`: the box with a 5 cm margin swept every 10 degrees from the arriving to the slot rotation). The turn point is the hover point on the slot's normal line, or straight above it when the turn needs room (a 5 m box tilting onto a vertical plate near the floor), and the turned box's way down to the hover point is swept. From the hover point the box goes down along the normal at the slot rotation (`DescentClear`: the box 2 cm inside its size; what it overlaps in its own slot, the plate's rails, is the grid's structure and ignored). `SlotApproach` checks all of this when the move is planned, and it is checked again from where the box actually hangs when it arrives. If blocked, the load replans to a hover point further out (`turn_room`), and the move fails after the replan limit. | `CARGO_TURN` when blocked, then `CARGO_REPLAN reason=turn_room` |
| P1-5 clips as mechanisms | `SCCargoMovers.cpp`: each candidate clip (animating a bone of the grid's attach chain) is evaluated off-actor. The grid's frame is computed with the rig posed at the clip's first and last frame. A clip that carries the grid less than 1 cm / 1 degree is not its mechanism (`CARGO_MOVER_SKIP`). One mechanism per rig: the clip carrying the grid furthest. The open pose is the combination of the mechanisms' ends (first or last frame each, rigs composed inner to outer) that puts the lattice centre furthest from the owner's centre (`FSCCargoMover::OpenEnds`), and each clip plays towards its open end, forwards or backwards. Ends are chosen together: judged alone, a plate unfolding on its strut can look like it closes (run 21). The Hull A data has no direction signal (no Mannequin fragments), so the rule is the grid's own geometry. | test `SCBridge.Cargo.MoverDirection`; `CARGO_MOVER ... open_end=last` (all 8) |
| P2-6 support failed | A load whose support can no longer arrive (the move bringing it failed or was cancelled) takes another slot (`ReturnSlot`) and a new approach instead of waiting for the step timeout. | `CARGO_RESLOT ... reason=support_gone` |
| P2-7 rotated footprint | A turned item's cells are `CellsFor(turned physical size)` per axis, not the yaw-0 counts swapped. | test `SCBridge.Cargo.PlacementYaw` (1 x 0.5 m cells) |
| P2-8 tractor values | Read at run time from the record the scenario names (`FSCBridgeClient::DataCoreRecord`). `FSCCargoDroneDefinition::ApplyTractorRecord` finds the `SWeaponActionFireTractorBeamParams` objects anywhere in the record (by `__type`, not by path) and takes the one whose `name` is the scenario's `action`. The multitool record has two, `TractorBeam` (aim action) and `Detach` (fire action), so the scenario names one. Stock and drones wait for the lookup. The scenario's copy (with provenance) is the fallback without the archive. `maxVolume` is applied as beams per box, read in SCU (owner ruling, see Drone teams); `volumeForceCoefficient` is logged only. | test `SCBridge.Cargo.TractorRecord`; `CARGO_TRACTOR source=datacore ...` |
| P2-9 synchronous A* | `FSCCargoRouteSearch` runs a search over frames (`Begin` / `Step(budget)`, the same result as `Plan`). Moves share a 2 ms per-frame search budget. A move waits in Planning (Step Waiting, drones claimed, `CARGO_MOVE ... step=Planning`) while its searches run. A load stopped mid-flight holds position while its replan runs, and only finished attempts count. Cells are world-aligned and cached per flying shape within a move (`FSCCargoRouteQuery::CellCache`). | test `SCBridge.Cargo.FlightRoute` (stepped with zero budget, the same route; cache: no cell sampled twice) |

#### Re-run after the review fixes (run 23, `K1bRun23.log`, shots `out/k1/shots23`)

```
CARGO_TRACTOR source=datacore record=EntityClassDefinition.grin_multitool_01_default_tractorbeam action=TractorBeam max_force=257500 min_distance_m=0.50 max_distance_m=150.0 full_strength_m=3.0 max_volume=20.0 ...
CARGO_MOVER rig=EntityClassDefinition.MISC_Hull_A socket=hardpoint_cargoplate_top_left clip=misc_hull_a_cargo_deploy travel_cm=179 open_end=last   (all 8 mechanisms: open_end=last)
CARGO_MOVE job=1 move=1 box=12 step=Planning drones=1                       (every move plans over frames first)
CARGO_TURN job=1 move=1 box=12 blocked="turning 10 deg hits SCVehicle_0/Mesh_145(start inside)"
CARGO_REPLAN job=1 move=1 box=12 reason=turn_room found=1 replans=1         (a hover point further out, turned there)
CARGO_JOB id=1 state=Done t=17.4 moves=1 done=1
CARGO_JOB id=2 state=Done t=46.7 moves=2 done=2
CARGO_JOB id=3 state=Done t=59.5 moves=2 done=2                             (dig-out: hold, carry, put back)
CARGO_JOB id=4 state=Done t=39.1..49.1 moves=2 done=2
CARGO_LOSE_DRONE drone=2 move=8 box=12
CARGO_TEAM job=5 move=8 box=12 lost_drones team=0
CARGO_MOVE job=5 move=8 box=12 kind=Carry step=Failed reason="the drone team is gone"   (no crash; the box is let down)
```

All 30 `CARGO_PATH ... found=1` lines show `hits=0`. Run 21 found a P1-5 regression: judged clip by clip, the plate deploy clip
looked like it closed on two plates. The joint rule (`OpenEnds`) gives `last` for every mechanism, and the `ship-extended`
shot shows the plates out. Run 21 also showed the descent check hitting the plate's own rails, so components the box
overlaps in its own slot are now ignored. And a stale `bRoutePending` kept moves in Planning: PrepareMove and SendHome now
reset it.

### Drone teams, sway, max item size (owner rulings; run 25, `K1bRun25.log`, shots `out/k1/shots25`)

- **Teams by force and volume.** A box's team is `max(ceil(m g / F_beam) with the acceleration reserve, ceil(V_box / maxVolume_beam))`
  (`FSCCargoPlanner::TeamSize`), both terms from data. The box's volume comes from its occupancy volume in its
  `volume_unit` (SStandardCargoUnit; SMicroCargoUnit = 1e-6 SCU). The beam's `maxVolume` is read in the same SCU unit;
  the record gives no unit. Evidence for that reading: with the multitool's TractorBeam action (maxVolume 20) a 16 SCU box
  needs one drone and 24 / 32 SCU boxes need two, which matches the community knowledge that one beam moves 16 SCU and
  32 SCU needs two. Log: `CARGO_TEAM ... scu=32 volume_scu=32.0 mass=30000 byForce=2 byVolume=2 team=2 beam_max_volume=20.0`.
  A box needing more drones than the fleet has is refused with its count ("needs N drones (a beam takes 20 SCU), M available").
  Test: `SCBridge.Cargo.TeamByVolume`.
- **Formation.** Team drones fly over the box, spread along its long side. Each beam attaches to the box top under its
  drone. The loaded acceleration uses the team's summed force (`(N F - m g) / (N m_drone + m)`). The lead holds the soft
  constraint. A drone lost mid-move leaves a team that may still be big enough by force and volume, or fails the move.
- **Fleet.** A second dock with two drones stands on the freight elevator (station equipment, an item on elevator grid 1),
  so the swarm has four drones. The swarm assigns the nearest free drones, so a team can mix ship and hangar drones, and
  each drone returns to its own dock. A dock keeps one cell around it free (`FSCCargoPlacement::KeepClear`): stock packed
  against it blocked the drones' way out (run 24).
- **Sway (gusts).** No data signal names a room's atmosphere, so the scenario says so (`"atmosphere": true`, "the hangar is
  pressurised"). Without the key, a scenario with an environment has air and one without does not; in vacuum there is no
  sway. `FSCCargoSway`: smooth band-limited noise (three sines per axis, random phases per drone) as a position wander and
  a roll/pitch wobble, which the drone's pose follows and eases out of.
  - Scenario parameters `flight.sway_amplitude_cm` (default 4), `sway_tilt_deg` (3) and `sway_frequency_hz` (0.3).
  - Smaller under load (`/ (1 + m_load / (10 N m_drone))`), a little larger at speed (`* (1 + 0.05 v)`).
  - The lead's sway reaches the box through the beam's soft constraint.
  - It fades out over 3 m at the precise ends (lift-off, the turn and descent onto a slot, docking) and is off while
    attaching, lowering, placing and docked.
  - Every swept margin grows by its envelope (`SwayEnvelopeCm`, amplitude x (1 + 0.05 max speed)).
  - Test: `SCBridge.Cargo.Sway` (bounded, smooth, per-drone).
- **Max item size orientation-free.** The authored max permitted item size holds no orientation (records put the long side in
  x, y or z), so a box fits when its sorted sides are within the max's sorted sides. Placement stays upright at yaw 0/90
  inside the interior. The refusal names the side ("is 4.0 m wide; ... take up to 2.0 m wide") or the interior height. Test:
  `SCBridge.Cargo.MaxItemSize` with a z-long max.
- **Catch.** The hanging box is first brought to the turn point at its arriving rotation (the lag taken up), then turned
  there (1.2 s). The turn is checked at the turn point, and a turn point blocked when a load replans is chosen again with
  `SlotApproach`.

Run 25: all five jobs Done (`t=16.8`, `31.7`, `46.2`, `40.9` (the 32 SCU two-drone team, elevator grid 1 to grid 2 over the
Hull A), `42.1`). The lost-drone step fails its move cleanly. All 25 `CARGO_PATH ... found=1` lines show `hits=0`, and
tests are 20/20 (`K1bTests26.log`). Shot `team-2`: both drones over the 32 SCU box, one at each end.

### Regression after C2 / multi-owner (run 26, `K1bRun26.log`, tests `K1bTests30.log`)

The single-ship route `cargo-hull-a.txt` still passes after the route-search, hold and ownership changes. All five jobs are
Done (16 SCU plate to plate, 32 SCU elevator to ship, the dig-out, the 32 SCU two-drone team, 32 SCU back), the lost-drone
step fails its move cleanly, all 34 `CARGO_PATH ... found=1` lines show `hits=0`, and tests are 21/21 (new:
`QueueNoDeadlock`).

### Mixed ownership: three Hull A sharing the cargo lift (`hull_a_multi.json`, `cargo-multi.txt`, `K1bMultiRun1.log`)

- **Vehicles.** Grids are discovered on every vehicle (`USCViewerSubsystem::GetVehicles`, re-diffed on
  `GetVehiclesRevision`). Each instance is its own group (`MISC Hull A · 2`), and the scenario spawns its extra
  instances (`"vehicles": [{"owner": "you"}, {"owner": "b"}, {"owner": "c"}]` through `SpawnVehicleInstance`) before any
  stock is placed.
- **Owners.** Owners are identities: the player ("you") and the others the scenario names. A vehicle's grids are its
  owner's. The hangar's grids and docks are shared. Boxes carry their owner (stock entry `owner`, else the grid's). Drones
  belong to their dock's owner, so a ship's drones are its owner's and the hangar's dock is a shared pool. Views fill K1c's
  `OwnerLabel` / `bMine` (grid, box, job, drone).
- **Rules.** A request (`FSCCargoTransferRequest::Requester`, console `--owner=<id>`) moves only its requester's boxes,
  between grids the requester may use. It refuses the rest with reasons (`9 boxes on the Hangar Freight Elevator Cargo
  Grid Large belong to another owner`, `Grid 2 of MISC Hull A · 2 belongs to Owner B`). It is served by the requester's
  own drones and the shared pool only. The shared pool is served in fair turns: the jobs of the owner served longest ago
  start their moves first.
- **Contention.** Box and cell reservations are exclusive and taken all or nothing when a job is planned. Airspace is
  shared through the right of way across all jobs. A request that only other transfers stand in the way of queues
  (`CARGO_JOB state=Queued reason="Waiting for the cargo lift ..."`), plans again every 2 s, and starts or fails with the
  planner's own reason. A queued job holds nothing, so two jobs never wait on each other. Test:
  `SCBridge.Cargo.QueueNoDeadlock` (job 2 needs the cell job 1 frees; it plans once job 1 is done).
- **Run.** Three transfers submitted within a second (you 32, B 48, C 32 SCU, each from the elevator to their own
  ship) all finished Done (t = 48.6, 71.6, 55.0 s). B's fourth request queued and was answered when B's first finished
  (B's ship full: failed with the planner's reason). Drones served only their owner's jobs, and the shared pool's drone 8
  carried a box of B's. Shots in `out/k1/multishots1`.

### Crusader C2 Hercules: large boxes through the ramp (`c2_hangar.json`, `cargo-c2.txt`, `C2FRun10.log`, shots `out/k1/c2f-shots10`)

Route `cargo-c2.txt`: 96 SCU (the three 32 SCU boxes, 2.5 x 2.5 x 10 m, two-drone teams) from the XL freight elevator into
the C2's large hold, then the other 64 SCU (two 24 SCU, one 16 SCU). Both jobs are Done, every move done:

```
CARGO_TURN_POINT move=1 box=4 found=1 at=V(X=11675.00, Y=-11699.99, Z=398.07) from_lift_m=0.0 tried=1 of 280
CARGO_LOAD job=1 move=1 box=4 found=1 end=found lifted=V(X=11675.00, Y=-11699.99, Z=398.07) lift_m=2.80 goal=V(X=-1925.00, Y=-375.00, Z=829.16) turn=by_lift ... hang_m=0.50 drones=2 spread_m=3.00 size_m=V(X=10.00, Y=2.50, Z=2.50) margin_cm=26
CARGO_PATH job=1 move=1 box=4 leg=load_turned found=1 end=found direct=0 waypoints=165 expansions=8717 length_m=213.2 minClearanceCm=0 hits=0 search_ms=676.7 frames=312 max_frame_ms=3.72
CARGO_PATH job=1 move=2 box=5 leg=load_turned found=1 end=found direct=0 waypoints=164 expansions=6641 length_m=210.2 minClearanceCm=0 hits=0 search_ms=550.0 frames=564 max_frame_ms=3.99
CARGO_PATH job=1 move=3 box=6 leg=load_turned found=1 end=found direct=0 waypoints=10 expansions=2577 length_m=203.0 minClearanceCm=0 hits=0 search_ms=413.0 frames=739 max_frame_ms=3.90
CARGO_JOB id=1 state=Done t=76.0 moves=3 done=3        (3 x 32 SCU)
CARGO_PATH job=2 move=4 box=7 leg=load_turned found=1 end=found ... length_m=206.7 minClearanceCm=0 hits=0 search_ms=161.8 frames=78 max_frame_ms=2.92
CARGO_PATH job=2 move=5 box=8 leg=load_turned found=1 end=found ... length_m=186.3 minClearanceCm=0 hits=0 search_ms=118.0 frames=138 max_frame_ms=2.51
CARGO_PATH job=2 move=6 box=9 leg=load_turned found=1 end=found ... length_m=198.4 minClearanceCm=0 hits=0 search_ms=85.1 frames=182 max_frame_ms=2.43
CARGO_JOB id=2 state=Done t=57.9 moves=3 done=3        (2 x 24 SCU, 16 SCU)
CARGO_LIST GRID id=3 ... used=160 ... blocked=38 boxes=[ 4:32SCU@X=0 Y=0 Z=0 5:32SCU@X=0 Y=2 Z=0 6:32SCU@X=0 Y=4 Z=0 7:24SCU@X=0 Y=6 Z=0 8:24SCU@X=6 Y=6 Z=0 9:16SCU@X=8 Y=0 Z=0 ]
```

All 18 `CARGO_PATH ... found=1` lines of the run show `hits=0` and none is `found=0`. The longest single search step was 4.0 ms
(`max_frame_ms`; the searches share 2 ms of a frame). `minClearanceCm=0` on the load legs: the route ends beside boxes
standing flush (the precise zone, below). The box on its beam (`CARGO_CARRY`): lag up to 84 cm in open flight at
10 m/s, 3-14 cm at 2 m/s in the hold.

**What was wrong** (seven K1b runs ended `found=0 ... expansions=60000`). Measured with new logs:

- The 60,000 expansions were not spent "at the lift room's exit". `CARGO_PATH_FRONT` (what each front of a failed search
  reached) showed the goal, a point outside the hold's lattice, **enclosed**: 62 expansions from it covered all it could
  reach (x 250..1200 cm beyond the hold, 1 m of height). The old one-ended search flooded the hangar looking for a way in.
- `sc.Cargo.Section 3` (the collision as the route queries see it, `CARGO_SECTION`) showed both ends of the cargo deck closed
  by slanted planes although the ramp was open: the C2's **hull colliders** (the root entity's game collision, ramps shut
  whatever their pose). They act only on what is outside the vehicle's physics grid (WP-P3); cargo queries had them in.
- The point "outside the entrance" came from four horizontal rays out of the hold's middle; the first free one led into
  the forward bay, not to the ramp.
- The box was flown crosswise to the hold, as it lay in the lift room, to be turned just outside the hold. Whether a 10 m
  box fits the ramp that way was not measured: it now turns onto the hold's axis first (below).
- `first_hit` on those lines was a point of the straight start-goal line, and `hits=0` / `length_m` meant nothing on a
  failure; the job ended "No box can be lifted" after three route failures while the smaller boxes were still searching.

**What changed** (generic: no asset names, no game values):

- **Search** (`SCCargoRouteSearch.cpp`): two fronts (start and goal) expanded in turn, the route where they meet. A front
  that runs out proves its end enclosed and ends the search at once (`end=start_enclosed|goal_enclosed`; the C2 case: 125
  expansions and 11 ms instead of 60,000 and 20 s); `end=budget` only when both are still open. The start and the goal
  are nodes of their own. Everything is resumable within the frame budget: the straight-line check and the shortcut
  sweeps go 3 m at a time, a node's neighbours one at a time, the final samples one at a time (before: one sweep across
  the hangar and the whole shortcut and smoothing pass in a single frame). Half-cell steps from nodes with a blocked full
  step (`bRefine`) find passages narrower than a cell at every lattice phase; a load's searches use them as the last
  resort (after the full-cell lattice found nothing at either hang), because flooding an enclosed room at half cells
  costs 8 times as much. The hold-entrance waypoints are gone: the goal's front finds the way out of a hold itself.
- **Hull colliders** follow WP-P3 per position (left out only where the shape is in that vehicle's physics grid), with the
  move's own vehicles left out everywhere (cross-review item 2 below, with the evidence); the hull blocks as it stands
  (render geometry in its pose, the interior's and the items' colliders).
- **Turn where there is room**: into a hold a long box turns onto the hold's axis by where it was lifted
  (`EarlyTurnPoint`: the top of the lift, else the nearest point over its grid reached in a straight sweep; box and
  drones swept through the turn) and flies the whole way as it will go in (`leg=load_turned`). The turn outside the hold
  stays as the fallback, over all four lattice sides and outwards, skipping points whose search ends `goal_enclosed`.
- **Precise zone**: inside an interior destination grid (and a lead of the box's half length + 1 m before it) the load
  flies at 2 m/s, the gusts fade out as it slows, and its swept margin is the placing one: 5 cm to the structure, flush
  (2 cm inside its size, as the descent onto a slot) to boxes at rest. Without it a hold 5 m high cannot be filled: a
  box cannot hover 26 cm clear of the neighbour it will stand against.
- **Column** of drones and beams turns with the box (it was a world-aligned box, wider at a diagonal yaw) and is swept
  in level turns.
- **Identity**: a route query names what it tests (shape, rotation, what it ignores, a hash of where the boxes at rest
  are, and since the cross-review a hash of the world: vehicles, rig poses); cell caches and kept searches are keyed by it
  (before: `load:<hang>`; a cell blocked by a box that has left
  stayed blocked, and a failed search answered every retry).
- **Reasons**: `CARGO_PATH ... end=`, `CARGO_PATH_BLOCKED` (the ends, what the straight line hits, box or drones),
  `CARGO_PATH_FRONT` per front, `CARGO_LOAD` (lifted pose, turn, hang, team). A failed pick says why ("... (nothing the
  load fits through leads to where it has to go)"); the job's text is "No box can be lifted" only for lift failures,
  else "No box can be carried to its place" / "moved". Moves still searching are not dropped when the pick limit is
  reached; a box whose routes were not found is not offered again as a substitute.
- A lift's plan ignores loads in the air (a neighbour turning at the top of its lift made the next box's lift fail; the
  right of way already holds it on its grid meanwhile).

Tests (`SCBridge.Cargo`, 23 of 23, `C2Tests4.log`), new: `FlightNarrow` (a 60 cm window in a wall at ten phases of the
1 m lattice: found every time with half steps, missed at some phases without), `FlightEnclosed` (a goal / start in a
closed room is reported enclosed after searching the room only; a door is found both ways; the cap reports `budget`;
stepped with no budget, the same route).

Regressions (the binary before the last change, see below): `cargo-hull-a.txt` (`C2FHullA3.log`): jobs 1-5 Done with every move done (t = 18.0, 29.7, 47.6, 50.2,
49.2 s; run 26: 18.5, 36.2, 40.2, 34.5, 44.2), the lost-drone move fails cleanly, 24 found routes with `hits=0`, longest
search step 2.4 ms. Moves into the lift room are slower (the precise zone). `cargo-multi.txt` (`C2FMulti1.log`): jobs 1-3
Done (50.1, 83.1, 63.0 s; before 48.6, 71.6, 55.0), job 4 queued, then answered with the planner's reason; 19 found routes with `hits=0`. Both
ran before the last change (a node's neighbours tried one at a time), which does not change what a search finds
(`FlightRoute` / `FlightEnclosed`: stepped with no budget, the same route).

Shots checked (`out/k1/c2f-shots10`): `c2-team-2` (a 32 SCU box under its two drones on its way round the ship),
`c2-bay-1..3` (the 32 SCU boxes standing lengthwise in the hold, flush), `c2-more-bay` (the 24 SCU boxes beside them),
`c2-loaded`.

#### Cross-review fixes (Codex Terra, `out/k1/review-k1c.md` items 1, 2, 3, 7; `C2FRun13.log`, `C2FHullA5.log`, `C2FMulti3.log`)

| Finding | Fix | Evidence |
|---|---|---|
| 1 search state survives changes of the collision world | A query's identity now also carries a hash of the world apart from the cargo (`WorldHash`, per frame: the vehicles revision, each vehicle's and the environment's transform, every rig's `LocalPose` and collision refits, so a ramp, door or plate moved by anyone) and of who the query leaves out (actors and components, not their count). A move drops what it keeps between ticks (searches under way or answered, sampled cells, turn points) when that hash or the stock at rest changes (`RefreshMove`, `CARGO_WORLD_CHANGED`). | `CARGO_WORLD_CHANGED move=2 box=5 searches_dropped=1 ...` (`C2FRun11.log`: a search under way restarted when a neighbour was lifted) |
| 2 hull colliders ignored everywhere | Per position (`FSCCargoHullRule`): a vehicle's hull colliders are left out only where the shape is in that vehicle's physics grid (`IsInPhysicsGrid(Bottom, Top)` over the shape's height, at its middle and the ends of its long side; a sweep: either end), and block elsewhere. Exception, with its evidence below: the vehicles a move loads or unloads (its grids', its drones' docks') are left out for that move everywhere. Applied to the route queries and to the lift, slot and hold-cell queries. | below |
| 3 clearance measured outside the budget | The search measures its own route as its last phase, a sample a step (`FSCCargoClearance`, the same arithmetic as `MinClearance`); `search_ms`, `frames` and `max_frame_ms` include it. | test `FlightClearance`; C2: `max_frame_ms` 3.82 at most, clearance included |
| 7 precise zone asymmetric at its boundary | A sweep crossing the zone's boundary is split there (`SCCargoSegmentInside`): the wide margin up to the boundary, the precise one inside, whichever way it is flown. | test `ZoneSplit` |

**Item 2, the evidence asked for.** With the rule by position alone (`C2FRun11.log`) the C2 transfer broke: 31 routes found, 45 not
(`load_turned end=smoothing` 13 times, `home end=start_enclosed` for a drone docked in the small hold), job 1 `Partly done` (1 of 3), job 2
`Failed`. `sc.Cargo.Section 3` (':' = free and inside the C2's physics grid, '#' = the probe overlaps something; the section runs
lengthwise through the hold, the rear ramp to the left):

```
z_m=   5.00 |........###.::::::::+++++++++++++++++++::::::::::::::::::::|
z_m=   4.00 |..........##..::::::+++++++++++++++++++::::::::::::::::::::|
z_m=   3.00 |...........###.:::::+++++++++++++++++++::::::::::::::::::::|
z_m=   2.00 |.............##.::::+++++++++++++++++++::::::::::::::::::::|
z_m=   1.00 |..............##.:::+++++++++++++++++++::::::::::::::::::::|
```

The slanted '#' line is the hull's ramp, shut (the ramp is open: without the hull colliders these rows are free, `C2FRun5.log`). It
lies outside the grid's volume, 1 to 2 m from it in this section (1 m steps). So a shape gets through only if it reaches from outside the
plane into the grid: a 10 m box did when a lattice step happened to straddle it, and then failed the 50 cm sample sweeps
(`end=smoothing`); the drones and the 5 m boxes did not. The grid's volume does not cover the approach to the open ramp, and how the
game admits an entity there is not known here. Hence the exception for the move's own vehicles; every other vehicle's hull
blocks by position (in `cargo-multi.txt` each owner's loads and drones are routed with the other two ships' hulls in).

Runs with the final binary (afterwards only a file split, `SCCargoWorldState.cpp`, rebuilt and tested 25 of 25, `C2Tests8.log`): tests 25 of 25 (`C2Tests7.log`). `cargo-c2.txt` (`C2FRun13.log`): jobs 1 and 2 Done (75.4 s, 58.2 s, 3 of 3
moves each, 160 SCU in the hold), 18 routes found with `hits=0`, none not found, longest search step 3.82 ms (clearance included).
`cargo-hull-a.txt` (`C2FHullA5.log`): jobs 1-5 Done with every move done (18.5, 29.7, 45.1, 50.3, 49.2 s), the lost-drone job fails as before
("the drone team is gone"), 25 routes found with `hits=0`, 5 not found (the full-hang attempts out of and into the lift room, then reeled
in), longest search step 2.33 ms.
`cargo-multi.txt` (`C2FMulti3.log`): jobs 1-3 Done (53.4, 77.7, 66.2 s), job 4 queued, then answered with the planner's reason; 17
routes found with `hits=0`, 5 not found (full-hang attempts), longest search step 3.06 ms; 72 `CARGO_WORLD_CHANGED` (searches under way
restarted when a box of another transfer was lifted or set down).

### Known issues (TODO(WP-K1))

- Hull A cargo clips have no Mannequin fragments (the catalogue's source is Chrparams), so which end of a clip is
  "extended" is geometric (P1-5 above). If K1a exports the ADB fragments / interactions (`StrutTransformExtend` /
  `Retract`), they could name it from data. The hull clip is shared by all four plates, so retracting needs every carried
  grid empty.
- The elevator grids are on static containers, not the platform bone. A moving platform would need the grid parented
  to its rig.
- A load hanging from its beam lags its flight point by up to about 0.5 m. The swept shapes carry a 20 cm margin, and the
  turn and descent are checked from where the box actually is.
- A failed move leaves its box where the drones let go (Stored, off-grid). The planner does not yet steer around it or
  pick a different destination slot when the planned one is obstructed physically.
- The 2 SCU box held aside during a dig-out goes back to the slot the planner recomputes once the box beneath is gone
  (lowest fit), not necessarily its old cell.
- Tractor beam: `volumeForceCoefficient` and the cargo-mode override of the record are read but not applied (semantics not
  established). `maxVolume` is applied as beams per box (see Drone teams).
- The dock blocks retracting its plate (simplest rule). The mobiGlas (F1) holds the camera while open, so the route
  opens it near the end.
- Cooked (Web) hangars have no rigs: there the environment grids are always open and nothing animates.
- C2 / large boxes (Fable-C2):
  - Hull colliders of the vehicles a move loads or unloads are left out for it everywhere, not by position: the grid's
    volume ends 1 to 2 m short of the hull's shut ramp (cross-review item 2). How the game admits an entity to a ship's
    grid at an open ramp is the missing rule; with it the exception can go.
  - In the precise zone the box still hangs from the physics handle while it passes boxes at rest with a 2 cm inset; a
    kinematic carry there (as the lift and the descent are) would make that exact. In open flight a 30 t box lags its
    flight point by up to 84 cm (the swept margin is 26 cm; the guard sweeps the stretch ahead every frame).
  - The planner fills a hold from its lattice origin. Here the boxes placed later still reached their slots (the 16 SCU
    box behind a 32 SCU box: `found=1`; by which way was not checked); a hold with one entrance needs its far end filled
    first.
  - One search step took up to 4.0 ms against the 2 ms budget (one neighbour of a node is up to eight overlap / sweep
    queries of a 10 m shape against complex collision; it is the smallest resumable unit).
  - Half-cell steps are the last resort of a load's searches (an enclosed room costs 8 times as much to flood); the turn
    outside a hold (the fallback of the turn by the lift) is not exercised by a route; the lattice axis boxes are stowed
    along still comes from the 2 m probe (`EntranceAxis`).
  - `SCCargoMoves.cpp` is 509 lines (the precise zone's speed limit and gust fade were added to its transit step).
- The access rule counts corridor gain in all access corridors of a grid, not only the segments drones use (entrance to
  slot).

## K1f: extra vehicle instances (several ships in one hangar) (Win64, UE 5.9, RTX, 2026-11-14)

Generic viewer capability for mixed-ownership scenarios (`Public/SCVehicleInstances.h`, `Private/SCVehicleInstances.cpp`;
hooks in `SCViewerSubsystem`, `SCImportJob`, `SCImporter.h`, `SCImportMaterials.h`). Route
`ue/scripts/routes/vehicle-instances-hull-a.txt` (K1fRun2.log, shots `out/k1/k1f-shots2`) and
`vehicle-instances-replace.txt` (K1fRun3.log, `out/k1/k1f-shots3`). Host:

```powershell
UnrealEditor.exe <abs>\ue\SCViewer\SCViewer.uproject -game -windowed -resx=1600 -resy=900 -SCLoad=MISC_Hull_A -SCExtraVehicles=*2 `
  -SCEnvironment=Data/ObjectContainers/PU/loc/mod/common/hangar/hightech_a/hangar_lrgtop_001.socpak `
  -SCScript=<abs>\ue\scripts\routes\vehicle-instances-hull-a.txt -SCShotDir=<abs>\out\k1\k1f-shots2
```

**Sharing.** The loaded vehicle's import is retained (`FSCImportOptions::bRetainAssets`, `sc.Vehicle.ShareAssets 1`):
its meshes, rigs (and their collision source), clip catalogue, textures and material instances move to the import's
session package (`/Temp/SC/<guid>/Shared`) and an `FSCImportedAssets` set holds them; the package bytes are released.
Every further instance of the entity is an assembly from that set (`FSCImportJob(Assets, Options)`): its own actor,
components, rigs (pose, CPU-skinned collision), lights, paint slots and exploration data, the same mesh/texture/MID objects.

```
IMPORT_ASSETS_RETAINED session=/Temp/SC/1DB2... objects=4944 meshes=821 rigs=21 textures=600 materials=3502 detached=4178
VIEWER_LOAD result=Succeeded entity=MISC_Hull_A export_s=1.464 export_cache_hit=1 import_s=13.668 assemble_s=5.205 total_s=15.618
VEHICLE_IMPORT idx=1 shared=1 import_s=5.53 derive_s=0.00 build_s=0.00 materials_s=0.00 assemble_s=5.15 warnings=1
VEHICLE_SPAWN idx=1 entity=MISC_Hull_A ok=1 at=0.25,-16.74,3.63 yaw=0 auto=1 shared=1 total_s=5.76 rigs=28 lights=128 meshes=821 spawns=10 doors=5
VEHICLE_SPAWN idx=2 entity=MISC_Hull_A ok=1 at=0.25,16.77,3.63 yaw=0 auto=1 shared=1 total_s=5.96
VEHICLE_SPAWN idx=3 entity=MISC_Hull_A ok=1 at=0.00,40.00,3.63 yaw=180 auto=0 shared=1 total_s=6.04      (sc.Vehicle.SpawnAt)
VEHICLE_SPAWN idx=4 entity=MISC_Hull_A ok=1 ... auto=1 shared=0 export_s=1.29 import_s=16.43 total_s=18.06  (ShareAssets 0: fresh import)
```

Second and third instance: 5.8 / 6.0 s against 15.6 s for the loaded one and 18.1 s for a fresh import; what is left is
the assembly itself (one game-thread step, of which material binding ~2.2 s). No new texture or MID is created
(`textures=0 material_instances=0` on the assemblies' `IMPORT_OK`).

**Placement** (`FSCVehiclePlacement`): envelope = solid bounds in the bind pose and at the last frame of every bound
clip (Hull A 32.2 x 10.3 x 7.2 m, the extended hull included, 0.05 s, cached per entity); candidates on a grid
(1/8 of the footprint's short side), nearest the loaded vehicle's footprint centre first, yaw of the loaded vehicle
first; a spot needs flat floor within 30 cm of the loaded vehicle's floor under 9 footprint points, no Visibility-channel
geometry in the envelope + 3 m walkway (from 50 cm above the floor), and no overlap with keep-out boxes: the other
vehicles' envelopes and the environment's inventory-container access lanes (lattice + its largest permitted item edge
on every horizontal side; 4 in this hangar). `VEHICLE_PLACE idx=1 found=1 tested=531 seconds=0.00 lanes=4 keep_out=5`.
Gear rest: `USCImportLibrary::RestOnFloor` (`VIEWER_REST gear_clips=2`). Environment change: `VEHICLE_REPLACE
environment=SCVehicle_4 placed=2` (run 3, same spots in the re-imported hangar).

**Independent animation.** `sc.Vehicle.Clip 1 cargo_deploy` (`misc_hull_a_cargo_deploy`, 544 frames) extends instance 1's
hull while 0 and 2 stay retracted (`plates-instance1-extended*.png`), `-1` plays it back (`plates-instance1-retracted.png`).

**Cleanup.**

```
VEHICLE_DESTROY idx=2 tracked=1724 alive_after_gc=0          (shared instance: its own objects)
VEHICLE_DESTROY idx=4 tracked=18419 alive_after_gc=0         (fresh import: actor + its whole session)
VEHICLE_RESET instances=2 asset_sets=1 tracked_objects=3448
VIEWER_UNLOAD tracked=21872 alive_after_gc=0 live_static_meshes=1010 live_textures=4032 live_mids=1529   (= the hangar's)
```

Shots checked: `instances-all`, `instances-front`, `instances-top` (three Hull A abreast on the ship elevator, walkways
between, nothing intersecting), `plates-*`, `after-destroy`, `explicit-and-fresh` (idx 3 at y=+40 m yaw 180).

### Known issues (TODO(WP-K1))

- An assembly is one game-thread step (~5 s hitch for a Hull A); split `Assemble` into budgeted steps.
- Cooked ships (web build) have no retained import: extra instances of a cooked ship need another level instance.
- First person, orbit focus/isolate, paint and the hierarchy act on the loaded vehicle (id 0) only; engine-glow MIDs
  are shared, so `SetEngineGlow` on one instance changes all of its entity.
- Explicit transforms (`SpawnAt`) are not checked for overlaps; auto-placed instances standing on a moving floor (the
  ship elevator in another state) are not re-placed when it moves.

## K1c: mobiGlas Cargo app and world-space callouts (Win64, UE 5.9, RTX, 2026-11-15)

Code: `ue/Plugins/SCBridge/Source/SCBridge/Private/UI/Cargo/` (tokens `SCMobiGlasStyle`, model `SCMobiGlasModel`, shell
`SSCMobiGlas`, panels `SSCMobiGlasGrids` / `SSCMobiGlasTransfer`, widgets `SSCMobiGlasWidgets`, live-view orbit
`SCMobiGlasOrbit`, callouts `SSCCalloutLayer` + pure `FSCCalloutSolver`, view source / console `SCCargoViewSource`,
offline layout data `SCCargoPreviewSource`); integration in `SSCViewerRoot`, `SSCViewportInput`,
`ASCFirstPersonCharacter` (F1 = `ESCFirstPersonAction::MobiGlas`, `bMobiGlasOpen`).

Use: **F1** opens / closes the mobiGlas in any camera mode, **Esc** closes it. While open the cursor is free and the
panels take clicks; **drag** (left / right; Shift or middle = pan) and the **wheel** on the live view (top band) orbit
and zoom a camera around the selected grid's ship or platform, which re-frames when another group is selected or a
transfer starts (its work area: source, destination, boxes in the air, drones). The first-person player stands still
and gets its own view back on close; an outside view change while open (teleport, `sc.Cargo.View`) re-targets the
orbit. Callouts (boxes of active transfers, drones away from their dock; with the mobiGlas open also the nearby
grids) are always on, showcase included. Route stand-ins: `sc.MobiGlas [0|1]`, `sc.MobiGlas.Select|From|To <id | ship |
elevator | label text>`, `sc.MobiGlas.Layer <n>`, `sc.MobiGlas.Amount <scu>`, `sc.MobiGlas.Confirm`,
`sc.MobiGlas.CancelJob <id>`, `sc.MobiGlas.Extend 1|0`, `sc.MobiGlas.Orbit <dx> <dy>`, `sc.MobiGlas.Zoom <n>`;
`sc.UI.Shot <tag>` saves the window with the Slate UI to `-SCShotDir` (route `shot` lines capture the bare viewport);
`sc.MobiGlas.Preview 1` feeds synthetic UI-preview views (badge "UI PREVIEW DATA · NOT GAME DATA") for offline layout
work only.

### Automation tests

`UnrealEditor-Cmd.exe <uproject> -ExecCmds="Automation RunTests SCBridge.UI.Callouts; Quit" -unattended -nullrhi`:
9 / 9 `Result={Success}` — Budget, Crowded, Hysteresis, LeaderShape, Motion, NoCover, NoCrossing, NoOverlap, Stacking (labels
never overlap or cover an object, leaders are 0° / 45° / 90° with a 24–64 px run and a ≥ 16 px shelf and never cross a
line or a label, previous sides are kept, at most 8 labels by priority, stacks follow anchor order; a cluster too dense
for the rules leaves dots; a 60-frame pan re-solved every frame with the previous sides keeps every rule without side
flips). The layer re-solves every frame and paints only solved placements (review item 8).

### Runs (live data, scenario `hull_a_hangar`, route `ue/scripts/routes/mobiglas-live.txt`, shots `out/k1c/live*`)

Host as K1b's with `-SCShowcase -SCScript=<abs>\ue\scripts\routes\mobiglas-live.txt -SCShotDir=<abs>\out\k1c\live14`.

- `live11/live-open.png`, `live14/live-open.png`: the ship framed in the live band from the player's side, four grid
  callouts stacked without crossing, Nearby grids with humanized labels ("Grid 1 · bottom left", "Hangar Freight
  Elevator · Large"), Grid detail lattice filling its panel, EXTENDED / RETRACTED chips, Retract disabled with the
  reason ("Grid holds 16 SCU; unload it before retracting").
- `live9/live-plan32.png` (and later runs): 32 and 24 SCU boxes refused with their reasons ("32 SCU box is 10.0 m long;
  MISC Hull A grids take up to 5.0 m"), stops 4 · 8 · 12 · 16 · 20 · 24 SCU, repeated moves summarized ("2 × 8 SCU →
  MISC Hull A · Grid 2"), "MISC Hull A will extend before loading", Confirm.
- `live9/live-flight-2.png`, `live9/live-flight-3.png`, `live14/live-flight-3.png`: during the transfer, ship (and in
  live14 the elevator) in the band with drone and grid callouts, Transfers row MOVING with progress, ETA and per-drone
  states (TO PICKUP / ATTACHING / CARRYING).
- `live1/live-callouts-closed.png` (and later): mobiGlas closed, box callouts "ORGANIC · 16 SCU / IN TRANSIT", "LIFTING"
  and drone callouts "DRONE 1 · CARRYING" anchored on the boxes and drones, labels clear of them.
- `live*/live-retract-refused.png`: Retract on the loaded ship refused inline with the subsystem's reason;
  `live*/live-720.png`: the same open view at 1280×720 (Slate DPI 0.67), panels and type scale down, still legible.

Multi-owner (`hull_a_multi`, route `ue/scripts/routes/mobiglas-multi.txt`, shots `out/k1c/multi*`): other owners'
transfers through the shared elevator under OTHER TRAFFIC with their state (OPENING GRIDS), their boxes on the elevator
hatched and paper-dim in the lattice and listed with their owner, ships of one type numbered ("MISC Hull A · 2"),
drones and boxes of other owners labelled with their owner ("DRONE 4 · OWNER B", "DRONE 7 · SHARED") in the neutral
style (no accent edge, below our own in priority).

Layout work on synthetic data (no badge on live shots): `out/k1c/shots1`–`shots3` (`mobiglas-preview.txt`).

### Known issues (TODO(WP-K1))

- TODO(WP-K1): the live-view orbit is placed by a spring-arm sweep on the visibility and pawn channels; the hangar shell
  has no such collision, so the boom is held by a known-good viewpoint (the player's eye / current camera) and a 45 m
  cap instead. A drag during the mobiGlas can still swing the camera through a wall (no per-frame pull-in).
- TODO(WP-K1): `ASCViewerPawn`'s own environment pull-in (one actor for the whole hangar) is turned off on the orbit
  pawn while the mobiGlas is open; it pulled the camera into the hull. Fix the pull-in per component in the pawn.
- TODO(WP-K1): a wide work area (ship and elevator 70 m apart) is framed across the source → destination line from the
  side nearer the hangar's origin; the ship may sit at the band's edge. Projected-bounds fitting would be exact.
- TODO(WP-K1): Nearby grids (80 m) are measured from the player; a player who walked to the elevator does not see the
  ship's grids (and vice versa). A "show all" toggle would help.
- TODO(WP-K1): callout occlusion (dashed line, hide after 0.5 s hidden) and the "dot only below 20 px" rule are not
  implemented; labels show for targets behind geometry.
- TODO(WP-K1): the commodity list is aggregated in the UI from the grid views (own stored boxes only); the subsystem's
  `GetCommodities` is no longer used by the panel.

#### Review fixes (Codex review `out/k1/review-k1c.md`, items 4, 5, 8, 9; run `out/k1c/live15`, `out/k1c/multi3`)

- 4: opening gives the mobiGlas keyboard focus (Esc / F1 close in orbit and fly too; the view also closes it on Esc);
  focus returns to the view on close.
- 5: one open state — it follows the first-person character while there is one and closes when the character goes:
  `sc.MobiGlas 1`, F1 (key table), `sc.Camera orbit` logs `MOBIGLAS_STATE open=0 first_person=1` then
  `MOBIGLAS_STATE open=0 first_person=0` (`live15/live-after-unload.png`).
- 8: callouts are solved every frame; no interpolated positions (test `SCBridge.UI.Callouts.Motion`).
- 9: per frame only drones and boxes in the air are fetched; grids and jobs on a revision change, while open also every
  0.25 s, closed every 2 s.

## Final verification (orchestrator, 2026-11-15, Win64 RTX, final tree)

Script: one locked run — `Build.bat SCViewerEditor Win64 Development`, `Automation RunTests SCBridge.Cargo` and
`SCBridge.UI.Callouts` (`-nullrhi`), then the four routes in `-game` (logs `Saved/Logs/Final_*.log`, shots
`out/k1/final/shots-*`).

```
BUILD rc=0
TESTS SCBridge.Cargo success=25 fail=0
TESTS SCBridge.UI.Callouts success=9 fail=0
ROUTE hull_a   script_done=True crash=False paths_found=23 not_found=5 found_with_hits=0 jobs 1-5 Done, 6 Failed (lost-drone step, by design)
ROUTE multi    script_done=True crash=False paths_found=17 not_found=5 found_with_hits=0 jobs 1-3 Done, 4 Failed ("No room on MISC Hull A · 2 for any Organic box", by design)
ROUTE c2       script_done=True crash=False paths_found=18 not_found=0 found_with_hits=0 jobs 1-2 Done (96 + 64 SCU into the C2 holds), max_frame_ms=3.94
ROUTE mobiglas script_done=True crash=False paths_found=12 not_found=2 found_with_hits=0 job 1 Done (ordered through the mobiGlas)
```

The not-found searches are first attempts leaving or entering the lift room at full beam hang (`end=start_enclosed` /
`goal_enclosed`, 0.3–90 ms each) that then reel in or turn by the lift and succeed. Cross-reviews: K1a (Codex Terra,
`out/k1/review-k1a.md`), K1b/K1e (`review-k1b.md`), K1c + C2 routing (`review-k1c.md`) — all findings fixed except the
accepted move-scoped hull-collider exception (C2 section) and the known issues listed above.
