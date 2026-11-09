# WP-R3 — Clip catalogue API

**Agent:** Codex. **Depends on:** WP-R1 (and shares discovery code with WP-R2).
**Build lock:** required.

## Goal
Give the viewer a cheap way to list every animation an entity can play before any
heavy export: `scviewer_core::Session::clip_catalogue(entity) -> Catalogue`.

## Shape
```
Catalogue {
  entity: String,
  skeletons: Vec<SkeletonInfo { owner_entity, chr_path, chrparams_path, dba_path, bone_count }>,
  clips: Vec<ClipInfo {
     id: String,            // same key the GLB animation uses (WP-R2 naming)
     display_name: String,  // humanised like decomposed.rs::humanize_animation_label
     owner_entity: String,  // root or child entity stem
     source: Chrparams | DbaOnly | Caf,
     fps: u16, frame_count: u32, duration_s: f32,
     has_position: bool, has_rotation: bool, bone_count: usize,
     fragments: Vec<Fragment { fragment, tags, frag_tags, scopes }>,   // Mannequin, may be empty
     boomerang: bool,       // first sample == last sample (whitepaper §14.5)
  }>
}
```
Source of truth: `.chrparams` map ∪ DBA metadata catalogue (clips that exist only in
the DBA must appear), Mannequin via `animation/mannequin.rs` +
`query_animation_controller_source`. Reading the DBA header/metadata must not decode
all keyframes (lazy — the viewer calls this on every selection).

## Done criteria
- `cargo run -p scviewer-cli -- clips Scorpius` prints ≥ 10 clips with fragments for
  landing gear; Gladius prints its gear/canopy clips; an entity without a skeleton
  prints an empty catalogue, not an error.
- `clip_catalogue` on Gladius runs in < 500 ms warm.
- Unit tests with a checked-in **synthetic** DBA/chrparams fixture (never copies of
  game data), plus p4k-gated integration tests.

## Implementation notes / deviations (2026-11-09)

- The fallible API is `Session::clip_catalogue(&str) -> Result<Catalogue>`;
  `clip_catalogue_cached` returns `Result<Cached<Catalogue>>`. Both update the
  session's `CacheStatus`. Catalogue persistence uses the layer-1 manifest,
  atomic publication and LRU, keyed by build/entity/exporter/format plus its own
  schema discriminator. No geometry export is needed on a miss.
- `SkeletonInfo::chrparams_path` and `dba_path` are optional because rigs can
  have no discoverable parameters or use standalone CAFs. `chr_path` also covers
  CGA/NMC rigid rigs. Merged clips expose the first owner in export discovery
  order; all contributing source-rig/bone variants participate in their summary.
- Matching needs first/final values for the existing exporter's motion tie-break.
  The sparse decoder reads controller metadata, time tables and these endpoints;
  it never decodes intermediate sample values. Frame count follows the scene
  index's final-key-time convention rather than substituting DBA `end_frame`.
- The current exporter namespaces interior child clips only for **rootless
  standalone socpak packages**. Cutlass as a ship has a root rig and merges clip
  names. The catalogue preserves this existing contract; changing its IDs alone
  would break the requested join. Live tests verify exact scene-index ID equality
  in both directions for Gladius (63), Scorpius (70) and Cutlass Black (39).
  The entity Session API does not add a standalone socpak export entry point.
- Synthetic DBA, CAF and binary chrparams fixtures and their generator live in
  the upstream crate's tests/fixtures/clip_catalogue directory; no game bytes are
  checked in. The root workspace tests and upstream animation tests pass.

TODO(WP-R3): Orchestrator must create the WP branch/commits and arrange the required
other-model cross-review before merging; this sandbox cannot write `.git`.
