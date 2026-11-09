# WP-U4 — Direct import: `scbridge-ffi` + editor tab

**Agent:** Codex. **Depends on:** WP-R1–R3, WP-U1–U3. **Build lock:** required for
the Rust side.

## Goal
Import straight from `Data.p4k` inside the Unreal editor: search an entity, pick
LOD/mip/options, import — no intermediate folder.

## Deliverables
- `crates/scbridge-ffi`: `cdylib` + `cbindgen` header `scbridge.h`. Functions:
  `sc_open(const char* path_or_null, sc_handle**)`, `sc_search_entities(h, q, limit,
  char** json)`, `sc_clip_catalogue(h, entity, char** json)`,
  `sc_export_decomposed_mem(h, entity, const sc_options*, sc_blob* out_tar)` (the
  decomposed package as an uncompressed tar in memory, same contract as on disk),
  `sc_last_error(h)`, `sc_free_blob`, `sc_free_string`, `sc_close`. All entry points
  `catch_unwind`; UTF-8 strings; no allocation ownership crosses without a matching
  free. Progress via an optional C callback.
- `ue/Plugins/SCBridge/Source/ThirdParty/SCBridgeLib`: links `libscbridge.so`
  (built by `scripts/build_ffi.sh` into `ue/Plugins/SCBridge/Binaries/ThirdParty/Linux/`).
- Editor tab "Import from Star Citizen" (Slate or Editor Utility Widget): p4k path
  (auto), search box with results, options (LOD, mip, attachments, interior, lights,
  animations, Nanite skeletal), Import button → in-memory tar → the WP-U1/U2/U3
  importers via a `FSCPackageSource` abstraction (disk folder | memory tar).
- Threading: FFI calls on a background task; UI stays responsive; cancel supported.

## Done criteria
- Importing Gladius from the tab produces byte-identical assets to the disk path
  (compare asset counts, Nanite flags, material params).
- `libscbridge.so` has no Tauri or UI dependencies; `nm -D` shows only `sc_*`.
- Panics inside Rust surface as an editor error dialog, never a crash.
