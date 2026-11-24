# WP-K1 — Cargo by drones (vertical slice)

**Agents:** Codex `gpt-6.1-terra` (K1a exporter), Opus 5.5 (K1b cargo domain + drones, K1c mobiGlas UI),
Codex `gpt-6-astra` (K1d stand-in art, UI mock). Cross-review: Codex ⇄ Opus.
**Depends on:** WP-U1, WP-U3 (clips), WP-P1/P2 (first person), the showcase-player work on `wp/showcase-player`.
**Branch:** `wp/k1-cargo-drones`; submodule branch `scviewer/inventory-containers`.

## Goal

A first-person player in a hangar with a **MISC Hull A** opens the **mobiGlas** (F1), sees the cargo grids around
them — the ship's four external grids and the hangar **freight elevator** — with their contents and free space, and
orders a transfer ("move 32 SCU of X from the elevator to the Hull A"). Small **cargo drones**, stored on the ship
when idle, fly out, lift the containers physically and lock them onto the receiving grid. The UI knows the receiving
grid's geometry: a container that does not fit (a 32 SCU box is 10 m long; a Hull A grid is 5 m) is refused with the
reason before anything moves. Transfers in progress are tracked in the mobiGlas and in world-space callouts.

This pivots the game's two loading modes — opaque instant loading while the ship is stowed, and hand loading with a
tractor beam — into an ordered, visible, physical process. Research: `docs/cargo/research-star-citizen.md`,
`docs/cargo/research-starfield-ui.md`; game data: `docs/cargo/datamine.md`.

## Facts from game data (4.10.2, `docs/cargo/datamine.md`)

- Ship grids are loadout items with `SCItemInventoryContainerComponentParams.containerParams` → an
  `InventoryContainer` record: `interiorDimensions`, `inventoryType` (`InventoryOpenContainerType`: `gridCellSize`
  1 cm, `minPermittedItemSize`, `maxPermittedItemSize`, `isExternalContainer`, `gridPosOffset`), excluded subtypes.
- Hull A: four `MISC_Hull_A_CargoPlate` items (ports `hardpoint_cargoplate_{top,bot}_{left,right}`, rig with
  `hardpoint_cargo_grid` under `BONE_cargoplate_extender`, clips deploy / deployed / retracted) each carry a
  geometry-less `MISC_Hull_A_CargoGrid` at `Hardpoint_Cargo`: 2.5 × 5 × 2.5 m, max item 2.5 × 5 × 2.5 → 16 SCU,
  64 SCU in total.
- Freight elevators are socpak entities (`Hangar_FreightElevator_CargoGrid_{Small,Medium,Large,XL}`, linked by a
  `LoadingPlatformManager`): Small 7.5 × 6.25 × 5 m (max item 16 SCU), Medium/Large/XL max item 2.5 × 10 × 2.5 (32 SCU).
- Containers: `SAttachableComponentParams.AttachDef.inventoryOccupancyDimensions` / `inventoryOccupancyVolume`,
  `SStandardCargoUnit.standardCargoUnits`, mass in `SEntityPhysicsControllerParams.PhysType.Mass`, geometry
  `cargo_multipurpose_1_*.cgf`. 1 SCU = 1.25 m cube (box data). Sizes 1 / 2 / 4 / 8 / 16 / 24 / 32 SCU.
- Tractor beams: `fireActions[].maxForce / maxDistance / fullStrengthDistance / maxVolume` (multitool and ship S1).

## K1a — Exporter: inventory containers (StarBreaker, generic)

Submodule branch `scviewer/inventory-containers` from the pinned submodule commit. Tests first. Nothing in this part
knows about drones, Hull A or this viewer: any consumer (Blender addon, other tools) can use it.

1. **Vehicle `children[]` get `port_path`**: the item-port names from the root entity to this item, e.g.
   `["hardpoint_cargoplate_bot_left"]`; a nested item `["hardpoint_x", "port_y"]`. It disambiguates siblings that
   share an `entity_name` (four identical plates). Additive; document it in `docs/decomposed-export-contract.md`.
2. **`inventory_containers[]` on the vehicle scene** (top level): one entry per loadout node whose entity has
   `SCItemInventoryContainerComponentParams`, **including geometry-less items** (the grid itself is one):
   `entity_name`, `entity_class_guid`, `port_path`, `parent_port_path`, `parent_node_name` (the helper/bone on the
   parent geometry the port resolves to, the same rule as `children[].parent_node_name`), `local_transform_sc`
   (relative to that helper, same convention as children), and `container` (below).
