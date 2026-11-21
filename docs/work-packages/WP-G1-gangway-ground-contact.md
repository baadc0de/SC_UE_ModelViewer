# WP-G1 — Gangway ground contact

An opening ramp follows the selected door clips until its first lowering surface meets supporting collision.
Its held playhead leaves the ramp walkable at that pose. A close or reversal uses the door player's existing
pose matching. Without supporting ground it completes the authored stroke.

The loader recognizes the authored Ramp or Gangway item type or subtype, or an exact ramp/gangway role token
in the leaf attachment port. The C2's two ramps are Door items: the rear uses a reversible transition and
the front uses separate Open and Close clips. Both use the same ground rule.

* Sample the rigid, socket-attached Pawn-blocking collision triangles in their bone frames, including triangle
  interiors and edges at up to 35 cm intervals. Coalesce duplicate surface samples through a 2 cm spatial hash.
* Advance the active opening layers together, sampling their authored bone rotations and translations.
  Within each tick, test intervals of at most one playhead frame and subdivide curved probe travel to
  a 1 mm midpoint tolerance. A broad-phase query skips clear intervals.
* Trace lowering probes against world-static, world-dynamic and physics-body collision that blocks the Pawn
  channel. Ignore the host, attached actors, riders, initial penetrations and faces that cannot support a lowering
  surface. Select the first contact across the ramp, leaving up to 1 mm clearance along the contacted probe's path.
* Hold all opening layers at the same tick fraction before advancing clip audio and updating collision.
  For a grounded ramp with a known reversible closing sweep, the cargo guard uses only the reached
  portion if the collider still matches the exact held pose. Unsupported closing paths retain M1's
  conservative occupied-aperture check. Ordinary doors, lift legs and route planning retain their existing rules.

The probe rule samples the authored rigid surface. Fine protrusions falling between probes and ramps carried
solely by deforming skin or by unlabelled ports are outside its coverage. A held ramp does not automatically
re-extend when the ship is moved.

## Verification

`SCBridge.Exploration.GangwayGround` covers uneven supporting heights, a clear stroke, rising probes,
between-key ground contact and role matching. The existing Ship Link access, obstruction and room-classification
tests cover the surrounding door behavior. `SCBridge.Gangway.PackageContact` accepts
`-SCGangwayPackage=<export-root>` and exercises each ramp of that ship. It compares its unobstructed endpoint
with the first contact against a solid tilted platform, then closes it from the contact pose. Its output reports
the actual surface clearance in centimetres.

Win64 Development built with the source fork. Four focused tests passed in one NullRHI session. In a separate
NullRHI session, the complete C2 LOD2 package's rear and front ramps stopped on a 4° pad with 0.100 and
0.040 cm clearance respectively. The rear reversed from frame 465.81 / 500; the front selected frame 49.20
of its separate closing clip. Both returned to their measured closed poses. The package check exited 0.

The final incremental build passed. The four asset-free tests, including the shortened cargo-sweep regression,
completed in 9.68 s including commandlet startup. A smaller fixture retained the original C2 root and front/rear
ramps, their authored clips and Chaos shapes. Its package test measured extrema on the actual posed triangles
independently of the predictive probes. It reproduced the two sloped-pad clearances. An isolated 110 cm-wide step
under just one edge gave rear/front clearances of 0.100/0.032 cm. Both closed cleanly again.
The smaller package case took 4.37 s inside the engine (exit 0).
