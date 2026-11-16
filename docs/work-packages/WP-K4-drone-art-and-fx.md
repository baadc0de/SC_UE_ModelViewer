# WP-K4 — Drone art, its dock, and the game's effects for beams and impacts

## Why

The cargo drones are stand-in art (WP-K1d): a round body whose heading does not matter. The owner bought a drone model
on Fab ("SciFi Drone 4") with a face, moving parts and a locomotion animation set. With it, a drone must face somewhere
on purpose, animate with its motion, and fit a dock built for it. The beam and box collisions also show no effects;
the game has its own.

## Parts

| Part | What | Who |
| --- | --- | --- |
| K4a | The Fab drone as the drone's look, when installed (stand-in otherwise): native size, re-centred, yaw offset; heading policy; animations from motion; beam from its camera | Opus 5.5 |
| K4b | The drone dock remade to house it: two lattice cells, two bays | Astra (Codex), reviewed by Opus 5.5 |
| K4c | The game's particle effects for the tractor beam and box impacts: where they are in the archive, their format, what it takes to show them (research first, then a plan) | Opus 5.5 (research agent) |

## K4a — the Fab drone

**Asset.** `ue/SCViewer/Content/SciFi_Drone_4/` (UE 5.7 pack, added through the Epic launcher's "Add to Project"). It is
licensed for use, not redistribution: it is gitignored and installed per machine; without it the stand-in drone is used.
Measured in 5.9 (`out/fab/SciFi_Drone_4-inspect.json`):
- `SKM_SciFi_Drone_4`: 92 × 138 × 42 cm. The long axis and the face are along +Y.
- Bones: `root` on the ground, `body` 158 cm above it, `camera` at the front underside, and two guns per side. No
  sockets.
- 25 animation sequences:
  - Idle 1–3 (10 s);
  - Move front/back/up/down, each start/loop/end (1 s);
  - Rotate L/R (1 s);
  - Slide L/R, start/loop/end (1 s);
  - Scanning (5 s);
  - Attack (5 s, unused).

**Art description, not code.** `ue/SCViewer/Config/Cargo/drone_art.json` names the skeletal mesh, the yaw that turns
its face to +X, the bone the beam leaves from (`camera`), the body bone that is centred on the actor, and which
sequence plays for which motion. No asset name appears in C++. With the file or the mesh missing, the K1d stand-in is
used as before.

**Size.** Native size (owner's choice, 2026-11-16). The hull bounds that flight, separation, sweeps and docking use
come from the mesh, as they do today.

**Heading** (owner's choice: face where it flies):
- In flight it turns toward its horizontal direction of travel, at a limited yaw rate. Turning in place plays Rotate
  L/R.
- Hovering, it keeps its heading.
- Carrying a box as a team, every drone faces the load's direction of travel.
- Docked, it faces out of its bay: it leaves forwards and backs in.

**Animations.** The sequence comes from the drone's velocity in its own frame:
- forward and back: Move front / Move back;
- sideways: Slide L / Slide R;
- vertical: Move up / Move down;
- moving into a direction plays its start clip, holding it the loop, leaving it the end clip;
- under a threshold speed: Idle;
- locking its beam onto a box: Scanning.

## K4b — the dock (Astra)

`ue/scripts/create_cargo_art.py` `dock()`: original art, no game geometry.
- **Footprint:** two lattice cells, authored at 250 × 125 × 125 cm.
- **Bays:** two, stacked. Each holds the drone at native size with clearance, its face toward +X.
- **Sockets:** `Bay_i` where the drone's body centre rests, `BayExit_i` in front of each bay's opening on +X,
  `BayLight_i`.
- **Placement in code:** the dock's size in cells comes from the mesh's bounds over the lattice step, and the dock
  takes that many cells on its grid. A dock that is not square turns only by half turns when it looks for a clear
  exit.

## K4c — the game's effects

To find before building anything:
1. What the tractor-beam record links for its beam visuals, in DataCore and the archive.
2. What a cargo box's collision or impact effects are, for example material effects by surface pair.
3. The format of those effects in LIVE 4.10.2, and whether StarBreaker reads it.
4. What a faithful UE version takes: Niagara built at run time, or a translated set.

Report with evidence before the build plan.

### K4c findings (2026-11-16, LIVE 4.10.2; scratch under `out/fk4c/`)

**Tractor beam.** The multitool tractor record (`dc0b1117-…`, `SWeaponActionFireTractorBeamParams`) lists its effects
in `beamGroup`:
- **Beam:** `beamEffects[0].particleEffect = 2c211ca4-…|Gadgets/tractor_beam.fps.beam_only` on helper `muzzle_flash`.
- **Muzzle:** `fireEffects[0]` → `tractorbeam/fps_tractorbeam.FPS_Tractor_Beam.Muzzleflash_1st`.
- **Hit:** `hitEffectName = Tractor_Beam_FPS`. In `Libs/MaterialEffects/MaterialEffects.xml` this is a custom effect that
  gives every surface `tractor_beam_fps:Default`, which leads to `Gadgets/tractor_beam.fps.impact`.
- **Beam strength:** `beamStrengthValues` (stable 1, unstable 0.5, no target 0; blend 2.0) feeds the effects'
  `EmitterStrength` curves.
- **Ship beam:** `WEP_TractorBeam_S1_Utility_1` has `Vehicle_Tractor_Beam` effects of its own (`tow_beam` impact).

An effect reference is `<GUID>|<library>.<effect>`, and the library is `Data/Libs/Particles/<library>.xml`.

**Box collisions: the effect is the ship's, not the box's.** This was found again once the CryXML fix restored element
text; the first pass had concluded "audio only".
- The box records (2183 `carryable_*`) attach no particle effect for collisions, only sounds. Their surface is
  `metal_dense`, fx group `metal`.
- Every ship has `CollisionMFXEntityParams.customEffect` (`Ships_Collision_Effects_S1…S5`; ground vehicles have
  `Vehicles_Collision_Effects_*`).
- In `MaterialEffects.xml` that custom effect maps the box's group, `metal`, to `sc_ships_surface_impacts:Metal_Small`
  (S1: `Metal_Tiny`; the large sizes: `Metal_Large` / `Giant`). That entry plays the sound
  `Play_SSDM_Shared_Collisions_Material_Metal` and the particle
  `c4b45b50-…|spaceships/surface_effects/Ground_Impacts.Metal.small_impact_gpu` (large: `large_impact_gpu`).
- That effect, all GPU emitters:
  - a flash, `spark_impacts1_2x2`, orange, 0.025 s;
  - smoke, 5 × `smoke_wispy_5x5` with flow map, 1.5 s, size 0.6, speed 3;
  - a spark burst whose children throw 15 × `spark_single1` sparks: size 0.02, speed 4, gravity 0.5, 1.4 s.
  - Strength scales most values; the data gives no impulse threshold for the particles.
- Box on hangar floor and box on box stay audio only: the default surface table's `metal` row plays no particles. That
  the engine plays a ship's effect for any object hitting it is inferred from the data's structure.

**Format.** 877 particle libraries, binary CryXML (`CryXmlB`):
- The legacy CryEngine `ParticleLibrary` format (`ParticleVersion="50"`), with Star Citizen modifier curves such as
  `EmitterStrength=(t=…,v=…)` and spline-guided GPU emitters (`ParticleSystem="RParticleGPU"`).
- Textures are named `.tif` but are `.dds` in the archive (glows, plasma flipbooks with flow maps, noise).

**StarBreaker** has no particle support. Its CryXML converter drops node text: `item_type` in `types.rs` is really the
text offset. As a result every `<Name>` in the effect libraries is lost. The fix needs a test first and goes on the
submodule branch `scviewer/cryxml-content`.

**Recommended way.** A few Niagara templates made by hand: a beam ribbon along a curve, an impact glow and a muzzle
flash. At run time they are driven by the values decoded from the library (colours, curves, sizes, counts, textures
through the existing DDS path) and by the beam strength. The web build needs those values baked at cook time.

The alternatives:
- an offline translation of each library emitter into Niagara assets (higher fidelity, much larger);
- a glow-material approximation.

### K4c build plan: port to Niagara (owner, 2026-11-16: "fix StarBreaker, reload the XMLs, port to Niagara")

1. **StarBreaker.** CryXML keeps element text: submodule branch `scviewer/cryxml-content`, 770884e6, with tests first.
   The effect libraries were re-extracted with their references intact.
2. **Rust, `scviewer-core` and the FFI:**
   - `sc_p4k_read` / `sc_p4k_read_xml` read any archive file; CryXmlB is decoded.
   - `sc_particle_effect(reference)` returns an effect and its emitters as JSON, with the CryEngine parameter syntax
     parsed: a value plus modifiers such as `Random`, `EmitterStrength` and `ParticleAge` curves.
   - `sc_material_effect(lib:effect)` resolves an FXLib entry to its particles and audio.
3. **Builder: a C++ commandlet in a small editor module, `SCBridgeFx`.**
   - The module sets `bTreatAsEngineModule`, so it may include Niagara's `Internal/Stateless` headers; the rest of
     `SCBridgeEditor` stays clear of that flag.
   - Each effect becomes a `UNiagaraSystem`. Each game emitter becomes a **stateless (lightweight) emitter**,
     duplicated from the engine's `FountainLightweight` system: the default template with all modules, each one
     switchable. It is added through `FNiagaraEmitterHandle(UNiagaraStatelessEmitter&)` and
     `UNiagaraSystem::AddEmitterHandleDirect`.
   - Module values come from the parsed parameters, written as the public `Stateless/NiagaraStatelessDistribution.h`
     types.
   - Textures come through the existing DDS path. The materials are two generic particle masters (additive and alpha
     blended, with texture, flipbook and emissive scale) from `create_materials.py`, which hold no game data.
   - Systems are saved under the gitignored `Content/SC/FX/<library>/<effect>`, as game-derived data like the cooked
     hangars.
4. **First set:**
   - `Gadgets/tractor_beam.fps.beam_only` (core, thin cores, glow, flare light);
   - `fps.impact`, and `fps.muzzle_1st`;
   - `spaceships/surface_effects/Ground_Impacts.Metal.small_impact_gpu` and `large_impact_gpu`.
5. **Runtime:**
   - The beam runs from the drone's emitter to the box: the system is oriented and scaled along the beam, and its
     strength follows `beamStrengthValues` (stable 1, unstable 0.5, no target 0).
   - The impact plays at the box and the muzzle at the emitter.
   - A box hitting a ship plays that ship's `CollisionMFXEntityParams.customEffect`, looked up with the box's surface
     group, at the contact point, scaled by impulse.
   - This waits for the overlap PR to merge (drone and cargo files).

Approximations, recorded per emitter in the build log:
- Spline-guided GPU emitters (`SplineGuided`, `EmitPerDistance`) become a stretched box along the beam with curl
  noise.
- Flares, flow maps and distortion are skipped at first.

### K4c, steps 1–4 done (2026-11-16)

**What was built:**
- **FFI:** `sc_p4k_read`, `sc_p4k_read_xml`, `sc_p4k_read_dds` (split mips merged, through the core's
  `read_package_dds`), `sc_particle_effect`, `sc_material_effect`; stubbed in `SCBridgeNoFFI.cpp` for the web build.
- **Core:** `archive.rs` (archive reads, a small XML tree), `particle_params.rs` (the CryEngine parameter syntax) and
  `particles.rs` (effects and material effects). CLI: `scviewer p4k-xml`, `effect`, `material-effect`.
- **Curve semantics, checked across all 877 libraries:** a key with no `t` or `v` means 0. A curve key's missing colour
  component is 0, a base colour's is 1.
- **Inheritance:** `Inheritance="Parent"` takes the parent's resolved values, merged field by field. Otherwise the
  defaults were inferred from the data.
- **Editor module `SCBridgeFx`** (`bTreatAsEngineModule`), commandlet `-run=SCFxBuild`. Its list is
  `ue/SCViewer/Config/FX/effects.json`; its output goes to `/Game/SC/FX` (gitignored). Each game emitter becomes a
  stateless emitter duplicated from `FountainLightweight`; the mapping rules are in the commandlet's header comment.
- **Materials:** masters `M_SC_Particle_Additive` / `_Translucent` from `create_materials.py`.
- **Runtime preview:** `sc.Fx.Spawn <path> [x y z] [dist= right= up= pitch= yaw= roll= rel=1 sx= scale= every=]` and
  `sc.Fx.Clear`.

**Mapping notes:**
- **Units:** game (x,y,z) becomes UE (y,x,z) × 100, as in the importer; the emit axis is system +X.
- **Size and brightness:** size is read as CryEngine's radius (sprite width = 2 × size). Emissive lighting is in scene
  units of 1000 cd/m², `FSCStarEngine::SceneUnitNits`.
- **Lights:** each becomes a light-only emitter.
- **Beam:** spline-guided emitters become a 0–100 cm box along +X, and the runtime scales the component's X to the
  beam length.
- **Strength:** `EmitterStrength` curves are baked at 1. `User.SC.Strength` is exposed but not bound yet.
- **Not ported:** flares, flow maps, refraction, soft particles, spline noise and curvature, velocity stretch.
- **Muzzle:** `fps.muzzle_1st` has every emitter disabled in LIVE 4.10.2. The tractor record fires
  `FPS_Tractor_Beam.Muzzleflash_1st`, a light only, which was built too.

**Verification:**
- `cargo test -p scviewer-core` 51 passed; `-p scbridge-ffi` 11 unit tests plus the header test passed. The ignored
  live tests passed with the archive. `tests/shell.rs` needs `python3`, which this host does not have.
- `build_ffi.ps1` and UBT `SCViewerEditor Win64 Development` succeeded.
- `-run=SCFxBuild`: `SC_FX_DONE systems=6 textures=7 materials=7 errors=0`.
- Previews in `hull_a_space` (`out/fk4c/fx-shots/`):
  - the beam is a thin glowing blue line;
  - the tractor impact is a blue glow;
  - the metal impacts are an orange flash, sparks and smoke.

### K4c fidelity pass (branch `wp/k4c-fx-fidelity`)

The approximations of steps 1–4, worked through one at a time with before/after previews and an aesthetic review by
Astra (`out/fk4c/astra/`); the data decides, Astra decides only what the data leaves open. Rules and their reasons are
in the header comment of `SCFxBuildCommandlet.cpp`; `-Features=` switches them off for comparisons.

**Runtime contract** (User parameters of the built systems; `sc.Fx.Spawn … strength= len= bend=` sets them):

| Parameter | Type | What the runtime sets |
|---|---|---|
| `SC.Strength` | float 0..1 | The effect's EmitterStrength input: the tractor's `beamStrengthValues` (stable 1, unstable 0.5, no target 0), blended by the runtime over time (`beamEffectBlendCoeff`). Default 1. |
| `SC.BeamEnd` | vec3 cm | The beam's end (the target), in the component's space; the component sits at the beam start (the emitter helper), any rotation. |
| `SC.BeamStartTangent` | vec3 cm | The emitter's forward direction in component space × the beam length. |
| `SC.BeamLength` | float cm | `|SC.BeamEnd|`. |
| `SC.BeamLength2` | vec2 cm | (`SC.BeamLength`, `SC.BeamLength`): stateless expressions cannot widen a float. |

Defaults (100 cm along +X) reproduce a 1 m straight beam. The component is no longer scaled.

**Per item:**
1. **Strength.** The game's strengths are read from DataCore (`effects.json` `strength`: the tractor record's
   `SWeaponActionFireTractorBeamStrengthValueParams`, values 0 / 0.5 / 1). Each EmitterStrength curve becomes the
   polynomial in `SC.Strength` through its values there (stateless expressions have add/multiply only): exact at the
   game's strengths. Colour and alpha go to Dynamic Material Parameter 0 (the masters multiply by it, clamped at 0),
   speed to the cone velocity scale, light intensity to the light's diffuse/specular scale. Count, lifetime and size
   curves stay at strength 1: UE 5.9 spawn rates evaluate no expression, lifetimes and random sizes take none.
2. **Beam length.** Spawn rates bind `SC.BeamLength`: round(EmitPerDistance / 100 / lifetime) spawn infos per emitter,
   so the beam holds EmitPerDistance particles per metre at any length (beam_core ×1.11 of the data, thin cores exact).
3. **Spline.** Particles start at the emitter and travel to `SC.BeamEnd` over their life on a quadratic Bezier whose
   control point is ½ (curvature × tangent + (1 − curvature) × end): velocity + constant acceleration, both stateless
   expressions of the User vectors, so the beam follows a moving target at once. Spline noise is a static curl field of
   the noise amplitude over the shortest spatial wavelength. Particle age therefore maps onto distance from the
   emitter (the data's fades read along the beam) — Astra flagged this as an interpretation to keep in mind.
4. **Flares.** `Flare` names `Data/Libs/Flares/<library>.xml` (LensFlareLibrary). Glow elements become camera-facing
   procedural glows ((1 − r)^Gamma), Ghost elements sprites of their texture; colour = the emitter's × Tint,
   luminance = root × element Brightness × light intensity, the light's age curve. Sizes are screen-space in
   CryEngine; here they are 2 × emitter Size × element Size × Scale metres × `flare_world_scale` (0.35, Astra). `FakeLight`
   lights only the flare.
5. **Soft particles.** Depth fade in the masters: SoftFade = FadeThickness, else half the sprite radius (Astra).
6. **Velocity stretch.** Ordinary emitters: ScaleSpriteSizeBySpeed (UE scales by speed² to a threshold). Beam
   emitters: + `SC.BeamLength` × stretch / lifetime of length, growing with the path travelled so it never reaches
   behind the muzzle (Astra: the centred full-length stretch spilled past both ends).
7. **Flow maps.** The masters blend flipbook frames along a flow-vector flipbook (FlowTexture × FlowMapIntensity in
   frame units). The flow maps are uncompressed RGB32 DDS, which the package writer rejects; SCFxBuild reads those
   raw.
8. **Refraction.** `M_SC_Particle_Distortion` (Substrate unlit, transmissive, pixel-normal-offset refraction) with
   NormalTexture or the sprite's alpha as a lens; one twin emitter per Refraction emitter. Verified with a ×40 test
   (`-RefractionScale=40`) bending the hull; at the data's 0.05 it is subtle (see round 2).
9. **Other.** RandomFlipU/V (per-particle mirroring in the masters), CameraMaxDistance (distance culling), inherited
   parent velocity (added to the speed range).

**Viewer stand-ins** (`effects.json`, chosen by Astra where the data is silent): `unlit_diffuse_scene_units` 0.35
(smoke was 2–4× too self-luminous at 1), `flare_world_scale` 0.35.

**Verification:** UBT `SCViewerEditor Win64 Development` succeeded; `-run=SCFxBuild` (all features)
`SC_FX_DONE systems=6 textures=10 materials=12 errors=0`; previews per item under `out/fk4c/fx-shots/r0-none` …
`r9-final`, sheets in `out/fk4c/sheets/`.

**Round 2 (defects in the final sheets):**
- **Blocky metal flash.** The flash texture (`spark_impacts1_2x2_diff`, BC7) is white RGB with the sparks in alpha.
  Its alpha ≥ 1/255 region is a blocky blob (BC7 4×4 blocks, 8-bit steps). At EmissiveLighting 10000 (7.2 M cd/m²),
  even alpha 1/255 clips to white, so the sprite showed that blob. The game removes the skirt with `AlphaErosion`
  (0.5 × an age curve), which the port skipped. Now AlphaErosion (own or inherited) drives Dynamic Material Parameter
  1, and the masters cut texture alpha below it and rescale the rest. Filtering, sRGB and tiling were checked and fine.
- **Beam layers apart.** Two port causes:
  - the thin cores (SplineCurvature 0.5) and the glow (0.6) took separate curves;
  - the glow's centred stretch, growing with age, reached past the target.
  All guided emitters of an effect now ride one curve: the effect's first guided emitter, since the weapon evaluates
  one beam spline. The stretch length follows a tent over age, 2 min(age, 1 − age), so a centred streak stays
  between emitter and target. There is no data on how the game blends per-emitter curvatures, so the single spline
  is an inference.
- **Refraction did nothing.** Three causes:
  - a new translucent material defaults to the After-DOF translucency pass, which the distortion pass skips: the
    master is now Before DOF;
  - the Substrate Unlit BSDF's Normal is world space (`CompileWithDefaultNormalWS`), so the tangent-space lens tilted
    the whole quad: it is now transformed tangent → world;
  - the coverage is now a Substrate weight, so the refracted scene blends by coverage, and pixels without coverage
    are clipped.
- **A 1 px rectangle around sprites.** This appeared during round 2. A bisect with `-Features` traced it to the
  random UV flips. The glow textures keep 2–3/255 alpha on their edge texels, and at EmissiveLighting 100–10000 that
  border lights up; the flip moved it onto the visible side. Frames are now sampled clamp-to-transparent: clamped
  half a texel inside their cell, fading to 0 over the outermost texel. That also stops bleed between flipbook
  frames.
- **Smoke stand-in.** `unlit_diffuse_scene_units` 0.35 is the value set after Astra's "2–4× too self-luminous, try
  0.25–0.5".

**Still not ported** (listed per emitter in `skipped`): random first flipbook frame (the stateless SubUV module starts
at frame 0), AlphaErosion, flare occlusion fade and sensor/dynamics terms, spline noise scrolling and damping, the
Count cap of per-distance emitters, strength curves on counts, lifetimes and sizes, collisions (Elasticity),
HalfRes / ResponsiveAA / MinPixels and other render-pipeline flags.

## Verification

- `SCBridge.Cargo` stays green, with new tests for the heading policy and the multi-cell dock footprint.
- The cargo routes and explorations end with `overlaps=0` and no new anomalies.
- Screenshots and a video of a team flying, attaching, carrying and docking: with the Fab drone, and without it (the
  stand-in).
