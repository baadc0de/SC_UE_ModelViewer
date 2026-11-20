# WP-I1 — Door and lift panels: visible, faithful, operable

Branch `wp/I1-door-and-lift-panels`. Exporter changes (slices 2–3) go on a submodule branch `scviewer/panel-screens`.

**Owner decisions (2026-11-19), binding for this WP:**

- Door and lift operating panels (door pads, lift call buttons, floor-select panels, elevator call panels) are
  **inherited from the game, faithfully**: their look, their states and their behaviour.
- Game HUDs and MFDs (cockpit and dashboard screens) are **not** inherited. They will be replaced by our own designs,
  like the cargo grids and the mobiGlas Cargo app (WP-K1c). They are out of scope here (section 7 keeps only the
  facts a replacement needs).
- Lift motion and logic (carriage, transit manager, floors) belong to **WP-P5** (`docs/work-packages/WP-P5-lifts.md`).
  This WP owns the panels' look, content and press routing. The interface between the two is in section 5.

Evidence: LIVE 4.10.2. The C2 Hercules and `hangar_xltop_001` packages from the layer-1 cache (2026-11-19), and
DataCore queries with the prebuilt CLI. The findings log, extracted files and scripts' outputs are in
`out/screens-datamine/` (`NOTES.md`).

## 1. Diagnosis: why the panels are invisible

**Cause:** the panel's display face is a render-to-texture screen material. The importer draws every such material as an
additive emissive surface at strength 0, which is identical to its own "invisible" material. Of the possible causes,
this is (c) "the content is UI-only" made fatal by (b) "the importer draws it transparent". It is not (a): every panel
is exported with its mesh, entity id, links and even a rendered image of its default content.

### The data

Every panel face uses a `DisplayScreen` submaterial with a virtual `$RenderToTexture` slot (`TexSlot9`, role
`render_to_texture`, no image). In the game it is an **opaque** surface: the sidecar's `alpha_config` has opacity 1
and `forward_rendering` false. It has an authored `Glow` and a `BackColour`.

| Material (sidecar, submaterial) | Used by | Glow | BackColour |
|---|---|---|---|
| `Starlifter_Int.mtl` sub 8 `RTT_Screens` | all C2 touchpads, bridge and room buttons, lift buttons | 0.08 | 0.0176 grey |
| `screen_16x9_1_rtt_a.mtl` sub 1 `screen_rtt` | `screen_1x1_*`, `screen_9x16_*` wall screens | 0.04 | black |
| `template_int_a.mtl` sub 20 `screen` | hangar `console_elevator_1_panel_a` | 0.06 | black |

### The importer (code path responsible)

1. `SCMaterialDescribe.cpp:593` maps the families `DisplayScreen`, `UIPlane`, `Monitor`, `UIMesh` and `Hologram*` to
   `ESCMaster::EmissiveUnlit`.
2. `:622` makes that master `Translucent`.
3. `:700–705`: a screen without a `DiffuseMap` (every `$RenderToTexture` screen) gets `EmissiveStrength = 0`. The
   comment there says "Screens draw render-to-texture content bound per instance (ui_bindings, WP later); dark until
   then."
4. `ue/scripts/create_materials.py:735` builds `M_SC_Emissive_Unlit` as `BLEND_ADDITIVE`, with emission = colour ×
   `EmissiveStrength`. With a strength of 0 it adds nothing, so the surface is fully transparent.
5. `SCImportMaterials.cpp:128` `Invisible()` is the same description (EmissiveUnlit, Translucent, strength 0).
   `SCMaterialTests.cpp:499` pins the current behaviour.
6. The translucent split in `SCImportJob.cpp:1140–1163` moves screen slots into the non-Nanite variant. That is harmless
   on its own.

The importer reads no `ui_bindings`. `FSCPackageReader` has no field for them, so the PNGs the exporter wrote are
never loaded.

### Panel by panel

| Panel | Mesh (display face) | What the viewer shows |
|---|---|---|
| C2 corridor ring pads (5) | `Cor_Cyl_Touchpad_Ring_DoorFrame_A` is screen only (0.126 × 0.253 m, curved) | nothing |
| C2 wall pads (5) | `Top_Touchpad_Wall_DoorFrame_2m` and `Wall_Cap_Front` are screen only | nothing |
| C2 escape pod and locker wall screens (4) | `screen_1x1_1_00255x00255_a` is screen only (0.225 m square) | nothing |
| C2 bulkhead, engineering and hump pads (25) | housing plus a screen quad (0.07–0.11 m) | the housing with a see-through hole |
| C2 bridge, door, locker and toilet buttons (12) | `CRUS_Starlifter_Button_*`, `StarlifterBridge_Button_Componant` | the housing with a see-through hole |
| C2 lift call buttons (2) | `CRUS_Starlifter_Elevator_Call_Button`: touchpad, decal and `Screen` | the housing and decal arrow; screen transparent |
| C2 lift inner buttons (1) | `CRUS_Starlifter_Elevator_Inner_Buttons` is screen only (12 verts) | nothing |
| Hangar elevator call panels (8) | `console_elevator_1_panel_a`, `mesh_screen_1` only (0.21 × 0.375 m) | nothing |
| Hangar medical lift panels (2) | `console_elevator_1_panel_a`, `screen_9x16_1_00308x00547_a` | nothing |

The press already works. DOORS-1 (`ASCVehicle::DoorControls`, `FindInteraction`, `SCExploration.cpp:176–190`)
resolves a trace hit on an invisible pad to its door: `sc.FP.Pads` gave 80/80 on the Galaxy. The missing piece is
only what the pad looks like.

### Smallest fix that makes them appear

1. **Draw `DisplayScreen` as what the data says it is:** an opaque screen. That means the base colour from
   `BackColour`, a smooth dielectric glass top (authored `Specular`, gloss from `Shininess` or the DDNA when present),
   and emission = screen image × `EmissiveStrength`. The strength comes from the authored `Glow` through the existing
   `FSCStarEngine::GlowNits` rule (`SCMaterialDescribe.cpp:205`).
   - With no image, the face is a dark glass screen, not a hole. This alone makes every panel in the table appear.
   - `UIPlane` (`RTT_Hud`, HUD projector glass), `Hologram*` and the `Invisible()` material keep their current path.
2. **Bind the default-state image the exporter already wrote.**
   - Read `ui_bindings[]` on children and interior placements.
   - For each binding with a `generated_image_path`, load the PNG (sRGB, mips; `SCTextureImporter` already decodes PNG).
   - Give the placement's `$RenderToTexture` slots a material instance cached per (sidecar, submaterial, image). The 51
     C2 door pads share one image, so this is one instance, not 51.

## 2. What the game shows, and where the content lives

### Door pads: BuildingBlocks UI driven by the door's state

Example: `ControlPanel_Screen_DoorControl_9x16_Small`, record `2a7c4264-7399-4e45-9a41-f0fbdbb72e78`. The same holds
for `ControlPanel_Screen_DoorControl`, `_1x1` and `_Starlifter_Storage`.

- **Content.** `UIBuildingBlocksEntityComponentParams`: layer `MainRoot`, view `DefaultView`, canvas
  `libs/foundry/records/ui/buildingblocks/i_interactivescreens/i_door/i_door_standard.json`
  (`BuildingBlocks_Canvas.I_Door_Standard`). `UIRenderToTextureEntityComponentParams.runtimeImageSource =
  UserInterface` puts it on the `$RenderToTexture` slot.
- **State.** `SInteractionStateMachineParams` mirrors the linked door through `SLinkedInteractionStateChangedStateModifier`.
  State types:
  - `DoorOpenState`: DoorClosed / Opening / Opened / Closing
  - `DoorLockState`: DoorUnlocked / Locked
  - `PowerState`, `LightState`, `DoorBlockState`, `DoorAccessState`, `DoorPressureState`, `SecurityState`,
    `OverrideableState`
- **Bindings.** The canvas binds `state.dooropenstate.{dooropening, doorclosing, dooropened}`,
  `state.doorlockstate.{doorlocked, doorunlocked}`, `state.powerstate.powerlost` and
  `state.overrideablestate.CanOverride`. Variant canvases exist: `I_Door_Standard_OpenCloseOnly`, `_Elevator`,
  `_Airlock`, `_Red` and `_Locked_Hackable`.
- **Per-class options** in user variables: `ui_HideLockControls`, `ui_HideLightControls`, `ui_CustomLabel`,
  `uv_RoomTitle` and others.
- **Exported image.** The exporter renders the default state once per canvas to
  `Data/UI/Generated/ship/crus/Starlifter C2/buildingblocks_canvas_i_door_standard.png` (1920 × 1080). It shows an
  "OPEN" button on the dark blue door style with a door icon and a settings cog.

### C2 lift buttons: geometry, decal and an empty screen

`CRUS_Hercules_Elevator_ButtonsCall` (`4a218af8-…`) and `CRUS_Hercules_Elevator_ButtonsInner` (`ca2d6f90-…`) have no
UI components: no canvas and no `ui_bindings`.

- **Look.** The call button is a dark painted touchpad with a decal arrow (`Decal_Lift_TouchPad`) and a `Screen` face
  (`RTT_Screens`) with no provider. In game that face is presumably the dark `BackColour` glass, which needs
  confirming with a capture.
- **State.** `SCItemInteractionGeometryMapParams` maps interactions to geometry tags (`Ascend` → `WaitingForAscend`,
  `Descend` → `WaitingForDescend`, `RequestFloor1` → `WaitingForLobbyTop`, `RequestFloor2` → `WaitingForLobbyBottom`).
  This is the "called" feedback. What the tags switch on this CGA is open (Q2).
- **Sound.** `SCItemInteractionEffectsMapParams` tag `fb5ee7d5-…` plays `Play_UI_Levski_Elevator_Button`.

### Hangar elevator call panels: BuildingBlocks UI driven by the transit system

`TransitInteractionUIPanel_Elevator_Ext`, 8 of them in the `playerhangar_elev_hangar_ht_gateway` containers.

- **Content.** Canvas `libs/foundry/records/ui/buildingblocks/i_interactivescreens/trn/i_trn_elevator_exterior.json`.
- **Bindings.** `transitdisplay.panelLocation`, `transitdisplay.currentStopName`,
  `transitDisplay.stopManuallyRequestedAtDisplayLocation` and `state.PowerState.NoPower`.
- **Texts.** From user variables: `@ui_interactor_call_elevator`, `@ui_interactor_elevator_called` and
  `@elevator_is_at_location`.
- **Exported image** (512 × 740). A red field with "CALL ELEVATOR" drawn left-aligned and cut off: the layout is
  wrong, and the red may be an unpinned no-power state (Q1).
- **Other panels in the package.** The medical drop-off lift panels (`TransitInteractionUIPanel_EmergencyServicesElevator_Ext`
  and `_Int`) render acceptable images. The in-car floor-select panel (`TransitInteractionUIPanel_Elevator_Int`, canvas
  `I_Trn_Elevator_Interior`, a floor list from `transitDisplay.destinations`) lives on the carriage, which is not in the
  hangar package (WP-P5).

### Light panels (same mechanism, same fix)

C2 `ControlPanel_Screen_LightControl` (4, canvas `Screen_LightControl`, "Door Control:" heading and three light-mode
rows). They come free with the door pads.

## 3. How a press reaches the door or lift

### In the game

- **Interaction point.** The panel's `SEntityInteractableParams.InteractionPoints` sits on a helper in its CGA:
  `$IP_enter` on pads, `$IP_Button` on the lift call button, `$proxy_elevator_button` inside the car and `$IP_Screen`
  on hangar panels. Its radius is 1.6 m, and it carries a highlight behaviour.
- **Door pads.** The pad's shared interactions `OpenDoor`, `CloseDoor`, `LockDoor` and so on are `Sendable` with a
  `LinkingTag`. They go to the entities named in the pad's `EntityLinks`: the door, its proximity sensor, and one id not
  in the package. Linkable interactions come back (`OpenComplete`, `CloseComplete`, `SyncLock` and others), and the
  pad's state machine mirrors the door's state.
- **Lift buttons** send `Ascend` / `Descend` / `RequestFloorN` / `RequestSurface`. They receive `ElevatorArrived`.
- **Hangar panels** send `CallElevator` to the transit system (`TransitInteractionPanelParams`,
  `TransitPeripheralParams`). Their `links` are empty: the route goes through a transit manager that is not exported.

