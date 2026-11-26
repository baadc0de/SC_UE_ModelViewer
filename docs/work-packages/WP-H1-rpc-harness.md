# WP-H1 — RPC harness and state log (hardening)

The running viewer is driven over a localhost HTTP/JSON API so that incremental scene changes (ships, cargo,
transfers, grids, camera) can be scripted and explored for stability, and it writes a state log that says when the
scene is idle and settled, so a driver continues as soon as it can instead of sleeping.

Code: `USCRpcSubsystem` (`Public/SCRpcSubsystem.h`, `Private/SCRpcSubsystem.cpp`), the cargo side in
`Private/Cargo/SCCargoRpc.cpp`, JSON helpers in `Private/SCRpcJson.h`. Client: `viewer/scripts/rpc/SCRpc.psm1`; the
exploratory driver `game/scripts/rpc/explore.ps1`.

## Switches

| Switch | Meaning |
|---|---|
| `-SCRpc` / `-SCRpc=<port>` | HTTP server on `127.0.0.1:<port>` (8770 by default; the HTTPServer module binds localhost). Turns the state log on. |
| `-SCStateLog` / `-SCStateLog=<path>` | State log without the server; default `Saved/Logs/SCState.ndjson`. |
| `-SCStateLogMaxMB=<n>` | Size at which the log rotates (64). |
| `-SCStateLogFiles=<n>` | Files kept: `SCState.ndjson`, `SCState.1.ndjson` … (4). The previous run's log becomes `.1` at start. |
| `-SCStateLogHz=<n>` | State records per second while anything happens (2); one every 10 s once settled. |

| `-SCFfmpeg=<exe>` | ffmpeg for `/record` (default: `ffmpeg` on PATH). |

Built only for desktop targets that are not Shipping (`SC_WITH_RPC`); Shipping packages have neither.

**No window.** `Start-SCViewer` (SCRpc.psm1) launches with UE 5.9's `-AgentMode`: the viewer renders off screen
(`-RenderOffScreen`), runs unattended, and answers modal dialogs with their default, so nothing takes focus;
screenshots and video still capture the viewport. `-Visible` shows the window instead. It also passes the ffmpeg it
finds (PATH, or a winget install) as `-SCFfmpeg`.

## Idle and settled

- **Idle**: nothing loads (ship, environment, archive), no vehicle instance is spawning, no box package is loading, the
  cargo scenario has been placed, no job is active, no grid is extending or retracting, every drone is docked, no box
  is off its grid, and no RPC answer is pending (waits aside). The causes are the `reasons` keys: `loading`,
  `vehicle_spawning`, `environment`, `archive`, `cargo_packages`, `cargo_scenario`, `cargo_jobs`,
  `cargo_grid_access`, `cargo_drones`, `cargo_boxes_moving`, `cargo_boxes_settling` (loose boxes falling or coming to rest, WP-K2), `rpc:<path>`.
- **Settled**: idle, and for 0.5 s nothing has moved faster than 1 cm/s (simulated boxes, drones, vehicles, the
  first-person pawn) and nothing is left to stream or compile (`unsettled` keys: `moving`, `camera` (it moved more than 0.5 cm or 0.05° in the last frame), `streaming`, `shaders`,
  `assets`). Screenshots after a settled wait show the final scene.

## API

Every route takes `GET` or `POST`; a POST body is a JSON object, and `GET ?key=value` query parameters are read as
string fields. Answers are JSON objects with `ok` (HTTP 200) or `ok: false` and `error` (HTTP 400). Positions are
world metres, rotations `[pitch, yaw, roll]` degrees, forces newtons.

