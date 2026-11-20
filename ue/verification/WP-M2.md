# WP-M2 verification

The stacked Win64 Development target built successfully. The last retry compiled ten actions in 6.95 seconds.
The grouped NullRHI run passed 92 tests in 19.19 seconds, including all four Garage policy and kiosk tests and all
selected Ship Link, Cargo, Doors, Panels, Lift and reader tests.

```powershell
$project = (Resolve-Path 'ue/SCViewer/SCViewer.uproject').Path
& "$env:USERPROFILE/UnrealEngine-5.9-src/Engine/Build/BatchFiles/Build.bat" SCViewerEditor Win64 Development "-Project=$project" -WaitMutex
& ../SC_UE_ModelViewer-wp-Q1/ue/scripts/rpc/automation.ps1 -Project $project -Filter SCBridge.Garage,SCBridge.ShipLink,SCBridge.Cargo,SCBridge.Doors,SCBridge.Panels,SCBridge.Lift,SCBridge.Reader.C2FixesFields
```

A single D3D12 game-mode run on the procedural Viewer map reached Cutlass ready in 26.95 seconds. It verified the
fleet endpoints and the occupied-bay confirmation refusal. The separate free-floor request for a Buccaneer hit the
old placement search limit (60,000 candidates) before the remaining Garage assertions. This attempt exited normally.
The stacked Ship Link verification passed in the same process.

The follow-up bounds the procedural-map search to the collision floor and reports candidate rejection counts.
The state-polling driver now checks primary store, retrieval, aboard refusal and remembered paint before optional
extra-vehicle placement. The bounded-floor and lifecycle retest below exercised those checks.

```powershell
& ue/scripts/rpc/garage.ps1 -Port 8789 -BayEntity DRAK_Cutlass_Black -CheckAboard -Screenshots
& ue/scripts/rpc/garage.ps1 -Port 8789 -BayEntity DRAK_Cutlass_Black -FloorEntity DRAK_Buccaneer_AI_Super_CIV
```

## Bounded-floor and lifecycle retest

The updated Win64 build succeeded and the broader grouped NullRHI run passed 93 tests in 14.30 seconds. The
same procedural Viewer fixture reached Cutlass in 29.62 seconds. The Garage route completed in 45.59 seconds and
the viewer exited normally.

- Cutlass primary retrieval paired the ship. An aboard player could not store it. After leaving, store removed the
  pairing, and a fresh retrieve restored both the pairing and the chosen paint.
- An occupied primary bay asked for confirmation. The separate Buccaneer used normal floor placement and paired as
  instance 1 after 702 candidates. Its placement was approximately (-3.72, 27.13, 2.21) m. It retained its position
  and pairing when the Cutlass was stored and retrieved a second time.
- The client resolved manufacturer code DRAK and authored size 3 from the Cutlass class record.
- Astra's first Garage screenshot review found placeholder strings in the catalog, an offscreen selected row and
  Cargo callouts over the Garage. The follow-up excludes the game's placeholder localization markers, shows live
  ships first, scrolls the selected ship into view, uses the mobiGlas paper list and hides Cargo callouts in Garage.
  Its incremental build succeeded and all five Garage tests passed. The corrected rendered capture is part of the
  C2/XL follow-up.

## Authored XL hangar

The combined C2/XL game-mode run reused the process previously used for Ship Link and exited with code 0. The
authored bay route passed with the C2, including the aboard refusal, paint persistence, storing, retrieval and
both pairing transitions. On the separate floor route, the Cutlass envelope measured 35.7 × 26.1 × 15.5 m. The
solver rejected 29,780 candidates in keep-outs and the remaining 5,612 for missing flat floor at the reference
height of -5 cm. The output distinguishes that geometry result from the verified procedural floor placement.

```powershell
& ue/scripts/rpc/garage.ps1 -Port 8789 -BayEntity CRUS_Starlifter_C2 -FloorEntity DRAK_Cutlass_Black -CheckAboard -Screenshots
```

Astra confirmed the C2 in-hangar row pinned at the top of the readable list with no leaked Cargo overlay in
`out/M2-C2XL-20261120-222506/garage-fleet.png`. Several remaining catalog variants share a display label. The
camera faced the side wall. Live kiosk interaction, a showcase retake and a secondary ship on authored XL floor
remain to verify.
