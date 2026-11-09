# WP-U0 — Unreal test project and renderer smoke test

**Agent:** Codex. **Depends on:** nothing. **Build lock:** no cargo involved.

## Facts
Unreal Engine **5.8.2** (`++UE5+Release-5.8`, CL 56702186) is installed at
`~/UnrealEngine/5.8`; editor binaries in `Engine/Binaries/Linux/` (`UnrealEditor`,
`UnrealEditor-Cmd`). GPU: RTX 4090. Disk: `/` is ~95 % full — keep the project's
`DerivedDataCache/`, `Intermediate/` and `Saved/` bounded and never commit them.

## Goal
A C++ test project `ue/SCTest/SCTest.uproject` where Nanite, Lumen and hardware ray
tracing are verified on Vulkan, plus a first look at what Interchange keeps from a
StarBreaker GLB.

## Tasks
1. Record engine version, path and `df -h` headroom in `ue/README.md`. Add
   `.claude/launch.json` entry `ue-editor` →
   `~/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor ue/SCTest/SCTest.uproject`.
2. Create `ue/SCTest/SCTest.uproject` (C++ project so plugins build) with plugins
   `Interchange`, `InterchangeEditor`, `GeometryScripting`, `PythonScriptPlugin`,
   `EditorScriptingUtilities` enabled. Project settings: Nanite on, Lumen GI +
   reflections, HWRT on, virtual shadow maps, Vulkan SM6 default RHI.
3. Level `Content/Maps/LumenTest`: sky light, directional light, a few Nanite cubes,
   an emissive sphere. Confirm with `stat GPU` / Nanite visualisation that Lumen and
   Nanite are active; note any Vulkan warnings.
4. Smoke import: `scripts/starbreaker.sh entity export Gladius out/gladius.glb` and
   import the GLB through Interchange in the editor UI; record in `ue/README.md`
   which features survive (tangents, UV1, vertex colour, lights, skins, animation
   count once WP-R2 lands). This informs WP-U1's builder decision.
5. Check whether Nanite skeletal meshes are still experimental in 5.8 and which
   project setting enables them; record it.

## Done criteria
- `UnrealEditor-Cmd ue/SCTest/SCTest.uproject -run=pythonscript -script=<x>` works
  headless (needed by WP-Q1).
- `ue/README.md` has version, paths, RHI, headroom, Interchange findings.
- `ue/SCTest` committed without `Binaries/ Intermediate/ Saved/ DerivedDataCache/`.
