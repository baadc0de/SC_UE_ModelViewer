# PAINT-1 — vehicle paints (the tint system) (2026-11-13)

Owner: support the different vehicle skins ("based on the tint system").

## The game's model

Hull materials carry tint channels (primary, secondary, tertiary, glass, decal red/green/blue); a tint palette
(DataCore TintPaletteTree) fills them with colours and a finish. A paint item swaps the vehicle's palette and, for
many liveries, its exterior material file (other decal/stencil textures), selected by `SubGeometry` tag. The export
already carried this: `paints.json` (each variant's display name, palette id, exterior material file, tag),
`palettes.json`, and every variant's material file.

## Viewer

- Reader: `paints.json` -> `FSCSceneManifest::Paints`; every variant's exterior material file is loaded with the
  scene's; a paint without its palette or file in the package is dropped (warned).
- Rule (`FSCSceneManifest::PaintTarget`, resolved palettes): an instance on the vehicle's own exterior material file
  takes the paint's file; an instance whose resolved palette is the vehicle's takes the paint's palette (attached
  items and interior placements that inherit it, by the reader's chain: own, container, material default, host).
  Instances with their own palette are untouched. Submaterials are matched in the paint's file by name; decal host
  channels are recomputed per file.
- Import builds every paint's (file, palette) materials up front and records each touched slot
  (`ASCVehicle::PaintSlots`); `ASCVehicle::SetPaint` swaps them (instant). Side panel: Paint row (when the vehicle has
  paints); `sc.Paint`; the choice is remembered per vehicle (`USCViewerSettings::Paints`, by palette id).

## Verification

| Check | Result |
|---|---|
| `FSCPaintTarget` (hull, palette follower, own palette, hull file with own palette, out of range) | passed; `test_importer.py` 41 passed |
| Galaxy: 7 paints, 30,559 slots | default blue stripes; Starwalker orange; Magnitude blue hull, gold stripes; Protector pale hull, gold canopy (`out/smoke/paint-galaxy-*.png`) |
| Galaxy interior, default vs Magnitude | corridor 0.7 %, cockpit 1.2 % RMSE: interior materials barely use the tint channels |
| Cutlass Black: 12 paints, 24,421 slots | 2950 Best In Show white/gold; Mistwalker: decal and palette change |
| Cost | Galaxy import +~10 s warm (53 s), material instances 2,062 -> 4,930, textures 1.67 GB |

## Open

- Camo liveries (e.g. Cutlass Mistwalker): the pattern is a HardSurface `%STENCIL_MAP` layer
  (`camo_digi_stencil_01_mask`, `StencilTiling`, `StencilDiffuseColor1..3`, `StencilTintOverride`, `UseUV2ForStencil`);
  the material port implements stencils only for MeshDecal, so the camo pattern is not drawn yet.
- Paints that swap geometry through their `SubGeometry` tag are not handled (none seen on the Galaxy or Cutlass).
- Lumen emissive proxies keep the default paint's glow colours until the next load.
