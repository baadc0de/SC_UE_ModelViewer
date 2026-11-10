# StarEngineRender.json — provenance notes

Companion to `StarEngineRender.json` (JSON carries no comments). The fixture holds the values of the game's
renderer that the Viewer cannot read at run time: cvar defaults compiled into the game executable and
constants / functions of its tokenised shader sources. `FSCStarEngine` (`Public/SCStarEngine.h`) loads it with
no in-code fallback; `ue/scripts/create_display_mapping_material.py` reads the function hashes. The rules
built on these values are derived in `ue/verification/WP-U3b.md` §13. Update this file in the same commit as
any fixture change.

## Extracted from

| Field | Value | Source |
|---|---|---|
| `branch` | `sc-alpha-4.10.2` | `LIVE/build_manifest.id` → `Branch` |
| `version` | `4.10.197.2878` | `Version` |
| `change` | `12913470` | `RequestedP4ChangeNum` |

Inputs, both part of the installed game (untrusted data, never checked in):

- `LIVE/Bin64/StarCitizen.exe` — the cvar defaults.
- `Engine/ShaderCache_Vulkan.pak` inside `Data.p4k` — a zip of FXB1 tokenised shader sources
  (`Shaders/CFXIB/Vulkan/*.cfib|*.cfxb`). It is a stored, unencrypted member of the archive, so the validator
  reads it in place (ZIP64 central directory → member → byte window opened as the nested zip); nothing is
  extracted and no extracted copy can go stale. To read the shaders by hand, extract the pak with the vendored
  CLI (Data.p4k is auto-discovered):

  ```bash
  scripts/starbreaker.sh p4k extract --filter 'Engine/ShaderCache_Vulkan.pak' -o out/shaders
  ```

## Validator

```bash
python3 -I scripts/verify_star_engine_render.py
# SOURCE exe: …/LIVE/Bin64/StarCitizen.exe
# SOURCE shaders: Engine\ShaderCache_Vulkan.pak (2391328688 bytes, crc32 aea61527) read in place from …/LIVE/Data.p4k
# STAR_ENGINE_RENDER OK: 47/47 checks match the installed game
```

`--exe PATH` and `--p4k PATH` (defaults: the LIVE executable and `Data.p4k` under `~/Games/star-citizen/...`),
`--fixture PATH`, `--dump` (print the extracted values as JSON instead of comparing — how the fixture was
authored). `--shaders DIR|PAK` checks an extracted copy instead of the archive; the `SOURCE` line and the verdict
then say that the shader inputs were not read from `Data.p4k`, because nothing proves such a copy belongs to
the installed build. Standard library plus binutils `objdump`; run with `-I` because both inputs are untrusted.
Any difference is a non-zero exit naming the value. Run it after every game patch; a failure means the fixture
and, for a changed function hash, the rule ported from that function must be re-derived — never edit a value to
make the validator pass.

The 47 checks are the 46 fixture values plus one completeness check: the fixture must list exactly the
`r_ExposureCompensationEV<n>` cvars the executable registers (the loader builds the compensation curve from
whatever anchors the fixture carries, so a dropped or invented anchor would otherwise pass). The shader side of
the same curve — which anchors are read and where the segments join — is pinned by the `CalculateEVCompensation`
hash.

Method:

- **`exe_cvars`** — the cvar name string is located in the image, its RIP-relative `lea rdx` references are
  found, and the enclosing function (bounds from the PE exception directory) is disassembled up to the console
  registration call: the default is in `r9d` (integer registration slot) or `xmm3` (float slot). Registers are
  tracked because the compiler shares constants between registrations. `kind` records which slot was used.
- **`shader_constants`** — `pattern` is matched against the space-joined token text of `file`; the first group
  is the value.
- **`shader_functions`** — `sha256` of the space-joined tokens of every overload definition of the function in
  `file`. The functions are not stored (game-derived text does not enter git); the hash pins the source a rule
  or port was written from.

## What each value drives

