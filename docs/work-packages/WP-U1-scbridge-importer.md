# WP-U1 — `SCBridge` runtime importer core: package reader + Nanite static meshes

**Agent:** Codex. **Depends on:** WP-U0 **Decision** (standalone path, runtime
collision findings); a decomposed export from WP-R1
(`scviewer decomposed Gladius out/dec`). **Build lock:** UE build lock; no cargo.

## Goal
The `SCBridge` **Runtime** module turns a StarBreaker decomposed package into
transient UE objects at runtime — Nanite static meshes with complex collision, placed
in an `ASCVehicle` actor with correct units and axes — in whichever host WP-U0 chose
(game target or editor `-game`). Nothing here may depend on editor-only modules
unless the WP-U0 decision was the editor `-game` path, and even then keep editor
dependencies behind `WITH_EDITOR` so the game-target path stays possible.
Materials are a placeholder master in this WP (WP-U2 replaces it); skeletal parts
are imported as static for now (WP-U3 replaces that). The in-game UI is WP-V1; this
WP exposes `FSCImporter::Import(FSCPackageSource&, FSCImportOptions, FSCImportResult&)`
and a minimal menu entry "Import SC package folder…" for testing.

## Read first
`docs/03-unreal-export-design.md`, `vendor/StarBreaker/docs/decomposed-export-contract.md`
(scene manifest, child instances, `local_transform_sc` with `gltf_y_up` basis,
interiors/placements, path rules).

## Deliverables
- Modules: `SCBridge` (runtime: importer, `ASCVehicle`, `USCClipCatalog` stubs),
  `SCBridgeEditor` (optional host: "Import SC package folder…" menu entry for
  testing, later "Keep as assets").
- `FSCPackageSource`: abstract read-only view of a package (`Exists(path)`,
  `ReadBytes(path)`, `ListFiles(prefix)`) with two implementations from day one —
  a disk folder and an in-memory uncompressed tar (`FSCTarPackageSource`; the tar
  layout is defined in WP-R4). Importers only ever see `FSCPackageSource`.
- `FSCPackageReader`: parses `scene.json`, `palettes.json`, `*.materials.json` into
  UStructs; validates version `1` / `export_kind = Decomposed`; path normalisation.
- `FSCMeshImporter`: for each distinct `mesh_asset` (`Data/**/*.glb`), build a
  transient `UStaticMesh` (outer = a per-load `UPackage` or the actor) on the path
  WP-U0 validated. Prototype **both** Interchange (editor host only, as a baseline) (`UInterchangeManager::ImportAsset` with
  a custom pipeline that sets Nanite) and a direct `FMeshDescription` builder from the
  GLB (tiny GLB parser or `cgltf` as ThirdParty). Keep the one that preserves normals,
  tangents, UV0/UV1, vertex colours and lets us set `NaniteSettings.bEnabled = true`,
  `FallbackRelativeError = 1.0` before build. Record the decision in
  `docs/03-unreal-export-design.md`.
- Units/axes: glTF metres Y-up RH → UE cm Z-up LH. Validate on the Gladius against
  a reference measurement (ship length from DataCore `vehicleDefinition` or the
  GLB bounds): the hull must be ~16–20 m long, nose +X, up +Z.
- Object naming: `/Temp/SC/<session>/<Data path>` for transient objects (the editor
  host's Keep later renames into `/Game/SC/<Entity>/...`); `ASCVehicle` built in C++
  with a component tree following `scene.json` children (`local_transform_sc`) and
  `interiors[].placements`. Repeated `mesh_asset` paths share one `UStaticMesh`.
  Loading another entity destroys the previous actor and its objects (GC verified).
- Collision: per WP-U0 finding — complex trimesh cooked at runtime
  (`CTF_UseComplexAsSimple`) or Nanite fallback-mesh collision; cooking on a worker
  thread; expose `bBuildCollision` in `FSCImportOptions` (WP-P1 adds caching).
- **Lumen / ray-tracing representation (WP-U0 finding):** a mesh built with
  `BuildFromMeshDescriptions` has no mesh cards, distance field or RT geometry and
  shows as a black cut-out in the Lumen overview. Build them explicitly (host B links
  `MeshCardBuild`/`MeshUtilities`; or use the editor `Build()` path) and set
  `bSupportRayTracing`; verify with `ShowFlag.VisualizeLumen 1, r.Lumen.Visualize 1`
  that the ship is present in "Lumen Scene" and "Reflection View, HWRT". Expose
  the cost per mesh in the import log.
- Cache hooks (WP-C1 fills them): every builder is split into *derive* (pure data:
  mesh description, cooked collision, texture platform data) and *instantiate*
  (UObject creation), with a content hash per input so WP-C1 can store and replay
  derived data without touching the builders' logic.
- Logging: one line per mesh with triangle count, Nanite accepted or the reason
  Nanite was disabled (e.g. translucent material, degenerate mesh).

## Done criteria
- In the chosen standalone host: import `out/dec` (Gladius) → actor in the `Viewer`
  level shows a textured-grey ship
  with every attachment positioned (thrusters, gear, seats), Nanite visualisation
  shows clusters on the hull, Lumen GI reacts to an emissive test material, and the
  Lumen overview shows the hull in the Lumen Scene and HWRT reflection panels (no
  black cut-out).
- Loading Gladius twice in a row leaves no leaked UObjects (`obj list class=StaticMesh`
  count returns to baseline after GC); a line trace hits the hull.
- Headless import via Python script works (for WP-Q1).
- A unit test imports the same package from the folder and from a tar of that
  folder and gets identical asset counts.
