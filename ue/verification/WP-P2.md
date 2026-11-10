# WP-P2b verification — armed first person (Unreal)

Brief: [docs/work-packages/WP-P2-armed-first-person.md](../../docs/work-packages/WP-P2-armed-first-person.md) (P2b),
status/deviation note at the end of its P2b section. Host: UE 5.8.2, Vulkan, RTX 4090, Star Citizen LIVE 4.10.2.
Kit: `scbridge_export_player` with
`-SCPlayerLoadout=Data/Scripts/Loadouts/Player/DefaultLoadouts/PVE_medium_behr_rifle_gmni_rifle.xml` (male).

## What was built

| Area | Files |
| --- | --- |
| `player` block model + fragment/port rules | `Public/SCPlayerKit.h`, `Private/SCPlayerKit.cpp` |
| Animator (clips, blend spaces, cross-fades, additive/one-shot layers, root pin, spine pitch, off-hand IK), weapon state machine, worker clip loader | `Public/SCPlayerAnimator.h`, `Private/SCPlayerAnimator.cpp` |
| Body component (rig, followers, port alignment, weapons, FP hiding, camera, third person) | `Public/SCPlayerBody.h`, `Private/SCPlayerBody.cpp` |
| Character: body component, eye camera (near plane), keys `1`-`9` `H` `C`, HUD line | `SCFirstPersonCharacter.{h,cpp}` |
| Kit pipeline (export → parse → import → clips), settings/switches | `SCViewerSubsystem.{h,cpp}`, `SCViewerSettings.{h,cpp}`, `SCBridgeClient.{h,cpp}` (`ExportPlayer`) |
| Package reader / import job: child `id`, `port`, nested assemblies, nested palettes, bone-port placement, unpublished/rig-all-skins/no-clip options, instance anchors + helper frames | `SCPackageReader.{h,cpp}`, `SCImporter.h`, `SCImportJob.cpp`, `SCVehicle.{h,cpp}` |
| Automation | `SCViewerAutomation.{h,cpp}` (`sc.Player.*`, `sc.FP.Key`, `waitkit`, `snap`, `-SCPlayer*`) |
| Tests | `Private/SCPlayerTests.cpp`, `SCImportTests.cpp` (`-SCPlayerTests`), `ue/scripts/test_player.py` |
| Routes | `ue/scripts/routes/cutlass-armed-walkthrough.txt`, `ue/scripts/routes/player-demo.txt` |

## Build

```bash
ue/scripts/build.sh SCViewerEditor   # Result: Succeeded (no warnings in the new files)
```

## Null-RHI tests

```bash
"$HOME/UnrealEngine/5.8/Engine/Binaries/Linux/UnrealEditor-Cmd" "$PWD/ue/SCViewer/SCViewer.uproject" \
  -run=pythonscript -script="$PWD/ue/scripts/test_player.py" -SCPlayerTests \
  -SCPlayerPackage="$PWD/out/fps/player-p4-male.tar" -nullrhi -unattended -nop4 -nosplash -ddc=SCViewerDDC
```

Male P4-AR kit: exit 0, `WP-P2 HEADLESS_OK`, 4/4:

```text
UNIT_TEST name=FSCPlayerBlendWeights passed=1   (synthetic grid 42x42 incl. one cell outside, 20 real 2-D kit spaces 26x26; 16429 samples: weights >= 0, sum 1 +-1e-4; examples reproduce themselves; edge midpoint 0.5/0.5)
UNIT_TEST name=FSCPlayerAdditive passed=1       (additive at its last frame leaves the base unchanged; first frame = delta * base; weight 0 no-op)
UNIT_TEST name=FSCPlayerWeaponStates passed=1   (Unarmed->Selecting->Armed->Deselecting->Unarmed; requests ignored mid-transition; swap queue)
UNIT_TEST name=FSCPlayerKitRig passed=1         (kit import, every player joint on the leader rig, 183 clips / 0 failures, camera joint 172.4 cm above the capsule floor,
                                                 P4-AR and S71: stow joint -> hand joint at select start -> hand during deselect -> stow joint after; holster ignored mid-select)
```

Female P4-AR kit (`out/fps/player-p4-female.tar`): **FSCPlayerKitRig fails** (`root rig` null). Not a P2b
defect: the importer rejects six female skins (`Skin inverse bind disagrees with node reference pose; skeleton
sidecar required (WP-R6)`: head, undersuit, arms, core, mobiglas, scalp), so the kit has no root rig. The viewer
then reports `Player body unavailable (capsule only): the package has no body rig` and first person stays WP-P1.
See exporter follow-ups. The other three tests pass on the female package.

