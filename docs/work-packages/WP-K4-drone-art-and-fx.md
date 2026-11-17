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
- `SKM_SciFi_Drone_4`: imported bounds 92 × 138 × 42 cm; the visible hull is 40 × 49 × 42 cm (the bounds include a
  scan-fan triangle, see "K4a … built" below). The long axis and the face are along +Y.
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

**Size.** Native size (owner's choice, 2026-11-16); in round 2 the owner chose about the stand-in's size, `scale` 1.9. The hull bounds that flight, separation, sweeps and docking use
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
- **Bays:** two (round 2: side by side, one per cell, open on the long +Y side). Each holds the drone with clearance,
  facing its opening.
- **Sockets:** `Bay_i` where the drone's body centre rests, `BayExit_i` in front of each bay's opening (the drone faces
  `Bay_i` → `BayExit_i`),
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

### K4a, the K4b code and the K4c runtime (branch `wp/k4a-fab-drone`, 2026-11-16)

**The drone measured again** (`out/k4a/fab-measure.json`, per material section through Geometry Script):
- The imported bounds (92 × 138 × 42 cm) are not the drone. `Scanner_Material` is one triangle, a 92 × 92 cm scan fan
  in front of the camera bone; it made the bounds 138 cm long and off-centre by 42 cm.
- The visible hull (body, visor, thruster flame) is **40 × 49 × 42 cm** about the `body` bone. At native size the drone
  is half as long as the stand-in (98 × 99 × 36 cm). `drone_art.json` `scale` changes that without code (owner's call).
- The clips move only bones below `root`; no root motion. The Rotate clips turn `body` 90° in 1 s. The Move clips
  raise or lower it by up to 10 cm and tilt it by up to 16°.

**Art description** (`ue/SCViewer/Config/Cargo/drone_art.json`, `FSCCargoDroneArt`): `heading` (yaw rate 90°/s, the
Rotate clips' pace; hover speed 20 cm/s; turn-clip rate 20°/s; crossfade 0.25 s; travel smoothing 0.2 s) applies to
every drone. `art` names the mesh, `scale` 1, `yaw_offset_deg` −90 (face +Y → +X), `centre_bone` `body`,
`beam_bone` `camera`, `beam_axis` down, `light_bone` `camera`, `yaw_lock_bone` `body`, `hidden_slots`
`Scanner_Material`, and the clips by motion. Without the file, or with `-SCDroneArt=none`, or without the mesh, the
drone is the K1d stand-in exactly as before (its hull from its static meshes, its rotors). Fab asset names appear only
in this file.

**Look and hull** (`ASCCargoDrone::SetupArt`): a skeletal mesh component, the centre bone's reference-pose place on
the actor origin, turned by the yaw offset, collision off. The hull is the visible sections' vertices (CPU copy of
LOD 0; hidden slots left out; the imported bounds if the vertex data is gone), carried by the centre bone through
every clip (9 samples each). The collision shape (`HullCentre` / `HullHalf`, world-aligned, used by every sweep,
overlap, route query and the tug) covers that hull **at every heading**: level, square in plan, half side = the
farthest corner's plan distance from the origin. `CARGO_DRONE_SHAPE half=(36.8, 36.8, 34.5) centre=(0, 0, −2.6)
radius_cm=52` (the stand-in: 48.5 × 49.5 × 18, radius 69). Heading is visual: it turns the mesh and moves the emitter
about the body, never the shape.

**Heading** (`FSCCargoHeading`, applied by the drone in every step it takes: `Lean`, `FlyTo`):
- the target is its bay's yaw while it is in its dock's corridor (docked, launching, docking: out forwards, in
  backwards);
- else the direction of its travel when that is at least the hover speed horizontally; its travel is its own
  velocity, smoothed, or, carrying as a team, its load's (the route's velocity: `SetLoadVelocity` from `Formation` and
  `TugStep`; zero while the box is at rest, so the team keeps its heading);
- else its current yaw (hovering, rising, descending);
- it turns toward the target at the yaw rate, the short way round.
The callers no longer pass a yaw (`Pose` / `Tug` / `FlyTo`). The tug physics is unchanged: the emitter is the body point
it flies, and the actor is placed about it as before.

**Clips** (`FSCCargoDroneMotion`, pure; `USCCargoDroneAnimInstance`, C++):
- From its smoothed velocity in its own frame: the dominant axis (front / back, left / right, up / down; another axis
  takes over once 25 % larger). It starts moving above 1.25 × the hover speed and idles under 0.75 × (25 / 15 cm/s).
  Hovering, it plays Idle, or Turn L/R when it turns at 20°/s or more. Scanning plays while its beam locks on
  (Attaching).
- A direction plays Start, then Loop. Leaving it to hover, turn or scan plays its End first. Leaving it for another
  direction crossfades straight into that one's Start, once the current clip has played `axis_dwell_s` (0.2 s); a 1 s
  End would lag the motion.
- Idle clips play once each and cycle; each drone starts at its own. Turn clips hold at least 0.35 s.
- The instance crossfades between two sequence slots (no anim graph, no Blueprint in the Fab folder). The turn clips play
  with a yaw lock: `body` keeps its reference yaw (swing-twist), so the actor turns and the clip adds only its tilt.
- `VisibilityBasedAnimTickOption = OnlyTickPoseWhenRendered`: a docked drone out of view costs no pose evaluation.
- The status light rides `light_bone`. The emitter is the beam bone's animated world position.

**Dock** (K4b code): `create_cargo_art.py` (Astra's two-bay dock) was run. Only `SM_CargoDroneDock.uasset` was kept;
the other regenerated `/SCBridge/Cargo` assets were reverted. Validation (`out/k1d/validation.json`): 250 × 125 × 125
cm, 8460 triangles, `Bay_0/1` (72, 0, 32/93), `BayExit_0/1` (150, 0, z), `BayLight_0/1` (112, 0, z + 24).
- `FSCCargoDockFootprint`: the cells are round(mesh size / lattice step) per axis, at least 1. The scale is uniform,
  the smaller plan ratio. A square footprint tries all four quarter turns for clear exits; a long one tries only 0° and
  180°.
- `SpawnDock` asks `FindSlot` for `cells × CellM`. A turned slot turns the actor (`SlotToWorld`), so the dock's long
  axis lies along the slot's. `CARGO_DOCK … cells=(2,1,1) rotated=0`.
- The drone in its bay (the hull posed over every clip, actor frame x −33…30, y ±34, z −37…32; the idle poses
  ±1 cm, ±17° yaw) clears both bays' decks, ceilings, cheeks and rear panels.
- The dock was dimensioned for the inflated bounds, and Astra found the native drone "stored at the back of an
  oversized rack" (`out/k4a/astra/dock.md`). Small changes in `create_cargo_art.py`:
  - `Bay_i` moved forward to (72, 0, z), the drone's face about 30 cm behind the front lip (the decks stay centred at
    `DECK_X` 20);
  - a cradle under each drone (two 46 cm guide rails with emitter pads, 4 cm under the hull);
  - `BayLight_i` moved to the front upper edge (112, 0, z + 24), where the dock now puts a neutral 8 cd fill.
  The footprint (250 × 125 × 125) and the rest of the art are unchanged; 8460 triangles.

**Runtime effects** (`FSCCargoFx`, `SCFxNames.h`):
- **Naming:** an effect reference `<GUID>|<library>.<effect>` → `/Game/SC/FX/<library>/<effect_with_underscores>`. It is
  split as scviewer-core's `split_reference`. `SCFxNames.h` is shared by SCFxBuild and the runtime.
- **Tractor:** `ApplyTractorRecord` now also reads, from the same `SWeaponActionFireTractorBeamParams`:
  - `beamGroup.beamEffects[0].particleEffect` (the beam);
  - `fireEffects[0].firstPerson` (the muzzle);
  - `beamGroup.hitEffectName`;
  - `beamStrengthValues`.
  The hit is resolved through `MaterialEffects.xml` (`CustomEffects/CustomEffect[name]/SurfaceTypeGroup[box group]`,
  else `default`) and `sc_material_effect`. The chain is `Tractor_Beam_FPS` → `tractor_beam_fps:Default` →
  `Gadgets/tractor_beam.fps.impact`.
- **Box contacts:** a box hitting a ship resolves the ship record's `CollisionMFXEntityParams.customEffect`
  (`Ships_Collision_Effects_S1…S5`) the same way. The data's selection rule is the ship's size class, not the impact:
  S1 `Metal_Tiny`, S2 `Small`, S3 `Medium`, S4 `Large`, S5 `Giant` (Hull A and C2: S5 → `Metal_Giant` →
  `large_impact_gpu`, `minscale` = `maxscale` = 3).
- **Client:** `FSCBridgeClient::P4kReadXml` and `MaterialEffect` (`sc_p4k_read_xml`, `sc_material_effect`), async.
- **Beam components:** three Niagara components per drone, absolute transforms, no scale.
  - Beam: at the emitter, +X along the beam axis. `SC.BeamEnd` = the attach point in its space,
    `SC.BeamStartTangent` = (length, 0, 0), `SC.BeamLength`, `SC.BeamLength2`, `SC.Strength`.
  - Hit: at the attach point, +X back toward the emitter.
  - Muzzle: at the emitter.
  - Off: `DeactivateImmediate` (the data's `killParticlesOnDeactivate`).

**Strength mapping:**

| Drone beam | Beam / muzzle `SC.Strength` | Hit `SC.Strength` |
| --- | --- | --- |
| On its box, not holding it (Attaching: locking on; Releasing) | `unstable` 0.5 | `unstable` 0.5 (valid target, not yet controlled) |
| Holding its box (Carrying: lift, transit, lower, place) | `stable` 1 | `impactControllingTarget` 1 |
| Lit | starts at `noTarget` 0 | starts at `noTarget` 0 |

The shown strength approaches the state's at `beamEffectBlendCoeff` per second: s += (target − s)(1 − e^(−2·dt)),
a 0.5 s time constant. The data does not state the unit; reading it as a rate is an inference. It holds across all 77
records (all 2.0); `beamEffectBlendRange` 0.8 is not used. The record has no "no target" beam in our flows: drones beam
only at boxes.

**Contacts** (`ASCCargoContainer` hit events → `USCCargoSubsystem::OnBoxContact`):
- Only against a vehicle of the set: the hangar, other boxes and drones play nothing, as the data has it.
- The threshold is the speed change along the normal (|NormalImpulse| / mass) of at least `contact_min_speed_cm_s`
  (100). The FXLib entry's minscale..maxscale spreads over 100..`contact_full_speed_cm_s` (350). At most one effect
  per box per `contact_min_interval_s` (0.5 s, game time). The numbers are Astra's; its proposed scale range 1–3 is not
  taken, because the data gives `minscale` = `maxscale` = 3.
- The scenario ship's contact effect is resolved at start, so the first knock plays too.
- These, and the boxes' surface group `metal`, are viewer stand-ins in `Config/FX/effects.json` `runtime`. The data has
  no impulse threshold (CryEngine's `mfx_*` thresholds are engine settings), and no box surface reaches the runtime.
- `CARGO_FX_CONTACT box ship at dv_cm_s scale`.

**Fallbacks** (each logged once):
- No archive, `-SCNoCargoFx`, or the beam system not built: the K1d beam mesh (`CARGO_FX the drones draw the beam
  mesh: …`).
- A missing hit or muzzle system: that component is skipped.
- A missing contact system, or none in the data: nothing.

**Packaging:**
- `SCBridgeEditor` registers a `ModifyCook` delegate that adds the packages `drone_art.json` names, when this machine
  has them, and everything under `/Game/SC/FX` (`SC_COOK drone art packages=… effect packages=…`). A machine without
  them cooks without them. No asset name is written in an ini.
- `SCViewer.Build.cs` stages `Config/Cargo/drone_art.json` and `Config/FX/effects.json` (UFS).
- Verified (2026-11-17) by a Win64 Development `package_showcase.ps1` build (DRAK_Golem, hightech_a/hangar_smltop_001):
  `SC_COOK drone art packages=25 … effect packages=57`, both JSON files in the UFS manifest. The packaged viewer ran
  `c2_hangar.json`: `CARGO_DRONE_SHAPE … look=art`, the tractor and contact effects resolved, docks `cells=X=2 Y=1 Z=1
  bays=2`, `CARGO_JOB id=1 state=Done … overlaps=0`.
- That run found the plugin's cargo art (`/SCBridge/Cargo`: the stand-in drone, the dock, the beam mesh, all loaded by
  path) missing from packaged builds since K1d: the dock fell back to a cube with no bays. `DefaultGame.ini` now
  always cooks `/SCBridge/Cargo`.

**effects.json:** `material_effects` adds `Metal_Tiny`, `Metal_Medium`, `Metal_Giant` (the S1–S5 ship sizes) and
`tractor_beam_fps:Default`. These build no new system; they share `small` / `large_impact_gpu` and `fps.impact`. The
`runtime` block is described above.

**RPC state:** drones add `yaw`, `heading` and `clip`; docks add `yaw`, `bays`, `fwd` and `up`.

**Switches for comparisons:** `-SCDroneArt=none` (or `=<file>`) and `-SCNoCargoFx`.

**Astra** (`out/k4a/astra/`):
- **dock.md:** "stored at the back of an oversized rack". Its smallest fix was taken: the sockets moved forward, plus
  the cradle and the bay fill light. Its proposals to rescale the drone or resize the rack were not taken (native size
  and two cells are the owner's choices).
- **dock2.md:** "much more clearly docked and facing the launch opening". It still finds the drone hovering above the
  cradle (open).
- **flight.md:**
  - Taken: hover hysteresis 15 / 25 cm/s and a 0.2 s axis dwell.
  - Not taken: slowing travel while the heading error is over 45°, because heading is visual and flight timing stays
    as planned. A lens emissive accent is not taken either, because it would mean editing the licensed asset.
  - Its notes on the hit-glow size and the pink-to-blue blend are the effect data's (fps.impact, beamEffectBlendCoeff),
    so they stay.
- **fx.md:**
  - The beam reads as holding the box, and unstable versus stable is legible (colour and continuity).
  - The impact reads as a hit.
  - The contact threshold 100 cm/s and full speed 350 cm/s are taken.

**Verification (2026-11-16):**
- UBT `SCViewerEditor Win64 Development`: `Result: Succeeded`.
- `Automation RunTests SCBridge.Cargo`: 53 / 53, the 48 before plus `DroneHeading`, `DroneMotion`, `DockFootprint`,
  `DroneArtParse` and `FxNames`; `TractorRecord` was extended.
- `-run=SCFxBuild`: `SC_FX_DONE systems=10 textures=10 materials=12 errors=0`.
- Runs (RPC harness, `out/k4a/<tag>.out`, logs `Saved/Logs/K4a-<tag>.log`), every job `Done` with `overlaps=0` and
  no `CARGO_DRONE_OVERLAP`:
  - `hull_a_hangar` 16 SCU elevator → ship with the art (h1), and with `-SCDroneArt=none -SCNoCargoFx` (s1: the
    stand-in, `CARGO_DRONE_SHAPE half=(49.15, 48.15, 17.80)` as before, the beam mesh);
  - `c2_hangar` 32 SCU, a team of two (c1, c2, c3);
  - `hull_a_space_fleet` 16 SCU, two moves (f1).
- A 16 SCU box dropped 12 m onto the C2 played `CARGO_FX_CONTACT … dv_cm_s=377 scale=3.00` (Metal_Giant).
- Shots: `out/k4a/shots/<tag>/` and videos `<tag>-transfer.mp4`, `<tag>-drop.mp4`.
- Sheets in `out/k4a/sheets/`:
  - `k4a-1-dock`, `k4a-2-launch-flight`, `k4a-3-team-beams`, `k4a-4-return-dock`, `k4a-5-box-knocks-ship`;
  - `k4a-6-standin-no-fx`.

**Open:**
- **Overlaps at 1.9× while tugging** (round 2): fixed in round 3 (the body held to the actor, and the shortest hang
  respecting the hull); every round 3 run ends with `overlaps=0`.
- **Size.** Resolved in round 2: scale 1.9 (owner's decision, Astra's pick).
- **The drone hovers over its cradle** (by design: a legless drone; round 2 brought support blocks to 3–4 cm under it). Astra would seat it 0–1 cm above the pads with a
  ±0.5 cm idle bob. That needs the hull's underside profile, and the idle clips move the body ±1 cm.
- **The box surface group** is a config stand-in (`metal`). Reading it would take the box package's material
  `surface_type` (and SurfaceTypes.xml's fxGroup) through to the runtime.
- **Without the archive** (the web build) the effects resolve to nothing, and the beam mesh is drawn. The scenario's
  tractor fallback carries no effect references yet.
- ~~**Cook rule.**~~ Verified by a packaged build (see Packaging); it also fixed `/SCBridge/Cargo` not being cooked.
- **Bay exits.** The dock's bay-exit probe (a cell-sized sphere at each exit) is unchanged from K1. A sphere at a low exit
  may reach the floor and fail every turn, and the dock then keeps its base turn. This was not examined.
- **The stand-in's AABB.** The stand-in keeps its world AABB at spawn as its shape (unchanged; inexact at 45°
  headings, as before).

### K4a round 2 (2026-11-16): the drone at ~90 cm, the side-by-side dock, review fixes

This round supersedes the size, dock and beam-strength notes above where they differ.

**Size (owner):** the Fab drone is scaled to about the stand-in's size, `drone_art.json` `scale` **1.9**: about 94 × 76 × 80
cm (hull 40 × 49 × 42 × 1.9).
- Astra picked 1.9 from editor renders at 1.85 / 1.9 / 2.0 in the new bay (`out/k4a/astra/r2dock.md`). It found 1.85
  slightly loose and 2.0 crowding the cradle; 1.9 "gives the drone convincing presence while preserving visible
  clearance".
- The hull, sweeps, separation and docking follow the scale: `CARGO_DRONE_SHAPE half=(69.85, 69.85, 65.59)
  centre=(0, 0, −4.91) radius_cm=99`, plan radius 69.8. The posed hull at 1.9 is x −63…58, y ±65, z −70…61.

**Dock (K4b, redone):** `create_cargo_art.py` `dock()`, 250 × 125 × 125 cm, 5580 triangles.
- **Layout:** one bay per cell, side by side, each open on the dock's long +Y side. A yellow frame, a closed back
  with fins, closed ends and a centre divider.
- **Per bay:** a charging spine whose top stands ~10 cm above the drone, with a cyan indicator. A two-rail cradle with
  support blocks and pads 3–4 cm under the drone's body. A cyan threshold strip, two chevrons pointing out, bay
  ticks, and a light bar under the roof's front edge.
- **Sockets:** `Bay_i` (∓62.5, 7, 57): the face is ~12 cm behind the lip at 1.9. `BayExit_i` (∓62.5, 145, 57): the
  drone's tail clears the roof edge by ~20 cm before it climbs (Astra, r2final). `BayLight_i` (∓62.5, 50, 112): the dock's neutral 8 cd fill.
- **Previews:** `create_cargo_art.py` now shows the drone `drone_art.json` names, when installed, in the preview bays.
  The script takes `-CargoArtDroneScale=` for comparisons and names no asset.
- **Astra (r2dock):** "reads clearly as two forward-facing drone bays". Its contact and spine suggestions were taken
  (support blocks, taller spine with an indicator).
- **Docked heading from the bay:** `BayTransform` faces from `Bay_i` toward `BayExit_i`, level in the dock, so a
  bay may open on any side. The docked, launching and docking heading follows it. In C2 / Hull A hangar runs the drones
  dock at yaw 0 in a dock of yaw −90: the openings are on the dock's +Y.
- **Exit-clear turn search:**
  - Before, it probed a sphere of the cell's size at each exit. Low exits reached into the floor, so no turn was ever
    clear.
  - Now the probe is the cell in front of each bay, a box in the dock's frame from 2 cm above the dock's floor.
  - Half turns for this non-square dock; every `BayExit` must be clear. `CARGO_DOCK_EXITS grid=… turn0/bay0=clear
    turn0/bay1=clear -> turned`.

**Flight tilt:**
- Measured through the RPC (`pitch` / `roll` per drone), the tilt comes from both sources compounding. The actor's
  lean was 8° per full thrust plus the gust wobble (±3°); the Move / Slide clips lean the body up to ~12° themselves.
- `drone_art.json` `actor_lean_scale` (0.25) now scales the actor's lean when the art is in use; the stand-in stays
  at 1.
- In the C2 run the actor's pitch / roll stay within ±4° (gusts included). The clips carry the lean.

**Beam colours:**
- The port fitted a quadratic in `SC.Strength` through the game's strengths (0, 0.5, 1), which overshot between them.
  The impact's blue went 12 % over at 0.75 and its red under.
- Stateless expressions have no clamp. Each EmitterStrength curve of ≤ 4 keys is now exposed as `SC.SK<i>.T` / `.V`
  and bound through `SC.SV<i>`. The runtime evaluates it piecewise linear over strength (`SCFxStrength.h`, shared by
  the drones and `sc.Fx.Spawn`). That is exact at every key and monotone between them.
- The magenta mid-blend stays, because it is the data's: the impact's colour keys are red-orange (0.643, 0.016,
  0.027) at strength 0.05 and blue (0.106, 0.263, 0.784) at 0.41. Componentwise between them lies magenta, which the
  beam passes through while it blends from no-target (0) to unstable (0.5).
- Strengths 0 / 0.25 / 0.5 / 0.75 / 1: `out/k4a/sheets/r2-fx-strength.png`. At 0.5 and above the curves are flat,
  so 0.5, 0.75 and 1 are identical.

**Box-on-ship impact:**
- The right system played: Metal_Giant → `large_impact_gpu`, at the contact point, along the normal, scale 3.
- The scale did not reach the sprites. A local-space stateless emitter takes the component's scale on positions and
  speeds, but no sprite size takes it, so a scale-3 hit drew scale-1 sparks and smoke.
- SCFxBuild now ranges every sprite's size curve by a user `SC.Scale2` (vec2, default (1, 1)). The contact and
  `sc.Fx.Spawn scale=` set it.
- Against black space the effect is complete: flash, sparks and smoke, at scale 1 versus 3 (`r2-fx-impact-scale.png`).
- In the hangar the hit is now shot in slow motion against the back wall (`r2-box-knocks-ship.png`).

**Launch shots:** `k4run` frames the first drone's bay before it launches and shoots it leaving forwards
(`r2-launch.png`).

**Astra, final round-2 review** (`out/k4a/astra/r2final.md`, on the r2 sheets):
- **Dock:** pass ("the larger drones fill the bays convincingly").
- **Launch and flight:** pass ("read as powered flight rather than tumbling, with the nose aligned to the apparent
  travel direction"). Its one change, 15–20 cm of clearance beyond the roof edge before the climb, was taken
  (`BayExit` y 125 → 145).
- **Beams:** pass. The data-driven blend "reads acceptably".
- **Hull impact:** qualified pass. It asked to check that the effect starts at the box–hull contact: it does
  (`Hit.ImpactPoint`, logged; the sparks rise along the hull's normal as the data throws them).

**Verification (round 2):**
- UBT `SCViewerEditor Win64 Development`: `Result: Succeeded`.
- `-run=SCFxBuild`: `SC_FX_DONE systems=10 textures=10 materials=12 errors=0`.
- `Automation RunTests SCBridge.Cargo`: `TESTS success=53 fail=0`.
- Runs, each job `Done` and every dock's `CARGO_DOCK_EXITS … -> turned`; `overlaps=0` except where noted:
  - c2_hangar 32 SCU team: c4 0; h3 1 (a drone sliding under a hangar ceiling structure); h4 2 (each drone of
    the team against its own box while tugging; see Open);
  - hull_a_hangar 16 SCU, art (h2);
  - hull_a_space_fleet 16 SCU, two moves (f2);
  - hull_a_hangar with `-SCDroneArt=none -SCNoCargoFx` (s2: the stand-in, half (48.15, 49.15, 17.80) as before).
- Drops onto the C2: `CARGO_FX_CONTACT … dv_cm_s=531 scale=3.00` (c4) and `dv_cm_s=523` (d1).
- Sheets in `out/k4a/sheets/`: `r2-dock-ingame`, `r2-dock-art`, `r2-dock-scales`, `r2-launch`, `r2-flight`,
  `r2-team-beams`, `r2-return-dock`, `r2-fx-strength`, `r2-fx-impact-scale`, `r2-box-knocks-ship`.

### K4a round 3 (2026-11-16): the overlap fix

The round 2 dock, size, beams and flight tilt were accepted. Round 2 left overlaps while tugging (h3 1, h4 2). Two
changes fix them; every run is back to `overlaps=0`.

**1. The actor carries the motion; the clips may not move the body.**
- `USCCargoDroneAnimInstance` holds the centre bone (`centre_bone`, `body`) at its reference place in every clip,
  within `art.body_bob_cm` (default and config 0; converted to mesh units by the scale), as it already locks its yaw in
  the turn clips.
- The clips keep their rotations (lean, tilt, look-around) and their moving parts (guns, camera).
- The hull is built from the poses as they render with that lock.
- Before: `CARGO_DRONE_SHAPE half=V(X=69.85, Y=69.85, Z=65.59) centre=V(Z=-4.91) radius_cm=99`, posed z −70.49…60.68.
- After: `CARGO_DRONE_SHAPE half=V(X=69.85, Y=69.85, Z=50.69) centre=V(Z=-5.54) radius_cm=99`, posed z −56.23…45.16.
  The rest above the reference ~40 cm is the clips' tilts (up to ~16°) of a 94 cm hull.

**2. The shortest hang respects the hull.**
- `FSCCargoTug::HullHangCm` gives how close a drone's emitter may come to the face its beam holds: the emitter's
  height over the hull's bottom (`ASCCargoDrone::HullHangCm`, reference pose) plus `OwnBoxGapCm` (2).
- `FSCCargoTug::ShortestCm` is the larger of the record's minimum and that hang, plus the share's stretch.
- It sets the tug's shortest beam (`TugStep`: the room floor, the tuck while held up, `TuckUntil`, the no-station
  test) and every `MinHangCm` (the formation hang, the lift's reserved envelope).
- The stand-in: emitter −19, hull bottom −20.15 → hang 3.15 cm: the record's 50 cm stands, unchanged.
- The Fab drone after (1): camera −29, hull bottom −56.2 → 29.2 cm: the record's 50 cm still decides, now with 21 cm
  to spare (before (1) the hull reached 41 cm below the emitter and touched its box under the beam's compression).
- Test `SCBridge.Cargo.HullHang`.

**Verification (round 3):**
- UBT `SCViewerEditor Win64 Development`: `Result: Succeeded`.
- `Automation RunTests SCBridge.Cargo`: `TESTS success=54 fail=0` (53 + `HullHang`).
- Runs, all `CARGO_JOB … state=Done … overlaps=0` with no `CARGO_DRONE_OVERLAP` line:

| Run | Scenario | Job line |
|---|---|---|
| r3c4 | c2_hangar, team, slow motion 0.3 | `t=49.6 moves=1 done=1 overlaps=0` |
| r3h3 | c2_hangar, team, slow motion 0.3 | `t=49.7 moves=1 done=1 overlaps=0` |
| r3h4 | c2_hangar, team, real time | `t=54.1 moves=1 done=1 overlaps=0` |
| r3h2 | hull_a_hangar, art | `t=56.6 moves=1 done=1 overlaps=0` |
| r3f2 | hull_a_space_fleet | `t=56.4 moves=2 done=2 overlaps=0` |
| r3s2 | hull_a_hangar, stand-in, no effects | `t=55.6 moves=1 done=1 overlaps=0` (shape (48.15, 49.15, 17.80) as before) |

- Re-shot: `out/k4a/sheets/r3-flight.png` and `r3-team-beams.png` (run r3c4).

## Verification

- `SCBridge.Cargo` stays green, with new tests for the heading policy and the multi-cell dock footprint.
- The cargo routes and explorations end with `overlaps=0` and no new anomalies.
- Screenshots and a video of a team flying, attaching, carrying and docking: with the Fab drone, and without it (the
  stand-in).
- Done for K4a, the K4b code and the K4c runtime on 2026-11-16: see "Verification (2026-11-16)" above.
