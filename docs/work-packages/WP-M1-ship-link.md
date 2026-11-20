# WP-M1 — Ship Link (mobiGlas app: the paired ship, remote doors and lights)

**Agents:** Opus 5.5 (domain, mobiGlas app, RPC), Codex `gpt-6.2-astra` (visual review against the mobiGlas mock).
Cross-review: Codex `gpt-6.1-terra` ⇄ Opus.
**Depends on:** WP-K1c (mobiGlas shell, orbit live view, callout layer), WP-I1 (door pads and their state images),
the C2 fixes (door data state, light groups), WP-P5 (lifts), WP-H1 (RPC harness).
**Branch:** `wp/M1-ship-link`. No submodule change expected (the export already carries `is_locked`, see Data).
**Owner decisions (2026-11-20), binding for this WP:** pairing is implicit when a ship is placed (owner role only);
doors and lights are live; every other feature is a visible placeholder; vehicle configuration is **not** here (it is
the Garage, WP-M2).

## Why

Star Citizen's common ship gripes have no home in the player's hands: a ship drifts off or is left with doors and
ramps in odd states, anyone can walk aboard, sit in the pilot seat and fly away, and the only tools are walking back
and pressing pads one at a time. Ship Link is the place and the mechanic for all of that: the player is **paired** with
a ship, the mobiGlas shows that ship live, and the ship takes orders from it. This WP builds the mechanic end to end
for doors and lights, and lays out every other remedy as a visible placeholder, so the demo shows where each one lives
before it is built. Features are added later as they are needed, each into a row that already exists.

## Concepts

- **Pairing**: a relationship between a player and a vehicle instance with a **role**. Roles in the model: `Owner`,
  `Crew`, `Guest` (only `Owner` is ever assigned in M1). A vehicle without a pairing for an actor gives that actor role
  `None`. Pairing is the single truth for "whose ship is this": the cargo domain's per-vehicle owner
  (`FSCCargoScenario::VehicleOwners`, `State->VehicleOwners`) reads it instead of keeping its own (or the two are made
  one store; no second copy).
- **Implicit pairing**: every path that places an `ASCVehicle` for the player (browser load, cooked showcase,
  `-SCScript` route, `SpawnVehicleInstance` / `-SCExtraVehicles` / `sc.Vehicle.Spawn`, cargo scenario, RPC) pairs it
  to the player as `Owner` at placement; removing the vehicle ends the pairing. One hook, not one per path:
  `USCViewerSubsystem::OnVehiclesChanged` with the instance ids (`GetVehicleInstanceId`, 0 = the loaded vehicle). Scenario vehicles with another owner (multi-owner hangars, WP-K1f) are paired to that owner. WP-M2 moves
  "placement" into the Garage's retrieve; nothing here changes then.
- **Access check**: one function answers "may actor A perform verb V on door D" (V: open, close, lock, unlock) from
  the door's lock state and A's role. Every way a door moves for an actor asks it: E on the door, a pad press, a door
  sensor, Ship Link. Paths that are the ship's own (a lift opening its gate, the cargo access rule for a transfer the
  owner started) are not actors and are described under Locks.

## Behaviour

### Doors

1. **Lock state is real door state.** Each door interaction has `locked` beside its open state, initialised from the
   export's `is_locked` (unset = unlocked). Lock and unlock change no pose.
2. **A locked door does not open for anyone without the right.** E on it, its pads and its sensor are refused; the pad
   shows its **LOCKED** state image (`DoorLockState.DoorLocked`, already rendered by WP-I1) and the first-person HUD says
   why ("Locked"). A locked door that is open can still be closed; it then stays closed.
3. **Pads' own lock controls work**: a pad press of the shared `LockDoor` / `UnlockDoor` interactions (where its canvas
   offers them, `ui_HideLockControls` false) sets the state through the access check, the same as Ship Link.
4. **Remote orders**: open, close, lock, unlock per door, and per **group** (below). An order goes through
   `ASCVehicle::PlayDoor` (never a snap), so animation, sound, `item_port_links` and pad images follow as for a press.
5. **Groups from data, never names.** `Exterior` (a door between the ship's inside and outside) and `Interior`; how a
   door is classified is evidence from the export, recorded per door (`group_source=`): the rooms on each side (WP-L1
   room volumes: one side in no room = exterior), or the hull-entrance navlink
   (`NavigationLinkComponentParams.navLinks[].linkingType`, a WP-P1 open item) if the agent exports it. Doors whose
   class cannot be decided are listed under `Other`, not guessed.
6. **Secure ship** closes every `Exterior` door (ramps included), then locks them; interior doors are untouched.
   A door that cannot close (blocked, a cargo transfer using it) is reported in the result, the rest go ahead.
7. **Locks never trap the player.** A locked exterior door refuses entry, not exit: from inside, the door and its inner
   pad open for the paired owner. (A full lockdown with interior doors is a placeholder.)
8. **Labels**: a door's name in the app comes from data (its pad's `ui_LocationName` / `ui_CustomLabel`, then the
   door entity's display name, then its port name cleaned); duplicates numbered (the waypoint rule's `NumberDuplicates`).