| Route | Body | Answer |
|---|---|---|
| `/health` | | `port`, `pid`, `log`, `idle`, `settled`, `reasons`, `anomalies_total`, `frame` |
| `/state` | | The state record (below) |
| `/catalog` | | Box `packages` (entity, state, scu, mass, size), stored `commodities`, `environments` |
| `/wait` | `until`: `idle` \| `settled` \| `job` (+ `job`) \| `lift_idle` (+ `lift`, default every lift; WP-P5), `timeout_s` (120), `min_s` | Answers once the condition holds on a frame after the request: `waited_s`, `reasons`, `unsettled`, `anomalies` (`lift_idle`: and `lifts[]`); `ok: false` on timeout |
| `/mark` | `note` | Writes a `mark` record (a driver's step marker) |
| `/console` | `cmd` | Runs a console command; `lines`: what it logged (verification hooks: `sc.Cargo.LoseDrone busy`, `sc.Cargo.Interrupt [job]`) |
| `/load`, `/unload` | `entity` | Loads / unloads the main vehicle (instance 0) |
| `/environment` | `folder` | Sets the hangar (object container path) |
| `/ship/spawn` | `entity` (`loaded`), `x_m`, `y_m`, [`z_m`], [`yaw`]; or none of them: free floor beside the ship; `wait` | `id`; with `wait`, answers when the instance is ready |
| `/ship/state` | — | `ship.pairings`, `ship.doors`, `ship.lights`; also present in `/state` and the state log |
| `/ship/door` | `ship` (instance id, default 0), `door` id or `group` name, `action` (open/close/lock/unlock), optional `as` actor and `local` | Per-door `pending` / refusal. `local: true` uses the actual first-person location for owner egress. |
| `/ship/secure` | `ship`, optional `as` actor | Close exterior doors and lock after playback. Per-door outcomes. |
| `/ship/lights` | `ship`, `group` id or `all: true`, `mode` (authored state or `toggle` for all), optional `as` actor | Apply through each group's interaction. All-group orders report skipped ids. |
| `/ship/locate` | `ship` | Toggles the paired ship's world marker and returns `locating`. |
| `/ship/despawn` | `id` | Removes an extra instance (its grids, boxes, docks and drones go; jobs touching them fail) |
| `/garage/fleet` | Optional `entity` filter | `garage.fleet` with vehicle catalogue entries, placement state, progress and live paint |
| `/garage/retrieve` | `entity`, optional `at` (`bay` or `floor`) and `confirm_store` | Starts retrieval. If the bay holds a paired ship, returns its `occupant` and asks for `confirm_store`. |
| `/garage/store` | `ship` instance id | Removes the paired ship if no player or cargo transfer depends on it. |
| `/garage/paint` | `ship` or `entity`, `paint` palette id or `default` | Applies and remembers the chosen paint for the retrieved ship. |
| `/cargo/package` | `entity` | Answers when the box entity's package is exported and imported |
| `/cargo/spawn` | `grid` (id or selector: `ship`, `elevator`, label text), `entity`, `count`, `commodity`, `label`, `owner`; or loose (WP-K2): `at` ([x, y, z] m, `yaw`) or `random` (`seed`: free floor spots near the ship with room above for a pick) | `boxes`: ids placed (best fit); `warning` when not all fit. Waits for the package. Loose boxes fall, come to rest and join or found a beacon. |
| `/cargo/remove` | `box` \| `grid` \| `all`, `force` | `removed`, `refused`. A box a job holds is refused unless `force`: that job is abandoned (drones let go and fly home) and its boxes in the air go too. |
| `/cargo/dock` | `grid`, `drones` | A drone dock on the grid |
| `/cargo/transfer` | `from`, `to` (selectors: grids, or for `from` a beacon id or name, `beacon` / `loose` for all), `commodity`, `scu`, `owner`, `preview` | `job` (or `error`), `planned_scu`, `moves`, `refusals` |
| `/cargo/cancel` | `job` | |
| `/cargo/access` | `grid`, `open` | Extends / retracts a grid's mechanism |
| `/camera` | `mode` (`fly`, `orbit`, `first`); `at` + `look_at` or `rot` (a pose: the fly camera unless `mode` says otherwise); or `focus`: vehicle id or `all` (the orbit camera frames it) | `mode` |
| `/screenshot` | `name` or `path`, `ui` | Answers when the PNG is written: `path`, `bytes` (directory: `-SCShotDir` or `Saved/Screenshots/rpc`) |
| `/render` | Fixed-step camera path, dimensions, output formats and frame count, or `action: status` | Frame sequence, per-frame camera and timing sidecar, optional encode. See [WP-H2](WP-H2-offline-render.md). |
| `/record` | `action`: `start` (`name` or `path`, `fps` 30, `max_s` 300, `width`, `keep_frames`) \| `stop` \| `status` | 8-bit preview video (`source_format` identifies the viewport texture): frames grabbed off the back buffer (no stall), JPEGs written off the game thread with their capture times, encoded by a hidden ffmpeg to H.264 MP4 that plays in real time. `stop` answers when the file is written: `path`, `frames`, `dropped`, `seconds`, `bytes`. `max_s` is capped at 4 h; a recording still running when the app closes is finished (encoded) during shutdown. Directory: `-SCShotDir` or `Saved/Videos/rpc`. ffmpeg: `-SCFfmpeg=<exe>` or on PATH; without it the frames and their timing list are kept. |
| `/quit` | `code` | Exits |

## State log (NDJSON)

One JSON object per line; every record has `ev`, `t` (world seconds), `wall` (UTC ISO 8601) and `frame`.

| `ev` | Fields |
|---|---|
| `start` / `stop` | `cmd`, `pid`, `port` |
| `idle` / `busy` / `activity` | `reasons` (activity: the busy reasons changed) |
| `settled` / `unsettled` | `idle_for_s` / `why` |
| `state` | `idle`, `settled`, `reasons`, `unsettled`, `frame_ms`, `speed_ms`, `anomalies`, `load` (entity, pending, phase, archive, environment: the request, environment_ready: that hangar loaded, space), `vehicles` (id, entity, state, pos, rot, error), `camera` (mode, pos, rot), `cargo`: `grids` (id, kind, vehicle, owner, group, label, access, used / reserved / capacity SCU, boxes, pos), `boxes` (id, entity, scu, commodity, owner, state, grid, job, actor, pos, rot, sim, vel, ang_vel, slot_err_cm), `drones` (id, state, owner, dock, job, move, carrying, pos, vel, beam_force_n, beam_rest_m, beam_len_m, blocked, in_move), `docks`, `beacons` (id, name, origin, origin_label, owner, scu, boxes, pos, accepts_drops), `jobs` (id, state, owner, moves, moves_done, summary, reason, age_s); boxes also carry `beacon` and `origin` |
| `rpc` / `rpc_done` | `id`, `path`, `body` / `ok`, `ms`, `error` |
| `log` | `level`, `cat`, `msg`: warnings, errors, and job / scene lines (`CARGO_JOB`, `CARGO_DRONE_OVERLAP`, `VEHICLE_`, …) |
| `anomaly` / `anomaly_cleared` | `key`: `nan:<kind>:<id>`, `fell:<kind>:<id>` (below −10 m), `box_no_actor`, `box_off_slot` (a stored box > 5 cm from its slot), `box_simulated_on_grid`, `box_stored_without_grid`, `box_reserved_nowhere`, `box_unbeaconed` (loose and at rest but in no beacon), `drone_orphaned` (out of its bay in no move that has not ended) |
| `mark` | `note` |

## Exploratory testing

`explore.ps1 -Scenario <c2_hangar|hull_a_hangar|hull_a_multi> -Steps N -Seed S` starts the viewer, waits until the
scenario settles, then picks seeded random actions among those that make sense in the current state (ship spawn /
despawn, cargo spawn / remove, loose boxes put down at random reachable spots and carried from their beacons, removal of a box in the air, a load in flight interrupted or left with too few drones, transfer, cancel, grid access); about half are followed by
a settled wait, the rest run back to back. After each step it checks that the viewer is alive and answering, and logs
the step to `steps.ndjson`; at the end it requires the scene to settle with no active job, no drone away and no
anomaly, every loose box in a beacon and every beacon's members loose, and reads the game log for crashes and ensures (it counts re-targeted loads and boxes let down). Output: `out/h1/explore-…/report.json`, `steps.ndjson`,
the state log, screenshots.
