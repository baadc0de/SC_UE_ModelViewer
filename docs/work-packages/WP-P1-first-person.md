# WP-P1 — First-person exploration with collision

**Agent:** Codex. **Depends on:** WP-U1 (meshes + per-mesh collision setup), WP-U3
(door clips, `ASCVehicle` play API), WP-V1 (UI shell, camera modes). **Build lock:**
UE lock.

## Goal
Walk around and inside a loaded ship as a first-person character that collides with
the imported geometry — hull, ramps, corridors, interior props — and can open doors.

## Read first
`docs/03-unreal-export-design.md` §Player modes and §Object mapping (collision row),
`ue/README.md` Decision section (runtime collision findings from WP-U0).

## Deliverables
- Collision: every imported static mesh gets complex collision per the WP-U0 finding
  (runtime Chaos trimesh from the full triangles, else Nanite fallback-mesh collision);
  skeletal parts get per-bone simple or complex collision so landing-gear and ramps
  collide in their animated pose. Cooked data cached on disk by mesh hash under the
  app cache dir with a size cap. Collision cooking runs on a worker thread with
  progress; the UI shows "collision ready".
- `ASCFirstPersonCharacter` (`ACharacter`): capsule 34 × 88 cm, crouch 44, walk
  400 cm/s, run 650, jump, step height 45, gravity −Z (ship spawned level); mouse
  look; interact key (E) triggers the nearest door clip within 3 m (clips whose
  Mannequin fragment or name indicates door/open/close on the owning child entity;
  toggle open↔close by state); flashlight (F) spot on camera; noclip/fly toggle (V)
  switches to flying movement with collision off; teleport menu (T).
- Spawn points: derived at import (`ASCVehicle::SpawnPoints`) from seat entities,
  entrance/door helpers and interior container floors; plus "spawn where I look"
  from orbit mode. The first-person mode starts at the best spawn (entrance if any,
  else pilot seat, else above the hull centre).
- HUD (UMG): crosshair, interact prompt with the door's display name, mode and speed,
  position relative to the ship origin, "collision cooking…" progress.
- Safety: falling below the floor teleports back to the last spawn; stuck detection
  (no movement with input for 2 s inside geometry) offers noclip.

## Done criteria
- Cutlass Black: spawn at the rear ramp, walk up the ramp, open the interior door via
  E, walk the corridor and cockpit without falling through or clipping into walls;
  crouch under a low pass.
- Gladius: walk on the hull, climb the ladder region via jump, sit-height check at
  the pilot seat spawn.
- Collision cook for Cutlass < 20 s cold, < 2 s cached; frame time with collision on
  unchanged from orbit mode within 10 %.
- No per-ship hard-coding: spawn and door logic use categories (seat type, door
  entity category, helper names from NMC/port flags), never ship or asset names.
