# WP-P5 — Lifts: carriages, stops and lift doors that move like the game's

Branch `wp/P5-lifts`. Exporter changes go on a submodule branch `scviewer/lifts`.

**Depends on:** WP-P1 (first person, interactions, doors), DOORS-1 (pads, `links`, proximity sensors), WP-P3 / WP-P4
(game collision, grid rule, `no_surface`), WP-K1e (`FSCEnvironmentAccess`, environment rigs).
**Works with:** WP-I1 (`docs/work-packages/WP-I1-door-and-lift-panels.md`). I1 owns the panels: their entities,
look, content and how a press is routed. P5 owns the lift: the carriage, its motion and logic, the stops, the lift
doors, and the inputs the lift logic accepts. Section 6 is the contract between them.
**Build locks:** cargo lock (exporter, `scviewer-core`, FFI), UE lock (plugin).

## Goal

Lifts in the viewer move as they do in the game. The carriage travels between its stops at the authored speed and
easing. The landing doors, the carriage gate and any hatch it passes open and close in the authored order. The call
buttons and floor buttons drive it. The player rides it in first person, and what is attached to the carriage moves
with it. As with doors, everything comes from the game files: no lift, stop, height or timing is written by hand.

Scope:

- ship-internal lifts of the `SCItemElevator` family, starting with the C2 Hercules;
- the hangar's freight elevators and ship elevator (`LoadingPlatform`);
- the hangar's personnel elevator (a transit gateway). Its carriage is not in the hangar data, so it is a later
  slice (section 8).

## Read first

`AGENTS.md`; `viewer/README.md` §First-person exploration (interactions are Mannequin fragments with an Open/Close tag pair,
`sc.FP.Door`, `FP_DOOR_LIST`); `viewer/verification/DOORS-1.md`; `docs/work-packages/WP-P4-moving-collision.md`;
`docs/cargo/datamine.md` §Freight elevators; `docs/work-packages/WP-I1-door-and-lift-panels.md` §3, §5;
`out/lifts-datamine/NOTES.md` (the findings log, with every file and command behind this brief).

## 1. Findings: the C2 Hercules internal lift

Evidence: LIVE 4.10.2, DataCore through the prebuilt CLI, `top_deck_c2.socpak` unpacked and its `.soc` entity
document converted (`out/lifts-datamine/c2_soc_lift_entities.xml` holds the 11 entities verbatim). Coordinates are
the container frame in metres, CryEngine Z up. The container sits on bone `animated_interior_top_deck`, and today's
export places it at the ship origin.

### Entities

| Entity (instance) | Class record (GUID) | Geometry | Position |
|---|---|---|---|
| `CRUS_Hercules_Lift_Platform-001` (`42d29922-9f0a-b30b-33a9-05929f61d2bb`): **the carriage** | `CRUS_Hercules_Lift_Platform` (`ea08b529-6dba-4893-baca-b2412482a31e`), `AttachDef.Type = Elevator` | `Objects/Spaceships/Ships/CRUS/Starlifter/Interior/Lift/CRUS_Starlifter_Elevator_Platform.cga` (nodes `Lift_Platform` with a 172-triangle trimesh, `Decal_Lift_Platform`, `$nav_a`, `$nav_b`) | (−7.5, −3, −7.875) |
| `CRUS_Hercules_Elevator_Inner_Gate-001` (`4b931226-…`): carriage gate | `…_Inner_Gate` (`16bd601d-fc4b-42d6-99f0-081fd780726e`) | `…/Lift/CRUS_Starlifter_Elevator_Inner_Gate.cga` | attached to the carriage |
| `CRUS_Hercules_Elevator_ButtonsInner-002` (`4e867e5a-…`): floor panel | `…_ButtonsInner` (`ca2d6f90-5516-4b2f-9a5c-49a38d0ae00d`) | `…/Lift/CRUS_Starlifter_Elevator_Inner_Buttons.cga` (`$proxy_elevator_button`) | attached to the carriage |
| `Elevator_Floor-003` / `-001` / `-000` (`4418aa71-…`, `4bed6723-…`, `45276da2-…`): **stops** | `Elevator_Floor` (`64b85850-0ee2-4397-aabb-6cb3ce2a7f6a`) | none | z −14.65 / −7.875 / −0.5, all at x −7.5, y −3 |
| `CRUS_Hercules_Elevator_Door-001` (`4376aaa1-…`): hold landing door | `…_Elevator_Door` (`a13a3784-175e-4dc0-a4ff-a81e09ce5431`) | `…/Interior/Doors/CRUS_Starlifter_Lift_Doors.cga` | (−6.08, −3, −7.875) |
| `CRUS_Hercules_Elevator_Door_Top-001` (`4edb736f-…`): top landing door | `…_Door_Top` (`0196e46a-0c96-4da4-b230-dc9921242d13`) | `…/Doors/CRUS_Starlifter_Lift_Doors_Top.cga` | (−6.08, −3, −0.5) |
| `Door_Ship_Exterior_CRUS_Starlifter_Elevator_Door` (`405af200-…`): belly hatch | same name (`a9140cda-2dd0-416d-b164-b636aa8207fb`), the full exterior-door state machine | `…/Lift/CRUS_Starlifter_Elevator_Exterior_Door.cga` | (−7.375, −3, −9.75) |
| `CRUS_Hercules_Elevator_ButtonsCall-000` / `-002` (`4d52b73d-…`, `49b375f1-…`): call buttons | `…_ButtonsCall` (`4a218af8-2210-4e50-b88e-50f854508e01`) | `…/Lift/CRUS_Starlifter_Elevator_Call_Button.cga` (`$IP_Button`) | hold z −7.875, top z −0.5 |
| `CRUS_Hercules_Elevator_Buttons_Exterior-000` / `-001` (`49bd738e-…`, `472581fe-…`): ground call | `…_Buttons_Exterior` (`646e5ad1-fbe2-4345-9934-f3623c463680`) | `ORIG_600i_Button_Exterior.cga` (1.7 KB, helper only, no `.cgam`) | z −9.70 and −13.5 |

So the lift has three stops: the ground under the ship ("Exit Ship", z −14.65), the hold ("Lower Deck", z −7.875,
where the carriage rests) and the top deck ("Top Deck", z −0.5). On the way down it passes the belly hatch.

### How it moves: code, driven by data

The carriage has no clip, no `.chrparams` and no Mannequin database. The game's `SCItemElevator` component moves the
entity along its path nodes. Its parameters are data (`SCItemElevatorParams` on the class):

