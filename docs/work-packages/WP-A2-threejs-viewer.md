# WP-A2 — three.js viewer

**Agent:** Opus 5.5. **Depends on:** WP-A1. **Build lock:** not needed (frontend
only) unless a command signature changes.

## Goal
Render the GLB from `preview_geometry` / `export_entity_glb` with textures,
correct orientation and a usable camera; show the node hierarchy.

## Deliverables
- `app/src/viewer/` module: renderer (WebGL2, sRGB output, ACES tone mapping),
  `GLTFLoader` with `KHR_materials_*` and `KHR_lights_punctual` honoured, optional
  `KHR_texture_transform`; neutral studio environment (`RoomEnvironment` PMREM) +
  hemisphere fill; grid + axes; `OrbitControls` with fit-to-bounds on load and on
  double-click of a node.
- Orientation: the GLB already carries the `CryEngine_Z_up` wrapper; verify a ship
  appears nose +X/-Z as in game (document which) and expose a "Z-up/Y-up" toggle
  only if needed.
- Controls panel: LOD (0–3), texture mip (0/2/4), materials (textures/colours/none),
  include attachments / interior / lights / shields, default pose on/off → re-export
  via `export_entity_glb` with a progress bar; cancel button.
- Hierarchy panel: tree of glTF nodes with mesh/skin badges, click = highlight +
  isolate toggle, hover = outline; search filter.
- Inspector: for the selected node show material names, texture count, triangle
  count, bone count; for the model show totals and export options from
  `asset.extras.export_options`.
- Memory hygiene: dispose geometries/materials/textures on reload (upstream's
  `geometry-preview.tsx` shows the pattern); stale-load guard.
- Screenshot button (PNG of the canvas) — also used by WP-Q1.

## Done criteria
- Gladius at LOD 1 / mip 2 renders textured in < 10 s from click; Scorpius and
  Cutlass Black load; a 400-mesh interior does not drop below 30 fps on the 4090.
- No WebGL context loss across 20 consecutive loads (watch heap in devtools).
- `npm run build`, `tsc`, `oxlint` clean.
