# WP-A3 — Animation UI

**Agent:** Opus 5.5. **Depends on:** WP-A2, WP-R2 (clips inside GLB), WP-R3
(`clip_catalogue` command, add it to `src-tauri` + `commands.ts`).

## Goal
The user picks an entity, sees every animation it can play, and triggers it.

## Deliverables
- **Catalogue panel** fed by `clip_catalogue(entity)`: grouped by owner entity
  (root first), each row = display name, source badge (chrparams/dba/caf),
  duration, Mannequin fragment/tag chips (e.g. `Landing_Gear · Deploy`), boomerang
  marker. Search/filter by name or tag.
- **Transport:** play/pause, scrub (frame-accurate at clip fps), loop / once /
  ping-pong, speed 0.25–2×, "snap first" / "snap last" (whitepaper §14.5), reset to
  bind pose. Use `THREE.AnimationMixer`; map catalogue `id` → `gltf.animations` by
  name (WP-R2 guarantees equality).
- **Compose:** multi-select plays several clips at once (gear + canopy); where two
  clips drive the same node, the most recently started wins (mixer weight 1/0) and
  the UI says so.
- **Skeleton overlay:** toggle `SkeletonHelper` per skin; highlight bones the
  current clip animates.
- Loading: if the GLB in memory was exported without animations, trigger a re-export
  with animations on and show why.
- Keyboard: space = play/pause, ←/→ = step frame, Home/End = snap.

## Done criteria
- Scorpius: landing gear, canopy and wings clips each visibly animate; snap-last of a
  deploy clip matches the "default pose on" export within the ±5° quantisation the
  whitepaper documents.
- Gladius gear + canopy compose without fighting; Cutlass interior door clips are
  listed under their owning entity and animate the right door.
- No animation leaks between loaded models (mixer disposed on reload).