| Field | C2 value | Meaning (how sure) |
|---|---|---|
| `MaxSpeed` | 3.5 (m/s) | cruise speed (certain) |
| `EasingDistance` | 5.1 (m) | distance over which it ramps up and down (the effect triggers `RampUp` / `FullSpeed` / `RampDown` confirm a three-phase profile; the ramp's shape is code) |
| `SlowDownAtBends` | false | no slowing at intermediate nodes (certain, irrelevant on a straight shaft) |
| `OpenWaitTime` | 4 (s) | doors-open dwell before the carriage serves the next request (inferred) |
| `QueueWaitTime` | 5 (s) | time a new request waits to be batched (inferred) |
| `IdlingPeriod` | 120 (s) | idle time before `PowerDown` (inferred from the `PowerUp` / `PowerDown` triggers) |
| Interactions | `OpenInnerDoor`, `CloseAllDoors` (sendable); `OpenInnerDoorFinished`, `CloseInnerDoorFinished` (linkable) | the carriage's own door handshake |
| Effects | tags `EntityEffectSystem/Triggers/Elevators/{Start, RampUp, FullSpeed, RampDown, Stop, PowerUp, PowerDown}`, `…/Tags/Elevators/InTransit` | phase events; sounds `Play_SSMP_CRUS_StarLifter_Elevator_Move_{Start,Loop,Stop}` |
| Physics | own grid `LocalGrid_Lift_Platform.cgf`, `gridType Small`, gravity (0, 0, −9.81), `inheritGravity false`, `PushableByPlayers true` | in the game the rider changes into the carriage's grid |

The stops are `Elevator_Floor` path nodes (`SCItemElevatorPathNodeParams`: `TeleportTimeScale 1`, gateway
`timingInTransit 0.5`, interactions `DoorOpen`, `DoorClose`, `AscentRequest`, `DescentRequest`, `CancelStop`,
`GatewayOpen`). Their `EntityLinks` form a directed chain and name each stop's landing door:

```
Floor-003 (z -14.65)  -> Floor-001
Floor-001 (z -7.875)  -> hold door, Floor-000          <- platform links Floor-001 (where it rests)
Floor-000 (z  -0.5)   -> top door
```

Chain order from the head (the node no other node links to) gives the stop index the carriage's slots use: 0 =
ground, 1 = hold, 2 = top. On the C2 and the 600i this equals the order by height.

### How it is wired (`InteractionLinkController`, source slot → target slot)

| From | Source slot | To | Target slot |
|---|---|---|---|
| floor panel | `RequestSurface` | carriage | `From Elevator call 0` |
| floor panel | `RequestSurface` | belly hatch | `Open` |
| floor panel | `RequestFloor0` ("Lower Deck") | carriage | `From Elevator call 1` |
| floor panel | `RequestFloor1` ("Top Deck", primary) | carriage | `From Elevator call 2` |
| hold call button | `Descend` ("Call Elevator") | Floor-001 | `DescentRequest` |
| top call button | `Descend` | Floor-000 | `DescentRequest` |
| ground call buttons (2) | `Descend` | Floor-003 | `DescentRequest` |
| ground call buttons (2) | `Descend` | belly hatch | `Open` |
| carriage | `Arrived at 1` | belly hatch | `Close` |
| carriage | `Arrived at 2` | belly hatch | `Close` |
| Floor-003 | `DoorOpen` | Floor-001 | `DoorOpen` |

Runtime links, built by the game's traversal trees (`SEntityTraversingTreeParams`): Floor-003 `GatewayOpen` → the
vehicle's item port `hardpoint_Elevator_Exterior_Door`, `OpenSelf`; Floor-001 `GatewayOpen` → the same port, `Close`.
`hardpoint_Elevator_Exterior_Door` appears only as a resource-network link of the C2 entity, so mapping it to the
belly hatch is not resolved (Q4). The panel's `RequestFloor2..4` exist on the class but are not wired.

### Riding parts, doors, timing

- **Attached to the carriage:** the inner gate and the floor panel. Both have `ParentEntityCryGUID = 42d29922…`,
  `AttachmentType = CharacterBone`, `AttachmentTarget = Lift_Platform`, and an identity offset. No light is parented
  to the carriage. The shaft lights (`Lt_TD_Lift_*`, `EP_Lift_Shaft`) are static.
- **Doors** all use `SCItem_Door_Anims.adb`, Open/Close tags, `DoorAnimationSpeed 1` and `UseAutoCloseDelay false`.
  Clip lengths at 30 fps, from the export:

  | Door | Frames | Seconds |
  |---|---|---|
  | hold door | 80 | 2.67 |
  | top door | 50 | 1.67 |
  | inner gate | 15 | 0.5 |
  | belly hatch | 200 | 6.67 |

- **Travel time** under the trapezoid reading (acceleration a = MaxSpeed² / (2 · EasingDistance) = 1.20 m/s²). This
  reading is not verified against the game (Q1).

  | Leg | Distance | Time | Peak speed |
  |---|---|---|---|
  | hold ↔ top | 7.375 m | 4.96 s | 2.98 m/s |
  | ground ↔ hold | 6.775 m | 4.75 s | 2.85 m/s |
  | ground ↔ top | 14.15 m | 6.96 s | 3.5 m/s |

  From the hold, the carriage reaches the hatch plane (1.875 m down) after 1.77 s, but the hatch takes 6.67 s to
  open. So the carriage must wait for the gateway.

### Same family

20 classes carry `SCItemElevatorParams` (`out/lifts-datamine/sc_item_elevator_params.txt`):

- **Ships:** C2, 600i, 890J, Reclaimer, RAFT, Corsair, Polaris, Starlancer, Idris bridge and cargo.
- **Locations:** Levski, Chemline, `lowtech_bravo`, templates.
- **Speeds:** 1 to 60 m/s; easing 0.1 to 5.1 m.

The 600i (`ORIG/600i/middle.socpak`) is wired the same way, and adds two links: landing door `Open` → call button
`ElevatorArrived`, and call buttons → carriage `Request descent from N`. The path-node classes include
`Elevator_Waypoint` and `ElevatorBase_TeleportNode`, which are nodes that are not stops. One implementation covers all
of these.

## 2. Findings: hangar elevators

### Ship elevator and freight elevators: clip-driven "doors"

`LoadingPlatform` (`23a69a27-f4e6-4308-b29b-62f3acf56e31`) is an `SCItemDoorParams` door: Open/Close tags on
`SCItem_Door_Anims.adb`, default `IsOpened true`. Its Open/Close clip moves the whole pad.

| Clip | Travel | Frames | Seconds |
|---|---|---|---|
| `elev_ht_cargo_pad_l` | 16 m | 138 keys | |
| `ht_hangar_ship_elev_lrg_a` | 64 m | 388 keys | |
| `ht_hangar_ship_elev_xl_a` | | 690 | 23 |
| roof doors (`ht_hangar_ship_elev_door_xl_a`) | | 810 | 27 |
| freight pad xl | | 150 | 5 |
| freight gate | | 60 | 2 |

Both rows with a travel figure are eased curves on one bone.

`LoadingPlatformManager` (`867b7b28-e62f-49dd-ae39-9057953e8e7b`, placed per hangar module) carries
`SCLoadingPlatformManagerParams`:

- `loadingPlatformEntityReferences`: platform(s), `frontGate` (`LoadingPlatformFrontGate`, `323fe170-…`), `kiosk`,
  `obstructionCheckBounds`, and a light group. Each reference is an `entitySuperGUID` of the form
  "container instance, entity".
- `trackviewParams`: open and close TrackView sequences `sq_ht_xlrg_elev_open` and `sq_ht_xlrg_elev_close` (10 s).
  These animate only VFX and `LightState` (`Emergency` while in transit). There is no motion in them.
- Instance overrides change timing: the medfrnt ship elevator has `DoorAnimationSpeed 1.2`.

The viewer already plays these clips: WP-K1e's `FSCEnvironmentAccess::OpenGrid` and `FSCEnvironment::SetState`.
What is missing:

- the manager linkage: what moves together, the gate before the pad, the lights in transit;
- the per-instance animation speed;
- a verified ride: the pad is a rigid `.cga` part under a bone component, so `CharacterMovement` based movement should
  carry the player.

The kiosk is a UI and belongs to WP-K1 / WP-I1. Cooked hangars bake rigs static (WP-C1), so nothing moves there until
the cooked rigs can be animated.

### Personnel elevator: a transit gateway without a carriage

`playerhangar_elev_hangar_ht_gateway.socpak` appears 8 times in `hangar_xltop_001` and 4 times in
`hangar_medfrnt_001`. It holds:

- `TransitGateway` (`e52604d7-1685-4ff3-bf4b-e53ef8fd56c0`): radius 8, tag `Locations/Transit/Spaceport_Hangar`;
- `TransitLimboNode`, 6 m below the gateway;
- `ElevatorDoorDefaultTemplate` (`f8dfcf6b-…`): `elevator_door_double_a.cga`, 75 frames = 2.5 s;
- `TransitInteractionUIPanel_Elevator_Ext` (`eed1fe3c-…`).

The carriage and its `TransitManager` (`TransitManagerParams`: carriage type and interior, `carriageWaitTime`,
`carriageDoorTimeout`, `carriageAcceleration`…) belong to the landing zone's transit line. They are not in the
hangar's data. The carriage arrives from limbo at the gateway, so in the hangar there is no travel to show, only
doors. The exporter skips `TransitGateway` and `TransitManager` by class (`socpak.rs` skip list).

`elev_ht_dropoff` is the medical drop-off room, with `EmergencyServicesElevator_Ext/_Int` panels and its own door. It
has no carriage either. Ship lifts on the transit system (`TransitCarriage_*`: 890J, Carrack, Javelin, Idris,
Polaris, Ironclad, 400i, Hull C; `TransportCarriage_VehicleElevators_*`: Galaxy, Hyun, Liberator, Polaris) carry their
manager and nav spline in the ship's own containers: a later slice.

## 3. Data versus engine code

| Data (export it, read it) | Engine code (re-implement, state the reading, verify against the game) |
|---|---|
| stops: path-node positions, chain, landing doors (EntityLinks) | the speed profile's shape between the eased phases |
| `MaxSpeed`, `EasingDistance`, `SlowDownAtBends`, `OpenWaitTime`, `QueueWaitTime`, `IdlingPeriod` | the request queue: order, direction, batching |
| interaction names, interaction links (who sends what to whom) | the door handshake order: gate and landing door close, then move, then open |
| what rides the carriage (`ParentEntityCryGUID`, bone) | when a gateway hatch opens relative to the transit (`timingInTransit 0.5`: seconds or fraction?) |
| door clips and their speeds (`DoorAnimationSpeed`, per-instance overrides) | grid transition of the rider (we use UE based movement) |
| loading-platform manager references; its TrackView light and VFX keys | what `ElevatorArrived` reaches when no link names it (C2) |
| effect and sound triggers per phase | |

The viewer re-implements the right column as a documented reading. Each reading is named in the code and the README
with its source fields, so a later capture can confirm it or correct it. No value in it is invented. Where data is
silent, the behaviour follows from the data that exists (for example: wait for the hatch's own open clip to end).

## 4. What the exporter carries today, and what it must add

**Today** (C2 package, `out/screens-datamine/c2`): the carriage, both landing doors, the belly hatch, the inner gate,
the floor panel and the two call buttons are placements of `interiors[2]` with `entity_id`, `entity_class_guid` and
EntityLinks (`links`). The doors have Open/Close clips. The carriage is a rigid static placement at the hold.

| Missing | Effect |
|---|---|
| the three path nodes (no geometry, dropped) | no stops |
| both ground call buttons (helper-only CGA, dropped) | nothing to press outside |
| every `InteractionLinkController` link | the call buttons and the floor panel route nowhere (WP-I1 "no links") |
| the parent attachment of gate and panel | they would not ride |
| `SCItemElevatorParams` | no speed, easing or timings |

There is also a **side effect**: the DOORS-1 pad pass (`SCImportJob.cpp:1293–1306`) makes the carriage a door control
of the inner gate, because its EntityLinks name a door rig. E aimed at the carriage floor toggles the gate.

**Additions** (submodule branch `scviewer/lifts`; contract in `vendor/StarBreaker/docs/decomposed-export-contract.md`;
tests first):

1. **Interaction links** on every interior placement (and on the new non-mesh entities below):
   `interaction_links[] {source_slot, target_entity_id, target_slot}` from `InteractionLinkController/Link`, in
   authored order. This is generic: doors and pads use the same mechanism, and WP-I1 consumes it for routing.
2. **Parent attachment** on placements: `parent_entity_id`, `attachment_type` (`CharacterBone`), `attachment_target`
   (node name) and the parent offset (pos, rot, scale). The placement's own transform stays as authored (world in
   the container). `attachment_target_frame` (`socpak.rs:1731`) already parses this for light groups: reuse it.
3. **Geometry-less interactive entities** as `interiors[].entities[]`, recognised by component, not by class name:
   - path nodes (`SCItemElevatorPathNodeParams`);
   - transit gateways and limbo nodes (`TransitGatewayParams`, `TransitLimboNodeParams`);
   - interaction-only placements whose CGA has no mesh (the ground call buttons): give them `interaction_point`
     helpers instead of a mesh.

   Each carries `entity_id`, class GUID, transform, `links`, `interaction_links`, and the instance's
   `PropertiesDataCore` overrides that matter: `gatewayParams.interaction`,
   `accessibilityParams {type, accessibleTimingInTransit}`, and the traversal tree's
   `{itemPortName, linkedInteractionName, localInteraction}` resolved to the interaction's name.
   Take `TransitGateway` and `TransitManager` off the skip list for this record only (they stay unrendered).
4. **`interiors[].lifts[]`**, one per entity whose class has `SCItemElevatorParams`:
   - `entity_id`, class GUID and record name;
   - `params`: the six fields above plus the four interaction names;
   - `grid_geometry` path;
   - `rest_node_id`: the node the carriage links;
   - `stops[]`, in chain order: `{index, node_entity_id, class (stop / waypoint / teleport), position,
     landing_door_ids[], gateway_link?}`;
   - `riders[]`: the placement ids whose parent chain reaches the carriage.

   Chain order: follow the directed EntityLinks between path nodes from the node no other node links to. A branched
   or cyclic graph is a contract error, logged, and that lift is not exported.
5. **Door timing:** `SCItemDoorParams.DoorAnimationSpeed` (class value and instance override) and
   `UseAutoCloseDelay` / `AutoCloseDelay` on placements that carry a door clip.
6. **Loading platforms (hangar slice):** each `LoadingPlatformManager`'s resolved references, with each
   `entitySuperGUID` "container instance, entity" pair mapped to the exported placement ids. The light group and
   `LightState` keys come from the open and close sequences, the obstruction box as a volume.
7. `scviewer-core` mirrors the fields (`CACHE_FORMAT_VERSION` + 1); `FSCPackageReader` parses them. The FFI needs no
   new call. `sc_datacore_record` remains the fallback for a field the package lacks.

## 5. Viewer design

### Model

`FSCLift` lives in a new `Private/SCLift.cpp` / `Public/SCLift.h`. One is built per `lifts[]` entry and owned by
`ASCVehicle` (`Lifts`), which ticks it before physics. It holds:

- **Carriage:** a new anchor `USceneComponent` `Lift_<entity_id>`, created at the carriage's transform under the
  interior container. The carriage's mesh components and every rider (gate rig, panel, any lights or GeomEntities
  parented to it) are re-parented under this anchor at load, keeping their world transform; a rider whose parent is a
  bone of the carriage mesh attaches at that node. The anchor and its children become `Movable` and are excluded from
  `FSCInstancing` merging. This is the "lights and rooms move with it" rule. Light areas (WP-L1) stay static: a light
  riding a carriage keeps its room membership (nothing on the C2 needs more).
