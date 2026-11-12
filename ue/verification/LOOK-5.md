# LOOK-5 / LOOK-6 — the game hangar as the adjudication stage (2026-11-12)

The viewer presents Star Citizen's own hangar (`hightech_a/hangar_lrgtop_001`) around the ship. This round made that
stage trustworthy enough to judge the look in it, then prepared the next look experiments.

## Fixes (each verified with capture_route runs; logs in `ue/SCViewer/Saved/Viewer/`)

| Problem | Cause | Rule now |
|---|---|---|
| Hangar floor, elevator and doors misplaced (Mesh_105) | Object-container root entities are labelled `gltf_y_up` but their matrices are in the parent's CryEngine frame | Children placed on their parent entity without a helper use the CryEngine basis (`FSCMeshImporter::InCryFrame`) |
| Floor shown open, holographic ring visible | Bind pose; VFX-only placements imported | Presented state = door fragments' Open end pose (elevator raised flush); effect-only placements skipped |
| Crosshatch on the floor (real time only) | 15 hangar gobo materials + the ship's overflowed the 16-slot light-function atlas | `r.LightFunctionAtlas.Size=8` |
| Stripes on the floor and the player (real time only) | The shaders read a light's atlas index from 8 bits (`LightData.ush`, `& 0xFF`); 382 hangar + up to 184 ship gobo lights wrapped onto other lights' projections | `r.LightFunctionAtlas.MaxLightCount=255` (nearest lights keep gobos; farther ones a plain cone) |
| Ships hovering, gear in the "before touchdown" pose | Viewer convention lifted models 1 m; bind pose = gear extended | `ASCVehicle::RestOnLandingGear` snaps every `Landing_Gear` fragment tagged `Compress` to its last frame; the lowest **solid** point (posed collision triangles; non-shadow-casting parts such as glass, glow and light beams skipped) rests on the map floor (-5 cm) |
| Below-native rendering | Display-based default screen percentage | `r.ScreenPercentage=100` (manual mode), TSR |
| Camera inside a hangar pillar; under the floor after fly -> orbit | Orbit distance ignores the environment; fly->orbit pivot at a point under the floor | Orbit stops in front of environment surfaces facing the target; fly->orbit pivots on the surface in view |

## Tooling

- Route pseudo-command `waitpt [s]`: hold until the path tracer reaches `r.PathTracing.SamplesPerPixel` (256 spp: 8-30 s
  here, instead of fixed 240 s waits).
- UI: **Panels** toggle / `sc.UI.Panels 0|1` (scripted runs start hidden); loading bars for the model and the hangar
  over the 3D view.
- Look switches (`SCLook.cpp`): `sc.Look.Camera <x y z yaw pitch>` (now turns the fly camera too),
  `sc.Look.WallLights <scale>` (environment spot/rect lights aimed within 30 degrees of horizontal),
  `sc.Look.Gobo <texture part> <0|1>`, `sc.Look.Display game|ue`, `sc.Look.BloomStar <points> <length> [intensity]`
  (FFT bloom with a generated diffraction kernel; 0.15-0.3 reads as clean glints), `sc.Look.Focus <f-stop> [sensor mm]`
  (diaphragm DOF focused at the screen centre).

## Findings for the next iteration

- GPT Astra on the game hangar (orbit/wide, three ships, player): keep it; it suits the path-traced target; fix real-time
  artefacts before rebalancing its lighting.
- Native resolution costs: orbit views ~15 ms GPU; wide views are render-thread bound (25-60 ms, ~3900 components):
  instancing the hangar's repeated meshes is the performance item.
- Diaphragm DOF cannot blur a ship 25-70 m away at the viewer's ~90 degree FOV (beyond the hyperfocal distance) without
  a multi-metre virtual sensor. Real bokeh needs the engine's Accumulation DOF plugin (aperture-sampled; Movie Render
  Queue and an editor-only viewport preview in 5.9), to be ported to the game viewport.

## GPT Astra verdict (iteration 6) and what shipped

- Keep LumenDiffuseColorBoost 1.15 (1.0 reduces fill without closing RT/PT gaps) and the hangar's wall fixtures as
  authored (x0.8 barely changed separation; "near-horizontal" does not identify wall washes reliably).
- Keep the game's display curve: UE filmic crushed the undersides and left the player glaring white.
- Star bloom default 0.05 (`USCViewerSettings::StarBloom`, applied at startup; 0 = standard bloom); 0.15 for stills.
- Depth of field off by default; opt-in for stills (`sc.Look.Focus`), 2000 mm f/1.4 the upper limit before the ship
  reads as a miniature.
- Next: hangar instancing (render thread), a narrower-FOV hero framing preset, the Scorpius canopy RT/PT mismatch.
