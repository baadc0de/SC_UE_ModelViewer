# WP-T1 — Full-resolution textures: the game's compressed blocks, verbatim

## Why

The viewer renders textures at mip 2 (a quarter of the game's resolution in each direction). The exporter
decodes every DDS to an RGBA PNG and the importer uploads it as uncompressed `PF_B8G8R8A8`: at mip 0 the Gladius
alone needs 4.06 GiB of decoded texels against the importer's 2 GiB budget. The game ships every texture block
compressed (survey of the Gladius sidecars, 264 textures: BC5 SNORM ×88 — every `_ddna` —, BC1 ×79, BC3 ×40,
BC4 ×39, BC7 ×17, R8 / legacy uncompressed ×9); the same textures as the game's blocks at mip 0 are 0.72 GiB.
UE can create a texture in each of those formats at run time and copy the blocks in unchanged.

## Contract (decomposed export, `TextureContainer::Dds`)

`starbreaker_3d::ExportOptions` gains `texture_container: TextureContainer` (`Png` — default, unchanged
upstream behaviour — or `Dds`). With `Dds`, a texture the decomposed writer exports through
`export_texture_asset` (Generic, Normal flavours) or the DDNA roughness derivation is written as a DDS **when the
source can pass through**, else exactly as today (PNG). The sidecar `export_path` names whichever file was written
(extension `.dds` or `.png`); everything else about the path (`Data/...`, `_TEX<mip>`) is unchanged, except that
the DDNA alpha file is named `<stem>_smoothness_TEX<mip>.dds` (it holds smoothness, not roughness).

A package DDS file is: `"DDS "` + the 124-byte header + **always** a DX10 header, then the mips from the
selected mip to the last mip present, blocks byte-for-byte as stored in the P4k (after the split-sibling
reassembly `DdsFile::from_split` already does). Header: width/height of the selected mip, `mipMapCount` = mips
written, `DDSD_MIPMAPCOUNT|DDSD_LINEARSIZE|CAPS|HEIGHT|WIDTH|PIXELFORMAT`, `pitchOrLinearSize` = bytes of the first
written mip, pixel format FourCC `DX10`. DX10: `resource_dimension = 3` (2D), `misc_flag = 0`, `array_size = 1`.

- Selected mip: the requested mip clamped to the mips present (as the PNG loaders clamp).
- DXGI format: the DX10 header's, or from the FourCC (`DXT1`→71, `DXT3`→74 (BC2, not BC3), `DXT5`→77,
  `ATI1`/`BC4U`→80, `BC4S`→81, `ATI2`/`BC5U`→83, `BC5S`→84), or from legacy masks (8-bit R/L `0xFF` → 61 R8_UNORM;
  32-bit `A 0xFF000000 R 0x00FF0000 G 0x0000FF00 B 0x000000FF` → 87 B8G8R8A8; `R 0x000000FF G 0x0000FF00
  B 0x00FF0000 A 0xFF000000` → 28 R8G8B8A8).
- sRGB: when `DdsFile::is_srgb()` is true the DXGI format is written as its `_SRGB` variant (71→72, 74→75,
  77→78, 98→99, 87→91, 28→29). A consumer reads the encoding from the DXGI format alone.
- Passes through: 61, 71/72, 74/75, 77/78, 80, 81, 83, 84, 98/99, 87/91, 28/29. Does **not** pass (PNG as
  today): BC6H (HDR; the PNG path tone-maps it), cubemaps / arrays / volumes, any other format, an sRGB-flagged
  texture whose format has no `_SRGB` variant, a mip whose byte length is not exactly the format's size for its
  dimensions, an empty chain or more than `floor(log2(max(width, height))) + 1` mips,
  or a block-compressed format whose **selected top mip** width or height is not a multiple of 4
  (including 1×1, 2×2 and odd sizes). Smaller tail mips remain valid; this alignment limit applies only
  to the top level. The bridge rejects packages with invalid mip counts or unaligned compressed tops too.
- Normal flavour: the `_ddna` colour mips (BC5) only; the smoothness alpha is not in this file. Same
  `texture_path_looks_normal_gloss` gate as the PNG loader.
- DDNA smoothness (the roughness derivation): the alpha mips, from the requested mip clamped to the alpha mips
  present, as one DDS (BC4 UNORM 80 / BC4 SNORM 81 / R8 61 per `alpha_mip_format_for_mip`); PNG as today when the
  written alpha mips do not share one format or a size does not match. Its `derived_textures[]` /
  `ddna_derivations[]` ref states what is stored: `packed_texture_format = "smoothness"`,
  `packed_channel_semantics = { r: "smoothness" }`, `value_channel = "r"`, `value_transform = "identity"`,
  no constant channels. (A PNG fallback keeps today's `roughness_grayscale` / `sqrt_one_minus`.)
- With `Dds`, nothing decodes or PNG-encodes a texture that is written as DDS (prewarm included).

Not covered: the player package (`character/package.rs` `texture_png`), UI render-to-texture PNGs, gobo EXRs —
they stay PNG / EXR.

## Viewer side

- `scviewer-core`: `GlbOptions::upstream(ExportKind::Decomposed)` sets `TextureContainer::Dds`; the default mip
  becomes 0; `CACHE_FORMAT_VERSION` is bumped.
- `scbridge-ffi` (handle-free, error via `sc_last_error(NULL)` on the calling thread):
  - `sc_dds_prepare(uint8_t *data, size_t len, bool normal_map, struct sc_dds_info *out)`: validates a package
    DDS (contract above) and adapts it **in place** for UE, which has no SNORM block formats: BC4/BC5 SNORM →
    UNORM by re-biasing each BC4 endpoint byte (`u = max(s, -127) + 128`: D3D/Vulkan decode SNORM −128 as −127,
    so it is clamped first; the map is then order-preserving, so the palette mode and the 3-bit indices stay as
    stored) and returning 80 / 83. `normal_map` (BC5 only — anything else is an error):
    negate the second channel (glTF/OpenGL +Y → UE −Y) in the block: swap the green endpoints with their
    negations and remap indices 0↔1, k↔9−k (8-value mode) / 2↔5, 3↔4, 6↔7 (6-value mode). `sc_dds_info`:
    `dxgi_format`, `width`, `height`, `mip_count`, `data_offset`, `data_size`.
  - `sc_dds_mean_rgb(const uint8_t *data, size_t len, float out_rgb[3])`: linear-light mean RGB of the top mip
    (texels of an `_SRGB` format are linearized before averaging), as `FSCTextureImporter::MeanColor` does.
    Single-channel R8/BC4 averages are `(R, 0, 0)` and BC5 averages are `(R, G, 0)`, matching GPU sampling.
- UE importer: package files starting `"DDS "` go through `sc_dds_prepare` and are created with the matching
  `EPixelFormat` (BC1→`PF_DXT1`, BC2→`PF_DXT3`, BC3→`PF_DXT5`, BC4→`PF_BC4`, BC5→`PF_BC5`, BC7→`PF_BC7`,
  R8→`PF_G8`, B8G8R8A8→`PF_B8G8R8A8`, R8G8B8A8→`PF_R8G8B8A8`), sRGB from the format, mips as stored; the budget
  counts the bytes uploaded. PNG keeps today's path. The masters read the DDNA smoothness map as smoothness; a
  PNG fallback of it (derived roughness) is decoded back to smoothness, 1 − r², before its mips are built.
