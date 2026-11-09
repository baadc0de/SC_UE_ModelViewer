# WP-U0 — Unreal project, renderer smoke test, standalone build-path spike

**Agent:** Codex. **Depends on:** nothing. **Build lock:** UE lock; no cargo.

## Facts
Unreal Engine **5.8.2** (`++UE5+Release-5.8`, CL 56702186) at `~/UnrealEngine/5.8`;
binaries in `Engine/Binaries/Linux/` (`UnrealEditor`, `UnrealEditor-Cmd`). GPU RTX
4090, Vulkan. `/` is ~95 % full — keep `DerivedDataCache/`, `Intermediate/`,
`Saved/`, `Binaries/` bounded and never commit them.

## Goal
The project `ue/SCViewer` exists, renders with Nanite + Lumen + HWRT on Vulkan, and
**the standalone build path is decided** (plan D3): can a game-mode window build
Nanite meshes and cook complex collision at runtime, and if so from which target?

## Tasks
1. `ue/README.md`: engine version, path, `df -h` headroom. `.claude/launch.json`
   entries `ue-editor` (`UnrealEditor ue/SCViewer/SCViewer.uproject`) and
   `ue-game` (`UnrealEditor ue/SCViewer/SCViewer.uproject -game -windowed -resx=1600 -resy=900`).
2. Create `ue/SCViewer/SCViewer.uproject` (C++; targets `SCViewer` Game and
   `SCViewerEditor` Editor) with plugins `Interchange`, `InterchangeEditor`,
   `GeometryScripting`, `PythonScriptPlugin`, `EditorScriptingUtilities`. Settings:
   Nanite on, Lumen GI + reflections, HWRT on, VSM, Vulkan SM6 default RHI. Create the
   empty plugin skeleton `ue/Plugins/SCBridge` with modules `SCBridge` (Runtime),
   `SCBridgeEditor` (Editor), `SCBridgeLib` (ThirdParty, empty for now).
3. Map `Content/Maps/Viewer`: sky light, directional light, neutral floor with
   collision, Nanite cubes, emissive sphere. Verify Nanite visualisation and Lumen
   scene view; record Vulkan warnings.
4. **Spike A — runtime Nanite in a game target.** In the `SCViewer` Game target
   (Development config) add a console command `sc.SpikeBuildMesh` that creates a
   transient `UStaticMesh` from a procedurally generated `FMeshDescription`
   (≥ 100 k triangles), enables Nanite (`NaniteSettings.bEnabled`), builds it
   (`BuildFromMeshDescriptions` or the editor-path `Build()` behind `WITH_EDITOR`),
   and places it. Try linking `NaniteBuilder`, `MeshBuilder`, `MeshDescription`,
   `StaticMeshDescription` from the Runtime module (`.Build.cs`, Development only).
   Record: compiles? Nanite clusters visible in the `-game` window? build time?
5. **Spike B — Editor target with `-game`.** Same command run via
   `UnrealEditor SCViewer.uproject -game`. Record the same facts. This path is the
   guaranteed fallback.
6. **Spike C — runtime complex collision.** For the spike mesh, create a `UBodySetup`
   with `CollisionTraceFlag = CTF_UseComplexAsSimple`, add the triangles, call
   `CreatePhysicsMeshes()` at runtime, and verify a line trace hits it in both hosts.
   Record cost for 100 k triangles. If runtime cooking is unavailable, test
   collision from the Nanite fallback mesh.
7. **Spike D — processed-data round-trip.** In both hosts, serialise the spike mesh's
   `FStaticMeshRenderData` (with Nanite resources) and its `UBodySetup` cooked data
   through `FMemoryWriter`, write to a file, restart the app, read back with
   `FMemoryReader` into a fresh transient `UStaticMesh`, render it and line-trace it
   without calling any build/cook. Record whether this works per host and the
   load time; if not, test `UPackage::SavePackage` to a mounted content root in the
   `-game` host as the fallback (plan D10).
8. **Interchange baseline:** `scripts/starbreaker.sh entity export Gladius out/gladius.glb`,
   import through Interchange in the editor; note which features survive (tangents,
   UV1, vertex colours, lights, skins). Note the resulting orientation and size for
   the axis-matrix unit test in WP-U1.
9. Check Nanite skeletal mesh status in 5.8 (experimental? flag?) and record it.

## Done criteria
- `ue/README.md` has a **Decision** section: chosen standalone path (A or B), with
  compile results, Nanite-at-runtime yes/no per host, collision-at-runtime yes/no,
  render-data round-trip yes/no per host, timings, and the exact launch command. `docs/02-implementation-plan.md` D3 and
  `docs/03-unreal-export-design.md` updated to say which path won.
- `UnrealEditor-Cmd ue/SCViewer/SCViewer.uproject -run=pythonscript -script=<x>` works
  headless (needed by WP-Q1).
- `ue/SCViewer` and `ue/Plugins/SCBridge` committed without build products.
