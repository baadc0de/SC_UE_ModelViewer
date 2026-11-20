# WP-A1 — Audio: the game's own ambience and action sounds, played from its data

Branch `wp/A1-audio`. Exporter and crate changes go on a submodule branch `scviewer/audio`.
(The id `WP-A1` belonged to the Tauri viewer packages dropped on 2026-11-09; this brief reuses it.)

**Depends on:** WP-P1 (first person, doors), WP-P5 (lifts, `FSCLift::OnPhase`), WP-I1 (panels and their press
path; I1 §4 T4 lists "the button sound" as an effect-tag sound), WP-R2/R3 (clips with `sources`), WP-P2 (player kit,
weapon, flashlight). **Build locks:** cargo lock (crates, exporter, FFI), UE lock (plugin).
**Status:** research and plan (2026-11-20). No code yet. The findings log with every file, command and script
is `out/audio-datamine/NOTES.md`.

## Goal

The viewer plays Star Citizen's own sounds where and when the game plays them:

- **ambience:** ship room tone, interior hum and machinery spots, hangar ambience, light hums, engine idle;
- **actions:** doors opening and closing; the lift (start, travel loop, stop, gate, belly hatch); panel buttons;
  footsteps; the flashlight toggle; weapon draw, aim and fire; landing gear and ramps.

Like everything else in the viewer, all of it comes from the game files:

- which sound plays, where it plays, and on which event;
- how it attenuates over distance;
- how a sound is varied: random or sequence variations, loop, volume and pitch.

There are no invented sounds and no event name written per asset. Game audio is game-derived data: it never enters
git, and it is decoded at export time into `out/`, the cache or an in-memory package.

`docs/01-starbreaker-analysis.md` lists `starbreaker-wwise` / `-wem` as "not relevant"; that verdict is outdated and
should change in the PR that lands A1-1.

## Read first

- `AGENTS.md`;
- `out/audio-datamine/NOTES.md`;
- `vendor/StarBreaker/crates/starbreaker-wwise/src/{atl.rs,datacore_audio.rs,hirc/mod.rs,hirc/attenuation.rs}`
  and `vendor/StarBreaker/crates/starbreaker-wem/src/vorbis.rs`;
- `vendor/StarBreaker/cli/src/wwise.rs`;
- `docs/work-packages/WP-P5-lifts.md` §1, §6;
- `docs/work-packages/WP-I1-door-and-lift-panels.md` §3, §5;
- `ue/README.md`, §First-person exploration.

## 1. How the game binds audio

Evidence: LIVE 4.10.2, the prebuilt CLI, DataCore `Game2.dcb`, C2 `.soc` layers cut and converted, ATL documents,
banks dumped (`out/audio-datamine/`).

### 1.1 The trigger dictionary: ATL

