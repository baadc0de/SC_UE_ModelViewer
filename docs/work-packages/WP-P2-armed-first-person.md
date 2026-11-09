# WP-P2 — Armed first person: real player body, FPS weapon, stash / unstash

**Agents:** P2a exporter — Codex (cargo lock). P2b Unreal — Opus 5.5 (UE lock).
**Depends on:** WP-P1 (`ASCFirstPersonCharacter`, door interaction, spawn points),
WP-U3 (`USCRigComponent`, clip sidecars), WP-R3 (Mannequin fragments in the catalogue).
P2b starts when P2a's package contract is on disk.

## Goal
In first-person mode the player is the real Star Citizen player character: the default
male (or female) body in a real player loadout, camera in the head, an FPS weapon (first
target: the Behring P4-AR, `behr_rifle_ballistic_01`) held with the game's own
animations, walk / run / strafe with the game's locomotion blend spaces, and a key that
stashes the weapon on the back and draws it again using the game's `deselect` /
`select` animations. No shooting. It is the same character that WP-P1 walks through a
loaded ship with: collision, E on doors, flashlight, noclip and teleport keep working
while armed and while stowed.

"Convincing" means: feet match ground speed, the weapon sits in the hands and on the
back where the game puts it, and nothing is positioned by eye. Every name and number
comes from DataCore / P4k.

## How Star Citizen does it (research, LIVE 4.10.2 — verify, do not trust blindly)

Unified first / third person: there are no separate arms. One full-body rig, camera on
a head joint, weapon on a hand joint, first person hides head parts.

- **Player entity** `EntityClassDefinition.Player`: skeleton
  `objects/characters/human/male_v7/export/bhm_skeleton_v7.chr` (female:
  `female_v2/export/bhf_skeleton_v2.chr`), `actionController =
  Animations/Mannequin/ADB/playerControllerDefs.xml`, `animDatabase3P =
  Animations/Mannequin/ADB/playerAnims.adb` (includes the `Players/*.adb` sub-databases
  by scope), tags in `PlayerTags.xml`, fragment ids in `PlayerActions.xml`.
- **Skeleton joints** (all present in the exported rig): camera joint `HeadCam` (child of
  `Head`, keyed by locomotion clips, which is the camera bob; the name is a literal in
  `StarCitizen.exe` and the Player entity's audio listener uses the same joint);
  `RightWeaponBone` / `LeftWeaponBone` under the hands; stow joints
  `wep_stocked_attach_2_override`, `wep_stocked_attach_3_override`,
  `backpack_attach_1_override` under `Spine3`, `wep_sidearm_attach_override` under the
  right thigh; IK joints `LWeapon_IKTarget`, `LWeapon_IKBlend`, `RWeapon_IKTarget`,
  `RWeapon_IKBlend`. Root motion is the translation of the root joint `World`.
- **Item ports carry everything about attachment** (`SItemPortDef` in
  `SItemPortContainerComponentParams`):
  - body `body_01`: `weapon_attach_hand_right` → `SItemPortDefAttachmentImplementationBone`
    helper `RightWeaponBone`, flags `select usegripdata`; `weapon_attach_hand_left` →
    `LeftWeaponBone`.
  - backpack `rrs_combat_heavy_backpack_03_01_01`: `wep_stocked_2` → helper
    `wep_stocked_attach_2_override`, `ItemOffsetHelperName = attach_offset_right_01`,
    `Extension = SItemPortDefExtensionFPS { SelectTag = backLeft }`; `wep_stocked_3` →
    `wep_stocked_attach_3_override`, `attach_offset_left_01`, `SelectTag = backRight`.
    Undersuits and torsos define the same ports for the no-backpack case; legs define
    `wep_sidearm` → `wep_sidearm_attach_override`.
  - port flags include `invis_p1` (not drawn in first person) and `HeadWear`.
  - The weapon has matching helpers `attach_offset_right_01` / `attach_offset_left_01`:
    when stowed, that weapon helper coincides with the port's bone (plus the port's
    `Offset`). In hand, the weapon's `root` coincides with the hand port bone.
