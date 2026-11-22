# WP-P6 — Seats, ladders and boarding: the player's body sits, climbs and boards like the game's

The cockpit slice uses branch `wp/p6-cockpit-seats`. Exporter changes go on submodule branch `scviewer/seats-ladders`.

**Depends on:** WP-P1 (first person, interactions, spawn kinds `Seat` / `SeatAccess`, `FSCSeatBox`), WP-P2 (player kit,
`USCPlayerBodyComponent`, `FSCPlayerAnimator`, clip sidecars with root motion), WP-P5 (`ASCVehicle::SendInteraction`,
interaction points, several interactions on one point picked with the mouse wheel), DOORS-1 (door fragments, proximity
sensors, `ASCVehicle::PlayDoor`), WP-P3 / WP-P4 (game collision, grid rule, rig collision that follows the pose).
**Works with:** WP-I1 (prompts and selection on a point with several interactions).
**Build locks:** cargo lock (exporter, `scviewer-core`, FFI), UE lock (plugin).

## P6-1 implementation record

The first export confirmed two cockpit seats and two access children in the C2 without loading a hangar or the
player kit. The pilot's authored vehicle item port offers a FlightController priority group. The co-pilot's user
definition omits it. The same XML ties each seat's usable priorities to its dashboard port by target tags. Both
seat attachments, 1.6 m access points, helper transforms and usable exit-validation flags were present.

The live geometry-only C2 export at LOD 2 took 47.75 s. Both cockpit seats resolved seven body and seven look
fragments. The pilot entry has the Right path and PositionAdjust, and the co-pilot entry has the Left path,
ColliderMode and PositionAdjust. The two dashboard clips carry `SeatState` Retract and Deploy. The access points
resolved “Enter Pilot Seat” and “Enter Copilot Seat” from the shared interaction localization.

The clip-only archive exported all eleven body clips for each gender with zero missing, using 3.30 MiB for male
and 3.33 MiB for female. The requests took 9.31 s and 9.86 s on cache misses. A separate request classified both
authored look clips as unsupported aim poses, with zero missing. The UE reader exposes seat metadata through
`sc.Seat.List`.

The runtime cockpit path resolves each access helper, seat attachment and dashboard rig from the imported ports. It
requests the seven authored actions asynchronously and polls clip readiness, body motion completion and the two
vehicle rigs. The player follows the seat socket through Retract, enters Driver or Crew after Select, and returns to
walking after Deploy and Exit. A blocking overlap at the authored exit endpoint sends the seat back to its selected
state. `sc.Seat.State` reports the active role for A2 integration and automated checks.

This implementation covers cockpit routes with SkipDeploy, SkipRetract, no gateways, no flight-state requirements and
a loaded player kit. It uses the authored yaw and pitch limits and the P2 spine-pitch implementation pending aim poses.
The UE 5.9 Win64 Editor build succeeded. Q1's grouped NullRHI runner passed `SCBridge.Mesh.BasisAndAttributes` and
`SCBridge.Seat` (3 tests in **9.66 s**, including launch and exit). These cover helper-only GLBs, wrapped skeletal
root motion, action pose blending and seat package metadata.

The live archive C2 at LOD 2/mip 4 with all interiors took **237.114 s** cold (145.191 s export, 91.824 s import).
It exposed a reader error that skipped both JSON-only access-helper assets. The fixed reader accepted both helpers
during the next C2 run. The pre-extracted geometry-only fixture took **25.897 s** to import and resolved both
authored access prompts plus both seat and dashboard rig pairs. This fixture omitted the bridge floor, so it could
verify route resolution but could not place a standing pawn near the seats.

The follow-up fixture retained the authored `bridge_c2` interior in the extracted LOD 2/mip 4 C2 package. With
a warm shared DDC, it reached scene-ready in **15.34 s** including editor launch. The C2 import itself took **8.084 s**
for 71 assets and 152 placements, and the pre-exported player kit loaded in **1.209 s**. Both original seat and dashboard
rig pairs played Deploy and Retract. The co-pilot and pilot reached Crew and Driver respectively after Select. Both
entries logged zero attachment position error, and both exits restored standing floor contact without crouch,
noclip, a movement blockage or a fall recovery. The process exited successfully after both cycles.

To repeat the smoke, extract the C2 LOD 2/mip 4 export and keep `bridge_c2` in the manifest's `interiors` array.
Launch with `-SCDefaults -SCPackage=<fixture> -SCExplore -SCPlayerPackage=<kit tar>` and
`-SCSeatClipsPackage=<clips tar>`. This bypasses archive export of the ship and body clips. Poll `sc.Seat.State`
for the occupied role after E and `sc.FP.Where` for standing floor contact after the second E.

The blocked-exit case, the capsule-only camera path, weapon and door/lift regressions and the three requested
screenshots remain open.

The Rust checks used `cargo test --manifest-path vendor/StarBreaker/Cargo.toml --locked -p starbreaker-3d --lib seat_`
(3 passed), the same command with `interaction_points_keep_their_interactions_in_order` (1 passed), and
`cargo test --locked -p scviewer-core -p scbridge-ffi --lib player_clip` (1 core and 1 FFI passed).
The live checks used `scviewer-cli decomposed-tar CRUS_Starlifter_C2 out/p6-research/c2-seats-lod2.tar --lod 2 --materials none --no-interior --no-lights`
followed by `scviewer-cli player clips` for the unique body layers of the exported seat fragments.

## Boarding and ladder follow-up

The C1 selects its untagged Constellation Enter and Exit fragments by template and state. The C2 retains its
side-tagged Freelancer fragments. The Buccaneer selects its ladder route on the ground and uses its authored
canopy and ladder gateway delays and Open/Close fragments. It waits for both gates before playing the body
clip. The gates close again after entry and exit. The regular seat action path polls animated seat and dashboard
rigs, and a static seat can use its authored attachment helper.

Win64 first-person fixture runs completed both C1 and both C2 cockpit seats and the Buccaneer pilot seat. Each
reported the expected driver or crew role and returned to a standing floor. The Buccaneer ran its authored
canopy and ladder pairs in both directions. The C1 fixture needed a cockpit-floor waypoint below the roof for
the state-polled smoke test.

The ladder exporter follows the player actor's LadderConfig V2 and resolves the unarmed Mannequin fragments.
A placement can override the type's scalar settings. At runtime, walking toward an authored auto-mount point
starts the corresponding clip once the body and clips are ready. W and S move up and down after mounting.
The loop pose follows physical height in both directions. Top dismount chooses only enabled sides and hands
off to walking at a checked standing position. E returns to the approach if the player needs to
leave the ladder early.

The C2 package exported the ladder's 7.25 m instance height and right-only top exit. In the live C2
fixture, pressing forward while approaching each end triggered the bottom and top mount clips. Ascent
ran for 5.75 s before the authored right dismount, and descent ran for 5.96 s before the bottom dismount.
Both steps ended standing with floor contact and no recovery. This slice uses the unarmed walk animation.
Its movement conversion divides one authored loop's root travel by its configured rung count.

The merged thruster preview was included in a final Win64 build. The C2 ascent and descent passed again.
Pressing E during a third traversal returned to the standing approach with collision active and noclip off.
A comparison with the baseline packages preserved all four dashboard clip source records on each of C1 and C2,
with the same frame counts and source metadata.

The logs and state-polled drivers are local at `out/p6-research/`. Use `sc.Seat.State`,
`sc.Ladder.List`, `sc.Ladder.State` and `sc.FP.Where` to reproduce the transitions on a loaded ship.
The live fixture runs used NullRHI. An independent visual check of the player's contact with individual rungs
remains open.

## Goal

In first person, with the real player body (WP-P2), the player can:

1. **sit in a seat and get up again**, with the game's enter, seated and exit animations, and with the seat and dashboard
   moving as they do in the game. First targets: the C2 Hercules (`CRUS_Starlifter_C2`) pilot and co-pilot seats.
2. **board a single-seat fighter** from the ground and leave it. The canopy opens, the ladder deploys, the body climbs in
   and sits. First target: the Drake Buccaneer (DataCore `EntityClassDefinition.DRAK_Buccaneer`,
   `efec4d48-77ab-4cba-9add-a3d96c5be8d8`).
3. **climb a ladder**: mount at the bottom or the top, climb with hands and feet on the rungs, and dismount. First target:
   the C2 ladder between the hold and the top deck, next to the main lift.

As with doors and lifts, every entity, helper, fragment, clip and number comes from DataCore and the P4k. Where the data
is silent, the viewer's behaviour is a named reading of the data, kept in one function and listed in section 4.

## Read first

