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