## GPU route

```bash
ue/scripts/capture_route.sh p2-cutlass ue/scripts/routes/cutlass-armed-walkthrough.txt -SCDefaults \
  -SCLoad=DRAK_Cutlass_Black -SCCamera=first \
  -SCPlayerLoadout=Data/Scripts/Loadouts/Player/DefaultLoadouts/PVE_medium_behr_rifle_gmni_rifle.xml
```

Kit timings (log): cold `FFI_EXPORT_PLAYER ... seconds=8.973 bytes=258748928 cache_hit=0`,
`PLAYER_KIT_IMPORT seconds=2.609 rigs=23 meshes=41`, `PLAYER_KIT result=Ready seconds=11.871 ... weapons=8
states=144 blend_spaces=39 clips=183 clip_failures=0`; cached: `result=Ready seconds=3.306`. First person is
usable (capsule) during the load; `PLAYER_BODY attached rigs=23 followers=14 head_parts=10 slots=8`.

Frame cost (`frametime 6`, `r.VSync 0`, `t.MaxFPS 0`, same settled view in the cockpit, armed idle; A/B/A by
detaching the body with `sc.Player.Body 0`, i.e. the WP-P1 capsule-only first person):

```text
SCRIPT_FRAMETIME tag=body-a       frames=843  frame_ms[mean=7.12 p50=6.04 p95=12.42] game_ms[mean=2.10 p50=2.11 p95=2.60] render_ms[mean=4.89 p50=3.61 p95=10.39] gpu_ms[mean=5.73 p50=5.81 p95=6.09]
SCRIPT_FRAMETIME tag=capsule-only frames=1101 frame_ms[mean=5.45 p50=5.48 p95=9.62]  game_ms[mean=1.72 p50=1.73 p95=2.24] render_ms[mean=4.89 p50=5.24 p95=6.05]  gpu_ms[mean=5.16 p50=5.28 p95=5.43]
SCRIPT_FRAMETIME tag=body-b       frames=841  frame_ms[mean=7.14 p50=5.94 p95=12.37] game_ms[mean=2.15 p50=2.16 p95=2.63] render_ms[mean=5.01 p50=3.62 p95=10.46] gpu_ms[mean=5.66 p50=5.73 p95=5.99]
```

Body vs capsule-only: frame mean +31 % (p50 +9 %), GPU +11 %, game thread +0.4 ms. Against the WP-P1 first-person
number (5.94 ms mean) the body is +20 %. **FAIL** for the 10 % budget. Profiling in the cargo bay
(`sc.Player.ProfileHide`, removed afterwards): hiding the 14 follower skinned parts recovers 0.7 ms of the
1.2 ms frame delta (render thread −0.7 ms, GPU −0.23 ms); hiding the ~30 item meshes 0.1 ms; ray tracing of
skeletal meshes off 0.15 ms GPU; removing the body from distance fields / Lumen scene: no change. Kept: leader
pose for every follower whose joints all exist on the leader (all 14 here), first-person-hidden head parts drop
their render state unless they cast the head shadow (only worn head wear does), body out of DF/Lumen. Next steps
(shared skeletal builder, so out of this change): per-section bone maps (every section of every part maps all 499
joints today), merging the body parts into one skeletal mesh, skeletal Nanite at import.

## Done-when table

Screenshots `out/smoke/p2-cutlass-*.png` (gitignored); every one was inspected.

