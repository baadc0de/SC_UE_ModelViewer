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

Open: decals (`M_SC_MeshDecal`) take no dirt yet; the Layered master's wear map and vertex dirt are not ported.
