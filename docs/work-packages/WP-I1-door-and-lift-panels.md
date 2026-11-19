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
    not an inherited one.
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
