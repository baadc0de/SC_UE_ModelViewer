# WP-O1 — Split into StarBreaker core, the viewer and the UnrealCitizen game

The repository was renamed `baadc0de/UnrealCitizen` (2026-11-22). One Unreal project and one plugin
(`ue/SCViewer` + `ue/Plugins/SCBridge`) grew both a model viewer and game-like features (mobiGlas, cargo
operations, music, Ship Link, Garage, cooking). This package separates them into three parts.

## Target

| Part | Where | What it is |
|---|---|---|
| Core | `vendor/StarBreaker/` (submodule), `crates/` | The exporter and our Rust (`scviewer-core`, `scbridge-ffi`, `scviewer-cli`). Unchanged by this WP. |
| Shared UE code | `plugins/SCBridge/` | Everything both apps run: the ship/environment actors and their rendering (materials, lights, look, exposure, SSDO, FX), doors, door and lift panels, lifts, seats, ladders, first person, the interaction popout, the RPC core. Works from UE assets. The runtime importer (FFI client, package reader, import job, processed cache) becomes its own module, `SCBridgeImport`, in O1-4, so a build can leave the p4k reader out. |
| Viewer | `viewer/` (`SCViewer.uproject`) | Loads an environment and a vehicle, from the command line or from a picker. Runtime import with the local cache; no cooking, no mobiGlas, no music, no cargo. |
| Game | `game/` (`UnrealCitizen.uproject`) | A normal, cookable Unreal project. Runs a scenario definition; carries the mobiGlas, cargo, music, Ship Link and Garage. Never reads `Data.p4k`: a prep step (editor) imports what a scenario needs as UE assets first. |

Decisions taken with the owner (2026-11-22):

- **Pure move first.** A move-only change lands quickly; open branches rebase onto it (git follows the renames),
  and the code split comes after in its own changes.
