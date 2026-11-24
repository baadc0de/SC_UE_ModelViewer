# Cargo art — WP-K1d

Original procedural stand-ins for the cargo vertical slice. The geometry, palette and dimensions are authored for this task, not copied game constants. All saved assets are under `/Game/Cargo` (the game's content; the shared plugin's `/SCBridge/Cargo` until WP-O1-4c-3c). No extracted mesh, texture, font or game material is incorporated into these assets.

## Reference study

Studied LIVE 4.10.2 using a private copy of the existing debug StarBreaker binary in `out/k1d/bin`, with automatic archive discovery. Extracts, GLBs, node dumps, measured bounds and a neutral shaded contact sheet remain in `out/k1d/study/`; the viewed contact sheet is `out/k1d/preview_study.png`. Its colours distinguish material IDs and are **not** extracted paint colours. Each object is framed independently, not displayed at equal scale.

| Reference | Measured envelope (metres) | Geometry and useful cues |
|---|---|---|
| `Data/Objects/Spaceships/Ships/AEGS/Reclaimer/Exterior/AEGS_Reclaimer_Drone.cga` + `.cgam` | Authored model bounds X/Y/Z: **1.361 / 2.394 / 2.319**. Reconstructed rest geometry: approximately **1.361 / 2.357 / 2.294** in source axes. | 61 NMC nodes, 149 submeshes, 92,934 triangles. Large circular centre spinner, four articulated arm/joint chains, antennae, side cylinders and service rails. Clearly not a one-metre cargo quadrotor. Borrow the protected mechanism and layered service-access language, not its silhouette or dimensions. |
| `Data/Objects/buildingsets/human/lowtech/prop/vehicle/mining/drone/mining_drone_body_001_a.cga` + `.cgfm` | Rest export width / depth / height: **4.018 / 2.348 / 1.366** | 20 source NMC nodes; 14 material primitives, 75,806 triangles. Low rectangular chassis, chamfered nose, raised perimeter rails, broad inset covers, piston/carriage joints and separately named front/back lights. Much larger than the requested art. |
| `Data/Objects/Vehicles/Repair/ph_repairdrone.cgf` + `.cgfm` | Rest export width / depth / height: **0.659 / 0.659 / 0.394** | 17 material primitives, 14,436 triangles. Annular guard around a cross-shaped hub, layered concentric rim and protruding lower mechanism; node names include engine coils. Closest scale reference for guarded fans and a belly tool. |
| Hull A exterior, existing `out/cargo-datamine/hull_a/.../MISC_Hull_A_LOD0.glb` | Rest exterior width / length / height: **9.346 / 23.416 / 5.585**; excludes separately exported attachments | 303,226 triangles in this exterior mesh. Long rounded rectangular pods, stepped panel edges, broad chamfers, inset bands and exposed service structure. These informed the rounded/chamfered utility housings, vents, rubber bands and protected corners. |

Bounds were evaluated on **referenced triangle vertices after node transforms**, not by summing accessor counts (several primitives share a full vertex accessor). GLB Y-up is mapped to height in the table. The Reclaimer CLI skin exporter excludes `.cgam`; its raw files were extracted and inspected with `nmc dump`. The scratch study decoder follows upstream `ivo/skin.rs`, `dequant.rs` and `pipeline/nmc_bridge.rs`, including parent composition and index page bases. Its node matrices come from the rounded text dump, so reconstructed bounds are approximate; the authored MeshInfo box is recorded separately. This study decoder is not a production export path.

Reproduce the raw study with the copied binary:

```powershell
out/k1d/bin/starbreaker.exe p4k extract -o out/k1d/study --filter '**/AEGS_Reclaimer_Drone.*'
out/k1d/bin/starbreaker.exe skin export mining_drone_body_001_a out/k1d/study/mining.glb
out/k1d/bin/starbreaker.exe skin export ph_repairdrone.cgf out/k1d/study/repair.glb
# nmc dump accepts the extracted local .cga path.
```

