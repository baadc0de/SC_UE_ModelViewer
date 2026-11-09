# WP-U0 — Unreal Engine on this machine

**Agent:** Codex. **Depends on:** nothing. **Build lock:** no cargo involved.
**Caution:** asks the user before any download > 1 GB or install outside `~`.

## Goal
A working UE 5.6+ Linux editor with a test project where Nanite, Lumen and
hardware ray tracing are verified on the RTX 4090 (Vulkan RHI).

## Tasks
1. Check free space (`df -h ~ /`): the Linux editor is ~40 GB unpacked. `/` has
   ~84 GB free; if another volume has more room, propose it. Do not proceed under
   30 GB headroom after install.
2. Obtain the newest Linux prebuilt editor from Epic's GitHub releases
   (`EpicGames/UnrealEngine` → Releases → `Linux_Unreal_Engine_5.x.y.zip`; requires
   the user's GitHub account to be linked to Epic — if the release page is 404,
   stop and tell the user exactly what to link). Unpack to `~/UnrealEngine/5.x`.
3. Record the exact version and the editor path in `ue/README.md`; add
   `.claude/launch.json` entry `ue-editor` that runs
   `UnrealEditor ue/SCTest/SCTest.uproject`.
4. Create `ue/SCTest/SCTest.uproject` (C++ project so plugins build), enable plugins
   `Interchange`, `InterchangeEditor`, `GeometryScripting`, `PythonScriptPlugin`,
   `EditorScriptingUtilities`. Project settings: Nanite on, Lumen GI + reflections,
   HWRT on, virtual shadow maps, Vulkan SM6 as default RHI, DirectX none.
5. Level `Content/Maps/LumenTest`: sky light, directional light, a few Nanite cubes
   and an emissive sphere. Confirm in `stat GPU` that Lumen and Nanite are active.
6. Smoke import: `starbreaker entity export Gladius out/gladius.glb` and import the
   GLB through Interchange in the editor UI; note which mesh/material features
   survive (tangents, UV1, vertex colour, lights) in `ue/README.md` — this informs
   WP-U1's builder decision.

## Done criteria
- Editor launches headless: `UnrealEditor-Cmd SCTest.uproject -run=pythonscript -script=...`
  works (needed by WP-Q1).
- `ue/README.md` has version, paths, RHI, and the Interchange smoke findings.
- `ue/SCTest` committed without `Binaries/ Intermediate/ Saved/ DerivedDataCache/`.
