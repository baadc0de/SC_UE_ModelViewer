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

`AGENTS.md`; `ue/README.md` §First-person exploration (interactions are Mannequin fragments with an Open/Close tag pair,
`sc.FP.Door`, `FP_DOOR_LIST`); `ue/verification/DOORS-1.md`; `docs/work-packages/WP-P4-moving-collision.md`;
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

**Done means** (`ue/verification/WP-P5.md`; route `ue/scripts/routes/c2-lift.txt`; `-SCLoad=CRUS_Starlifter_C2
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
