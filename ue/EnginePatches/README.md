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
| (inline, in `engine_patches.py`) | `Shaders/Private/DeferredLightingCommon.ush` (`GetLocalLightAttenuation`) | a local light's range window is the game's `GetLightForcedDistanceFade`, `saturate((1 − d / R) / forcedFalloffFraction)²`, instead of UE's `(1 − (d / R)⁴)²` (deferred, MegaLights, Lumen, translucency and volumes all call it) | [WP-L3](../../docs/work-packages/WP-L3-light-range-fade.md) |
| (same) | `Shaders/Private/LightCulling.ush` (`EstimateLocalLight`) | the light grid's energy estimate (MegaLights sampling and culling) uses the same window | WP-L3 |
| (same) | `DeferredLightingCommon.ush` (`GetLocalLightAttenuation`, `GetCapsule`), `LightCulling.ush` | the attenuation tweak's distance offset: point / spot lights fall off as `candela / (max(d, bulb) + offset)²`; the importer stores the offset in `SoftSourceRadius` (cm), which UE then no longer reads as a soft specular radius | WP-L3 |

| (same) | `Shaders/Private/LightFunctionAtlas/LightFunctionAtlasCommon.usf` (`GetLocalLightFunctionCommon`) | a spot light function is zero outside its texture (the game samples projector gobos with a border sampler; UE's atlas wraps with `frac`); the importer opens projector cones to the square frustum's corners | WP-L3 |

On Windows the engine is the launcher install: set `UE_ENGINE_ROOT` (PowerShell
`$env:UE_ENGINE_ROOT = 'C:\Program Files\Epic Games\UE_5.9'`) and run the script with UE's own Python
(`Engine\Binaries\ThirdParty\Python3\Win64\python.exe -I ue\scripts\engine_patches.py apply`) when `uv` is not
installed. `DeferredLightingCommon.ush` is included by most shaders, so the first run after `apply` recompiles global
and material shaders.

Development A/B: `r.GeneralPurposeTweak 0` is UE's behaviour (no SSDO, Lumen short-range AO), `1` (default; fixed in
shipping builds) is the game.
`r.GeneralPurposeTweak2 2` / `3` show the SSDO AO / bent normal as the MegaLights diffuse light.