### In the viewer today (DOORS-1)

- **Linking.** `SCImportJob.cpp:1293–1306` reads each placement's `links` (or `loadout_owner_entity_id`) and records
  pad anchor → door rig in `DoorControls`.
- **Pressing.** `FindInteraction` resolves a crosshair hit on a pad to its door. E (`sc.FP.Interact`) toggles it, and
  `sc.FP.Door` and `FP_DOOR_LIST` list and drive it. Proximity sensors open doors on approach.
- **C2 links.** Every C2 pad resolves to its door (`out/screens-datamine/c2/panel_links.txt`).
- **Gaps.** No lift button has a working route: the call buttons have no links, and the platform links the inner
  buttons and gate but not a manager. The hangar elevator panels have none either.

This WP keeps that routing. It adds what the pad shows in each state, and the lift-panel hooks that WP-P5 consumes.

## 4. Tiers, cheapest first

| Tier | Shows | Effort | Data path | Risks |
|---|---|---|---|---|
| **T0 Visible screens** | Every panel as a dark glass screen with reflections. No hole, no missing quad. MFDs blank cleanly too. | S, UE only | sidecar `alpha_config`, `BackColour`, `Specular`, `Glow` | brightness and look against D15; Nanite now allowed on those slots |
| **T1 Default image** | Door pads show the game's idle UI ("OPEN" button); light panels; elevator call panels ("CALL ELEVATOR") | S–M, UE only | `ui_bindings[].generated_image_path` → PNG already in the package | render fidelity (§8); wrong aspect on non-16:9 faces |
| **T2 Aspect-correct renders** | The same images laid out for the face's real shape: square, portrait, 4:3 | M, exporter | `screen_aspect.rs` measured for interior placements, keyed on the `$RenderToTexture` slot | layout regressions in starbreaker-ui's frozen targets (run its guards) |
| **T3 State images** | Pad flips to "CLOSE" while open, shows opening, closing and locked states; lift panels show called and arrived states | M–L, exporter + UE | exporter pins `state.<type>.<state>` per state and writes `state_images[]`; viewer swaps on door and lift events | number of renders; states that need runtime text (floor names) |
| **T4 Feedback** | Highlight on aim, the game's prompt text (`@interaction_open`, `@ui_interactor_call_elevator`), the button sound | S–M, UE | `InteractionPoints` (radius, helper, `HighlightBehaviors`), `DisplayName` loc, effect-tag sounds | Wwise playback path |
| T5 Live UI (later, only if needed) | Live text such as floor lists and timers | XL | an in-engine BuildingBlocks interpreter, or starbreaker-ui per event | not worth it for panels with a handful of states |

**Minimum that makes the C2 interior and the hangar lifts look alive:** T0 + T1. **Faithful:** T0–T4 for door pads;
T0, T3 and T4 for lift panels, with the state events from WP-P5.

## 5. Interface with WP-P5 (lifts)

**WP-I1 provides** a panel component per lift panel placement, with:

- the panel's interactions, read from data: `Ascend`, `Descend`, `RequestFloorN`, `RequestSurface`, `CallElevator`;
- its interaction point (helper, radius) and prompt text.

**WP-P5 provides** for each lift:

- `Call(panel)` and `RequestStop(panel, interaction)`;
- events `OnCalled(panel)`, `OnArrived(stop)`, `OnDeparted(stop)` and the current stop name.

**Behaviour.** The panel shows the image for the current state, or the geometry tag `WaitingFor*` on C2 buttons, and
plays its effect tag.

**Ownership.** Until WP-P5 lands, lift panels are visible (T0/T1) and their press logs `PANEL_PRESS` with the
interaction name, with no motion. How a panel finds its lift (transit manager, carriage, the missing link ids) is
WP-P5's to resolve. This WP does not add links.

## 6. Recommended first slice — I1-1 "panels appear" (T0 + T1)

**Changes (UE side only; no exporter change, no re-export: the PNGs are in today's packages):**

1. **`SCMaterialDescribe`.** `DisplayScreen` with a `render_to_texture` slot becomes an opaque screen description,
   either a new `ESCMaster::Screen` master in `create_materials.py` or the opaque lit master with an emissive input:
   - base colour `BackColour`;
   - specular from `Specular`, roughness from `Shininess` or the DDNA smoothness;
   - `ScreenImage` texture parameter (default black) × `EmissiveStrength = GlowNits(Glow)`;
   - `EmissiveSaturation` and `EmissiveContrast` applied as the game's screen shader does (or recorded as a TODO with
     the exact formula once decoded).

   No `bNonNanite` for it. Update `SCMaterialTests.cpp:499` to the new contract (a test first).
2. **`FSCPackageReader`.** Parse `ui_bindings[]` on `children[]` and `interiors[].placements[]`: `binding_kind`,
   `source_entity_name`, `helper_name`, canvas names, `default_state_is_off`, `generated_image_path`,
   `ui_screen_aspect_w_over_h`.
3. **`SCImportJob`.** For each instance whose bindings carry an image, bind it to the instance's screen slots:
   - **Children:** match `helper_name` to the mesh node. This is the MFD case, so skip `binding_kind == "mfd"` and HUD
     bindings: they stay blank by the owner's decision.
   - **Interior placements:** bind all of the placement's `$RenderToTexture` slots.
   - **Caching:** use one material instance per (sidecar, submaterial, image) and load each PNG once per import.
   - **Logging:** log `SCREEN_BIND instances=<n> images=<m> skipped_mfd=<k>`.
4. **Docs.** Update `ue/README.md` (materials and first-person sections) and `vendor/StarBreaker/docs/decomposed-export-contract.md`'s
   consumer note through WP-D1 (the UE consumer now exists).

**Done means:**

- The C2 (`-SCLoad=CRUS_Starlifter_C2`) and the hangar (`-SCEnvironment=…/hightech_a/hangar_xltop_001.socpak`) show
  every panel in the section 1 table. Evidence: first-person screenshots in `ue/verification/WP-I1.md` (`sc.FP.Spawn`
  plus `sc.FP.Look`) of:
  - the bridge door pads and bridge buttons;
  - a corridor ring pad;
  - an escape-pod 1×1 screen;
  - an engineering pad;
  - the lift call button and the inner buttons;
  - two hangar elevator call panels.
- Pads show the "OPEN" door UI; hangar panels show the elevator UI; lift buttons show dark glass with their decal.
- `SCREEN_BIND` counts:
  - C2 interior: 51 door + 4 light + extinguisher bindings;
  - hangar: 8 elevator + 2 medical + kiosks.
- Regressions:
  - `DOOR_LINKS` unchanged and `sc.FP.Pads` 100 % on the C2 (pads must remain hittable);
  - MFD faces are dark glass with no image.
- `UNIT_TESTS_OK`; the importer tests pass. Astra reviews the screenshots for brightness against the D15 look.

**Next slices:**

- **I1-2 (T2) aspect.** Submodule branch `scviewer/panel-screens`:
  - measure `ui_screen_aspect_w_over_h` for interior placements;
  - identify render-target faces by the `$RenderToTexture` virtual slot, not the `rtt_screen` / `rtt_hud` name markers
    (which miss `screen_rtt` and `screen`; a name heuristic against the upstream rules);
  - make the render key include the aspect, which it already does.
- **I1-3 (T3) states.** Exporter:
  - for a physical binding whose owner has `SInteractionStateMachineParams` state types with `bindingsMethod = States`
    that the canvas binds, render one image per state of `DoorOpenState` × `DoorLockState` (pinning
    `state.dooropenstate.*` / `state.doorlockstate.*` through `DefaultValueRegistry::insert_path`);
  - add `ui_bindings[].state_images[] {state_type, state, generated_image_path}` to the contract.

  Viewer: swap the panel's image on the door's state change. This is the existing door-open bookkeeping: `IsDoorOpen`,
  `ToggleDoor`, sensor opens, and fragment start and end for opening and closing.
- **I1-4 (T4) feedback.** The highlight, prompt text from loc, the interaction radius from data (1.6 m against the
  viewer's 3 m reach), and the button sounds.

## Result (I1-1)

**State (2026-11-19):** done for T0 + T1, viewer-only. No exporter change and no re-export. The package and cache
formats are unchanged, so no cache key moves.

**Material.** `DisplayScreen` with the virtual `render_to_texture` slot maps to a new master, `ESCMaster::Screen` /
`M_SC_Screen` (`create_materials.py` `screen_master`). It is opaque and Nanite, and is not split into the non-Nanite
mesh. The master takes:

- base colour `ScreenBackColor`, the authored `BackColour`;
- F0 from the authored `Specular`, with the look's glass reflectance (`GlassReflectanceScale` / `GlassReflectanceFloor`,
  set by `ApplyGlassLook` and `sc.Look.GlassReflectance`). On screens the floor applies even where `Specular` is 0, since
  the face is the display's glass cover;
- gloss from `Shininess` (`SurfaceGloss`);
- emission `ScreenImage × EmissiveColor × EmissiveStrength`, with the strength from `GlowNits(Glow)` (the existing rule).
  `ScreenImage` defaults to `T_SC_Black`, so an unbound screen is dark glass.

A forward-flagged `DisplayScreen` stays opaque. `UIPlane` / `Monitor` / `UIMesh` / `Hologram*` faces, inactive screens and
`Invisible()` keep their old path. Q3 is still open: `EmissiveSaturation` / `EmissiveContrast`, the ViewingAngle falloff,
the pixel grid and the TexSlot6 screen mask are not applied (TODO in `SCMaterialDescribe.cpp` `Screen`).

**Bindings.** `FSCPackageReader` reads `ui_bindings[]` into `FSCSceneInstance::UiBindings`: kind, source entity, helper,
canvases, `default_state_is_off`, the image path (normalized; a path outside the package is dropped) and the aspect.
`FSCSceneInstance::PanelImage` is the data rule for "operating panel":

- a `physical` binding with a render that is not off by default;
- on a child, only when `source_entity_name` is the child itself. A screen entity mounted on a dashboard or seat (the HUD
  projector, `Vehicle_Screen_Physical`) is a vehicle screen;
- `mfd` / `radar` bindings never count;
- different images on one entity return none and are counted as `ambiguous`; the helper-to-face match is not
  implemented.

`FSCImportMaterials` decodes each panel image once with the material textures. `FindScreen` makes one instance per
(sidecar, submaterial, image): every C2 door pad shares one. The import logs
`SCREEN_BIND instances= images= panel_images= skipped_mfd= ambiguous=`. Screen slots do not record paint swaps.
`sc.Screens [i [cm]]` lists every `M_SC_Screen` face (image, size, resident mips, UV0 range, centre, normal) and views one.
The view logs the first hit on the ray to the face, so an occluder shows up.