| Value | Used by | Rule |
|---|---|---|
| `HDR_LUMINANCE_SCALE` | `FSCStarEngine::SceneUnitNits` | one unit of scene colour = 1000 cd/m²; scales lights, glow and the exposure multiplier |
| `LOG2_LUMINANCE_TO_EV` | `SceneEV` / `NitsAtEV` | scene EV = log2(cd/m²) + 3 (`LogLumToEV`) |
| `sqrGlowFactor` | `GlowNits` | material glow is quadratic: scene units = Glow² × 400 (`MaterialGlowToLinearGlow`) |
| `HDR_LUMINANCE_SCALE` (again) | `InstanceGlowNits` | per-instance glow is linear: `CalculateEmissive` adds `instanceGlow × glowColour` to the converted material glow, so an instance amount is already in scene units |
| `middleGray` | `LogMultiplierAt` | `exposure = middleGray / exp2(logLumTarget)` (`CalcExposureAndNoise`) |
| `MaxDisplayNitsLDR` | display-mapping material | SDR peak passed to the display curve (`DisplayMapping`) |
| `LOG_MIN`, `LOG_MAX` | `FSCExposureSettings` histogram range | the meter's log2 scene-unit range (`Histogram.cfib`) |
| `r_ExposureCompensationEV0/4/8/12/16` | `CompensationAt` | anchors of `CalculateEVCompensation`; the scene EV is the number in the cvar name |
| `r_ExposureMultiplierMin/Max` | `EVAtMultiplier` → UE min / max brightness | clamp of the exposure multiplier |
| `r_ExposureDiscardRatioDark/Bright` | UE histogram low / high percent | histogram fractions left out of the meter |
| `r_ExposureAdaptTimeDark/Bright/Pupil`, `r_ExposureAdaptRangePupil` | UE adaptation speed (pupil stage) | `PigmentAdaptation` / `PupilAdaptation` half-lives |
| `r_HDRTonemappingCurve`, `r_HDRTonemappingCurveToe` | display-mapping material | curve 1 = `HuePreservingShoulder`, toe 1.0 |

Recorded as evidence only (they justify a documented residual or a path not taken; nothing reads them):
`r_exposureMeterMode` 3 and `r_exposureIncidentBlendAmt` 0.5 (the game blends an incident-light meter into the
reflected one; UE meters reflected luminance only), `r_ExposureThresholdRange` (dynamic-range threshold of the
histogram reduction, not emulated), `r_ExposureTargetEV` 0 (no fixed target: the compensation curve is in
force), `r_HDRDisplayOutput` 0 with `r_HDRDisplayMaxNits` / `r_HDRDisplayRefWhite` (the SDR branch of
`DisplayMapping` is the one ported).

Functions and what was taken from each:

| Function | Role |
|---|---|
| `HuePreservingShoulder`, `GetLuminance`, `ToSRGB` | **ported** to the Custom node of `M_SC_DisplayMapping`; `create_display_mapping_material.py` refuses to build when one of these hashes differs from the one its port was written from |
| `DisplayMapping` | call site: which curve, peak and toe the SDR path uses |
| `CalculateEVCompensation`, `LogLumToEV`, `CalcExposureAndNoise`, `CalcLogLumMetered` | the exposure rule in `FSCStarEngine::CompensationAt` / `LogMultiplierAt` |
| `PupilAdaptation`, `PigmentAdaptation`, `ExponentialDecay` | adaptation model (two stages; UE gets the pupil stage) |
| `ParallelReduce_Range` | histogram reduction (discard ratios, threshold range) |
| `MaterialGlowToLinearGlow`, `CalculateEmissive` | `GlowNits` (material term) and `InstanceGlowNits` (instance term) |
| `GetPhysicalLightAttenuation` | `PointCandela`: `1 / (tweak · (d − bulb) / bulb + 1)²` |
| `PlanarLightDiffuse_LightSpace`, `DiffuseLambert` | `PlanarCandela` and the Lambert π folded into the light colour |

When a hash changes: decode the function again (`--dump` plus the token reader in the validator), compare it
with the rule above, update the port or the rule and its tests, then update the hash here and in the port guard.

## Inferences (not read from the game)

These are stated in WP-U3b §13 with their evidence; they are the places a future contradiction would land.

- The CPU-side normalisation of a point light's intensity includes the attenuation tweak (so that the authored
  intensity is reached one metre from the bulb surface for any tweak). The shader shows only the GPU half.
- Planar lights are not normalised on the CPU: the authored intensity is the panel's luminance over π.
- An entity-effect Glow value (DataCore `materialEffects`, `materialProperty == Glow`; `controls.engine_glow`) is the
  instance-glow amount as the shader receives it. The shader multiplies `instData.glow` by a per-material glow
  factor (`GetPerInstanceGlowFactor`, constant `PM_TintOverWrite_Opacity_fAlphaRef_GlowFactor.w`) whose value,
  and any CPU-side scaling between the effect value and `instData.glow`, were not established (taken as 1).
  Evidence for the reading: WP-U3b §13.3.
- The executable's cvar defaults are the values in force. Checked for this build: none of the 53 `.cfg` files in
  `Data.p4k` (`starbreaker p4k extract --filter '**/*.cfg'`, including every `Engine/Config/CVarGroups/sys_spec_*.cfg`)
  sets an `r_Exposure*`, `r_HDRTonemapping*` or `r_HDRDisplay*` cvar, and the install has no `user.cfg`. The
  validator does not repeat this search; a later build that adds such an override would not be seen by it.
