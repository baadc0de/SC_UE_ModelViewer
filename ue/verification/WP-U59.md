# WP-U59 — Unreal Engine 5.9 and the Lumen World Cache

Host: UE **5.9.0 preview-1** (`5.9.0-60351404+++UE5+Release-5.9`) at `~/UnrealEngine/5.9`, Vulkan SM6,
RTX 4090, Star Citizen LIVE 4.10.2. It replaces 5.8.2 (plan D9); the 5.8 install was removed after the
checks below passed. Decision: plan D13 (`r.Lumen.WorldCache.Supported=1` in `DefaultEngine.ini`).

## Install

Epic's Linux archive (unrealengine.com/linux) lists only `Linux_Unreal_Engine_5.9.0_preview-1.zip` for 5.9
(41,704,882,698 bytes, 315,407 entries, 81.3 GB uncompressed). It was extracted entry by entry from the
end of the zip, checking each entry's CRC-32 and size and truncating the zip behind it, so the peak disk use
was the extracted size instead of zip + extracted. All 315,407 entries verified; 7,578 executables kept
their mode bits.

## Build

`ue/scripts/build.sh SCViewerEditor` — `Result: Succeeded`, no warnings from project or plugin code. Targets
moved to `BuildSettingsVersion.V8` / `EngineIncludeOrderVersion.Unreal5_9` as in the 5.9 templates (V8 only
stops compiling `Tests/` folders; our automation tests live in `Private/`).

## Headless (null RHI)

```bash
E="$HOME/UnrealEngine/5.9/Engine/Binaries/Linux/UnrealEditor-Cmd"; P="$PWD/ue/SCViewer/SCViewer.uproject"
C="-nullrhi -unattended -nop4 -nosplash -ddc=SCViewerDDC"
"$E" "$P" -run=pythonscript -script="$PWD/ue/scripts/test_importer.py" $C
for n in cutlass gladius scorpius; do
  "$E" "$P" -run=pythonscript -script="$PWD/ue/scripts/test_first_person.py" -SCFirstPersonTests -SCPackage="$PWD/out/dec-p3-$n" $C
  "$E" "$P" -run=pythonscript -script="$PWD/ue/scripts/test_rig.py" -SCPackage="$PWD/out/dec-p3-$n" $C
done
uv run python ue/scripts/make_test_tar.py out/dec-p3-cutlass ue/SCViewer/Saved/Import/u59-cutlass.tar
"$E" "$P" -run=pythonscript -script="$PWD/ue/scripts/test_importer.py" -SCPackage="$PWD/out/dec-p3-cutlass" \
  -SCVerifyTar="$PWD/ue/SCViewer/Saved/Import/u59-cutlass.tar" $C
```

| Suite | Result |
|---|---|
| importer unit tests | 35 passed, 0 failed, `UNIT_TESTS_OK` |
| importer + Cutlass package + tar | 37 passed, 0 failed, `UNIT_TESTS_OK` |
| first person ×3 | `WP-P1 HEADLESS_OK`, `FSCFirstPersonCollision passed=1` on Cutlass, Gladius, Scorpius |
| rig ×3 | `WP-U3 RIG_TESTS_OK`, 0 errors; `RIG_VERIFY passed=1` (Cutlass clips 55/bound 53, Gladius 64/63, Scorpius 75/71) |

## GPU

```bash
SC_TIMEOUT=3600 ue/scripts/capture_route.sh u59-wc-warm ue/scripts/routes/cutlass-world-cache.txt \
  -SCDefaults -SCLoad=DRAK_Cutlass_Black -SCCamera=first
SC_TIMEOUT=1800 ue/scripts/capture_route.sh u59-cutlass ue/scripts/routes/cutlass-ramp-to-cockpit.txt \
  -SCDefaults -SCLoad=DRAK_Cutlass_Black -SCCamera=first
```

**World Cache active.** The log sets `r.Lumen.WorldCache.Supported:1` and allocates
`Lumen.WorldCache.{Accumulation,HashTable,Lighting,Header}`; `ProfileGPU` in the cargo bay shows a
`LumenWorldCache` pass (async compute) next to `LumenScreenProbeGather` and `LumenReflections`, and no
Surface Cache pass (scene update, card capture, scene lighting, radiosity). `r.Lumen.WorldCache.Debug 1`
draws the cache statistics overlay. Same view, World Cache vs Surface Cache (`r.Lumen.WorldCache.Allowed 0`):
GPU 7.28 ms vs 6.64 ms mean (+0.64 ms); the two images differ only in the darkest recesses.

**Regression route.** `cutlass-ramp-to-cockpit` runs all 81 steps; every shot lands within 2.1 cm of the
5.8.2 run of the same route (`p3c-cutlass`, WP-P3), wall/bulkhead blocks, noclip refusal and fall recovery
behave the same. Shots 02/05/08/10/12 side by side (`out/smoke/u59-vs-58-cutlass.jpg`): same geometry,
materials, emissives and lights; mean viewport luminance within 1.5 % (5.9 marginally darker in the darkest
areas). Frame cost, first person / orbit / first person:

| | frame ms (mean) | GPU ms (mean) |
|---|---|---|
| 5.8.2, Surface Cache (`p3c-cutlass`) | 7.80 / 6.30 / 8.04 | 6.12 / 5.71 / 6.10 |
| 5.9.0, World Cache (`u59-cutlass`) | 11.53 / 9.10 / 9.92 | 7.38 / 6.37 / 7.34 |

The 5.9 frame times were taken while an unrelated `rustc` build loaded the CPU (no other GPU client);
the GPU column is the comparable one: +1.2 ms in first person, of which ~0.6 ms is the World Cache.

Known, unchanged from 5.8.2: the transient `Invalid Lod 0 for Rendering Asset: SkeletalMesh` warnings during
assembly (also in the 5.8.2 logs) and the red on-screen message at the top of the viewport (present in the
5.8.2 shots too). MegaLights is still marked Experimental in 5.9.0.
