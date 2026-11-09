# WP-V2 — Animation panel (game-mode UI)

**Agent:** Opus 5.5. **Depends on:** WP-V1 (app shell, subsystem), WP-U3
(`USCClipCatalog`, `ASCVehicle::PlayClip/SnapClip/PlayFragment`, skeletal parts).
**Build lock:** UE build lock.

## Goal
The user sees every animation the viewed entity can play and triggers it in the
Lumen viewport.

## Deliverables
- Animation panel widget (Runtime module), fed by the loaded `USCClipCatalog`:
  rows grouped by owner entity (root first) with display name, source badge
  (chrparams / dba / caf), duration, Mannequin fragment + tag chips
  (`Landing_Gear · Deploy`), boomerang marker; text filter on name or tag.
- Transport: play/pause, scrub slider frame-accurate at clip fps, loop / once /
  ping-pong, speed 0.25–2×, snap first / snap last (whitepaper §14.5), reset to bind
  pose. Drives `ASCVehicle` through its Blueprint-callable API in the game world
  (standalone or PIE). Door clips must also be triggerable from WP-P1's interact key,
  so expose `ToggleClip(id)` on the actor, not only on the widget.
- Compose: multi-select plays several clips together (gear + canopy); when two clips
  drive the same bone the most recently started wins and the row shows a conflict
  marker.
- Bone overlay toggle: draws the skeleton (debug lines or `USkeletalMeshComponent`
  bone drawing) and highlights bones the active clip animates.
- Keyboard: space play/pause, ←/→ step frame, Home/End snap.
- Fragment mode: a second list of Mannequin fragments → `PlayFragment(fragment, tag)`.

## Done criteria
- Scorpius: gear, canopy and wing clips animate under Lumen; snap-last of a deploy
  clip matches the default-pose import within 5°.
- Gladius gear + canopy compose; Cutlass Black door clips are listed under their
  owning entity and move the right door.
- Viewing a new entity resets the panel; no clip state leaks between actors.
