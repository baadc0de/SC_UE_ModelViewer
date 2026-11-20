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
extra-vehicle placement. The follow-up floor test and the remaining live checks await the next Unreal slot.

```powershell
& ue/scripts/rpc/garage.ps1 -Port 8789 -BayEntity DRAK_Cutlass_Black -CheckAboard -Screenshots
& ue/scripts/rpc/garage.ps1 -Port 8789 -BayEntity DRAK_Cutlass_Black -FloorEntity DRAK_Buccaneer_AI_Super_CIV
```
