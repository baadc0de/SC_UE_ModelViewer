# WP-U1 — `SCBridge` plugin: package reader + static meshes

**Agent:** Codex. **Depends on:** WP-U0; a decomposed export from WP-R1
(`scviewer decomposed Gladius out/dec`). **Build lock:** no cargo.

## Goal
`ue/Plugins/SCBridge` imports a StarBreaker decomposed package into the project as
Nanite static meshes placed in a generated actor, with correct units and axes.
Materials are a placeholder master in this WP (WP-U2 replaces it); skeletal parts
are imported as static for now (WP-U3 replaces that).

## Read first
`docs/03-unreal-export-design.md`, `vendor/StarBreaker/docs/decomposed-export-contract.md`
(scene manifest, child instances, `local_transform_sc` with `gltf_y_up` basis,
interiors/placements, path rules).

## Deliverables
- Modules: `SCBridge` (runtime: `ASCVehicle`, `USCClipCatalog` stubs),
  `SCBridgeEditor` (importer, Slate/EUW entry "Import SC package…").
- `FSCPackageReader`: parses `scene.json`, `palettes.json`, `*.materials.json` into
  UStructs; validates version `1` / `export_kind = Decomposed`; path normalisation.
- `FSCMeshImporter`: for each distinct `mesh_asset` (`Data/**/*.glb`), build a
  `UStaticMesh`. Prototype **both** Interchange (`UInterchangeManager::ImportAsset` with
  a custom pipeline that sets Nanite) and a direct `FMeshDescription` builder from the
  GLB (tiny GLB parser or `cgltf` as ThirdParty). Keep the one that preserves normals,
  tangents, UV0/UV1, vertex colours and lets us set `NaniteSettings.bEnabled = true`,
  `FallbackRelativeError = 1.0` before build. Record the decision in
  `docs/03-unreal-export-design.md`.
- Units/axes: glTF metres Y-up RH → UE cm Z-up LH. Validate on the Gladius against
  a reference measurement (ship length from DataCore `vehicleDefinition` or the
  GLB bounds): the hull must be ~16–20 m long, nose +X, up +Z.
- Asset layout: `/Game/SC/<Entity>/Meshes/...` mirroring `Data/` paths;
  `/Game/SC/<Entity>/BP_<Entity>` actor Blueprint whose component tree follows
  `scene.json` children (`local_transform_sc`) and `interiors[].placements`.
  Repeated `mesh_asset` paths share one `UStaticMesh`.
- Collision: simple box per mesh; complex off.
- Logging: one line per mesh with triangle count, Nanite accepted or the reason
  Nanite was disabled (e.g. translucent material, degenerate mesh).

## Done criteria
- Import `out/dec` (Gladius) → actor placed in `LumenTest` shows a textured-grey ship
  with every attachment positioned (thrusters, gear, seats), Nanite visualisation
  shows clusters on the hull, Lumen GI reacts to an emissive test material.
- Re-import is idempotent (assets updated, no duplicates).
- Headless import via Python script works (for WP-Q1).
