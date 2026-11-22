# WP-I1 light style playback

Each authored light state references a style ID. The exporter reads the
`LightStyles` table in `Light.cfxb` from the installed game's nested Vulkan or
D3D11 shader cache. The scene carries the table once and retains each light's
speed and phase. A bounded ZIP reader opens the member in place.

The LIVE renderer applies speed to time, adds the phase byte divided by 255
times the sample rate, then converts to sample space with that same rate. The
viewer uses the same periodic linear interpolation, including the last-to-first
edge. It samples the active state against the world clock. A fully dark sample
uses black light color so the light stays registered across pulse edges. State
changes update the light immediately. Older packages without the table still
load with steady lighting.

The source table contains 89 definitions. Style 0 is the renderer's unanimated
state. The viewer loads the other 88. Group transition delays, TrackView
sequences and linked emissive surfaces are outside preset waveform playback.

## Verification

The installed Vulkan and D3D11 tables match across all 89 definitions and
5,299 samples. Both fresh ship exports carry the same table, with each light's
authored speed and phase. The Win64 Development Editor build, 75 Rust light
tests and all 14 `SCBridge.Lights` automation tests passed.

The XL hangar runs used D3D12 and Lumen Surface Cache. Each ship was loaded
in a separate Unreal session. A live probe measured changing light output
in Default and Emergency, then confirmed playback when returning to Default.

| Ship | Imported light components | Default loops | Emergency loops |
| --- | ---: | ---: | ---: |
| Spirit C1 | 346 | 10 | 268 |
| Cutlass Black | 225 | 51 | 86 |

The C1 count excludes the existing 22 ambient proxy records. The C1 nose
pulse kept its phase of 3, and Cutlass cockpit style 14 kept its 0.7 speed.
The authored group interactions switched two C1 cockpit lights, 52 Cutlass
cockpit lights and 32 Cutlass exterior lights through Emergency and back to
Default. Hull-attached lights now use the source container of their light
group while staying physically attached to the hull helper.