- **Path:** the stops' positions, a polyline in the container frame; position s (metres along the path) and
  velocity v.
- **Motion:** a three-phase profile from `MaxSpeed` and `EasingDistance`: accelerate over `EasingDistance`, cruise,
  decelerate over `EasingDistance`. Constant acceleration a = MaxSpeed² / (2 · EasingDistance) gives a triangle when
  the leg is shorter than 2 · `EasingDistance`. `SlowDownAtBends` stops at no intermediate node. The phase changes
  emit `Start`, `RampUp`, `FullSpeed`, `RampDown` and `Stop`. The profile shape is a reading (section 3, Q1), kept in
  one function with its source fields.
- **Doors:**
  - landing doors per stop (the node's EntityLinks targets that are door rigs);
  - the carriage gate (riders that are door rigs);
  - gateway doors: the targets of the carriage's and the panels' interaction links with a door verb (`Open`,
    `Close`, `OpenSelf`), here the belly hatch.

  All of them play through the existing `PlayFragmentAt(SCItemDoor, Open|Close, Rig)` at
  `DoorAnimationSpeed`. "Finished" is the clip's end.
- **Logic** (states `PoweredDown`, `Idle`, `Closing`, `WaitingForGateway`, `Moving`, `Opening`, `Open`), in order:
  1. A request is queued (a stop index and its source). After `QueueWaitTime` from the first request (or at once
     when idle with doors closed), the carriage serves the nearest requested stop in its direction of travel.
  2. **Closing:** `CloseAllDoors` closes the gate and the current stop's landing door. Wait for both clips
     (`CloseInnerDoorFinished`).
  3. **WaitingForGateway:** before entering a leg, sweep the carriage's collision along it. A door rig in the way
     whose open was sent by a link (the belly hatch: `RequestSurface` / ground `Descend` → `Open`) is waited for until
     its clip ends. Anything else in the way blocks the leg (`LIFT_BLOCKED`, no move).
  4. **Moving:** follow the profile; passing a stop emits `Passed`.
  5. **Arriving:** emit `Arrived at <N>` to the carriage's interaction links (the C2 closes its hatch here), then
     `OpenInnerDoor` and the node's `DoorOpen` open the gate and the landing door.
  6. **Open:** stay at least `OpenWaitTime` before serving the next request.
  7. After `IdlingPeriod` with no request: `PowerDown` (an event only).

  This is the reading of the handshake that the four carriage interactions name (section 3, Q2).
- **Rest state:** at load the carriage stands at its `rest_node_id`, doors closed. The landing door and gate are left
  in their bind pose, which is closed on the C2.

### Riding (WP-P4 moving collision)

The carriage floor is the platform mesh's own trimesh, which moves as a kinematic body with its anchor. The first-person
capsule stands on it, and `UCharacterMovementComponent` based movement carries it: the base is the platform component,
`bImpartBaseVelocity*` stays on. Rig colliders (the gate) already follow their bones.

The WP-P3 grid rule is unchanged. Below the hull (the ground stop) the rider leaves the ship's grid volume, and the
hull colliders count again. The carriage's own grid (`LocalGrid_Lift_Platform.cgf`) is recorded but not needed by UE.
The P4 `no_surface` rule must leave the belly opening free for the shaft (risk R3).

Teleport and noclip-exit checks see the carriage where it is, because there is one collision world.

### Inputs, states and events: the contract with WP-I1

See section 6. Input names are the data's own slot and interaction names, so a link exported from the game reaches
the lift unchanged.

### Hangar elevators

`LoadingPlatform` stays a door-clip rig (no `FSCLift`). The manager linkage groups pad, gate and light group:

1. Open: gate first, then the pad.
2. Close: pad, then gate. This order is the manager's reading (Q6).
3. The light group takes `Emergency` while in transit, from the sequence's `LightState` keys.

The pad's riding uses the same based movement (rigid parts under a bone component). The personnel gateway gets its
door and panel. Its call is logged (`LIFT_GATEWAY id= no_carriage`), and the door stays shut until a carriage can be
sourced (section 8, P5-4).

### Console, RPC and logs (scripted verification)

| Command | Effect |
|---|---|
| `sc.Lift.List` | `LIFT_LIST id=<n> entity=<id> class=<record> stops=<k> at=<stop> state=<s>`; one `LIFT_STOP lift= index= node= z_cm= name= doors=` line per stop |
| `sc.Lift.Call <lift> <stop>` | as a call button at that stop (`AscentRequest`/`DescentRequest` on its node) |
| `sc.Lift.Request <lift> <stop>` | as the floor panel (`From Elevator call <stop>`) |
| `sc.Lift.Doors <lift> open\|close` | `OpenInnerDoor` + node `DoorOpen` / `CloseAllDoors` + node `DoorClose` |
| `sc.Lift.Send <entity_id> <slot>` | deliver one interaction as if a panel sent it: follows the exported links (the I1 press path without a crosshair) |
| `sc.Lift.Where` | `LIFT_WHERE lift= s_m= z_cm= v= phase= queue=` and the player's movement base |

Logs:

- `LIFT_STATE lift= from= to= stop= t=`
- `LIFT_PHASE lift= phase=Start|RampUp|FullSpeed|RampDown|Stop`
- `LIFT_ARRIVED lift= stop= t_leg= dist_m=`
- `LIFT_DOOR lift= stop= door= open|close done=`
- `LIFT_BLOCKED lift= by=`
- `LIFT_RIDE base=<component> rel_z_cm=` once a second while the player stands on a carriage

RPC (WP-H1):

- `/console` already runs the commands;
- add `/wait until=lift_idle` (optionally `lift=<id>`);
- add `lift` records to `SCState.ndjson` (state, s, v, queue, door states).

## 6. The lift's inputs, states and events (interface for WP-I1)

WP-I1 maps a press to `(panel entity_id, interaction name)`. It calls one dispatcher:

```cpp
// ASCVehicle (P5, slice 1): follow the exported interaction_links of SourceEntityId whose source_slot == Interaction
// and deliver each target_slot to its target entity: a lift (carriage or path node) or a door. Returns the deliveries.
int32 ASCVehicle::SendInteraction(const FString& SourceEntityId, FName Interaction);
```

P5 lands the dispatcher, because slice 1 needs it and the link table is exported for it. I1 calls it from its press
path. The dispatcher only delivers along exported links: it never routes by panel class or name.

### Inputs (target slots the lift accepts)

On the carriage entity:

| Slot | Meaning | Typed equivalent |
|---|---|---|
| `From Elevator call <N>` | the floor panel asks for stop N | `RequestStop(N, Source)` |
| `Request descent from <N>`, `Request ascent from <N>` | a call at stop N (600i form; the ascent form follows the same pattern and is unseen so far) | `Call(N, Dir, Source)` |
| `OpenInnerDoor` | open the gate (and the landing door at the current stop) | `OpenDoors()` |
| `CloseAllDoors` | close the gate and the current landing door | `CloseDoors()` |

On a path node (stop N):

| Slot | Typed equivalent |
|---|---|
| `AscentRequest`, `DescentRequest` | `Call(N, Up\|Down, Source)` |
| `DoorOpen`, `DoorClose` | the landing door at N (`OpenDoors` / `CloseDoors` when the carriage is there) |
| `CancelStop` | `CancelStop(N)` |
| `GatewayOpen` | its traversal links (the hatch) |

Door targets (`Open`, `Close`, `OpenSelf`, `CloseSelf`, `Toggle`) go to the existing door API.

"Open" and "close" therefore exist as lift inputs. The C2's panels expose none: its doors open and close with the
carriage. They are there for panels that have them and for `sc.Lift.Doors`.

### State (readable at any time, on `FSCLift`)

| Field | Content |
|---|---|
| `Stops[]` | `{Index, NodeEntityId, Name, Z}`. The name is the display name of the floor-panel interaction routed to that stop: "Exit Ship", "Lower Deck", "Top Deck" from `innerthought_lift_*` / `ui_interactor_reclaimer_elevator_exit`; the stop index when none exists |
| `CurrentStop` | `INDEX_NONE` between stops; `TargetStop` |
| `Phase` | `PoweredDown`, `Idle`, `Closing`, `WaitingForGateway`, `Moving`, `Opening`, `Open` |
| `SpeedPhase` | `RampUp`, `FullSpeed`, `RampDown` |
| `Queue` | the requested stops, each with the source entity id and interaction |
| `DoorState(stop)` | `Closed` / `Opening` / `Open` / `Closing` |

### Events (multicast delegates on `FSCLift`; I1 subscribes)

| Event | Data counterpart |
|---|---|
| `OnRequested(Stop, SourceEntityId, Interaction)` | drives the button's `WaitingForAscend` / `WaitingForDescend` / `WaitingForLobby*` geometry tag and its effect tag |
| `OnRequestCleared(Stop)` | |
| `OnDeparted(Stop)` | |
| `OnPassed(Stop)` | |
| `OnArrived(Stop)` | the carriage's `Arrived at <N>`; for buttons that requested this stop, the `ElevatorArrived` they receive in the game; also fired along any exported link whose target slot is `ElevatorArrived` (600i: landing door `Open` → button) |
| `OnDoors(Stop, State)` | |
| `OnPhase(Start\|RampUp\|FullSpeed\|RampDown\|Stop\|PowerUp\|PowerDown)` | the effect triggers, for audio later |

This refines WP-I1 §5, which named `Call(panel)` and `RequestStop(panel, interaction)` (now `SendInteraction` plus the
typed calls), `OnCalled` (now `OnRequested`), `OnArrived`, `OnDeparted` and the stop name (`Stops[].Name`).

## 7. First slice — P5-1 "the C2 lift works"

**Exporter** (`scviewer/lifts`, tests first in `starbreaker-3d`):

- additions 1–5 of section 4: interaction links, parent attachment, geometry-less entities, `lifts[]`, door speed;
- tests use the C2 `.soc` excerpt (`c2_soc_lift_entities.xml` reduced to a synthetic document, like
  `socpak_entity_tests.rs`):
  - three stops in order ground, hold, top;
  - rest node Floor-001;
  - riders gate and panel;
  - 11 interaction links with their slots;
  - ground call buttons present as interaction-only entities;
  - a branched node graph rejected.
- `scviewer-core` cache version + 1; contract doc.

**Viewer:**

- `FSCPackageReader` fields;
- `FSCLift` (model, motion, logic, gateway wait, riders re-parented, movable, not instanced);
- `ASCVehicle::SendInteraction`;
- the carriage removed from the DOORS-1 pad pass (an entity that is a lift is not a pad);
- console, logs, RPC wait;
- automation tests `SCBridge.Lift.*`:
  - the profile: leg times for 7.375 m and 14.15 m against the formula, monotonic s, v ≤ `MaxSpeed`;
  - queue order;
  - stop order from a synthetic package;
  - a dispatcher test on the C2 links table.
- **The press:** until WP-I1 lands its press path, P5-1 routes E on a lift panel to `SendInteraction(panel,
  <the interaction point's interaction>)`. The floor panel has three interactions on one point ("Top Deck"
  primary, "Lower Deck", "Exit Ship"). The HUD prompt lists them and the mouse wheel picks one, as the game does with
  several interactions on one point. When I1's prompt and selection exist, this falls back to theirs.

**Done means** (`viewer/verification/WP-P5.md`; route `viewer/scripts/routes/c2-lift.txt`; `-SCLoad=CRUS_Starlifter_C2
-SCCamera=first`):

1. `LIFT_LIST` shows one lift with three stops, z_cm −1465 / −787.5 / −50 (container frame) and names Exit Ship /
   Lower Deck / Top Deck. The carriage rests at the hold.
2. In first person, in the hold: E on the hold call button (the real button, by crosshair) finds the carriage already
   there, so it only opens the gate and the hold door (`LIFT_DOOR … open`). Walk in. E on the floor panel, "Top Deck":
   - gate (0.5 s) and hold door (2.67 s) close, the carriage climbs, and the gate and top door open;
   - `LIFT_ARRIVED stop=2 dist_m=7.375 t_leg=` within 0.1 s of the profile (4.96 s under the stated reading);
   - the player's Z rises 737.5 cm with `LIFT_RIDE base=<carriage>` throughout, no `stuck`, no `recover`;
   - the player walks out onto the top deck.
3. On the top deck: send the empty carriage down with `sc.Lift.Request 0 1`. Then E on the top call button brings it
   back up and opens it. Ride down to "Lower Deck".
4. "Exit Ship": the belly hatch opens (6.67 s); the carriage waits for it (`LIFT_STATE … WaitingForGateway`), descends
   to the ground and opens. The player steps off onto the hangar floor (`FP_GRID inside=0`). The ground call
   (`sc.Lift.Send <Buttons_Exterior id> Descend`, until helper-only panels are pressable in I1) brings it back. Riding
   up to the hold closes the hatch on `Arrived at 1`.
5. Regressions:
   - `DOOR_LINKS` loses exactly one control (carriage → gate) and nothing else;
   - `cutlass-ramp-to-cockpit` and `c2-ramp-walk-in` unchanged;
   - `cargo-c2` unchanged (the carriage is movable but at rest).
6. Rust suites green; `UNIT_TESTS_OK`; `SCBridge.Lift.*` pass. Screenshots: inside the carriage mid-travel, the open
   top door from inside, and the open hatch from the ground.

## 8. Later slices

- **P5-2 family sweep.** The 600i and 890J (`ORIG_890J_Lift_Platform_1`), Reclaimer and Corsair with the same code.
  Waypoint and teleport nodes (`Elevator_Waypoint`, `ElevatorBase_TeleportNode`, `TeleportTimeScale`).
  `Request descent from N`. Landing door `Open` → `ElevatorArrived`.
- **P5-3 hangar platforms.**
  - `LoadingPlatformManager` linkage (gate/pad order, light state in transit);
  - per-instance `DoorAnimationSpeed`;
  - riding the freight pad and the ship elevator (`LIFT_RIDE` on the pad bone);
  - a route that stands on the xl freight pad while K1e opens it.
- **P5-4 transit carriages.**
  - **Ship-internal:** the `TransitCarriage_*` / `TransportCarriage_VehicleElevators_*` lines in ship containers
    (`TransitManager` + `TransitNavSpline` + `TransitCarriageParams` `CarriageState` fragments).
  - **Hangar gateway:** find the manager that serves `Locations/Transit/Spaceport_Hangar` gateways in the
    landing-zone containers. Until then the gateway's door stays shut.

## 9. Risks

- **R1 profile shape.** The data gives `MaxSpeed` and `EasingDistance`; the curve is code. A constant-acceleration
  ramp and a smooth (eased) ramp differ by up to about 20 % in leg time. Mitigation: one function, named reading,
  game capture (Q1).
- **R2 handshake order.** Gate, landing door, hatch and dwell order are read from interaction names, not from the
  executable. A wrong order shows as doors closing on the rider or a carriage moving with doors open: the sweep in
  `WaitingForGateway` and the "doors closed before moving" rule guard the second.
- **R3 hull over the shaft.** The C2 belly opening is not in WP-P4's `no_surface` table. If the `Body` collider caps
  the shaft with surfaced triangles, the rider is pushed off the carriage at the hull plane. Check with
  `sc.FP.Probe` down the shaft before building the ground stop; fall back to stopping the slice at hold ↔ top.
- **R4 based movement at 3.5 m/s up.** UE's CMC handles moving bases, but a kinematic trimesh that moves into the
  capsule from below can depenetrate with a jitter. Tick the carriage pre-physics and let the CMC follow its base.
  Measure `LIFT_RIDE rel_z_cm` stability.
- **R5 re-parenting riders.** Instancing, Nanite or Lumen card caches and the collision refit must accept moving
  components. Riders must leave `FSCInstancing` before it merges.
- **R6 scope creep into panels.** Prompt selection among several interactions on one point is I1's. P5-1 adds the
  minimum and hands it over.
- **R7 cooked builds.** Cooked environments bake rigs (WP-C1). A cooked C2 needs the lift's parts left unbaked. Cooked
  ships are not baked today, but check before the showcase.

## 10. Open questions

1. **Speed profile.** Constant acceleration over `EasingDistance`, or a smooth ease? Needs a timed in-game capture of
   the C2 hold → top ride (prediction: 4.96 s door-closed to arrival).
2. **Handshake and dwell.** Do the C2 doors close on their own after `OpenWaitTime`, or stay open until the next
   request? What do `QueueWaitTime` and `IdlingPeriod` do exactly? Needs a capture.
3. **`timingInTransit` / `accessibleTimingInTransit` 0.5:** seconds or a fraction of the leg? When does the game open
   the hatch?
4. **`hardpoint_Elevator_Exterior_Door`.** Which entity is it at run time? (The hatch is a socpak entity on tile port
   `Port17_…`; the name appears on the C2 only as a resource-network link.) Slice 1 does not need it: the explicit
   links open and close the hatch.
5. **`ElevatorArrived` on the C2's buttons.** No link names it. Is it the reverse of the request link? P5 fires
   `OnArrived` either way; I1 decides what lights up.
6. **Loading platform order.** Gate and pad order, and who sends what between manager, kiosk and platform. The
   kiosk's UI is I1's or K1's.
7. **Personnel elevator.** Which landing-zone `TransitManager` serves the hangar gateways, and are its values usable
   for an instanced hangar shown alone?

## Verification commands used for this brief

```
target/debug/starbreaker.exe dcb query EntityClassDefinition --filter '*Hercules*'
target/debug/starbreaker.exe dcb query EntityClassDefinition --filter '*Elevator_Floor'
target/debug/starbreaker.exe dcb query "EntityClassDefinition.Components[SCItemElevatorParams].MaxSpeed"   # and the other five fields
target/debug/starbreaker.exe dcb query EntityClassDefinition --filter '*Transit*'
target/debug/starbreaker.exe dcb query EntityClassDefinition --filter '*TransportCarriage*'
target/debug/starbreaker.exe dcb query EntityClassDefinition --filter '*LoadingPlatform*'
target/debug/starbreaker.exe dcb extract -o out/lifts-datamine/dcb --format json --filter 'libs/foundry/records/transitsystem/**'
target/debug/starbreaker.exe entity loadout CRUS_Starlifter_C2
target/debug/starbreaker.exe p4k extract -o out/lifts-datamine/archive --filter 'Data/ObjectContainers/Ships/CRUS/Starlifter/**'
target/debug/starbreaker.exe p4k extract -o out/lifts-datamine/archive --filter 'Data/ObjectContainers/Setup/elevator_setup/playerhangar/*.socpak'
target/debug/starbreaker.exe cryxml convert <.soc CryXmlB blob> <xml>
target/debug/starbreaker.exe nmc dump <CRUS_Starlifter_Elevator_Platform.cga and the door / button CGAs>
```

Package evidence: `out/screens-datamine/c2` (C2 `scene.json`, 2026-11-19), `out/screens-datamine/hangar`
(`hangar_xltop_001`), `out/k1e/pkg` (lrgtop clip sidecars). Findings log: `out/lifts-datamine/NOTES.md`.

## Result (P5-1)

**State (2026-11-19):** the C2 Hercules lift works in first person from its real call buttons and floor panel, hold ↔
top deck and down through the belly hatch to the ground and back. Exporter on submodule branch `scviewer/lifts`
(`a95900c0`, review fixes `2c99e25c`), cache format 31. Nothing committed in this repo.

### Exporter (`vendor/StarBreaker`, contract "Interaction wiring, entities and lifts")

- **Placements** carry `entity_name`, `interaction_links[] {source_slot, target_entity_id, target_slot}`
  (`InteractionLinkController`, authored order), `parent_attachment {parent_entity_id, attachment_type,
  attachment_target, offset_*}`, `door {animation_speed, use_auto_close_delay, auto_close_delay}` (class
  `SCItemDoorParams` with the instance's `PropertiesDataCore` overrides, found by component `__type`) and
  `interaction_points[] {helper_name, radius, helper_transform, interactions[{name, display_name_key, display_name,
  primary}]}` (English `global.ini`, loaded once per build on first use) (`socpak/entity_interactions.rs`,
  `pipeline/interior_interactions.rs`).
- **`interiors[].entities[]`**: geometry-less interactive entities, by component: path nodes
  (`SCItemElevatorPathNodeParams`), transit gateways and limbo nodes (`TransitGatewayParams` / `TransitLimboNodeParams`;
  `TransitGateway` and `TransitManager` left the class skip list, they have no geometry), and helper-only entities that
  offer interactions and send links (the C2's two ground call buttons, with their `$IP_Button1` frames from the NMC).
  Each carries its `traversal_links[]` (`SEntityTraversingTreeParams`, the local interaction resolved: Floor-003
  `GatewayOpen` → `hardpoint_Elevator_Exterior_Door` `OpenSelf`).
- **`interiors[].lifts[]`** (`pipeline/lifts.rs`): per `SCItemElevatorParams` carriage the six values and four
  interaction names, `grid_geometry`, `rest_node_id`, `stops[]` in chain order (kind from the node's `Type`, position,
  landing doors, teleport scale, gateway timing and interaction, accessibility) and `riders[]`. A branched, cyclic or
  disconnected node graph is logged and that lift is not exported.
- **Bug fixed (test first):** a root container `ItemPort` that a `.soc` entity occupies (`TileAttachmentParams`) was
  placed a second time from the port. The C2 had a second, static carriage 1 m below the hold, in the shaft.
- Tests: `socpak::lift_tests` (7, from the C2's own `.soc` entities: 13 links / 11 distinct, parents, overrides,
  traversal, tile ports, the port duplicate), `pipeline::lifts::tests` (the chain rule: order, branched, cyclic, two
  lifts; and `c2_lift_resolves_from_game_data` on LIVE 4.10.2: three stops ground / hold / top, rest Floor-001, riders
  gate and panel, landing doors, params, grid, ground buttons as interaction entities showing "Call Elevator", the floor
  panel's Top Deck / Lower Deck / Exit Ship, one carriage), `pipeline::interior_interactions::tests` (3).

### Viewer (`plugins/SCBridge`)

- `SCInteractionRecords.h`, `SCPackageReader.*`: the new fields (a lift without two stops or speeds is reported and
  left out).
- `SCLift.h/.cpp`: `FSCLiftProfile` (the constant-acceleration reading; `LIFT_PROFILE` states it in the log), `FSCLift`
  (model, queue, door handshake, gateway wait, events `OnRequested` … `OnPhase`, typed and slot inputs per §6) and
  `FSCInteractionNetwork` (link routing, door verbs, stop slots).
- `SCVehicleLifts.*`: assembly at import (moving anchor `Lift_<id>`, carriage and riders re-parented keeping their world
  transforms, gate / landing / gateway doors, stop names from the panel interactions routed to them, lift panels) and
  the dispatcher `ASCVehicle::SendInteraction`.
- `SCVehicle.*`: lifts tick first; `PlayFragmentAt` takes a speed scale (`DoorAnimationSpeed`) and **prefers the clips
  authored for the target rig's own geometry**. This was a bug (test first): the C2's hold and top lift doors share bone
  names, so each played the other's clip at random, and the hold door opened in 1.67 s with its cage left closed,
  blocking the way in.
- `SCImportJob.cpp`: an entity that is a lift is not a door pad (`DOOR_LINKS panels=34 sensors=8`, one control fewer
  than before: carriage → gate). `SCExploration.cpp`: doors a lift drives are not toggled by E (in the first run E on the
  top landing door opened it onto the empty shaft). `SCInstancing.*`: parts under a lift anchor are not instanced.
- `SCFirstPersonCharacter.*`, `UI/SSCViewportInput.cpp`: E on a lift panel sends its chosen interaction; the HUD lists
  the point's interactions and the mouse wheel picks one (replaced by the floor panel: "Result (floor panel)"); `LIFT_RIDE`; the character ticks after the vehicle.
- `SCLiftCommands.cpp` (`sc.Lift.*`), `SCRpcSubsystem.cpp` (`/wait until=lift_idle`, `lifts[]` in `/state`),
  `SCViewerAutomation.cpp` (`FP_PROBE … panel= choice=`), `viewer/scripts/rpc/c2-lift.ps1` (the scripted run below). The
  route runner has no "wait for the lift", so the run is an RPC driver rather than `viewer/scripts/routes/c2-lift.txt`, and
  the evidence is here rather than in `viewer/verification/WP-P5.md`.
- Tests: `SCBridge.Lift.Profile`, `.StopOrder`, `.QueueAndDoors`, `.Dispatch`, `SCBridge.Rig.FragmentPrefersOwnClip`.

### Tests

| Suite | Result |
|---|---|
| `cargo test -p starbreaker-3d --lib` | 966 passed, 4 ignored (the new lift tests included; `c2_lift_resolves_from_game_data` ran against LIVE 4.10.2) |
| `cargo test -p scviewer-core` | 52 passed, 2 ignored |
| `Automation RunTests SCBridge` (`P5_tests_full.log`) | 165 of 169 pass; the 4 failures are the known `Package.FolderTarImport`, `Player.KitRigAndWeapons`, `Materials.FamilyMapping`, `FirstPerson.SkeletalCollision`. New: `SCBridge.Lift.Profile`, `.StopOrder`, `.QueueAndDoors`, `.Faults`, `.Dispatch`, `SCBridge.Rig.FragmentPrefersOwnClip` (failed 5/16 and 7/16 before its fix) |

### Scripted first-person run (C2 in `hangar_xltop_001`, `viewer/scripts/rpc/c2-lift.ps1`)

The final build's run is `P5_run6.log` (shots in `out/lifts/run4`, driver output `out/lifts/run4/driver.txt`); the run
before the review fixes (`P5_run3.log`, shots `out/lifts/run3-before-review`) gave the same sequence and timings.

`LIFT_LIST`: one lift, stops 0 "Exit Ship" z_cm −1465, 1 "Lower Deck" −787.5, 2 "Top Deck" −50; rests at the hold
(`LIFT_BUILD … rest=1 riders=2 moved=3 gate=1 gateways=1 panels=3`).

| Leg (`P5_run6.log`) | Predicted | `t_leg` | Δ max |
|---|---|---|---|
| hold → top, 7.375 m | 4.956 s | 4.963, 4.959, 4.967 | 0.011 s |
| top → hold | 4.956 s | 4.959, 4.960 | 0.004 s |
| hold → ground, 6.775 m | 4.750 s | 4.756, 4.751 | 0.006 s |
| ground → hold | 4.750 s | 4.754, 4.755 | 0.005 s |
| top → ground, 14.15 m | 6.957 s | 6.958 | 0.001 s |

1. Hold, the real call button by crosshair (`FP_PANEL_PRESS … Descend`): the carriage is there, so only the gate and
   the hold door open (2.67 s, the hold door's own 80-frame clip). Walked in; the base is the carriage.
2. Floor panel, "Top Deck" (primary): after `QueueWaitTime` the gate and the hold door close, the carriage climbs, the
   gate and the top door open. The player's Z rose 737.5 cm (−797.19 → −59.69). Walked out onto the top deck.
3. `sc.Lift.Request 0 1` sent it down empty; the real top call button brought it back up and opened it.
4. Wheel to "Lower Deck" and ridden down; wheel to "Exit Ship": `RequestSurface` → carriage `From Elevator call 0` and
   hatch `Open`. The hatch's 6.67 s ended before the doors had closed, so the gateway check passed at once; down to the
   ground, `FP_GRID inside=0` at Z −1150 on the way. Stepped off onto the hangar floor.
5. Carriage sent up (`Arrived at 1` → hatch `Close`), doors closed, then the ground call (`sc.Lift.Send
   <Buttons_Exterior-000> Descend` → Floor-003 `DescentRequest` + hatch `Open`): **`WaitingForGateway` 6.67 s**
   (136.902 → 143.571) for the hatch, then down. Ridden back up to "Lower Deck"; `Arrived at 1` closed the hatch.
6. After the review fixes, a ground call during a hold → top leg: `Arrived at 2` closed the hatch, the queued ground
   request's own link opened it again (`LINK_REDELIVER`, `LIFT_GATEWAY reopened=1`), and the carriage went top → ground.

**Ride check** (`LIFT_RIDE`, once a second): on every leg the base stayed the carriage platform
(`CRUS_Starlifter_Elevator_Platform`), with the capsule's feet −785.20 … −786.11 cm relative to the carriage anchor and
the floor distance 2.15 cm on every sample while moving (run 3: −785.15 … −786.28 and 1.97 … 2.19); no
`FIRST_PERSON_RECOVER`, no stuck. The only other bases were walked on: the hold threshold and the top landing door's sill,
and the hangar pad at the ground (below).

**Astra** (gpt-6.2-astra, high, the 16 shots of run 3 in `out/lifts/run3-before-review`, answer in
`out/lifts/astra-verdict.txt`): the motion
reads correctly (rails and pedestal fixed to the rider, the shaft passing, nothing left behind at a landing); the landing
door states are sensible and the hatch is open from below; no player or carriage passes through the hull, the hatch or a
landing door. It could not make out a closed carriage gate in the travel shots, and flagged **d4: the carriage at the
ground stop sits in the hangar floor**.

**Findings.**

- **The ground stop was 60 cm below the hangar floor.** The node is at z −14.65 m. In the viewer's landed pose (gear
  compressed, on the xltop ship-elevator pad `ht_hangar_ship_elev_xl_a`) the pad surface is at about −14.05 m, so the
  carriage stopped 60 cm into the pad and the player at the ground stood on the pad above it. Fixed by the ground-stop
  rule below.
- **R3 (hull over the shaft)** did not occur: no hull collider caps the belly opening, and the rider passes the hatch
  plane on the carriage.
- The carriage gate (`CRUS_Starlifter_Elevator_Inner_Gate`, 15 frames on `walltop`) closes and opens with the carriage
  (`LIFT_DOOR role=gate`), but is not distinct in the stills.

### Ground stop (P5-1 follow-up, 2026-11-19)

**What the data says.** It authors no ground stop.

- `SCItemElevatorParams` (DataCore schema, `Game2.dcb` of LIVE 4.10.2) has a field the earlier dumps left out,
  `Collision`. It is a strong pointer to `SElevatorBaseCollisionParams` with two subtypes:
  - `SElevatorBBoxCollisionParams` (`Scale`);
  - `SElevatorCustomCollisionParams` (`Extent`, `Offset` Vec3).

  The executable names `CSCItemElevator::DoesElevatorCollide` and `CSCItemElevator::FillIntersectionParams : unhandled
  collision type`. So the game can test a carriage volume for intersection. `Collision` is **null on all 20 lift classes**,
  the C2's included, and the C2's `.soc` instance does not override it.
- `SCItemElevatorPathNodeParams` carries only `Type`, `TeleportTimeScale`, the five interactions, `gatewayParams`
  (`timingInTransit`, `interaction`) and `accessibilityParams`: nothing about the ground, collision or landing gear.
- The exported stops are unchanged: Floor-003 at z −14.65 m, the same x/y as the other stops.
- The executable's elevator strings have no "stop on ground contact" setting. Its only elevator cvars are
  `g_freight_elevator.*` (freight kiosks).
- Landing gear: the C2's `crus_starlifter_landingsystem` springs are front `lengthTarget` 8.625 (min 7.43, max 9.5) and
  mains 8.64 (min 7.403, max 9.7). In the game the ship rests on these springs. The viewer poses it on the end of the
  `Landing_Gear` `Compress` clip (`RestOnLandingGear`). If that end is the spring's minimum, the game ship stands up to about
  1.2 m higher than the viewer's. The authored stop might then reach the ground there, or even stay above it. Not settled
  (Q8).

The owner's reading (the travel is over-extended and the game stops the carriage on contact) is therefore code behaviour,
not data. The intersection test the executable names is the likely mechanism, but the C2's data does not switch it on.

**Rule implemented** (`FSCLiftLegPlan`, `SCVehicleLifts::GroundContact`):

- When a leg starts downward, the box of the carriage's own colliders is swept from where it stands toward the stop.
  - The sweep is against world collision: `WorldStatic` / `WorldDynamic` components that block the player.
  - It ignores the vehicle itself (hull, gear, carriage), pawns (the riders), actors attached to the vehicle, anything the
    box already overlaps, and hits whose normal does not oppose the motion.
- The first hit above the stop ends the leg there.
- The profile is made for the shortened distance, so it ramps down onto the surface.
- The arrival is the stop's own: `Arrived at <N>`, gate, events and `LIFT_ARRIVED … contact=1`.
- The next leg starts from the contact.
- When nothing is hit, the authored stop is used.
- Log: `LIFT_GROUND_CONTACT lift stop authored_z contact_z authored_dist_m dist_m authored_s predicted_s by` (with
  `carriage_bottom_z_cm`, vehicle frame).

Hold ↔ top is unchanged: no surface lies under an upward leg, and the ship is not ground. The test is
`SCBridge.Lift.GroundStop`:

- the rule alone: a contact above the stop gives the contact and the recomputed time; unset, at the stop, beyond it or
  negative gives the authored stop;
- the lift with a stub surface: hold → ground arrives on it (doors, `Arrived at 0`), ground → hold takes the shortened leg,
  hold → top is still 4.956 s.

**Run** (`P5_ground_run2.log`, `viewer/scripts/rpc/c2-lift.ps1`, shots and `driver.txt` in `out/lifts/ground`).
`c2-lift.ps1` now stands the player in the middle of the carriage (`OnCarriage`) before "Exit Ship" and before the ride up
from the ground. In run 1 (`P5_ground_run1.log`) the walk-in ended on the hold's lip (Y −763.8 instead of −784), and the
player stayed in the hold when the carriage left.

- `LIFT_GROUND_CONTACT stop=0 authored_z=-1465.0 contact_z=-1384.1 authored_dist_m=6.775 dist_m=5.966 authored_s=4.750
  predicted_s=4.458 by=SCVehicle_0/Mesh_147 (ht_hangar_ship_elev_xl_a_LOD0_GameCollision3) carriage_bottom_z_cm=-1505.6`.
  The carriage node stops 80.9 cm above the authored stop. Its floor is about 21 cm above the pad, the platform's
  thickness. Before the fix it was 60 cm below the pad.
- **The pad beside the carriage** (`sc.FP.Probe` straight down from the pad) is at Z **−1505.59**. The carriage's bottom is
  at −1505.6: the gap is under 0.1 cm.
- Legs (`t_leg` vs predicted):

  | Leg | Predicted | `t_leg` |
  |---|---|---|
  | hold → top, 7.375 m | 4.956 s | 4.964, 4.957 |
  | top → hold, 7.375 m | 4.956 s | 4.958, 4.964 |
  | hold → ground, 5.966 m (was 6.775 m / 4.750 s) | 4.458 s | 4.463, 4.464 |
  | ground → hold, 5.966 m | 4.458 s | 4.460, 4.465 |

- At the ground stop the player stands on the carriage (`LIFT_WHERE player base=Mesh_3127`, the platform). Stepping off
  onto the pad drops the capsule 21.6 cm (Z −1393.8 → −1415.4). The ground call brought the carriage back to the same
  contact. Teleported onto it, the player rode up to the hold.
- `LIFT_RIDE` on the carriage: 66 of 67 samples at rel_z −785.20 / floor 2.15 cm, one at −785.08 / 2.26. No
  `FIRST_PERSON_RECOVER`, no stuck.
- **Astra** (gpt-6.2-astra, high; d2, d4, d5, d6; `out/lifts/ground/astra-ground-check.txt`): **OK**. Nothing sinks into
  the pad and nothing floats above it. The plinth's lower edge meets the pad, and a step of about 20 cm looks plausible.
- Tests: `Automation RunTests SCBridge` (`P5_ground_tests.log`) passes 166 of 170, the new `Lift.GroundStop` included. The
  4 failures are the known `Package.FolderTarImport`, `Player.KitRigAndWeapons`, `Materials.FamilyMapping` and
  `FirstPerson.SkeletalCollision`.

Q8 (open): where does the game's C2 stand on its gear, and where does its carriage stop on the ground? Does
`DoesElevatorCollide` run with a null `Collision`? This needs an in-game capture (with Q1).

### Regressions

On the final build (`P5_reg2_*.log`; the pre-review build gave the same numbers, `P5_reg_*.log`):

- **`DOOR_LINKS panels=34 sensors=8`**: one control fewer than WP-I1's 35 (the carriage → gate), nothing else.
  `FP_DOOR_LIST` still has 34 doors.
- **`c2-ramp-walk-in`**: ramp foot X −4796, on the ramp −3090, hold **−1386.1** (WP-P4: −1388), back out −6885 (−6887),
  in again −1385.9 (−1387), ramp shut: stopped at −3142.5 (−3144).
- **`cutlass-ramp-to-cockpit`**: bulkhead −105.3 (WP-P4: −109), cockpit **1064.77** (1064.77), the second noclip exit
  refused as before.
- **`cargo-c2`** (scenario `c2_hangar`): job 1 Done, 3 of 3 moves (141.8 s), after four `load_turned` searches ended
  `start_enclosed` at the hangar's freight elevator (start X 11675 … 12175, Y −11700, blocked by the hangar's instanced
  game colliders) and were replanned; job 2 was still moving when the route ended. Both runs identical. Not attributed to
  P5: the failing leg starts in the hangar, whose export is identical before and after this slice (5031 placements, no
  difference), the environment opens its grids through `PlayClipAt` (not the fragment path changed here), and the last
  clean `cargo-c2` (WP-P4, 2026-11-15) predates nine cargo and drone commits (WP-K1 … K4). A run of `cargo-c2` on the
  base commit would settle it; not done.
- The carriage is movable but at rest after load (`LIFT_LIST … at=1 state=Idle`), and the hangar has no lifts.

### Review

Terra (gpt-6.1-terra, xhigh) and Astra (gpt-6.2-astra, high) on `out/lifts/review/{repo,submodule}.diff` (answers in
`out/lifts/review/terra-review.md`, `astra-code-review.md`). Fixed once:

- **The leg check** (both, high): the span test counted only the linked gateway doors. Now every door of the vehicle whose
  origin lies in the leg's path (the box of the carriage's colliders from start to end, the lift's own doors excepted)
  decides: closed blocks, playing waits, open is clear. A true shape sweep was tried and dropped: the carriage colliders
  are trimeshes (no shape sweep), the box sweep hit the shaft's own rear wall above the hold, and a door rig's colliders
  include its fixed parts (the hatch's guide rails run up the shaft), so it blocked hold → top and top → hold. Hull or
  cargo obstacles in a shaft are not detected (low, below).
- **A ground call lost to `Arrived at 2`** (Astra, high): a closed gateway door in the path is opened again once by the
  door deliveries of the queued requests' own links (`LINK_REDELIVER`, `LIFT_GATEWAY`); checked in the game (ground call
  during hold → top: the hatch reopened at the top and the carriage went down, `t_leg` 6.958 s vs 6.957 s).
- **Leaving with a door open** (Astra, high): before a leg the gate and the stop's landing door must be closed (closed
  if needed); a door that stays open blocks it (`reason=door_did_not_close`).
- **A door without a clip strands the lift** (both, high): such a door is not waited for (`played=0`); one stuck open
  blocks the leg and the lift stands open. Test `SCBridge.Lift.Faults`. A landing door without a placed rig is reported
  at build.
- **Sensors reopening lift doors** (Terra, high): proximity sensors skip the doors a lift drives.
- **`CancelStop` during the gateway wait** (both, medium): the chosen leg is dropped (test).
- **`lift_idle` while the hatch still moves** (Astra, medium): gateway doors count.
- **A closing door reversed mid-stroke** (Astra, medium): the doors are not reopened while they close (a new clip
  starts at its far end); a call at the stop is served after.
- **Rider hierarchy** (both, medium): each rider hangs on its own parent (the carriage or another rider), at the named
  socket when the parent is a rig. **Riding lights** (Terra, medium): lights carry `parent_entity_id` (exporter) and are
  re-parented with the carriage (`LIFT_BUILD lights=`; none on the C2).
- **Port dedup** (Terra, medium): a port is dropped only once its `.soc` occupant was placed.
- **Instance interaction-point overrides** (Astra, medium): an empty `Interactions` array removes that point (the hatch's
  point 0); a replacing override is logged.
- **Gateway contract dropped by the reader** (Astra, medium): traversal links, gateway interaction and accessibility are
  read; traversal links are logged as unresolved (`LIFT_TRAVERSAL … reason=item_port_not_exported`, Q4).
- **Clip-owner preference vs aliases** (Astra, medium): the preference is taken among the options that would play.
- **Door timing invented** (Astra, medium): absent or non-numeric values are `null` in the export and reported at build;
  the clip then plays at its own rate.
- **Repeated containers** (Astra, medium): unique anchor names; an id found in several interiors is reported and the
  first is used (panels send in their own interior).

Not fixed: **cooked builds** (Astra, high; the brief's R7): `cook_ships.py` bakes rigs and clears the clip catalogue, and
the lift's assembly is runtime state, so a cooked C2 has a static lift. That needs the lift inputs serialised into the
cook and rebuilt on load: a slice of its own, before a cooked C2 is shown.

Lows (open):

- The leg check counts doors only (by origin in the path box); static geometry or cargo in a shaft is not an obstacle.
- Cancelling the target during `Moving` is not defined (the leg completes); a request made during a leg is served after
  it (no stop at a passed requested stop).
- `SendInteraction(id)` without an interior picks the first interior holding the id (reported when ambiguous).
- An instance override that replaces a point's interactions with entries is not applied (logged).
- `FP_CMD interact` still logs the focused door after a panel press (the `FP_PANEL_PRESS` line is the press).
- The mouse wheel was exercised through `sc.FP.Key MouseScrollDown`, not a physical wheel.

### Not covered

- Hangar platforms, transit gateways (exported, not used by the viewer), the P5-2 lifts (600i, 890J, waypoint and
  teleport nodes, `Request descent from N`, `ElevatorArrived`), audio and effect tags (`OnPhase` fires, nothing listens).
- The Q1–Q3 readings (profile shape, dwell and batching, `timingInTransit`) are unverified against the game.
- The ground call button has no geometry: it is pressed with `sc.Lift.Send` until WP-I1 makes helper-only panels
  pressable (its `helper_transform` is exported).
- A request made during a leg is served after the leg: the carriage does not stop at a requested stop it passes.
- Cooked builds (R7): the ship cook bakes rigs and clears the clip catalogue, and the lift is runtime state, so a cooked
  C2 has a static lift (review, not fixed).

## Result (floor panel)

**State (2026-11-20):** the C2's inner lift panel shows its stops on its screen face, drawn by the viewer: the row under
the crosshair is highlighted, E sends that row's own interaction, the current floor, the called floor and the direction of
travel are marked. The game draws no UI on this panel (WP-I1 "Lift panels: where the UI really is"); the design is ours,
approved by the owner, in the look of the C2's door pads. Exporter on submodule branch `scviewer/panel-screens`
(`8282072b`, review fixes `966be180`), cache format 35. The P5-1 mouse-wheel pick is gone. Nothing committed in this repo.

### What is data, what is ours

| | Source |
|---|---|
| The options | the panel's interactions whose exported links ask a carriage for a stop (`From Elevator call <N>`), one row each; a panel whose links reach no stop (a call button) or two lifts draws nothing |
| Row order | the stops' heights, highest first (on the C2: Top Deck, Lower Deck, Exit Ship) |
| Labels | each row's own panel interaction's display name (on the C2 the stops' names), rendered by the exporter as white glyph masks in the door pads' label font: the pad canvas's large-button label compiled to IR (`BuildingBlocks_FontStyle.BarlowCondensed-Bold`, export symbol `$Billboard` in the shared font library), its letter spacing scaled to the mask's em, cased as the pad's label (capitals) |
| Colours | the pads' brand palette (`BuildingBlocks_Style.S_CRUS_HUD`, the style the chosen pad's canvas rendered with: its IR's `selected_style_source`) by `BB_ColorStyle` role; the label colour is the pad label's own role (`Bright`) |
| State | the lift: `CurrentStop` (none while travelling), `TargetStop`, the request queue, the leg's direction |
| The request E sends | the row's own panel interaction through `ASCVehicle::SendInteraction` (Exit Ship's `RequestSurface` also opens the belly hatch, as its links say) |
| Brightness | the face keeps its `M_SC_Screen` instance (authored glow, viewing angle, `%DBA_GLOBAL`), the image a render target instead of a panel render |
| **Ours** | the layout (rows over the face above a thin bottom bar, square margins 7 % of the shorter side, gaps 3 %, bottom bar 9 %), the role assignment (field `Accent2`, rows `Base`, row under the crosshair `Selected` with its label in `Background`, current floor `Accent1` bar, called floor `Moderate` bar, blinking at 1.5 Hz for the leg in progress, bottom bar `Background` with a `Bright` chevron for the direction), the label size (em 45 % of the row, at most 80 % of its width), the render-target resolution (448 px high), the mask raster em (128 px) |

Without `panel_style` (a package before cache format 35) the panel draws in the engine font, white on black (absence of
data, logged as `style=none(white-on-black)`); no colour is invented. A name the pads' font cannot spell gets no mask and
its row the engine font, cased as the pads' label.

### Exporter (`vendor/StarBreaker`, contract "lifts[].panel_style")

- `starbreaker-3d/src/lift_panel_style.rs`: `stop_display_names` (every (stop, name) the panel links to `From Elevator
  call <N>` offer), `is_door_pad` (a binding whose owner has a `DoorOpenState` state type), `label_style_from_ir` (the
  first active, resolved label inside an active large button of the pad canvas's IR), `style_manufacturer` (the style
  the canvas rendered with), `build` (palette by role, label style, label masks; each part alone), `attach_masks`.
  `decomposed.rs` `lift_panel_styles` compiles the door pads (the lift's own container first) until one has a label and
  writes the masks beside the panel renders (`Data/UI/Generated/<type>/<manufacturer>/<ship>/
  lift_label_<symbol>_<text>.png`).
- `ui_pipeline`: `manufacturer_colour_styles` (the matched `BuildingBlocks_Style` record's `colorStyles` by index, an
  unauthored slot `None`), `shared_ui_fonts`.
- `starbreaker-ui`: `text::render_label_mask` (one line in an imported SWF font, white, alpha = coverage, centred with a
  quarter-em margin; drawn by `draw_swf_font`), `pipeline::load_shared_fonts` (the shared font library the canvas hosts
  merge; its path is now one constant), `style::colour_roles::BB_COLOUR_STYLE_ROLES` (the enum's names in order).
- Tests: `lift_panel_style::tests` (stop names from links, two panels naming one stop, the door-pad rule, the IR label and
  style-source rules, the palette alone, an unspellable name, masks keeping their own paths, and
  `c2_lift_panel_style_from_game_data` on LIVE 4.10.2), `text::label_mask::tests` (3),
  `colour_roles::tests::role_names_are_the_enum_in_order`.

### Viewer (`plugins/SCBridge`)

- `SCFloorPanel.h/.cpp`:
  - `FSCScreenFaceGeometry`: the face's triangles and UV0 (captured from the mesh description while the importer still
    has it: the build drops it; per triangle its node and part, skinned or rigid), under the component's scale (the
    face is handled in the component's frame without scale, so a reflected placement stays unmirrored), the frame an upright, unmirrored image uses (the normal from the
    vertex normals, up from the vehicle's up), and the least-squares affine map UV -> image (`FitError` 0.0068 on the
    C2's rounded plate); the C2 face samples a window of UV space (u 2.387-2.504, v 0.640-0.738), which the map stretches
    over the whole image.
  - `FSCFloorPanelLayout` (rows, `OptionAt`), `FSCFloorPanelView` (what is shown; drawn again on change),
    `FSCFloorPanelColours` (the role assignment above).
  - `ASCVehicle::BuildFloorPanels` (at import, after the screen faces: the panel's largest blank face joins
    `ScreenFaces` and takes a new `M_SC_Screen` instance, the actor's own (`FloorPanelMaterials`), showing a float render
    target (at most 2048 a side), through `SetScreenFaceMaterial`),
    `FloorPanelOptions`, `LiftPanelPressInteraction` (a call button's primary; nothing for a floor-select panel),
    `AimFloorPanel` (the ray against the face's front within reach, nothing that answers a trace in front of it, the
    panel's own housing included; UV -> image -> row), `SetFloorPanelHighlight`, `RefreshFloorPanels` (vehicle tick and character tick),
    `PressFloorPanel`, drawing with `FCanvas` (linear `RTF_RGBA16f`, alpha 1 everywhere: the screen multiplies by it).
- `M_SC_Screen` / `_Translucent` (`create_materials.py`): the image is sampled through `ScreenUVTransformX` / `Y`
  (identity by default; every existing instance unchanged).
- `SetScreenFaceMaterial` drops the face's paint slot: the remembered paint applied at load (`PAINT index=4`) put the
  face's authored instance back over the drawn image (test `SCBridge.FloorPanel.FaceKeepsItsImageOverPaint`, failing
  first).
- `SCFirstPersonCharacter`: each tick the aim sets the highlight (`FP_PANEL_AIM`); E takes the aim again and sends the
  row under the crosshair (`FLOOR_PANEL_PRESS`), off its rows or off its face E does nothing on it (`FP_PANEL_PRESS …
  option=none`), nor on a floor-select panel whose face was not drawn; call buttons send their primary interaction;
  leaving first person clears the highlight. The HUD shows "E: <floor>". The wheel actions
  (`InteractNext` / `InteractPrev`, `CycleInteraction`, `PanelChoice`) are removed.
- `sc.Lift.Panels` (rows with their image rects, the face's material and render-target pixels), `sc.Lift.PanelAim`
  (holds the view on a row or a point of the face, re-aimed each tick a quarter of the way: the body's camera joint
  swings with the view when looking steeply down at the panel); `c2-floor-panel.ps1`; `c2-lift.ps1` uses the rows
  instead of the wheel.
- Logs: `FLOOR_PANEL_BUILD`, `FLOOR_PANEL_VIEW` (on every change but the blink), `FLOOR_PANEL_PRESS`, `FP_PANEL_AIM`.

### Tests

| Suite | Result |
|---|---|
| `cargo test -p starbreaker-ui --lib` | 672 passed, 1 ignored (new: `text::label_mask::tests` 3, `colour_roles::tests::role_names_are_the_enum_in_order`) |
| `cargo test -p starbreaker-3d --lib` | 984 passed, 4 ignored (new: `lift_panel_style::tests` 9, the LIVE one included: on LIVE 4.10.2 the C2's style resolves to `BarlowCondensed-Bold` / `$Billboard`, size 90, letter spacing 3, capitals, colour role `Bright`, palette `S_CRUS_HUD`; masks EXIT SHIP, LOWER DECK, TOP DECK) |
| `cargo test -p scviewer-core` | 52 passed, 2 ignored |
| `Automation RunTests SCBridge` (`out/lifts/floor-panel/fp_tests3.log`) | 172 of 176 pass; the 4 failures are the known `Package.FolderTarImport`, `Player.KitRigAndWeapons`, `Materials.FamilyMapping`, `FirstPerson.SkeletalCollision`. New: `SCBridge.FloorPanel.OptionsAndLayout` (options from the panel's links and the stops, row order, rects, `OptionAt` on rows, gaps, bar and margin), `.AimToOption` (the C2-like UV window mapped upright onto the image; rays at each row's centre -> UV -> that row; beside the face, from behind, beyond reach: no hit; the vehicle's aim highlights the row and nothing off the face), `.PressDispatches` (E on a row queues its stop with the panel's own interaction; an invalid row sends nothing; the view through a leg: no current floor, target, direction, then the new current floor), `.Colours` (the role assignment; white on black without a palette), `.FaceKeepsItsImageOverPaint` (failed before its fix); the review added E on a floor-select panel without a drawn face sending nothing, a row's own display name, and a reflected, stretched face |

### First-person run (C2 in `hangar_xltop_001`, off-screen, port 8771; `viewer/scripts/rpc/c2-floor-panel.ps1`)

Run 7, on the final build (`out/lifts/floor-panel/run7`: shots, `driver.txt`, `FP_run7.log`; run 6 before the review fixes gave the same sequence):

- `FLOOR_PANEL_BUILD panel=4e867e5a… lift=0 face=57 size_cm=27.3x22.9 fit_error=0.0068 target=534x448
  style=BuildingBlocks_Style.S_CRUS_HUD font=BuildingBlocks_FontStyle.BarlowCondensed-Bold rows=2:Top Deck(RequestFloor1)
  1:Lower Deck(RequestFloor0) 0:Exit Ship(RequestSurface)`. The call buttons draw nothing (no stop options).
- **Aim:** with the view held on each row's centre, `sc.FP.Probe` reads `choice=RequestFloor1`, `RequestFloor0`,
  `RequestSurface` and the face shows that row highlighted (`a0…a2-aim-row-*`); above the face and beside it
  `panel=- choice=-` and nothing is highlighted (`b2`, `b3`); E there sends nothing (`FP_CMD interact door=-1 ok=0`; the
  script checks `highlight=-1` and `queue=[]`).
- **E:** each press is `FLOOR_PANEL_PRESS … option=<row> stop=<stop> interaction=<the row's>` and the lift goes there:

  | Row | Request | `LIFT_ARRIVED` |
  |---|---|---|
  | TOP DECK (from the hold) | `RequestFloor1` -> `From Elevator call 2` | stop 2, `t_leg` 4.962 s (predicted 4.956) |
  | LOWER DECK (from the top) | `RequestFloor0` -> `From Elevator call 1` | stop 1, 4.958 s |
  | EXIT SHIP (from the hold) | `RequestSurface` -> `From Elevator call 0` + hatch `Open` (`handled=2`; `WaitingForGateway` 0.1 s) | stop 0 on the pad (`contact=1`), 4.466 s (4.458) |
  | LOWER DECK (from the ground) | `RequestFloor0` | stop 1, 4.461 s |

- **State marks** (`FLOOR_PANEL_VIEW`): on each press `requested=<stop>`, then `target=<stop> dir=±1` with
  `current=-1` while moving, and `current=<stop>` on arrival (cyan bar moved: `c3-top-arrived`, `d3`, `e3`, `f3`); the
  chevron points the way during travel (`c2-top-travel-*`); a call made at a stop shows a steady yellow bar until its leg
  starts (`d1-lower-called`).
- **Legibility:** `g1-arms-length` / `g2` from where the rider stands (the face 33-50 cm from the eye): all three labels
  read.
- Aiming in a script: the body's camera joint swings with the view when looking steeply down at the panel (it is
  pedestal height, about 45 cm in front of the carriage's middle), so a single turn misses; `sc.Lift.PanelAim` re-aims
  each tick a quarter of the way and the script nudges the target up or down until the probe names the row
  (`AimRow`). A held aim at the 9 % status bar settled on the EXIT SHIP row; the bar's "no row" is covered by the test.

**Astra** (gpt-6.2-astra, high; the cargo-bay and habitation references, our habitation pad, rows aimed, off the face,
travel, arrival, arm's length; `out/lifts/floor-panel/run6/astra-floor-panel.md`): style consistency **MATCH** (the
blue field of our door pad, centred condensed capitals, thin dark bottom bar), highlight **MATCH**, state marks
**CLOSE** (the chevron and the cyan current-floor bar clear; the yellow called-floor bar not in the frame given: the
blink's off phase), legibility at arm's length **MATCH**, orientation **MATCH**; no defect; verdict PARTIAL for the
unverified yellow bar. On run 7, with a call shown before its leg (`d1-lower-called`): style **CLOSE** (the face's
brighter blue surround, `Accent2`, against the pads' dark margins), highlight, state marks, legibility and orientation
**MATCH**; verdict **PASS** (`out/lifts/floor-panel/run7/astra-floor-panel-2.md`).

### Review

Terra (gpt-6.1-terra, xhigh) and Astra (gpt-6.2-astra, high) on `out/lifts/floor-panel/review/{repo,submodule}.diff`
(answers in `terra-review.md`, `astra-code-review.md`). Fixed once (exporter `966be180`):

- **A floor-select panel that failed to build pressed its primary interaction** (both, high): a panel whose
  interactions ask a carriage for stops is a floor-select panel whether or not its face was drawn
  (`ASCVehicle::LiftPanelPressInteraction`); without a drawn row under the crosshair E sends nothing on it.
- **E used the last tick's aim** (Astra, high): the aim is taken again at the key press.
- **Colliding label paths** (both, medium): each mask's written path goes to its own label (`attach_masks`, test).
- **Names keyed by stop** (Terra, medium): labels are exported per (stop, name), and a row takes its own interaction's
  display name (tests on both sides).
- **One unusable pad or a missing font discarded the rest** (both, medium): door pads are compiled in order (own
  container first) until one has a large-button label; palette, label style and masks stand alone (tests).
- **Palette from another style than the pad's** (Astra, medium): the palette is the style the chosen pad's canvas
  rendered with (its IR's `selected_style_source`); a canvas-linked style gives none.
- **A name the font cannot spell** (Astra, medium): no mask for it (logged); the row falls back to the engine font,
  cased as the pads' label.
- **Highlight left on when leaving first person** (both): cleared in the character's `EndPlay`.
- **Own housing could not occlude the face** (Astra, medium): any blocking hit in front of the face occludes.
- **Reflected or stretched placements** (Astra, medium): the face is handled in the component's frame without scale,
  its geometry scaled (`FSCScreenFaceGeometry::Scaled`, test with a mirrored, stretched face).
- **A skinned screen took rigid triangles** (Astra, medium): the capture keeps each triangle's part (skinned / rigid)
  and a face selects its own.
- **Render targets held by the shared import** (Astra, medium): the panel's screen instance is the actor's own
  (`FloorPanelMaterials`), not in `Materials`.
- **Unbounded render-target width** (Astra, medium): both sides at most 2048, aspect kept.
- **A skipped draw recorded as drawn** (Astra, low): only a submitted draw updates the view shown.
- The off-face step of `c2-floor-panel.ps1` now fails the run on a highlight or a queued request (Astra).

Not fixed, with the reason:

- **Third-person orbit** (Astra, medium): the floor panel aims from the same origin and view as every E interaction
  (doors, lift panels: `InteractionOrigin`, control rotation); changing it for one is a separate change of the press
  path.
- **Cooked showcase** (Astra, medium): a cooked C2 has no lift logic (R7, the P5-1 review), and the floor panel is
  runtime state drawn from it. The cook copies the face's instance with its transient render target, so a cooked C2's
  face should load without an image (dark glass, as before this change); not verified (no cook run). With R7.
- Deforming skinned screen faces are captured in their bind pose (none on the C2).

### Open

- The face's surround (`Accent2`) is brighter than the pads' dark margins (Astra, minor): a role choice of ours, kept.
- Blinking is not visible in stills; the call before a leg shows steady.
- A scripted aim at a pedestal-height panel close to the rider needs the held, damped turn and a nudge (`AimRow`): the
  body's camera joint swings with the view. A player at the mouse is not affected.
- The cooked C2 (R7) and third-person aiming (above).
- Nothing committed in this repo.

## Result (C2 fixes: the rider caught at the lower deck, 2026-11-20)

**Owner report.** Riding to "Exit Ship" from any floor, the player was caught at the lower deck and fell through only
after wiggling.

**Reproduction** (`out/c2-fixes/repro`, the build before the fix, `sc.Lift.Send <panel> RequestSurface` with the player
teleported onto the carriage at the top deck): from the carriage's centre (Y −784, vehicle cm) the ride reached the ground;
from Y −700 (84 cm toward the landing doors) and from X −204 / Y −764 the player stayed at the hold, feet at Z −796.2, on
`Mesh_3632` = `CRUS_Starlifter_Hold_Shoulder_Ceiling` while the carriage went on to the ground. The catch zone is the half
of the carriage toward the landing doors (Y > about −765).

**Cause (data).** `CRUS_Starlifter_Hold_Shoulder_Ceiling.cgf` (hold_c2, placed at (0, 2, −1.5) m) carries a game box
collider of half-extent 7.5 × 0.3 × 14.25 m whose edge runs along the shaft's centre line (SC x −7.5, the carriage's own
x): it reaches half way into the lift shaft under the hold floor, its top about 1 cm above the carriage's floor at the hold
stop. The carriage passes through it (it is moved, not swept), but the rider was moved by UE's based movement, which sweeps
the capsule: a capsule over that half landed on the slab when the carriage dropped past it, and at the hold stop it stood
on the slab (1 cm higher) rather than on the carriage. In the game the carriage has its own physics grid
(`LocalGrid_Lift_Platform.cgf`, `gridType Small`; §1) whose members it carries.

**Rule implemented** (a reading of that grid membership; the grid geometry is exported as `grid_geometry` but not loaded):
- `ASCVehicle::LiftRiddenBy`: a capsule rides a moving carriage when its movement base is part of the carriage or one of
  the carriage's colliders lies under its feet within the step height + 5 cm (the capsule on the slab 1 cm above the
  carriage floor at the hold is on it; one on a landing beside the shaft is not).
- `ASCVehicle::LiftPathObstacles`: what the leg passes through, once per leg (`LIFT_PATH`): the colliders that block the
  player in the box of the carriage's colliders swept from where it stands to the leg's end, less a 1 cm skin (the surface
  a leg ends on is not passed through), not the carriage and its riders, not door rigs (the leg check decides on doors),
  not pawns. On the C2: top → ground 11 (the hull, the top floor and walls around the shaft, the hold shoulder ceiling …),
  hold → ground 5 (`CRUS_Starlifter_LOD0`, `Hold_Lift_4m`, `Lift_Shaft_Bottom`, `Lift_Wall_Rear_4m`,
  `Hold_Shoulder_Ceiling`).
- The rider's capsule ignores them while it rides (`IgnoreComponentWhenMoving`, rebuilt with the WP-P3 hull ignores in
  `RefreshMoveIgnores`); `sc.Lift.CarryRiders 0` turns the rule off for A/B.
- Tick order: the character's movement now ticks after the character (`PrimaryComponentTick.AddPrerequisite`, with
  `bTickBeforeOwner` off so the pair is not a cycle: no `would form a cycle` warning in `c2fix-run3.log`), which
  already ticks after the vehicle: this frame's move-ignore list and input reach this frame's move (the engine ran the
  movement first; the per-tick trace showed a one-frame lag of `v · dt`).
- Trace: `LIFT_RIDE_TICK` each tick while riding (feet over the carriage anchor, floor distance, base) and `LIFT_RIDE_LEG
  samples rel_z_min_cm rel_z_max_cm spread_cm off_carriage` at the leg's end.

**Result** (`out/c2-fixes/run3`, the final build, `viewer/scripts/rpc/c2-fixes.ps1` steps 7–8, `c2fix-run3.log`; `run2` before the review fixes gave the same): nine legs, top → Exit Ship and
lower deck → Exit Ship from Y −700, X −204 / Y −764 and the centre, and back up: every one arrived with the player on the
carriage (`LIFT_WHERE player base=Mesh_3193 lift=0 … Z=-1393.82` at the ground) and `LIFT_RIDE_LEG … rel_z −785.20 …
−785.20 spread_cm=0.00 off_carriage=0` (452–681 samples per leg). Run 1, before the tick-order change, rode all legs too but
its trace spread 15–74 cm (the lag × frame time, screenshot hitches included).

**Test** `SCBridge.Lift.RiderPassesPath` (synthetic): with the rule off the rider over a slab reaching half way into the shaft
is caught (spread > 1 m, left on the slab — failing first); on, the capsule rides the leg down and back up with the feet's
height over the carriage steady (spread 0.00 cm); `LiftRiddenBy` only while moving and over the carriage; `LiftPathObstacles`
lists the slab, not the ground the leg ends on, a wall outside the path or the carriage's own platform.

Open: the carriage's physics grid volume itself is not loaded (the box of its colliders stands in); walking on the carriage
during a leg near an ignored collider can put the capsule into it (it is pushed out when the leg ends).