| Done when | Result | Evidence |
| --- | --- | --- |
| Cutlass loaded → first person: arms and P4-AR visible, idle | **PASS** | `02-fp-armed-idle` (hangar), `16-fp-ramp-foot-looking-up`, `17-fp-cargo-bay-armed` (rifle lower right, right hand on the grip, left hand on the handguard); `03-fp-armed-look-down` (spine pitch: weapon follows the view); head/helmet hidden, no head geometry in view |
| Running forward and strafing with matching feet (third person) | **PASS** | `05-tp-run-forward` (3.6 m/s, `walkrunstrafe` blend, weapon up), `06-tp-sprint-forward` (6.0 m/s, sprint space, weapon lowered), `07-tp-strafe-left-front` (crossing steps), `08-tp-strafe-right-behind`, `09-tp-backpedal`; feet planted and stride length matched to ground speed (blend space sampled at the capsule's ground speed and travel angle; motion blur in 08 from the trailing camera) |
| Mid-`select` and mid-`deselect` frames | **PASS** | `slomo 0.04`: `10-tp-mid-select` (rifle coming forward low, both hands), `11-tp-mid-deselect` (right arm carrying it over the shoulder), `12-fp-mid-select` (rifle rising into view), `13-fp-mid-deselect` (lowered out of view); state from `PLAYER_STATE` / `PLAYER_WEAPON` lines |
| Stowed weapon on the backpack in third person | **PASS** | `14-tp-stowed-back-left`, `15-tp-stowed-back-right`: P4-AR upright on the backpack's left side (`wep_stocked_2`, `backLeft`), S71 on the right (`wep_stocked_3`), aligned by `attach_offset_right_01/left_01` on the backpack's own `wep_stocked_attach_*_override` helpers; pistol on the right thigh, multitool/medgun on the legs |
| Walk up the ramp, open the interior door with E while armed, reach the cockpit | **PASS** | E on the ramp (`FIRST_PERSON_DOOR ... Cutlass_Ramp open=1`); armed from the bay floor down to the ramp foot and up (`16`, `17`); closed bulkhead blocks at X=76.7 (`18-fp-bulkhead-closed`, prompt "E: Open drak door bulkhead"; the rifle muzzle passes through the door, defect 2); E while armed opens it (`FIRST_PERSON_DOOR ... DRAK_Door_Bulkhead open=1`, `19-fp-bulkhead-opened-armed`, `20-tp-at-bulkhead-armed`); corridor `21`; cockpit reached at X=1219 Z=−154 (WP-P1: same) `22-fp-cockpit-armed` (canopy black as in WP-P1), `23-tp-cockpit-armed` (camera pulled in by the cockpit walls); no recovery |
| Frame time within 10 % of WP-P1 | **FAIL** | +11 % GPU, +31 % mean frame (above) |
| No asset or ship names in code | **PASS** | Names come from the `player` block / port records; the only literals are Mannequin vocabulary (fragment ids `MotionIdle`/`MotionMovement`/`select`/`deselect`, stance tags `stand`/`crouch`, parameters `MoveSpeed`/`TravelAngle`) and port flags `invis_p1`/`HeadWear` named in the contract. Ship, loadout and slot appear only in the route file and docs |
| Kit loads asynchronously, capsule-only until ready / on failure | **PASS** | `waitkit` holds the route while the HUD shows "Player body: …"; female kit failure path → "Player body unavailable (capsule only)" |
| Scripted demo (`-SCPlayerDemo`) | **PASS** | `capture_route.sh p2-demo ue/scripts/routes/player-demo.txt -SCDefaults -SCPlayerDemo=DRAK_Cutlass_Black -SCPlayerLoadout=…`: loads the ship, first person, waits for the body, runs, draws (`Selecting` 0.633 s → `Armed`), third person, stows (`Deselecting` 0.364 s → `Unarmed`), `SCRIPT_DONE`; `p2-demo-demo-0{1,2,3}-*.png` |
| Third-person toggle, E/F/V/T while armed | **PASS** | `C` / `sc.Player.View`; E used on ramp and bulkhead while armed; torch/noclip/teleport code paths untouched (WP-P1 keys share the table) |

## Known defects (most visible first)

1. **Armour, weapon and skin materials**: armour (`LayerBlend_V2`) renders untinted light grey/white in lit
   interiors, skin/hair/eyes use `M_SC_Fallback` (`HumanSkin_V2`, `HairPBR`, `Eye` families unknown). Material
   pipeline (WP-U2 area), not P2b. The weapon palettes now load from nested packages.
2. **Weapon clips into walls** when pressed against geometry (`18-fp-bulkhead-closed`): no weapon pull-back /
   lowered-at-wall state. The camera itself is swept back out of geometry.
3. **Frame cost** over budget (above): the visible skinned body parts.
4. **Female kit** not usable until the exporter fixes the female skin binds.
5. Turn-in-place is not played (yaw follows the mouse instantly; feet slide when turning on the spot); jump has
   no clip; the camera has no animated rotation (position bob only).
6. Walk-to-idle uses the idle clip directly (the `stop` run-to-idle spaces are not used): a short blend instead
   of a stopping step.
7. In third person E acts from the camera (3 m reach from 2-3 m behind the body).

## Exporter follow-ups

- **Female skins**: `Data/Characters/player_female/*` (head, undersuit, arms, core, mobiglas, scalp) carry inverse
  binds that disagree with the node reference pose (importer check, tolerance 0.002) — export the bind skeleton
  the skins were authored against (WP-R6 skeleton sidecar) or bake the skins to the exported rig.
- **Movement speeds**: the player's walk/run/sprint speeds live in DataCore actor movement parameters (not in the
  kit). Interim: nominal forward example speed of the slowest/fastest move-speed tag's blend space.
