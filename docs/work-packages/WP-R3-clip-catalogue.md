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

## Cross-review fixes (2026-11-09)

- Per-rig geometry/chrparams/DBA failures warn and skip; healthy rigs survive.
  Entity export and catalogue consume a shared rig-source list and merge policy,
  including record-based geometry fallback, gear filenames and empty-name rules.
- Sparse decoding preserves unknown-position blocks with the full decoder's
  zero fallback; zero CAF FPS uses 30 at the parser. Format strides and the
  inactive-axis sentinel are shared codec constants.
- Wildcard chrparams event names (`*`, `?`) are omitted as mapping rules. The
  whitepaper does not specify expansion semantics; concrete DBA entries remain
  discoverable through the metadata union, preserving real playable clips.
- `ClipInfo::contributors: Vec<String>` adds contributing owners and resolved rig
  node names without changing merged IDs; older JSON defaults this field to empty.
  Catalogue cache schema is v2 to invalidate the former binary layout.
- Synthetic regressions cover missing DBA/geometry, zero FPS, wildcard names,
  unknown position formats, inactive SNORM axes, bitmap time keys and provenance.
  Timing enforcement is opt-in (`SCVIEWER_PERF_TESTS=1`); the no-geometry live
  case is selected from DataCore. Matching is split into scoring, diagnostics,
  extraction and tests rather than remaining one monolithic module.

TODO(WP-R3): Namespace child-rig clips in ship packages upstream as a coordinated
exporter + importer contract change, needed by M5 to open individual doors.
The contributors field supplies provenance but does not change playback routing.


Review verification: 17 workspace tests and 41 upstream animation tests pass,
plus the two focused rig failure/fallback tests. Live export parity passes for
Gladius **63**, Scorpius **69**, Cutlass Black **40**; no wildcard IDs remain.
Gladius replaces `*` with the concrete DBA `landing_gears_retract.caf` path;
Scorpius drops `*`; Cutlass drops `*` and exposes `nacelles_retract.caf` and
`drak_cutlass_seat_pilot_idle`. The no-geometry entity was queried as `freeze`.

`out/dec-r1` was regenerated at LOD 1 / mip 2. Its scene index, 12 existing
animation sidecars and UI export stamp changed; one concrete sidecar was added
and the obsolete `animations/_.json` removed. Mesh, material and texture files
were byte-identical. The UE agent must reload animation data from the new index.
The reusable parity check is:

```bash
python3 -I scripts/check_clip_catalogue.py out/wp-r3-review/gladius.json 'out/dec-r1/Packages/AEGS Gladius_LOD1_TEX2/scene.json'
```