- **Weapon** `behr_rifle_ballistic_01`: `AttachDef` Type `WeaponPersonal`, SubType
  `Medium`, Size 2, Tags `stocked behr_rifle_ballistic_01 rifle` — these are the Mannequin
  tags the weapon contributes. Helpers in its skeleton: `root`, `L_IkGripTarget`,
  `R_IkGripTarget` (+ `_HumanFemale` variants), `magAttach`, `ADS_align`,
  `attach_offset_*`. `SEntityComponentCarryableParams` / `gripData` name the off-hand
  grip. Our decomposed export of it already works
  (`starbreaker entity export behr_rifle_ballistic_01 … --kind decomposed --format glb`):
  `assembly_kind: "fps_weapon"`, `weapon_assembly.anchors[]` has every helper, magazine
  child on `magAttach`, nine weapon clips.
- **Player loadouts** `Data\Scripts\Loadouts\Player\DefaultLoadouts\*.xml` (CryXmlB).
  `PVE_medium_behr_rifle_gmni_rifle.xml` is self-contained and carries the P4-AR in the
  backpack's `wep_stocked_2`, a second rifle in `wep_stocked_3`, a pistol in `wep_sidearm`.
- **Mannequin fragments** (first fragment in file order whose `Tags` are all in the
  current state and whose `FragTags` match wins; `AI`-tagged fragments are not the
  player's):
  - `Players/playerSelectDeselect.adb`: `select` / `deselect`; state tags `stocked`,
    `stand` / `crouch` / `prone`, `FP`; frag tags from the port's `SelectTag`
    (`backLeft`, `backRight`, `front`) and `carry_base`. Example: `stocked+FP+stand` →
    layer `stocked_alerted_stand_select_01_fp_add`; `stocked+stand` →
    `stocked_alerted_stand_select_01` + `…_select_01_add` at speed 2.2.
  - `Players/playerMotionAnims.adb`: `MotionIdle`, `MotionMovement`, turn fragments; frag
    tag `stocked_base` → blend space
    `2D-BSpace_StockedBase_Alerted_Stand_WalkRunStrafe_FwdRgt` (and `_BckLft`); unarmed →
    `2D-BSpace_NW_Neutral_Stand_WalkRunStrafe_Q1..Q4` etc.; scopes select sub-databases
    (`scope_LookAimPose` in `playerMotionLookAimPoses.adb`).
- **Clips**: locomotion in `Data\Animations\Characters\Human\male_v7\weapons\{stocked,
  no_weapon}\locomotion\stand.dba` (also `crouch.dba`, `jump.dba`); the current
  select / deselect clips are loose CAFs in
  `…\male_v7\weapons\select_deselect_v2\stocked\`. The skeleton's `.chrparams` lists the
  search roots (`$TracksDatabase …/*/*.dba`, `#filepath`, `*/*.caf`, `*/*.bspace`).
  In the full-body `stocked_alerted_stand_select_01` the clip itself moves
  `RightWeaponBone` from the back to the hand, so the weapon stays on the hand port for
  the whole select and only changes port when a deselect ends. `_add` clips decode as
  deltas that end at identity.
- **Blend spaces** `…\male_v7\bspace\**\*.bspace` are CryXmlB `ParaGroup` files:
  `<Dimensions>` (`MoveSpeed`, `TravelAngle`, with min / max / cells), `<ExampleList>`
  of `<Example AName=clip Alias=… [SetPara0 SetPara1 PlaybackScale
  UseDirectlyForDeltaMotion*]>`, `<Blendable>` quads `<Face p0 p1 p2 p3>` by alias. An
  example without `SetPara*` takes that parameter from its clip's root motion (speed =
  root travel / duration, travel angle = direction of travel in the root frame).
  StarBreaker has no parser for these yet.
- Upstream already exports dressed characters: `starbreaker armour export <out>
  --gender male --kind glb|package --loadout '<p4k xml>' [--body-anim <caf|dba>
  --body-anim-clip <stem>]` (`cli/src/armour.rs`,
  `crates/starbreaker-3d/src/character/{package,clips,outfit,loadout,items}.rs`). Its
  package path writes the Blender flavour (`write_decomposed_export_blend`); ours needs
  the GLB decomposed writer (`decomposed::write_decomposed_export`, crate-private). It
  bakes one body clip and **skips non-skinned loadout items** (backpack, weapons,
  magazines: "no Male .skin, skipped").

## P2a — exporter (Codex)

Submodule branch `scviewer/player-kit` (no trailers there), then `scviewer-core`,
`scviewer-cli`, `scbridge-ffi`. Tests first for each parser.

1. **Character as a GLB decomposed package.** Default wearer of a gender in a loadout
   XML → the same contract v2 package vehicles use (scene.json first, GLB mesh assets,
   material sidecars, textures), `assembly_kind: "character"`. Root = body skeleton with
   all joints (socket-only joints included); children = skinned parts.
2. **Bone-attached loadout items are exported, not skipped.** Category fix: any loadout
   entry whose port uses `SItemPortDefAttachmentImplementationBone` becomes a child
   entity carrying a `port` record `{ name, owner_entity, flags[], types[], attach_bone,
   offset, item_offset_helper, select_tag }`. Rigid items (backpack) are plain children;
   `WeaponPersonal` items nest the existing `fps_weapon` assembly (anchors, magazine and
   attachments from the loadout XML, the weapon's own clips). Every skinned part also
   records its port name and flags so first-person hiding is data-driven.
3. **`player` section in scene.json**:
   - `gender`, `skeleton`, `hand_ports[]` (ports flagged `select`, with bone), `camera_bone`
     (state in the contract doc where it comes from; if only the executable names it,
     keep one named constant with that evidence), `root_motion_bone`.
   - `weapons[]`: per stowed weapon — entity, child id, stow port, mannequin tags from
     `AttachDef.Tags`, grip data (off-hand grip helper for this gender), size / subtype.
   - `motion.states[]`: Mannequin queries resolved in Rust for the player's own states:
     unarmed and one per weapon tag set, × `stand` (and `crouch` if the data resolves) ×
     first person / third person. Each holds the resolved fragments `MotionIdle`,
     `MotionMovement` (every frag-tag variant it needs, e.g. forward-right and
     backward-left), turn-in-place, `select`, `deselect` (per stow port `SelectTag`), with
     layers in order: `{ clip | blend_space, additive, loop, speed, blend_in, blend_out,
     weight, joint_mask? }`. The resolver is a tested function implementing the ADB
     matching rule; no fragment or clip name appears in code.
   - `blend_spaces[]`: dimensions, examples `{ clip, alias, params[], playback_scale }`
     with unset parameters computed from root motion, faces as example indices.
4. **Clips**: every clip referenced by the above is exported once as a sidecar in the
   existing clip JSON format, bound to the body skeleton, plus per-clip `additive`
   (from the asset's own flag or encoding, not from the file name), `cyclic`,
   `duration`, and `root_motion` (start / end transform of the root-motion joint).
   Resolve names through the chrparams search roots (DBA entries and loose CAFs).
5. **API**: `scviewer-core` `export_player(PlayerRequest { gender, loadout })` with the
   D10 raw cache (key includes gender, loadout path, P4k identity), FFI
   `scbridge_export_player` + header + `examples/header`, CLI
   `scviewer-cli player export <out.tar> [--gender] [--loadout]` and `player loadouts`
   (lists `DefaultLoadouts` with their weapons). Default loadout when none is given: the
   first default loadout, in name order, that puts a `WeaponPersonal` item in a stow
   port — chosen by data, not by file name.
6. **Docs**: `vendor/StarBreaker/docs/decomposed-export-contract.md` and
   `docs/03-unreal-export-design.md` gain the character / player sections in the same
   change.

Done when: `scviewer-cli player export out/fps/player-male.tar` succeeds in under 60 s
cold; `scripts/verify_wp_p2.py` (reads with `python3 -I`) asserts scene.json first, a
skinned body with the joints named in `player`, the backpack and P4-AR as children with
port records, ≥ 1 resolved `select` and `deselect` per stowed weapon, blend spaces whose
example parameters lie inside their dimensions, every referenced clip present, at least
one additive clip flagged; the same for `--gender female`; `cargo test` green for the
new parsers (bspace, loadout ports, fragment resolver, root motion); vehicle exports
byte-identical before / after (Gladius package hash).

## P2b — Unreal (Opus 5.5)

All in the Runtime module; no editor-only dependency.

1. **Loading**: `USCViewerSubsystem` requests the player kit through the bridge when
   first-person mode is first entered (async, cached); WP-P1 behaviour stays available
   until it is ready and if it fails. Setting / command line picks gender and loadout.
2. **Body**: a component (or actor) owned by `ASCFirstPersonCharacter` builds the rig:
   one leader `USCRigComponent` on the body skeleton, skinned parts following the
   leader pose, rigid children on their port bones, weapons as nested assemblies.
   Feet on the capsule floor; body yaw follows control yaw. No collision on body parts.
3. **Animator** — a plain evaluator class with unit tests, writing `LocalPose`:
   - base layer from `motion.states`: idle and 2-D blend space sampled by (ground speed,
     travel angle relative to facing), time-normalised so examples stay in phase,
     cross-fades between states with the exported blend times;
   - additive layers (`local = base ∘ delta`; confirm the composition order visually);
   - one-shot `select` / `deselect` layers;
   - view pitch: aim poses if P2a exported them, otherwise pitch distributed over the
     spine chain so hands and weapon follow the view — documented as interim;
   - off-hand two-bone IK to the weapon's grip helper while armed;
   - capsule moves by `UCharacterMovementComponent`; the root-motion joint is pinned so
     the mesh does not run away. Walk / run speeds come from the blend-space examples
     (replace WP-P1's fixed 400 / 650 while a body is present).
4. **Camera**: at `player.camera_bone` in first person (position from the animated
   joint, orientation from control rotation plus the joint's animated offset); near
   clip small enough for the arms. Parts on ports flagged first-person-invisible or
   under the head port are hidden from the first-person view but keep casting shadows.
   A third-person orbit toggle (`C`) shows the whole body; it is also how the stowed
   weapon gets verified.
5. **Weapon state**: `Unarmed → Selecting → Armed → Deselecting → Unarmed`. `1` / `2` /
   `3` select the stowed weapons in port order, the active weapon's key or `H`
   holsters. On select start the weapon moves from its stow port to the hand port and
   the resolved `select` fragment plays; on deselect end it moves back to the stow
   port, aligned by `item_offset_helper`. Interaction (E), flashlight, noclip and
   teleport are unaffected. HUD shows the weapon name and state.
6. **Automation**: console `sc.Player.Weapon <index|none>`, `sc.Player.View fp|tp`,
   command-line equivalents for `capture_viewer.sh`, and a scripted walk
   (`-SCPlayerDemo`) that loads a ship, enters first person, runs, draws, stows.
7. **Tests** (nullrhi automation): package import builds the rig with all `player`
   joints; blend-space weights are non-negative and sum to 1 across the whole
   dimension grid; an additive layer at its last frame leaves the base pose unchanged;
   weapon attachment parent is the hand bone when armed and the stow bone when stowed;
   state machine ignores input mid-transition.

Done when (GPU, screenshots in `ue/verification/WP-P2.md`): Cutlass Black loaded →
first person at the ramp: arms and P4-AR visible, idle; running forward and strafing
with matching feet (third-person capture); mid-`select` and mid-`deselect` frames;
stowed weapon on the backpack in third person; walk up the ramp, open the interior
door with E while armed, reach the cockpit; frame time within 10 % of WP-P1; no asset
or ship names in code.

## Out of scope
Firing, reload, ADS, recoil, zero-g / EVA, prone, ladders, seats, character
customisation (DNA), face animation, helmet HUD. Crouch, jump clips, turn-in-place and
the sidearm are stretch goals that the data model must not preclude.