- **Stow frame**: say which frame a bone port resolves to. Observed: the backpack carries helpers named like the
  body's `wep_stocked_attach_2/3_override` joints; UE resolves the owner item's helper first. A `port.frame`
  `{child_id, helper}` (or `owner_child_id`) would make this explicit.
- **Off-hand grip per gender**: `grip_data` is raw; the female helper variant (`L_IkGripTarget_HumanFemale`) must be
  chosen by the exporter (`off_hand_grip_helper` per gender).
- **IK weights**: the clips encode IK blend weights as joint channels (`LWeapon_IKBlend` x = 0, `LHand2Aim_IKBlend`
  x = 1 in the stocked idle/locomotion). Export which IK chains a fragment enables (or the channel semantics) so
  consumers need not guess; until then off-hand IK is off by default.
- **Aim poses** (`scope_LookAimPose`, `playerMotionLookAimPoses.adb`) are not exported: spine pitch is interim.
- **Directional movement options**: annotate fragment options with the travel-angle range they serve
  (`backward_left` vs default) — the consumer cannot map frag tags to directions without names.
- **Display names**: weapons show their entity class name on the HUD; export the localised item name.

---

# Round 2 — look and correctness

Same host and loadout. Screenshots `out/smoke/p2r2-*.png` (round-1 `p2-*` kept for comparison); every one was
opened and judged. Route: `ue/scripts/routes/cutlass-armed-walkthrough.txt` (now with a Shift-strafe
measurement, a third-person prompt shot at the bulkhead and wall shots 24-27).

## 1. Body and weapon in lit interiors

