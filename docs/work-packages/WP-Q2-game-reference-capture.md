# WP-Q2 — Game reference capture

**Goal:** pairs of pictures, the viewer and the real game from the same place, for looks the data does not settle (how
the game draws a force field, glow on blended surfaces, exposure) and for checking reported bugs against the game.

There are too many shots to take by hand, so the game half is taken by **computer use**: an agent drives Star Citizen
with the mouse and keyboard and takes screenshots, as a player would. The owner decided this on 2026-11-26 and accepts
the risk under RSI's terms ("automation" programs) and Easy Anti-Cheat. The owner prefers **Codex** for the driving.

## Rules

- **Input and screenshots only.** Never read or write the game's memory, inject anything, attach a debugger, or touch
  Easy Anti-Cheat, game files or `user.cfg`. The game is played, not instrumented.
- **The owner logs in** to the RSI Launcher and the game. The agent never types a password, a 2FA code or account details.
- No real-money purchases, no pledge store, no chat or messages to other players, no trading of any kind. In-game moves
  a capture needs (requesting a hangar, retrieving a stored ship, travel, departure requests) are fine.
- Stay out of fights: armistice zones and hangars. If the character is attacked or the session goes wrong, stop and report.
- Game pictures are game-derived files: they stay in `out/reference/` (gitignored) and are never committed or uploaded.

## Layout

| Path | What |
| --- | --- |
| `viewer/reference/shots.json` | The shot list: per shot an `id`, `title`, `why`, the viewer setup (`environment`, `load`, `console` steps, fly `camera` in metres, `settle_s`) and the game instructions (`where`, `frame`, `conditions`). |
| `viewer/scripts/reference/Capture-ViewerShots.ps1` | The viewer half: starts its own off-screen viewer (or `-Attach -Port <n>` to a running one), per shot sets the hangar and the vehicle, runs the steps, places the camera and writes `out/reference/<id>/viewer.png` and `viewer.json` (pose, commit, flags). `-Id` picks shots, `-Extra` passes viewer flags (e.g. `-noraytracing`). |
| `viewer/scripts/reference/Take-GameShot.ps1` | The game half, per picture: `-Arm` (notes what is in `LIVE\ScreenShots`), the game's screenshot key (Print Screen by default), then `-Id <id>` files the newest picture that was not there when armed as `out/reference/<id>/game-<variant>.<ext>` with a `game-<variant>.json` note. A key press that wrote nothing is an error, never an older picture. `-Screen` grabs the primary display instead (no arming). |
| `viewer/scripts/reference/ReferenceShots.ps1` | The shared shot-list reader: UTF-8, ids are lower-case letters and digits joined by dashes, unique, not Windows device names. |
| `viewer/scripts/reference/Build-ReferencePage.ps1` | `out/reference/index.html`: per shot the viewer and game pictures side by side; a click flips them in place. |

## Workflow

1. **Viewer first.** `viewer/scripts/reference/Capture-ViewerShots.ps1` (minutes; one viewer at a time). The viewer
   picture is the target framing for the game half.
2. **Game.** The owner starts the RSI Launcher and logs in, starts Star Citizen LIVE and spawns a character (or leaves
   that to the agent once logged in). Then the Codex desktop app (computer use only works there, not in `codex exec`)
   gets the prompt below.
3. **Compare.** `viewer/scripts/reference/Build-ReferencePage.ps1`, open `out/reference/index.html`. Findings become
   fixes in their own WPs; a shot's pair is re-captured on the viewer side after each fix.

Game settings for comparable shots: the owner's usual graphics preset, the field of view and resolution written into
the note once per session, motion blur and film grain off, borderless window mode (so `-Screen` works too).

## Adding shots

Add an entry to `shots.json`: an `id` (lower-case, dashes), what the shot is for, the viewer setup (find the pose with
the fly camera and `/state`), and game instructions a person who knows the game could follow. Prefer places that are
easy to reach in the game: hangars, stations, the ship's own interior.

## The Codex prompt

Paste into a new Codex desktop session whose working folder is this repository:

> You are taking reference screenshots in Star Citizen for this repository. Read
> `docs/work-packages/WP-Q2-game-reference-capture.md` and follow its rules exactly: input and screenshots only, no
> memory reading, injection or anti-cheat contact, no credentials (the owner has logged in), no purchases, no chat, no
> trading. The shot list is `viewer/reference/shots.json`; the viewer pictures to match are
> `out/reference/<id>/viewer.png`. For each shot (or only: `<ids>`): read its `game` instructions and look at its viewer
> picture; get there in the game; match the framing as closely as you can (position, direction, height; the viewer
> picture is the target); hide or note the HUD; run `viewer/scripts/reference/Take-GameShot.ps1 -Arm`; press Print
> Screen; then run `viewer/scripts/reference/Take-GameShot.ps1 -Id <id> -Note '<location, hangar, time of day, helmet or HUD state, FOV,
> anything that differs from the instructions>'` (a second framing of the same shot: `-Variant b`). If a place is not
> reachable, skip the shot and say why. When done, run `viewer/scripts/reference/Build-ReferencePage.ps1` and report which
> shots were taken, which were skipped, and anything in the game that looked different from the viewer picture.

## Done when

- The three hangar force-field shots have viewer and game pictures and the page shows them.
- The force field's look in the viewer is calibrated against them (follow-up WP; the shield_edge strips read as a bright
  flat sheet in the viewer today).