`AGENTS.md`; `viewer/README.md` §First-person exploration and §Armed first person; `docs/work-packages/WP-P2-armed-first-person.md`
(player kit, fragment rule, clip sidecars); `docs/work-packages/WP-P5-lifts.md` §4–§6 (interaction points, links,
`SendInteraction`, the brief's style); `vendor/StarBreaker/docs/decomposed-export-contract.md` §Child scene instances,
§Interior containers, §Player kits; `out/seats-ladders-datamine/NOTES.md` (the findings log, with every file, command and
measurement behind this brief).

Evidence: LIVE 4.10.2. DataCore through the prebuilt CLI; Mannequin ADBs, tag and controller definitions extracted with
`--convert cryxml`; clip lengths from the DBA metadata; root motion measured by baking each clip on the default male
(`starbreaker armour export … --body-anim`) and reading the joints. Positions are metres, SC axes (Z up, +Y forward).
Fragment numbers `[n]` are positions in `out/seats-ladders-datamine/*_fragments.txt`.

## How Star Citizen does it, in one paragraph

A **seat** is two items on the vehicle: the `Seat` (`SCItemSeatParams`: which Mannequin templates to use and the bone the
user attaches to) and a `SeatAccess` (`SCItemSeatAccessParams`: interaction points, entrances with their path tag, and
optional gateways such as a canopy or a ladder). Entering plays one Mannequin fragment, `SeatState`, on four contexts at
once: the user's body, the seat, the dashboard and the ship (`SCItemSeatControllerDefs.xml`). The fragment's tags come from
data: the seat's class tag, its transition template (`trans_*`) or control template (`ctrl_*`), the state (`Enter`,
`Retract`, `Select`, …) and the entrance's path tag (`Right`, `Left`, `Ladder`, `ZeroG`). A procedural `PositionAdjust`
pulls the body onto the seat's attachment bone while the enter clip plays. A **ladder** is an entity with
`LadderComponentParams` (height, auto-mount radii, angles, which way to leave at the top). It has no prompt: the player
mounts by walking into it. The climb loop is a clip with speed 0 whose time the game sets from the climber's height
(`LayerManualUpdate LadderFraction`); speeds and stop distances are the Player's `LadderConfig.ActorLadderConfig_V2`.

## 1. Findings: seats (C2 Hercules)

### Entities

| Port | Entity (GUID) | `AttachDef.Type` | Geometry |
|---|---|---|---|
| `hardpoint_seat_pilot` | `CRUS_Starlifter_SCItem_Seat_Pilot` (`0c6bbae0-67ba-4538-82ce-cec3835bc306`) | `Seat` | `Objects/Spaceships/Seats/CRUS/Starlifter/CRUS_Starlifter_Pilot_Seat.cga` |
| `hardpoint_seat_pilot_seat_access` | `CRUS_Starlifter_SCItem_SeatAccess_Pilot` (`9ecff7cf-1496-4904-87f6-34182f326d64`) | `SeatAccess` | `…/CRUS_Starlifter_SeatAccess_Pilot.cga` |
| `hardpoint_seat_copilot` | `CRUS_Starlifter_SCItem_Seat_CoPilot` (`edc004ef-61a3-47f0-b053-d4984989d6fa`) | `Seat` | `…/CRUS_Starlifter_CoPilot_Seat.cga` |
| `hardpoint_seat_copilot_seat_access` | `CRUS_Starlifter_SCItem_SeatAccess_Copilot` (`d187f273-655c-429d-b91e-cd9e9d06cb7c`) | `SeatAccess` | `…/CRUS_Starlifter_SeatAccess_CoPilot.cga` |
| `hardpoint_dashboard_{pilot,copilot}` | `CRUS_Starlifter_SCItem_Dashboard_{Pilot,CoPilot}` | `SeatDashboard` | `…/CRUS_Starlifter_{,Co}Pilot_Dashboard.cga` |

### Seat parameters (`SCItemSeatParams`, pilot / co-pilot)

| Field | Pilot | Co-pilot | Use |
|---|---|---|---|
| `AttachDef.mannequinTags.mannequinClassTag` | `MISC_Freelancer_Pilot_Seat` | `MISC_Freelancer_CoPilot_Seat` | the seat-type Mannequin tag (`SeatTypeTags.xml`); the C2 reuses the Freelancer's |
| `transitionTemplate` | `trans_MISC_freelancer` | same | frag tag of the enter and exit fragments |
| `controlTemplate` | `ctrl_Yoke_nopedals` | `ctrl_DualKeypad_nopedals` | frag tag of the seated fragments |
| `seatType` | `HOTAS_C_L` | same | flight controls (not used here) |
| `actorAttachment` | `Seat`, bone `pilot_sit_pos`, stance `Seated` | `copilot_sit_pos` | where the user's root ends and rides |
| `minYaw/maxYaw`, `minPitch/maxPitch` | ±70°, ±45° (`setYawPitchLimits false`) | same | look limits (Q8) |
| `useAnimationBasedTransition` | true | true | enter and exit are clips |
| `userAnimationDatabase` | `…/MISC_Freelancer_Seat/misc_freelancer_base_scitem_seat_pilot_humananims.adb` (empty) | `…_copilot_…` (empty) | user scope context `SeatUser` |
| `dashboardAnimationDatabase` | `…/scitemdashboard/misc_freelancer_dashboard/misc_freelancer_dashboard_anims.adb` | same | scope context `SeatDashboard` |
| seat's own Mannequin (`SAnimationControllerParams`) | `MISC_Freelancer_Base_SCItem_Seat_Pilot_Anims.adb`, `SCItemSeatControllerDefs.xml` | `…_CoPilot_…` | the seat's clips |
| `AnimationControlActorIKs` | `LftLeg02 → IK_foot_left_pilot`, `RgtLeg02 → IK_foot_right_pilot` | `…_copilot` | foot IK while seated (later slice) |
| `views[0]` | `Camera.Default_Cockpit`: `CameraSeatCockpitConfig`, `attachmentName ""`, `enterExitCamera true`, `blendingToTime 1.0`, `nearPlane 0.02` | same | first-person camera stays at the head |
| `SActorUsableParams` | `UsableArchetype.OperatorSeat`, channel `Operate`, `shouldValidateExitAnimations true` | same | the exit is refused when its end pose does not fit |

### Seat access (`SCItemSeatAccessParams`, `SEntityInteractableParams`)

- Interaction point `$IP_SeatAccess` (helper in the seat-access CGA, under `IP_SeatAccess` at (0.275, −3.444, 1.253)),
  radius 1.6 m, `angleRange 181`, `viewAngleLimit 110`; it highlights bone `seat` of `hardpoint_seat_pilot`.
- Interactions: `SeatEnter` "Enter Pilot Seat" (`@ui_interactor_enter_pilot_seat`; condition preset
  `enterrestriction_operatorseat_traversableinterior_pilot`) and `SeatExit` "Exit". Co-pilot: `CoPilotSeatEnter` "Enter
  Copilot Seat", `CoPilotSeatExit`.
- One entrance, `PilotSeatEnter` (co-pilot `CoPilotSeatEnter`): conditions all `Either`; `seatConnection
  hardpoint_seat_pilot`; `enterState {interaction SeatEnter, fragmentTags Right, skipState SkipDeploy, interiorEntrance true,
  attachment pilot_sit_pos Seated}`; `exitState {SeatExit, Right, SkipRetract}`. Co-pilot: `Left`. No gateways.

### Helpers on the seat (`nmc dump` of the seat CGA, item frame)

