# WP-U1 GPU validation — 2026-11-09 (orchestrator, RTX 4090, Vulkan)

Codex's sandbox had no GPU, so its WP-U1 runs used `-nullrhi`. The orchestrator ran
the committed importer (superproject `784f70c`) in the chosen host B on the GPU with
the **final-exporter** Gladius package (`out/dec-r1`, materials in sidecar slot
order, produced by `scviewer decomposed Gladius out/dec-r1 --lod 1 --mip 2` at
superproject `1de75d0`):

```bash
ue/scripts/run_import.sh out/dec-r1 -vulkan -SCStay
ue/scripts/run_import.sh out/dec-r1 -vulkan -SCStay -ExecCmds="ShowFlag.VisualizeNanite 1, r.Nanite.Visualize Overview"
ue/scripts/run_import.sh out/dec-r1 -vulkan -SCStay -ExecCmds="ShowFlag.VisualizeLumen 1, r.Lumen.Visualize 1"
```

Key log output (identical across the three runs apart from timing):

```text
IMPORT_OK seconds=2.4948 assets=45 instances=61 children=51 interiors=3 materials=17 palettes=18 clips=63 game_world=1
IMPORT_OK seconds=2.3886 ...   (second run)
IMPORT_OK seconds=2.0139 ...   (third run)
BOUNDS_CM min=X=-851.495 Y=-868.891 Z=-287.760 max=X=1283.931 Y=867.638 Z=294.102 size=X=2135.426 Y=1736.529 Z=581.862
ATTACHMENT_ROOT_FALLBACK entity=EntityClassDefinition.AEGS_Gladius helper=hardpoint_fuel_port (resolved local transform retained)
```

Screenshots (half size) in `ue/verification/wp-u1/`:

| File | What it shows |
| --- | --- |
| `lit.png` | the Gladius under Lumen GI with VSM shadows; hull, canopy, wings, fins, landing gear, main thrusters, nose gun, countermeasure launchers and wing-tip parts all placed; nose along +X; ~21.4 m × 17.4 m × 5.8 m bounds |
| `nanite.png` | `r.Nanite.Visualize Overview`: Triangles / Clusters / Instances / Primitives panels show the whole ship rasterised by Nanite; Overdraw and Shading/Raster bins populated |
| `lumen.png` | `r.Lumen.Visualize 1`: the ship is present in Geometry Normals, **Reflection View (HWRT)** and **Lumen Scene** — no black cut-out (the WP-U0 gap is closed); a handful of yellow/pink pixels on small parts (culled / missing surface-cache coverage on tiny meshes), acceptable |

Observations for the follow-up round:
- The DDC was warm from Codex's runs, so 2.0–2.5 s is a warm import; cold was 10.1 s in Codex's measurement.
- `materials=17` is the sidecar count the reader resolved for the slot-ordered package; the earlier compact `out/dec` reported the same count, so the reader is index-agnostic as intended.
- No visual check of per-slot material assignment is possible until WP-U2 (all surfaces are grey).

## Re-validation after the review fixes (superproject `4a44d2c`)

Same three runs on `out/dec-r1`:

```text
IMPORT_OK seconds=2.4380 assets=45 instances=61 children=51 interiors=3 materials=17 palettes=18 clips=63 game_world=1 render_meshes=78 render_instances=111 warnings=0
```

- Lit and Nanite views unchanged (same bounds, same placement).
- `lumen-after-split.png`: the hull is still present in the Lumen Scene and the
  HWRT reflection view (no black cut-out). The split-off Glass / MeshDecal meshes
  now show as **yellow (culled surface cache)** patches — canopy glass, wing-tip
  glow decals, small decals — because they are small separate non-Nanite
  components. That is expected for translucent surfaces, which should not feed the
  surface cache anyway; TODO(WP-U2): when real translucent/decal materials exist,
  set those components to not affect Lumen (`bAffectDistanceFieldLighting=false`,
  no surface-cache capture) so the overlay is clean, and keep the host hull as the
  GI contributor.
