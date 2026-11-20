# WP-A2 — Audio propagation and game states: what the game decides in code, derived from its data and its engine

Branch `wp/A2-audio-propagation-states`. Exporter and crate changes go on the submodule branch `scviewer/audio`
(continuing WP-A1).

**Depends on:** WP-A1 (A1-1 player, A1-2 mix: buses, meters, EQ, limiter, rooms and reverb), WP-P1 (first person,
doors), WP-P3 / WP-P4 (game collision for line-of-sight rays), WP-P5 (lifts), WP-P6 (seats: the vehicle game contexts).
**Works with:** WP-K4 (surface types in the package, the same gap as footsteps A1-5).
**Build locks:** cargo lock (crate, exporter, FFI), UE lock (plugin).
**Status:** research and plan (2026-11-20). No code yet. The findings log with every command, file and script is
`out/audio-a2-research/NOTES.md`; numbered references `[N§k]` below point to its findings.

## Goal

WP-A1 plays the C2 and the hangar from the game's data, but leaves open what the game decides in code: how much a sound
is occluded, which ambience area wins when areas nest, which game states and listener parameters the game sets, who
moves the `DM_SCEV_EnvDucker` ducker, how a Wwise LPF/HPF value becomes a cut-off, and how a multi-position spot pans.
The visible symptom is the A1 open item: the hangar's drones dominate the mix inside the C2.

WP-A2 closes each of those with, in order of preference:

1. **the data**: a bank, ATL, DataCore, `.soc` or `Libs` field;
2. **documented engine behaviour**: the Wwise Help and SDK, and the CryEngine 3.8 ATL lineage that Star Citizen's sound
   system shares (AWS Lumberyard sources: the cvar help texts in `StarCitizen.exe` match Lumberyard's `SoundCVars.cpp`
   word for word [N§1]);
3. **a named judgement call**, listed in §11 for the owner, kept in one function each and logged.

No invented values: every number below has its source.

## Read first

- `docs/work-packages/WP-A1-audio.md`, including "Result (A1-1)", "Result (A1-2)" and "Owner decisions (2026-11-20)";
- `out/audio-a2-research/NOTES.md`;
- `ue/Plugins/SCBridge/Source/SCBridge/Private/SCAudioSystem.cpp` (`RtpcValue`, `UpdateAreasAndSpots`,
  `UpdateInstance`), `SCAudio.cpp` (`FSCAudioGain::Add`), `SCAudioBusFx.cpp`;
- `vendor/StarBreaker/crates/starbreaker-wwise/src/{stmg.rs,mix.rs,manifest.rs,datacore_effects.rs}`,
  `crates/starbreaker-3d/src/decomposed_audio.rs`, `socpak/audio_entities.rs`.

## 1. Summary of findings

| Topic | Answer | Source class |
|---|---|---|
| LPF/HPF value → cut-off | Documented table: 0 → 20 kHz linear to 30 → 7 kHz, geometric to 100 → 17 Hz; HPF h = LPF 100 − h. **LPF 58 → 627 Hz.** Values sum through the hierarchy (STMG filter behaviour 0 = Additive) | Wwise Help + data |
| Occlusion / obstruction | Wwise's own obstruction/occlusion curves are **disabled** in the project (Init.bnk ENVS) and every attenuation uses them; the effect is entirely the built-in-bound game parameters `Occlusion_Amount` / `Obstruction_Amount` (0..100) and their authored curves. The amount: the engine's ray test against physics with per-surface `sound_obstruction` (data) | data + lineage |
| Room propagation | Rooms (`physicalEncapsulationValue` 1), connectors (`RoomConnector` entities, doors with `physicalOcclusionValueWhenClosed` 1) and the `propagation_*` / `pressure_*` parameters are named in data and code; only the hangar's big doors carry a `propagation_occlusion` curve in these packages. Pathing (re-positioning through portals) is **actors only** (`GlobalAudioSettings`) | data; formula is judgement |
| Nested areas | Lineage rule: areas interact only when they share a group **> 0**; then the higher priority fades the lower. Every audio area in the C2 and hangar is **Group 0 Priority 0** → no interaction: both play. The hangar bed is reduced inside the ship by the A1-2 side-chain (−10 dB) and, once LPF is applied, its `output_bus_lpf` 58 (627 Hz) | lineage + data |
| Game context | **In data:** `AudioGameContextGlobals` records set `SC_AudioGameContext_Master:<context>` and the context RTPCs. On foot = `ActorDefault` (state `Player_Default`, RTPC `Player_Default` 1). Which context applies is code, but the record names map one-to-one to situations | data |
| Interior / exterior, location | `Interiority` is a planet/ecosystem listener parameter (`WorldBuilderSound.cpp`); in these packages only QDrive sounds read it. `Listener_Atmospheric_Pressure` 1 and the `pressure_*` defaults (1) are right for a pressurised ship and hangar; `pressure_gameContext` defaults to 0. `SC_Location_Station:Interior` would mute the ship bus (−96 dB): not set in a hangar. `Ship_Spool_State` default 0 plays the C2 "powered down" — **already applied today** | data + judgement |
| `DM_SCEV_EnvDucker` | **No writer** in LIVE 4.10.2: no meter, no `SetGameParameter` action in any of 2,410 banks, no DataCore / Libs / executable string. It stays at −48 → 0 dB, as A1-2 has it | data (negative search) |
| Multi-position panning | Spots use `multiPositionType 0` → `s_DefaultMultiPositionType` 2 = MultiDirections: **one voice, per speaker the maximum over the positions** | exe + Wwise SDK |
| States and bus effects | Ten state groups on the C2/hangar chains (table §5.4); the default (no state) is right for all but the game context. Transition times and game-parameter ramps are in STMG and not yet honoured | data |
| HDR | No HDR bus in SC (`bIsHdrBus` never set). Dynamic range is `MIX_DR` (presets 0 / 0.5 / 1; game default profile preset 2) plus the meter side-chains | data |