| Helper | Position | Role (reading) |
|---|---|---|
| `seat` | (0, −2.613, 0.526) at bind | the moving seat; at bind it stands back for boarding |
| `pilot_sit_pos` (child of `seat`) | (0.015, 0.004, −0.018) from `seat` | the user's attachment: rides the seat |
| `pilot_mount_pos` | (0.611, −1.937, −0.003) | about where the enter clip starts (0.25 m off the clip's own start: `PositionAdjust` absorbs it) |
| `pilot_exit_end_pos` | (0.592, −1.686, −0.017) | about where the exit clip ends |
| `IK_foot_{left,right}_pilot` | | foot IK targets |

The co-pilot seat mirrors x.

### Player animation: where it comes from

1. `Animations/Mannequin/ADB/playerAnims.adb` routes `SeatState` to `Players/Seats/SCItemSeat_Master_HumanAnims.adb`.
2. The master lists 127 `SubADB Tags="<seat type>"` entries, then `Players/Seats/SCItemSeat_SeatState_HumanAnims.adb`
   (1071 fragments). Mannequin merges sub-databases, ANDing each SubADB's tags into its fragments, so per-seat fragments
   need the seat-type tag and the generic file serves every seat. The entries for the C2's class tags
   (`MISC_Freelancer_{Pilot,CoPilot}_Seat` → `MISC_Freelancer_Base_SCItem_Seat_{Pilot,CoPilot}_HumanAnims.adb`) and the
   Buccaneer's (`DRAK_Buccaneer_Seat`) point at empty files (299 bytes): both seats resolve in the generic file. (The
   merge rule is CryEngine Mannequin's; the resolver tests pin it.)
3. That file's `FragDef` / `TagDef` are the seat's (`SCItems/Ships/SCItemSeat/SCItemSeatFragments.xml`,
   `SCItemSeatTags.xml`: seat types, character tags, scope tags; frag-tag groups `ControlTemplate`, `TransitionTemplate`,
   `State`, `Path`, `User`, `idlePose`).
4. The query: tags = the seat's class tag plus the player's own state (never `AI`); frag tags = template + state + path.
   A fragment whose tags include `scope_LookAimPose` is the same state's look layer.
5. Clips: enter and exit in `Animations/Characters/Human/male_v7/spaceships/misc/freelancer_base.dba` (female:
   `female_v2/…`), seated poses as loose CAFs under `male_v7/spaceships/{seats,cockpit_types}/`. Fragments carry no gender
   tags; the body skeleton's `.chrparams` resolves the clip names per gender.

| State | `[n]` | Frag tags | Clip | Length | Root motion | Procedurals |
|---|---|---|---|---|---|---|
| pilot **enter** | [359] | `trans_MISC_freelancer+Enter+Right` | `misc_freelancer_base_pilot_enter_combat_sq_01` | 51 f, 1.70 s | (−0.50, +0.41, +0.46) | `PositionAdjust` (blend 0.6 s), `MovementControlMethod` |
| pilot **exit** | [362] | `…+Exit+Right` | `misc_freelancer_base_pilot_exit_combat_sq_01` | 34 f, 1.13 s | (+0.72, +0.03, −0.47) | — |
| co-pilot enter | [355] | `…+Enter+Left` | `misc_freelancer_base_copilot_enter_combat_sq_01` | 46 f, 1.53 s | (+0.56, +0.13, +0.47) | `ColliderMode Disabled` (1.0 s), `PositionAdjust` (0.3 s) |
| co-pilot exit | [356] | `…+Exit+Left` | `misc_freelancer_base_copilot_exit_combat_sq_01` | 45 f, 1.50 s | (−0.85, −0.13, −0.46) | — |
| look layer | [976] / [846] | `scope_LookAimPose` + the above | `cockpit_standard_lookposes` from 1.1 s / `nw_neutral_stand_lookposes2` from 0.83 s | aim pose | | |
| seat moving | [453] / [451] | `ctrl_Yoke_nopedals+Retract` / `+Deploy` | `pilot_idleseat_nopedals_retractdeploy_sq_01` (loop) | pose | 0 | `EyeStabilization1P 0.5` |
| hands to controls | [454] | `ctrl_Yoke_nopedals+Select` | `pilot_idleseat_nopedals_to_yokewn_controls_sq_01` | 0.77 s | 0 | `EyeStabilization1P 0.5` |
| hands off controls | [450] | `…+Deselect` | `yokewn_controls_to_pilot_idleseat_nopedals_sq_01` | 0.83 s | 0 | `EyeStabilization1P 0.5` |
| **flying idle** | [644] | `ctrl_Yoke_nopedals` | `cockpit_yoke_w_n_idle_sq_01` (loop) + layer 1 `3d-bspace_seat_gforce_reactions_add` | | 0 | `EyeStabilization1P 0.5` |
| co-pilot seated | [460] / [457] / [645] | `ctrl_DualKeypad_nopedals+Select` / `+Deselect` / ∅ | `pilot_idleseat_nopedals_to_dualkeypad_controls_sq_01` / `dualkeypad_controls_sq_01_to_pilot_idleseat_nopedals` / `cockpit_dual_keypad_idle_sq_01` | | | |

In the seated clips the root (`World`) is the attachment, `Hips` sit 0.073 m above it and `HeadCam` is 0.76–0.78 m
above and 0.08–0.13 m behind it. The player's `FragmentBlendList` blends `SeatState → MotionMovement` over 0.5 s.

### Ship side: the seat slides, the dashboard deploys

- The seat's ADB: `SeatState+Retract → Cockpit_Seat_Enter`, `+Deploy → Cockpit_Seat_Exit`, `+Idle` empty; audio
  `Play_SSMP_MISC_Freelancer_Seat_Pilot_Enter_01/02`. In the exported clip the `seat` node slides **2.58 m forward**
  (y −2.613 → −0.034) in 1.83 s of its 70 frames: the seat waits at the rear for boarding and carries the seated player to
  the controls.
- The dashboard (through the seat's `dashboardAnimationDatabase`): `SeatState+Retract → Cockpit_Dashboard_Enter`, `+Deploy →
  Cockpit_Dashboard_Exit` (70 f, 2.33 s).
- The ship's own ADB (`CRUS_Starlifter_Anims.adb`) has no `SeatState`: nothing on the hull moves.

### State flow (reading of the state tags and `skipState`)

The full sequence is `Deploy → Enter → Retract → Select → control idle` to sit and `Deselect → Deploy → Exit → Retract` to
get up. `SkipDeploy` drops the leading Deploy on the way in (the seat is already back) and `SkipRetract` the trailing
Retract on the way out (the seat stays back). Deploy and Retract are the states in which the seat and dashboard clips play
and the user holds the static seated pose. For the C2 pilot this gives:

| Step | User | Seat | Dashboard | Time |
|---|---|---|---|---|
| Enter | enter clip, pulled onto `pilot_sit_pos` (`PositionAdjust` 0.6 s) | at the rear | retracted | 1.70 s |
| Retract | seated pose, attached to `pilot_sit_pos` | slides 2.58 m forward | deploys | 1.83 s (dashboard 2.33 s) |
| Select | hands to the yoke | | | 0.77 s |
| Seated | yoke idle loop | | | — |
| Deselect, Deploy, Exit | hands off; seated pose while the seat slides back; exit clip | slides back | retracts | 0.83 s, 1.83 s, 1.13 s |

### Other C2 seats

The sofa (`Seat_Chair_Bench_2_CRUS_Starlifter_Sofa`, `0e9001ae-504c-4e37-a5b7-c013f17159cf`, `modular_c2`) and the beds
(`Bed_CRUS_Starlifter_Single_Left`) use the generic **usable** system, not `SCItemSeat`:
- `SActorUsableParams` `UsableArchetype.Seat`, alignment slot helper `$anchor_seat`, `fragmentTag standard+backrest+right`
  (`…left` for the second seat), `linkMode FeetLink`, `stance Seated`;
- interaction points `$IP_enter`, `$IP_exit_left`, `$IP_exit_right` (and `_2`) with `Sit` / `Exit` "Get Up";
- the player fragment is `usable` in `Animations/Mannequin/ADB/usable/usables.adb`: `standard+backrest+sit+enter` has three
  options `use_sit050_ntrl_a_enter_{r1,b1,f2}_01` (right, back, front approach), `PositionAdjust` 0.7 s,
  `ColliderMode Disabled`; `standard+sit+exit` → `use_sit050_ntrl_a_exit_f2_01`.

These come after the cockpit seats (P6-4). The booth and desk tables export no interaction points.

## 2. Findings: boarding a single-seat fighter (Drake Buccaneer)

### Entities

- Vehicle `DRAK_Buccaneer` (`efec4d48-…`), vehicle XML `drak_buccaneer.xml` (parts `Cockpit_Canopy_Door` `AnimatedJoint`,
  `hardpoint_seat_access`, `hardpoint_seat_pilot`). Ship Mannequin `DRAK_Buccaneer_Anims.adb` +
  `DRAK_Buccaneer_ControllerDefs.xml` (fragment ids `Landing_Gear`, `SeatDriver`, `Ladder`, `Canopy`; scopes `Ladder` and
  `Canopy` on context `Ship`).
- Loadout: `hardpoint_seat_pilot → DRAK_Buccaneer_SCItem_Seat_Pilot` (`a4de3e11-828d-4d12-a5ea-54ac56d97c7b`),
  `hardpoint_seat_access → DRAK_Buccaneer_SCItem_SeatAccess` (`51559801-c6b2-4874-9b9e-0b80c33230ba`),
  `hardpoint_dashboard_pilot → DRAK_Buccaneer_SCItem_Dashboard_Pilot`.
- Seat: class tag `DRAK_Buccaneer_Seat`, `trans_DRAK_buccaneer`, `ctrl_HOTAS_C_L_pedals`, attachment `pilot_sit_pos` (0,
  0.177, −0.292 in the seat CGA). The seat's own ADB has an empty `SeatState`: the seat does not move.

### The seat access: two gateways and two entrances

| Element | Data |
|---|---|
| gateway `canopy` | open: interaction `CanopyOpen` "Open Pilot Canopy", fragment tags `Open`; close: `CanopyClose`, `Close`; condition `inQuantum False`; `audioCanopyTreatment true` |
| gateway `Ladder` | open: `LadderOpen` "Open Ladder", `Open`; close: `LadderClose`, `Close` |
| entrance `PilotLadderEntrance` | conditions `idle True`, `zeroG False`; enter `SeatEnter` "Enter Ship", frag tags `Ladder`; exit `SeatExit` "Exit seat", `Ladder`; `skipState None`; attachment `pilot_sit_pos`; linked gateways: canopy (open delay enter 0.01 s / exit 0.01 s), Ladder (open delay enter 1.15 s / exit 0.01 s), close delays 0, `dependencyType Never`, `ignoreCloseOnEnter/Exit false` |
| entrance `PilotCanopyEntrance` | `zeroG True`, frag tags `ZeroG`, canopy only |
| `$IP_SeatAccess_Ground` | under `IP_SeatAccess_Ground` (−1.400, 0.711, −0.306), radius 2.25: Enter Ship (primary), Open/Close Ladder, Open/Close Pilot Canopy, Inventory, Loot |
| `$IP_SeatAccess_EVA_Pilot` | (0, 0.096, 1.204), radius 1.6: zero-G enter, canopy |
| `$IP_Exit`, `$IP_Exit_ZeroG` | (−0.505, 0.398, 0.484), (0, 0.584, 0.800), radius 1.6 |

The gateway name is the vehicle's fragment id (`canopy` → `Canopy`, `Ladder` → `Ladder`) and the gateway's fragment tags
are that fragment's tags. Reading (Q5): the seat access requests that fragment on the vehicle's `Ship` context.

### Animations

| Who | Fragment | Clip | Length | Notes |
|---|---|---|---|---|
| player enter | [5846] `trans_DRAK_buccaneer+Enter+Ladder` | `aegs_sabre_pilot_enter_combat_sq_01` (the Sabre's, `sabre.dba`) | 119 f, 3.97 s | root (+0.47, +2.88, +2.00): climbs 2 m and 2.9 m in; `PositionAdjust` 0.6 s; `MovementControlMethod H 3 V 3` |
| player exit | [5920] `…+Exit+Ladder` | `aegs_sabre_pilot_exit_combat_sq_01` | 133 f, 4.43 s | root (−2.81, +0.46, −2.01) |
| zero-G | [5879] / [5926] `…+ZeroG` | `cockpit_pilot_nopedals_{enter,exit}_up_straight_combat_zerog_sq_01` | | `FullRootPriority`, `HeadStabilization1P 0.45` |
| seated | [673] / [572] / [567] / [566], [571] | `cockpit_hotas_c_l_idle_sq_01`, `pilot_idleseat_pedals_to_hotascl_controls_sq_01`, its reverse, `pilot_idleseat_pedals_retractdeploy_sq_01` | | |
| canopy | `Canopy+Open` / `+Close` | `canopy_deploy` 30 f (1.00 s) / `canopy_retract` 35 f (1.17 s) | | audio `Play_Buccaneer_Canopy_Open_01`, particle `large_canopy_open` |
| ladder | `Ladder+Open` / `+Close` | `ladder_deploy` 100 f at speed 2 (1.67 s) / `ladder_retract` at speed 1.5 (2.22 s) | | audio at `Cockpit_Ladder_Door`. The same clips are also on `SeatDriver+Enter/Exit` (Q6) |

So the Buccaneer has no ladder entity: its "ladder" is a ship part animated by a gateway, and the body's climb is one
long enter clip.

### Boarding flow (reading)

1. E on `$IP_SeatAccess_Ground`, "Enter Ship" (conditions: landed and idle, not zero-G).
2. The canopy opens at +0.01 s; the ladder deploys at +1.15 s if it is closed. The enter clip plays from a start pose
   that `PositionAdjust` computes from `pilot_sit_pos` and the clip's root motion: about 2 m below and 2.9 m out from the
   seat.
3. Seated: `Deploy`/`Retract` hold the static pose (no seat motion), then `Select`, then the HOTAS idle loop. Because
   `ignoreCloseOnEnter` is false, the gateways close once the entry completes: canopy closed, ladder retracted.
4. Exit (E, "Exit seat"): Deselect. Canopy and ladder open at +0.01 s, the exit clip runs 4.43 s and ends on the
   ground, then the gateways close.

"Open/Close Ladder" and "Open/Close Pilot Canopy" also work on their own from the ground point. The viewer already toggles
canopy and ladder with E, because they are Open/Close fragment pairs (WP-P1).

## 3. Findings: ladders (C2)

### The ladder entity

| Item | Data |
|---|---|
| Entity | `Ladder-002` (`4678a3af-9c24-7231-5614-74e444b10694`) in `top_deck_c2.socpak`, class `Ladder` (`95935af0-cd56-4cd2-acda-7b0a03a97f7e`) |
| Placement | container (−7, 2.5, −7.875), yaw 180° (`.soc` `Rotate="0,0,0,1"`, CryEngine w,x,y,z) |
| Geometry | `…/Interior/Top_Deck/CRUS_Starlifter_Ladder.cgf`: rails at x ±0.36, rungs in a 0.1 m thick plane, z 0.01–6.76 |
| Rungs | mesh vertex clusters repeat every **0.25 m** (0.24, 0.49, 0.74, …) |
| Instance `LadderComponentParams` | `height 7.25`, `userIgnoreStaticCollisions 1`, `directionAtTopIncludesBack 0`, `directionAtTopIncludesRight 1` |
| Class `LadderComponentParams` | `height 8`, `slideable true`, `autoMountable true`; auto-mount radius walk 1.0 / run 1.25 / sprint 1.5 m at the bottom and the top; `approachAngleAtBottom 60`, `facingAngleAtBottom 45`, `remountDelayAtBottom 1`; `approachAngleAtTop 0`, `facingAngleAtTop 45`, `remountDelayAtTop 1`; top exits Back / Left / Right `true/false/false` (the C2 overrides to Right only); `autoDismountOnHighestMidPointWhenNoTopAvailable false` |
| Usable | `UsableArchetype.Navlink` / `Navlink_UseLadder` for AI (`canPlayerUse false`), `linkMode FeetLink`; `enterParams.holsterWeapon true`; `exitParams.inputExit true`, `exitOnMovement/Jump/DamageTaken false`; view ±45 |
| Interaction | **none** (no `SEntityInteractableParams`): the player mounts by walking into it |
| Span | bottom on the hold deck (z −7.875, the lift's hold stop), top at −0.625 (the lift's top-deck stop is −0.5) |
| Area | box (−1.5, −1.5, −0.5)–(1.5, 1.5, 7.75) around it |

Next to it:
- the top **hatch** `Door_RN_RoomConnector_Template-LadderDoor` (`49463d1a-…`) at (−7, 2.125, −1.031): Open/Close door,
  clips 1.67 s, with its proximity sensor `Door_Ship_Sensor_Proximity_1x1x1` (1 m box, exit delay 0.5 s). DOORS-1 already
  plays it, so it opens as the climber's capsule reaches it;
- brushes `CRUS_Starlifter_Ladder_Top`, `…_Cage`, `…_Tunnel`.

### The player's ladder data (`EntityClassDefinition.Player.ladderConfigV2` → `LadderConfig.ActorLadderConfig_V2`, `b8e73229-9088-4b12-a1c0-52c0f24aa3f6`)

| Group | Fields |
|---|---|
| movement | `movementSpeedUpwards 3.5`, `movementSpeedDownwards 3.25`, `movementSprintUpSpeedScale 1.25`, `movementSprintDownSpeedScale 1.25`, `movementAcceleration 50`, `movementInertiaDecayRate 50`, `movementSettleSpeed 5.5`, `movementQuickSettleSpeedScale 1.6`, slide: `movementSlideFriction 0.1`, `movementSlideMinGravity 2`, `movementSlideSpeedMaxVel 24` |
| animation, per gait (3 entries) | `animationTravelDistance 4`, `mountClimbDistanceFromBottom 0.5`, `stopClimbDistanceFromBottom 0.5`, `stopClimbDistanceFromTopBack 2.0`, `stopClimbDistanceFromTopLeft/Right 1.75` (first) / `2.0` (others); transitions `animationTravelDistanceTrans{WalkToRun 3, RunToWalk 3, RunToSprint 3, SprintToRun 2}` |
| get on | `getOnDistanceAwayBottom 0.1`, `…Top 1.0`, `…TopSide 0.8` (walk 1.15, run / sprint 1.5), `…MiddleSide 1.0`, `…VerticalTopSide 0.1`, `getOnForwardDistanceAwayMiddle 1.0`, **`playerHorizontalOffset 0.35`** |
| look | `horizontalViewLimit 90`, `verticalUpViewLimit 90`, leaning limits yaw / pitch ±90, roll ±45; look-around: `blendBackToClimbingLimitAngle 10`, `…LerpSpeed 45` |
| jump off | forward 1.5–2.5 m/s, up 1.0–2.0 m/s, pitch cutoff 45°, yaw cutoff 90° |

(`ladderConfig`, V1, is older and has other distances; the V2 fragments are the current ones, Q3.)

### Player animation (`Players/playerLadderAnims.adb`)

- Fragment ids (from `playerAnims.adb`): `LadderGetOnV2`, `LadderClimbV2`, `LadderGetOffV2`, `LadderSlideV2`,
  `LadderLookAroundV2`, `LadderDodgeV2` (and the V1 set). Scopes `FullBody3P+LookAimPose`.
- Frag tags (`playerLadderTags.xml`): `foot` (`lfoot rfoot loop`), `direction` (`up down upleft upright downleft downright
  midleft midright midfront fallon jumpon`), `speed` (`walk run sprint trans_*`), `slide` (`slideLoop slideExit
  slideDismount`).
- 224 fragments, with weapon (`pistol`, `stocked`) and `crouch` variants.
- Clips: loose CAFs in `Animations/Characters/Human/male_v7/ladders_v2/` (114 files, 73 used).

| Phase | Fragment (unarmed) | Clip | Length | Root motion | Procedurals |
|---|---|---|---|---|---|
| mount at bottom | `LadderGetOnV2 up+walk` (`+run` at speed 1.3) | `nw_ladders_embark_run_bottom_climbon_rfup` | 0.67 s | (0, +0.41, +0.50) | `PositionAdjust ParamName TargetPos` 0.2 s |
| mount at top | `LadderGetOnV2 lfoot\|rfoot+down+walk` | `nw_ladders_embark_run_top_lfup` | 2.23 s | (0, +1.38, −2.00) | same |
| climb loop | `LadderClimbV2 loop+up+walk` | `nw_ladders_ascend_walk_rfup` (loop, **speed 0**) | 1.00 s | (0, 0, **+1.00**) | **`LayerManualUpdate ParamName LadderFraction`** |
| climb loop | `…loop+up+run`, `…+sprint` | `nw_ladders_ascend_run_rfup` | 0.73 s | (0, 0, +1.00) | same |
| look layer | `LadderClimbV2 scope_LookAimPose` | `nw_neck_only_reduced_lookposes` | aim | | `HeadStabilization1P` |
| dismount at top, straight | `LadderGetOffV2 r/lfoot+up+walk/run` | `nw_ladders_disembark_run_top_{rf,lf}up` | 2.00 s | (+0.04, +1.15, +2.33) | `PositionAdjust TargetPos`; `ActionEvent Yield` at 1.7–1.8 s |
| dismount at top, right | `LadderGetOffV2 r/lfoot+upright+*` | `nw_ladders_disembark_run_topright_{rf,lf}up` | 2.23 / 2.17 s | (+1.06, −0.19, +2.10) / (+1.65, −0.23, +2.13) | same, `Yield` 1.9–2.45 s |
| dismount at bottom | `LadderGetOffV2 down+*` | `nw_ladders_disembark_run_bottom_climbdown_rfup` | 0.63 s | (0, −0.24, −0.50) | `PositionAdjust`, `Yield` 0.5 s |
| slide | `LadderSlideV2 r/lfoot+slideLoop`, `+slideExit`, `slideDismount` | `nw_ladders_slide_into_rfup` → `nw_ladders_slide_idle` (loop), `nw_ladders_slide_stop_rfup`, `nw_ladders_disembark_slide_bottom` | 0.60 / 1.97 / 0.93 / 0.40 s | | `ActionEvent SlideEnterFinished` |
| look behind | `LadderLookAroundV2 r/lfoot` | `nw_ladders_idle_look360_{rf,lf}up` | | | `ManualAnimationControl LayerTimeParam ladderClimb_idle360Time` |

The data agrees with itself:
- one climb cycle is 1.0 m, i.e. four 0.25 m rungs: the hands are 0.45 m apart and move two rungs per pull;
- the bottom mount rises 0.50 m (`mountClimbDistanceFromBottom 0.5`) and the bottom dismount drops 0.50 m
  (`stopClimbDistanceFromBottom 0.5`);
- the top-right dismount rises 2.10–2.13 m (`stopClimbDistanceFromTopRight 2.0`);
- in the loop the hands sit 0.28–0.32 m and the feet 0.19–0.24 m in front of the root, which matches
  `playerHorizontalOffset 0.35` to the rung plane;
- the V2 set has no ladder idle clip: stopping holds the loop at its current fraction;
- there is no descend loop: descending drives the same loop backwards (reading).

### Ladder flow (reading)

1. **Auto-mount**, at the bottom: the capsule comes within the gait's radius (walk 1.0, run 1.25, sprint 1.5 m) of the
   ladder's foot, moving at most 60° off the ladder's approach direction and facing within 45° of it. At the top: radius
   from the top point, facing within 45°. Then the weapon is holstered (`holsterWeapon`).
2. **Get-on** fragment by side (`up` at the bottom, `down` + leading foot at the top) and gait tag. `PositionAdjust` moves
   the body so that the clip ends on the climbing line: `playerHorizontalOffset` from the rung plane, at
   `mountClimbDistanceFromBottom` (bottom) or just under the top.
3. **Climb**: W / S drive a height along the ladder (speed, acceleration and settle from `movementParams`). The loop's
   time is the height's phase within one 1.0 m cycle, so hands and feet stay on the rungs. The gait tag follows input.
4. **Get-off**: climbing up past `height − stopClimbDistanceFromTop{Back|Left|Right}` plays the dismount toward an
   allowed top exit (C2: right only), with the foot tag from the loop's phase. Climbing down to
   `stopClimbDistanceFromBottom` plays the bottom dismount.
5. **Interrupts**: none during get-on; get-off can be cut after its `ActionEvent Yield`. Jump-off (`jumpParams`) and slide
   (`slideable`) come later. Leaving needs input (`inputExit`): moving or taking damage does not drop the player.
   `remountDelay` is 1 s.

## 4. Data versus engine code

| Data (export it, read it) | Engine code (re-implement, name the reading, verify against the game) |
|---|---|
| seat / seat-access / dashboard items, their ports and helpers (`*_sit_pos`, `*_mount_pos`, `*_exit_end_pos`, `$IP_*`) | the state order Deploy → Enter → Retract → Select and its reverse, with `skipState` |
| `SCItemSeatParams` (class tag, templates, attachment bone, limits, views) | how `PositionAdjust` picks its target and warps (we: clip end on the attachment, linear blend over the procedural's blend time) |
| entrances, gateways, linked-gateway delays and flags, interaction names and points | when gateways close after entry and exit; what `dependencyType Never` means |
| `SeatState` fragments of the user, seat and dashboard; ship gateway fragments; clip lengths and root motion | the exit-pose check (`shouldValidateExitAnimations`) |
| `LadderComponentParams` per class and instance; `ActorLadderConfig_V2`; ladder fragments and clips | ladder detection, choice of the get-on and get-off variant, the `LadderFraction` mapping, climb speed integration, top-exit choice |
| procedural parameters (`TargetPos`, `LadderFraction`, `Yield` times, `ColliderMode`, stabilisation) | `MovementControlMethod 3` (CryEngine's enum value 3 is `DecoupledCatchUp`; Q4), head/eye stabilisation, g-force additive spaces |
| the camera record (`attachmentName ""`, near plane) | the seated / ladder view limits (Q8) |

## 5. What the exporter carries today, and what it must add

**Today:**
- Vehicle package (C2, cache 40): the seats, seat accesses and dashboards are children with `item_type` (`Seat`,
  `SeatAccess`, `SeatDashboard`), meshes whose GLBs keep the helper nodes (`pilot_sit_pos`, `$IP_SeatAccess`, …), and
  no interaction points.
- The seat's clips `Cockpit_Seat_Enter` / `Cockpit_Seat_Exit` are exported with fragment `SeatState`, frag tags `Retract` /
  `Deploy`. The dashboard's `Cockpit_Dashboard_Enter/Exit` are exported **without** fragments.
- The ladder is an interior placement (`entity_id`, `entity_class_guid`, transform, mesh) **without** ladder parameters;
  the hatch is a door with its sensor.
- Buccaneer (old cache): canopy and ladder clips with `Canopy`/`Ladder` Open/Close fragments; no seat-access data.
- Player kit: `motion.states[]` hold only `MotionIdle`, `MotionMovement`, `select`, `deselect`, without procedurals.
  Clip sidecars have `root_motion` (start/end of `World`) and every bone track. There are no seat, usable or ladder
  fragments, clips or `LadderConfig`: the P2 resolver admits fragment ids by tag-def schema (`MotionHemisphere` /
  `selectTag`), so these are excluded by construction.

**Additions** (submodule branch `scviewer/seats-ladders`; contract in `vendor/StarBreaker/docs/decomposed-export-contract.md`;
tests first; `scviewer-core` mirror; `CACHE_FORMAT_VERSION + 1`):

1. **Interaction points on loadout children** (generic): `children[].interaction_points[]` in the placement shape, with
   `helper_transform` from the child's NMC (the seat access's `$IP_SeatAccess`, the Buccaneer's four points). Display names
   resolved through `global.ini`, as for placements.
2. **`children[].seat`** for an item with `SCItemSeatParams`:
   - `class_tag`, `transition_template`, `control_template`, `seat_type`;
   - `attachment {type, bone, stance}`, `yaw/pitch limits`, `set_limits`, `animation_based_transition`;
   - `user_scope_context`, `dashboard {adb, scope_context}`, `actor_iks[] {limb, bone}`;
   - `camera` (first view: `{record, config_type, attachment_name, near_plane, enter_exit_camera, blend_time, fov_scale}`);
   - `usable {archetype, validate_exit}`;
   - **`user_fragments[]`**: the player's `SeatState` fragments for this seat, resolved in Rust by the WP-P2 rule through
     `playerAnims.adb` → the seat master's SubADBs (tags ANDed) → the generic SeatState file. One entry per state the
     flow needs: each entrance's `Enter` / `Exit` with its path tag (and `ZeroG` where an entrance has it), the
     control template's `Deploy`, `Retract`, `Select`, `Deselect` and its bare idle, and the `scope_LookAimPose` layer
     of each. An entry is `{state, frag_tags, tags, scopes, layers[] {clip, flags, loop, speed, blend, exit_time},
     procedurals[] {type, params, blend}}`. Query tags: the class tag plus the player's non-AI state tags; record them.
     The entries are gender-agnostic clip names (the fragments carry no gender tags).
3. **`children[].seat_access`** for an item with `SCItemSeatAccessParams`:
   - `gateways[] {name, conditions, open {interaction, fragment_tags}, close {…}, audio_canopy, vehicle_fragment}`;
     `vehicle_fragment` is the vehicle controller's fragment id that matches the gateway name, case-insensitively, or
     null (record the match; Q5);
   - `entrances[] {name, conditions {idle, landed, docked, zero_g, landing_gear_down/up, in_quantum}, seat_port,
     seat_child, dashboard_port, enter {interaction, fragment_tags, skip_state, interior, attachment}, exit {…},
     linked_gateways[] {gateway, sync_to_entrance, one_way, ignore_close_on_enter, ignore_close_on_exit, open {dependency,
     enter_delay, exit_delay}, close {…}}}`.
4. **Dashboard fragments**: bind the dashboard child's clips to the fragments of the seat's `dashboardAnimationDatabase`
   (`SeatState` `Retract` / `Deploy`), like the seat's own.
5. **Ladders**: `placements[].ladder` (and on `entities[]`, and on children for item ladders) for an entity whose class
   has `LadderComponentParams`. Fields: `height`, `slideable`, `auto_mountable`, `auto_mount {bottom, top}
   {walk, run, sprint, offset}`, `approach_angle_{bottom,top}`, `facing_angle_{bottom,top}`, `remount_delay_{bottom,top}`,
   `top_exits {back, left, right}`, `auto_dismount_highest_mid`, `ignore_static_collisions`, with **instance overrides
   applied** (C2: 7.25, right only, ignore 1). Plus the usable summary `{holster_weapon, input_exit, exit_on_movement,
   exit_on_jump, exit_on_damage, view_limits}`.
6. **Player kit:**
   - `player.ladder`: the Player entity's `ladderConfigV2` record verbatim (field names snake-cased), with its record name.
   - **ladder fragments** in `motion.states[]` for every state (unarmed and per weapon tag set × stand / crouch): admit a
     fragment id when its `subTagDef` schema has the `direction` and `foot` groups (by schema, as P2 does, not by name),
     plus their `scope_LookAimPose` layers.
   - **`procedurals[]` on every fragment**, the same shape as in item 2.
   - their clips as sidecars. Check that the `World` track is kept in full: seat and ladder actions move the actor by it.
7. **Player clips by name** (new API): `scviewer-core::export_player_clips(PlayerClipsRequest {gender, clips})`, FFI
   `scbridge_export_player_clips`, CLI `scviewer-cli player clips <out.tar> --gender <g> <name>…`. It resolves the names
   through the body skeleton's `.chrparams` search roots (DBA entries and loose CAFs, as P2's `clips.rs` does) and writes
   sidecars in the existing format. D10 cache key: gender, the sorted names, P4k identity. The viewer collects the names
   from a vehicle's `seat.user_fragments` and asks for them when first person starts in that vehicle. This keeps body
   clips bound to the body skeleton and out of the vehicle package.
8. Docs: the contract sections above and `docs/03-unreal-export-design.md` (seats, seat accesses, ladders, player-clip
   API).

## 6. Viewer design

### Model

All runtime-module, no editor dependency. Three new data types, built at import and owned by `ASCVehicle` like
`FSCLift` (WP-P5):
- `FSCSeat` (`Public/SCSeat.h`, `Private/SCSeat.cpp`): seat child, its rig and `attachment` socket, the dashboard child,
  `user_fragments`, the seat's own `SeatState` clips, the camera and the limits;
- `FSCSeatAccess`: interaction points (world helper frames), entrances (each resolved to its `FSCSeat`), gateways
  (each resolved to a vehicle fragment on the hull rig, or to nothing);
- `FSCLadder` (`Public/SCLadder.h`): the bottom point, the up axis, the top point (`height`), the climbing normal, the
  rung plane, parameters and the placement's mesh component.

`FSCSpawnKind::Seat` / `SeatAccess` keep working: the seated teleport (`FSCSeatBox`) stays for the capsule-only mode and
for the teleport menu.

### Interaction: E, as with doors and lifts

- **Seat access**: E acts on a seat access's point when the crosshair or the player is inside the point's `radius` and
  its angle constraint holds. The prompt shows the point's interactions (display names from the export). Several
  interactions on one point (the Buccaneer's ground point) use WP-P5 / WP-I1's mouse-wheel selection; Inventory and Loot
  are listed but disabled. Seated: E is the entrance's exit interaction ("Exit" / "Exit seat").
- **Ladder**: auto-mount per the data (radius, angles, `remountDelay`). E on the ladder mesh or inside the auto-mount
  radius mounts as well; it is a viewer convenience, documented as such. A setting `LadderAutoMount` (default on, as in
  the game) turns the automatic mount off.
- Logs: every press logs `FP_INTERACT seat|ladder …` like doors.

### Player modes (`ASCFirstPersonCharacter`)

`Walking → SeatEntering → Seated → SeatExiting → Walking` and `Walking → LadderMounting → Climbing → LadderDismounting →
Walking`. In every non-walking mode the capsule leaves `MOVE_Walking`: `MOVE_None` while seated and in transitions,
`MOVE_Custom` (`ESCCustomMove::Ladder`) while climbing. Collision is off during transitions (`ColliderMode Disabled`,
`FeetLink`). Input is ignored mid-transition (as the P2 weapon state machine). Weapon: before a ladder mount the P2
deselect plays (`holsterWeapon true`). Before a seat it also plays: reading, Q7.

### Animator additions (`FSCPlayerAnimator`, plain C++, unit-tested)

- **Action one-shots** that replace locomotion (not overlays) and **apply root motion to the actor**: the root joint is
  pinned as today, and its per-frame delta (from the `World` track) moves the actor.
- **Warp** (`PositionAdjust`): at the start, compute the transform the clip must start from so that it ends on the
  target (target ∘ inverse(clip root delta)), and blend the actor from where it stands to that start over the
  procedural's blend time. Targets: seat enter → the seat rig's attachment socket at clip end; ladder → the climbing line
  point (`TargetPos`).
- **Held loops** (seated poses, seated idles) and **manual-time layers** (`LayerManualUpdate`: the animator exposes
  `SetLayerParam("LadderFraction", f)`).
- **Look layers**: the fragments' `scope_LookAimPose` aim poses are not evaluated (P2 has no aim poses either); keep the
  P2 interim spine pitch, clamped by the seat's or ladder's view limits. The g-force additive 3-D blend space is skipped
  and logged once (`PLAYER_KIT_DIAGNOSTIC`).

### Seat flow

1. **SeatEntering**:
   - optional Deploy (seat clip at `Deploy`, unless `skip_state` says so);
   - the user's `Enter` fragment with the warp onto the attachment, while the dashboard and seat stay as they are;
   - at clip end, attach the actor to the seat rig's attachment socket (`AttachToComponent`, keep world).
2. **Retract**: the seat's and the dashboard's `Retract` clips (`PlayFragmentAt(SeatState, Retract, rig)`); the user
   holds the seated pose and rides the socket. **Select**, then **Seated** with the control idle.
3. **SeatExiting** is the reverse:
   - Deselect, Deploy (seat back);
   - **validate**: a standing capsule must fit at the transform the exit clip ends on, else `SEAT_EXIT_BLOCKED` and the
     player stays seated;
   - detach and play `Exit` with root motion; at clip end return to `MOVE_Walking` and blend to locomotion over 0.5 s
     (`FragmentBlendList`).
4. **Gateways** (Buccaneer): at enter or exit start, each linked gateway's open fragment plays on the hull rig after its
   delay (`PlayDoor` on that rig, so the door state stays consistent with E on the canopy). Close after the transition
   unless `ignore_close_on_*`.
5. **Camera**: `USCEyeCameraComponent` stays at `HeadCam` (WP-P2), which in the seated clips sits 0.78 m above the
   attachment. Control rotation is clamped to the seat's yaw and pitch limits relative to the seat frame while seated.
   The near plane is the camera record's 0.02 m (equal to P2's).

### Ladder flow

`MOVE_Custom` climbing keeps the capsule on the climbing line (`playerHorizontalOffset` from the rung plane, facing the
ladder). Static collision is ignored when `ignore_static_collisions` is set (the C2's: the cage and the deck opening).
The capsule stays a query shape, so the hatch's proximity sensor still fires. The rest is section 3's flow: height `h`,
velocity from W / S with the `movementParams` (units Q1), loop time `= frac((h − h_mount) / cycle_rise)`, where
`cycle_rise` comes from the loop clip's root motion (1.0 m), not from a constant. Get-off at the stop distances; top exit
from `top_exits`; foot tag from the loop phase (first half `rfoot`, second `lfoot`; reading). The get-on and get-off
variants are chosen by frag tags from the exported fragment list, never by clip name.

### Without the player body

If the kit is not ready or failed (WP-P2 fallback), the same state machine runs with the capsule: the camera moves along
the clip's `HeadCam` track when the clip is available, else jumps to the seated eye (attachment + the seated idle's
`HeadCam` offset) after the seat clip. Ladders move the capsule on the same line and speed.

### Console, RPC and logs

| Command | Effect |
|---|---|
| `sc.Seat.List` | `SEAT_LIST id= child= port= class_tag= trans= ctrl= bone= entrances=<name:path:skip,…> fragments=<n>` |
| `sc.Seat.Enter <seat> [entrance]` / `sc.Seat.Exit` | as E on the access point / seated E |
| `sc.Seat.Gateway <access> <name> open\|close` | a gateway alone |
| `sc.Ladder.List` | `LADDER_LIST id= entity= bottom_z_cm= top_z_cm= height_cm= exits= auto_mount=` |
| `sc.Ladder.Mount <id> bottom\|top` | mount from the nearest standing spot |
| `sc.Player.Action` | the running action, its clip, time, warp error |

Logs:
- `SEAT_STATE seat= from= to= clip= t=`;
- `SEAT_ATTACH seat= socket= err_cm=` (root to socket after the warp);
- `SEAT_RIG seat= clip= travel_cm=`;
- `SEAT_EXIT_BLOCKED seat= at=`;
- `LADDER_STATE ladder= from= to= h_cm= v= gait= frag=`;
- `LADDER_PHASE ladder= h_cm= fraction=` once a second while climbing.

RPC (WP-H1): `/wait until=seated|walking|climbing`; `seat` and `ladder` records in `SCState.ndjson`.

## 7. First slice — P6-1 "C2 cockpit seats"

**Exporter** (`scviewer/seats-ladders`, tests first in `starbreaker-3d`):

- additions 1, 2, 3, 4 and 7 of section 5, plus `procedurals[]` on fragments (from addition 6);
- resolver tests on the extracted ADBs reduced to synthetic documents:
  - the C2 pilot resolves [359] / [362] / [453] / [451] / [454] / [450] / [644] and their LookAimPose layers;
  - the co-pilot resolves `Left` and `ctrl_DualKeypad_nopedals`;
  - `AI` fragments never match;
  - a per-seat SubADB fragment needs its seat tag;
  - the Buccaneer seat falls through its empty SubADB to [5846] / [5920];
- contract tests: the C2 export has two `seat` and two `seat_access` children, `$IP_SeatAccess` with radius 1.6 and
  "Enter Pilot Seat", and dashboard clips bound to `SeatState` `Retract` / `Deploy`;
- `scviewer-cli player clips` returns the 11 C2 seat clips (four enter/exit, the shared seated pose, two selects, two
  deselects, two control idles) for male and female; the aim poses (`*.aim`) are listed as unsupported, not failed.

**Viewer:**
- `FSCPackageReader` fields; `FSCSeat`, `FSCSeatAccess`;
- the animator's action, warp and held-loop features (unit tests: warp end error < 1 mm on a synthetic clip; the root
  delta is applied once; input is ignored mid-action);
- seat modes; E on access points; exit validation; console, logs, RPC;
- automation tests `SCBridge.Seat.*` (synthetic package: state order with and without skip states; attach and detach;
  blocked exit).

**Done means** (`viewer/verification/WP-P6.md`; route `viewer/scripts/routes/c2-cockpit-seats.txt`; `-SCLoad=CRUS_Starlifter_C2
-SCCamera=first`):

1. `SEAT_LIST` shows two seats:
   - pilot: `class_tag=MISC_Freelancer_Pilot_Seat trans=trans_MISC_freelancer ctrl=ctrl_Yoke_nopedals bone=pilot_sit_pos
     entrances=PilotSeatEnter:Right:SkipDeploy`;
   - co-pilot: `…CoPilot… ctrl=ctrl_DualKeypad_nopedals bone=copilot_sit_pos …:Left:…`.
2. Walking up to the pilot seat's access point shows "Enter Pilot Seat" within 1.6 m. E:
   - `SEAT_STATE … to=Entering clip=misc_freelancer_base_pilot_enter_combat_sq_01` for 1.70 s ± 1 frame, then
     `SEAT_ATTACH err_cm` < 2;
   - `Retract`: `SEAT_RIG clip=Cockpit_Seat_Enter travel_cm` 258 ± 1, the dashboard plays `Cockpit_Dashboard_Enter`;
   - the camera stays within 3 cm of the `HeadCam` joint; then `Select` (0.77 s) and `Seated` with the yoke idle.
3. Seated, the view is clamped to ±70° yaw and ±45° pitch. E: Deselect, Deploy (the seat returns 258 cm), then the exit
   clip (1.13 s). The player stands on the cockpit floor (`FP_GRID inside=1`, floor contact, no `stuck`, no `recover`),
   within 30 cm of `pilot_exit_end_pos`.
4. The same for the co-pilot (`Left`, 1.53 s / 1.50 s).
5. A crate placed on the exit spot (`sc.Cargo.Spawn` there) gives `SEAT_EXIT_BLOCKED`, and the player stays seated.
6. Armed (P4-AR drawn): E first plays the deselect, then enters (Q7 decides whether to keep this).
7. Without the kit (`-SCPlayerPackage=` a missing file): enter and exit run with the capsule camera; no crash.
8. Regressions: `cutlass-ramp-to-cockpit`, `c2-ramp-walk-in`, `c2-lift`, the P2 player demo are unchanged; Seat and
   SeatAccess spawns still teleport.
9. Rust suites green, `UNIT_TESTS_OK`, `SCBridge.Seat.*` pass. Screenshots: mid-enter (third person, `C`), seated after
   the slide, the first-person view at the controls.

## 8. Later slices

- **P6-2 C2 ladder.**
  - Exporter additions 5 and 6 (ladder parameters, `player.ladder`, ladder fragments and clips);
  - `FSCLadder`, `MOVE_Custom` climbing, auto-mount and E, get-on and get-off at both ends, the hatch by its sensor.
  - Done: from the hold, walk into the ladder (`LADDER_STATE … to=Mounting frag=up+walk`) and climb to the top deck,
    leaving to the right (`frag=…upright…`).
    - feet and hands on rungs: in third-person captures the hand joints stay within 3 cm of a rung during the loop;
    - total time matches the reading Q1 settles (≈ 8.3 s if rungs, ≈ 4.3 s if metres);
    - the same down from the top (`down` get-on, bottom get-off).
- **P6-3 Buccaneer boarding.**
  - The ground point with five interactions; gateways through the hull rig; the long enter and exit clips with warp;
  - closing after entry; the zero-G entrance listed but disabled (no zero-G in the viewer).
  - Done: from the hangar floor, "Enter Ship" opens the canopy, deploys the ladder, climbs in (3.97 s) and closes both;
    "Exit seat" reverses it and ends on the floor.
- **P6-4 Usable seats.** The usable system (sofa, beds): `SActorUsableParams` alignment slots (`$anchor_*`,
  `fragmentTag`, `linkMode`, `stance`), the `usable` fragment family, option choice by approach direction (`r1` / `b1` /
  `f2`: pick the option whose clip start is nearest to the player; reading). Done: sit on and get up from the C2 sofa from
  both enter points.
- **P6-5 Family sweep.**
  - Other cockpit seats (Cutlass, Gladius, Avenger: `Canopy` path tags, `AEGS_Gladius_Seat` per-seat SubADB with
    `Exit+Canopy` overrides), turret seats (`trans_turret_*`);
  - ladder-type seat accesses (`DRAK_Vulture_SCItem_SeatAccess_Ladder`, `ORIG_300i_SeatAccess_Ladder`, `MISC_Fortune_…`,
    which use `LadderAccessClimbParams`); side-door entrances (`Entrance_Side_Door_Player`);
  - seated foot IK (`AnimationControlActorIKs`); ladder slide, jump-off and look-around; mid-ladder mounts (`mid*` frag
    tags).

## 9. Risks

- **R1 Mannequin selection.** The ADB rule (tags ⊆ state, first match in merged order) was written for P2's motion. Seat
  queries add SubADB tag merging and scope-tag layers. A wrong pick shows as the wrong clip, which the resolver tests
  catch. Keep the resolver in Rust, with every pick and its source index in the export.
- **R2 Warp and root motion.** `PositionAdjust` semantics are code. A wrong reading puts the body beside the seat or makes
  it slide. Measure `SEAT_ATTACH err_cm` and the warp distance (C2 pilot: ≈ 0.25 m, from the helpers) and log them.
- **R3 Moving attachment.** The seated player rides a socket on a rig that the C1 cook may bake, and the WP-P4 refit
  must move the seat's collider. Keep seat rigs unbaked (as lift parts in WP-P5 R7). Collision is off while seated.
- **R4 Exit into geometry.** The exit clips were authored for the Freelancer's cockpit; the C2's differs. The exit check
  guards against a capsule inside a wall; the warp does not exist for exits. If the C2 exit ends inside the console, the
  check refuses it: report it, do not move the target by hand.
- **R5 Ladder units** (Q1) change the climb speed by 4×. Isolate the conversion in one function with both readings
  named, and decide with a capture.
- **R6 Ladder line and side.** Which side of the ladder plane is the climbing side is not in the data, so it is a reading
  (Q2). Measure the free side with a capsule sweep from the bottom point and log the choice.
- **R7 Clip volume.** Seat clips per vehicle are few (11 for the C2); ladder clips are ~40 for unarmed + crouch + two
  weapon families. Cache them like the kit; the kit grows by about that.
- **R8 Comfort.** Body clips move `HeadCam` during transitions (3.97 s climbs). The game stabilises with
  `HeadStabilization1P` / `EyeStabilization1P`. If captures show discomfort, damp `HeadCam` rotation with the procedural's
  strength (0.45–0.5) and record it as a reading.

## 10. Open questions

1. **Ladder units.** Are `movementSpeedUpwards 3.5` and `animationTravelDistance 4` metres (3.5 m/s, a 4 m animation
   distance) or rungs (0.875 m/s, 4 rungs = the clip's 1.0 m)? The rung reading is consistent with the clips (run and
   sprint loops both play at ≈0.63× their authored rate); the metre reading plays the walk loop 3.5× too fast. A
   stopwatch on the C2 ladder decides: ≈ 8.3 s bottom to top-deck exit (rungs) or ≈ 4.3 s (metres).
   **Working assumption (owner, 2026-11-20): rungs** (0.875 m/s up, ≈ 8.3 s on the C2), consistent with the clips; the
   implementation logs the assumption and a stopwatch on the game can still overturn it.
2. **Ladder side and frame.** Which local axis of the `Ladder` entity faces the climber? Settle it with the geometry (the
   cage's open side) and the top exit (`right` relative to the climber).
3. **V1 or V2.** The Player has both `ladderConfig` and `ladderConfigV2`, and the ADB has both fragment sets. We assume V2
   (its clips are the `ladders_v2` set); confirm in a capture (the V1 loop clips `ladders_*_ascend_singlecycle` look
   different).
4. **`PositionAdjust` and `MovementControlMethod`.** Empty `ParamName` on seats (target = the attachment, reading) and
   `TargetPos` on ladders (target from code). What do `Horizontal 3 / Vertical 3` mean in SC's enum?
5. **Gateway binding.** Does a seat-access gateway request the vehicle fragment of the same name (`canopy` → `Canopy`)?
   What do `enterSequenceDelay 1.15` on the ladder (on enter) and `dependencyType Never` do: does the climb wait for the
   ladder clip?
6. **`SeatDriver+Enter/Exit`** on the Buccaneer's hull plays the ladder clips too. Is it legacy or still requested?
7. **Weapons and seats.** Does the game holster before an operator seat, or refuse entry while armed? The ladder's
   usable says holster (`holsterWeapon true`); the seat's usable does not say.
8. **Seated look limits.** `setYawPitchLimits false` with ±70° / ±45°: are the limits applied? The aim-pose layer
   (`cockpit_standard_lookposes`) suggests a head-only look with its own range.
9. **`pilot_mount_pos` / `pilot_exit_end_pos`.** Used by AI routing, by the exit check, or as warp targets? Their
   distance to the clip ends (≈ 0.25 m) says they are not exact clip anchors.
10. **Gateways on exit.** After "Exit seat" on the Buccaneer, do canopy and ladder close by themselves or stay open?

## Verification commands used for this brief

```
target/debug/starbreaker.exe p4k list > out/seats-ladders-datamine/p4k_list.txt
target/debug/starbreaker.exe p4k extract -o out/seats-ladders-datamine/archive --regex '<see NOTES.md §5>' --convert cryxml
target/debug/starbreaker.exe dcb query EntityClassDefinition --dcb out/audio-datamine/dcb_raw/Data/Game2.dcb --filter '*Seat*'      # and '*Ladder*', '*Buccaneer*'
target/debug/scviewer-cli.exe record EntityClassDefinition.Player
target/debug/scviewer-cli.exe record b8e73229-9088-4b12-a1c0-52c0f24aa3f6    # LadderConfig.ActorLadderConfig_V2
target/debug/scviewer-cli.exe record camera.default_cockpit
target/debug/starbreaker.exe nmc dump --p4k <Data.p4k> 'Data\Objects\Spaceships\Seats\CRUS\Starlifter\CRUS_Starlifter_Pilot_Seat.cga'
target/debug/starbreaker.exe nmc dump --p4k <Data.p4k> 'Data\Objects\Spaceships\Ships\DRAK\Buccaneer\Exterior\DRAK_Buccaneer_SCItem_SeatAccess.cga'
target/debug/starbreaker.exe armour export <tmp.glb> --gender male --kind glb \
  --loadout 'Data\Scripts\Loadouts\Player\DefaultLoadouts\Mannequin_Undersuit.xml' \
  --body-anim 'Data\Animations\Characters\Human\male_v7\spaceships\misc\freelancer_base.dba' \
  --body-anim-clip misc_freelancer_base_pilot_enter_combat_sq_01                 # then tools/glbanim.py
python -I out/seats-ladders-datamine/tools/adb.py <adb> [--has <tag>]         # fragment tables
python -I out/seats-ladders-datamine/tools/dbameta.py <dba>                    # clip lengths
```
