# WP-S1 — Standalone deliverable

**Agent:** Codex. **Depends on:** WP-U0 Decision (standalone build path), WP-V1
(app shell). **Build lock:** UE lock.

## Goal
A user runs one command, gets a window with the model list, picks `Data.p4k` if it
was not auto-discovered, loads a ship, and never sees the Unreal Editor.

## Deliverables
- Build per the chosen path: either the `SCViewer` Development game target binary
  (`Binaries/Linux/SCViewer`) or the `SCViewerEditor` target launched with `-game`.
  `scripts/build_viewer.sh` builds Rust FFI (`scripts/build_ffi.sh`) + the UE
  target(s); `scripts/run_viewer.sh` launches with the right flags (`-game -windowed`
  or the game binary), `-log` optional, and passes `--p4k <path>` through to the app.
- Startup flow in the Runtime module: `USCViewerSettings` (p4k path, last options,
  window size) persisted under `Saved/Config`; auto-discovery via `sc_open(NULL)`;
  a native file dialog (`IDesktopPlatform`) when discovery fails; clear error dialogs
  for a missing/old archive, missing `libscbridge.so`, or insufficient disk.
- No editor UI reachable: the `-game` host must not show editor toolbars; `Esc`
  opens the in-app menu, not the editor. Console (`~`) stays available in
  Development.
- Settings screen: cache location, cap and Clear cache (WP-C1), p4k path, options
  defaults.
- Cold start to the populated model list < 20 s (archive mmap + DataCore load
  through FFI, measured and shown in the footer).
- `ue/README.md` "Running the viewer" section: prerequisites (engine path, built
  `.so`), the two commands, known limitations (Development config only, no Shipping
  packaging; first Nanite build of a ship takes N s).

## Done criteria
- From a fresh shell: `scripts/run_viewer.sh` → window → search "gladius" → Load →
  Gladius under Lumen, in under a minute total, with no editor window at any point.
- Killing and restarting the app remembers the p4k path and last options, and
  loading the same ship again is a full cache hit (no processing phases).
- Launching without a reachable `Data.p4k` shows the picker, not a crash.
