# Engine shader patches (plan D14)

The installed UE 5.9 is a binary build: its C++ cannot change, its shaders can. A game rendering rule that needs an
engine shader edit is kept here and applied to `$UE_ENGINE_ROOT` (default `~/UnrealEngine/5.9`) by
`ue/scripts/engine_patches.py`:

```bash
uv run python ue/scripts/engine_patches.py apply   # after every engine install, before running or cooking
uv run python ue/scripts/engine_patches.py check   # exit 1 when a patched file is missing or stale
uv run python ue/scripts/engine_patches.py revert  # restore the engine's originals
```

`apply` always rebuilds from the original kept beside each patched file (`<name>.scviewer-orig`), so it is idempotent
and picks up edits here. Insertions are anchored on exact engine lines; an engine update that moves them makes the
script fail instead of patching blindly. Constants come from `ue/Plugins/SCBridge/Resources/StarEngineRender.json`.

| Patch | Engine file | What | WP |
|---|---|---|---|
| `MegaLights/SCSSDO.ush` (+ generated `SCSSDOParams.ush`) | `Shaders/Private/MegaLights/MegaLightsShading.usf` | Star Engine SSDO cone on lights that cast no shadow (and directional lights) | [WP-L2](../../docs/work-packages/WP-L2-unshadowed-occlusion.md) |
| (same) | `Shaders/Private/DiffuseIndirectComposite.usf` | Star Engine SSDO on ambient light: replaces Lumen's short-range AO (diffuse and rough specular) | WP-L2 |

Development A/B: `r.GeneralPurposeTweak 0` is UE's behaviour (no SSDO, Lumen short-range AO), `1` (default; fixed in
shipping builds) is the game.
`r.GeneralPurposeTweak2 2` / `3` show the SSDO AO / bent normal as the MegaLights diffuse light.
