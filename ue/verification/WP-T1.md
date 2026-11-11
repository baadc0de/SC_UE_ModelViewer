# WP-T1 verification — full-resolution textures as the game's blocks

Brief: `docs/work-packages/WP-T1-compressed-textures.md`. Upstream branch `scviewer/dds-textures`.

## Found

The viewer rendered every texture at mip 2 (¼ of the game's resolution per axis): the exporter decoded each DDS
to an RGBA PNG and the importer uploaded BGRA8, so mip 0 needed 4.06 GiB on the Gladius against the 2 GiB import
budget. The game ships the same textures block-compressed — 0.72 GiB at mip 0 (survey of the 264 Gladius sidecar
textures: BC5 SNORM ×88, every `_ddna`; BC1 ×79; BC3 ×40; BC4 ×39; BC7 ×17; R8 / legacy uncompressed ×9).

## Change

- Exporter (`TextureContainer::Dds`, viewer only; PNG stays the upstream default): the reassembled DDS blocks from
  the requested mip down, DX10 header, sRGB as the DXGI `_SRGB` variant; the DDNA alpha as its own
  `*_smoothness_TEX<mip>.dds` (BC4 / R8, `value_transform = identity`). PNG fallback for what the contract cannot
  carry (BC6H, cubemaps, 24-bit RGB stencils, ...).
- Bridge: `sc_dds_prepare` (validate; BC4/BC5 SNORM → UNORM, −128 clamped first; DDNA green negated in the blocks)
  and `sc_dds_mean_rgb` (top-mip linear mean for the layer spec fold).
- Importer: package DDS → the matching `EPixelFormat`, mips as stored, never decoded; budget counts uploaded
  bytes. Masters read `SmoothnessMap` as the game's smoothness (was `RoughnessMap` = sqrt(1 − s)); the PNG
  fallback (derived roughness) is decoded back to smoothness, 1 − r², before its mips are built. Viewer default mip 0, `CACHE_FORMAT_VERSION` 19.
- Single-channel textures (BC4, R8) sample as (R, 0, 0, 1) on the GPU, as in the game; the PNG path expanded them
  to (R, R, R). It matters where a master reads more than red: the stencil decal reads R, G, B as three palette
  colours, so an R8 stencil (`aegis_gladius_stencil_geometric_diff`, a livery, not the default paint) now tints
  with its red channel only. Not checked on the GPU (no livery selection in the viewer yet).

## Tests first

Rust (Codex; each failed before the change): package-DDS writer header / mip slicing / DXGI mapping (DXT3 → BC2,
legacy masks) / sRGB promotion / size rejection; PNG default unchanged; exporter DDS paths per flavour; SNORM
re-bias and normal flip compared texel by texel against `bcdec_rs` on random blocks in both palette modes,
−128 endpoints included (the first contract, `u = s + 128`, failed that test: −128/−127 switched palette mode);
mean colour; viewer mip default.

```
cargo test -p starbreaker-dds        ok. 5 passed (+ tests/package.rs)
cargo test -p starbreaker-3d --lib   ok. 887 passed; 3 ignored
cargo test -p scviewer-core          ok. 32 passed
cargo test -p scbridge-ffi           ok. 8 passed; header, shell ok
scripts/build_ffi.sh; scripts/build_ffi.sh --check-header   scbridge.h matches cbindgen output
```

UE (`test_importer.py`, 33 suites, `WP-U1 UNIT_TESTS_OK`): new `SCBridge.Materials.DdsBlocksPassThrough` (BC5
SNORM normal → `PF_BC5`, red endpoints re-biased 100/−50 → 228/78, green (60, −20) → (148, 68) with index 0 → 1,
colour role not flipped; BC1 `_SRGB` → `PF_DXT1` sRGB under a linear role; means through the bridge; a short mip
rejected; created textures carry the format, mips and encoding) and
`SCBridge.Materials.SmoothnessMapIsTheGameSmoothness` (DDS bound as linear data; the PNG fallback bound as
derived roughness and decoded r 0 / 128 / 255 → s 255 / 191 / 0, mips averaged in smoothness). Masters regenerated
(`create_materials.py`, `WP-U2 MATERIALS_OK`).

## Exports and headless

`target/debug/scviewer --no-cache decomposed <entity> out/dec-t1-<ship>` (LOD 0, mip 0):

| Ship | DDS | PNG (gobos, UI screens, 24-bit stencils) | package | GPU texture bytes | import |
|---|---|---|---|---|---|
| Gladius | 348 | 18 | 810 MB | 633 MB (271 textures, 0 failed) | 5.1 s |
| Scorpius | 336 | 19 | 877 MB | 654 MB (247, 1 failed) | 5.9 s |
| Cutlass | 588 | 32 | 1.5 GB | 1 066 MB (423, 1 failed) | 7.8 s |

The failures are the existing emissive `emittance_texture = "TexSlot14"` (a slot name exported as a path; present
in the mip 2 exports too), not textures. Texture decode 0.17 – 0.22 s per ship (the bridge only validates and
rewrites BC4/BC5 endpoints). `test_rig.py` (`RIG_VERIFY passed=1`, counts unchanged: 64/63/80/636/16,
75/71/90/675/23, 55/53/68/371/32) and `test_first_person.py` (`WP-P1 HEADLESS_OK`) exit 0 on all three.

## GPU (RTX 4090, Vulkan, the app's own load path)

```
ue/scripts/capture_route.sh t1-gladius ue/scripts/routes/gladius-hull-seat.txt -SCDefaults -SCLoad=AEGS_Gladius -SCCamera=first
ue/scripts/capture_route.sh t1-cutlass ue/scripts/routes/cutlass-ramp-to-cockpit.txt -SCDefaults -SCLoad=DRAK_Cutlass_Black -SCCamera=first
ue/scripts/capture_route.sh t1l1-cutlass ue/scripts/routes/cutlass-ramp-to-cockpit.txt -SCDefaults -SCLoad=DRAK_Cutlass_Black -SCCamera=first -SCLod=1
```

Gladius against `lod0-gladius` (mip 2 PNG, same route; `wp-t1/mip0-gladius.jpg`, left mip 2, right mip 0, native
pixels): the hull's Aegis logo is sharp instead of blurred and stair-stepped; the cockpit's lighting, normals and
gloss match (green flip and smoothness read correct), the dial emblem is sharper. App status: export 8.9 s
(cache miss) against 8.2 s, materials 0.27 s against 0.11 s, total 15.2 s against 13.6 s.

Cutlass: renders cleanly (1.07 GB of textures). The route stops at the bulkhead at LOD 0 but reaches the cockpit
at LOD 1 with the same textures: through the cargo bay the player covers 357 cm in a 2 s move at LOD 0 against
670 cm at LOD 1 — a LOD 0 geometry / collision effect from 5e041a6, not this change (open).

## Not done

Player package textures (`character/package.rs`) stay PNG; light gobos stay PNG/EXR; BC6H stays tone-mapped PNG.

## Cross-review (Codex, read-only) — six findings, all fixed

1. Mip counts were not bounded by size (a 4×4 BC1 declaring 4 mips passed): writer and bridge reject
   `mip_count > floor(log2(max(w, h))) + 1`.
2. A PNG smoothness fallback (mixed-format alpha chains) lost its map: it is now bound as
   `ESCTextureRole::DerivedRoughness` and decoded back to smoothness 1 − r² before its mips.
3. Block-compressed tops that are not 4-aligned (UE rejects them) were exported as DDS: the exporter writes PNG
   for those, the bridge rejects them.
4. `sc_dds_mean_rgb` replicated R8 / BC4 into RGB; the GPU samples (R, 0, 0): means now match.
5. Standalone decomposed weapon exports still decoded bundled textures before the DDS writer: suppressed for
   `TextureContainer::Dds` (root and attachments).
6. `.dds` outputs were classified `PackageManifest`: now `TextureAsset`.

Each fix has a test that failed first. After the fixes: `cargo test` starbreaker-dds 7, starbreaker-3d 892
(3 ignored), scviewer-core 36 (live exports included), scbridge-ffi 8 + header + shell; `build_ffi.sh
--check-header` ok; `test_importer.py` 33 suites; the Gladius re-export's 348 DDS files are byte-identical to the
first export; `test_rig.py` on it `RIG_VERIFY passed=1` (same counts), textures 271 created, 0 failed.
