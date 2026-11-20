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

Cutlass covered placement, state, remote door access, room groups, secure completion and real light modes. Full C2/XL
acceptance is still pending for a door with a linked operating pad, local owner egress, and cargo access through a locked hold.

## Review and follow-up

Astra approved the retakes from the stacked Garage build at `out/M2-Cutlass-20261120-221057/`. The locked
capture shows the same Locked door in the callout and access row, with one locked and zero moving. The opening
capture shows Opening in both places and zero fully open with one moving. The three panes were readable at
1280 × 720 and the shader banner was absent. The corrected Cutlass run passed in 3.99 seconds after asset readiness.

The stacked build compiled the pad-control follow-up and passed all 92 selected tests, including the 88
Ship Link and existing regression tests. The C2/XL pad, local egress, cargo lock and first-person pad image
checks remain queued. The state-polled route accepts `-RequireExterior -RequirePad -C2Cargo` for that fixture.

## Physical lights and aperture follow-up

The combined worktree built successfully and passed 93 focused tests in 14.30 seconds. The Cutlass state-polled
Ship Link route passed in 3.44 seconds and shut down normally. The selected rear light group had 70 authored lights;
the Off order changed all 70 and set the sum to zero. Default restored all 70 and 46,808.4 cd total. The route
selected a group that actually has lights, and the RPC journal now also includes actor-lock and pad-state checks.