3. **`interiors[].inventory_containers[]`** for socpak entities whose class has the component (found by component,
   like `proximity_sensors`): `entity_id`, `entity_class`, `entity_class_guid`, `transform` (container-local, the frame
   of `placements[]`), `links[]`, `container`. Also export the entities that link to them by their component
   (`SCLoadingPlatformManagerParams` references) only if it is cheap and generic; otherwise leave a TODO.
4. **`container` object** (resolved `InventoryContainer` record): `record`, `record_guid`, `interior_dimensions`
   [x,y,z] m, `inventory_type` (the concrete type name), `grid_cell_size` m (as authored), `grid_pos_offset`,
   `min_permitted_item_size`, `max_permitted_item_size`, `is_external_container`, `excluded_item_sub_types`, plus any
   capacity / type-restriction fields the record has, verbatim. Zeros stay zeros; no invented semantics.
5. **Root item occupancy**: when the exported root entity has `SAttachableComponentParams.AttachDef`
   inventory-occupancy fields, `root_entity` gains `inventory_occupancy` {`dimensions` m, `volume` (+ its unit as
   authored)}, `standard_cargo_units` (from `SStandardCargoUnit`, if present), `mass` (physics controller, raw),
   `item_type` / `item_sub_type`, and `display_name` (localised, if the exporter already resolves names). This lets a
   consumer export a cargo box with `entity export` and know its footprint.
6. Keep the scene version (2): the fields are additive and readers reject unknown versions; consumers detect support by the presence of `inventory_containers`. Bump our `scviewer-core` cache version so stale packages re-export.
7. Tests: unit tests with synthetic records for each field; an integration check (ignored without game data) that
   `MISC_Hull_A` yields four containers with distinct `parent_port_path`, 2.5/5/2.5 dimensions, and that the hangar
   socpak used by the slice yields its freight-elevator grid with a transform. Document field semantics, the grid
   origin convention once K1b has verified it visually (corner vs centre; `gridPosOffset`), and the sources.

Verification: `cargo test -p starbreaker-3d`, `cargo test --workspace` (superproject), plus
`target/debug/scviewer.exe decomposed MISC_Hull_A out/k1/hull_a --no-cache` and the hangar socpak export, with the
relevant JSON excerpts pasted. The superproject's FFI needs no new entry point: the data rides in `scene.json`.

## K1b — UE cargo domain, containers, drones (Runtime module)

Files under `SCBridge/Private/Cargo/` and `SCBridge/Public/Cargo/` (one writer). Works without the FFI at run time
except for exporting packages: the cargo logic itself is plain C++.

- **Reader**: parse `inventory_containers` (vehicle + interiors) and `children[].port_path`, `root_entity.inventory_*`
  into `FSCSceneManifest` (`SCPackageReader`); keep a port-path → anchor map on `ASCVehicle` (import job).
- **Grid** `USCCargoGridComponent` (scene component on the helper anchor, so it follows the plate rig): lattice cell
  = `min_permitted_item_size` per axis (data; 1.25 m), cells = interior / cell, max item size; occupancy;
  placed containers; owner label from the port path; access state. Origin convention verified on the Hull A plate and
  the elevator platform by rendering the grid's box (debug draw) — record it in K1a's contract doc.
- **Planner** `FSCCargoPlanner` (pure, automation-tested): placement of an item footprint on a lattice with
  yaw 0°/90° (no tipping), gravity support (a box rests on the floor or fully on boxes), max-item check, best-fit
  ordering; transfer planning from (source grids, commodity, amount SCU) to destination grids → container moves or a
  refusal with a human reason ("24 SCU box is 7.5 m long; Hull A grids take up to 5.0 m"); the achievable amounts
  (subset sums of whole containers that fit) for the UI slider. Containers move whole; no repacking of contents.
