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

## Stencils (camo liveries)

Camo paints (e.g. Cutlass Mistwalker) put a HardSurface `%STENCIL_MAP` layer on the hull: `TexSlot7`
(`camo_digi_stencil_01_mask`) holds three distance fields. Ported from the game's `stencilLib.cfib` non-sticker path
(`CalcStencilParams`, `GetStencilLayer_Premultiplied`, `ApplyStencilAsTint`) and `paletteLib.cfib`
`GetPaletteStencilColors`, token hashes pinned in `StarEngineRender.json` and checked by `create_materials.py`:

- Builder (`HardSurfaceStencil`): `StencilMap`, `StencilTiling`, `StencilOpacity`, `StencilUV2` (`UseUV2ForStencil`)
  and the resolved `StencilColor1..3`: `StencilTintOverride` 2 or a `$TintPaletteDecal` material = palette decal
  red/green/blue; 1 = palette primary/secondary/tertiary diffuse; otherwise the authored `StencilDiffuseColor{,2,3}`.
  A paint's palette therefore recolours its camo through the paint's material pair.
- `M_SC_HardSurface` (`STENCIL_TINT`): alphas = saturate((d − 0.5 + w/2) / w), w = fwidth(d) (0 where there are no
  derivatives, i.e. ray tracing: a hard step); R over G over B; × opacity; albedo = albedo × (1 − Σα) + Σ colourᵢ αᵢ,
  on the Primary layer after its primer and before the wear blend; the glow colour follows the albedo as in game.
- Assumption: the stencil breakup texture (`StencilBreakup`, TexSlot8: diffuse overlay R, wear alpha B) is not in
  the sidecars, so it is taken as neutral: no diffuse overlay, wear alpha 1 (`CalcStencilWearAlpha` is then 1 at
  any wear). `%STENCIL_AS_STICKER` (specular, gloss, edge wear) is not drawn.

## Verification

| Check | Result |
|---|---|
| `FSCPaintTarget` (hull, palette follower, own palette, hull file with own palette, out of range) | passed; `test_importer.py` 41 passed |
| Galaxy: 7 paints, 30,559 slots | default blue stripes; Starwalker orange; Magnitude blue hull, gold stripes; Protector pale hull, gold canopy (`out/smoke/paint-galaxy-*.png`) |
| Galaxy interior, default vs Magnitude | corridor 0.7 %, cockpit 1.2 % RMSE: interior materials barely use the tint channels |
| Cutlass Black: 12 paints, 24,421 slots | 2950 Best In Show white/gold; Mistwalker: decal and palette change |
| Stencil port: `FamilyMapping` (3 fixture HardSurface `%STENCIL_MAP` materials: authored, main tints on UV1, decal colours), `MasterParameters` strict | passed; `test_importer.py` 41 passed |
| Cutlass Mistwalker, hightech `hangar_smltop_001` | digital camo drawn in the palette's main tints, anti-aliased edges (`out/smoke/stencil-cutlass-mistwalker.png`; before: `paint-cutlass-paint3.png`, plain grey) |
| Cutlass default paint, before vs after the port | 0.52 % RMSE (run-to-run noise: no stencil materials on the default hull) |
| Cost | Galaxy import +~10 s warm (53 s), material instances 2,062 -> 4,930, textures 1.67 GB |

## Open

- Paints that swap geometry through their `SubGeometry` tag are not handled (none seen on the Galaxy or Cutlass).
- Lumen emissive proxies keep the default paint's glow colours until the next load.
