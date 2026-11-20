# WP-M1 verification

## Win64 development build and synthetic tests

```powershell
$project = (Resolve-Path 'ue/SCViewer/SCViewer.uproject').Path
& "$env:USERPROFILE/UnrealEngine-5.9-src/Engine/Build/BatchFiles/Build.bat" SCViewerEditor Win64 Development "-Project=$project" -WaitMutex
& ../SC_UE_ModelViewer-wp-Q1/ue/scripts/rpc/automation.ps1 -Project $project -Filter SCBridge.ShipLink,SCBridge.Cargo,SCBridge.Doors,SCBridge.Panels,SCBridge.Lift,SCBridge.Reader.C2FixesFields
```

Build succeeded. The first cold build surfaced two compile fixes. The incremental rebuild finished eight actions
in 6.29 seconds. All 88 selected tests passed in one NullRHI process, which exited normally after 15.46 seconds.

## Live state and UI

One Win64 D3D12 game-mode process, 1280 × 720, with `-SCDefaults` and the existing export cache. Waited on
`/state` for a ready archive in the procedural Viewer map, then issued
`/load {"entity":"DRAK_Cutlass_Black","wait":false}` in the same process. Waited on the loaded entity and phase
before starting the feature driver.

```powershell
& ue/scripts/rpc/ship-link.ps1 -Port 8787 -TimeoutSeconds 60 -Screenshots
```

- Procedural Viewer ready in 10.73 s. Cutlass, one owner pairing and 12 door rows ready in 42.68 s from launch.
- Door 0 classified Exterior with room-volume evidence. Its lock held it stationary. An unpaired open was refused.
  Owner unlock/open, playback completion, Secure Ship and authored light Off/on all passed in 20.54 s.
- Screenshots were written to `out/M1-cutlass/ship-link-locked.png` and `out/M1-cutlass/ship-link-opening.png`.
  Viewer exited with code 0.

Cutlass covered placement, state, remote door access, room groups, secure completion and real light modes.

## Review and follow-up

Astra approved the retakes from the stacked Garage build at `out/M2-Cutlass-20261120-221057/`. The locked
capture shows the same Locked door in the callout and access row, with one locked and zero moving. The opening
capture shows Opening in both places and zero fully open with one moving. The three panes were readable at
1280 × 720 and the shader banner was absent. The corrected Cutlass run passed in 3.99 seconds after asset readiness.

The stacked build compiled the pad-control follow-up and passed all 92 selected tests, including the 88
Ship Link and existing regression tests.

## Physical lights and aperture follow-up

The combined worktree built successfully and passed 93 focused tests in 14.30 seconds. The Cutlass state-polled
Ship Link route passed in 3.44 seconds and shut down normally. The selected rear light group had 70 authored lights;
the Off order changed all 70 and set the sum to zero. Default restored all 70 and 46,808.4 cd total. The route
selected a group that actually has lights, and the RPC journal now also includes actor-lock and pad-state checks.

## C2 in the authored XL hangar

The combined Garage build at `ba56d13` carries Ship Link runtime `fa6a939`. The same D3D12 process exercised the C2 XL
cargo scenario, Ship Link and Garage. The cargo scenario initially issued its ship request while the hangar was
loading. The full run waited for the published environment and issued C2 load again in the same process.

```powershell
& ue/scripts/rpc/ship-link.ps1 -Port 8789 -TimeoutSeconds 250 -RequireExterior -RequirePad -C2Cargo -Screenshots
```

- The C2 paired to `you` and exposed 31 doors, including four exterior and 22 interior. The rear ramp's linked pad
  selected `DoorLockState.DoorLocked`. The locked external physical open was refused, then the owner opened it
  locally from inside with the lock retained. Secure Ship returned it to closed and locked. The light route passed.
- The owner's 16 SCU transfer opened two locked exterior doors. Job 1 ended Done in 93.0 seconds with zero overlaps.
  The front-left door closed and relocked automatically. The stored rear-grid box still overlapped the rear doorway
  query. Removing that one fixture box started rear closure on the next frame, then the locked pad state returned.
  The route passed in 263.21 seconds with this intervention. Whether the stored slot intersects the ramp collision
  or the current query is conservative remains open.
- The first captures were `ship-link-locked.png` and `ship-link-opening.png` in
  `out/M2-C2XL-20261120-222506/` of the Garage worktree. Astra confirmed both state rows and header counts. The orbit
  framing hid the selected rear ramp and an engine ray-tracing budget overlay was visible.

## C2 physical pad and framing retest

The warmed viewer reached XL plus C2 readiness in 77.27 seconds. The same route without the transfer replay passed
in 34.77 seconds, including denial, secure closure, and light modes. It captured the rear ramp from behind and
then aimed the first-person view at its linked physical pad from 85 cm. The viewer reported focus on door 0 and
the material selected `DoorLockState.DoorLocked` with no pinned override.

Astra approved `ship-link-locked.png`, `ship-link-opening.png` and `locked-pad.png` under
`out/M2-XLretest-20261120-225717/` in the Garage worktree. The Rear ramp leader and access row agree as the
header changes from one locked to one moving. The physical pad shows a red closed padlock.
