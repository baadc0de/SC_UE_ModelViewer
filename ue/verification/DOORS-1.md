# DOORS-1 — ship interior doors, pads and proximity sensors (2026-11-13)

Owner: doors open as in the game — from their wall panel (pad) with the action key, and as the player approaches.

## Cause

The ship export carried a ship's interior doors as frozen geometry: object-container entities with inline geometry lost
their `EntityClassGUID` (the door class carries the animation controller, `SCItem_Door_Anims.adb`, fragment
`SCItemDoor`), `EntityLinks` were never read, door proximity sensors were dropped by a class-name prefix, and a door's
own inline loadout (its `control_panel_a/_b` panels) was ignored.

## Exporter (StarBreaker `scviewer/interior-doors`)

Interior placements keep `entity_class_guid` and gain `entity_id`, `links` (EntityLinks targets),
`loadout_owner_entity_id`, `item_port_name`; each container gains `proximity_sensors[]` (transform in the placement
frame, box volume, `exit_delay`, linked doors), recognised by their `SCItemProximitySensorParams` component. Ids are
unique within one `interiors[]` entry. Contract: `vendor/StarBreaker/docs/decomposed-export-contract.md`.
`cargo test -p starbreaker-3d --lib`: 923 passed. Galaxy: clips 36 -> 64 (14 interior door rigs gained Open/Close
clips), 16 sensors, 8 loadout panels added.

## Viewer

- Pads: a placement whose `links` (or loadout owner) names a door in its container operates that door: E on the pad
  toggles it, the prompt names the door (`ASCVehicle::DoorControls`, `FindInteraction`).
- Sensors: an anchor in the placement frame with the box mapped like mesh vertices; while the first-person player
  stands in it the linked doors open; `exit_delay` after leaving every sensor of a door a sensor opened, it closes
  (`ASCVehicle::UpdateDoorSensors`). A door opened with E stays open.

## Verification

| Check | Result |
|---|---|
| Galaxy import | `DOOR_LINKS panels=80 sensors=16`, `EXPLORATION doors=86` (was 27) |
| Corridor walk (`door2` route) | door opens on approach, closes after passing, reopens on return, closes |
| `sc.FP.Pads` (aim at every pad from 80 cm, four sides) | Galaxy 80/80 resolve to their door; Cutlass 2/2 |
| `test_importer.py` | 39 passed, `UNIT_TESTS_OK` |

Open: the lift-car doors have no pad or sensor link in their containers; hangar (socpak) exports drop `entity_id` when
they promote animated pieces, so links aimed at promoted hangar doors resolve to nothing.