### Locks and the ship's own paths

- **Lift-driven doors** (`LiftDrivenRigs`) are not lockable in M1 and not listed as lockable (lift lock is a placeholder).
- **Cargo access** (`FSCEnvironmentAccess`, `USCCargoSubsystem::RequestAccess`): a transfer that the vehicle's owner
  started may open a locked hold door; it unlocks it for the transfer and locks it again when the hold closes, as it
  already closes what it opened. A transfer by anyone else is refused with "hold locked by owner".

### Lights

1. One row per light group of the ship (`ASCVehicle::LightGroups`), named from data (the group entity's name, cleaned),
   with the modes that group actually receives (`FSCLightGroup::Interactions`, labelled by their `LightState`), never a
   fixed on / low / emergency list.
2. A mode is set through `DeliverLightGroupTag`, so the physical light panel's linked state follows (the same path as
   `PressButtonPanel`), and a panel press shows in the app.
3. The quick-action **Lights** toggles every group between its first "on" mode and "off" where the group has both;
   groups without an off mode are left alone and said so.

### Placeholders (owner rule)

Everything not live is a **visible, disabled row** with a small `NOT INSTALLED` tag and a one-line description of what
it will do (in the app's own copy). No placeholder looks pressable; none does anything. Rows:

| Section | Placeholder | Description line (draft) |
|---|---|---|
| Access | Lift control | Call or lock the ship's lifts from anywhere. |
| Access | Lockdown | Lock every door, inside and out. |
| Systems | Power | Bring ship power up or down. |
| Systems | Engines | Start or stop the engines. |
| Flight | Hold position | Stop the ship drifting and keep it where it is. |
| Flight | Auto-land | Find a pad or flat ground and set the ship down. |
| Flight | Recall | Bring the ship to you. |
| Security | Ignition lock | Only paired players can start the ship. |
| Security | Crew access | Grant and revoke crew and guest access. |
| Security | Event log | Who opened, locked or flew the ship, and when. |
| Loadout | Configure | Change the loadout in the Garage. (Becomes a link when WP-M2 lands.) |

A placeholder becomes live in the WP that builds it; that WP removes its row from this table.

## UI (mobiGlas)

The mobiGlas rail gains **SHIP LINK** beside CARGO (the K1c rail had one app and no placeholders; it now has two apps,
and GARAGE shows on the rail as `NOT INSTALLED` until WP-M2). Layout follows the mobiGlas style
(`SCMobiGlasStyle`, `docs/cargo/mobiglas-mock.html`); extend the mock with a Ship Link page first and have Astra review it
before Slate work.

- **Header**: ship display name, `PAIRED · OWNER`, distance and bearing from the player, the ship's state in one line
  (doors open / locked counts). With several paired ships, a picker (a fleet list; one is shown at a time).
- **Live view** (upper part, as Cargo): the orbit camera (`FSCMobiGlasOrbit`) frames the paired ship wherever it is;
  the callout layer (`SSCCalloutLayer`) puts one callout on each listed door: name, state (`OPEN` / `CLOSED` /
  `OPENING` / `CLOSING` / `LOCKED`). Clicking a callout selects that door's row.
- **Quick actions**: `SECURE SHIP`, `RAMP` (open / close every exterior door of the ramp kind, if the data marks one;
  otherwise the button is absent), `LIGHTS`.
- **Sections** (lower part): Access (door rows with open/close and lock/unlock, group headers with group actions),
  Systems (light groups, then the placeholders), Flight, Security, Loadout (read-only summary: items by port, from the
  manifest's `children[]` with display names; the Configure placeholder).
- **Pending vs done**: an order shows `OPENING…` / `CLOSING…` from the door's real playback (`IsRigPlaying`, the door
  state) until it ends; a refused order shows its reason in the row. Never an optimistic flip.
- **Find my ship**: a toggle that keeps a world-space marker on the ship (callout layer, visible with the mobiGlas
  closed) with its distance.

## Data and code

- **Reader**: parse `door.is_locked` into `FSCDoorTiming` (or a door state beside it), next to `bIsOpened`
  (`SCPackageReader`). The exporter already writes it (`vendor/StarBreaker` `interior_interactions.rs`,
  `entity_interactions.rs`; documented in `decomposed-export-contract.md`). If loadout-child doors lack it, add it on
  `scviewer/doors` in the submodule (tests first) and bump the `scviewer-core` cache version.
- **Vehicle**: `IsDoorLocked(Index)`, `SetDoorLocked(Index, bool)` on `ASCVehicle`; the access gate in front of every
  actor path into `PlayDoor` (E, pads, sensors); `RefreshPanelScreens` pins `DoorLockState` from the lock state.
- **Pairing**: a small store (a `UWorldSubsystem`, e.g. `USCShipLinkSubsystem`, Runtime module) mapping vehicle
  instance → { actor, role }; queries `PairedShips(Actor)`, `RoleOf(Actor, Vehicle)`; the access check lives here.
- **Model / view**: a Ship Link model beside `FSCMobiGlasModel` (door rows, light rows, groups, pending orders);
  Slate widgets under `UI/ShipLink/`. The rail becomes a list of apps.
- **RPC** (WP-H1): every live action is an endpoint, and the state log reports it:
  - `/ship/state` → `ship.pairings[]`, `ship.doors[]` (`label`, `group`, `group_source`, `open`, `moving`, `locked`),
    `ship.lights[]` (`label`, `state`, `modes[]`);
  - `/ship/door` `{ ship, door | group, action: open|close|lock|unlock, as? }` (`as`: an actor id, default the player;
    lets a test act as an unpaired actor);
  - `/ship/secure` `{ ship }`, `/ship/lights` `{ ship, group | all, mode }`.
  - Anomalies: `door_open_while_locked_by_actor` (a locked door opened by an actor path), `pad_lock_mismatch` (a pad's
    lock image disagrees with its door).
- No hard-coded ship, door or light names anywhere; no branching on asset names.

## Tests (automation, failing first)

- Access check: role × verb × lock state table, including "locked door, owner inside, exit allowed".
- Lock gate: a locked door refuses E, pad and sensor for an unpaired actor; the owner's unlock then open works; a lock
  changes no pose.
- Pad image: `RefreshPanelScreens` shows LOCKED for a locked door, the open state otherwise.
- Groups: classification from synthetic room volumes; an undecidable door lands in `Other`.
- Secure ship: closes then locks exterior doors, leaves interior ones, reports a blocked door.
- Cargo access: an owner's transfer opens and re-locks a locked hold door; another owner's is refused.
- Lights: modes listed from the group's interactions; setting one changes the panel's linked state.
- Pairing: placing a vehicle pairs it to the player; removing it ends the pairing; cargo owner reads it.

## Verification

- `Automation RunTests SCBridge.ShipLink` (new) and the existing `SCBridge.Cargo`, `SCBridge.Doors`,
  `SCBridge.Panels` and `SCBridge.Lift` suites, all passing.
- RPC run on the C2 in `hangar_xltop_001` (WP-H1 harness, off screen): `/ship/state`; secure ship; a refused open as an
  unpaired actor; the owner's open from inside; lights all off and back; a cargo transfer through a locked hold door.
  Paste the state-log excerpts.
- On screen: mobiGlas Ship Link screenshots (overview, a door pending, locked callouts, placeholders) and a first-person
  shot of a pad showing LOCKED after a remote lock. Astra compares against the extended mock.

**Done means:** in first person in the hangar, the player opens the mobiGlas, picks SHIP LINK, sees their C2 with door
callouts, presses SECURE SHIP and watches the ramp and doors close and lock; walking up, the ramp pad shows LOCKED and
E is refused for an unpaired actor (RPC), while the owner opens it from the app. Light groups switch from the app and
the physical panels agree. Every other remedy is visible as a NOT INSTALLED row.

## Out of scope

Roles other than Owner in use, the pairing ceremony, lift lock, lockdown, power, engines, hold position, auto-land,
recall, ignition lock, crew list, event log (all placeholders); persistence of pairing and locks across sessions;
loadout changes (WP-M2).

## Implementation notes

- The viewer's active vehicle set drives a world-scoped pairing store. The cargo scenario can assign an owner to a
  queued instance. Cargo grid discovery reads the same pairing as Ship Link.
- Door egress uses the player's actual first-person location. An RPC order counts as local when it explicitly
  uses that player's position. Secure ship waits for the exterior door to finish closing before locking it.
- Group evidence comes from room volumes sampled on both sides of the door's installed collision. An unresolved side
  pair leaves a door in Other. The ramp action requires an exported item type or subtype that identifies a ramp.
  The C2 package examined during implementation marks both ramps as Door.
- The package exposes the ship display name. Door labels use available item display names and port labels. The
  operating-panel bindings in the inspected package lack localized `ui_LocationName` and `ui_CustomLabel`.
- For a quick game-mode check, load a door-capable ship such as Cutlass Black in the default Viewer map, then run
  `ue/scripts/rpc/ship-link.ps1 -Port 8770`. It polls pairing, door playback and lock state. Add
  `-Screenshots` for Slate evidence or `-RequireExterior` for an exterior-door acceptance fixture.