The MISC-like yellow / graphite / silver palette is an original interpretation of the brief. The stand-in uses four visible annular ducts, chamfered armoured panels, narrow inset seams, a vented metal cover, fasteners, rubberised bumpers and geometric hazard ticks. No texture or normal-map dependency is needed.

## Mesh contract

Paths are `/Game/Cargo/Meshes/<name>.<name>`. Units are **cm**, scale **1**, X forward, Y starboard, Z up. The five body/dock/rotor slots, in order, are **Paint, Trim, Metal, Emitter, Lights**. Rotor and dock retain unused slots so indices remain stable.

| Mesh | Local bounds / pivot | Triangles |
|---|---|---:|
| `SM_CargoDrone` | Min `(-48.15,-49.15,-20)`, max `(48.15,49.15,15.6)`; **96.3 × 98.3 × 35.6 cm**. Origin is the authored mass-centre/controller reference, between the four fans. | 9,324 |
| `SM_CargoDroneRotor` | Hub origin; seven blades within radius 14.04 cm, Z approximately -2..2 cm; rotation axis +Z. Attach four instances to the body fan sockets with identity transform. | 220 |
| `SM_CargoDroneDock` | Min `(-62.5,-62.5,0)`, max `(62.5,62.5,125)`; exact **125 cm cube**, pivot bottom centre. | 4,800 |
| `SM_TractorBeam` | Open cylinder, radius **50**, Z **0..100**, pivot at emitting end. UV0 U around circumference, **V=0..1 along length**. | 1,536 |
| `SM_GridSlotFrame` | X/Y -50..50, Z 0..100, bottom-centre pivot. Twelve **1 cm** bars within the unit 100 cm cube. | 720 |

The dock is a stacked, **front-exit (+X)** two-bay rack. There are no doors. Lower pivot Z=30, upper Z=91; both face +X. Floor/deck/roof and corner castings stay inside the SCU cube. Each opening is at X=62.5, with exit socket X=122.5: **60 cm outside the opening**. A drone's aft extremity clears the dock by 11.85 cm at that exit point. Translate straight along socket +X before changing heading; reverse for docking. The underbody landing rails reach Z=10 and 71 when docked and meet the cradle rails. The tractor lens sits 1 cm above the dock contact plane.

These are visual meshes with **no generated collision**; flight/grid logic supplies clearance and occupancy. Do not generate a convex hull for the dock: it would close the bay openings. If physical dock collision is needed, use separate floor, roof, wall and post boxes. The authored origin is not a claim about a game-derived mass/inertia tensor.

## Socket table

Locations are local cm. Rotations are **Pitch, Yaw, Roll** in degrees. A socket's **+X is its pointing direction**; rotor spin uses its **+Z**. The script uses explicit named Rotator arguments.

| Mesh | Socket | XYZ | Pitch / Yaw / Roll |
|---|---|---|---|
| Drone | `Fan_0` | `28, -29, 2` | `0, 0, 0` |
| Drone | `Fan_1` | `28, 29, 2` | `0, 0, 0` |
| Drone | `Fan_2` | `-28, -29, 2` | `0, 0, 0` |
| Drone | `Fan_3` | `-28, 29, 2` | `0, 0, 0` |
| Drone | `Tractor` | `0, 0, -19` | `-90, 0, 0` (+X points down) |
| Drone | `Light_Port` | `23, -12.6, 7` | `0, -90, 0` |
| Drone | `Light_Starboard` | `23, 12.6, 7` | `0, 90, 0` |
| Drone | `Light_Status` | `-22, 0, 14` | `90, 0, 0` |
| Drone | `Dock` | `0, 0, -20` | `0, 0, 0` |
| Dock | `Bay_0` | `0, 0, 30` | `0, 0, 0` |
| Dock | `Bay_1` | `0, 0, 91` | `0, 0, 0` |
| Dock | `BayExit_0` | `122.5, 0, 30` | `0, 0, 0` |
| Dock | `BayExit_1` | `122.5, 0, 91` | `0, 0, 0` |
| Dock | `BayLight_0` | `62.5, -55, 30` | `0, 0, 0` |
| Dock | `BayLight_1` | `62.5, -55, 91` | `0, 0, 0` |

