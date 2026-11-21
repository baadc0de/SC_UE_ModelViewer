# LOOK-9 — HDR highlights and bloom

The Windows player can use UE's Standard ACES display transform when the window is presenting PQ or scRGB. It takes graded scene colour, the display's maximum luminance and paper white. The original game display method applies its 80-nit SDR shoulder, saturates the result, then feeds UE's HDR expansion. Once two highlights have reached that shoulder's ceiling, changing the display peak cannot recover their difference.

The runtime selects the transform after the player's view has evaluated its post-process volumes. It follows the actual output target each frame. `sc.Look.NativeHDR 0` restores the previous game-curve expansion for comparison. An explicit `sc.Look.Display ue` still defers to the project tonemapper. SDR uses the installed game's original shoulder. H2 renders the graded SDR PNG and scene-linear EXR through its fixed output devices.

Windows allows HDR through `r.AllowHDR` and requests it in the default game user settings. UE checks display support and honors previously saved HDR preferences. Paper white follows the platform's report by default. For a controlled HDR comparison use `r.HDR.PaperWhite.Mode 1` and `r.HDR.PaperWhite 203`, then return the mode to 0. Absolute monitor luminance requires an instrument and a specified window size. An SDR proof or the window's declared peak can only establish part of the display chain.

## Visual references

Jonrellim's original [C1 cockpit](https://starcitizen.tools/File:CRUS-C1-spirit-cockpit-HDR.jpg) and [Anvil show floor](https://starcitizen.tools/File:Invictus-2954-day7-anvil-showfloor-HDR.jpg) are 2024 in-game photographs containing SDR base renditions and Adobe HDR gain maps. Their XMP specifies four stops of maximum rendering headroom. The doorway strip leaves the trim seams readable in the C1. On the show floor the overhead sign reads brighter than the white painted ship, with shaded panels still distinct. The photographer's export includes rendering choices. Its metadata does not establish a required monitor peak for SCViewer.

More recent composition references include the photographer's [2025 Star Citizen Alpha 4.0–4.5 collection](https://www.hasgaha.com/2026/01/12/my-42-favorite-star-citizen-screenshots-from-2025/) and a [November 2026 Galaxy PTU galley comparison](https://www.reddit.com/r/starcitizen/comments/1y0n4hd/galaxy_ptu_kitchen_was_updated_with_higher/). Their published previews have no verified HDR mastering metadata.

The 2026 Crimson Desert Serkis Estate original provides a contemporary HDR night scene from another game. The photographer describes an unedited Steam capture at midnight. The original 3840 × 2160 AVIF identifies BT.2020 primaries and the PQ transfer function. The photographer reports a roughly 500-nit peak. The relationship between the lit sky, torchlight and shaded terrain makes a better comparison than matching that number across games. [Original capture](https://eu.zonerama.com/PrestigiousCap4934/Photo/15065535/609355335) and [photographer's description](https://www.reddit.com/r/HDR_Den/comments/1sgr2ui/this_is_crimson_desert_native_hdr_screenshots/).

## Controlled comparison

Use the same settled camera, exposure, resolution, antialiasing, lights and Surface Cache for each pair. Keep the LOOK-8 exposure bias of −0.10 EV. In the first pair change only `sc.Look.NativeHDR 0/1` on the same HDR target. For bloom, use standard bloom, threshold 1.0, and compare `sc.Look.PP BloomIntensity 0.10`, `0.14` and `0.18` at fixed display settings. Judge the emitter core, the separation between neighboring control marks, the adjacent trim, and the difference between diffuse white paint and small reflected glints. The H2 PNG provides an independent SDR bloom proof. A native-HDR comparison needs the display output transform and its encoding.

The synthetic GPU check is `SCBridge.RenderGpu.FrozenOutputDevice`. A 1000-nit PQ target receives frozen neutral scene values of 1, 4 and 16, alongside the existing dark and colored patches. It compares the separation of the two brighter values between transforms and checks that both H2 outputs remain identical across synthetic SDR and HDR targets.

## Win64 verification

The Development Editor build and `SCBridge.RenderGpu.FrozenOutputDevice` passed. At neutral scene levels 1, 4 and 16 the native transform emitted PQ codes 0.55518, 0.66748 and 0.72559, equivalent to 159, 459 and 786 nits on the synthetic 1000-nit target. The previous curve placed 4 and 16 at 735 and 903 nits. Native HDR reserved more separation for high scene values. The H2 PNG and EXR each had zero differing RGB pixels out of 2,048 between simulated display devices.

Three matched 1600 × 900 player cameras used Surface Cache, the LOOK-8 exposure settings and the standard bloom intensity of 0.10. Windows Advanced Color and UE HDR output were active. The live player selected output device 5 (scRGB), and native viewport screenshots retained values above reference white in floating-point EXRs. Windows reported 1030 nits of peak capability. No instrument measured the physical panel. The table gives Rec.709 luminance from the scRGB signal in nits (1.0 = 80 nits), with one capture per mode and scene. [Microsoft's scRGB convention](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range).

| Player camera | Previous median | Native median | Previous 99.9th percentile | Native 99.9th percentile | Previous peak | Native peak |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Cutlass cockpit | 0.15 | 0.12 | 312 | 146 | 988 | 940 |
| Cutlass cargo | 0.16 | 0.14 | 228 | 268 | 925 | 834 |
| Aeroview medium hangar | 8.50 | 8.34 | 305 | 223 | 988 | 940 |

Astra reviewed SDR viewing copies with a common 203-nit white anchor and the same compressive transfer for every image. The native hangar kept the overhead fixture distinct from the white A1 paint, and the blue floor markings were less neon. In the cargo bay the amber paint was easier to distinguish from the pale lamps, with the deck grate and rail edges intact. The cockpit kept the worn windscreen and dark bezels, with less competition from small white highlights. These findings support the native HDR transform. Physical appearance on other HDR monitors remains unmeasured.

The standard bloom intensities of 0.10, 0.14 and 0.18 at threshold 1 looked almost identical at normal scale in all three scenes. The strongest variant slightly softened strip edges without a visible veil. The first comparison established no convincing preference across that narrow range.