Everything in the game names a sound by an **ATL trigger name** (CryEngine's Audio Translation Layer), never by a
file.

- **Where.** `Data/Libs/GameAudio/*.xml`: 2,416 documents (CryXmlB, a few as text), holding 62,657 `ATLTrigger`,
  5,196 external sources, 2,742 switch states and 1,164 RTPCs.
- **What a trigger carries.**
  - The Wwise event (`WwiseEvent wwise_name`).
  - `wwise_duration_type`: `OneShot` or `Infinite`.
  - `wwise_duration_min` and `wwise_duration_max`.
  - `wwise_radius_max`, the attenuation's maximum distance.
  - RTPC dependees.
  - An `ATLPreload` whose `WwiseFile` names the bank.
- **Example.** `SC_SSMP_CRUS_StarLifter.xml`: `Play_SSMP_CRUS_StarLifter_Elevator_Door_Top_Open` has duration
  4.0308 s, `OneShot`, radius 10, bank `SSMP_CRUS_StarLifter.bnk`.

File prefixes are the sound categories:

| Prefix | Category |
|---|---|
| `SSAM` | ship ambience |
| `SSMP` | ship moving parts |
| `SSSW` | ship sweeteners |
| `SSTH` | thrusters |
| `ENVA` | area ambience |
| `ENVP` | placed props |
| `MCDR`, `MCAN` | doors |
| `CMFS` | footsteps |
| `CMMV` | movement foley |
| `WPFA` | FPS weapons |
| `CGHL` | helmet light |
| `UI*` | user interface |

### 1.2 Four routes from the world to a trigger

| Route | Where | Fields | Fires on |
|---|---|---|---|
| **A. Placed audio entities** | the `.soc` of every object container | `AudioTriggerSpot` (`AudioTriggerSpotParams`), `AudioAreaAmbience` + `AreaShape`, `VehicleAudioPoint`, `VibrationAudioPoint`, `AudioEnvironmentFeedbackPoint`, `LightGroup` state triggers | load (loops), timers (`playRandom`), the listener entering an area |
| **B. Entity effect components** (DataCore) | `SEntityComponentEffects.entityEffects.soundEffects` | `triggerEffects[]` (a trigger **tag** fires a one-shot) and `tagEffects[]` (a sound plays while a state **tag** is set), each with `soundEffect.audioTrigger`, `attachmentSettings` (bone + offset), `multiPosition` | entity-system tags such as `EntityEffectSystem/Triggers/Elevators/RampUp` and `…/Interactions/Activate` |
| | `SEntityEffectCoreParams.EffectParams[SEffectParamsNodeSound].SoundParams` | `Name` (the trigger), `tag` (effect group), `Helper`, `IsLooped`, `Enabled` | door effect groups `OpenStart/OpenLoop/OpenEnd/CloseStart/CloseLoop/CloseEnd` (`SCItemDoorParams.*EffectGroup`) |
| | item components | `SCItemWeaponComponentParams` (fire, aim, dry fire, fire-mode switch, report tails), `SCItemFlashlightParams.Light_On_SFX/Light_Off_SFX`, `EntityPhysicalAudioParams`, `ShipAudioComponentParams` (thrusters, take-off, landing), `SCItemSuitArmorParams.automaticFootstepDef` | the item's own action |
| **C. Animation events** | `.cga/.chr` → `.chrparams` `$AnimEventDatabase` → `.animevents` keyed by CAF path | `audio_trigger` (played forward), `audio_trigger_reverse` (played backward), `audio_trigger_bothdir`, `audio_rtpc`, each with normalised `time`, `parameter` (the trigger) and `bone` | the clip crossing that time |
| **D. Mannequin procedural clips** | `.adb` fragments | `<Procedural type="Audio">` or `type="PlaySound"` with `StartTrigger`/`StopTrigger` (4,418 + 481 clips in 262 ADBs) | the fragment playing |

`starbreaker-wwise::datacore_audio` today captures only fields named `audioTrigger`. It misses route B's
`SoundParams.Name`, the footstep map's `heelLandAudioTrigger`/`footLiftAudioTrigger`, the melee `audioTriggerName`,
and routes A (`.soc`), C and D entirely.

### 1.3 The C2 Hercules (`CRUS_Starlifter_C2`)

**Placed ambience.** Counts across all C2 layers:

- 1,550 `AudioTriggerSpot` (ATS);
- 9 `AudioAreaAmbience` (AAA);
- 66 `VibrationAudioPoint`;
- 16 bridge `VehicleAudioPoint`, flight sweeteners with `loopStart`/`loopStop`;
- 5 `AudioEnvironmentFeedbackPoint`.

That is 164 distinct triggers. The `_default` and `_emergency` light-state layers hold no audio.

- **Spots.** `top_deck_c2` has 566 ATS. Each of the 246 that carry a trigger has:
  - a position;
  - `audioTriggerPlayTriggerName` (for example `Play_SSAM_CRUS_StarLifter_Vent_Detail_04`);
  - `volume` (dB), `pitch`, `pitchRandomRange`, `volumeRandomRange`;
  - `attenuationScalingFactor` (for example 1.5);
  - 20 of them use `playRandom` with `minDelay`/`maxDelay`/`delayIsFromEnd`: incidental creaks and clicks.

  The other 320 carry no trigger. They are **extra positions of a multi-position spot**: 318 `NewLink` EntityLinks go
  from a triggered ATS to them. One light hum plays at many light positions.
- **Areas.** An `AreaShape` (a polygon plus a height; its `<Entities>` names the AAA) → `AudioAreaAmbience`. The AAA
  carries:
  - `audioPlayTrigger`, `volume`, `atsVolumeOffset`;
  - `fadeDistance`, `environmentFadeDistance` and `positionFadeDistance` (5 m by default);
  - `areaFadeRTPC area_fade_distance`;
  - EntityLinks to the ATS it governs (241 on the top deck).

  Top deck: `AS_Top_Deck_C2-001` (35 points, height 4.5) → `AAA_Top_Deck_C2-001`
  `Play_SSAM_CRUS_StarLifter_Top_Deck_C2_01`, volume 2, ATS offset −6. The others are `Hold_01`, `LiftBox_01` (two),
  `Systems_C2_01`, `Crew_Quarters_01`, `Kitchen_01`, `Escape_Pods_01` and `Bridge_01`.
- **Light groups.** `LightGroup` carries 10 state play/stop triggers. All are empty in the C2 (209 groups) and in the
  hangar (1,472). Light hums are ATS (`*_Lighting_NN`).

**What the action cases reference:**

| Case | Entity / asset | Route | Trigger(s) | Position |
|---|---|---|---|---|
| Top lift door | `CRUS_Hercules_Elevator_Door_Top` → `CRUS_Starlifter_Lift_Doors_Top.cga` → `.chrparams` → `crus_starlifter_lift_doors_top.animevents` | C | `…_door_open.caf`: `audio_trigger` t 0.0001 `Play_SSMP_CRUS_StarLifter_Elevator_Door_Top_Open`, `audio_trigger_reverse` t 0.9999 `…_Top_Close` (and the mirror for `…_door_close.caf`) | bone `CRUS_Starlifter_Lift_Doors_Top` |
| Hold lift door | `CRUS_Starlifter_Lift_Doors.cga` → `crus_starlifter_lift_doors.animevents` | C | `…_Elevator_Door_Bottom_Open/Close`. As authored, `…_lift_doors_close.caf` plays `…_Bottom_Open`: open and close are swapped in the data | bone `Door_Lift_Top` |
| Other C2 doors, lockers, seats, dashboards | `CRUS_Starlifter_{SideDoor,CircleDoor,Door_*,…}.chrparams` → `crus_starlifter_*.animevents` (27 files) | C | `Play_SSMP_CRUS_StarLifter_*` | their bones |
| Lift carriage | `CRUS_Hercules_Lift_Platform` `SEntityComponentEffects` | B | trigger tag `Elevators/RampUp` → `Play_SSMP_CRUS_StarLifter_Elevator_Move_Start`; `Elevators/Stop` → `…_Move_Stop`; state tag `Elevators/InTransit` → `…_Move_Loop` (`Infinite`) | bone `roomlg_elevator_platform_08x06x04_a` + (3, −3, 2). The C2 platform CGA has no such bone: a template leftover (WP-P5) |
| Carriage gate | `CRUS_Starlifter_Elevator_Inner_Gate.chrparams` | — | no `$AnimEventDatabase`, no effect sound: the game plays nothing | — |
| Belly hatch | `Door_Ship_Exterior_CRUS_Starlifter_Elevator_Door` | B | Door Open / Close / Opened / Closed → `Play_Silence` (authored silence); its `.chrparams` has no events | — |
| Lift buttons (`ButtonsInner`, `ButtonsCall`, `Buttons_Exterior`) | `SEntityComponentEffects` | B | trigger tag `Interactions/Activate` → `Play_UI_Levski_Elevator_Button` (bank `UI_MCCS_Lift_Controls.bnk`) | bone `SCItem_MOLever` |
| Ramps, landing gear, nacelles, weapon hatches | `CRUS_Starlifter.chrparams`, `Starlifter_Rear_Ramp.chrparams` … → `crus_starlifter.animevents`, `crus_starlifter_rear_ramp.animevents` | C | `Play_SSMP_CRUS_StarLifter_Landing_Gear_{Front,RearLeft,RearRight}_{Deploy,Retract}` (radius 250), `…_Rear_Ramp_{Deploy,Retract}` (radius 100), `…_Nacelle_*`, `…_Weapon_*` (radius 500) | gear hardpoints, `Rear_ramp_FloorLevel` |
| Engine idle / thrusters | `CRUS_Starlifter_C2` `ShipAudioComponentParams.playerParams.thrusterParams[]` | B | `Play/Stop_SSTH_CRUS_Starlifter_{Primary,MAV,Retro}_LP`, `…_Engine_On/Off`, take-off, landing; ship size switch `SW_Spaceship_Size:Large` | driven by flight state and RTPCs (later slice) |

### 1.4 The hangar `hangar_xltop_001`

- **Areas.** 3 AAA:
  - `AAA_hangar_xltop_001_sfx`: `Play_ENVA_Hangar_Room_Tone_Tonal_Dark_01`, fade 20 m (environment and position fade
    30 m), volume 10, ATS offset 10;
  - two `AAA_e56a_sfx_int`: `Play_ENVA_RoomTone_Large_Corridor_Heavy`, fade 28 m.
- **Spots.** 228 ATS with 65 distinct `ENVP_*` triggers (EMF interference, server hum, pipes, machinery, room tones).
- **Loading platform.** Sound comes through route B (`SEntityEffectCoreParams`):
  - `LoadingPlatformLoadingGate`: `Play_MCDR_Metal_Door_Open/Close/Opened/Closed` and `_Override`;
  - `LoadingPlatformFrontGate`: `Play_MCDR_Lorville_HabModule_Door_Entrance_Open/Close`;
  - `LoadingPlatform`: `Play_Connie_Andromeda_Cargo_Bay_Base_01`.

### 1.5 The player kit

- **Footsteps.** Steps are detected by code from foot motion: locomotion CAFs carry no `footstep` events; the 270
  that exist are in emotes and ladders. The data:
  1. Footwear: worn armour `SCItemSuitArmorParams.automaticFootstepDef` (undersuits → `armor_boot_undersuit_default`),
     else `Player.baseFootstepDef` (`barefoot`).
  2. That points to a `FoleyFootstepDefinition` (29 records). It holds:
     - `velocityRTPCName Footstep_Speed`;
     - detection thresholds `footMovementThreshold 0.01`, `footRotationThreshold 10`, `heightThreshold 0.3`;
     - user RTPCs;
     - `surfaceMap.audioSurfaces[]`: 36 surface types, each `surfaceType` → `heelLandAudioTrigger` /
       `footLiftAudioTrigger` (for example `carpet` → `Play_CMFS_HM_Material_Carpet_Down/_Up`).
  3. The surface type comes from the ground's material; `SurfaceAudioPropertiesDefinition` maps each surface type to
     the switch `SurfaceType:<name>`. The viewer does not carry material surface types to the runtime yet (WP-K4
     notes the same gap).
- **Flashlight.** `SCItemFlashlightParams.Light_On_SFX` / `Light_Off_SFX`: helmets use
  `Play_CGHL_Spacesuit_Helmet_Light_On/Off`; the weapon attachment uses `Play_WPFA_Attachments_Flashlight_On/Off/Loop`.
- **Weapon** `behr_rifle_ballistic_01`:
  - **fire:** `fireActions[Rapid]` `Play/Stop_WPFA_BEHR_Rifle_P4AR_Fire_Auto`; `[Single]` `…_Fire_Single`; dry fire;
    fire-mode switch;
  - **aim:** `aimAction.aimStart/aimStop` → `…_ADS_Raise/_ADS_Lower`, with RTPC `Weapon_ADS`;
  - **report tails:** `audioReportEnvironmentParams`, 10 tails chosen by environment
    (`Play_WPFR_Report_Rifle_Medium_INT_{VerySmall,Small,Medium,Large}`, EXT, cave);
  - **draw and holster:** route D in `playerSounds.adb` (scope "sound"): fragments `select`/`deselect` with tags
    `<weapon>+stocked+sound` → `Play_WPFA_Handling_BEHR_Rifle_P4AR_Select` / `_DeSelect`;
  - **handling:** `EntityPhysicalAudioParams` (pickup, place, movement loop).
- **Foley.** Clothing foley (`CMMV`) and `audio_rtpc Automatic_Foley_Boost` come through animevents; this is a later
  slice.

## 2. The audio data path

### 2.1 Where it lives in `Data.p4k`

- `Data/Libs/GameAudio/*.xml`: ATL, 67.9 MB.
- `Data/Sounds/wwise/*.bnk`: 2,082 banks, plus 328 in `English(US)/`; 113 MB in all. Bank version 154; sections
  BKHD and HIRC, and DIDX/DATA in only 139 banks.
- `Data/Sounds/wwise/Init.bnk`: buses, aux buses, effect share sets and the global attenuations.
- `Data/Sounds/wwise/Media/<id>.wem`: 77,896 streamed or loose WEMs, 9.8 GB, plus 40,588 in `Media/English(US)/`
  (voice), 1.35 GB. Every WEM met is Wwise Vorbis.

### 2.2 From a trigger name to playable media

1. **Trigger → event and bank.** ATL trigger name → `WwiseEvent wwise_name` and the bank (`ATLPreload` →
   `ATLPreloadRequest` → `WwiseFile`).
2. **Event id.** FNV-1 32 of the lower-case event name (`starbreaker_wwise::fnv1_hash`) → the `Event` in the bank's
   HIRC.
3. **Event → actions.** Play is `0x0403`; also Stop, SetSwitch, SetState and others → the target node.
4. **Node tree.** The target is a tree of `ActorMixer` / `RanSeqContainer` (random or sequence: weights, avoid-repeat,
   loop count, transition mode and time) / `SwitchContainer` (switch group, default, branch map) / `BlendContainer`
   (all children together; RTPC crossfade layers) down to `Sound` leaves (`media_id`, plugin; prop `74` = loop count,
   0 = infinite).
5. **Media.** Codec sounds (`plugin_id & 0xF == 1`) → `Media/<id>.wem` when that file exists, else the DIDX/DATA of
   **any** bank. HIRC says `Embedded` either way. Example: 96 media of `SSAM_CRUS_StarLifter.bnk` live in
   `SSAM_CRUS_Spirit.bnk` and 4 in `SSAM_CRUS_StarFighter.bnk`. Source plugins (`0x640002`, 8 in the SSMP bank) have
   no media.
6. **Inherited properties.** Walk `direct_parent_id` up from the played node:
   - volume and pitch add along the chain;
   - positioning and attenuation come from the nearest ancestor that overrides positioning (`bits_positioning` 3);
   - prop `75` = AttenuationID → an `Attenuation` object in the bank or in `Init.bnk`.

Worked chains (`trace.py`, NOTES §15):

- **Top lift door.** `Play_SSMP_…_Door_Top_Open` → Blend → Sound 603652163 → media 872222338 (loose WEM, 25 KB,
  4.03 s). Attenuation 463427172, inherited from ActorMixer 917279810: the volume curve is flat to 1 m and falls to
  silence at 10 m (the ATL radius), with LPF, HPF and spread curves to 10 m.
- **C2 top-deck ambience.** `Play_SSAM_…_Top_Deck_C2_01` → Blend of three looping streamed beds plus a nested Blend
  holding a random container (infinite loop, avoid-repeat 3, 1,000 ms crossfade) over four streamed sounds.
- **Incidental pipe creaks.** A random container of 15 variations with avoid-repeat 10. The ATS `playRandom` timer
  re-triggers it.

**Coverage** (`c2_trigger_coverage.txt`). Every trigger the C2 (`.soc`, animevents, DataCore) and the hangar
reference: 288 distinct.

- **263 resolve to playable media.** 22 of them resolve only through another bank's DIDX.
- **23 have no media:** `Stop_*` triggers, or sources that are plugins only.
- **1 is missing from ATL:** `Play_SSMP_CRUS_Starlifter_A2_Bomb_Mech_Door_Retract`, an A2-variant data bug. A second
  entry is noise from the extraction regex.

**Media budget (Vorbis, as stored):**

| Source | Media | Size |
|---|---|---|
| C2 `.soc` ambience | 468 | 106.5 MB |
| C2 doors, ramps, gear | 48 | 3.0 MB |
| Lift and buttons | 4 | 0.1 MB |
| Hangar | 139 | 26.4 MB |

### 2.3 What the vendored crates do today

| Capability | Status |
|---|---|
| BKHD, DIDX/DATA, HIRC, STID parse | yes (`BnkFile`) |
| HIRC types | Sound, Action, Event, RanSeq, Switch, ActorMixer, Blend, music, buses, modulators, FX parse; **Attenuation (type 14) fails on v154**: 350 of 354 in 20 banks come back `Unknown`. The parser lacks one leading flag byte. With `u8 flag, u8 cone, [5×f32 cone], 19×i8 curve_to_use, u8 n, n×(u8 scaling, u16 count, count×(f32, f32, u32 interp)), InitialRtpc`, 63 of 64 sampled blobs end exactly at the empty RTPC list |
| Event resolution | `Hierarchy::resolve_event`: Play actions only. It returns the union of all leaves (every random and switch branch), ignores props, loop counts and inheritance, and never reads the attenuation |
| Media lookup | per bank: DIDX, else `Media/<id>.wem`. It never searches other banks' DIDX, so CLI `list` shows those media as `?` and `extract` skips them |
| ATL | trigger → event, bank, duration type, radius (`AtlIndex`); external sources per language. Not yet read: `wwise_duration_min/max`, RTPC, switch and environment tables |
| DataCore | `find_audio_triggers` / `search_entities_with_audio`: fields named `audioTrigger` only |
| `.animevents` | `starbreaker-3d::chrparams` reads `$AnimEventDatabase`; `parse_animevents_targets` returns only bone and parameter hints, without event name, time or direction |
| WEM decode | `starbreaker-wem`: Vorbis → Ogg (ww2ogg, default then aoTuV codebooks, validated). Opus, PCM and ADPCM are detected but not decoded (none met in the C2 or hangar data) |
| Tests | unit tests on synthetic RIFF, HIRC and ATL documents; no game data checked in |
| CLI | `starbreaker wwise info\|list\|extract [--decode]\|events\|trace\|dump\|search --trigger\|--entity` (all work with the prebuilt binary) |
| App | Tauri `audio_*` commands: search, resolve trigger, bank media, decode WEM, export |

### 2.4 3D and variation parameters in the banks

- **Attenuation.** It is recoverable once the v154 fix lands:
  - curves on distance for dry volume, game and user wet volume, LPF, HPF, spread and focus (`curve_to_use` maps
    each slot to a curve);
  - scaling type (dB, linear);
  - interpolation per point (Wwise `AkCurveInterpolation`: 4 linear, 9 constant, and the log, exponential and
    S-curve shapes);
  - cone (inside and outside angles, outside volume, LPF/HPF).

  The curve's last x is the maximum distance and equals the ATL `wwise_radius_max`. The ATS
  `attenuationScalingFactor` scales the distances.
- **Variation.**
  - random and sequence containers: weights, avoid-repeat, loop count, crossfade or delay transitions;
  - Blend layers (111 of 464 Blends cross-fade on an RTPC);
  - LFO modulators (84), on pitch or volume;
  - ranged modifiers (random volume and pitch).
- **RTPCs.** 276 nodes in the 20 dumped banks carry RTPC curves; `area_fade_distance` is one of them (it drives
  volume). The ATL `ATLRtpc` table gives the names behind the hashes.
- **Switches and states.** Only 3 `SwitchContainer`s in these banks; 23 nodes carry state groups. Footsteps do not
  use a switch: the data maps each surface to its own trigger.
- **Not portable.** Bus routing, aux reverb sends (428 aux buses, `AudioEnvironment`, CIG's propagation and room
  system) and effect plugins.

## 3. Playback options in UE

| | (a) Decode and play with UE audio | (b) (a) plus the game's container logic and curves | (c) Embed Wwise |
|---|---|---|---|
| **What** | Exporter resolves each trigger to its media, decodes the WEMs to Ogg, writes an audio manifest (trigger → media list, loop flag, volume, attenuation max distance); UE plays `USoundWave`s through `UAudioComponent` with `USoundAttenuation` | (a) with the resolved node tree in the manifest (random with weights and avoid-repeat, sequence, blend layers, loop counts, crossfades, volume/pitch offsets and random ranges) and the attenuation **curves**, run by a small viewer-side player (C++ `USCAudioPlayer`, or MetaSounds where they fit) | The Wwise UE integration loads the game's `Init.bnk` and banks; triggers posted by name |
| **Effort** | S–M (exporter manifest + decode; UE runtime sound wave, emitters, hooks) | M (+ tree player, curve evaluation into `USoundAttenuation` custom curves or a per-voice volume/LPF update) | L–XL (integration, SDK version match, plugins, ATL bridge, packaging) |
| **Fidelity** | right sounds at the right places; a random container plays one variation, crossfades and layering are lost; distance falloff approximated by the max distance | close to the game for everything except mixing: variations, layers, loops, falloff and LPF curves as authored; no buses, no reverb sends, no RTPC-driven behaviour beyond defaults | highest in principle: buses, effects, RTPCs, switches |
| **Licensing and constraints** | game data, never in git; decoded at export into the cache/package (D10) | same | Audiokinetic licence and registration; banks are bank version 154 and need a matching SDK; `Init.bnk` references non-Audiokinetic effect plugins (company ids 100, 256, 259) that we cannot ship; CIG's ATL, propagation and environment logic is engine code, not data, and we would still write the trigger mapping; a second audio engine beside UE's; the standalone deliverable then carries Wwise binaries |
| **Verdict** | the floor | **recommended target**, built incrementally on (a) | rejected |

**Runtime sound waves.** The viewer is a game-mode app that imports packages at runtime (D2–D5). Editor import
factories are not available there. Instead, a `USCSoundWave : USoundWaveProcedural` plays PCM that the FFI decodes
from the package's Ogg (a Rust Vorbis decoder in the FFI, or the exporter's ww2ogg output read back), and it loops
itself. It is still a `USoundBase`, so attenuation, spatialisation, occlusion and concurrency work unchanged.