Attach the **drone origin**, not its `Dock` socket, at `Bay_n`. For the beam, orient its **local +Z** along the tractor socket's **+X** or directly from emitter to load; do not attach the cylinder with identity rotation and expect it to point down. Set XY scale to `desired_radius_cm / 50`, Z scale to `distance_cm / 100`. A 6 cm radius / 150 cm beam uses `(0.12,0.12,1.5)`. Rotor animation must use the socket location plus local yaw, not rotate the entire body.

The concurrent K1b `SCCargoDrone.cpp` was read for compatibility: its `/SCBridge/Cargo/Meshes/` soft paths, `Fan_n` attachments, `Tractor` location and +Z-to-load beam alignment match this contract. No runtime files were edited by the art worker.

Grid outlines use the mesh, not debug lines, so they can ship/cook. Set scale to `dimensions_cm / 100`, location at footprint centre on the grid's bottom plane. The bar width scales with the object; it is not a constant-pixel overlay. Grid corner origins must first be offset by half the footprint. Match these to the domain's measured grid origin rather than assuming a source convention.

## Material contract

Masters and matching `MI_*` instances live in `/Game/Cargo/Materials`. All graphs connect explicit **Substrate BSDFs to Front Material**. No legacy-only surface inputs are relied upon.

| Master | Parameters | Behaviour |
|---|---|---|
| `M_CargoPaint` | vector `BaseColor`; scalar `Roughness` | Painted metal appearance; original utility yellow defaults, roughness 0.38. |
| `M_CargoTrim` | vector `BaseColor`; scalar `Roughness` | Dark graphite rubberised trim, roughness 0.72. |
| `M_CargoMetal` | vector `BaseColor` (conductor F0), `DiffuseAlbedo`; scalar `Roughness` | Brushed silver hardware, roughness 0.28. |
| `M_CargoEmitter` | vector `EmissiveColor`; scalar `Intensity` | Opaque unlit emitter/nav lenses. Default cyan, intensity 5. |
| `M_TractorBeam` | vector `Color`; scalar `Intensity` | Additive, two-sided, depth-tested without depth writes. Twelve UV bands scroll along V; both ends fade over 10% of the normalized length. No world-position or object-size dependency. |
| `M_GridSlot` | vector `Color`; scalar `Opacity` | Flat unlit translucent lines or an independently supplied fill mesh. No time, Fresnel, scanning, flicker or extra glow. Substrate transmittance is `1-Opacity`; emitted colour is premultiplied by opacity. |

Colour values are **linear RGB**, not 8-bit sRGB. Runtime may create MIDs. The Lights slot is independently overrideable from Emitter; all built-in nav lenses share it. To distinguish port/starboard/status individually, attach small light components at the provided sockets (or use per-socket lens components); no asset-name-driven game-data lookup is implied. No actual point lights are baked into a static mesh.

## Rebuild and verification

`game/scripts/create_cargo_art.py` updates existing assets in place and replaces its named sockets. It scans its asset directory (`/Game/Cargo`) before reusing assets, so commandlet registry startup does not create duplicate packages. It never saves the preview level or modifies the project/engine configuration. Geometry Scripting and Python must be enabled; the current project already enables both.

Take the shared UE lock **before** either launch, retry an occupied lock at 60-second intervals for at most 45 minutes, and release it in `finally`. Do not remove another worker's lock. The launch flags below keep shader scratch in the workspace and use this repository's filesystem DDC:

