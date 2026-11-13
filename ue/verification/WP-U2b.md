# WP-U2b — Dirt layer: verification

Host: UE 5.9.0 preview-1, Vulkan SM6, RTX 4090; packages `out/dec-p3-*`. Brief and rule:
`docs/work-packages/WP-U2b-dirt.md`.

## Build and masters

`ue/scripts/build.sh SCViewerEditor` — Result: Succeeded. Masters regenerated (UE 5.9 needs
`-AllowCommandletRendering` instead of `-nullrhi` to compile materials):

```bash
"$HOME/UnrealEngine/5.9/Engine/Binaries/Linux/UnrealEditor-Cmd" "$PWD/ue/SCViewer/SCViewer.uproject" \
  -run=pythonscript -script="$PWD/ue/scripts/create_materials.py" -unattended -nop4 -nosplash \
  -AllowCommandletRendering -ddc=SCViewerDDC                   # WP-U2 MATERIALS_OK
```

`MPC_SCDirt` created; `M_SC_HardSurface` 137 expressions (ps 1009), `M_SC_Layered` 183 (ps 1017).

## Tests

- `SCBridge.Dirt.Exterior` (new): a channel-0 component gets exterior factor 1, room components (channel 1 / 2) 0;
  `sc.Dirt 0.6 Sand` sets the amount and the preset colour.
- Importer 37/37; importer + Cutlass package + tar 39/39; `test_rig.py` and `test_first_person.py` OK on the Gladius
  and the Cutlass.

## GPU

`routes/gladius-dirt.txt` (Gladius orbit overview and a hull close-up at dirt 0 / 0.3 / 0.6 / 1.0 Dust, then Sand /
Mud / Ash at 0.6): the log reports `DIRT_EXTERIOR exterior=430 interior=114`; dirt gathers where the wear blend map
and the vertex AO allow it (fins and stripes first), dark presets use the game's slope blend
(`out/smoke/dirt-gladius-amounts.jpg`, `dirt-gladius-hull.jpg`, `dirt-gladius-presets.jpg`). Cutlass, dirt 0 vs 1:
cargo bay RMSE 0.82 (the frame-to-frame noise floor: interiors stay clean), ramp from outside RMSE 5.50
(`DIRT_EXTERIOR exterior=436 interior=761`). The options panel shows the Dirt slider (30 %) and the four presets.

Default 30 % Dust: chosen by eye against the owner's Gladius references, which show light grime on panel edges and
recesses. Not game data — a ship's authored dirt is 0.


## Follow-up: the gaps closed (2026-11-12)

Decal dirt, the Layered master's wear map / vertex dirt / engine detail map, and the direction-dependent dirt (see the
brief). Evidence:

- `scripts/verify_star_engine_render.py`: `STAR_ENGINE_RENDER OK: 62/62` (new: `shader_textures.WearDirtFrostDetailMap`,
  `shader_constants.detailScale`).
- `cargo test -p scviewer-core`: 39 passed (new `engine_textures` tests: fixture entry, family detection, the
  texture copied from the installed Data.p4k and only into LayerBlend_V2 packages).
- UE unit tests 37/37 (new assertions: Layered `WearMap` / `UseWearDirtAOVertex` / `DirtDetailMap` /
  `DirtDetailScale`, decal `DirtBlendMap` / `DirtBlendTiling` / `UseVertexColors`, `sc.Dirt` underside); importer
  + package 39/39; first person and rig OK on the Gladius, Cutlass and Scorpius.
- GPU `routes/gladius-dirt.txt` (`dirt2`, `dirt3`): with the underside tilt the top of the hull carries less dirt
  than the first version at the same amount, and the Aegis logo decal takes dirt
  (`out/smoke/dirt-gaps-before-after.jpg`). The ship and player packages now contain
  `Engine/EngineAssets/Textures/wearDirtFrost_mask.dds` and the import logs no texture warning (264 before the player
  path was hooked). The detail map changes 22 pixels of the Gladius at full dirt: its exterior LayerBlend parts are a
  few gun/component surfaces whose wear maps already hold the dirt back.
- UE 5.9: the deferred-decal domain rejects the world→local transform, so decals take the world normal.