**Tests (tests first).** `SCBridge.Materials.ScreenFamily` failed on the old rule (master `M_SC_Emissive_Unlit`, blend
2, non-Nanite, strength 0 against 2560). `SCBridge.Package.UiBindings` is new. The `FamilyMapping` pin is kept (the
fixture's `screen` is a UIPlane) with the message reworded; `MasterAssets` covers the new master.

| | Tests | Pass | Failures |
|---|---|---|---|
| before | 159 (158 + ScreenFamily) | 154 | the 4 known + `ScreenFamily` |
| after | 160 | 156 | the 4 known only (`FolderTarImport`, `KitRigAndWeapons`, `FamilyMapping` on its `scene v3 rejected` line, `SkeletalCollision`) |

`create_materials.py` printed `WP-U2 MATERIALS_OK` (`M_SC_Screen` 30 expressions, ps=465).

**Game run** (C2 in `hangar_xltop_001`, `-SCCamera=first`, then fly; port 8771). Screenshots are in `out/panels/v1`
and `probe*/`, with the face list in `out/panels/v1/faces.txt`.

| Check | Result |
|---|---|
| C2 `SCREEN_BIND` | `instances=78 images=4 skipped_mfd=11 ambiguous=0`. 78 = 51 door pads + 2 ramp buttons (children, own UI) + 4 light panels + 14 extinguisher cabinets + 7 extinguishers. 11 = 7 MFD + 2 radar + the HUD projector + the annunciator |
| Hangar `SCREEN_BIND` | `instances=22 images=5`: 8 elevator call, 2 + 2 medical drop-off, 2 freight kiosks, 8 ship-selector kiosks |
| Door routing | `DOOR_LINKS panels=35 sensors=8`, the same as before the change. `FP_PADS total=35 resolved=33 no_door=2`: every pad that has a door resolves, so the pads are still hittable. `FP_DOOR_LIST` has 34 entries |
| Faces | 133 `M_SC_Screen` faces. 53 show the door UI, 4 the light UI, 7 the extinguisher UI and 23 hangar images; 46 are dark: MFDs, lift buttons, the personal-inventory kiosks |

**Astra** (gpt-6.2-astra, effort high, 12 shots):

- Door pads, wall and bulkhead pads, the engineering pad and the light panel read as in-game controls. Their brightness
  is right for D15, with no clipping or bloom.
- The C2 lift call button and the copilot MFD are clean dark glass with no leftover content.
- The hangar elevator call panel is the brightest screen in the set: "somewhat pale salmon" but not clipped.

It raised three items, all outside I1-1's material and binding scope:

1. **Escape-pod `screen_1x1` faces** are larger than the opening they appear in, overlapping the pod door's bulkhead
   pad. The mesh is screen-only, so where it sits is placement data. Check it against an in-game capture.
2. **Medical drop-off exterior panel** (`console_elevator_1_panel_a` in `elev_ht_dropoff`) shows a blank glow instead of
   "EMERGENCY DROP OFF". This is not the binding: the texture is right (512 × 740, all mips resident, and the decoded
   pixels hold the box and label), and it is not glass, translucency, Nanite or mips (A/B probes). The `SCREEN_VIEW` ray
   hits `hosp_elevator_door_frame_a` 6 mm in front of the screen face, so a surface of the door frame covers it. This is a
   placement or geometry question for a later slice.
3. **Medical interior panel**: the UI sits low and small in the glass. This is the exporter render's layout on a
   non-16:9 face; it belongs to I1-2 (aspect) and the §8 render-fidelity risk.

**Not done or open:**

- I1-2 aspect: door images stretched onto portrait and square faces;
- Q1–Q3;
- the `decomposed-export-contract.md` consumer note (WP-D1, vendor submodule): written in the review fixes (below);
- the 14 extinguisher cabinets count as bound in `SCREEN_BIND`, but none of their faces appears in the `sc.Screens` list.
  Resolved in the review fixes: their screen slot has no triangles, and only drawn slots count now;
- `sc.Emissive.Report` counts unbound screens at their nominal glow, because it does not sample `ScreenImage`. Resolved
  in the review fixes: an unbound screen's strength is 0.

## Review fixes (I1-1)

Reviews of 3e3b85d: `out/panels/review/terra-review.md` (T1–T4) and `astra-code-review.md` (A1–A8), triaged in
`out/panels/review/fix-brief.md`. The fix set was reviewed again (`out/panels/review2/`, Terra and Astra); its high and
medium findings were fixed once (below). Exporter changes are on the submodule branch `scviewer/ui-binding-role`
(commits `672cb046`, `011f35f3`, `b658c230`, `416f5d6d`); the cache format is 30.

**Classification (T1/A1), from data.** The exporter now writes three facts on every `ui_bindings[]` entry
(`decomposed-export-contract.md`):

- `source_item_type`: the UI owner's `SAttachableComponentParams.AttachDef.Type`;
- `source_owner_resolved`: whether that owner's record was found;
- `source_kiosk_interaction`: whether the owner has `KioskInteractionEntityComponentParams`, i.e. the player operates its UI.

A binding is an operating panel's when it is `physical`, its owner is resolved, and the owner is not a `Display` item or
is an operated one (`FSCUiBinding::IsOperatingPanel`). Who carries the binding no longer matters.

| Owner (LIVE 4.10.2) | Item type | Kiosk component | Result |
|---|---|---|---|
| door and light panels (`ControlPanel_Screen_*`) | `ControlPanel` | yes | image |
| transit panels (`TransitInteractionUIPanel_*`) | none (not attachable) | yes | image |
| ship-selector and freight kiosks | `Display` / `UNDEFINED` | yes | image |
| fire-extinguisher cabinets and extinguishers | `Usable`, … | — | image |
| `Vehicle_Screen_MFD` / `_Physical`, HUD projector, radar displays | `Display` | no | dark |
| `SCItemDisplayScreen` (signage, including the Ironclad's Cutlass countermeasures HUD canvas) | `Display` | no | dark |
| an inline canvas on an entity without a class | unknown (`source_owner_resolved` false) | — | dark |

A geometry-less door-control child's binding carried by its parent stays a panel. A self-owned cockpit screen stays
dark. An export without these fields cannot be classified: its screens stay dark and the import warns to re-export.

**Per face (T2/A2, T3).** `FSCSceneInstance::ScreenSlotImage` decides each screen material slot:

- The importer records which rig nodes draw each screen slot (`DisplayScreen` submaterial), separately for the skinned
  part and the rigid parts.
- A binding whose `helper_name` names a node of the mesh owns that node's subtree (the nearest helper wins).
- The entity's own other bindings own the faces no helper claims. A binding propagated from another entity whose helper
  is not on the mesh claims none.
- One image over all faces of the slot binds it. A face nobody owns, an excluded binding, or faces with different
  outcomes sharing the slot leave it dark (`ambiguous`).

Two panel images on one entity land on their own faces; a shared slot that cannot be told apart stays dark.

**Other items.**

- **A4.** A `DisplayScreen` with authored opacity below 1 or a blended alpha mode takes the new
  `M_SC_Screen_Translucent` (lit translucent, coverage = `Opacity`, non-Nanite split); the mesh split agrees.
  A GLB-only `BLEND` on a screen its sidecar describes as opaque no longer puts it in the split. An alpha-tested RTT
  screen is drawn opaque with a warning (none exists in the C2, hangar or Ironclad exports).
- **A5.** Only screen slots with triangles in the drawn static variant or skeletal LOD are bound and counted. New
  `SCREEN_BIND` fields: `slots_bound`, `slots_dark`, `ambiguous` (per slot), `unclassified`.
- **A8.** An unbound screen's `EmissiveStrength` is 0; the image-bound instance takes the authored glow
  (`FSCMaterialDesc::ScreenGlow`). The emitter tools skip sections at 0 nits at any threshold.
- **T4/A6.** `sc.FP.Pads` passes a pad only when the view ray's first visibility hit is under the pad's anchor and
  `FindInteraction` from that ray returns its door. `sc.FP.Pads negative` repeats this with the pads' collision off (the
  full collision state is restored); `FP_PADS_NEGATIVE ok=1` needs no pad to pass and at least one door still in
  fallback range.
- **A7.** `sc.Screens` reads a resident LOD held by a reference while it reads it, checks the CPU buffers, the section's
  index range and indices against the position and UV buffers, and lists `geometry=unavailable` otherwise.
- **A3.** Cooked levels carry `USCCookLibrary::FormatVersion` (2) in `<level>.cookformat`, written only after a
  successful cook.
  - The cook scripts report an older or missing stamp as STALE, and refuse an export whose `ui_bindings` lack the new
    fields.
  - `package_showcase.ps1` refuses stale levels; the workspace's `DRAK_Golem` and `hightech_a_hangar_smltop_001` need a
    recook.
  - Pooled meshes are keyed by their Nanite build and section topology.

**Tests.** The new `SCBridge.Screens.PerFaceAssembly` imports a synthetic package through assembly. It covers a mixed
panel/MFD child, a door control propagated to its host, a self-owned cockpit screen, two images on one entity, a
propagated binding whose helper is missing, a 0.6 overlay in the translucent split, and an unused screen slot.

- With the I1-1 rule (one image on every screen slot) the test failed on 9 checks.
- `ScreenFamily` covers A4 and A8. `UiBindings` covers the rule cases, including the Ironclad HUD placement, a kiosk, a
  transit panel, an unresolved owner and an older export.
- Rust: `ui_binding_json_carries_source_item_type` and the live `ui_bindings_carry_the_source_item_type`. The latter
  failed before the inline-canvas path set the type, and before the kiosk field existed.

| | SCBridge tests | Pass | Failures |
|---|---|---|---|
| before | 161 | 157 | the 4 known |
| after | 162 | 158 | the 4 known (`FolderTarImport`, `KitRigAndWeapons`, `FamilyMapping`, `SkeletalCollision`) |

`starbreaker-3d --lib`: 949 passed, 4 ignored. `create_materials.py` printed `WP-U2 MATERIALS_OK`; only the new
`M_SC_Screen_Translucent` asset was kept (every other regenerated asset was restored from git, `M_SC_Glass` included).

**Runs** (C2 in `hangar_xltop_001`, port 8771; `out/panels/v2/report.txt`, first iteration in `v2/iter1/`):

| Check | I1-1 | Now | Why |
|---|---|---|---|
| C2 `SCREEN_BIND` | 78 / 4 images / 11 skipped | `instances=64 images=3 panel_images=4 skipped_mfd=11 slots_bound=64 slots_dark=17 ambiguous=0` | the 14 extinguisher cabinets drop out: their `DisplayScreen` slot 3 has no triangles (A5), so the cabinet image is decoded but bound nowhere |
| Hangar `SCREEN_BIND` | 22 / 5 | `instances=22 images=5 skipped_mfd=0 slots_bound=22 slots_dark=28` | unchanged; the 8 ship-selector kiosks are `Display` items with the kiosk component |
| `DOOR_LINKS` | 35 / 8 | 35 / 8 | |
| Faces | 133 | 133 (same per-mesh counts and images) | |
| `sc.FP.Pads` | 33 / 33 routable resolved (any ray) | `resolved=28 hittable=28 no_door=2 fallback_only=5` | the strict check finds 5 pads that geometry covers (below) |
| `sc.FP.Pads negative` | — | `resolved=0 hittable=0 fallback_only=28`, `ok=1` | the old check would have passed 28 pads without collision |

The 5 pads that are not hittable resolve their door only through the `NearestDoor` fallback. In each, the first hit
is another mesh in front of the pad:

- the two escape-pod `screen_1x1` pads: `CRUS_Starlifter_Cor_Cyl_Bulkhead_A`, 21.8 cm in;
- a wall pad: `Top_Wall_DoorFrame_2m`, 7.4 cm;
- the other wall pad: `Top_Wall_Addon_Unit_A_2m`, 48 cm;
- the lift platform's gate: `Hold_Wall_lighed_0_5m001`.

This is placement or geometry, the same class as the I1-1 findings on the escape-pod screen and the medical drop-off
panel.

**Ironclad** (`-SCLoad=DRAK_Ironclad`, first fix iteration; `out/panels/v2/ironclad/`):

- `SCREEN_BIND instances=172 images=5 skipped_mfd=78 slots_bound=184 slots_dark=130`, `DOOR_LINKS panels=107`;
- every `ui_screen_RTT_4x3_a` face, including the command-module countermeasures HUD, is dark glass;
- the 0.6 `Flash` faces (`ui_screen_4x3_a`, `ui_screen_9x16_a`) draw with `M_SC_Screen_Translucent`;
- the DRAK door pads show `I_Door_Small_DRAK`.

The later fixes do not change these owners' classification: `SCItemDisplayScreen` has no kiosk component, which the
live Rust test checks.

**Astra** (first iteration's 16 shots, `out/panels/v2/astra-visual.md`):

- 10 pass. Door, wall, bulkhead, hump and engineering pads and the light panel read as lit in-game controls without
  clipping or bloom; the C2 lift buttons, the copilot MFD and the inventory kiosk are clean dark glass.
- The 4 fails and 1 partial are the I1-1 open items, unchanged by this fix (the `bridge_button` shot is identical to
  I1-1's):
  - the bridge button seen from behind its strut;
  - the bed-light panel's image sideways on its face (I1-2 layout);
  - the escape-pod `screen_1x1` overlap;
  - the medical drop-off exterior panel covered by its door frame;
  - the medical interior layout (I1-2).
- The MFD context shot was obscured (unverified).

**Review of the fix set** (`out/panels/review2/terra-review.md`, `astra-code-review.md`). Fixed:

- unresolved owners treated as panels (H; `source_owner_resolved`);
- a propagated binding with a missing helper painting other faces (H);
- the 8 ship-selector kiosks going dark under a blanket `Display` rule (H; `source_kiosk_interaction`);
- a stale export cooking with a current stamp, and a stale stamp beside a failed cook (H);
- skinned ownership including rigid triangles (M);
- a GLB-only `BLEND` screen in the split with the opaque master (M);
- `sc.Screens` reading an LOD without holding it (M), and indices checked against one buffer only;
- older packages going dark silently (M; the import warns);
- `sc.FP.Pads negative` leaving a Custom profile and passing with nothing measured (M);
- `sc.Emissive.Lights` with `MinNits 0` lighting unbound screens (M);
- the pool key reusing a Nanite mesh for an all-translucent build (M).

**Low or open:**

- No assembly fixture for a hybrid (skinned and rigid) rig, nor for retained-asset reassembly. Skinned screen faces
  are owned by their GLB mesh node, not by the joints that skin them.
- Alpha-tested RTT screens have no masked master (warned; no live case).
- The GLB-only `BLEND` rule is not exercised through a GLB fixture; `ScreenFamily` sets `bBlended` directly.
- The cook-script stamp and stale-export checks were not run against a real cook (the `package_showcase.ps1` check was
  dry-run: `DRAK_Golem` is stale).
- The 5 non-hittable C2 pads (placement or geometry).
- The I1-1 items above: escape-pod placement, the medical drop-off door frame, I1-2 aspect.

## Labels and state UI (research, 2026-11-19)

**Owner (binding):** "The panels should be drawing the UI for the floor selection, but they do not. Same for door and
other panel associated buttons. They are missing actual labels and state UI." Door and lift panels are inherited from
the game; HUDs and MFDs stay out (section 7).

Read-only research: no build, no vendor edit. Evidence and the full log are in `out/panel-ui-datamine/` (`NOTES.md`). The C2
and hangar packages were re-rendered with `starbreaker ui render --dump-ir-dir`; the door render is byte-identical to the
PNG in the package, so the IR dumps describe exactly what the viewer shows.

### Why the text is missing

Text rendering is not the cause. Glyphs come from the BuildingBlocks SWF font library, and `@` keys resolve through
`global.ini`. The door pad's IR holds the resolved texts OPEN, LOCK, UNLOCK, NO POWER and EMERGENCY OVERRIDE (the last four
on inactive nodes), and "OPEN" is in the PNG. The labels go missing, or land in the wrong place, at four points before
drawing, all in `starbreaker-ui` or the exporter:

1. **The large button component is not built.** `BuildingBlocks_ComponentGeneralButtonLarge` is not a node type that
   starbreaker-ui knows (`bb_scene/types.rs` `BbNodeType`). It is not expanded from its template
   `ButtonGeneralLargeComponentStandard`, although that template is in DataCore. `bb_resolve/engine_04.rs`
   `expand_widget_standards` expands only icons, the primary and secondary buttons and scrollbars.
   - **Door pad.** The main button (`DoorButton`) gets no frame, no background and no label layout. Its node spans the
     whole content area (78, 47, 1764 × 903) and draws "OPEN" left-aligned at the edge. The blue bar under the text is the
     button shelf (`ButtonContainer`), and the lighter box is `LightButton_Emergency_Toggle`, not the door button.
   - **Hangar elevator panel.** `Button_Call` has the same defect, which cuts "CALL ELEVATOR" off at the left edge.
   - **Other components.** `ComponentLabelCaptionPair` (the transit floor labels), lists and checkboxes are not
     expanded either.
2. **Labels set by a style are dropped.** `BuildingBlocks_FieldModifierLocalization` on the `Text` field falls into
   `bb_brand_apply/modifiers.rs` `apply_modifier`'s fallback arm. It is logged as unrecognised, and the raw `@key` is
   written to a field the IR never reads (24 times in the C2 render log). This is how `LightControl_Component` labels
   its rows On, Auxiliary and Emergency, so the light panel's rows show an icon and an empty bar. Only the `ParamInputN`
   form is handled (`bb_scene/clone_expand.rs`). The INI keys are `ui_label_auxiliary,P` and `ui_label_Emergency,P`, so the
   fix must look them up with that suffix.

   `out/panels/v2/door_button-0.png` is this light panel (bed light, `Screen_LightControl`, drawn sideways), not a
   door pad.
3. **Two binding operations are not evaluated.** `BuildingBlocks_BindingsBooleanFromTagSwitch` and
   `BindingsOperation_BooleanFromLocalizationIsEmpty` are missing from `bb_bindings/eval.rs`, as is
   `BindingsTagFromTagSwitch`. An unevaluated binding keeps the authored value.
   - **Hangar panel.** `StyleColor_BG_Overlay.IsActive` and `Button_Call.Disabled` are a tag switch on
     `StateType.EnableState.TagGUID` that defaults to false. The overlay therefore stays on. This is very likely the red
     field of Q1, the disabled or lockdown look rather than the idle one.
   - **Door header.** `Text_RoomName` stays active with no text. This is harmless.
4. **The export passes none of the panel entity's own values.** `ui_pipeline.rs` `UiBindingView` carries the canvas,
   kind, helper, aspect and a transit location key. It carries no user variables and no state, and starbreaker-3d reads
   no `UserVariables` at all.
   - **Room name.** The door header's room name is `ui_LocationName`, a localized-string user variable. Its class default
     is `@LOC_EMPTY`, and no C2 instance overrides it, so the C2 pads show no room name in the game either.
   - **Hidden controls.** Every C2 `ControlPanel_Screen_DoorControl_9x16_Small` instance in `top_deck_c2` overrides the bool
     user variables at index 0, 2, 4 and 5 to true: `ui_HideLockControls`, `ui_HideSensorControls`,
     `ui_HideLightControls` and `ui_HideEmergencyLightControls` (`.soc` `EntityComponentUserVariables`). The export still
     draws the emergency-light toggle these hide.
   - **Transit labels.** On the hangar panels, `transitdisplay.panelLocation` ("YOUR LOCATION …") and `currentStopName`
     ("ELEVATOR LOCATION …") are empty. The binding's `transit_location_loc_key` is null, because the transit manager is
     not in the hangar's data (WP-P5 §2).

**Root cause, in one line:** unsupported BuildingBlocks constructs in starbreaker-ui (the large button component,
style-set localized text, two boolean operations), plus an export that passes no panel-entity values. Fonts, glyphs and
localization work.

### Door panels: states and what they change

The state comes from the pad's `SInteractionStateMachineParams`, which mirrors the linked door. Everything else is
bindings in `I_Door_Standard` and its sub-canvases `IC_DoorHeader`, `IC_DoorButtonShelf` and `IC_DoorFooter`:

| Input | Effect |
|---|---|
| label index `(ui_AccessDenied ? 4 : 0) + (doorlocked ? 2 : 0) + (dooropened ? 0 : 1)` | `DoorButton` text: 0 **Close**, 1 **Open**, 2–3 **Locked**, 4–7 **Access Denied** (`BindingsLocalizationFromIntegerSwitch`; upper-cased by the label style) |
| `state.doorlockstate.doorlocked` | button `Disabled`; footer status icon |
| `state.dooropenstate.dooropening` / `doorclosing` | the "animate 1" state tag on the button: a colour animation while the door moves |
| `state.dooropenstate.dooropened`, `doorlocked`, `doorbreachedstate.doorbreached` | footer `Icon_DoorStatus` SVG |
| `state.powerstate.powerlost` | hides the main content and shows NO POWER |
| `state.overrideablestate.CanOverride` | EMERGENCY OVERRIDE overlay and its colour |
| `ui_LocationName`, `ui_Hide*`, `ui_HideDoorButton`, `ui_AccessDenied` | per-instance user variables: room name, which shelf buttons exist |

For the C2 the useful set is **Closed (OPEN)**, **Open (CLOSE)**, **Moving** (OPEN or CLOSE with the animate tag, which
one still image cannot show faithfully), **Locked** and **No power**. The light panel adds its light state (`lightson`,
`lightslow`, `lightsalarm`, `lightswitch*`). `DefaultValueRegistry::insert_path` can pin each state path; the CLI cannot,
so no CLOSE or LOCKED render was made here.

### Lift panels: where the UI really is

- **C2 floor panel (`CRUS_Hercules_Elevator_ButtonsInner`).** The earlier "no UI content" finding stands, with one
  refinement.
  - The panel and the call buttons have no UI components. The floor names exist only as the display names on the panel's
    interaction point (`$proxy_elevator_button`): Top Deck (primary), Lower Deck and Exit Ship. That is the game's
    interaction prompt, which P5-1 already lists with mouse-wheel choice.
  - **Refinement.** The carriage `CRUS_Hercules_Lift_Platform` does carry UI:
    - canvas `I_Door_Standard_Elevator`: one large button reading CALL, UP or Wait from the carriage's door state, through
      the user variables `UpOverride` `@panel_call`, `DownOverride` `@panel_up` and `WaitOverride`;
    - a render-to-texture component, a kiosk component, and a full copy of the door panel's state machine.

    The platform mesh has no screen, though: its NMC nodes are `Lift_Platform`, `Decal_Lift_Platform` and `$nav_a/b`,
    and no primitive uses `RTT_Screens`. On the C2 that canvas has nowhere to draw. The exported carriage placement has
    `ui_bindings: []`, which `ui_binding_for_record` should not produce; this is unexplained and matters only if a face
    exists.
  - **The face.** The inner panel's one screen face is a rounded 0.27 × 0.23 m plate that samples UV u 2.387–2.504,
    v 0.640–0.738. That is a window of about 12 % × 10 % of a texture, with the plate's aspect (1.19 against 1.18), not a
    full canvas. What the game shows there is not in the data: Q2, which needs a capture.
  - **Owner decision.** There is no game data for a floor list drawn on the C2's panel. Drawing one would be our own UI,
    not an inherited one. Done as our own UI in WP-P5 "Result (floor panel)", drawn on this face through
    `SetScreenFaceMaterial` (the face's UV window mapped by `M_SC_Screen`'s `ScreenUVTransformX` / `Y`).
- **C2 call buttons.** They show no screen UI. Their feedback is the geometry tags `WaitingForAscend` /
  `WaitingForDescend` (Q2) and the button sound (I1-4).
- **Hangar elevator call panel (`I_Trn_Elevator_Exterior`).** It has a "YOUR LOCATION" floor name
  (`transitdisplay.panelLocation`), an "ELEVATOR LOCATION" current stop (`currentStopName`), and the large CALL ELEVATOR
  button, which reads ELEVATOR CALLED when `stopManuallyRequestedAtDisplayLocation` is set. The two names need the
  hangar's transit line, which is not in the hangar's data (WP-P5 P5-4).
- **A real floor-select list** is `I_Trn_Elevator_Interior`, on transit carriages: a `WidgetList` of
  `ComponentGeneralListItem` bound to `name` and `stoprequested`, plus `currentStopName`, `nextStopName`,
  `traveldirection` and `@elevator_currentfloor`. This is P5-4's carriages (890J, Carrack, Idris …), not the C2.

### Options

| | What | Effort | Fidelity | Data path | Where | Risks |
|---|---|---|---|---|---|---|
| **(a) Fix starbreaker-ui, bake one image per state** | the four fixes above; the exporter feeds class and instance user variables and pins `state.*` per state; `ui_bindings[].state_images[]`; the viewer swaps the image on door and lift events | M–L (exporter M; viewer S, the I1-3 bookkeeping) | High for every bounded state: door labels and all their states, light rows, call or called | DataCore canvases → IR → PNG per (canvas, aspect, user-variable values, state) | upstream, submodule branch `scviewer/panel-screens`, tests first; viewer `SCImportJob` / panel component | starbreaker-ui's parity regime (`ui_check.sh`, frozen targets: a new expansion type must use the second ID band so it cannot renumber frozen IDs); no in-game reference for these canvases yet; render count (about 5 states × aspects × variable sets per canvas); generated paths must carry the key |
| **(b) Bake static, draw dynamic text in UE** | (a)'s static parts, plus floor or stop names drawn into the text-field rect from the IR | M on top of (a) | Medium: the font is the game's SWF glyphs in (a) but a UE font here; the layout is redone | the IR's text rects and styles in the package; strings from loc | viewer | a second text and layout engine; nothing to gain for door pads, whose texts are a closed set; lift names are a closed set per lift too (`lifts[].stops[]`), so (a) can bake them |
| **(c) Live canvases in UE (UMG / Slate → render target)** | interpret the canvas description at runtime | XL | High only after re-implementing BuildingBlocks (styles, bindings, layout, SWF shapes) | the IR or the raw canvases | viewer | duplicates starbreaker-ui; out of proportion for panels with a handful of states |
| (a2) Render on demand through the FFI | (a)'s renderer, called per new state at runtime and cached | M on top of (a) | as (a), plus unbounded text | the P4k at runtime | `scbridge-ffi` | cooked and showcase builds have no P4k at runtime; a hitch per render |

**Recommendation: (a).** Every label the owner names is a closed set that the game's own canvases describe: door
Open / Close / Locked / Access Denied / No power, the light rows, call or called. The C2 floor names are the interaction
prompt, which P5-1 shows, and the hangar's floor names need a transit line that is not in the data. (b) and (c) add a
second UI engine without gaining fidelity.

### Recommended next slice: I1-5 "panel labels" (exporter, then I1-3 on top)

Submodule branch `scviewer/panel-screens`. Each defect gets a failing test first in starbreaker-ui, against an extracted
fixture with a provenance note (crate rules: no copied game values, no name branches).

1. **Large button.** `ComponentGeneralButtonLarge` becomes a node type, expanded from `ButtonGeneralLargeComponentStandard`
   in the second expansion ID band.
2. **Style-set text.** `FieldModifierLocalization` on `Text` sets the node's text through the loc fetcher, including the
   `,P` keys.
3. **Boolean operations.** `BindingsBooleanFromTagSwitch`, `BindingsTagFromTagSwitch` and
   `BindingsOperation_BooleanFromLocalizationIsEmpty` are evaluated.
4. **Panel-entity values.**
   - The exporter reads the owner's `UserVariablesComponentParams` class defaults and the `.soc` instance overrides
     (`EntityComponentUserVariables`, by `__index`).
   - It passes them to the registry for that binding.
   - The generated image path carries a hash of the values. Shared output paths are first-written-wins today (§8).
5. **Docs.** `decomposed-export-contract.md`; the cache version + 1.

**Done means:**

- **C2 door pad.** The render shows a framed, centred OPEN button, and no emergency-light toggle on the C2 instances
  that hide it.
- **Light panel.** The rows read On / Auxiliary / Emergency.
- **Hangar call panel.** It shows a centred CALL ELEVATOR with no red overlay.
- **Evidence.** `ui render --dump-ir-dir` IR diffs and PNGs in the PR.
- **Tests.**
  - `bash scripts/ui_check.sh` reports ALL GREEN, with no drift on frozen targets (Clipper GOLD).
  - `cargo test -p starbreaker-3d --lib` passes.
  - The viewer's `SCREEN_BIND` counts are unchanged.
- **Review.** Astra compares the in-game shots against owner captures of a C2 corridor pad (closed, open, locked), a bed
  light panel and a high-tech hangar elevator panel. No capture exists yet; without them parity is unverified.

**Then I1-3 (states) on top:**

- the exporter renders the door set (Closed, Open, Locked, No power, plus Moving as the animation's sample) and the
  transit called state into `state_images[]`;
- the viewer swaps images on the door bookkeeping (`IsDoorOpen`, fragment start and end, lock) and on the lift's
  `OnRequested` / `OnArrived`.

**Done means:** in first person, a C2 pad reads OPEN, then CLOSE while its door is open, and LOCKED when locked. A
hangar panel reads ELEVATOR CALLED after a press.

**Not in this slice:**

- a floor list on the C2's inner panel: there is no game data for one, so it needs an owner decision and the Q2 capture;
- the hangar's floor and stop names, which need P5-4's transit line;
- I1-2 aspect, which composes with this: the render key already includes the aspect.

## Result (I1-5, with I1-3 and I1-2)

Submodule branch `scviewer/panel-screens` (from `scviewer/lifts`), commits `6b00f574` (starbreaker-ui), `1919f186`
(owner values, state renders, texture aspect), `f6761e08` (review fixes), `df52450e` (operating-panel rule, text
alignment enumerations). `CACHE_FORMAT_VERSION` 31 → 32; the cook stamp `FormatVersion` 2 → 3.

### Root causes and fixes (each with a failing test first, starbreaker-ui)

1. **Large button.** `ComponentGeneralButtonLarge` is a scene node type, expanded from
   `ButtonGeneralLargeComponentStandard` in the second expansion band, styled from the brand's `buttonlarge` kit sheet
   (brand record fallback when the canvas carries no brand styles, only for a known manufacturer). The component's
   label is instanced (`_LabelInstanced_`) so the host does not draw a second, left-aligned copy; the host-bound
   `parameter` input is wired through synthetic params. Tests in `bb_resolve/engine_04_tests_expansion.rs`.
2. **Style-set text.** `FieldModifierLocalization` on `Text` sets the node's text (raw key resolved by the loc fetcher,
   `,P` keys included); `Horizontal/VerticalAlignment` enumerations map to the text alignment. Tests in
   `tests_modifiers.rs`.
3. **Boolean operations.** `BindingsBooleanFromTagSwitch`, `BindingsTagFromTagSwitch` and
   `BooleanFromLocalizationIsEmpty` are evaluated in bindings and in the state filter (which no longer skips a tag
   switch as an unset variable). Tests in `tests_state_tags.rs`, `tests_b.rs`.
4. **Per-instance values.** The exporter reads the owner's `UserVariablesComponentParams` defaults and the `.soc`
   `EntityComponentUserVariables` overrides (by `__index`) and passes them as a per-binding registry overlay; render
   identity includes them (`ui_owner_values.rs`, `pipeline/tests.rs`, `socpak_lift_tests.rs`). They attach only to an
   operating panel: physical, owner resolved, and not a plain `Display` item unless a kiosk.

### I1-3 states and I1-2 aspect

- **States.** The exporter reads the owner's `SInteractionStateMachineParams` state types, renders the default (every
  type at its default) plus one image per non-default state, drops renders identical to the default, and writes
  `state_images[]` (`state.<type>.<state>`) into `ui_bindings`. Door pads export `DoorOpenState.DoorOpened`,
  `DoorLockState.DoorLocked` and `PowerState.PowerLost`; Opening, Closing, PowerLow and CanOverride render identical to
  the default and are dropped. Kiosks export their `Main.*` states. 69 state images in the C2 + hangar scene.
- **Viewer.** `FSCScreenFace` per (component, slot); `RefreshPanelScreens` follows the linked door's rig state each tick
  (open → `DoorOpened`), one MI per (sidecar, submaterial, image); faces carry `SCKeepComponent` so instancing does not
  merge them. `sc.Screens.States [face] [state|-]` lists and pins.
- **Aspect.** `$RenderToTexture` faces are measured in texture space (|∂P/∂u| / |∂P/∂v|) and rendered at an
  area-preserving size bounded at 8192 (cargo-bay pad 1440x1440, habitation pad 1086x1909, light panel 720x1280).

### Verification (C2 + `hangar_xltop_001`, first person, port 8771)

Shots in `out/panels/v3/`. `FP_PADS total=34 resolved=28 hittable=28` (unchanged), 131 screen faces.

- **Cargo-bay pad** (face 41, door 28): reads OPEN closed, CLOSE after the toggle, back to default when closed; the
  LOCKED and no-power pins swap. Label centred, framed. Astra: PARTIAL — layout CLOSE, label MATCH/CLOSE, colour
  MISMATCH (pale on screen; the baked image is saturated blue with a dark footer, so the wash is the viewer's screen
  emissive/exposure), aspect MISMATCH (the reference shows the landscape recessed wall pad, not the square bulkhead pad
  framed here).
- **Habitation pad** (face 51, door 30): OPEN → CLOSE → OPEN, centred. Astra: PARTIAL — layout MATCH, label CLOSE,
  aspect CLOSE, colour MISMATCH (same wash; turquoise in the reference).
- **Light panel**: header and the On / Auxiliary / Emergency rows resolve, but the rows are laid out off the left edge
  and the header reads "Door Control:". Astra: FAIL.
- **Hangar call panel**: CALL ELEVATOR, no red field, but on an amber field and at the top edge. Astra: PARTIAL.

Tests: `ui_check.sh` ALL GREEN (starbreaker-ui lib 632 passed), `starbreaker-3d --lib` 975 passed, CLI `ui::` passed;
UE `SCBridge` 171 tests, 167 pass, the 4 known failures unchanged; new `SCBridge.Screens.StateImages`.

### Review (Terra + Astra, `out/panels/review3/`)

Fixed: state types in the render key; output path collisions (64-bit stem digest, one namespace); second expansion band
kept across child-scene merge; empty host-bound strings kept; CLI replay names and sanitising; texture-aspect bound;
reset and untargeted playback refresh faces; pin release on unlinked faces; instancing keeps faces; brand fallback only
for a known manufacturer; state discovery follows canvas/value references; aspect gated on operating panels; cook
stamp. Declined: lift called state (scope), DoorOpenState literals (game vocabulary, not asset names), render-count
optimisation and sparse overrides (low).

### Open

- Light panel row layout (rows off-canvas) and header text: a layout defect in the list container, not fixed here.
- Hangar call panel: the pipeline's default brand (`.or(Some("drak"))` in starbreaker-ui `pipeline/mod.rs`) is a hard-coded game value,
  flagged; the label sits at the top edge.
- Screen emissive wash in the viewer (colour parity on all pads); the header gear icon renders in the Critical red.
- LOCKED shows the label plus the lock icon; the reference's lock strip is absent on the pads framed here.
- ELEVATOR CALLED (needs the transit state); C2 inner ramp buttons are not texture-aspect measured.
- Frozen Clipper parity (`ui_check.sh --full`) not run: no Clipper export on this machine.
- `SCBridge.Screens.StateImages` and the socpak override parse test were written with the code, not failing first.

## Fix pass (I1-5)

Submodule branch `scviewer/panel-screens`, commits `80eb8711`, `b44edbf2`, `47f1858e` (from `df52450e`).
`CACHE_FORMAT_VERSION` 32 → 33 and the cook stamp `USCCookLibrary::FormatVersion` 3 → 4 (the panel renders and the
screen master's instance constants change). Owner references in `out/panel-ui-datamine/refs/`; shots and
Astra verdicts in `out/panels/v4/`.

### 1. Screen wash (M_SC_Screen)

**Cause.** The game's `DisplayScreen` shader (`DisplayScreen.cfxb` `DisplayScreenPS` over glassLib `GlassShared`'s
`%EMISSIVE_MAP` path, read from the installed shader cache) does not emit a fixed luminance. The C2's `RTT_Screens`
carries `%DBA_GLOBAL`, and its emission is

    image → MonitorEffects_ApplyViewingAngle(contrast, saturation 1.15, viewing angle)
          × MatEmissiveColor.a (the linear glow: Glow² × 400 = 2.56 scene units for Glow 0.08)
          × GetGlobalDBAMultiplier_HDR = clamp(adapted luminance, EV 1, EV 14) / 0.18
          × (1 − glass opacity 0) × transmittance (white Diffuse: 1)

The DBA term cancels the exposure: the screen is displayed at 2.56 × image after exposure, whatever the room's
brightness. The viewer emitted a fixed 2,560 cd/m² × image. In a dim corridor the exposure is set for a few cd/m², so the
face was exposed ~10–30× too high and the display curve's shoulder rolled it to near white (v3: "pale on screen").

**Checked and not the cause.** sRGB: the panel PNGs carry no colour chunk, are decoded as sRGB (`ResolveSRGB`), and
the sampled field is (0, 124, 208, 255), alpha 1. Tonemapping: the viewer's display curve is the game's
`HuePreservingShoulder` (fork, WP-E1 E1-4); for the exposed field (0, 0.56, 2.02) it gives sRGB (63, 174, 252), the
colour now on screen. Bloom and Lumen follow the emission (the screen is no longer a 2,560-nit source in a dim room).
Glass floor 0.045 (the viewer's glass look, raising the authored F0 0.0097): it adds a reflection of a few percent of the
room, which lifts a dark footer slightly but cannot whiten a 2.56-exposed field; unchanged, not A/B-measured. The red header gear was not a decode error: it is the
settings button, which the game hides at rest (item 2), and its red was the large-button kit sheet's alert colour bleeding
onto every button icon (item 2b).

**Fix (tests first).** `M_SC_Screen` / `M_SC_Screen_Translucent` port the chain: `SCREEN_VIEWING_ANGLE`
(MonitorEffects_ApplyViewingAngle) and `SCREEN_DBA` (GetGlobalDBAMultiplier_HDR through UE's `EyeAdaptationInverse`:
adapted luminance = middle grey / exposure, in cd/m², clamped to `NitsAtEV(1)`..`NitsAtEV(14)`, ÷ 0.18 × 1000), and
`ScreenTransmittance`. `Describe` sets `ScreenDBA` from the `DBA_GLOBAL` token, the bounds, the authored
`EmissiveContrast` / `EmissiveSaturation` / `ViewingAngle` / `ViewAngleMode` (the shader's declared defaults when
absent). Every constant is in `StarEngineRender.json` (`dbaMiddleGray`, `dbaMinAdaptedEV`, `dbaMaxAdaptedEV`,
`monitor*Default`) with the hashes of `GetGlobalDBAMultiplier_HDR`, `MonitorEffects_ApplyViewingAngle` and
`DisplayScreenPS`; 70/70 shader entries re-checked against the installed pak (the executable cvars were not re-run:
no `objdump` on this machine). `SCBridge.Materials.ScreenFamily` failed on the old describe (red log in the session
scratch), passes now; `SCBridge.StarEngine.Fixture` covers the new fixture fields.

**Result.** The field is saturated blue again with a dark footer and white text (v4), but still lighter than every
owner reference (Astra: colour MISMATCH on all nine pairs; the references read at roughly 0.3 × our exposed value).
The ported chain reproduces the game's documented maths exactly, so the remainder is a runtime value the data does
not give. Evidence for the next step:

- `GlassShared` applies `RTT_Tonemap(emission, RTT_CalcExposure(...))` when `RenderToTextureSizeParams[1].y >= 0`
  (the render target's luminance mip): either a Reinhard to LDR or a rescale of the UI so its average matches the
  scene. Those parameters are set by the engine per render target; they are not in DataCore, the MTL or the shaders.
- The executable names a `SRTTTonemapGammeEncodeRes` pass and `DirectRTT_AfterTonemappingParams`,
  `r_RTT_AR_AfterToneMapping`: UI render targets may be tonemapped / gamma-encoded before the screen samples them.

No constant was tuned to the references. The pixel grid (its map is not exported), interference and dirt are not ported.

### 2. Light panel layout and header; door header gear

**Causes (starbreaker-ui).**

- The header `Label_Container` is `IsActive = !IsEmpty(ui_LocationName)`. The state filter's evaluator did not
  evaluate `BooleanFromLocalizationIsEmpty`, and an unset variable counts as unknown runtime data, which keeps the node
  shown; the light panel's owner declares no `ui_LocationName` at all. The door header's `parent_Header`
  (`ui_showSettingsButton || !IsEmpty(ui_LocationName)`) and its red gear stayed shown the same way.
- `LightControl_Frame` (anchor 0, pivot 0.5, 100 % wide) in a `ColumnReverse` Start/Start column was placed at
  `−pivot × width`: half off the left edge.
- The door column (`parent_middle`: header Auto, DoorButton grow 1, shelf Auto) overlaid its non-grow items on the whole
  column; the button shelf sat across the middle of the DoorButton.

**Fixes (failing tests first).** The exporter passes the owner's complete user-variable set; the pipeline marks the
registry (`set_owner_user_variables_known`), and a bare-named variable the owner does not declare reads at its type's
default. `BooleanFromLocalizationIsEmpty` over a known localized variable becomes a static boolean in the state filter.
A start-justified item without a cross anchor keeps its pivot inside the column (as the Center branch already did).
Non-grow items flow beside a growing one at their content extent (`auto_content_main_extent`). Main-axis `Stretch`
shares a wrapped line's free space (one lock button spans the shelf, two split it). A localized variable holding a `@`
key reads as its text. Tests: `tests_owner_values.rs` (3), `engine_03.rs` (3 layout), `tests_state_tags.rs` (1).

Light panel: header hidden (the owner has no room name), three full-width rows inside the screen. Door pads: no header
gear (no reference shows one), shelf under the field.

### 2b. Light panel colours and glyphs (owner follow-up)

Evidence chain, all from the CRUS data (`selected_style_source` `manufacturer:crus`):

| Data | Value | Before | After |
|---|---|---|---|
| `sk_crus_env_buttonlargestyles` `RootIconElementInstance` (Tag `icon-element-instance`) | `FillColor` = `Critical` (the alert red) | applied scene-wide: every button's glyph and the door header's settings gear red | the sheet applies inside `ComponentGeneralButtonLarge` hosts only (it is the large-button standard's sheet); the large button's lock icon keeps its red |
| `sk_crus_env_buttonlargestyles` `RootIconWidgetInstance` | size 0.5 × 0.5, anchor (0.5, 0.3), pivot 0.5, ScaleY 0.81 | the same sheet sized and placed the secondary buttons' icons (large, high, right) | not applied to secondary buttons |
| `sk_crus_env_buttonsecondarystyles` `RootFilledIconElementInstance` (`icon-element-instance` under `fill-style-filled`, parent not `disabled`) | `FillColor` = `Bright` | overridden by the large sheet | the glyph colour: white |
| `…buttonsecondarystyles` `RootFilledSelected` / `RootFilled` | the On row's selected blue, the others' teal (α 0.175) | unchanged | unchanged |
| `buttongeneralsecondarycomponentstandard` embedded `RootPosTop` / `RootIconPosVertical` (gated on the `icon-pos-top` state tag; `ComponentRoot.SecondaryStateTag` ← `IconPosition`) with the kit's `RootIcon` (size 1 × 1, `MaxSize` 35, `MinSize` 18) | column flow, icon height capped at 35 | the state tag resolves (`Tag.299578d0` icon-pos-top), but the template's embedded entries gated on it are not in effect and `EnableMaxHeight` stays off, so the icon fills the row's width at the top | **open** |

The red was therefore not a state (no alert state, power or light value reaches it): it was the large-button kit sheet
bleeding into other components (test `modular_kit_sheets_include_the_large_button_sheet`, failing on the scene-wide
application). Re-shoot `out/panels/v4/reshoot/light_panel.png`: white glyphs, On blue, Auxiliary / Emergency teal
(Astra, `astra-light-panel-reshoot.md`: PARTIAL — rows inside the screen, On blue, others teal, glyphs white; glyphs larger,
left, straddling each button's top edge). The
glyphs still sit at the top-left of each row: their size and position come from the template's `icon-pos-*` entries,
which starbreaker-ui does not apply yet. The host's `iconPosition` now reaches the template's `IconPosition` parameter
through the template's own tag switch (`template_enum_param_index`); for these rows it is the default 0 (top), so it does
not change them.

### 3. Brand

`.or(Some("drak"))` and the manifest's, SWF selection's and `load_style`'s `"drak"` defaults are removed: the brand comes
from the canvas's `style` or the binding's manufacturer, else none (register entry in `ui-fallback-register.md`). The
hangar transit panel has no data brand: `TransitInteractionUIPanel_Elevator_Ext` is not attachable (no manufacturer),
its `defaultStyle` is null, and neither `hangar_xltop_001.socpak` nor the gateway container names a UI style. Door pads
do carry one: their `.soc` instances override `AttachDef.Manufacturer` to `SCItemManufacturer.CRUS` (whose
`BuildingBlocksStyle` would be the per-entity source; today the binding's `crus` comes from the root entity's code).
The CLI replay now uses the exporter's manufacturer rule (`ui_pipeline::root_manufacturer_id`; it derived `hangar`).

Hangar call panel: no amber (that was the Drake palette); a neutral black field with CALL ELEVATOR. The label still
sits at the top: without a brand no modular-kit sheet centres the large button's text field (open).

### 4. LOCKED

The canvas binds the DoorButton's `ParamInput1` (ShowIcon) to `doorlocked` and `ParamInput0` (ShowLabel) to
`!doorlocked`: locked shows the lock icon only. The binding resolved correctly, but the modular kit's
`RootTextFieldElementInstance` (`IsActive` true), re-applied after the bindings, revealed the label again. A
binding-resolved `IsActive` on a widget-standard instance now stands against later style passes
(`BOUND_IS_ACTIVE_KEY`; test `bound_visibility_wins_over_a_style_is_active`).

### Icons (owner references, Crusader pads)

- **Which buttons.** `IC_DoorButtonShelf`: Lock (`!HideLock && !locked`), Unlock, Light normal / low (`!HideLight`, by
  `lightslow`), Emergency light (`!HideEmergencyLight`); the shelf shows when any is enabled. C2 `top_deck` instances
  hide all four (no row: the WC, wide and square references); loadout pads keep the class defaults (light low +
  emergency: the two-button row of the portrait references). Our pads now show the same rows in the same place.
- **Glyphs: missing (open).** Each button's glyph is an authored `base_Icon` (`icon_common_light_low.svg`,
  `…_light_emergency.svg`, `…_lock_action.svg`). The CRUS brand block of the shelf canvas hides `base_Icon`
  (`Hide_Base_Icon`) and instead sets the component's `ParamInput1` (`ShowSecondaryButtonIcon`, on nodes tagged
  `general-button-secondary`), i.e. the secondary button standard's `ShowIcon` with its `customIcon`. starbreaker-ui
  neither tags a component host with its component tag nor routes a style-set `ParamInputN` into the expanded template
  (brand styles apply after the expansion), so the buttons draw blank.
- **Corner icon.** `Icon_DoorStatus` (door state SVG) — present; Astra finds ours taller and yellow where the
  references read compact cyan.

### Square pads

The square bulkhead pads (`Cor_Cyl_Touchpad_Bulkhead_A*`, 1440 × 1440) operate
`CRUS_Starlifter_Door_Component_4m` (`FP_DOOR_LIST` 28): a component-access door, as the owner suspected.

### Verification (C2 + `hangar_xltop_001`, port 8771, `out/panels/v4/report.txt`)

`FP_PADS total=34 resolved=28 hittable=28` (unchanged), 132 faces. Shots: cargo-bay wall pad (the landscape
`Ramp_Button_Landscape`, the ACS LV1 pose of the M2 reference) closed / open / locked, square component pad,
habitation pad, Crusader portrait ring pad, WC pad (each closed / open / locked), light panel, hangar call panel, pilot
MFD.

| Reference | Ours | Layout | Label | Icons | Colour | Aspect | Verdict |
|---|---|---|---|---|---|---|---|
| C2 cargo bay | cargo_bay_wall-open | CLOSE | MATCH | MISMATCH | MISMATCH | CLOSE | PARTIAL |
| M2 ACS LV1 | cargo_bay_wall-open-context | MATCH | MATCH | MISMATCH | MISMATCH | CLOSE | PARTIAL |
| M2 wide | cargo_bay_wall-open | CLOSE | CLOSE | MISMATCH | MISMATCH | MISMATCH | PARTIAL |
| C2 habitation | habitation-closed | CLOSE | MATCH | CLOSE | MISMATCH | CLOSE | PARTIAL |
| Crusader icons | portrait_ring-closed | CLOSE | MATCH | MISMATCH | MISMATCH | CLOSE | PARTIAL |
| Crusader icons 2 | portrait_ring-closed | CLOSE | MATCH | MISMATCH | MISMATCH | MISMATCH | PARTIAL |
| Crusader main deck | portrait_ring-closed | CLOSE | MATCH | MISMATCH | MISMATCH | CLOSE | PARTIAL |
| Crusader portrait | habitation-closed | CLOSE | MATCH | MISMATCH | MISMATCH | CLOSE | PARTIAL |
| C2 WC | wc_pad-closed | CLOSE | CLOSE | CLOSE | MISMATCH | MISMATCH | PARTIAL |
| C2 square | component_square-closed | MATCH | MATCH | CLOSE | MISMATCH | CLOSE | PARTIAL |

No reference: light panel PARTIAL (inside the screen; icons high-right of the labels, blue-on-teal labels), hangar call
panel PARTIAL (label at the top), pilot MFD PASS (clean dark glass). Against v3 the layout and label columns moved from
CLOSE/MISMATCH/FAIL to MATCH/CLOSE, the light panel from FAIL to PARTIAL; colour is closer but still MISMATCH.

Tests (after the review fixes): `ui_check.sh` ALL GREEN (747 passed), `starbreaker-3d --lib` 975 passed, CLI `ui::` 2 passed; SCBridge 171 tests,
167 pass, the 4 known failures (`Package.FolderTarImport`, `Player.KitRigAndWeapons`, `Materials.FamilyMapping`,
`FirstPerson.SkeletalCollision`). `create_materials.py` printed `WP-U2 MATERIALS_OK`; only `M_SC_Screen` and
`M_SC_Screen_Translucent` were kept.

### Review (Terra + Astra, `out/panels/review4/`)

Fixed: Auto text beside a growing item collapsing to zero (now measured), `PercentOfY` / `PercentOfX` sizes and the
cross offset of non-grow items beside a growing one, a held non-boolean registry value defaulted to false (only a miss
defaults), the large-button sheet's scene-wide application (above), the cook stamp (3 → 4: cooked levels re-cook),
the screen emission's colour factors (GlassShared multiplies by the scalar glow and the target's alpha; the master no
longer multiplies by `EmissiveColor` and now by the image alpha).

Not fixed, with the reason:

- **DBA from the clamped exposure (high, both).** The game's DBA uses the unclamped adapted luminance; UE's
  `EyeAdaptationInverse` gives the clamped exposure, so below the exposure clamp (adapted luminance under
  ~9 cd/m²) the viewer's screens are brighter than the game's, up to 36× at EV 1. Reading the fork's adaptation state
  needs an engine feature (WP-E1); recorded in Open.
- **Owner-set completeness (medium, both).** A bare variable is defaulted only when the binding carries owner user
  variables; an owner with none, and a bare runtime binding beside owner variables, are not told apart. Needs an
  explicit completeness flag in `ui_bindings` (contract change).
- **Lumen card capture (medium, both)** caches the exposure-dependent emission until the card refreshes.
- **Transmittance at unit path (medium, Astra)**: exact for the white Diffuse of every C2 screen; per-pixel path not ported.
- **Wrapped Stretch with an Auto item (medium, Astra)**, **visibility precedence across passes (medium, Astra)**:
  no case in the C2 / hangar data; noted.
- **Replay manufacturer list (medium, Astra)**: the replay now follows the exporter's code list
  (`derive_manufacturer_id`, a hard-coded table, flagged: `espe` against DataCore's `ESPR`); the data path is
  `AttachDef.Manufacturer → SCItemManufacturer.BuildingBlocksStyle` (section 3).

### Open

- Screen colour: the ~0.3 factor to the references (runtime RTT tonemap / exposure, above).
- Shelf button glyphs: component tag on hosts and style-set `ParamInputN` routing (above).
- Light-panel and shelf glyph size and position: the button template's `icon-pos-*` embedded entries (above).
- DBA below the exposure clamp; owner-set completeness flag; Lumen card capture of the screen emission (review).
- Hangar call panel label position; the hangar's brand at run time (location style) is not in its data.
- Wide-pad lock bar glyph, M2 side strip (a separate face), the WC field aspect (Astra: 1.8:1 against our 1.1:1 —
  the toilet button's face is measured square), corner-icon size and colour.
- DBA uses the host's clamped exposure; the game's DBA reads the unclamped adapted luminance, so screens differ in
  scenes darker than the exposure clamp (`r_ExposureMultiplierMax`).

## Fix pass 2 (I1-5)

Submodule branch `scviewer/panel-screens`, commits `879413c2` (fixes) and `e26c4cc0` (review fixes), from `47f1858e`.
`CACHE_FORMAT_VERSION` 33 → 34 and the cook stamp `USCCookLibrary::FormatVersion` 4 → 5 (every panel render changes).
Shots and Astra verdicts in `out/panels/v5/`, reviews in `out/panels/review5/`. The rule for this pass: every fix is a
datum the renderer missed; the references only located the gaps. No value was tuned to a reference.

### 1. Shelf glyphs, corner icon

| Data | Value | Before | After |
|---|---|---|---|
| `IC_DoorButtonShelf` `brandStyles[s_crus_env]` "ShowSecondaryButtonIcon" (`Tag(general-button-secondary)`) | `FieldModifierBoolean ParamInput1 = true` | written to the host's raw `ParamInput1` and read by nothing; the template's `ShowIcon` (Boolean `ParamInput1`) stayed at the authored `show: false` | recorded per modifier family (`_StyledParams_`, `Boolean:ParamInput1`) and routed into the template's Boolean parameter on that slot: the icon instance shows |
| `buttongeneralsecondarycomponentstandard` `QuarternaryStateTag` ← `TagFromBoolean(ShowIcon)` | `show-icon` / `hide-icon` | `hide-icon` | the slots the canvas's styles set are found on a probe cascade and routed before any selector runs; the tags are rebuilt |
| the same shelf's embedded "Override Icon Shelf Button Size" (`Type(CustomShape)` inside a secondary button) | `ScaleX/Y 2`, pivot / anchor 0.5 | not applied: `ScaleX` had no handler, and the glyph's custom shape is the nested icon instance, expanded at the parent level after the shelf's styles ran | `ScaleX/Y` write the `scale` the layout applies; a child canvas's runtime entries are re-applied to instances the parent level expands inside it |
| `iconwidgetstandard` `defaultStyles` "IconElement" (no CRUS container) | `SvgContainPositionX/Y 0.5` | template defaults never applied; `SvgContainPosition*` had no handler: glyph top-left in its box | an expanded standard's runtime look (its container's kit sheet, then its brand container else `defaultStyles`, the canvas rule) applies to its own instance at the lowest tier; the field writes `svgFill.containPosition*` |
| `sk_crus_env_buttonprimarystyles` `RootIconElementInstance` | `PivotY 0.5`, `HeightBehavior 2` | applied scene-wide: every secondary glyph hung half a box above its row | each standard names its own kit sheet (`defaultStyles.sharedStyles`): the primary / secondary / large sheets now style their own hosts only |
| `IC_DoorFooter` `Icon_DoorStatus` (a `WidgetCustomShape`) | own fields `color` Bright α 0.7, `enableColorOverlay` true; its `svgFill.color` null | the IR read only the null `svgFill.color` and defaulted to the generic icon colour MissionObjectives (yellow) | the shape's own overlay colour (unless a style sets one): near-white at 70 % on the `Icon_DoorStatus_BG` Base at 50 % (blue) |
| `Icon_DoorStatus_BG` | Fixed 90 × 90 in `I_Door_Standard` (`coordinateMethod aspectOverridesWidth`, 1920 × 1080) | per-axis units: 48 × 167 px on the 1034 × 2005 ring pad (the "taller" icon) | `aspectOverridesWidth` keeps the authored height and lets the aspect set the width, so one unit is `target_h / 1080` on both axes (`aspectOverridesHeight`: `target_w / 1920`): 167 × 167 px. Text keeps its scale (min of the axes; unchanged) |

The glyphs are the data's: light-low `icon_common_light_low.svg` (a disc over an arc; the references' "eye"),
emergency `icon_common_light_emergency.svg` (an exclamation in a broken ring), lock `icon_common_lock_action.svg`.

### 2. Glyph position (the button templates' layout)

| Data | Value | Before | After |
|---|---|---|---|
| secondary template `embeddedStyles` `RootPosTop` / `RootIconPosVertical` (`icon-pos-top`) | `FlexDirection Column`; `EnableMaxHeight true` | the column applied; size bounds were not implemented at all | authored and styled bounds (`sizing.min/max*` + `enable*`, `MinSize*` / `MaxSize*` / `Enable*` / `Min/Max*Behavior`) clamp every resolved size, flex items included |
| kit `RootIcon` | `SizeX/Y 1` Percent, `MaxSize 35`, `MinSize 18` | full-row-height icon | door shelf: the icon is capped at 35 units, ×2 by the shelf's scale: about half the button height, centred |
| `LightControl_Component` `defaultStyles` (its runtime look, no brand container) | `IconWidgetInstance SizeY 0.55`, `EnableMaxHeight false` | — | the light rows show the glyph centred above the label inside each row (Astra: PASS). The kit's `RootIcon SizeY 1` still overrides the canvas's 0.55 (kit sheets apply after the canvas cascade: upstream tier order, ledger 106), so the glyph box is the row minus the label; open |

### 3. Brightness residual: not established from the data

Evidence chain (shader cache `ShaderCache_Vulkan.pak`, executable strings, DataCore):

- `GlassShared` (`glasslib.cfib`) applies `RTT_CalcExposure` + `RTT_Tonemap` to the screen image only when
  `RTT_RequiresTonemapping()` (`RenderToTextureSizeParams[1].y >= 0`); its two branches are a Reinhard to LDR
  (`FLAG_TONEMAP_TO_LDR`) or `RTT_CalculateExposureToMatchScene` (image × adapted scene luminance ÷ the render target's
  mean luminance × an EV-compensation ratio). `RenderToTextureSizeParams` / `RenderToTextureExposureParams` and the flags
  are set by the engine per render target (`PI_RenderToTexture*` per-instance constants in the executable); nothing in
  DataCore, the MTL or the shaders says which UI render targets take either branch.
- The executable's "RTT Tonemap/GammaEncode" pass (`SRTTTonemapGammeEncodeRes`) is `hdrpostprocess.cfxb` `HDRRTTPass` /
  `HDRRTTScenePS`: `CheapToneMappingCurve` then `ToSRGB`, for HDR render-to-texture content (3D scenes). The door pads'
  `UIRenderToTextureEntityComponentParams` say `runtimeImageSource: UserInterface`, `directRenderStage: None`: an LDR UI
  target, so neither that pass nor the `DirectRTT_AfterTonemappingParams` / `r_RTT_AR_AfterToneMapping` path (the
  AR / direct render stages) is established for them.
- `GetGlobalDBAMultiplier_HDR` reads `ExposureState.adaptedLuminance`, which `CalcUpdatedFrameState` stores BEFORE the
  exposure clamp (`curState.exposure = clamp(unclampedExposure, min, max)` is separate). The viewer derives it from the
  clamped exposure. The v5 readback (`sc.Look.ExposureLog` per shot) shows the clamp in force only at the habitation pose
  (`exposure=0.0199985`, metered 0.353); the other poses are inside the clamp, so this does not explain the general
  ~0.3 factor.

So the missing step is the engine's per-render-target RTT exposure state for UI targets (flags and the luminance mip),
which is runtime-only. No constant was added and nothing was tuned.

**Proposed engine feature (WP-E1, not implemented):** expose the metered, unclamped adapted luminance to materials on
the fork (a `View` uniform written by the eye-adaptation pass beside the exposure, read by a material expression; UE
materials that do not use it are unchanged), so `M_SC_Screen`'s `SCREEN_DBA` reads the value `GetGlobalDBAMultiplier_HDR`
reads. Evidence: the fork's readback already carries it CPU-side (`FEyeAdaptationReadback::AverageSceneLuminance`, the
`metered` field); the game shader above. It fixes screens in scenes darker than the clamp (habitation pose), not the
general residual.

### 4. Hangar elevator call panel: the label

`buttongenerallargecomponentstandard` `TextFieldWidgetInstance` authors `verticalTextAlignment: Top`; its
`defaultStyles.sharedStyles` names `modularkit_default_buttonlargestyles`, whose `RootTextFieldElementInstance` sets
`HorizontalAlignment` / `VerticalAlignment` Center, padding 50, and whose `RootBackgroundElementInstance` draws
`Default_button_large_bg.svg` at the Base role, α 0.3. The hangar canvas has no brand, so no brand kit sheet replaced
it — and the standard's own base sheet was never applied. Applied (§1, standard runtime styles): CALL ELEVATOR is centred
in the button. The button background is now the default kit's Base at 30 % (light grey in the hangar's palette); Astra
calls it washed out (PARTIAL). That is the data with no brand: the hangar's runtime UI style (a location style) is not in
its data (fix pass §3).