**Root cause (one, not exposure, not exporter data):** every skinned part — body armour, gloves, helmet,
P4-AR, S71, pistol, gadgets — rendered with the engine's **default WorldGrid material**. The SC masters had no
skeletal-mesh usage flag; in the editor `CheckMaterialUsage` adds it silently, but the viewer runs `-game`
(`GIsEditor` false, `Material.cpp` `UMaterial::SetMaterialUsage`), so `FSkeletalMeshSceneProxy` swaps the
section to `UMaterial::GetDefaultMaterial(MD_Surface)` (`SkeletalMeshSceneProxy.cpp:250`). Evidence: round-1
log `p2-cutlass.log` had 1,307 `Material with missing usage flag was applied to skeletal mesh …` warnings, every
one on a kit rig (P4-AR 176, S71 187, helmet 60, …) or the Cutlass landing gear (48); round 2: 0. WorldGrid is a
light-grey checker: dark grey in the dim hangar (`p2-cutlass-02`), white under the cargo-bay floods (`p2-17`),
the "checker" on the helmet (`p2-23`). The data was fine: the P4-AR sidecar layers are dark (tints 0.018–0.03,
palette channels from its nested `palettes.json`), the armour layers authored (helmet/arms BaseLayer2 olive
0.08/0.08/0.045; the torso and legs use the `m_cds_medium_armor_01_01_010` variant whose BaseLayer2 is white
paint — the loadout's choice, not a defect). The cargo-bay floor hotspot is the ship's own lighting (identical in
WP-P1 `p1-cutlass-05`).

**Fixes**
- `ue/scripts/create_materials.py`: `used_with_skeletal_mesh` on every master; masters regenerated
  (`WP-U2 MATERIALS_OK`, log `ue/SCViewer/Saved/Spikes/p2r2-create-materials.log`). The regeneration now keeps
  `M_SC_LightFunction` (owned by `create_light_material.py`; the old blanket `rmtree` deleted it).
  `SCBridge.Materials.MasterAssets` asserts the saved flag on all 18 parents/variants.
- Character families (`SCMaterialDescribe.cpp`): `HumanSkin_V2`, `Eye` → Illum (base colour, DDNA, spec map,
  authored F0); `HairPBR` → Illum masked, coverage = `opacity` slot red channel (new `OpacityMap` on the surface
  masters, white default = unchanged for every other material), colour = UE's melanin model from the
  material's own `BaseMelanin`/`BaseMelaninRedness`/`DyeColor`×`DyeAmount`/`BaseTintColor`. No family falls back
  to `M_SC_Fallback` in the kit any more. Fixture `synthetic.materials.json` gained `hair_cards`/`hair_cap`.
- Hair under head wear (`SCPlayerBody.cpp`): with a `HeadWear` part worn, parts whose submaterials are all
  `HairPBR` are hidden in both views (`PLAYER_PART … hidden_under_head_wear=1` for hair, brows, lashes, beard,
  scalp). Round-1 third person showed orange hair tufts through the helmet once materials worked.

**Before / after**: `p2-cutlass-17-fp-cargo-bay-armed` → `p2r2-cutlass-17-fp-cargo-bay-armed` (black P4-AR,
dark glove, not blown out); `p2-cutlass-02-fp-armed-idle` → `p2r2-cutlass-02-fp-armed-idle`;
`p2-cutlass-23-tp-cockpit-armed` → `p2r2-cutlass-23-tp-cockpit-armed` (olive helmet, white torso, no checker);
`p2-cutlass-04/05/09` → `p2r2-cutlass-04/05/09` (coherent armoured character in the hangar). WP-P1 ships:
`p1-cutlass-05-cargo-bay` vs `p2r2-p1-cutlass-05-cargo-bay` (the WP-P1 route re-run; the body now sets the walk speed, so the view is 35 cm further forward (X −1025.8 vs −1060.4)) — same surfaces, lighting and exposure; all 17 WP-P1 shots re-captured as `p2r2-p1-cutlass-*`.

## 2. Weapon through walls and doors

Engine first-person rendering (UE 5.8 source: `PrimitiveComponent.h` `EFirstPersonPrimitiveType`,
`CameraComponent.h` `FirstPersonScale`, `SceneView.cpp:745` `WorldToFirstPerson` = view-space uniform scale
toward the eye, `MaterialTemplate.ush` `ApplyMaterialFirstPersonTransform`). Each first-person frame probes from
the eye to the held weapon's box corners (union of all its primitives in its anchor frame) and the hand joints;
the scale is the largest that keeps every probe 1.5 cm in front of the first hit (min 0.04), the near plane
follows (floor 0.2 cm). Only while scale < 1 are the kit's visible primitives `FirstPerson` (they cast no
shadows by engine rule, `PrimitiveSceneProxy.cpp:635`), and the hidden head stops casting its head shadow
(the scaled body sits inside the head; MegaLights ray-traced shadows would hit the helmet). Away from walls the
body is exactly round 1 (world-space, full shadows). Rejected: the engine's `WorldSpaceRepresentation` pairing
needs duplicate components (≈2× skinned parts) and the first-person G-buffer bit, which requires
`r.AllowStaticLighting=False` (project-wide shader rebuild); a lowered-weapon pose cannot retract a 70 cm rifle
from a wall 6 cm ahead of the eye.

Evidence: `p2-cutlass-18-fp-bulkhead-closed` (muzzle through the door) → `p2r2-cutlass-18-fp-bulkhead-closed`,
`18c-…-look-down`; nose to the cockpit wall `p2r2-cutlass-24-fp-nose-to-wall`, `25-…-look-down`, other side
`27-fp-nose-to-other-wall` (scale 0.06); third person unaffected `26-tp-nose-to-wall`. Log
`PLAYER_FP_PRIMITIVES on=1 … scale=0.101` at the bulkhead, `0.511` / `0.065` at the walls.
Test `SCBridge.Player.FirstPersonScale` (pure rule) and in `KitRigAndWeapons`: no wall → scale 1, no FP
primitives; a cube 10 cm ahead of the capsule → scale 0.26, FP primitives on, camera scale enabled; third
person → scale 1, off.

## 3. Small items

- **Third-person E** reaches from the camera joint along the view (`ASCFirstPersonCharacter::InteractionOrigin`);
  `p2r2-cutlass-18b-tp-bulkhead-prompt` shows "E: Open drak door bulkhead" in third person at the door.
- **Stop (run-to-idle)**: not done. The kit has `MotionIdle` options tagged `forward_hip,stop` and
  `backward_hip,stop` (blend spaces `run2idle_fhip/bhip`); which applies depends on the gait phase at the stop
  (which hip leads), which the export does not state. Choosing by phase heuristics would be guessing.
- **Turn-in-place**: not done (time went to the review fixes); the `steprot_*` combined spaces now parse and
  select correctly, which is the data it needs.

## Codex review of f60b36b — disposition

