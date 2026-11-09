# WP-A1 — Tauri app scaffold

**Agent:** Opus 5.5. **Depends on:** WP-R1 (`Session`, `search_entities`, `list_dir`,
`geometry_file_glb`, `export_entity_glb`). **Build lock:** required for the
`src-tauri` crate (it joins the root workspace); npm steps are free.

## Goal
`app/`: Tauri 2 + React 19 + TypeScript + Vite + Tailwind 4 + zustand, mirroring the
upstream stack (`vendor/StarBreaker/app/package.json`) so patterns transfer. No
upstream code copied wholesale; crib structure and IPC idioms.

## Deliverables
- `app/src-tauri/`: `AppState { session: Mutex<Option<Arc<scviewer_core::Session>>> }`,
  commands `discover_p4k`, `open_p4k(path)`, `search_entities(query, limit)`,
  `list_dir(path)`, `search_paths(glob, limit)`, `preview_geometry(path) -> bytes`
  (return `tauri::ipc::Response` for raw bytes, as upstream does for `dna_preview_glb`),
  `export_entity_glb(name, opts) -> bytes` with progress events
  (`export-progress` channel). Add `src-tauri` as a workspace member.
- `app/src/lib/commands.ts` typed bindings; `stores/session-store.ts`,
  `stores/browser-store.ts`.
- Layout: left sidebar with two tabs — **Entities** (search box, virtualised result
  list with display name + class name + category) and **Files** (P4k tree limited to
  geometry extensions); centre = viewer placeholder (WP-A2 fills it); right =
  inspector placeholder; bottom status bar (p4k path, build branch, timings).
- Startup dialog when no p4k is discovered (path picker via `plugin-dialog`).
- Dark theme by default, system font, no hard-coded game values anywhere.
- `.claude/launch.json` entry `app` → `npm run tauri dev` in `app/` so the
  in-app browser can preview it.

## Done criteria
- `cd app && npm ci && npm run build` green; `npm run tauri dev` opens, discovers
  the LIVE p4k, searching "gladius" lists `AEGS_Gladius` within 1 s warm.
- Clicking a `.cga` in Files calls `preview_geometry` and the bytes arrive in the
  frontend (log size) — rendering is WP-A2.
- `cargo build` green; `oxlint`/`tsc` clean.
