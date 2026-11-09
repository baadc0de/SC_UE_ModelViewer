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

## Current state and constraints (orchestrator addendum, 2026-11-09, main a6062eb)

What already exists, so build on it instead of re-doing it:
- **Host and app shell (WP-V1):** `USCViewerSubsystem` (`Public/SCViewerSubsystem.h`) owns
  load/unload, `SetCameraMode(ESCCameraMode)` (`FirstPerson` is declared and refused
  today), `FocusComponent`, `FrameAll`; `ASCViewerPawn` is the orbit/free-fly camera;
  `ASCViewerGameMode` / `ASCViewerPlayerController` add the Slate UI
  (`Private/UI/SSCViewerRoot.cpp`, top bar has Orbit / Fly / First person buttons, the
  last disabled). The player's view rect is the gap between the panels. Automation
  switches live in `Private/SCViewerAutomation.cpp` (`-SCLoad`, `-SCCamera`, `-SCFlyView`,
  `-SCHold`, `-SCQuitAfterLoad`, console `sc.*`); headless runs use
  `ue/scripts/ue_load_gladius.py` through `UnrealEditor-Cmd -run=pythonscript -nullrhi`.
- **Static-mesh collision (WP-U1):** every runtime static mesh already gets a Chaos
  trimesh body with `CTF_UseComplexAsSimple` (`Private/SCMeshBuild.cpp:157`), cooked on
  workers during import (`collision_s` in the load report, ~0.11 s Gladius). Components
  get a collision profile only when `Options.bBuildCollision` is set
  (`Private/SCImportJob.cpp:772`). So the brief's "collision cooking with progress" is
  done for static parts; **do not add a second collision cache** (layer-2 caching is
  WP-C1).
- **Skeletal parts have no collision:** `SCImportJob.cpp:742` sets rigs to `NoCollision`.
  This matters: the Gladius hull itself is a skinned mesh (`Rig AEGS_Gladius_LOD1`,
  473k tri), and ramps, doors and landing gear are skeletal too. P1 must give skeletal
  meshes collision that follows the animated pose (UE per-poly collision on
  `USkeletalMeshComponent`, `bEnablePerPolyCollision`, or a generated physics asset
  with per-bone convex/trimesh bodies built from the bind-pose geometry; measure both on
  the Cutlass ramp and pick by correctness, then cost).
- **Doors and clips (WP-U3/R3):** `ASCVehicle` (`Public/SCVehicle.h`) exposes
  `PlayClip`, `SnapClip`, `PlayFragment(Fragment, Tag)`, `ToggleClip`, `ResetPose`,
  `ClipCatalog` (`USCClipCatalog`), `Manifest`. The package's `scene.json`
  `root_entity.animations[]` holds Mannequin fragments with `fragment` name, `frag_tags`
  (`Open`/`Close`/`Retract`…), the animation names and `procedurals[].params.AttachmentJoint`
  (the joint/helper the fragment drives). **Door semantics come from the fragments and
  their tags plus the joint they drive, not from asset file names.** Nearest-door
  selection: fragments whose driven joint is within reach of the player.
- **Seats, entrances and the rest of the hierarchy:** `scene.json children[]` carry
  `entity_name`, `parent_node_name` (the item port / helper), `port_flags`, `ui_bindings`,
  transforms. There is **no item type yet**. Derive seat/door/entrance categories from
  DataCore, not from names: add `item_type` (the DataCore item port type / entity class
  type, e.g. Seat, Door, Turret, Thruster) to `children[]` in the exporter
  (`vendor/StarBreaker` on a new branch `scviewer/item-types` stacked on
  `scviewer/lights`; mirror in `scviewer-core`, bump `CACHE_FORMAT_VERSION`, docs in
  `docs/03-unreal-export-design.md`, tests first). The UE side reads `item_type`; where a
  package lacks it (older cache entries) the first-person mode still works with the
  hull-centre spawn.
- **Scene:** the default `/Game/Maps/Viewer` is a closed 80 m hangar generated by
  `ue/scripts/create_viewer.py` (floor at Z 0, walls, ceiling 40 m); ships spawn
  level on the floor. Gravity is world −Z. Lighting is auto-exposed; a flashlight is
  still useful inside unpowered ships.
- **Lock rules:** you hold the UE build lock (`ue/scripts/build.sh SCViewerEditor`) and
  the cargo lock. Your sandbox has no GPU: run UE with `-nullrhi` (collision traces,
  character movement and spawn-point logic are all testable headless: script a walk with
  `UnrealEditor-Cmd -run=pythonscript` or a `-SC*` switch that spawns the character,
  applies movement for N seconds and logs position, capsule hits and floor contact). The
  orchestrator does the GPU checks and screenshots; name the exact launch commands and
  expected log lines in your report so they can be reproduced.
- **Tests:** add to `ue/scripts/test_importer.py`-style scripts (UE python) or the
  `SCBridge` automation tests (`Private/SCLightTests.cpp` is the pattern) for: spawn point
  derivation per ship, skeletal collision hit on the Cutlass ramp in deployed pose, no
  fall-through along a scripted walk path, door toggle via the fragment path.

Deliverables adjusted accordingly: the collision bullet means skeletal collision plus the
existing static path (UI "collision ready" = import collision phase done, skeletal bodies
built); the HUD may be Slate rather than UMG to match the existing UI; the spawn points
go on `ASCVehicle` as `SpawnPoints` (kind, display name, transform) derived from
`item_type` and the interior containers (`scene.json interiors[]`, floor estimated from
placements' bounds). Verification output, headless, belongs in a new
`ue/verification/WP-P1.md`; leave GPU rows for the orchestrator to fill.
