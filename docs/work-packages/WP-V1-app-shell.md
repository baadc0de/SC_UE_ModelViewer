# WP-V1 — App shell and model browser (game-mode UI)

**Agent:** Opus 5.5. **Depends on:** WP-U1 (runtime importer, `FSCPackageSource`),
WP-R4 (`libscbridge.so`). **Build lock:** UE lock.

## Goal
The user starts the app (standalone host chosen in WP-U0, or PIE in the editor),
sees the model list, searches, picks options, clicks Load, and the ship appears in
the Lumen `Viewer` level with an orbit camera. All UI is in the **Runtime** module
(UMG or Slate), so it is identical in every host.

## Read first
`docs/03-unreal-export-design.md` (§Hosts, §How a view happens, §In-game UI, §FFI),
`ue/Plugins/SCBridge/Source/ThirdParty/SCBridgeLib/include/scbridge.h`,
`ue/README.md` Decision section.

## Deliverables
- `SCBridgeLib` third-party module (`.Build.cs`: runtime dependency on the `.so`,
  public include path); `FSCBridgeClient` RAII wrapper: `Open`, `Search`,
  `ClipCatalogue`, `ExportDecomposed` on `FAsyncTask`/`UE::Tasks` with progress
  marshalled to the game thread; errors as `FText`.
- `ASCViewerGameMode` + `USCViewerSubsystem` (`UGameInstanceSubsystem`): owns the FFI
  client, current `ASCVehicle`, load/unload lifecycle, camera mode switching;
  `UFUNCTION(BlueprintCallable)` `LoadEntity(Name, Options)`, `Unload()`,
  `SetCameraMode(Orbit|Fly|FirstPerson)` (first person implemented by WP-P1).
- Startup screen: archive status from `sc_open(NULL)`; if none, a native file picker
  (`IDesktopPlatform::OpenFileDialog`); path remembered in `USCViewerSettings`.
- Model browser widget: debounced search → virtualised list (display name, class,
  category), recents; options panel bound to `USCViewOptions` (LOD, mip,
  attachments, interior, lights, shields, animations, default pose, collision);
  Load button, progress bar (export → mesh build → collision), cancel.
- Hierarchy panel: component tree with badges (static/skeletal/light, Nanite yes/no,
  triangles); click focuses the orbit camera; isolate toggle.
- Orbit camera (fit to bounds on load, double-click focus) and free-fly camera.
- Footer: timings (export, build, collision, total), memory, `df` headroom.
- Reuse: same entity + options hash → skip export/import, reuse objects.
- Editor host extra (in `SCBridgeEditor`): a Keep button that saves the transient
  objects as assets under `/Game/SC/<Entity>/`.

## Done criteria
- In the standalone host: cold load of Gladius (LOD 1, mip 2) < 30 s; Scorpius and
  Cutlass Black load; progress moves through all phases; loading a second ship
  frees the first (object counts).
- Headless: `UnrealEditor-Cmd ue/SCViewer/SCViewer.uproject -run=pythonscript
  -script=scripts/ue_load_gladius.py` drives `LoadEntity` (used by WP-Q1).
- No UI hard-codes an entity or asset name.

## Implementation notes and deviations (2026-11-09, Opus 5.5)

- UI is Slate (not UMG) in the Runtime module; the local player's view rect is
  restricted to the gap between the panels so the scene is centred and framed correctly.
- Async import: the importer's `TODO(WP-V1)` is resolved by `FSCImportJob` (phase
  progress, cancel, workers for parse/derive/collision); `FSCImporter::Import` wraps it.
  Static-mesh builds are still one game-thread step per mesh (parallel async compile
  is a follow-up, see `ue/verification/WP-V1.md`).
- Settings live in `GameUserSettings.ini` (UE 5.8 does not save arbitrary `Game.ini` sections).
- The headless entry creates its own `UGameInstance` (`USCViewerSubsystem::CreateHeadless`)
  because commandlets have none; the script is `ue/scripts/ue_load_gladius.py`.
- Keep saves textures, material instances (as constants), Nanite static meshes,
  skeletons, clips and the clip catalogue; runtime skeletal meshes are skipped (no
  editor source model; `TODO(WP-U3)`).
- The archive build branch is not shown: the FFI has no call for it (`TODO(WP-R4)`).
