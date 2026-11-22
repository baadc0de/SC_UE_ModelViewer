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
| Round 4 | A pair holding a box from its ends where the room over it is short; each drone's own attitude dynamics (below) | Opus 5.5, reviewed by Astra |
| Round 5 | The hold to a point; each drone searching for its grip (the ends about 3 of 4 times, where they fit); its wobble on the box; the fleet, the zig-zag (below) | Opus 5.5, reviewed by Astra |

## K4a — the Fab drone

**Asset.** `viewer/Content/SciFi_Drone_4/` (UE 5.7 pack, added through the Epic launcher's "Add to Project"). It is
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

**Art description, not code.** `game/Config/Cargo/drone_art.json` names the skeletal mesh, the yaw that turns
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

`game/scripts/create_cargo_art.py` `dock()`: original art, no game geometry.
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
through the existing DDS path) and by the beam strength.

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
  `read_package_dds`), `sc_particle_effect`, `sc_material_effect`.
- **Core:** `archive.rs` (archive reads, a small XML tree), `particle_params.rs` (the CryEngine parameter syntax) and
  `particles.rs` (effects and material effects). CLI: `scviewer p4k-xml`, `effect`, `material-effect`.
- **Curve semantics, checked across all 877 libraries:** a key with no `t` or `v` means 0. A curve key's missing colour
  component is 0, a base colour's is 1.
- **Inheritance:** `Inheritance="Parent"` takes the parent's resolved values, merged field by field. Otherwise the
  defaults were inferred from the data.
- **Editor module `SCBridgeFx`** (`bTreatAsEngineModule`), commandlet `-run=SCFxBuild`. Its list is
  `viewer/Config/FX/effects.json`; its output goes to `/Game/SC/FX` (gitignored). Each game emitter becomes a
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

**Since WP-E1 E1-6** (the fork's stateless expressions) this table is the whole contract: the built systems evaluate their
EmitterStrength curves (exact piecewise-linear clamp expressions in `SC.Strength`) and the beam emitters' spawn rates (expressions of
`SC.BeamLength`) themselves, and an effect's scale is the component's scale (sprite sizes through Niagara's Apply Owner Scale To
Attributes module). `SC.SV<i>` / `SC.SK<i>`, `SC.BR<i>` / `SC.BK<i>` and `SC.Scale2` (below, K4a round 2) are gone; systems built
before E1-6 need a rebuild (`-run=SCFxBuild`). Since the E1-6 review the contract is versioned: every built system carries the int User
parameter `SC.FxContract` (`SCFxRules::FxContractVersion`, 2); the runtime warns `SC_FX_STALE` (non-shipping) for a system without it or
with an older one, `-run=SCFxBuild -VerifyContract` fails on one, and `game/scripts/package_showcase.ps1` refuses to package while one exists.

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

