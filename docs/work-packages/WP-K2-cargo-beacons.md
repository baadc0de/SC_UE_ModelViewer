# WP-K2 — Freestyle cargo beacons (loose cargo)

## Why

WP-H1's exploratory run on `hull_a_hangar` lost a box: a carry whose route closed mid-flight failed, the box was let
down under gravity and stayed loose, and nothing could ever pick it up again (a pick needed a grid). Loose cargo is a
first-class part of this vertical slice, not an error path: boxes on the hangar floor, dropped or put there, are
cargo like any other, and later work builds on them.

## Behaviour

1. **A carry that fails in the air sets its box down on the nearest free slot it can still reach.** Candidate slots:
   every open grid the box's owner may use, nearest first (the one it was going to included); a candidate whose route
   cannot be found is skipped for the next. Failures that leave no team able to hold the box (drones lost) skip this.
2. **Else the box is let down loose**, under gravity, where it is. It keeps its owner and the location it was carried
   *from* (its origin).
3. **Loose boxes are always pickable**: from where they lie, at any rotation, lifted straight up (world up) and righted
   to level at the top of the lift, then carried like any other box.
4. **Beacons.** A loose box at rest belongs to a *beacon*: a virtual cargo location (no physical representation) with a
   random name ("Loose cargo Kestrel-42"), the origin location's id and label, an owner, and its member boxes. A box
   that comes to rest near boxes of a beacon with the same origin and owner joins it (near: within the larger of the
   two boxes' longest sides); otherwise it founds one. A beacon with no boxes left is gone. A beacon's members are
   arbitrary in pose and number: it has no lattice, layers or capacity.
5. **Transfers from beacons, not to them.** A beacon is a transfer source like a grid (by id or name in selectors, the
   `beacon` / `loose` keyword for all of them); as a destination it is refused ("loose cargo is a source only").
6. **mobiGlas**: beacons appear in the transfer source picker and the nearby list with their counts (boxes and SCU per
   commodity), never with geometry.
7. **Support lost**: loose boxes resting on a vehicle that goes away fall and settle again (their beacon is recomputed).

## Architecture: cargo locations

Grids and beacons are both **cargo locations**, ids from one counter (a beacon id never equals a grid id), so every
place that names a source or destination by id (`FSCCargoTransferRequest`, `FSCCargoMovePlan`, jobs, the RPC, the UI)
takes either. Each kind says what it can do:

| | source | destination | lattice | access (extend / retract) |
|---|---|---|---|---|
| grid | yes | yes | yes | if it has a mechanism |
| beacon | yes | **not yet** (`bAcceptsDrops = false`) | no | no |

A future "set down at a beacon" (beacon as destination) needs only a placement rule for free-form drops (where in the
clump a box goes) and turning the flag on; selectors, ownership, planner input, views and the RPC already carry
beacons. The planner sees beacon members as free items (never covered: nothing to dig out), so a beacon never takes
part in dig-out, relocation or holds.

## Data

- `ESCCargoBoxState::Loose`: off every grid, in no job. `FSCCargoBox::BeaconId` (INDEX_NONE while it falls or
  settles), `OriginId`. At rest (speed under 1 cm/s and 1°/s for half a second) the body is frozen where it lies.
- `FSCCargoBeacon` / `FSCCargoBeaconView`: id, name, origin id and label, owner, boxes, centre and bounds, commodities.
- `USCCargoSubsystem::GetBeacons()`; `FSCCargoPlanInput::Beacons` (beacon id → item ids).
- State log / RPC: `cargo.beacons[]`, box `state: Loose`, `beacon`, `origin`; activity `cargo_boxes_settling`;
  anomaly `box_unbeaconed` (loose and at rest without a beacon). `/cargo/spawn` with `at` or `random` puts boxes loose
  (random: a free, reachable spot on the floor, lift corridor clear).

## Tests

- Planner (automation): a beacon as source (members chosen by commodity and size, no dig), a beacon refused as
  destination, a grid and a beacon together.
- Beacons (automation): clustering by origin, owner and distance; naming; removal of the last member.
- Retarget choice (automation): nearest usable grid with a free slot first, the planned one included, tried ones skipped.
- Exploratory (RPC): loose boxes spawned at random reachable spots, a drone lost under a load in the air (the box set
  down on a slot or loose), transfers from beacons, ship despawn under loose boxes; every loose box at rest has a
  beacon at the end and every transfer from a beacon finishes or is refused with a reason.

## Verification (2026-11-16, LIVE 4.10.2, Win64 Development, off screen through the WP-H1 harness)

- `Automation RunTests SCBridge.Cargo`: 44 / 44 Success (new: `BeaconJoin`, `BeaconName`, `BeaconRetargetOrder`,
  `BeaconSource`).
- C2 scenario, scripted:
  - Three boxes put down at random reachable spots (found in 2–24 tries) each came to rest and founded a beacon.
  - A beacon as destination was refused ("… is loose cargo: a source only, choose a grid as destination").
  - A transfer from a beacon finished in 41 s, and the beacon was gone with its last box.
  - A drone lost under a load in flight: the box was let down and founded a beacon "from Hangar Freight Elevator ·
    Grid 1".
  - A box put down beside another of the same origin joined its beacon (two boxes, 24 SCU).
  - The mobiGlas shows the beacon in the nearby list with its counts, its detail as name, origin and the contents
    table (no lattice), and offers it as a source only.
- `explore.ps1 -Scenario hull_a_hangar -Steps 50 -Seed 5 -Video`:
  - failures=[], anomalies=[], crash=false, overlaps=0.
  - 10 loose spawns; 7 transfers from beacons (4 done, 3 refused for room); every job done.
  - At the end, 14 loose boxes in 9 beacons, all consistent.
- `explore.ps1 -Scenario hull_a_hangar -Steps 40 -Seed 6 -HitInFlight 0.6 -Video`:
  - failures=[], anomalies=[], crash=false, overlaps=0.
  - 7 loads interrupted in flight, all 7 re-targeted to the nearest other open grid with a free slot (two after one
    without room). Their jobs finished; one finished partly done because another of its boxes could not be lifted.
  - One drone lost: its box was let down, founded a beacon and was later carried from it.
  - Beacons founded and emptied as their boxes were taken.

Known limits: re-targeting happens in transit only (a failure while lifting, lowering or placing lets the box down);
a re-targeted carry counts as done for its job (the job's summary keeps the destination it was planned for); a beacon's
members are never covered, so loose boxes resting on one another are lifted in any order.