| # | Item | Status | Evidence |
| --- | --- | --- | --- |
| 1 | Failed kit leaks actors | **fixed** | `FailPlayerKit` hides, detaches and destroys the kit actor and drops `KitData`/`KitClips` on every failure; the actor is hidden right after import; attach failures fail the kit too. GPU: female kit (`-SCPlayerPackage=out/fps/player-p4-female.tar`), 4 attempts → `PLAYER_KIT_RELEASED live_vehicles=1` each time (the ship only), log `p2r2-female.log` |
| 2 | Ready with missing clips | **fixed** | `FSCPlayerAnimator::FragmentReady`: slots need every clip of draw, holster and armed idle (`PLAYER_SLOT_DISABLED`); the unarmed idle (FP and TP) must be complete or the kit fails (capsule only). Male kit: 8 slots, 0 disabled |
| 3 | Root part hidden as head | **fixed** | the root-port clause is gone: port flags + head-subtree influence only (male root: influence 0.91, still hidden) |
| 4 | Nested accessory parenting | **guarded** | the kit's nested entries all name the assembly root (`EntityClassDefinition.<weapon>`) and carry no ids; an entry naming a sibling now logs `nested parent … known limit` and stays on the root. Known limit |
| 5 | Sequencing / exit times | **guarded** | `PLAYER_KIT_DIAGNOSTIC` for fragments with several entries on a layer or exit/start times; none in the male kit (no diagnostics logged). Start times are honoured for additive layers |
| 6 | Joint masks, base weight | **guarded / fixed** | all 2,934 kit layers export `joint_mask: null`; the exporter reads `Animation@jointMask` (`motion.rs:129`) — whether ADBs carry masks elsewhere is an exporter follow-up. Base-layer `weight` now scales against the bind pose (1 in the kit) |
| 7 | Additives on the phase clock | **fixed** | sources carry a seconds clock; `LayerSeconds` = start + clock × speed, looped or held; test with a 2 s additive on a 0.5 s base |
| 8 | Combined spaces | **fixed** | `components`/`selectors`/example `component` parsed; the choosing selector picks the first covering component (exported order), only its faces interpolate; unevaluable selectors make the fragment unplayable with a diagnostic. Kit: 6 combined spaces, all evaluable (nameless 1-D selector of `steprot_stocked_alerted_crouch` resolves to its only dimension). Test: overlapping components |
| 9 | Sprint speed while strafing | **fixed** | `FSCPlayerAnimator::CanRun` (fast tag exists and its space covers the intended direction, from the movement input) drives both the tag and `MaxWalkSpeed`. Measured (route, 2 s each, P4-AR): Shift+A 680 cm = 3.40 m/s (walk 3.57 incl. acceleration), Shift+W 1114 cm = 5.57 m/s (run 6.01) |
| 10 | Queued swap clocks | **fixed** | the new draw starts with the frame time left after the holster on both clocks (`GetOvershoot`, `PlayOneShot(StartTime)`); unit test with a 0.5 s frame across a 0.4 s holster; kit test: one-shot time = state clock within 1 ms with 0.25 s frames |
| 11 | Degenerate faces | **fixed** | closest point over all edges (ties: shortest edge); collinear A,B,C sampled at C → C; midway B–C → B/C; coincident A=B → C reachable |
| 12 | Weak tests | **fixed** | real attach parent/socket asserted for stow and hand; camera at the camera joint; additive with a non-identity terminal key against an independent axis-angle expectation (full and half weight) |
| 13 | TravelAngle seam | **measured, fixed** | 13 kit spaces span ±π; 3 differ across it (`nw_neutral_stand_runstrafe` 1.36, `nw_neutral_crouch_movestrafe` 1.61, `stocked_alerted_crouch_movestrafe` 1.49 summed clip-weight difference): straight backward travel could alternate between foot-lead clips frame to frame. `StickySeamAngle` keeps the side used last frame within 0.35 rad of π (tests) |

## 4. Frame cost

Change: per-section bone maps in the shared skeletal builder (`SCSkeletonBuilder.cpp`): each render section
lists only the joints that weight its vertices (was the whole 499-joint skeleton per section); required/active
bones stay complete (sockets). Consumers that read influences map through the section (`SubtreeInfluence`;
the skinned-collision refit already did). Rig test updated; WP-P1 suites and the Cutlass GPU route (landing-gear
rigs) pass.

Final route, same cockpit view, A/B/A with `sc.Player.Body 0`:

```text
SCRIPT_FRAMETIME tag=body-a       frames=830  frame_ms[mean=7.23 p50=6.00 p95=12.46] game_ms[mean=2.10] render_ms[mean=5.01 p50=3.55] gpu_ms[mean=5.71 p50=5.77]
SCRIPT_FRAMETIME tag=capsule-only frames=1106 frame_ms[mean=5.43 p50=5.44 p95=9.58]  game_ms[mean=1.65] render_ms[mean=4.82 p50=5.21] gpu_ms[mean=5.13 p50=5.22]
SCRIPT_FRAMETIME tag=body-b       frames=836  frame_ms[mean=7.18 p50=5.89 p95=12.38] game_ms[mean=1.99] render_ms[mean=4.99 p50=3.54] gpu_ms[mean=5.62 p50=5.66]
```

Body vs capsule-only: mean +33 % (p50 +9 %, GPU +11 %, game thread +0.4 ms) — **no measurable change from
round 1** (+31 %, run-to-run noise). **FAIL** for the 10 % budget. The bone-map change is kept (correct, smaller
uploads) but the cost is elsewhere: the mean is dominated by a bimodal frame-time distribution (p50 6.0 vs p95
12.4 ms with the body; render-thread p50 3.6 vs p95 10.4 ms suggests alternating fast/slow frames) rather than a uniform
per-part cost. Next: an Unreal Insights capture of the body-on frames (render thread: skin-cache dispatch and
the 14 follower proxies' dynamic-data updates), then merging the body parts into one skeletal mesh.

## Tests (round 2)

```bash
ue/scripts/build.sh SCViewerEditor                                   # Result: Succeeded
UnrealEditor-Cmd … -script=ue/scripts/create_materials.py -nullrhi … # WP-U2 MATERIALS_OK
UnrealEditor-Cmd … -script=ue/scripts/test_importer.py -nullrhi …    # exit 0, 16/16 synthetic suites
UnrealEditor-Cmd … -script=ue/scripts/test_player.py -SCPlayerTests \
  -SCPlayerPackage="$PWD/out/fps/player-p4-male.tar" -nullrhi …      # exit 0, WP-P2 HEADLESS_OK, 5/5
for p in gladius scorpius cutlass; do UnrealEditor-Cmd … -script=ue/scripts/test_first_person.py \
  -SCPackage="$PWD/out/dec-r5b-$p" -SCFirstPersonTests -nullrhi …; done   # 3× exit 0, P1_VERIFY passed=1
UnrealEditor-Cmd … -script=ue/scripts/test_importer.py -SCPackage="$PWD/out/dec-r5b-cutlass" \
  -SCVerifyTar="$PWD/ue/SCViewer/Saved/Import/p1-cutlass.tar" -nullrhi …  # 18/19: FolderTarImport fails on the
                                                                     # pre-existing LIGHT_VERIFY mismatch (WP-P1 doc)
```

`P1_VERIFY`: gladius `soft=3 refitted=3 … passed=1`, scorpius `soft=0 … passed=1`, cutlass `soft=2 refitted=2 …
passed=1` (skinned collision refit reads the new per-section bone maps). Player: `BlendWeights` (synthetic grid,
degenerate faces, selectors, seam, 20 kit grids, seam measurement, 6 combined spaces evaluable),
`AdditiveLastFrame` (+ terminal key, layer clocks), `WeaponStateMachine` (+ overshoot), `FirstPersonScale`,
`KitRigAndWeapons` (+ real attachment, camera, swap clocks, wall scale).

## Done-when (round 2)

| Done when | Result | Evidence |
| --- | --- | --- |
| Armed FP in the cargo bay, at the bulkhead, in the cockpit: weapon and gloves with believable colours | **PASS** | `p2r2-cutlass-17`, `-18`, `-22`, `-24`: black/dark-grey P4-AR with its metal charging handle, dark gloves |
| Third person: tinted armour, no checker/fallback | **PASS** | `p2r2-cutlass-04/05/09/14/23/26`: olive helmet and arms, white torso/legs (the loadout's variant), no WorldGrid; 0 usage-flag warnings, 0 fallback-family warnings for the kit |
| WP-P1 ships look the same | **PASS** | `p2r2-p1-cutlass-05-cargo-bay` vs `p1-cutlass-05-cargo-bay`; the masters' graphs are unchanged for ships (usage flag; `OpacityMap` white default) |
| Nose to wall / closed bulkhead: no weapon cut by the wall (FP) | **PASS** | `p2r2-cutlass-18`, `-18c`, `-24`, `-25`, `-27` |
| Body shadows right, third person unaffected | **PASS (limit)** | away from walls identical to round 1; while a wall is in reach the body casts no shadow (engine first-person rule); `-26` third person |
| TP E from the head | **PASS** | `p2r2-cutlass-18b-tp-bulkhead-prompt` |
| Stop / turn-in-place | **not done** | see §3 |
| Frame time within 10 % | **FAIL** | §4: +33 % mean, +9 % p50, +11 % GPU |

## Known defects after round 2 (most visible first)

1. **Armour reads flat and too bright in strong light**: the Layered master has no material-level (TexSlot3)
   macro normal, wear layers or HAL control (WP-U2 open item, shared with ships; adding it changes ship looks),
   and metallic layers with authored gloss 1 mirror the bright interiors (white patches on the helmet in `-23`).
   *Round 3: macro normal, HAL AO/luminance and the game's gloss mapping are in; still blown out in the Cutlass
   interior — see "Round 3" below.*
2. **No body shadow while a wall is in reach** (first-person primitives cast none). A proper fix is the
   engine's world-space-representation pairing with `r.AllowStaticLighting=False` (project-wide).
3. **Frame cost** (§4).
4. Third-person camera in tight spaces is pulled into the helmet (`-18b`, `-23`): presentation only.
5. Turn-in-place and run-to-idle stops are not played; jump has no clip.
6. Female kit unusable (exporter skin binds, unchanged).

## Exporter follow-ups added in round 2

- `joint_mask`: confirm where Mannequin ADBs author joint masks (currently `Animation@jointMask`, null for all
  2,934 kit layers).
- `stop` fragments: state which gait phase each `forward_hip` / `backward_hip` option serves.
- Character customization (`root_entity.character.customization`: hair dye, `BodyColor`, `EyeColor`) is not
  applied; the viewer uses each material's own authored values.

---

# Round 3 — armour look (2026-11-10)

Defect 1 above. The rules, their shader sources, tests, commands, frame times and the Codex cross-review
disposition are in [WP-U2.md](WP-U2.md) ("Macro surface, HAL and the gloss convention"); the masters are shared
with ships, so the contract lives there. Same host, loadout and route as round 2; shots `out/smoke/p2r3-cutlass-*`
(position-matched to `p2r2-cutlass-*` within 7.8 cm), before | after sheets `out/smoke/armour-r3-*.png` and
`wp-u2/armour-*-before-after.jpg`.

| Part of the defect | Result | Evidence |
| --- | --- | --- |
| No macro normal | **fixed** (TexSlot3 over the blended layers, macro gloss) | hangar 04/14: panel relief on helmet and limb plates, backpack folds |
| No HAL control | **fixed for AO and luminance**; hue shift not implemented (needs the game's `HueLUT`) | hangar 04: shaded seams and recesses, deeper olive plates |
| Too bright / mirror patches in strong light | **not fixed** | bulkhead 20, cockpit 23: still blown out; plates mirror the lights more sharply under the game-exact gloss. Cause: over-bright Cutlass interior lights (open) |
| No wear layers | LayerBlend: **not done** (per-instance at runtime; pristine = none). HardSurface static wear: **done** 2026-11-10 (WP-U2.md "Static wear") | `wp-u2/wear-*.jpg` |

Side effect on ships (reported, not tuned): every lit master now maps gloss the way the game does, so ship
exteriors are glossier than before and than the in-game references until static wear is drawn
(`wp-u2/gladius-gloss-before-after.jpg`). Cutlass interior shots are otherwise unchanged in content.
Frame time: about +0.4 ms GPU (+8 %) in the cockpit view with and without the body (WP-U2.md); the body's own
cost (§4) is unchanged. Tests: `test_importer.py` 17/17, `test_player.py` 5/5.

## Known defects after round 3 (most visible first)

1. ~~Armour and interiors blow out to white in the Cutlass (bulkhead, cockpit)~~: cause fixed 2026-11-10 —
   the exterior under-hull floods were exported 5.46 m off, inside the cockpit and cargo bay (WP-U3b.md §12;
   cockpit 25 320 → 197 lux at the player). Still bright and not compared with the game: white door-frame
   strips at the bulkhead, helmet and armour in the third-person cockpit view, the hangar floor and ramp
   under the viewer's own hangar panels.
2. ~~Ship paint too glossy / too clean~~: fixed 2026-11-10 (HardSurface static wear, palette finish gloss and
   specular, DDS colour encoding, energy split; WP-U2.md). Dirt and per-instance wear are still not drawn.
3. Items 2–6 of the round-2 list, unchanged.
