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