## 2. LPF / HPF: value, accumulation, cut-off

### Findings

- **The table** (Wwise Help 2024.1.18, "Associating low-pass and high-pass filter values with their corresponding cutoff
  frequencies"; `out/audio-a2-research/wwise_lpf_hpf_cutoff.tsv`). LPF value v → cut-off:
  `v ≤ 30: 20,000 − v · 433.33 Hz`; `v > 30: 7,000 · (17 / 7,000)^((v − 30) / 70) Hz` (max 2.9 % off the table, which
  Audiokinetic calls approximate). HPF value h → the cut-off of LPF value 100 − h. Values that matter here:

  | Where | Value | Cut-off |
  |---|---|---|
  | `DM_Ship_Ambience` ≥ −30 dB → bus 2496768650 `output_bus_lpf` (hangar ENVA/ENVP ambience) | 58 | 627 Hz |
  | `Ship_Spool_State` 0 → C2 SSAM trigger context LPF / HPF | 44 / 4 | 2,095 Hz / 24 Hz |
  | `Occlusion_Amount` 100 → C2 SSAM / hangar ENVA-ENVP | 40 / 30 | 2,957 Hz / 7,000 Hz |
  | `propagation_occlusion` ⅓, ⅔, 1 → hangar big door | 28 / 33 / 38 | 7,867 / 5,405 / 3,513 Hz |
  | `Listener_Atmospheric_Pressure` 0 (vacuum) → C2 / hangar | 60 / 75 | 528 / 145 Hz |
  | `Ship_Translation_CruiseDampener` 0 → C2 SSAM | 10 | 15,667 Hz |
  | bus 3775940196 `SC_FPS_Player_Life:Down` state | 30 | 7,000 Hz |

- **Accumulation.** STMG `uFilterBehavior` = 0 = Additive (wwiser `AkFilterBehavior`; Wwise Help "Understanding
  property filter behavior": Sum All Values, project-wide, covering LPF, HPF, output-bus LPF/HPF, aux LPF/HPF). Node
  props, RTPC curves (accumulation 6 "Filter"), state props and the attenuation's distance LPF/HPF curves add, clamped to
  0..100, then map to one cut-off. Environmental (obstruction/occlusion) curves would accumulate by highest value, but
  they are disabled (§3).
- **Today.** The player reads node `lpf` / `hpf` (`SCAudio.cpp:189`) and applies no filter: no voice LPF/HPF, no
  attenuation distance filter, no bus `output_bus_lpf` (A1-2 open item).

### Applied from data / documentation

The value chain, the sum, the clamp and the table. Bus-level filters (`output_bus_lpf` / `output_bus_hpf`, bus `lpf` /
`hpf` RTPCs and state props) run on the bus's submix; voice-level ones per voice.

### Judgement

- **Filter shape.** The Help calls the LPF a "recursive filter" and gives no slope. Proposal: a one-pole (6 dB/oct)
  low-pass and high-pass per voice and per bus, the cheapest recursive filter and the same family as UE's source LPF
  (§11 D6).
- **Interpolation of a changing cut-off** per audio buffer (no zipper): the existing `SCAudioBusFx` gain ramp pattern.

## 3. Occlusion and obstruction

### Findings

- **Wwise side is inert.** Init.bnk `ENVS` holds the project curves for obstruction, occlusion, diffraction and
  transmission × volume, LPF, HPF: all `enabled = 0` [N§11]. All 10,366 attenuations in the 2,410 banks set those
  twelve slots to −2, "use project" (Wwise Help, "Defining the attenuation curves…": *Use Project Obstruction / Occlusion*).
  So `SetObjectObstructionAndOcclusion` (called by the game: `CAudioSystemImpl_wwise`, 0x7f49908) changes no curve by
  itself.
- **The game parameters carry it.** STMG binds `Occlusion_Amount` to the built-in Occlusion (filtering over 0.3 s),
  `Occlusion_Amount_Slow` to Occlusion (slew 25 / s) and `Obstruction_Amount` to Obstruction [N§10] (Wwise Help,
  "Property Editor: Game Parameter": built-ins are "as set by the game on the game object", 0–100). Authored curves at
  100: C2 SSAM (124 triggers) −12 dB, LPF 40; hangar ENVA/ENVP (66 triggers) −6 dB, LPF 30. The hangar's dominant bed
  `Play_ENVA_Hangar_Room_Tone_Tonal_Dark_01` has **no** occlusion curve; no trigger in the two packages has an
  `Obstruction_Amount` curve [N§9].
- **How the engine computes the amount** (CryEngine 3.8 lineage, Lumberyard `ATLAudioObject.cpp`, `ATLUtils.h`) [N§16]:
  - per object, a calc type Ignore / SingleRay (1 ray) / MultiRay (5 rays); SC keeps the ATL switch
    `ObstrOcclCalcType` (Ignore / SingleRay / MultiRay);
  - ray 0 from the listener to the source; rays 1–4 from the listener offset ±up / ±side by 0.3 m (≤ 1 m away) to 1.0 m
    (≥ 20 m away), with a random jitter of 0.05–0.5 m;
  - each ray: Σ `sound_obstruction` of the surfaces it hits (≤ 5 hits), clamped to 1, smoothed with α 0.05 and a
    hit-count correction;
  - occlusion = mean of rays 1–4; obstruction = ray 0 × min(`s_FullObstructionMaxDistance` / d, 1) − occlusion, ≥ 0;
  - both smoothed (`v += (t − v) / 1.04` per update); nothing under 0.3 m or beyond `s_OcclusionMaxDistance`.
  - SC's values: `s_OcclusionMaxDistance` **500 m**, `s_OcclusionMaxSyncDistance` 10 m, `s_FullObstructionMaxDistance`
    **5 m** (executable help texts) [N§1].
- **Per-surface data.** `Libs/MaterialEffects/SurfaceTypes.xml` `sound_obstruction`: metals 0.40 (`metal_shell` 0.25,
  `metal_hollow` 0.35, `metal_grate` / `metal_ladder` 0.05), `mat_default` 0.25, glass 0.10–0.20, `mat_nodraw` 0 [N§17].
  A ship wall (metal 0.40) and a hull (0.40) on one ray give 0.8: Occlusion_Amount 80 → on a C2 SSAM trigger LPF 32
  (5,892 Hz) and a volume between 0 and −12 dB along the curve's first segment (interpolation 2, `Log1`). The viewer's
  collision does not carry surface types yet (A1 §1.5, WP-K4).
- **Which objects run rays.** The lineage's `AudioTriggerSpot.lua` and `AudioAreaAmbience.lua` default to Ignore; SC's
  `AudioTriggerSpotParams` has no calc-type field [N§16, N§7]; the authored `Occlusion_Amount` curves on the SSAM/ENVP
  spot triggers show the designers expect a value on spots.

### Applied from data / documentation

The amount → `Occlusion_Amount` / `Obstruction_Amount` (× 100) → the authored curves; their game-parameter ramps; the
cvar distances; the ray layout and smoothing; `sound_obstruction` per surface.

### Judgement

- **Calc type per object** (§11 D4): proposal MultiRay for spots and entity effects, Ignore for area beds (they sit at
  the listener when inside, so the lineage skips them anyway: distance < 0.3 m).
- **Surface type until WP-K4 lands** (§11 D4): proposal `mat_default` 0.25 per hit; with K4, the hit's surface.

## 4. Room propagation

### Findings

- **Data.** Rooms are `Room_RN` entities with a room volume (exported by A1 for reverb); their class
  `AudioPropagationParams.physicalEncapsulationValue` is 1 (0 only on 31 open vehicles). Connectors are `RoomConnector`
  entities (`SEntityComponentRoomConnectorParams.defaultAreaBounds`, `boundsOffset`) and doors of
  `Door_RN_RoomConnector_*` classes, whose `physicalOcclusionValueWhenClosed` is 1 [N§7, N§19]. The C2 layers hold 3
  `RoomConnector` entities (top deck) and 5 door instances with connector params (armoury, bridge, escape pods,
  systems, top deck); the hangar socpak one connector. The package
  carries rooms, not connectors.
- **Code names** (executable, beside `EntityComponentRoom` / `…RoomConnector` / `…AtmosphereContainer`):
  `propagation_occlusion`, `propagation_listenerInSameRoom`, `propagation_pressurePhysicalAudible`, `pressure_physical`,
  `pressure_soundSim`, `pressure_gameContext`, `pressure_playerActorContained` … [N§3]. STMG defaults: listener in same
  room 1, pressures 1, `propagation_occlusion` 0, `pressure_gameContext` 0 [N§10].
- **Where it matters here.** Only `propagation_occlusion` has a curve that bites: LPF 28 / 33 / 38 at ⅓ / ⅔ / 1 on the
  hangar's `Play_MCAN_Hangar_Doors_Hightech_XL_Top_{Open,Close}`. `propagation_pressurePhysicalAudible` (41 C2
  door/locker triggers) acts only below its default 1.
- **Pathing** (sound re-positioned to the portal): `GlobalAudioSettings.enablePropagationPathingActorOnly` true and
  `s_EnablePropagationPathing` "2 is actors only": not for spots, areas or doors [N§6].

### Applied from data

The room graph (rooms, connectors, door open/closed state from the door's clip), `physicalOcclusionValueWhenClosed`,
`propagation_listenerInSameRoom` (1 / 0), pathing off for non-actors.

### Judgement (§11 D5)

The `propagation_occlusion` formula. Proposal: 0 in the listener's room; otherwise the sum, along the fewest-connector
path from the emitter's room to the listener's, of each closed connector's `physicalOcclusionValueWhenClosed` and 0 for
open ones, divided by 3 and clamped to 1 — the curve's breakpoints at thirds suggest steps of one closed connector
(reading of the curve, not a source).

## 5. Game states and game parameters

### 5.1 The game context (data)

`AudioGameContextSetup` → eight `AudioGameContextGlobals` records [N§5]:

| Record | State `SC_AudioGameContext_Master` | RTPC at 1 | Viewer situation (judgement, §11 D1) |
|---|---|---|---|
| `DefaultGlobalState` | `Player_Default` | none | before the player spawns |
| `ActorDefault` | `Player_Default` | `Player_Default` | first person on foot (hangar, ship, lift) |
| `VehicleDriver` | `Player_VehicleDriver` | `Player_VehicleDriver` | seated as pilot (WP-P6) |
| `VehicleCrew` | `Player_VehicleCrew` | `Player_VehicleCrew` | seated as crew / co-pilot (WP-P6) |
| `ActorEVA`, `RemoteTurret`, `ActorDead`, `Cutscene` | as named | as named | not reachable in the viewer |

Effect today: bus 2527695131 bypasses EQ slot 1 (`pressure_playerActorContained`, 3 kHz) in `Player_Default` / EVA /
Dead / VehicleCrew / InCutscene and slot 0 (`Listener_Atmospheric_Pressure`) in VehicleDriver / RemoteTurret (A1-2 §3;
props 0x130 / 0x30 [N§13]). The state's transition time is 2,000 ms (STMG); `Player_Default` filters over 2 s.

### 5.2 Listener and location parameters

| Parameter | Default (ATL / STMG) | Curves in the packages | Proposal | Class |
|---|---|---|---|---|
| `Listener_Atmospheric_Pressure` | 1, filter 4 s | 124 C2 + 85 hangar triggers, one bus; none at 1 | 1 in a pressurised room; the default | data |
| `pressure_playerActorContained` (+ `…ConnectedByPressure`) | 1 | Init buses, EQ 2410147139 | 1 (default) | data |
| `propagation_pressurePhysicalAudible` | 1 | 41 C2 triggers; none at 1 | 1 (default) | data |
| `pressure_gameContext` | **0**, filter 7 s | player fire extinguisher: LPF 50, ±4.9 dB at 0 | 1 in pressurised rooms (§11 D9) | judgement |
| `Interiority` | 0, filter 3 s | 12 QDrive blends only (−18 / −9 dB at 1) | listener room's `physicalEncapsulationValue` (1 inside a room, 0 outside) | judgement |
| `SC_Location_Station` | none, transition 1,000 ms | bus 1943164433 (ship sounds) −96 dB in `…_Interior` | none in a hangar: `Interior` would silence the ship the player stands in | judgement |
| `SC_Local_Perspective` | 0 | none here | default | data |
| `MIX_DR` | 0, filter 1 s | Gain slots on 2633160988 (−2.7 / −5.2 dB at 0.5 / 1) and others; a slot bypass at 0 | §11 D2 | judgement |

`Interiority` is set by `CWorldBuilder::UpdateAudio` with the planet `ecosystem_*` parameters [N§3]: no effect on the
current walk; listed so the mapping is decided once.

### 5.3 The ship's own state

All C2 spots and areas subscribe to `Ship_Ambience_Parameters` (`Ship_Spool_State`, `Ship_Health`,
`Ship_Translation_Speed_NonNormalised`, `Ship_Translation_CruiseDampener`) [N§8]. At the defaults the C2 ambience plays
spooled down: trigger context pitch −200 cents, −3.5 dB, LPF 44 (2,095 Hz), HPF 4; several blends −50/−100 cents and
up to silent [N§9]. The A1 player already applies the pitch and volume parts. Whether the viewer's C2 is powered is the
owner's (§11 D3).

### 5.4 State groups on the C2 / hangar chains and their bus effects

Read from Init.bnk state chunks (prop ids from the chunk's accumulation types and in-dB flags: 0 volume, 1 pitch, 2 LPF,
3 HPF, 4 bus volume) [N§13]:

| Bus (role) | Group: state | Effect | Set in the viewer |
|---|---|---|---|
| 2527695131 (doors sibling) | `SC_AudioGameContext_Master`: per context | EQ slot bypass | yes, §5.1 |
| 1943164433 (ship SSAM, doors) | `SC_Location_Station`: `…_Interior` | voice −96 dB | no (none) |
| 3775940196 | `SC_FPS_Player_Life`: `SC_FPS_Player_Down` | −12 dB, LPF 30 | no |
| 886927447 | `SC_Loading_Screen`: `*_ON` | bus −200 dB | no |
| 2633160988, 2660374599 | `S42_VS_Trackview`: `_On` | −3 dB | no |
| 1729746120, 3860291366 | `SC_Mix_DeadZone`: `…_SpaceshipCombat` | LPF 40, −10 dB | no |
| 2660374599 | `SC_GameMode`: `SC_Frontend` | LPF 15, −19 dB | no |
| 3644258345 | `Editor_GameMode`: `Exit` | −96 dB | no |
| reverb aux 418909758 / 1276241260 / 2828019634 | `SOO_Mission_Global`: `EnterMissionArea` | −12 / −6 / −3 dB | no |

No C2 SSAM/SSMP node carries a state group; the hangar banks' nodes only Star Marine / frontend / tutorial / emergency
states. The viewer runs one state machine per group with the STMG default transition time, so a later situation (a
loading screen, the player down) is data, not code.

### 5.5 `DM_SCEV_EnvDucker`

Bus 2633160988 (hangar ambience parent) volume: 0 dB up to −18, −3 dB from −10 [N§4]. The id occurs in Init.bnk only;
the scripted-event banks' `SetGameParameter` actions write `SC_SCEV_PowBlast_*`; nothing else names it. **Applied:**
it stays at its default −48 (0 dB), as today. Re-check on each game update (§8).

### 5.6 Game-parameter interpolation

STMG gives each parameter a ramp: none, slew rate (units per second), or filtering over time (Wwise Help: "target 99.5 %
of the target value within the specific Attack/Release time") [N§10]. Not applied today; A1-2's `DM_Ship_Ambience`
side-chain moves instantly where the game filters over 3.5 s. Applied in A2-2: one-pole filter with
τ = time / ln(200) per direction; slew as a rate limit.

## 6. Nested ambience areas

### Findings

- **Lineage rule** (Lumberyard `AreaManager.cpp`, `ProcessArea`, `ProceedExclusiveUpdateByHigherArea`, `NotifyAreas`;
  `AudioAreaAmbience.lua`) [N§18]: areas influence each other only when they share a group and the group is > 0. Then,
  with the listener inside both, the lower-priority area fades as `(fadeDistance_high − d) / fadeDistance_high`, d the
  distance to the higher area's hull (the outer bed fades out as the listener goes deeper into the inner area); near
  cases follow the nearer hull. Group ≤ 0: each area's inside / near / far fade on its own.
- **Data.** Every audio `AreaShape` in the C2 (8) and the hangar (`AS_hangar_xltop_001_sfx`, 220 m tall; `Shape001`,
  `Shape002`) is `Group="0" Priority="0"`. The areas' `audioSwitchStateInside/Near/Far` are empty [N§20].
- **So** the documented behaviour is A1's: inside the C2 the hangar bed and the C2 bed both play. What reduces the hangar
  in the game is data A1-2 already runs (`DM_Ship_Ambience` → −10 dB) plus the `output_bus_lpf` 58 (627 Hz) that A2-1
  adds. The hangar bed has no occlusion curve.
- SC's area manager is CIG's (`CEAAreaManager`, `CEntityAudioProxy::GetAreaFadeInfo`, zone enter/leave events); its rule
  is not visible in strings.

### Applied from documentation

Export `Group` / `Priority`; implement the lineage rule (all four cases) for group > 0; leave group ≤ 0 independent.

### Judgement (§11 D7)

Whether to add a viewer rule for "the listener is inside a vehicle's area that sits inside a location's area" when the
groups are 0. Proposal: no, until A2-1's measurement shows the hangar still dominating.

## 7. Multi-position spots

### Findings

- 253 C2 spots have extra positions; every spot's `multiPositionType` is 0 [N§7] → `s_DefaultMultiPositionType` "Default:
  2" = `eAMPT_MULTIDIRECTIONS` [N§1].
- Wwise SDK ("Integration Details - 3D Positions"): MultiDirections — "the volume played in each speaker is the maximum
  in each direction"; one voice for all positions; virtual when every position is out of range; no additive build-up.
  MultiSources adds the volumes.
- Panning rule: the game's default profile sets `s_PanningRule` 1 [N§12]; `AkPanningRule_Headphones` = 1, "Left and
  right positioned 180 degrees apart" (constant power); `Speakers` = 0, 60° apart.
- Today: one voice at the nearest position (`SCAudioSystem.cpp:1532`).

### Applied from documentation

One voice; for each position its attenuation (volume and spread curves) and its pan; per output channel the maximum over
the positions. Implemented as a non-spatialised UE voice with per-channel gains computed by the player (stereo first).

### Judgement (§11 D8)

The panner law for the viewer's output: Wwise headphones (constant-power over ±90°) as the game's default profile, or
UE's own panner.

## 8. Viewer design

```
first-person context ──► FSCAudioContext (on foot / seated pilot / seated crew; room; pressurised)
                              │ sets states + global game parameters from AudioGameContextGlobals and §5.2
                              ▼
 STMG ramps ─► FSCGameParams (global + per-object values, slew / filtering, state machines with transitions)
                              │
 rays (UE line traces on game collision, surface → sound_obstruction)       room graph (rooms, connectors, door state)
   └─► FSCOcclusion per emitter (lineage algorithm) ─► Occlusion_Amount / Obstruction_Amount    propagation_* ◄─┘
                              ▼
 FSCAudioGain: volume, pitch (as today) + LPF/HPF value sum ─► cut-off table ─► per-voice 1-pole LPF/HPF
 bus chain: output_bus_lpf / hpf, bus LPF/HPF states ─► per-bus filter in SCAudioBusFx
 multi-position: per-position gain/pan ─► per-channel max ─► one non-spatialised voice
 areas: inside/near/far fade (A1) + group>0 exclusive fade
```

- **Context driver** (`USCAudioSubsystem`, fed by `ASCFirstPersonCharacter`): on foot → `ActorDefault`; seated (WP-P6) →
  `VehicleDriver` / `VehicleCrew` by the seat's role; no player → `DefaultGlobalState`. The listener's room from the
  rooms the package exports (`LightRoomAt` today, the audio room volumes); in the lift carriage the listener stays on
  foot in the carriage's room. Only these situation → record mappings are code; the states and values come from the
  records.
- **Game parameters** become a single store: global values (context, pressure, Interiority, MIX_DR, ship state per
  vehicle) and per-emitter values (Occlusion / Obstruction, propagation), each with its STMG ramp. `RtpcValue` reads it
  instead of the manifest default.
- **Occlusion**: per emitter, updated at the lineage cadence (one query per emitter per audio update; async traces;
  results smoothed); traces skip the listener's own body and the emitter's own entity.
- **Logs:** `AUDIO_STATE group=… state=… t=…`, `AUDIO_GP name=… value=… target=…` (on change ≥ 1 % of range),
  `AUDIO_OCCL emitter=… rays=… occlusion=… obstruction=… amount=…`, `AUDIO_FILTER voice=… lpf=… hz=… hpf=… hz=…`,
  `AUDIO_AREA` gains `group` / `priority` / `exclusive_by`. Console: `sc.Audio.State <group> <state>`,
  `sc.Audio.Gp <name> <value>`, `sc.Audio.Filters 0|1`, `sc.Audio.Occlusion 0|1|draw`.

## 9. Slices and "done means"

Every slice: failing test first (Rust synthetic fixtures; UE automation), no game literal in plugin or exporter code,
verification commands and output in the PR, cross-review Terra (code) and Astra (recordings).

### A2-0 — Export what the code needs (cargo lock)

- STMG game parameters with ramp type, up/down and built-in binding; state groups' default transition times; ENVS curves
  (enabled flags) into `audio.json`.
- `AudioGameContextGlobals` / `AudioGameContextSetup` → `game_contexts`.
- Area `Group` / `Priority` from `AreaComponent`.
- Rooms' `physicalEncapsulationValue`; connectors (`RoomConnector` entities and door entities with
  `SEntityComponentRoomConnectorParams`: transform, bounds, offset, door entity id, `physicalOcclusionValueWhenClosed`).
- `SurfaceTypes.xml` `sound_obstruction` table; `GlobalAudioSettings` pathing flags.
- Check and fix the node-vs-context sign of RTPC curves (`pressure_gameContext` blend 766019275 −50 vs context +50,
  [N§9]) with a synthetic test.

**Done means:** a C2 + hangar re-export logs `game_parameters` with `Occlusion_Amount builtin=Occlusion ramp=filter
0.3/0.3`, `DM_Ship_Ambience ramp=filter 3.5/3.5`; 8 `game_contexts`; areas with `group=0 priority=0` (11 audio areas);
connector count equal to a `.soc` census of the loaded layers (logged side by side); the ENVS block reports 12 disabled
curves; cache format bumps; Rust tests for each reader.

### A2-1 — Filters (UE lock)

- Value sum per voice (node + RTPC + state + attenuation distance curves) and per bus (`output_bus_lpf/hpf`, bus props,
  states), clamp 0..100, table → cut-off, one-pole LPF/HPF (D6), smoothed per buffer.

**Done means:**
- unit tests: value → Hz at 0, 30, 45, 58, 100 equals the table within 3 %; HPF 20 → 94 Hz; sum of 15 + 30 = 45 →
  1,922 Hz;
- `AUDIO_FILTER` on the hangar bed inside the C2 with `DM_Ship_Ambience` ≥ −30: `lpf=58 hz=627`;
- recording A/B (`sc.Audio.Filters 0/1`, top deck, fixed listener, 20 s each): the hangar ambience bus spectrum with
  filters on is −3 dB ±1 dB at 627 Hz ±10 % relative to off, and −6 dB/oct ±2 above;
- the C2 lift door at 9 m (attenuation LPF/HPF curves of attenuation 463427172) logs the curve's value and Hz;
- walk RMS per segment reported against run10; the owner listens.

### A2-2 — Game syncs (UE lock)

- `FSCGameParams` with STMG ramps; state machines with transition times; context driver (D1); listener parameters per
  §5.2 (D2, D3, D9 decided); `RtpcValue` reads the store.

**Done means:**
- tests: a step on a filtering parameter reaches 99.5 % within its time (±1 audio block); slew reaches at its rate; a
  state change interpolates its props over the transition time;
- walk log: `AUDIO_STATE SC_AudioGameContext_Master=Player_Default`, `AUDIO_GP Player_Default=1`; bus 2527695131 reports
  slot 1 bypassed, slot 0 active (`AUDIO_BUSFX`);
- mix trace: bus 2496768650's side-chain gain follows `DM_Ship_Ambience` with the 3.5 s filter (step response plotted);
- `Ship_Spool_State` set per D3: the C2 bed's pitch in `AUDIO_PLAY` is the curve value (0 or −200 cents) and a 20 s
  recording on the top deck per setting goes to the owner.

### A2-3 — Area precedence (UE lock)

- Lineage rule for group > 0 (four cases), group ≤ 0 unchanged; `AUDIO_AREA` with group, priority, exclusive_by.

**Done means:** automation tests with synthetic nested group-1 areas cover the four cases (fades within 0.01 of the
formula); the C2/hangar walk's `AUDIO_AREA` fades equal run10's (all group 0); D7 recorded.

### A2-4 — Ray occlusion (UE lock; surface types with WP-K4, fallback D4)

- `FSCOcclusion` per emitter (lineage algorithm, SC cvar distances), `Occlusion_Amount` / `Obstruction_Amount` per
  emitter with their ramps; debug draw.

**Done means:**
- tests: a synthetic wall of `sound_obstruction` 0.4 between listener and emitter gives occlusion 0.4 (all rays hit) and
  obstruction 0 (ray 0 equals the mean); 0 beyond 500 m or under 0.3 m; obstruction scales by 5 / d beyond 5 m;
- in the hangar, a hangar ENVP spot heard from the C2 top deck logs `AUDIO_OCCL amount` within ±5 of 100 × the summed
  per-hit values of its rays (hits logged) and `AUDIO_PLAY` gain/LPF per the −6 dB / LPF 30 curve at that amount;
- the same spot with the listener outside the ship in line of sight: amount 0;
- recording: walk from the hangar floor into the C2 and back; Astra review.

### A2-5 — Room propagation (UE lock)

- Room graph from rooms + connectors; door open state from the door's playback; `propagation_listenerInSameRoom`,
  `propagation_occlusion` (D5); no pathing for non-actors.

**Done means:** log of the graph (rooms, connectors, adjacency) for the C2 and hangar; closing / opening the hangar's
big door with the listener in the C2 changes `propagation_occlusion` on its trigger per D5 and its LPF to the curve
value (28 / 33 / 38 → Hz in `AUDIO_FILTER`); same-room emitters read 1 / 0.

### A2-6 — Multi-position MultiDirections (UE lock)

- One voice per spot; per-position attenuation and pan; per-channel maximum; virtual when all positions are out of reach.

**Done means:** test: two positions equidistant left and right give each channel the single-position level (no +3 / +6
dB); one position in range and one out gives the single-position result; on the top deck the 78 multi-position spots
play 78 voices (`AUDIO_EMITTERS`); recording on a light-hum spot walking along its positions.

## 10. Risks

- **Lineage ≠ SC.** CIG rewrote the area manager (`CEAAreaManager`) and added zones and room propagation; the Lumberyard
  rules are the documented ancestor, not SC's code. Mitigation: every lineage rule sits in one function, is logged, and
  is listed as such in `ue/README.md` §Audio.
- **Surface types missing** in the viewer's collision: occlusion with a single fallback value over- or under-states thin
  walls (`metal_grate` 0.05 vs metal 0.40). Mitigation: D4 fallback; WP-K4.
- **Filter shape**: a one-pole LPF at 627 Hz differs from a steeper filter by several dB at 2–4 kHz. Mitigation: D6,
  owner by ear; switchable.
- **CPU**: 5 rays per emitter per update over hundreds of spots. Mitigation: only voiced emitters within 500 m, async
  traces, the lineage's update cadence; budget in the PR.
- **Multi-position** needs a custom pan path for stereo sources (UE's spatialiser cannot take a per-channel max).
- **States set by unknown writers** (`SC_Location_Station`, `SC_Player_In_Ship`): assumed unset; a later location may
  need them.
- **`DM_SCEV_EnvDucker`** may gain a writer in a later game version; the negative search must be re-run per update
  (script in the notes).
- **Prop ids of state chunks** are read from accumulation types and in-dB flags, not from a table; A2-0 confirms them
  with a synthetic test and the A1-2 values.

## 11. Decisions needed from the owner

| # | Decision | Options (first = proposal) |
|---|---|---|
| D1 | Situation → game context | on foot `ActorDefault`; pilot `VehicleDriver`; other seats `VehicleCrew`; nothing else reachable |
| D2 | Dynamic range (`MIX_DR`) | (a) the game's default profile: preset 2 → `MIX_DR` 1 (curve breakpoints 0 / 0.5 / 1; label order Full / Medium / Minimum assumed); (b) ATL default 0 (today); (c) a viewer setting |
| D3 | Ship power (`Ship_Spool_State`) | (a) a per-ship toggle, default powered (1); (b) the data default 0 (powered down, today: −200 cents, −3.5 dB, LPF 44 once filters apply) |
| D4 | Occlusion objects and surfaces | MultiRay on spots and entity effects, Ignore on area beds; surface fallback `mat_default` 0.25 until WP-K4 |
| D5 | `propagation_occlusion` formula | closed connectors' `physicalOcclusionValueWhenClosed` along the fewest-connector path / 3, clamped to 1 |
| D6 | LPF/HPF shape | one-pole 6 dB/oct; alternative second-order |
| D7 | Nested areas with group 0 | lineage (independent) and measure; alternative a viewer containment rule |
| D8 | Panner for multi-position | Wwise headphones law (profile default `s_PanningRule` 1); alternative UE's panner |
| D9 | `pressure_gameContext` | 1 in pressurised rooms; alternative the default 0 |
| D10 | `Interiority` | listener room's `physicalEncapsulationValue`; alternative leave 0 (affects only QDrive today) |
| D11 | Spot `lowPassFilter` / `highPassFilter` = 500 (class default on all 834 spots) | treat as inactive (any reading as a value or Hz would band-pass every spot); ask again if a spot differs |

## 12. Open questions

1. Who sets `SC_Location_Station` and `SC_Player_In_Ship` (146 per-ship-room states, none for the C2)? Not in the
   executable strings, DataCore or the hangar socpak.
2. CIG's actual `propagation_occlusion` / `pressure_*` formulas (`CAudioPropagationComponent::Update`): only names are
   visible.
3. The exact `s_DynamicRangePreset` → `MIX_DR` mapping and the preset label order.
4. Whether CIG's zone system culls the hangar's area query for a listener inside a vehicle zone
   (`s_UseInstancedZoneCullingV2`, `CAudioTriggerSpot::Play(…, SZoneSystemEnterLeaveEvent)`).
5. Whether SC still runs the lineage's five-ray layout and smoothing constants (the cvars match; the code is not
   visible).
6. Meaning of the `AudioAreaAmbience` `positionFadeDistance` / `environmentFadeDistance` beyond the lineage's
   `fRtpcDistance` / environment fade (A1 uses `fadeDistance`).

## 13. Export and runtime checkpoint

- The C2 and XL hangar packages contain eight DataCore game contexts, STMG ramps and state transition definitions,
  12 disabled ENVS curves, and 72 surface obstruction entries. The C2 has eight audio areas and the hangar has three.
  All export group and priority 0. The C2 has twelve room connectors with bounds, including three standalone connectors
  on the lift. The loaded C2 layers supplied no closed-occlusion override on those connectors. Absent fields stay
  null in the export.
- The viewer evaluates signed source and attenuation LPF/HPF contributions, clamps their sum at the DSP boundary,
  and processes one-pole filters on the voice and output submix. It reads the authored parameters for the actor
  and free viewer contexts and switches effect-slot bypass state when the context changes. The area priority rule
  applies only to positive groups.
- State chunks supply typed volume, pitch, LPF, HPF, and bus-level offsets. The viewer interpolates numeric values
  over the authored STMG duration for the specific state pair, or over the group default. An interrupted transition
  continues from the current mix. Effect-slot bypass remains a discrete state.
- On stereo outputs, a spatial multi-position spot decodes each voice once and applies the maximum
  attenuation-weighted gain for each speaker. The equal-power panner keeps centered stereo material in its
  original channels and folds it down for hard pans. The wet send keeps the existing nearest-position level.
  Other output layouts continue using the viewer's existing spatialization.
- When given an explicit distance profile, the viewer evaluates occlusion for spatial spots and attached emitters
  with STMG ray bindings. It queues five asynchronous traces, uses the archive's obstruction table and
  `mat_default` for unclassified collision, and advances each bound parameter with its own authored ramp.
  `sc.Audio.OcclusionMinM`, `sc.Audio.OcclusionMaxM`, and `sc.Audio.ObstructionFullM` supply the measured
  profile for a test. Unconfigured sessions use the parameters' authored defaults. `sc.Audio.OcclusionDebug 1`
  enables trace and reduction logging. The layout follows the lineage geometry with deterministic offsets.
- Room membership uses the vehicle instance, interior placement and room GUID. A point inside two distinct rooms is
  unresolved. Placed audio keeps its authored room. A listener and emitter in the same known room read 1 / 0 for
  `propagation_listenerInSameRoom` / `propagation_occlusion`. Known rooms in different vehicles read 0 for same-room.
- The graph checks the opposite face centers of each transformed connector box against the placed room volumes.
  A unique pair identifies an edge. The graph refreshes when an interior moves relative to the vehicle. D5 uses
  minimum-hop paths only when their costs agree. Missing closed weights, missing or ambiguous door rigs, moving
  doors and unresolved paths defer to the bank default. Cooked packages retain connector data in the audio sidecar
  but lack a serialized per-placement rig binding. The live C2 and XL inputs have 12 and 11 bounded connectors
  respectively, all missing a closed weight. These exports cannot demonstrate the D5 closed-door LPF effect.
  `AUDIO_ROOM_CONNECTOR` records the scoped endpoints, door and authored weight of each connector.
- The P6 integration drives VehicleDriver and VehicleCrew. The top-deck filter captures cover the stereo master.
  The isolated hangar-bus and moving listener acceptance still needs the checks in §9.