**Art description** (`game/Config/Cargo/drone_art.json`, `FSCCargoDroneArt`): `heading` (yaw rate 90°/s, the
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
- **Without the archive** (a cooked showcase on a machine without Star Citizen) the effects resolve to nothing, and the beam mesh is drawn. The scenario's
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

### K4 round 4 (2026-11-17): held from the ends where the room is short, and each drone's own wobble

Branch `wp/k4a-drone-height-beam-spawn`, on top of the fixes there (drone height from the hull, `DroneHullRule`, the lift
plan ignoring loads in flight, the column never ending below the hulls, docked drones powering down).

#### A. Holding a box from its ends (owner: "it would be better if the drone was in front or behind a container ... the
C2 is a good example")

**Placement** (`FSCCargoEndGrip`, `Cargo/SCCargoTug.h`, pure; tests `SCBridge.Cargo.TugEnds`, `TugEndsSwap`):
- A pair holds the box from the two end faces of its long axis, one drone beyond each, at about the box's height. Each
  beam leans out from the vertical by an angle θ to an attach point on its end face.
- **Force rule.** A beam is a spring along itself, so on station at rest it lies along the force it gives: half the
  weight straight up (`ShareN` = m g / 2) and a pull out along the axis, `TensionN` = ShareN tan θ. The two pulls cancel;
  F = ShareN / cos θ must stay within the record's `MaxForceN`. Under the route's acceleration each beam gives half the
  net force plus its pull (`Force`); the route is flown no harder than keeps both beams within `MaxForceN` and their
  forces within `EndsMaxTurnDeg` (15°, viewer choice) of their lines, in every direction (`AccelLimit`; the route's
  acceleration then as `RouteAccel`: the scenario's minimum kept, over √2).
- **Geometry, from the record, the load and the hull** (no constants): the smallest θ (0.5° steps) at which the emitter is
  far enough out for the hull, at any heading, to clear the end face (hull half + the emitter's plan offset + 2 cm), the
  beam between the record's minimum plus its stretch and the full-strength length, and the hull's top at or under the
  box's top with the attach point on the face (10 cm in from its edges, `EndsInsetCm`). A box lower than that: the
  lowest drone. The attach point is as high on the face as keeps the hull's top at the box's top.
- **Only a pair.** One beam alone pulls the box sideways (its pull unopposed), so a lone drone cannot hold a box so; in zero
  g there is no weight to lean against (held over). The volume rule is unchanged (per beam: the pair holds 2 × 20 SCU).
- The C2 numbers (`CARGO_GRIP … ends`): in k4b-c2b θ 35.0°, beam 195 cm, out 112 cm, rise 159 cm, attach 114 cm under
  the centre; 32 SCU: 179.5 kN of 257.5 (tension 102.9 kN), 24 SCU: 157.0 kN; acceleration limit 3.11 m/s², route
  1.49 m/s² (held over: 4.44). Final (hull 72.44 cm): 16 SCU with a pair, θ 34.0°, beam 197 cm, 88.7 kN, route
  1.46 m/s². The shape is the box's height, 2 × 2.2 m longer.

**In flight** (`TugStep`, `Formation`): the stations are fixed in the box's frame (`Station`), each beam's anchor set for
its force (half the net and its pull). The drone keeps off its end face (`FSCCargoTug::FStation::KeepOffFace`, and its
own steps: `FSCCargoDroneSteer::OffFace` now takes the face its beam holds, `ASCCargoDrone::HeldFace`).

**Planner** (`PlanMove`, in turn, `CARGO_GRIP_TIER` when one fails):
1. held over with a drone's half clearance (20 cm) kept over the hulls everywhere (`bRoomy`; the precise zone had left
   0 cm, so the drones on their stations slid along the C2's hold-entrance ceiling);
2. from the ends (a pair; a lone drone's move asks for a second drone, `CARGO_GRIP_PAIR`, claimed in `StartMove` when one
   is free, refused when its owner has fewer than two);
3. held over as tight as it fits (the old way), last. Holds (boxes dug out and put back) are held over.
Full-cell searches first; half steps (`bRefine`) for tiers 1 and 2.

**Transitions** (`SwapGrip`, `CARGO_GRIP_SWAP`): the box held still (rigidly on its route point), each drone flies round it
to its new station: up to the higher of the two heights, across, down (`FSCCargoEndGrip::SwapWay`; its hull never enters
the box: tested), at 0.35 of its top speed; its beam slides from the top face down the end face. A swap needs its room
(`SwapClear`: the column over the box, each drone's whole way and its end station).
- **To the ends:** at the top of the lift (not into a hold), or after the turn onto a hold's lattice (by the lift or at its
  entrance): the second route piece is flown from the ends (`EndsPiece`).
- **Back over the box:** at the nearest point over the destination lattice at the goal's height where the swap is clear
  and from which the load held over sweeps straight to its goal (`SwapOutPoint`; `OverLeg`); else, onto a level slot, held
  from the ends down onto it (`bEndsToSlot`: the descent sweeps the drones beside the box too, `DescentClear`).
- Every query of the load takes the grip's shape (`LoadQuery`: the box and the column, or the box and the two end hulls,
  `EndsQuery`), so the guard, replans, the right of way and `HoldShort` follow it.

**Also fixed on the way:**
- Route searches share 2 ms of *search* time per frame (`SearchSpent`); the planning around them (overlaps, the entrance
  search repeated every tick) had eaten the budget, and one C2 move planned for 115 s.
- A deadlock: a team attaching over its box waits for loads lifting near its way up, and such a load was stopped by the
  space kept for that team's drones (`CARGO_REPLAN … waits_for_drones=1` for 180 s; move 1 failed). A load planned first
  keeps no space for a later team still attaching (`KeptSpace`); those drones step out of a load's way (`DodgesLoads`).

**Runs** (logs `viewer/Saved/Logs/<tag>.log`, outputs `out/k4b/<tag>.out`):

| Run | Scenario | Job line | Notes |
|---|---|---|---|
| c2full5 (before) | c2_hangar 208 SCU elevator → C2 | `t=204.7 moves=9 done=8 overlaps=3` | move 9 "no way into Grid 1 lengthwise"; 33 carrying-drone blocks, 18 under the hold-entrance ceiling (Mesh_3639) |
| k4b-c2b (round-1 look) | the same | `t=598.0 moves=9 done=9 overlaps=0` | moves 3, 4 and 9 from the ends; 8 carrying-drone blocks, 0 at Mesh_3639 |
| k4b-c2r5 (final) | the same | `t=340.5 moves=9 done=9 overlaps=0` | move 9 from the ends (a pair); 6 carrying-drone blocks, 0 at Mesh_3639 |
| k4b-hullar5 (final) | hull_a_hangar 16 SCU, environment → ship | `t=54.0 moves=1 done=1 overlaps=0` | held over (tier 0) |
| k4b-fr5 (final, as r3f2) | hull_a_space_fleet 16 SCU | `t=200.4 moves=2 done=2 overlaps=0` | held over; slower: see Open (k4b-fr4, one build earlier: `t=63.9 … overlaps=0`) |

- **Move 9** was the headroom problem: a 16 SCU box for the C2 hold's upper layer; held over, its column at the hover point
  needed 4 cm more than the 164 cm under the hold's ceiling. Now its single drone asks for a second, the pair takes it in
  from the ends and lowers it onto its slot from the ends (`origin=X=0 Y=1 Z=2`).
- **The overlaps' causes, fixed.** In k4b-c2r4 the baseline's kind came back: an empty drone flying out through the C2's
  ramp met a load coming in and could not step aside there (`… state=2 … evaded_cm=38 slid_cm=20`: the box came onto it).
  `HoldShort` looked only at the drone's station; it now holds a drone short of the first point of its whole way ahead that
  a load planned before it has yet to fly through. The same run had a carrying drone pushed onto its own box by its
  teammate's separation; its step now keeps off its own box's face after that push too (`ASCCargoDrone::Step`). k4b-c2r5:
  `overlaps=0`. (Not unit-tested: both live in the subsystem and the drone's world step.)
- **Time** (340.5 s, before 204.7 s; the last box placed at 331.1 s against 192.6 s): the moves still overlap (no idle
  gap over 2.4 s), but they wait more on each other. Outbound legs took 251 s together (92 s): drones now hold short of
  a load's way across their own, not only of their stations (16 holds), and move 9 waited for a second drone. Transit
  legs took 368 s (249 s): 27 waits for a load ahead (10), move 1's load stopped ~23 s by the stations kept for move 2's
  team on its way out, moves 7 and 8 about 59 s each (46 s, 32 s). Held from the ends a leg is flown at 1.46 m/s²
  (held over: 4.44) with one or two 5–7 s swaps; in the final run only move 9 needed it.
  The intermediate 598 s run (k4b-c2b) had a further cause, fixed since: with the round-1 attitude envelope (hull half
  73.95 cm) the C2's own dock could not let its two drones out (`CARGO_DRONE_BAY_BLOCKED drone=5/6`), so the job ran on
  four drones, mostly one move at a time, and three moves were flown from the ends.

#### B. Each drone's own wobble (owner: "more individuality is needed, even if it is faked or forced ... real drones are
much less 'stable'")

**Model** (`FSCCargoAttitude`, `Cargo/SCCargoDroneArt.h`, pure; tests `SCBridge.Cargo.DroneAttitude`,
`DroneIndividual`, `DroneAttitudeBounds`), after a multirotor's (Embark has published nothing on ARC Raiders' drones):
- **Moving is tilting.** The attitude is commanded to `tilt_gain` × the thrust's tilt atan(a / g) for the body's
  acceleration in its own frame (g 9.81 m/s² for the look, in vacuum too), each drone its own share within ±`gain_spread`;
  nose down to go forward, rolled into a push sideways; the gusts' wobble on top.
- **An underdamped spring** (natural frequency and damping ratio per drone, from its id, in the configured ranges) that
  leads with `tilt_lead` × the command's rate: a start kicks in and overshoots, a stop rocks back past level and settles
  in decaying swings. Carrying, the lead is `tilt_lead_carrying` (the team's shared starts and stops softer).
- **Its own corrections:** random kicks (Poisson, `kick_rate_hz`, `kick_deg_s`, each drone its own timing) with a new
  heading to wander to (`yaw_jitter_deg`), an altitude hunt (two tones, `hunt_cm`, `hunt_hz`), and a drift where its tilt
  runs ahead of its command (g tan(error), at most `drift_cm`). `max_tilt_deg` bounds the tilt. Sub-stepped at 240 Hz.
- **Each drone its own onsets and directions** (owner: "the initial vector directions for twitching/wobble need to be
  independently randomized and pick from a plausible biased distribution"), each drawn from the drone's own stream, per
  event: it answers its command `onset_min_s`..`onset_max_s` late (0.04–0.26 s, spread over the drone ids by the golden
  ratio: any two of eight at least 20 ms apart); a motion starting or changing (the command moving on by `event_deg`, at
  most one per `event_min_s`) kicks it, a further 0..`onset_jitter_s` later; its corrections kick mostly back towards
  level. Every kick's direction is a wrapped normal (`scatter_deg` 30) about the expected one, `off_axis_share` (15 %) a
  quarter turn to either side and `opposite_share` (7 %) the other way; its size log-normal (`kick_log_sigma` 0.45) about
  its median (`event_kick_scale` 0.6 × `kick_deg_s` for a start). Tests `DroneOnset`, `DroneKickDraw`.
- **Visual layer.** The actor stays level (its yaw is its heading); the look (art or stand-in) hangs under a new
  `Attitude` component turned and offset by the dynamics. The clips' own lean of the centre bone is scaled by
  `art.clip_tilt_scale` (the anim instance's `TiltScale`). A drone not posed in a frame (docked; or arrived and waiting for
  its teammate) goes on settling there (Tick), powered down without corrections.
- **Hull, honestly:** it is built over the envelope: every pose of the clips as they render (scaled tilt), each tilted by
  `max_tilt_deg` every 45° round about, plus `drift_cm` in plan and `hunt_cm` up and down (the stand-in's shape too).
  `CARGO_DRONE_SHAPE half=(72.44, 72.44, 52.66)` (was 69.85, 69.85, 50.69): hull above the emitter 76.7 cm (74.1),
  hang 30.6 cm (29.2; the record's 50 cm still decides). The kicks and onsets do not widen it (the tilt is clamped).
- **In its dock's corridor it flies level** (docked, launching, docking: no command, no kicks, its look settles) and its
  steps there are swept with the bare hull (`StepHalf`, `bay_half=(64.66, 64.66, 42.20)`). With the envelope hull the
  C2's own dock could no longer let its two drones out (`CARGO_DRONE_BAY_BLOCKED drone=5/6 … SCVehicle_2/Mesh_3563`), and
  the transfer ran on four drones.
- **Physics on the body.** The tug, the stations and the formation work with `BodyEmitter()` (the reference emitter on the
  level actor turned by the heading); the beam is drawn from `Emitter()` (the camera bone as it renders), so the wobble
  moves no box. The RPC's `pitch` / `roll` are the look's.

**Config** (`drone_art.json` `motion`, documented in its `_comment`; Astra's values): `freq_min_hz` 0.7, `freq_max_hz` 1.7,
`damp_min` 0.18, `damp_max` 0.3, `tilt_gain` 0.08, `gain_spread` 0.5, `tilt_lead` 3, `tilt_lead_carrying` 1.5,
`max_tilt_deg` 9, `kick_rate_hz` 1, `kick_deg_s` 26, `hunt_cm` 1.5, `hunt_hz` 0.35, `drift_cm` 2, `yaw_jitter_deg` 2.5,
`onset_min_s` 0.04, `onset_max_s` 0.26, `onset_jitter_s` 0.12, `event_deg` 0.6, `event_min_s` 0.35, `event_kick_scale`
0.6, `kick_log_sigma` 0.45, `scatter_deg` 30, `off_axis_share` 0.15, `opposite_share` 0.07;
`art.actor_lean_scale` 1 (was 0.25), `art.clip_tilt_scale` 0.2.

**Astra** (`out/k4b/astra/r1.md` … `r3.md`; clips `out/k4b/clips/<tag>/<tag>-transfer.mp4` with `rec.txt`, sheets
`out/k4b/sheets/`; BEFORE is the K4a look emulated with `-SCDroneArt=out/k4b/drone_art_before.json`):
- **r1** (gain 0.2, spread 0.25, lead 1.5, kicks 16 deg/s, clips 0.5): qualified pass, "livelier, but the pair still reads
  as moving together". Measured: the drones' bodies, tied to the box, accelerate sideways by up to ~10 m/s² together, so
  the shared lean dominated. Taken: gain 0.08, spread 0.5, clip tilt 0.2, kicks 20; and lead 3.
- **r2:** qualified pass, "individuality still secondary; the single drone becomes steady too abruptly". Taken: lead 1.5
  while carrying, kicks 26, damping 0.18–0.30. Found in the data: an arrived drone waiting for its teammate was not posed,
  so its attitude froze mid-swing; it now settles in Tick.
- **r3:** **pass**: "subsequent poses diverge enough to suggest two drones making separate corrections"; the stop "has a
  convincing diminishing rock-back: roughly +8.8° → −2.8° → +1.3° → level"; no further changes. Its remaining note: the
  team's opening lean still reads as shared, which the owner then asked for (the onsets and biased kicks above).
- **r4** (the team's opening, `out/k4b/sheets/r4-*`): **pass**: "the opening responses differ in timing and strength, and
  subsequent corrections separate clearly … a common physical demand with individual off-axis corrections"; no changes.

**Verification:**
- UBT `SCViewerEditor Win64 Development`: `Result: Succeeded`.
- `Automation RunTests SCBridge.Cargo`: `TESTS success=62 fail=0` (55 + `TugEnds`, `TugEndsSwap`, `DroneAttitude`,
  `DroneIndividual`, `DroneAttitudeBounds`, `DroneOnset`, `DroneKickDraw`; `DroneArtParse` extended). The two `Condition failed` lines are the engine's
  own UnifiedErrorTest.

**Open:**
- Carrying drones still meet structure briefly, held over, never under the C2's hold ceiling: the elevator's overhead
  structure (`Instanced_449/450`) while a box turns at the top of its lift (as before, 3 lines in k4b-c2r5), and the C2's
  underside on one 141 m route round the ship (beams paid out to 3 m, then a climb; 3 lines). Each a single
  `CARGO_DRONE_BLOCKED` line, no overlap.
- Held from the ends, a beam's force turns up to 15° off its line under the route's acceleration (its anchor is set for
  its share; the station stays fixed in the box's frame).
- A swap needs its whole room at one point; a swap point is not searched along the route (only at the lift's top, the
  entrance turn, and over the destination lattice at the goal's height).
- The fleet run (k4b-fr5) took 200 s (r3f2 56 s, k4b-fr4 64 s): the second load, its drone held short 7 s by the new way
  check, reached its slot after the first box was placed beside it, replanned its approach (`turn_room`) and then
  `no_station` ten times before it went down. Clean (`overlaps=0`), but not examined further.
- Stations of a later team still on its way out are kept for a load planned first (move 1 waited ~23 s in k4b-c2r5);
  that team holds short of the load's way anyway, so this space could be left out as for a team attaching.
- The drones' bodies zig-zag laterally by up to ~10 m/s² while carrying (paying beams in and out under ceilings; the box's
  sway). Before, the small lean hid it; it is the shared part of the team's lean now (tilt_gain 0.08 keeps it to a few
  degrees). Not examined further.

### K4 round 5 (2026-11-17): held in front, a grip each drone searches for, its wobble on the box

Same branch. The owner's five asks: the fleet slowdown, the carrying drones' synchronised zig-zag, holding the box in
front (push, or pull in reverse) rather than over it, the drones' wobble passed on to the box (damped), and, refining the
third: "not rules ... a soft switch and a preference ... a drone should re pick where it wants to be relative to the load
every so often ... choose only from viable positions when space is scarce."

#### The hold: a tractor beam holds to a point (`FSCCargoEndGrip`, `Cargo/SCCargoTug.h`)

The record (`SWeaponActionFireTractorBeamParams`, the multitool) gives a force (`maxForce` 257.5 kN, `minForce`),
distances (`minDistance` 0.5 m, `fullStrengthDistance` 3 m, `maxDistance` 150 m), a cone (`maxAngle` 40°, now parsed:
`FSCCargoDroneDefinition::MaxAngleDeg`, `CARGO_TRACTOR … max_angle`), volume rules and its own movement / rotation limits
(the held object's: `maxAcceleration` 20 m/s², `maxAngularVelocity` 2.5 rad/s). It has **no damping field and no hold
distance**: the hold distance is where the drone puts it (between the two distances); the damping stays the spring's
critical damping (`FSCCargoBeam`, as before).

- **Force.** Each beam gives an equal share of what the route asks of the team (weight, the route's acceleration, the
  body's drag), in any direction, applied to the box **as a whole** (`AddForce`; it was at each attach point). The box's
  rotation is the beams' angular hold (the critically damped attitude spring, unchanged). So where a drone holds from
  no longer shapes the force: a drone can hold from in front, beside or over its box.
- **Geometry** (pure, `Plan`): in front of an end face, the hull clear of it at any heading (hull half + the emitter's
  plan offset + 2 cm), its top at the box's top, the hold level (or down onto the face for a box lower than the hull),
  within the record's cone and distances. The 15° force-turn limit (`EndsMaxTurnDeg`) and the pull-out tension are gone.
- **Team.** One drone at one end (a lone drone: the end that **trails** its route, so it pushes the box ahead; where the
  route doubles back it pulls it in reverse; documented preference, `PlanMove`'s `EndOf`), a pair at both ends; the
  volume rule and the team sizing are unchanged; larger teams hold over (no scenario sizes one). The acceleration limit
  is each share within the record's force (`AccelLimit`: N·maxForce/m − g, any direction).
- **Zero g** (the fleet): valid, at the same places (round 4 had no ends grip there, and the over grip's station followed
  its share's direction, which in zero g fell back to "the side it holds").

**Heading** (`FSCCargoHeading::Target`, `HoldYaw`): a drone holding its box in front or beside it faces the box (its hold
direction) whichever way it flies: pushing, pulling in reverse, strafing; held over (the hold straight down) it faces the
load's travel; empty, its own travel; its bay's yaw first in its dock's corridor. The clips follow its velocity in its
own frame (backwards when it pulls in reverse). The beam starts at the emitter (camera bone) and runs along its own line
to the hold point (`SetBeamFx` aims the effect along `Point − From`, whichever way the drone faces).

#### The synchronised zig-zag (owner: "~10 m/s² together")

**Cause:** each drone's station lay along its share's direction at the beam length it paid out (`FSCCargoTug::Station`,
`BeamRoom`, `RoomAhead`): the share swung with the route's acceleration (finite-differenced each tick) and the beams paid
in and out under ceilings, with a lever of up to 3 m, the same for every teammate, so the team's stations, and the
drones on them, zig-zagged together. **Fix at the source:** every station is now geometric, fixed in the box's frame on
its route (`GripNow`): nothing of the share, the box's sway or the room along a beam moves it. `BeamRoom`, the room
cache and the tuck are gone; the route's shape covers the planned places.

#### Where a drone holds from: a grip it searches for (`FSCCargoGripPick`, `SCCargoGrip.cpp`, drone_art.json `grip`)

- **Planned place** (inside the route's shape: the fallback a drone returns to): over its formation offset at the hang,
  or its end of the ends grip. Planned in turn (`TierOrder`, `CARGO_GRIP_TIER`): over with room, then the ends, then
  over as tight as it fits; a hold (dug out and put back) and teams of three or more over only. *Tried first and
  dropped:* the ends first. Every route ends over a lattice where boxes stand flush beside the slot, and a drone beyond a
  box's end stands where its neighbour is or goes: runs k5-c2a/b (the C2: a pair's end drone squeezed between its box
  and the box set down beside its slot, a swap back over taking 140 s, 2 to 7 overlaps) and k5-fleet-h..l (Hull A: the
  lone drone's end station beside a box at rest, 1 to 2 overlaps). So the preference is the drones', in flight, where the
  ends fit (below). The ends' route shape also keeps out of slots reserved for boxes still to come (`EndsQuery`), and
  its drones keep a quarter of a drone's clearance from what stands even in the precise zone (`EndsDroneMarginCm`, 10 cm;
  it was 5).
- **Candidates** (`Candidates`): in front of either end, beside either side, over the top; spread across each face
  (`spread` × its half width), out by up to `extra_reach` more than the hull's clearance (within full strength), up to
  `extra_height_cm` higher; every one clear of the box, within the cone (ends and sides) and the record's distances.
- **Pick** (`Pick`): weights by kind (`end_weight` 0.75, `side_weight` 0.1, `over_weight` 0.15; renormalised over the
  kinds that are viable, so about 3 of 4 picks are an end when all are), a teammate's face × `same_face_weight` (0.15:
  the team balances), never within the drones' own keep distance (`FSCCargoDroneSteer::Keep` and the emitter's reach) of
  a teammate's place, its next place or its planned place (it goes back there at any time), nor on a way that crosses
  one. (With a shorter distance two drones of a pair a metre apart swung each other round at 20 m/s: clip k5-pair.)
- **Re-pick**: every `interval_min_s` .. `interval_max_s` (3–7 s), each drone its own draw from its own stream, the
  first one a share of an interval (the team staggered); one drone of a team on its way at a time. It moves there round
  the box (`WayRound`: up to the higher place, over the top only where the straight way across passes the box) at
  `move_speed_scale` (0.35) of its top speed, **holding all the while** (its hold slides over the box's nearest face; it
  keeps off the face it is beyond).
- **Viable only** (`SpotFits`, `CARGO_GRIP_PICK`): its hull there, on its way there and on its way back to its planned
  place, swept along the route `lookahead_s` (3 s) ahead at the load's possible speed, against the world, the boxes at rest
  and other teams' kept space, with the load's margin; clear of every other load in the air and everywhere its drones
  could be (`OtherLoadNear`, `Reach`); back home in time (within the lookahead less one check). Checked again every 0.5 s
  (`GripCheckSeconds`); a place that stops fitting sends it back to its planned place. No search (all back to their
  planned places) in the precise zone, when the route piece ends within the lookahead (a turn, a swap, the descent), off
  the route (stalled, swapping, turning), or with the box hanging on its side.
- **Route shape:** chosen: the route is planned with the planned places only; the re-picks are restricted to what fits
  (above). (The alternative, planning every route with the whole reach, would have left the C2 with no way in.)

#### The wobble on the box (owner: "The wobble should transfer to the load, but damped")

Each drone's hold point (the anchor its spring pulls the box to) turns with its attitude about its body emitter and moves
with its drift, by `motion.load_transfer` (0.35, a documented viewer choice: alone the hold is a stiff, critically damped
spring near the wobble's band and would pass the wobble on whole) × `TransferFade` (the gusts' fade: none in the precise
zone, none at the route's ends) (`ASCCargoDrone::WobbledHold`). The box's spring and mass damp it (a critically damped
second-order follower passes on no more than its input, less above its frequency); teammates' wobbles are their own, so
a pair's average. Safety: the route's margin for a load (`LoadEnvelopeCm`) adds the worst case, load_transfer × (the
longest hold × sin(max_tilt_deg) + drift + hunt) over the gusts' envelope: CARGO_GRIP `envelope_cm=18.0` for the C2's
boxes (8.0 before).

#### Other fixes

- **Fleet slowdown** (k4b-fr5 200 s): its second load, at rest in zero g under the plate beside the first, had its
  station recomputed from the share's direction (none in zero g: "the side it holds") each time its approach was replanned
  (`turn_room`), found no room for it (`no_station`) ten times. Geometric stations end it (regression test:
  `TugEnds` "zero g … at the very places"). And a load no longer keeps space for a team planned after it still on its way
  out (as for one attaching): that team holds short of the load's way (`KeptSpace`).
- **Drones lagged their stations by up to 1.3 m** whenever the route accelerated (`CARGO_TUG_OFF`; the fleet's lone
  drone circled its end station): the drone's body (`FSCCargoDroneBody`) followed its station on a critically damped
  spring at W = jerk / acceleration = 2.5 /s with no acceleration fed forward, so a station riding a route at a lagged by
  a / W² (1.2 m at 7.8 m/s²), every teammate the same way at the same time: the other half of the synchronised zig-zag.
  The route's acceleration is now fed forward (`FStation::HoverA`; test `TugFeedForward`: within 3 cm, 0.64 m without),
  and so are the station's own velocity in a turn (the box turning at a hold's entrance: `TugRotation`) and on a drone's
  way round the box (its way's speed and acceleration; the way waits at rest for a drone held up: fed forward at speed
  while paused, one drone drifted 7 m off and its move timed out, k5-c2-n).
  A drone facing its box took its heading from where it was; its emitter is off its centre, so the heading moved it and
  that the heading again: it takes it from its station now. Before a swap of grip it first flies back onto the station
  it holds from (`SwapGrip`).
- A lone drone given its next move where it set its last box down (the C2's hold), with none of its way behind it clear
  of a load still to come in, used to hold where it was, in that load's way: it now flies aside to a place near it clear
  of the loads' ways and waits there (`HoldShort`'s refuge, `CARGO_HOLD_SHORT … aside to`) until its way is clear.
  `HoldShort` tests its way every metre, not only at its corners (a drone leaving the hold met a load coming in between
  two corners 50 m apart, k5-c2-o). A drone that plans its way out again from where it planned last and is still no
  nearer (pressed against the C2's ramp, its route found free, its every step blocked) goes back along its trail a
  couple of metres first, every other time.
- How a box held from its ends finishes (swap back over it, or the ends down onto the slot) is worked out once per
  situation (`EndsFinishes`): planning had swept the slot approaches every tick while a search ran (0.3 s a tick).
- `CARGO_SLOW` (a move's tick over 30 ms, its grip search and tug apart) and `CARGO_SLOW_DRONE` (a drone's tick).

**Config** (drone_art.json, documented in its `_comment`): `motion.load_transfer` 0.35; `grip` block: `end_weight` 0.75,
`side_weight` 0.1, `over_weight` 0.15, `same_face_weight` 0.15, `spread` 0.6, `extra_reach` 0.5, `extra_height_cm` 30,
`interval_min_s` 3, `interval_max_s` 7, `lookahead_s` 3, `move_speed_scale` 0.35 (viewer choices).

**Verification:**
- UBT `SCViewerEditor Win64 Development`: `Result: Succeeded`.
- `Automation RunTests SCBridge.Cargo`: `TESTS success=64 fail=0` (round 4's 62, `TugEnds` and `TugEndsSwap` rewritten for
  the hold to a point, the zero-g regression and the ways round the box; new: `GripPlaces` (places round a box, the
  weights about 3 of 4 at the ends, teammates, the interval), `TugFeedForward`; `DroneHeading` (facing the box held:
  push, pull in reverse, strafe, its bay first) and `DroneArtParse` (the grip block) extended). The two
  `Condition failed` lines are the engine's own.

**Runs** (logs `viewer/Saved/Logs/<tag>.log`; outputs `out/k4b/<tag>.out`; the final build is `t`):

| Run | Scenario | Job line | Notes |
|---|---|---|---|
| k4b-c2r5 (round 4) | c2_hangar 208 SCU elevator → C2 | `t=340.5 moves=9 done=9 overlaps=0` | the baseline |
| k5-c2-t2 | the same | `t=338.0 moves=9 done=9 overlaps=0` | 24 picks: 19 ends, 3 sides, 2 over; 6 refuges |
| k5-c2-t3 | the same | `t=337.8 moves=9 done=9 overlaps=0` | 24 picks: 18 ends, 3 sides, 3 over |
| k5-c2-t | the same | `t=477.8 moves=9 done=6 overlaps=2` | **failed**: see Open |
| k5-hulla-t | hull_a_hangar 16 SCU, elevator → Hull A | `t=69.3 moves=1 done=1 overlaps=0` | round 4 `t=54.0`: its route 99.7 m (90.5), half of it capped near 3 m/s |
| k5-fleet-t | hull_a_space_fleet 16 SCU (2 boxes) | `t=49.8 moves=2 done=2 overlaps=0` | round 4 `t=200.4`; round 3 56.4 |

The same code before the last two fixes (refuge waits, trail back) ran the C2 in 348.7 s and 327.3 s (9/9, 0) and once
failed (`k5-c2-r`, a drone stuck on the ramp); fleet and hull_a were clean in every run of the round from build `m` on.

**Where the time goes** (`out/k4b/k5-c2-t2-times.txt`, per move; sums over moves that overlap in time):

| | Outbound | Transit | Lifting | Lowering | Placing |
|---|---|---|---|---|---|
| k4b-c2r5 (round 4) | 250.6 | 368.3 | 19.0 | 18.1 | 5.4 |
| k5-c2-t2 | 371.6 | 312.8 | 18.8 | 17.8 | 5.4 |

Transit is 56 s shorter (no beams paid in and out, no room checks; the C2's last box planned in 4 s, not 47). Outbound
is 121 s longer: drones given their next move in the C2's hold wait there, aside, for the loads still to come in
(43 `CARGO_HOLD_SHORT`, 6 refuges; moves 4-7 outbound 56-70 s each). The job's critical path is move 9: free at 263.6 s,
19.8 s out, placed at 328.6 s. Fleet (`k5-fleet-t`): 18.4 s before any move (the hangar's grids opening), outbound
11.5 s, transit 17.1 s (round 4: 160.7 s in move 2 alone, 32 replans), lowering 7.4 s.

**The zig-zag, measured** (each drone's horizontal acceleration from its velocity, `rec.txt` of the clips, 0.09 s samples;
route acceleration included):

| Clip | RMS | p95 | max | reversals > 2 m/s² |
|---|---|---|---|---|
| b-after6 (before: round 4, a pair, 32 SCU) | 7.5 m/s² | 13.6 | 26.4 | 0.28 /s |
| k5-pair2 (a pair, 32 SCU) | 6.6 / 6.0 | 11.8 / 11.8 | 34.9 / 23.1 | 0.12 / 0.39 /s |
| k5-single2 (a lone drone, 16 SCU) | 6.2 | 12.4 | 25.7 | 0.15 /s |

The pair's accelerations still correlate (0.61-0.68: the same route), but the off-station spikes are gone with the feed
forward. `CARGO_TUG_OFF` (a drone over 40 cm off its station, sampled once a second) still logs 185 times in a C2 run:
see Open.

**Clips and Astra** (`out/k4b/clips/k5-pair2`, `k5-single2`; sheets `out/k4b/sheets/r5-*`; review
`out/k4b/astra/r5.md`): at the ends the pair's rear drone flies forwards (79 of 141 samples) and its front drone in
reverse (47 of 137); the lone drone held from its rear end, pushing (46 forwards, 33 strafing, 9 reverse). Astra:
a) holding from the ends reads as pushing and pulling: **qualified pass** (the pulling drone's facing is hard to read
against the beam glow); b) the grip changes read as changing grip, not jitter: **qualified pass** (give each arrival a
clearer ease into a settled hold); c) the end preference: **pass**; d) the box following the wobble: **qualified pass**
("controlled, does not visibly shake", not distinguishable from camera motion at this framing); e) nothing wrong (no box
penetration, no bunching): **qualified pass** (smaller contact blooms would show the grip).

**Open:**
- One C2 run in three still fails (`k5-c2-t`): a drone holding short in the hold at its next move's start was judged
  clear of a load coming in (`HoldShort`'s legs) and the load met it (`CARGO_DRONE_OVERLAP … state=2`), then three
  moves timed out. Likely: the legs a lone drone waits for leave out a load's way down onto its slot and the drones of a
  load held from its ends during its swap, and a drone standing in the hold is no obstacle to the load's guard.
- Drones still sit up to 1-2 m off their stations at times (`CARGO_TUG_OFF`); not from the route's acceleration any more
  (fed forward) and not from teammates (kept apart); its cause is not found yet. The hold is to a point, so the force on
  the box does not change; the route's margin covers the stations, not such a lag.
- Astra's suggestions (the drone's facing and beam contact easier to see; an ease into each new grip; a fixed-camera
  close-up to judge the transferred wobble) are not done.
- hull_a is 15 s slower (its route).

### K4 round 5, reliability and the re-pick cap (2026-11-17)

The owner: "Fix it first" (the C2 failing about one run in three), and, after the clips: "when drones pick new positions
aside from initial, limit max distance to new position, so they don't shift too far ... the overall pace of picking new
positions and the wobbling is already looking very good indeed."

**The cause.** Every failing C2 run before this (`k5-c2-q`, `q2`, `r`, `s`, `t`) began the same way: a lone drone that had
just set its box down in the C2's hold got its next move there, and the next load came into the hold while it was still
in it. Inside the hold, beside boxes standing flush, it had no room: the load's reach (its box and its drones anywhere
round it) and the distance drones keep cover the hold's width, so its refuge search found nothing and it held where it
was. The load came on. It hit the drone (`CARGO_DRONE_OVERLAP … state=2`), or the drone's separation pushed one of the
load's carrying drones off its station into the hold's structure (`Mesh_4099`, its ceiling), more than 3 m off. The load
then waited for that drone for good (`Transit took longer than 180 s`, its later moves timing out). The race is timing:
which move frees which drone where, frame by frame; the same code passed and failed. Two gaps let it happen:
- `HoldShort` modelled an incoming load as its box and its planned grip only: not its drones' re-picked places, not its
  swap back over, not its descent onto the slot.
- Nothing kept a load out of a hold that drones were still in: the guard saw a team's drones while they were placing, but
  not once they turned for home or took their next move there.

It became frequent in round 5 (the C2's moves 5 and 9 now planned from the ends, drones off their stations pushing each
other, outbound waits in the hold), but the gap itself dates from round 4 (k4b-c2r4's overlap was a drone in the ramp).

**The fix** (one rule from each side, no cycle):
- **The hold is entered clear.** A load waits short of its destination's precise zone (25 m, `ZoneGateCm`: outside the
  C2's ramp) while a drone of another move that holds no load in the air is in the zone or in the load's way into it
  (`ZoneOccupant`, `InZoneOf`; `CARGO_YIELD … waits short of its hold`, `goes on into its hold`).
- **Those drones do not wait for it.** `HoldShort` does not hold a drone for a load waiting at its gate, nor for a load
  still outside its hold while the drone is in that hold or the way into it. The load waits for the drone and the drone
  goes on out (`FSCCargoTug::DroneWaitsForLoad`; test `WaitClear`: in no case do both wait).
- **Everywhere else, a waiting drone keeps clear of the load's whole remaining motion**: its box and its drones anywhere
  round it (`FSCCargoKeepOut::LegShape` over `FSCCargoGripPick::Reach`) on every piece, and its descent onto the slot. It
  goes aside to a refuge when its own place lies in that motion, and the way to the refuge must also leave the load's way.
- A load also keeps where drones flying home are, and where a later team's drones holding short are (`KeptSpace`).

The coordinator suggested keeping only "waiting drones go outside" and dropping the gate. Inside the hold there is no
outside a drone can wait in, so the drone has to leave, and the load has to wait until it has: the gate is that rule seen
from the load's side. It costs 24 to 74 s of waiting per run (5 to 8 waits, at most 30 s each).

**Reliability** (the final code; logs `viewer/Saved/Logs/<tag>.log`; times `out/k4b/<tag>-times.txt`):

| Run | Job line | Gate waits | Refuges |
|---|---|---|---|
| k6-c2-a1 | `t=278.1 moves=9 done=9 overlaps=0` | 6 (43.8 s) | 3 |
| k6-c2-a2 | `t=349.2 moves=9 done=9 overlaps=0` | 5 (74.4 s) | 2 |
| k6-c2-a3 | `t=319.1 moves=9 done=9 overlaps=0` | 6 (28.5 s) | 2 |
| k6-c2-a4 | `t=265.1 moves=9 done=9 overlaps=0` | 8 (24.2 s) | 2 |
| k6-c2-a5 | `t=269.3 moves=9 done=9 overlaps=0` | 6 (33.4 s) | 3 |
| k6-fleet-a | `t=60.2 moves=2 done=2 overlaps=0` | | |
| k6-hulla-a | `t=70.0 moves=1 done=1 overlaps=0` | | |

Five in five, with no `CARGO_DRONE_OVERLAP` line in any run. Outbound summed over moves fell to 119-207 s (`k5-c2-t2`:
372 s) and transit rose to 339-491 s (313 s): the time moved from drones waiting in the hold to loads waiting short of it.
Tests: `TESTS success=65 fail=0` (`WaitClear` new; `GripPlaces` and `DroneArtParse` extended). These runs used a build
from before the wait-for rule became one pure function (`DroneWaitsForLoad`, the same conditions); that later build also
passed the tests, 65/65.

Tried and dropped in this pass:
- A carrying drone that held its station against lone drones (`k6-c2-u1`): lone drones then overlapped the carrying ones.
- A lone drone going on when it had no refuge (`k6-c2-x1`): one flew into the ramp as a load came up.

**The station lag** (`CARGO_TUG_OFF`: a drone over 40 cm off its station, sampled once a second; 145-210 per run): two
thirds come within 6 s of a grip move (by design, the drone's way round the box waits for the drone within its hull's
radius), the rest at the route's sharp turns. It did not cause the failures: their 2-5 m offsets came from waiting drones
pushing carrying ones, which no longer happens.

**The re-pick cap** (`grip.max_repick_move_frac` 0.3 of the box's longest side, `max_repick_move_min_cm` 150,
`max_repick_move_max_cm` 300; `FSCCargoGripPick::MoveCap`, measured along the way round the box, `WayLength`):
- A drone's first grip is its pick from its planned place over the box, of anywhere round it, and is not capped.
- After that, a re-pick only considers places within the cap; with none viable, the drone keeps its grip.
- When its own place stops fitting, it goes to the nearest viable place however far (its planned place at the latest).
- The cadence and the wobble are unchanged. `CARGO_GRIP_PICK` logs `move_cm` and `cap_cm` (-1 for the first grip).

| Grip moves (cm, along the way) | n | median | 90 % | max |
|---|---|---|---|---|
| re-picks, cap lifted (`k6-c2-nocap`) | 7 | 421 | 748 | 748 |
| re-picks, capped (`k6-c2-a1`..`a5`) | 48 | 69 | 116 | 158 |
| first grips (`a1`..`a5`) | 61 | 417 | | |
| moves when a place stops fitting (`a1`..`a5`) | 47 | 428 | | |

Astra (`out/k4b/astra/r6.md`, sheets `out/k4b/sheets/r6-*`):
- The capped re-picks read as small grip adjustments: **pass** ("the drones stay anchored to their respective ends").
- The uncapped moves: **qualified pass**. The first grips read as deployment; the returns over the box near the route's
  end "still risk reading as large relocations". Astra suggests staggering them, one drone keeping its grip while the
  other moves.

**Open:**
- The returns when a place stops fitting are 4-5 m (median 428 cm): mostly back over the box near the route's end, and
  both drones of a pair at once. **Staggered since (owner, 2026-11-17):** before the route's piece ends or a precise
  zone begins, the drones go back to their planned places one at a time (a teammate on its way round, the others keep
  their grip), and the team looks ahead `LookaheadS` once for each drone still away (`FSCCargoGripPick::ReturnAheadS`,
  test in `SCCargoTugTests`), so the last is back in time. Calm at once (no route to search on, no places, the box on
  its side) still sends every drone back together. Verified by build and the unit tests only (the owner asked not to
  run the scenario suite); the C2/fleet/Hull A runs above predate it.
- The cap-lifted run (`k6-c2-nocap`, not part of the bar) had one overlap.
- A drone's first grip of each route piece is uncapped, following the owner's wording ("aside from initial").

## Verification

- `SCBridge.Cargo` stays green, with new tests for the heading policy and the multi-cell dock footprint.
- The cargo routes and explorations end with `overlaps=0` and no new anomalies.
- Screenshots and a video of a team flying, attaching, carrying and docking: with the Fab drone, and without it (the
  stand-in).
- Done for K4a, the K4b code and the K4c runtime on 2026-11-16: see "Verification (2026-11-16)" above.