```powershell
$editor = "$env:USERPROFILE/UnrealEngine-5.9-src/Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
$project = "$PWD/game/UnrealCitizen.uproject"
$script = "$PWD/game/scripts/create_cargo_art.py"
# Build-only/headless path; acquire the shared UE lock first.
& $editor $project -run=pythonscript "-script=$script" -unattended -nop4 -nosplash `
  -AllowCommandletRendering -EnablePlugins=GeometryScripting -ddc=SCViewerDDC `
  "-ShaderWorkingDir=$PWD/out/k1d/shaders"
# Warm scene captures: editor tick allows scene/render resources to settle.
& $editor $project "-ExecutePythonScript=$script" -CargoArtWarmPreview -RenderOffscreen -DisablePlugins=DLSS `
  -unattended -nop4 -nosplash -ddc=SCViewerDDC "-ShaderWorkingDir=$PWD/out/k1d/shaders"
```

The offscreen run automatically exits after warm captures. The commandlet path is also useful to check generation without an interactive editor, but the warm path is the reference for visual review. Preview images use a transient studio scene containing the drone, dock with two drones, active beam and a 125 cm reference cube:

- `out/k1d/preview_overview.png`
- `out/k1d/preview_drone.png`
- `out/k1d/preview_dock.png`
- `out/k1d/preview_beam.png`
- `out/k1d/preview_study.png` — game reference study, scratch only
- `out/k1d/preview_mobiglas.png` — browser capture of the HTML mock

`out/k1d/validation.json` records **saved UE** mesh bounds, input/render triangle counts and sockets. Coincident corners at lathe poles are collapsed before calculating normals. Small bevel normals are normalized explicitly without FVector's gameplay-sized safe-normal tolerance. Generation asserts the drone envelope/triangle budget, exact dock bounds, five slot indices, saved socket positions/rotations and clockwise winding against outward normals (GeometryCore's left-handed convention). The studio disables physical camera exposure and the DLSS plugin for reproducible offscreen rendering/shutdown. Logs are under `out/k1d/`; this is art-only verification, not a substitute for the cargo domain's automation, shipping cook, flight test or cross-model review.

Verified on Windows / UE 5.9 / D3D12 SM6 on 2026-11-14. The final offscreen rebuild updated all **17 existing assets** in place, exited **0**, and logged `K1D_ART_COMPLETE` followed by `K1D_PREVIEWS_COMPLETE` in `out/k1d/warm.log` (15:20 UTC). No Python assertion or material compilation errors occurred. Saved triangle counts equal generated counts: drone 9,324; rotor 220; dock 4,800; beam 1,536; frame 720. All 15 socket transforms passed. All four final studio captures were viewed; the final pass smooths circumference normals on lathed surfaces while retaining sharp profile bevels, and uses a neutral painted 125 cm reference cube. The HTML capture was also viewed at 1920×1080; its inline JavaScript parses and it has no external resource links.

## mobiGlas visual specification

Open `docs/cargo/mobiglas-mock.html` locally at **1920×1080**. It is self-contained and makes no network requests. CSS variables reproduce §7 of `research-starfield-ui.md`; all text uses Roboto/system sans fallback as requested. The four paper panels have 16 px padding, 24 px gutters, 1 px rules, square corners and one four-stripe motif each. Capacity/progress bars are 6 px; label/body floors are 13/16 px. Selected rows invert paper/ink. The apps rail and live world retain the dark scrim.

The top-down selected grid shows 2×4 cells and a single 16 SCU box spanning both height layers. Layer buttons work. The illustrative amount slider snaps to 0 or 16 SCU: one empty full grid remains after the active job's reservation, while the dock occupies a cell on the other grid. The 32 SCU box remains refused by length. Runtime must compute achievable ticks from the actual stock and reservations. Confirm/cancel demonstrate local feedback only and never issue game actions. World dots are 5 px; diagonal leaders are 45°, shelves horizontal, and labels sit beyond projected bounds plus 16 px. Selected/active labels remain outside the objects.

TODO(WP-K1): Integrate socket-driven rotors, lights, beam orientation and bay approach with the cargo actors; verify against the Hull A grid's measured local origin.
TODO(WP-K1): Validate cooked asset references and packaging with the K1 runtime integration; art itself has no FFI or external-content dependency.
TODO(WP-K1): Cross-review the final art/runtime contract with the other model before merge; no staging or commits performed by the art worker.