### 5. WC face aspect: the wrong face was shot

The v4 "WC pad" was `CRUS_Starlifter_Button_Toilet` (binding `51f206eb…`, texture aspect 0.98, linked to no door). The
WC door's own pad is `CRUS_Starlifter_Button_ToiletDoor` (binding `ec57f17f…`, measured 1.759, rendered 1910 × 1086).
v5 shoots it (`wc_door-*`): Astra rates the aspect MATCH. No measurement was wrong.

### 6. M2 side strip

The C2's rear ramp door (`Door_RN_RoomConnector_Breachable_OpenReverse_crus_Starlifter_Rear_Ramp`) carries two
`ControlPanel_Screen_DoorControl_Starlifter_Interior_Button` children (`IP_Interior`, `IP_Exterior`), each one RTT face on
`CRUS_Starlifter_Ramp_Button_Landscape`; no other screen face is near it in the C2 + hangar scene (132 faces). The C2
does not have the strip; the M2 is not exported here, so where its strip comes from is open.

### Tests (failing first)

`starbreaker-ui` (668 lib tests, ui_check ALL GREEN): `tests_modifiers_number.rs` (contain position, scale, styled
parameter families), `bb_layout/tests_size_bounds.rs` (authored / styled / Percent bounds, a bounded grow item, square
`aspectOverrides*` units), `ui_ir/tests_custom_shape_colour.rs`, `bb_resolve/standard_styles_tests.rs` (routing, exact
families, declared parameters, runtime styles, shared kit sheet, kit scoping, child styles on nested instances, a child's
look kept at the parent level, a styled parameter's state tag before the cascade), `tests_state_tags.rs` (tag rebuild).
Each failed on the code before its fix (the review-fix tests on a worktree of `879413c2`); one
(`refresh_state_tags_replaces_the_bound_fields_tag`) passes on both and pins the replacement. `starbreaker-3d --lib` 975,
CLI `ui::` 2; SCBridge 171, 167 pass, the 4 known failures. The review fixes leave every re-rendered panel (portrait ring,
habitation, WC, WC door, light panel, hangar call panel, with their states) byte-identical to the v5 renders.

### Verification (C2 + `hangar_xltop_001`, port 8771, `out/panels/v5/report.txt`)

`FP_PADS total=34 resolved=28 hittable=28` (unchanged), 132 faces.

| Reference | Ours | Layout | Label | Icons | Colour | Aspect | Data expectation | Verdict |
|---|---|---|---|---|---|---|---|---|
| C2 cargo bay | cargo_bay_wall-open | CLOSE | MATCH | CLOSE | MISMATCH | CLOSE | MATCH | PARTIAL |
| M2 ACS LV1 | cargo_bay_wall-open-context | MATCH | CLOSE | MATCH | MISMATCH | MATCH | MATCH | PARTIAL |
| M2 wide | cargo_bay_wall-open | CLOSE | MATCH | CLOSE | MISMATCH | MISMATCH | MATCH | PARTIAL |
| C2 habitation | habitation-closed | CLOSE | MATCH | CLOSE | MISMATCH | CLOSE | MATCH | PARTIAL |
| Crusader icons | portrait_ring-closed | CLOSE | MATCH | CLOSE | MISMATCH | CLOSE | MATCH | PARTIAL |
| Crusader icons 2 | portrait_ring-closed | CLOSE | MATCH | CLOSE | MISMATCH | CLOSE | MATCH | PARTIAL |
| Crusader main deck | portrait_ring-closed | MATCH | MATCH | CLOSE | MISMATCH | CLOSE | MATCH | PARTIAL |
| Crusader portrait | habitation-closed | MATCH | MATCH | CLOSE | CLOSE | MATCH | MATCH | PARTIAL |
| C2 WC | wc_door-closed | MATCH | CLOSE | CLOSE | MISMATCH | MATCH | MATCH | PARTIAL |
| C2 square | component_square-closed | MATCH | MATCH | CLOSE | MISMATCH | CLOSE | MATCH | PARTIAL |

Against v4: icons MISMATCH → CLOSE / MATCH on every pair (the shelf glyphs, the lock-bar padlock, the square cyan-and-white
corner icon), WC aspect MISMATCH → MATCH; every render matches the data-derived expectation. Light panel PASS (glyphs
white, centred above their labels, inside their rows). Hangar call panel PARTIAL (centred label; pale default-kit fill).
Pilot MFD PASS. The remaining differences are colour (the field brightness / saturation, the footer buttons filled where
the references look dark and outlined, the label white where they look cyan) and the wide pad's field proportion.

### Review (Terra + Astra, `out/panels/review5/`)

Fixed (`e26c4cc0`): the standards' runtime styles re-applied by a parent level over a child's finished instances (both,
high); styled parameters routed after the selectors that test their state tags (both, high: probe cascade before the
selectors); the child-instance replay bypassing pending filtering and entry order (both, high); Integer / Number slot
collisions and declared parameters without synthetics (both); state-tag refresh dropping tags other fields or the author
supply (both); flex allocation and stretch bypassing size bounds (both); styled `Min/Max*Behavior` ignored (Astra);
Fixed bounds not scaled through a child-canvas merge (Astra); a custom shape's own alpha over a styled literal fill, a
styled overlay switch, solid colours (both); scale ignored for flex-placed items (Terra); the brand container's own kit
link and the canvas style link not used for standards (Astra); template JSON cloned per host (Astra).

Not fixed, with the reason:

- **Parent / pipeline styles setting a slot on a button a child canvas expanded** (both, high): the host annotations are
  finalized at the child level. No case in the panel data (the CRUS block lives in the canvas that expands its buttons);
  it needs host ownership carried through merges. Documented in `ui-cascade-passes.md`.
- **Child replay for canvases merged by a canvas-reference modifier** (Astra, medium): only `WidgetCanvas.canvas` merges
  carry it; no case in the panel data.
- **Text scale under `aspectOverrides*`** (Astra, medium): geometry uses the square unit, text keeps the min-axis scale
  (the reference door labels match it). The frozen `I_Door_Small_DRAK` (1920 × 1132, `aspectOverridesWidth`) gets Fixed
  widths ×1.048; `ui_check.sh --full` (the frozen Clipper pixels) was not run — no Clipper export on this machine.
- **Kit sheet scoping by the kit's family name** (Astra, medium): the sheet path is the existing kit naming
  (`sk_<kit>_<family>styles`, `modular_kit_style_path`), not an asset name; CRUS's kit is not linked from the standards'
  data, so a data link is not available for it.

### Open

- Screen colour: the ~0.3 brightness residual (§3, runtime RTT exposure state); the footer buttons' fill and the label's
  cyan in the references (no data reached: the CRUS secondary kit's `RootFilled` colours are what we draw).
- DBA below the exposure clamp: the proposed engine feature (§3).
- Light panel glyph box: the canvas's `SizeY 0.55` under the kit's `SizeY 1` (tier order).
- Hangar call panel: the pale default-kit fill without a location brand.
- The wide pad's field proportion (M2 wide reference); the M2 side strip (§6).
- Owner-set completeness flag; Lumen card capture of the screen emission (fix pass review).

## 7. Out of scope: HUDs and MFDs (replaced by our own UI)

Facts for the replacement work:

- **Which faces are screens.** Every face whose submaterial has a `render_to_texture` texture slot (`$RenderToTexture`):
  `DisplayScreen` for MFDs, `UIPlane` for the HUD projector (`RTT_Hud`), `Monitor` and `UIMesh` elsewhere.
- **Placement.** `ui_bindings[]` on the dashboard and seat children names each screen's mesh node in `helper_name`,
  and gives its game role (`binding_kind`, `content_canvas_record_name`) and measured aspect.
- **C2 copilot dashboard** (`CRUS_Starlifter_CoPilot_Dashboard_LOD0.glb`, material `Starlifter_Int_RTT_Screens_07`):
  - nodes `screen_left` 0.234 × 0.174 m (1.34), `screen_centre` 0.425 × 0.239 m (1.78), `screen_right` 0.369 × 0.208 m
    (1.78);
  - all three are 4-vertex quads with UV 0..1 on `TEXCOORD_0`;
  - other helpers: `screen_top_left` / `_top_right` / `secondary_1` / `secondary_2` (MFD), `hud_plane` (HUD, aspect
    0.56), `screen_annunciator` (3.80), `radar_helper`.
- **Cutlass Black:** HUD bindings on `Screen_Annunciator_L` / `_R`, `screen_flight_hud_left` / `_right` / `_upper`,
  `Screen_Left_Upper_RTT_Small`, `Screen_Central_Compass`, `Screen_Small_Radar1` / `2`, `Countermeasures_Screen` and
  `Screen_Radar_RTT`.
- **Blanking cleanly.** The T0 material blanks them: dark `BackColour` glass, no emission. A replacement UI later
  supplies its own render target to the same `ScreenImage` parameter on those slots. Nothing is hidden or deleted, and
  the face keeps its glass reflections.
- **Not needed.** The exporter's MFD renders (`MC_S_*_Master`, `MapDisplayMaster`, `H_HUD_Generic`) stay in the package
  and are not bound.

## 8. Risks

- **Render fidelity of non-parity canvases.** starbreaker-ui's parity work targets the Drake Clipper (`I_Door_Small_DRAK`
  is a GOLD target). `I_Door_Standard` and `I_Trn_Elevator_Exterior` are not targets, and their renders show layout
  errors (left-aligned or cut labels, a red field). Faithful panels may need parity arcs in starbreaker-ui (its
  `starbreaker-ui-screen-parity` skill and `ui-workflow.md`) with in-game reference captures, upstream. Budget for it in
  I1-2 / I1-3.
- **Aspect.** Until I1-2, every door image is 16:9 and is stretched onto square (1×1, bulkhead pads), portrait (ring
  and wall pads, 0.5) and 4:3 (engineering, hump) faces. That is visible, and a reason to do I1-2 straight after I1-1.
- **Brightness.** `GlowNits(Glow)` with Glow 0.04–0.08 against the D15 auto-exposure. The emission must read as a lit
  screen in a dim corridor without blooming. Adjudicate with Astra.
- **Opaque switch side effects.** Screen slots move from the translucent non-Nanite split to the opaque path: Nanite and
  Lumen cards now include them. Check that pads stay traceable for `FindInteraction` and that instancing
  (`FSCInstancing`) does not merge per-image instances.
- **Cache keys.** Exporter changes in I1-2 and I1-3 change packages: bump the importer and exporter cache version (D10).
- **Shared output paths.** Generated PNG paths are names, not hashes, and "first written wins" (contract). Per-state and
  per-aspect renders need distinct names, or the render key must reach the path.

## 9. Open questions

1. Is the hangar elevator panel's red field the game's idle look, or an unpinned `state.PowerState.NoPower` (or power
   default)? Needs an in-game capture of a high-tech hangar elevator panel. Research of 2026-11-19 (section "Labels and
   state UI"): probably neither. It is `StyleColor_BG_Overlay`, whose `BindingsBooleanFromTagSwitch` starbreaker-ui does
   not evaluate, so the overlay keeps its authored "active".
2. What do the C2 lift buttons' `Screen` faces show in game, and what do the geometry tags `WaitingForAscend` /
   `WaitingForLobbyTop` switch on `CRUS_Starlifter_Elevator_Call_Button.cga` and `_Inner_Buttons.cga`? (The inner
   buttons' face samples UV 2.39–2.50 × 0.64–0.74, a sub-window of something.) Needs a capture and a CGA, MTL and NMC
   inspection.
3. The game's `DisplayScreen` emissive formula (`EmissiveContrast`, `EmissiveSaturation`, pixel grid, `ViewingAngle`
   falloff). Decode it from the shader cache, as WP-L3 did for lights, or approximate it?
4. Which pad states does the owner want first: open and close only (`I_Door_Standard_OpenCloseOnly`-like), or lock and
   power too?
5. Pad entity links that point outside the package (one per corridor pad, e.g. `4250ef3f-…`). Is it a room or security
   entity that matters for the lock state?

## Verification commands used for this brief

```
target/debug/starbreaker.exe dcb query EntityClassDefinition --filter '*ControlPanel_Screen_DoorControl*'
target/debug/starbreaker.exe dcb query EntityClassDefinition --filter '*Elevator*'   # CRUS_Hercules_Elevator_*
target/debug/starbreaker.exe dcb query EntityClassDefinition --filter '*TransitInteractionUIPanel*'
target/debug/starbreaker.exe dcb query BuildingBlocks_Canvas --filter '*I_Door_Standard*'
target/debug/starbreaker.exe dcb query BuildingBlocks_Canvas --filter '*I_Trn_Elevator_Exterior*'
tar xf cache/<c2 key>.tar 'Packages/*/scene.json' 'Data/UI/Generated/*' <panel GLBs and sidecars>
```

Outputs, extracted files and the per-panel tables are in `out/screens-datamine/` (gitignored).