- **Ship Link and Garage are game-only.** The viewer keeps door pads and panels (a pad's own lock button too) but
  has no remote door control and no garage kiosk.
- **The game loads only UE assets.** The prep step is an editor step (today's cook path); the packaged game has no
  `scbridge` library and never reads p4k or package tars.
- **One shared plugin.** Both projects load `plugins/SCBridge`; the game adds its own modules on top.

## Stages

### O1-1 — Move (PR #40, merged)

Move only; the contents change only where a path changed.

| From | To |
|---|---|
| `ue/Plugins/SCBridge/` | `plugins/SCBridge/` |
| `ue/Plugins/NVIDIA/` (gitignored download) | `plugins/NVIDIA/` |
| `ue/SCViewer/` | `viewer/` |
| `ue/scripts/` | `viewer/scripts/` (the cook scripts moved on to `game/` in O1-2) |
| `ue/verification/`, `ue/README.md` | `viewer/verification/`, `viewer/README.md` |

Paths in scripts, `.gitignore`, crates' tests and docs follow. Nothing is renamed: the plugin stays `SCBridge`
and the project `SCViewer`.

Rebasing a branch onto it: `git -c merge.directoryRenames=true rebase main` puts files a branch added under the old
directories into the new ones. A worktree's ignored build output (`Binaries`, `Intermediate`, `DerivedDataCache`,
`Saved`) stays at the old place; move it with the project or let the next build recreate it.

### O1-2 — Split the shared plugin (this PR)

The game-only code left `SCBridge` for the new `game/` project (`UnrealCitizen.uproject`, modules `UnrealCitizen`
and `UnrealCitizenEditor`):

- cargo operations (`Cargo/`: drones, tug, jobs, beacons, flight paths, spawning, discovery, scenarios, the cargo
  subsystem and its console commands), the mobiGlas and the world callouts (`UI/Cargo`), Music (`SCMusicSubsystem`,
  `UI/Music`), the Ship Link app and its routes (`UI/ShipLink`, `SCShipLinkRpc`), Garage (`SCGarage*`, `UI/Garage`),
  and their tests → `game/Source/UnrealCitizen`;
- `SCBridgeEditor` (the cook library, Keep as assets, the Interchange baseline) → `game/Source/UnrealCitizenEditor`;
- `Config/Cargo`, the drone art install (`Content/SciFi_Drone_4`, ignored), the cook / packaging / cargo-art
  scripts, the cargo and mobiGlas routes and the cargo, Ship Link and Garage RPC drivers → `game/`;
- the cargo and Garage settings → `UUCSettings` (UnrealCitizen; saved in `GameUserSettings.ini` like
  `USCViewerSettings`, under the new class's section: a fleet saved before this change is not carried over).

Kept in the shared plugin, because the kept features use them:

- the cargo **grid model** (`SCCargoTypes`, `SCCargoRecords`, `SCCargoPlanner` with its dig-out and transfer plans,
  `USCCargoGridComponent`): package data the reader fills and environment access (`FSCEnvironmentAccess`) opens;
- the Ship Link **subsystem** (owners, roles, the door policy, door orders, lights) and `SCShipLinkDoors.cpp`: a door
  pad's lock button and the doors' refusals use them; only the app and its routes are the game's;
- the HUD design tokens and paint helpers, now `SCHudStyle` / `SCHudPaint` (`Public/UI/`), used by the interaction
  popout, the mobiGlas and the callouts;
- which drawn panels are garage kiosks and the aim at one (`ASCVehicle::IsGarageKiosk`, `AimGarageKiosk`): import
  data; what a kiosk does is the game's.

Where the shared code called into game features it now offers an extension point the game registers with at module
startup (`UnrealCitizen.cpp`):

| Extension point | What UnrealCitizen registers |
|---|---|
| `SCViewerShell::SetMenu`, `SetMenuOpen` / `IsMenuOpen` (the F1 menu; `ASCFirstPersonCharacter::bMenuOpen`, `EAction::Menu`, log `FP_MENU` / `VIEWER_MENU`) | the mobiGlas |
| `SCViewerShell::AddOverlay` (layers over the view in `SSCViewerRoot`) | the mobiGlas and the world callouts on one model |
| `SCViewerShell::AddInteraction` (`ISCInteractionProvider`: prompt rows and E, asked before the shared kinds) | the garage kiosk ("Open Garage") |
| `SCRpcExtensions::Add` (`ISCRpcExtension`: routes, state sections, activity, anomalies, catalogue, `/wait` conditions) | cargo (`/cargo/*`, `until=job`), Ship Link (`/ship/state|door|secure|lights|locate`), Garage (`/garage/*`) |
| `ASCVehicle::AddDoorUseQuery` (a reason a door is held) | a cargo job's access doors ("Door in use by cargo") |
| `ASCVehicle::DoorwayBlockerTag` (actors with a box root the doors wait for) | cargo boxes (`ASCCargoContainer`) |

`SCViewerShell::ViewerWorld` replaces `SCCargoUI::ViewerWorld`; the request-field helpers of the RPC harness are
shared in `SCRpcJson.h` (`SCRpcArgs`). Without the game module the viewer has no F1 menu (the help lines leave it
out), no garage prompt and no cargo, Ship Link or Garage routes. `Start-SCViewer -Game` launches UnrealCitizen.

The runtime importer's own module (`SCBridgeImport`) moves to O1-4: until the game runs prepared assets it imports at
run time like the viewer, so the split has nothing to leave out yet, and O1-4 shows which code a cooked game needs.
The plugin's cargo art (`/SCBridge/Cargo`) stays in the plugin until it can move with the editor's redirectors
(O1-4); the game cooks it (`DirectoriesToAlwaysCook`), the viewer no longer does.

### O1-3 — The viewer app (this PR)

**The menu.** The viewer's F1 menu (`viewer/Source/SCViewer/SSCViewerPicker`, registered with `SCViewerShell` as the
menu and a view overlay; also the top bar's "Menu (F1)") lists environments, vehicles and the loaded vehicle's paints in
the HUD look. A pick applies at once (`SetEnvironment`, `LoadEntity`, `SetPaint`), or, while a load runs, as soon as
it lets it: the latest click waits in the menu, marked LOADING. An environment's export or parse is asked to stop and its
result dropped; an import, once started, finishes; nothing is waited for on the game thread. A vehicle waits for the
environment it goes into; clicking the vehicle being loaded drops a later choice (unless its load is being cancelled:
then it is asked again), clicking the one it replaces brings it back after. The decisions are pure functions
(`DecideVehiclePick`, `DecideEnvironmentPick`) walked through these click sequences by `SCBridge.Viewer.PickerQueue`.
The row in the scene is an inverted bar marked IN SCENE; "in the scene" is archive-scoped (an environment or vehicle
built before the archive was reopened is loaded again when picked from the new archive's lists), and so is a choice: a
row keeps the archive session of the list it was made from, and a click on a row of an earlier session's lists (the
list not yet refreshed) or a waiting choice made from them is dropped, never applied to another archive. The environment list starts with the procedural
hangar (the map's own); the environment chosen is saved once it is in the scene (a failed one is not) and restored at
the next start; clicking a failed one requests it again. Both lists have a search box (`-SCSearch=<q>` fills the vehicle one); the menu
opens scrolled to the current environment, vehicle and paint, and by itself at start when the command line asks for no
scene (no `-SCLoad`, `-SCEnvironment`, `-SCScript`, `-SCPackage`, `-SCPlayerDemo`, `-SCShowcase`, `-SCRpc`). While it
is open the engine's on-screen messages (they draw over all UI) are off when they were on, and turned back on when it
closes (a `DisableAllScreenMessages` given while it is open is undone then); however it closes, keys go back to the view. The side panels (model
browser with its options, hierarchy) and `sc.UI.Panels` are gone from the shared root; RPC `/ui` takes
`{menu: true|false}` instead of `panels`. Each app names itself in the top bar (`SCViewerShell::SetAppName`). A paint
row acts on the vehicle whose manifest listed it and is remembered for it (`LoadedEntity`), not for one still loading.

**The lists come from game data** (`crates/scviewer-core/src/viewer_catalog.rs`, `sc_viewer_catalog`,
`scviewer catalog`; `USCViewerSubsystem::GetCatalog`, listed once per archive session on demand — the menu when it opens and RPC
`/catalog` call `RequestCatalog`, runs that never ask (`-SCNoUI`, `-SCLoad` with the menu closed, the game) do not pay
for the scan; a listing finished for an earlier archive session is dropped; `/catalog` answers `environments_ready`
and, once listing or the archive open failed for the session, the session was lost, or no archive is open (`-SCPackage`),
`environments_error` (`GetCatalogError`); a listing does not scan when another archive was opened before it ran; a
saved hangar waiting for a failed or closed archive is not busy work (Retry opens another, automation's archive-failure
exit runs; `-SCNoCooked` counts as no cooked copy for this as for loading), while a pick waiting for a ready archive is not
carried over to another one. An open retires the panic notice of the session it replaces, also when it fails, and a
notice is not taken while an open is in progress (`SCBridge.Archive.PanicNoticeRetiredByOpen`) — replacing the search for "hangar" in archive paths):

- *Environments*: `EntityClassDefinition` records with `InstancedInteriorComponentParams` and a set
  `SObjectContainerComponentParams.objectContainer` (84 records, 17 of them templates without a container): **61
  hangar containers**, grouped by size (`InstancedInteriorComponentParams.name`: Small / Medium / Large / XL Hangar),
  named by the places that offer them (`InstancedInteriorLocationMap.locationInteriors[]`, not `devOnly`:
  `location.name`, `defaultHangars[]`; records sharing a container merge their places, sorted, and take the smallest
  named size), else by the record (the first by name); independent of record order (`container_rows`, tested). Of the 239 archive paths with "hangar" in them the other
  178 are pieces of these containers (each city's hangar wraps the base room as a child: the Area18 XL hangar exports
  as one 3.9 GB package with it), building parts, test and event content, and legacy hangars nothing references.
- *Vehicles*: `VehicleComponentParams` records (1111) through these rules, in order:

| Rule | Keeps a record when | Left |
|---|---|---|
| R1 | it has `SEntityInsuranceProperties` (salvage hulks have none) | 1104 |
| R2 | `AttachDef.Localization.Name` resolves to non-blank text and is no `@LOC_*` placeholder | 1088 |
| R3 | no tag under `AI.Faction`, `AI.Spawning`, `AI.Ship.Unmanned`, and its crew manifest lists no crew; a record whose tags, crew component or crew manifest do not read (or whose manifest reference is malformed) is left out and listed in the catalogue's `skipped` | 412 |
| R4 | no `Missions.ItemType.Iasi.DisplayItem` tag | 411 |
| R5 | it insures to itself (`shipInsuranceParams.shipEntityClassName`) or `EAEntityDataParams.inclusionMode` is `ReadyToInclude` | 364 |
| R6 | one row per (`vehicleDefinition`, `modification`), split into members connected by shared `Ship.Model.*` tags (untagged members join the largest such group); independent of record order | **229** |

  R6's row is the self-insured member, else an entitled one (`DefaultEntitlementEntityParams`), else any: the one
  most records insure to, then the one whose name begins the others', then the most tagged. 206 spaceships, 23
  ground vehicles (11 hover bikes, `isGravlevVehicle`); the row shows the manufacturer (`SCItemManufacturer`), size,
  role and kind. Special editions (Pirate, Best in Show, Wikelo, Exec, ...) fold into their base; most are also paints of
  it. Known leftovers: a few real ships insure to another record (Idris-M/P, Fortune, Golem OX, Terrapin Medic, Aurora
  Mk II): in two such groups a template member won the row (`ANVL_Terrapin_Medic_AI_Template`,
  `RSI_Aurora_Mk2_MissileModule_TEMP`, identical in data to their bases); the old and new Constellation records both
  list; `DRAK_Caterpillar_PU_Hijacked` and `MISC_Starfarer_NoInterior` pass every rule. The shop and fleet lists that
  would settle "purchasable" are not in DataCore.
- *Paints*: the loaded vehicle's (`Manifest.Paints`). They are also knowable before load
  (`SGeometryResourceParams...SubGeometry[]` tags, paint items with `AttachDef.Type = Paints`); not used yet.

`scviewer catalog` printed the same 229 vehicles and 61 containers as the research scripts (and after the review's rule
fixes the same 229 vehicles; `skipped` holds one record, an AI Gladius whose crew manifest does not resolve).

**Command line.** `-SCEnvironment=<container path>` and `-SCLoad=<entity>` as before (the ids the lists show), and
`-SCSkin=<index|default|name part>` puts a paint on the first vehicle loaded or adopted from `-SCPackage` (as `sc.Paint`).

**No cooking in the viewer.** It never streams cooked levels (`SCViewerShell::SetCookedLevels(false)`): ships and
environments always import from the archive through the local cache. The locally cooked ships and hangars (ignored)
moved to `game/Content/SC`; the built effects (`/Game/SC/FX`, also ignored) are in both projects.

**Open (found by the research, not changed here):**

- `parse_localization` (`crates/scviewer-core/src/index.rs`) keeps the `,P` suffix some `global.ini` keys carry, so
  those keys never resolve in the entity index's display names; the catalogue looks keys up with and without it.
  Fixing the parser changes the cached browser index (a cache-format bump re-exports every cached package).
- `BrowserIndex::build` sets an entity's `category` from its record path (`entities/spaceships`): a path filter;
  `AttachDef.SubType` is the data's answer.
- Hangars without a place in data show their record name; ship fit by size (`sizeId` against `LandingPadSize` and
  `maxBoundingBoxSize`) is likely but not linked in data, so the menu does not filter hangars by the loaded ship.

### O1-4 — The UnrealCitizen game

The game runs prepared assets only. O1-4 lands in three changes:

- **O1-4a** (merged, #43): scenarios and the prep step; the game runs a scenario from its prepared levels.
- **O1-4b**: a prepared vehicle keeps its rigs, clips and the interactions they drive (doors, lifts, ramps, panels' animated
  parts): the cook bakes rigs static and drops the clip catalogue (since WP-C1; WP-P5 "Not fixed: cooked builds"), so in
  O1-4a a scenario's vehicle and hangar show their presented pose and their doors do not move. What a run still reads from
  the archive at run time becomes prepared too: the player body and its clips,
  the cargo boxes (the stock and live spawns), the tractor record and its effects, the soundtrack, extra instances of the
  ship (a cargo setup with several owners), the Garage's vehicles. Until then a scenario run opens the archive for these
  (as the game did before); without one they fall back as before (no body, the scenario's tractor values, no music).
- **O1-4c**: the runtime importer becomes `SCBridgeImport` (FFI client, package reader/source, import job, processed
  caches, `SCBridgeLib`), loaded by the viewer and by the game's editor module, not by the packaged game, which then has
  no `scbridge` library; the plugin's cargo art moves to the game's content.

#### O1-4b — Prepared vehicles keep their moving parts; the remaining archive reads prepared

Owner's direction (2026-11-23): a prepared level does next to zero building or post-processing at load — what it needs
is saved as real assets and saved properties at prep time (the general fix WP-U3 left open), not rebuilt from derived
data when it loads. In stages:

- **O1-4b-1a — rigs as assets** (this PR). The prep step keeps rigs instead of baking them static
  (`USCCookLibrary::CookEnvironment(..., bKeepRigs)`): each rig mesh gets an editor source model
  (`SCRigAssets::MakeSavable`: LOD 0's skeletal mesh description from the rig's soft source data — triangles, normals,
  tangents, UVs, colours, per-vertex skin weights to `SC_RigRoot` + 1 + rig node — then the engine's skeletal mesh build;
  the reference skeleton, its bone order (what clip tracks index), the sockets and the material slots stay; LOD 0 keeps
  CPU-readable vertices, 16-bit skin weights, four influences and its vertex colours, black ones too; the description
  carries the bones, so a socket-only rig's empty LOD saves its source model too) and is saved with a copy of its
  skeleton under `<root>/Rigs` — every rig mesh the actor references (its meshes, the clip bindings', the rigs'), as a
  copy (`SCRigAssets::SavableCopy`) the actor is moved onto, the import's meshes left for other vehicles assembled from
  them; a rig with soft collision gets its Chaos trimesh derived again in the built vertex order
  (`SCRigAssets::DeriveCollision`) and saved on the rig (`USCRigComponent::SavedCollision`, restored when the rig
  registers; the body itself is transient; the rig's collision settings stay); rig materials become constants; the clip
  catalog is saved with the actor without its editor-only sequences (playback reads the bone tracks); the held clip
  layers (`ASCVehicle::HeldPlayback`: the poses clips left, e.g. the landing gear deployed) are saved, so a clip played
  after the load composes with them; a prepared hangar saves its waypoint sources (`ASCVehicle::WaypointSources`, the
  anchors its components), so a waypoint on a mechanism follows it after the load as it does live; the environment
  saves the state it was posed in (`ASCVehicle::PresentedState`), and one asked for while its level streams in is
  applied when it is in. The cook makes the
  collision ready only for an import that built it (`ASCVehicle::bCollisionBuilt`). The import keeps each rig's source
  data on editor hosts (`FSCImportOptions::bRetainRigSources`, `ASCVehicle::RigSources`). Keep as assets keeps copies
  (`SCRigAssets::SavableCopy`: a runtime mesh made savable on a new mesh, an asset duplicated and built) with copies of
  their skeletons and of the clip sequences on them (a metadata-only sequence with a copy of its own skeleton), every
  texture with an editable source (`SCTextureSource`, block formats decoded by the bridge; a texture without one fails
  the Keep); the live rigs stay as they are. A rig with its own body never takes the engine's asynchronous physics
  creation. A saved rig stands in its saved pose when it registers, and restores its body, baked in that pose
  (`USCRigComponent::CollisionPose`), with the skin bones saved beside it: no vertex scan and no refit at load (a game
  host then drops the bytes). Float (HDR) textures, four channels or one, keep a float source; a texture that has a
  source keeps it. The clip catalog keeps the root rig mesh (`USCClipCatalog::RootMesh`, placed or not): Keep builds
  the clips without receiving bones on its kept skeleton. Prep format 18. In a prepared Golem the entry door opens (`sc.Vehicle.Clip 0 door_open`:
  `Entry_Door_Lower open=1`); door pads, lifts, seats and ladders still need their interaction state (1b).
  Known limit (an engine follow-up, WP-E1): a socket-only rig mesh (no triangles) is built again each time an editor
  host loads it — UE 5.9's `USkeletalMesh::IsInitialBuildDone` wants a LOD 0 section, so its render data skips the
  derived-data cache. The builds are short, and a packaged game loads cooked render data and builds nothing. And
  (pre-existing, live rigs too): a refit rig body keeps the local bounds of the pose it was cooked in — Chaos's
  `FTriangleMeshImplicitObject::UpdateVertices` rebuilds the BVH without recomputing them — so a trace can miss a part
  moved outside them; the engine fix (recompute the bounds before the rebuild) is a WP-E1 follow-up.
- **O1-4b-1b — interaction state saved**: the runtime structures the import builds become saved properties, so a
  prepared vehicle needs none of them rebuilt (the actor has no load-time rebuild of them). In three parts:
  - **1b-1 — doors** (this PR): the door records (`FSCDoorRecord`: `FSCDoorTiming`, `FSCItemPortLink`), the door
    interactions' tags, speed and links, the ship doors with their movement sweeps and ground probes (`FSCShipDoor`,
    hoisted out of `ASCVehicle`), door controls and pad reach (`DoorForPanel`, `PanelReachOf`), door sensors, port
    rigs, pad labels, the doors' fragment states (`FSCFragmentKey`), the lift-driven doors (`IsLiftDriven`: a lift gate
    is never a door order) and the physics grid. Prep format 20. A prepared Golem has its ten ship doors as a live import
    does, and Ship Link opens them. A pad's lock buttons (`DoorPanelLocks`, button panels) come with 1c; until then E on
    a prepared pad operates its door.
  - **1b-2 — lifts** (this PR): each lift's model and resting place are saved (`FSCSavedLift`, written by
    `ASCVehicle::PreSave`); a loaded vehicle makes its lifts again from them (`RestoreLifts` in `PostLoad`: the model
    copied, its hooks into the vehicle and its sound events bound by `SCVehicleLifts::BindLift`, the code the import
    uses; nothing derived). The interaction network (`FSCInteractionNetwork`: `FSCInteractionInterior`,
    `FSCInteractionEndpoint`, `FSCInteractionLink`) and the lift panels (`FSCLiftPanel`) are saved, and the audio
    scene's entity emitters (`ASCVehicle::AudioEntities`: a cooked scene plays the carriage's and buttons' effects);
    instancing keeps the parts under an interaction anchor (lift panel, door pad, kiosk) as components. The cook saves lifts at
    rest (`FSCLift::IsIdle`; one that is not fails it before the actor changes: its doors' clips are not saved, and
    another level save then keeps no lift); its power state, travel direction and the doors it found no clip for are
    kept. A static cook (rigs baked) keeps no lifts. The lift paths
    stay a cache made on first use. Prep format 22. A carriage's floor-select screen (`FloorPanels`, drawn panels) comes
    with 1c; until then a prepared lift is called and sent by its call buttons and links.
  - **1b-3 — seats, ladders, usables, light groups** (this PR): the manifest records seats read (`Seat`,
    `SeatEntrances`, `InteractionPoints`: `FSCSeatDefinition`, `FSCSeatEntrance`, `FSCInteractionPoint` reflected; a
    character makes its seat routes from them), the ladder anchors (`FSCLadderAnchor`), the usable targets
    (`FSCUsableTarget`), the light groups (`FSCLightGroup`) and the room channels are saved; each light's authored
    states are computed at import (`FSCLightImporter::AuthorStates`: intensity, colour, style per state) and saved, so a
    light group or panel changes a prepared light's state without its JSON record. Light areas come back from the
    cooked audio at load as before. Prep format 25. The prepared C2 has the live import's ladder, 7 usables, 52 panel
    aims, 16 light groups and 2 seat routes.
- **O1-4b-1c — panels and screens** (this PR; before 1b-3): the screen faces (`FSCScreenFace`: default, state, hover
  and custom renders), the panel aims (`FSCPanelAim`) and the drawn panels (`FloorPanels`: the lifts' floor select and
  the button panels; `DoorPanelLocks`: a pad's lock buttons) are saved, with their faces (`FSCScreenFaceGeometry`),
  layouts, colours and the UI records they use (`FSCUiStateType` with `FSCUiTransition` / `FSCUiLinkedState`,
  `FSCUiStateImage`, `FSCUiHoverImage`, `FSCUiButton`, `FSCSendableInteraction`). The cook makes the screens' render
  instances constants, saves each drawn panel's render target as an asset (`<root>/Panels`: a constant in its own package
  refers to it) and points the panels' renders and label masks at their pooled textures. A loaded panel draws again
  when the vehicle begins play (a render target's pixels are not saved). Prep format 23. A light panel's buttons act on
  the light groups, which 1b-3 saves next; until then a prepared light panel changes its own state only.
- **O1-4b-2a** (player body): the prep step prepares the scenario's player body as its own level
  (`/Game/SC/Players/Maps/Player_<gender>_<hash>`, `USCViewerSubsystem::PlayerLevelPath`; one per body, shared by the
  scenarios that name the same one, whatever the case, order or repetition of its names, the loadout's `Data/` prefix or
  a gender left to the export's default), and the record names it (`player_level`). The body is the definition's `"player"`
  block: `"gender"`, `"loadout"`, `"weapons"`, `"spares"`, typed as written (absent: the exporter's gender and loadout, all
  weapons, spares as the viewer's `bPlayerSpareAttachments`, true), so a changed body makes the record stale; a scenario run refuses `-SCPlayerGender`, `-SCPlayerLoadout`, `-SCPlayerWeapons`, `-SCPlayerSpares` and
  `-SCPlayerPackage`, and `package_showcase.ps1` no longer takes `-PlayerLoadout` / `-PlayerWeapons`. The level holds the kit
  actor as the viewer imports it, its rigs kept (O1-4b-1a), with the kit and body clips parsed once at prep
  (`USCPlayerKitData`: `FSCPlayerKit` and `FSCPlayerClipMap` written field by field into one payload after its layout
  version, SCPlayerKitData.cpp; a payload of another layout is read whole and skipped) and its
  personal lights' cookies pooled (the cook now keeps `GoboTextures` on their pooled textures); its package audio is staged
  like a vehicle's (`Content/SCAudio/<level>`: the scene its live import registers, its items' audio actions). A run streams the level alongside the
  vehicle's (`SetPreparedPlayerLevel`, `RequestPlayerKit`, state `Streaming`); first person attaches it as before; nothing is
  exported, imported or parsed. Still at attach: the weapons' fire clips are parsed against their rigs (`FireClipJson`) and
  the torch's light function is made from its cookie. A scenario that fails releases its body with the rest of its scene.
  `golem_showcase` names the showcase's body (`StarMarine_Marine.xml`, `behr_rifle_ballistic_01`). Prep format 27.
- **O1-4b-2b**: the traversal clips (seat entry and exit, ladders) a run still asks the archive for (`FSCTraversalClips`)
  prepared with the scenario.
- **O1-4b-3**: cargo boxes and effects, music, the Garage's details prepared.

#### O1-4a — Scenarios and the prep step

**A scenario** is `game/Config/Scenarios/<name>.json` (`FUCScenario`, `UCScenario.h`): `name`, `environment` (an object
container path; none: the map's own hangar, or space when the cargo setup says so), `vehicle` (required), `skin`
(`default`, an index or a part of a paint's name or palette id: `USCViewerSubsystem::ResolvePaint`, as `-SCSkin`), `player`
(`camera`: `first` (default) / `orbit` / `fly`; `start`: a part of a first-person teleport destination's label), `cargo` (a
cargo setup under `Config/Cargo`: its ship and environment, when it names them, must be the scenario's; one in space takes
no environment) and `music` (a soundtrack cue's id or title). Two ship with the project: `c2_hangar` (the C2 in the XL
high-tech hangar with the `c2_hangar` cargo setup) and `golem_showcase` (the showcase's Golem in the small one).

**The prep step** is `UnrealEditor-Cmd game/UnrealCitizen.uproject -run=SCPrepare -Scenario=<name>
-AllowCommandletRendering -unattended [-Force]` (`USCPrepareCommandlet`, `UnrealCitizenEditor`). It replaces
`cook_ships.py`, `cook_hangars.py`, `cook_hangars.sh` and `cook_audio.py` (removed): it exports the scenario's environment
and vehicle through the bridge (auto-discovered `Data.p4k`, full resolution: the shared pools key a texture by its
source), imports each from the export
in memory (`USCImportLibrary::ImportEnvironmentFrom` / `ImportVehicleFrom`) into a new level, presents it (the hangar's
mechanisms open, the ship elevator under the origin, its ship capacity recorded in the hangar index; the ship on its
landing gear on the floor with the scenario's paint on), stages its audio beside the level (`SCPrepareAudio`, the C++ form
of `cook_audio.py`), cooks it (`USCCookLibrary::CookEnvironment`: pooled meshes and textures keyed by source path and content — a mesh's
committed source description id and collision triangles, a texture's first-mip bytes — so a changed asset at the same path is pooled anew;
constant materials) and
stamps it with the prep format (`FUCPrepared::FormatVersion`, 10 in O1-4a, 11 since the series review; it was `USCCookLibrary::FormatVersion`) and the
archive build it was prepared from (written whole: a level counts as current only with a complete stamp); a hangar index that cannot be saved fails the run. Levels:
`/Game/SC/Hangars/Maps/<style>_<name>_<hash>` (a hash of the whole container path, so containers whose last two parts
make the same name stay apart) and `/Game/SC/Ships/Maps/<entity>`, with a paint `<entity>__<paint>_<hash>` (its
palette id, else its subgeometry tag, else its name, and a hash of all three; `USCViewerSubsystem::EnvironmentLevelPath` /
`ShipLevelPath`); the skin is resolved against the paints as the import reads them (`FSCPackageReader`: a variant whose
files are unavailable is left out), the same list `-SCSkin` uses. A level already prepared in this format is kept (shared by scenarios); `-Force`
prepares it again. A level's stamp is removed before it is rebuilt and written only once it saved, so a run that fails
part-way leaves a level no record can use (the game checks the stamps outside a packaged build; the packaging refuses
unstamped levels). An archive that does not open within 15 minutes ends the run. Last it writes the scenario's record, `Content/SCPrepared/<name>.json`
(`FUCPrepared`: the format, the levels, the archive build, the time, and the scenario's definition and cargo setup as
prepared), gitignored and staged with a packaged game (`DirectoriesToAlwaysStageAsUFS`).

**The game** plays `-Scenario=<name>` (or `UUCSettings::Scenario`; `-Scenario=none` turns it off) from the record only
(`UUCScenarioSubsystem`): it refuses — nothing loads, a notice over the view (`SUCScenarioNotice`) and the log
(`SCENARIO_REFUSED`) give the reason and the prep command — when the scenario has no record, the record's format is
another, a level is missing or not finished (no current stamp), or (outside a packaged game, which has no definitions)
the definition is missing or does not read or it or its cargo setup changed since prep. A level that does not stream in (or
not within 10 minutes) fails the run (`SCENARIO_FAILED`): what it started is cancelled and unloaded (also a vehicle loaded in its
place), space is left, and
the cargo subsystem stops applying its cargo setup (`USCCargoSubsystem::DropScenario`). It does not run either when the command line names another scene (`-SCLoad`, `-SCEnvironment`,
`-SCPackage`, `-SCCargoScenario`, `-SCPlayerDemo=<entity>`) or `-SCNoCooked` (`ConflictingSwitch`, tested): the notice says so, and those switches act as without a scenario. Otherwise it streams the environment's level, then the vehicle's (`LoadPreparedEntity`), sets the
camera, puts the player at the start, asks for the music, and hands the cargo subsystem the prepared cargo setup
(`UUCSettings::ScenarioCargoJson`; it then loads no ship or environment itself, and a setup that names no ship is for
the scenario's vehicle). Log lines `SCENARIO start|loaded|
running|start_at|music`, RPC `/state` section `scenario` (id, state, problem, levels) and `/wait until=scenario`.
Without a scenario the game behaves as before (the command line and the menus decide).

`game/scripts/package_showcase.ps1 -Scenario <name>` packages a prepared scenario: it checks the record, the definition,
its cargo setup and each level's stamp, cooks the scenario's levels and starts the build with `-Scenario=<name>`. Preparing and packaging
still happen only on the owner's request.

### Series review (O1-1 to O1-4a, this PR)

A review of the series as a whole (Terra) found what the per-stage reviews could not see:

- A cargo setup's tractor record lookup waits while the archive is still opening (a prepared scenario's ship can be in
  the scene first); before, a lookup that ran first settled on the scenario's copy for the session.
- `-SCPlayerDemo`'s built-in route moved from `viewer/scripts/routes` to the shared plugin
  (`plugins/SCBridge/Resources/Routes/player-demo.txt`, staged as a runtime dependency): the game had none.
- The Ship Link door-policy tests (`SCBridge.ShipLink.Access`, `.RoomClassification`) and the package reader's cargo-field
  test (`SCBridge.Cargo.ReaderFields`) moved back to the plugin (`SCShipLinkPolicyTests.cpp`, `Cargo/SCCargoReaderTests.cpp`):
  what they test is shared, so the viewer runs them too; the cargo-dependent ones stay in the game.
- The game stages its `Config/FX/effects.json` (the cargo effects' settings; only the viewer module staged its copy).
- A scenario's staleness check ignores line ends (git's checkout turned `golem_showcase.json` into CRLF and the game
  refused it as changed); the packaging script compares alike.
- A cooked level streams in the game only when it is current (`SCViewerShell::SetCookedLevelCheck`, registered as
  `FUCPrepared::IsLevelCurrent`): also loads without a scenario (`-SCLoad`, `-SCEnvironment`, the Garage) skip an unfinished
  or older preparation and import from the archive.
- With automation that puts the camera (`-SCCamera`, `-SCScript`, `-SCPlayerDemo`) a scenario sets neither its camera nor its
  start (`AutomationOwnsCamera`); `-SCCamera` also applies to a vehicle a scenario brought, so its route starts.
- A cargo setup run without a game scenario (`-SCCargoScenario`) requests the setup's environment as soon as it reads,
  replacing the hangar the viewer restored at start, unless `-SCEnvironment` or `-SCLoad` names the scene
  (`USCCargoSubsystem::TakesScenarioEnvironment`, `SCBridge.Cargo.ScenarioEnvironment`).
- The Garage lists an entity's own prepared level only: a paint variant's level (`<entity>__<variant>`,
  `USCViewerSubsystem::IsShipPaintLevel`; an entity's own part never holds `__`) is not another ship.
- A paint request on a vehicle without paint slots (a prepared one, its paint baked in) is refused (`PAINT … refused`)
  instead of reported as applied and remembered; a prepared vehicle saves the paint it was prepared with
  (`ASCVehicle::ActivePaint`, prep format 11), so the Garage reports it and a request for it is no change.
- `viewer/README.md`: the showcase packaging takes a prepared scenario (`-Scenario`, `Play_UnrealCitizen.bat`); the cargo
  RPC example launches the game (`Start-SCViewer -Game`), cargo routes are under `game/scripts/routes`.

## Verification

Each stage: `scripts/build_ffi.ps1`, the editor build of each project that exists, the SCBridge automation tests
(baseline 231/234), and for O1-3/O1-4 a launch through the RPC driver (C2 in the XL hangar: doors, panels, popout; for
O1-4a the game with `-Scenario=c2_hangar`, refused before prep and running after it).