- **Extend / retract** (owner, 2026-11-14: "The Hull A is a ship that extends and retracts. It can only accept and
  carry cargo in the extended state."): a grid whose owner has an open/close-family (extend/retract) fragment is
  usable only extended and stays extended while it holds anything; jobs extend before access and never retract a
  loaded grid; a retract request on a loaded grid is refused with its reason. The mobiGlas shows the state and
  offers EXTEND / RETRACT (`RequestGridAccess`, `sc.Cargo.Access`).
- **Containers** `ASCCargoContainer`: one package export per container entity (cached), meshes reused across
  instances; box collision from occupancy dimensions; mass from data; physics simulated while carried, locked
  (attached to the grid component, simulation off) once placed.
- **Drones** `ASCCargoDrone` + `ASCCargoDroneDock`: drones live in a dock that is itself a grid item (a 1-SCU
  footprint module on the ship's grid: stored *on* the ship, takes a cell like any box). Flight: kinematic controller
  with thrust/speed limits, clearance altitude from the scene bounds, approach aligned with the grid axes. Lift force
  per drone from a tractor-beam record named by the drone definition (data, e.g. the multitool cargo mode); a load
  needs `ceil(m·g / F)` drones, which fly in formation. The container hangs from the drones by a physics handle
  (it sways, collides, has its mass); placing it snaps to the slot and locks.
- **Transfers** `USCCargoSubsystem` (world subsystem, ticked): discovers grids on the vehicle and the environment
  after load, filters "nearby" (distance to the player, data-free radius setting), runs a queue of jobs (states:
  Planned → Deploying grid → Lifting → Transit → Placing → Locked; Cancelled/Failed with reason), assigns drones,
  deploys grids whose owner has an Open/Close-family Mannequin fragment (by fragment tags, not by names) before
  access, retracts after. Console: `sc.Cargo.List`, `sc.Cargo.Transfer <from> <to> <commodity> <scu>`,
  `sc.Cargo.Cancel <id>`, `sc.Cargo.Debug 0|1`. Log markers `CARGO_JOB id=.. state=..`.
- **Scenario** (content, not code): `game/Config/Cargo/<name>.json` — ship, environment, stock per grid
  selector (environment/vehicle + grid index or nearest), container entity names and counts, dock count. Settings
  `CargoScenario` / `-SCCargoScenario=`.

## K1c — mobiGlas and callouts (UI, Slate)

Files `SCBridge/Private/UI/Cargo/*` (one writer). Style per `docs/cargo/research-starfield-ui.md` §7: flat paper
panels, ink text, 0 radius, 1 px rules, 6 px bars, the four-stripe motif once per panel; engine Roboto (no font
downloads) in the type scale's sizes. Tokens in one style file (`SCMobiGlasStyle`), not scattered.

- **mobiGlas** (F1 toggles; Esc closes; cursor mode while open; the view becomes an orbit camera around the selected ship or elevator — drag rotates, wheel zooms — and the player stands still; closing restores the first-person view, owner 2026-11-14): apps rail with
  one app, **Cargo**. Columns: *Nearby grids* (grouped by owner: ship / elevator; used/total SCU bar, state, distance),
  *Grid detail* (top-down lattice per layer with containers coloured by commodity, contents table), *Transfer*
  (from, to — any grid or "ship" as a group —, commodity, amount slider snapped to achievable amounts, fit preview:
  which boxes go where, refusals with reasons; Confirm), *Transfers* (jobs, state, drones, progress, ETA, cancel).
- **Callouts** (`SSCCalloutLayer`, on the viewport overlay): grids, boxes in flight and drones. Anchor dot on the
  object, leader line of 0°/45°/90° segments to a label placed outside the object's projected bounds + 16 px, never
  covering it; candidates NE → NW → SE → SW with hysteresis; columns per side ordered by anchor y so lines do not
  cross; ≤ 8 labels, priority to active jobs and the selected grid; fade/slide 120–140 ms. Shown for active jobs
  always, for grids while the mobiGlas is open.
- Visible in showcase mode (not gated on chrome visibility).

## K1d — Stand-in art (Codex Astra)

Drone (≈ 1 m class, four ducted lift fans, belly tractor emitter, nav lights, MISC/industrial hard-surface language
matching SC content), drone dock (1.25 m footprint cradle holding the drones), tractor-beam material, grid slot
highlight (flat lines, not a hologram). Built reproducibly by an editor Python script with Geometry Script
(`game/scripts/create_cargo_art.py`) into the game's content `game/Content/Cargo/` (`/Game/Cargo/`; the shared plugin's `/SCBridge/Cargo/` until WP-O1-4c-3c; `/Game/SC/` is gitignored game-derived content); no game-derived files in git. Reference renders of SC
drone geometry (`AEGS_Reclaimer_Drone.cga`, `mining_drone_body_001_a.cga`, `ph_repairdrone.cgf`) may be exported to
`out/` for study. Also an HTML/SVG mock of the mobiGlas Cargo app at 1920×1080 using the tokens.

## Done when

- `cargo test --workspace` green; K1a fields present for Hull A and the hangar elevator (excerpts in the PR).
- UE Development Editor builds on Win64; cargo planner automation tests pass (`Automation RunTests SCBridge.Cargo`).
- Route `game/scripts/routes/cargo-hull-a.txt` (first person, hangar + Hull A): opens the mobiGlas, orders an
  elevator → Hull A transfer, the 32 SCU box is refused with its reason, 16 SCU boxes are carried by drones and lock
  on the grids, and back; shots at each stage via `-SCShotDir`. Shots and timings in `viewer/verification/WP-K1.md`.
- No asset-name branches; every game value comes from the package; docs updated.

## Out of scope

Elevator kiosk parity, commodities economy, persistence, multiplayer, ship-to-ship transfers in flight, drones
avoiding arbitrary dynamic obstacles beyond clearance altitude.