**Memory.** Decoded PCM is about 10× the Ogg size (C2 ambience: about 1 GB). Short one-shots therefore decode once
and stay cached, while long loops decode as they stream, or sit under an LRU budget.

**Keep as assets.** The editor's "Keep as assets" (and the cooked showcase) can save real `USoundWave`s instead. The
showcase then ships decoded game audio, the same status as its meshes and textures. The owner decides (§9).

**Where the code lives** (D1: no format code of our own):

- **`scviewer/audio` submodule branch, upstreamed with WP-D1:**
  - `starbreaker-wwise`:
    - the v154 attenuation layout;
    - a global media index (loose WEMs ∪ every bank's DIDX);
    - the v154 prop-id table (`0` volume, `74` loop, `75` attenuation, the rest once confirmed);
    - property and positioning inheritance;
    - `resolve_playable(trigger)`, which returns the node tree with modes, weights, loops, volume and pitch, and the
      attenuation;
    - ATL duration, RTPC, switch and environment tables;
    - a DataCore walker for every `GlobalResourceAudio` value whatever its field name, plus
      `SEffectParamsNodeSound.SoundParams.Name`.
  - `starbreaker-3d::chrparams`: a full `.animevents` parser (event name, time, end time, direction, parameter,
    bone).
  - `starbreaker-3d` decomposed export: `audio.json` and `audio/<media>.ogg` in the package.
  - Each change comes with a failing synthetic test first. Fixtures are synthetic, never extracted game audio.
- **The viewer:**
  - `scviewer-core`: the package and cache version;
  - `scbridge-ffi`: an Ogg → PCM decode entry point, if the decode runs at runtime;
  - `ue/Plugins/SCBridge` Runtime module: `USCSoundWave`, `USCAudioPlayer`, emitters, hooks.

## 4. Hooks: the viewer event that fires each case

| Case | Game binding | Viewer event (today's code) |
|---|---|---|
| Door open / close (player, sensor or lift) | route C on the clip's source CAFs | `ASCVehicle::Tick` (`SCVehicle.cpp:236–252`) advances each `Playback` from `Previous` to `P.Frame`. Fire every event of the clip's `sources` whose normalised time is crossed: `audio_trigger` when `Speed > 0`, `audio_trigger_reverse` when `Speed < 0`, `both` either way. Include the start frame in `StartClip`, since events sit at t = 0.0001. This covers `ToggleDoor`, `UpdateDoorSensors` and the lift's `LIFT_DOOR`, all through `PlayFragmentAt` |
| Door effect groups (hangar gates, `SCItemDoorEffectParams`) | route B core effects by `tag` | `PlayFragmentAt` with tag `Open`/`Close` → `OpenStart`/`CloseStart`; while playing → `OpenLoop`/`CloseLoop` (looped); clip end → `OpenEnd`/`CloseEnd` |
| Lift start, travel loop, stop | route B: trigger tags `Elevators/{Start,RampUp,FullSpeed,RampDown,Stop,PowerUp,PowerDown}`, state tag `Elevators/InTransit` | `FSCLift::OnPhase` (`SCLift.h:136`) already broadcasts exactly those names (`Phase("Start")`, `Phase(PhaseName(P))`, `Phase("Stop")`, `PowerUp`/`PowerDown`). Map a phase to the trigger effect with the same tag name. Hold `InTransit` from `Start` to `Stop` (`LIFT_DEPARTED` → `LIFT_ARRIVED`). The emitter is attached to the carriage anchor |
| Lift gate, belly hatch | none (no events) / `Play_Silence` | nothing plays, as in the game |
| Panel press | route B trigger tag `Interactions/Activate` on the pressed entity | the top-level `ASCVehicle::SendInteraction(SourceEntityId, …)` call (`SCVehicleLifts.cpp:40`, `SendDepth == 0`) from the panel press (`SCFloorPanel`, call buttons, DOORS-1 pads) fires the source entity's `Activate` effect at its bone |
| Footsteps | `FoleyFootstepDefinition` surface map, steps detected from foot motion | `SCPlayerAnimator` / `SCPlayerBody`: heel-plant detection on `LeftFoot`/`RightFoot` with the definition's thresholds; surface type from the hit component's material (needs `surface_type` in the package, a later slice); `Footstep_Speed` from the character speed |
| Flashlight | `SCItemFlashlightParams.Light_On_SFX/Off_SFX` of the kit's flashlight item | the F toggle in `ASCFirstPersonCharacter` (`Flashlight->SetVisibility`) |
| Weapon draw / holster | route D `playerSounds.adb` `select`/`deselect` with the weapon tag | the stash/unstash fragment play (WP-P2), at the fragment's procedural-clip time |
| Aim, fire | `SCItemWeaponComponentParams.aimAction`, `fireActions[]` | the aim toggle; `FSCPlayerFire` (`SCPlayerFire.cpp`): single shot → `fireAudioTrigger`; rapid start/stop → `startFire`/`stopFire`; empty → dry fire; report tail by environment later |
| Landing gear, ramps | route C on the exterior CHR's clips | the same `Tick` event hook as doors |
| Ambient emitters | route A | spawned at import per interior from `scene.json` `interiors[]`, beside `room_volumes`: spots at container-local positions, multi-position copies from the links, area volumes from `AreaShape` (the `FSCRoomVolume` prism parser in `SCLightAreas.cpp` already reads the same shape kind); loops start at load; `playRandom` spots run their own timer |

## 5. Package contract (proposal for A1-1)

`<package>/audio.json` and `<package>/audio/<media_id>.ogg` (only the media this package references):

```jsonc
{
  "version": 1,
  "triggers": {          // every trigger the package references, resolved
    "Play_SSMP_CRUS_StarLifter_Elevator_Door_Top_Open": {
      "duration_type": "OneShot", "duration_s": [4.0308, 4.0308], "radius_max": 10,
      "root": 12, "attenuation": "463427172" }
  },
  "nodes": [             // the resolved tree; kinds: sound, random, sequence, blend, switch
    { "kind": "sound", "media": 872222338, "loop": 1, "volume_db": -6.0, "pitch_cents": 0 },
    { "kind": "random", "children": [3, 4, 5], "weights": [50000, 50000, 50000], "avoid_repeat": 3,
      "loop": 0, "transition": { "mode": "crossfade", "ms": 1000 }, "volume_db": -21, "pitch_cents": 54 }
  ],
  "attenuations": { "463427172": { "max_distance": 10, "cone": null,
      "volume": { "scaling": "db", "points": [[0, 0, "constant"], [1, 0, "exp1"], [10, -1.0, "linear"]] }, "lpf": [[...]], "hpf": [[...]], "spread": [[...]] } },
  "media": { "872222338": { "file": "audio/872222338.ogg", "channels": 2, "rate": 48000, "seconds": 4.03 } }
}
```

`scene.json` gains, per interior:

- `audio_spots[]`: entity id, trigger, stop trigger, local position, extra positions, `volume_db`, `pitch`,
  `volume_random_db`, `pitch_random_cents`, `attenuation_scale`, `random_delay_s` or null, `delay_from_end`, area id;
- `audio_areas[]`: entity id, trigger, the shape in the `room_volumes` form, `fade_distance`, `position_fade_distance`,
  `environment_fade_distance`, `volume_db`, `ats_volume_offset_db`, linked spot ids.

Each entity gets `audio_effects[]`:

- `{ "on": "trigger|tag|effect_group", "name": "RampUp", "trigger": "…", "bone": "…", "offset": [x, y, z] }`, with
  the tag path kept as data, never matched by asset name.

Each clip `sources[]` entry gets `audio_events[]`:

- `{ "time": 0.0001, "dir": "forward|reverse|both", "trigger": "…", "bone": "…" }`.

Layer-1 `CACHE_FORMAT_VERSION` bumps.

## 6. Recommended first slice: A1-1 "the C2 sounds like the C2"

**Scope:** the C2 interior ambience (spots and areas), the lift doors and other route-C doors, the lift (start, loop,
stop), the lift buttons. Played in first person, with game attenuation. Containers are honoured (option (b) for
random, sequence, blend and loop; RTPCs at their defaults).

**Changes:**

1. **`scviewer/audio` (cargo lock), each with a failing synthetic test first.**
   - The v154 Attenuation layout.
   - The global media index.
   - The prop-id table.
   - Inheritance.
   - `resolve_playable`.
   - The full animevents parser.
   - The DataCore walker for `GlobalResourceAudio` and `SoundParams.Name`.
   - The decomposed export writes `audio.json`, the decoded Ogg files, `interiors[].audio_spots/audio_areas`,
     entity `audio_effects` and clip-source `audio_events`.
   - The CLI gains `wwise trace --playable <trigger>` (JSON) for verification.
2. **`scviewer-core` / FFI:** the package version; the Ogg → PCM decode (lazy, per media).
3. **UE Runtime module (UE lock):**
   - `USCSoundWave` (procedural, self-looping);
   - `USCAudioPlayer`: plays a trigger's tree, keeps avoid-repeat state, crossfades, applies volume and pitch
     offsets and random ranges;
   - a `USoundAttenuation` per attenuation id: the custom volume curve from the exported curve, LPF and HPF by
     distance, spread;
   - `ASCVehicle` spawns the spot and area emitters;
   - the area logic: inside → the area bed at the listener; outside within `fadeDistance` → faded, positioned at the
     nearest border point; ATS volume offset while inside;
   - the hooks of §4 for doors, the lift and panel presses;
   - console `sc.Audio.List`, `sc.Audio.Play <trigger>`, `sc.Audio.Mute 0|1`, and the log lines `AUDIO_PLAY`
     (trigger, media, emitter, distance, gain) and `AUDIO_EMITTERS` (counts per kind).
4. **Docs:** this brief's Result section; `ue/README.md` §Audio; `docs/01-starbreaker-analysis.md` (the audio crates
   are relevant); `docs/02-implementation-plan.md` (a D18 "audio comes from game data and plays through UE audio,
   not Wwise" and the WP row).

**Done means:**

- Loading the C2 in first person logs `AUDIO_EMITTERS` with 246 spots on `top_deck_c2` (plus their 320 linked
  positions) and 9 areas across the interiors, matching the `.soc` counts above.
- Standing on the top deck plays `Play_SSAM_CRUS_StarLifter_Top_Deck_C2_01` (all three beds and one variation of the
  random container at a time). Walking into the hold crossfades to `Play_SSAM_CRUS_StarLifter_Hold_01` over the
  areas' fade distance.
- Spot sounds fall silent at their attenuation's max distance (`radius_max` × `attenuationScalingFactor`), checked
  with `AUDIO_PLAY` gain at 0.5×, 1× and 1.1× the distance.
- Pressing E on the top lift door plays `…_Elevator_Door_Top_Open` at bone `CRUS_Starlifter_Lift_Doors_Top` at clip
  start, and `…_Top_Close` when it closes. When the lift drives the doors (`LIFT_DOOR`), the same sounds play.
  Reverse playback plays the `audio_trigger_reverse` sound.
- Calling the lift: the button press plays `Play_UI_Levski_Elevator_Button` once per press. `Start`/`RampUp` plays
  `…_Move_Start`, the loop runs between `Start` and `Stop`, and `Stop` plays `…_Move_Stop`. The gate and the belly
  hatch stay silent.
- Every sound that plays is one of the triggers in `c2_trigger_coverage.txt`. A grep of the plugin and exporter
  sources finds no trigger name or media id literal outside tests.
- Every resolvable C2 trigger resolves (263 of 288 in this research's count); the rest are reported with a reason.
- No game audio in git: `git status` is clean of `.ogg`, `.wem` and `.bnk` files after an export.
- **Verification:**
  - a scripted first-person run (RPC harness, WP-H1) that walks the top deck → lift → hold path and logs
    `AUDIO_PLAY`;
  - a short screen and audio capture for the owner;
  - cross-review by Codex (Terra) of the crate and exporter changes, and by Astra of the capture.

## 7. Later slices

- **A1-2, hangar.** `hangar_xltop_001` areas and spots; the loading-platform gates (route B effect groups).
- **A1-3, player kit.** Flashlight on and off; weapon draw and holster (route D), aim, single and rapid fire, dry
  fire.
- **A1-4, exterior.** Landing gear, ramps, nacelles (route C on the exterior CHR).
- **A1-5, footsteps.** `surface_type` from the materials into the package; foot-plant detection with the
  `FoleyFootstepDefinition` thresholds; the footwear from the kit.
- **A1-6, engine idle and flight.** `ShipAudioComponentParams` thruster loops with power state; `VehicleAudioPoint`
  sweeteners; RTPCs that matter (`Footstep_Speed`, thrust).
- **A1-7, environment.** Weapon report tails by room size; reverb from `AudioEnvironment` with UE submix reverb as an
  approximation, if the owner wants it.

## 8. Risks

- **Mix level.** Without CIG's bus volumes, ducking and the `MIX_*` RTPCs, the relative loudness of ambience and
  actions may be off. Mitigations: read the bus volumes from `Init.bnk` (`AudioBus` props) and apply the chain to
  the master; verify by ear with the owner.
- **Prop ids.** v154 prop ids beyond `0`, `74` and `75` are inferred. Confirm them against a second source (wwiser's
  v154 table) before relying on `2`, `3`, `6`, `12`.
- **Attenuation curve units.** The dB-scaled curves store values such as −1.0 at max distance. The mapping to dB
  (silence) needs one controlled comparison.
- **Area semantics.** AAA fade, positioning and `atsVolumeOffset` are CryEngine/CIG engine behaviour, not data. The
  brief's model (inside: listener-centred bed; outside: fade over `fadeDistance`, positioned at the border) is the
  documented CryEngine behaviour and must be checked against the game by ear. The bank's RTPC curve on
  `area_fade_distance` gives the fade shape.
- **Authored oddities kept as authored:** the hold lift door's swapped open and close triggers; the carriage's
  missing attachment bone (fall back to the entity pivot plus the offset; report it); the hatch's `Play_Silence`.
- **Memory and CPU.** 106 MB of Vorbis for the C2 ambience; hundreds of looping spots. Mitigations: honour
  `voiceCount`/`voiceStealingRule` and UE concurrency; virtualise spots beyond their max distance; decode lazily.
- **Codec gaps.** Opus or ADPCM WEMs elsewhere in the game cannot be decoded yet. The exporter must report them, not
  drop them silently.
- **Licensing.** Decoded game audio in a shared cooked showcase is a distribution question (§9).

## 9. Open questions

1. **Showcase.** May the cooked showcase (`package_showcase.ps1`) include decoded audio, or does audio need the
   archive at runtime?
2. **Runtime decode.** Should the decode run at export time (Ogg in the package, decoded to PCM at runtime) or should
   the package carry PCM (FLAC?) to avoid a runtime decoder? The recommendation is Ogg in the package with a Rust
   decoder in the FFI.
3. **Mix and listener.** Reproduce the game's `MIX_*` and listener RTPCs (helmet on, `Listener_Cone`), or keep a flat
   mix?
4. **Reverb.** Is reverb (aux sends) in scope at all, given D15's "path-traced sci-fi look" applies to lighting only?
5. **Propagation.** Should the room-propagation data (`AudioPropagationComponent staticApcRoom`, `RoomConnector`) gate
   or occlude sounds between rooms, or is UE occlusion tracing enough for the first slices?
6. **Decision record.** A D18 entry in the plan for "audio derived from game data, played through UE audio, no Wwise".

## Proof decode (research)

The prebuilt CLI decodes with `starbreaker wwise extract <bank>.bnk -o out/audio-datamine/decoded/<bank> --decode`.
Files are local and gitignored:

| File | Trigger | Check |
|---|---|---|
| `out/audio-datamine/decoded/SSMP_CRUS_StarLifter/872222338.ogg` | `Play_SSMP_CRUS_StarLifter_Elevator_Door_Top_Open` | 2 ch, 48 kHz, 4.03 s, equal to the ATL duration |
| `out/audio-datamine/decoded/ENVA_RoomTone/728282112.ogg` | `Play_ENVA_Hangar_Room_Tone_Tonal_Dark_01` (`hangar_xltop_001` ambience loop) | 2 ch, 48 kHz, 64.99 s |
| `out/audio-datamine/decoded/SSMP_CRUS_StarLifter/52843347.ogg` | `Play_SSMP_CRUS_StarLifter_Elevator_Move_Loop` | 2 ch, 48 kHz, 3.42 s |
| `out/audio-datamine/decoded/UI_MCCS_Lift_Controls/977801969.ogg` | `Play_UI_Levski_Elevator_Button` | 2 ch, 44.1 kHz, 0.11 s |
