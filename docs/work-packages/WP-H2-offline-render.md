# WP-H2 — Offline render for video (deterministic, display-independent)

The viewer renders a camera path frame by frame at a fixed timestep and writes every frame at full precision, without
reading the swapchain. This is the source of final Unreal footage for the video series in the sibling repo
`../SC_UE_VideoSeries` (`docs/pipeline.md`, "Capture quality"). The real-time `/record` (WP-H1) stays as a preview
and a stability-run tool.

Branch `wp/H2-offline-render`. Agent: Opus 5.5 (Codex cross-review). Depends on: H1 (RPC harness), E1-4 (the
engine's display curve). Locks: UE.

## Why

Every capture of the viewer so far blew out the HDR, juddered, or both (Snipping Tool, the H1 recorder, the ffmpeg
path). The causes, as of `e742978`:

- **Colour.** `SCRpcVideo.cpp` `ToColors` turns the back buffer into 8-bit BGRA. It cuts `PF_A2B10G10R10` (ST 2084
  PQ when an HDR output device is active) to its top 8 bits and clamps `PF_FloatRGBA` (scRGB). There is no
  HDR→SDR conversion, so highlights clip and colours wash out. Anything that grabs the swapchain off an HDR display
  has the same problem.
- **Time.** `/record` grabs frames in real time and stamps their wall-clock capture times. A slow frame is dropped
  (`MaxInFlight`, `MaxReadbacks`) or shows up as a repeat after encoding, so motion judders.
- **Camera.** A driver that moves the camera by calling `/camera` at the frame rate is not in step with the frames
  being rendered, which adds judder of its own.

## Goal

A `/render` RPC route (desktop non-Shipping builds, like the rest of H1) that:

1. advances the world by exactly `1 / fps` per output frame (fixed timestep), whatever the render cost;
2. evaluates the camera path inside the engine at each frame's time, from a path handed over once;
3. writes every frame (no drops, no repeats); the game thread waits on the writer instead of dropping;
4. produces output independent of the display. No HDR output device for the render; frames come from the
   renderer's own targets, not the swapchain or the viewport's back buffer;
5. answers when the last frame is on disk, with a per-frame sidecar (time, camera pose) for verification and the
   edit.

## The route

`/render` body (JSON; positions in world metres, rotations `[pitch, yaw, roll]` degrees, like every H1 route). The
action shapes are the same as the video pipeline's `ViewerAction` (`../SC_UE_VideoSeries/src/pipeline/types.ts`), so
it can pass them through unchanged:

| Field | Meaning |
|---|---|
| `name` or `path` | Output folder (frames + sidecar). Default `<-SCShotDir or Saved/Renders>/<name>/` |
| `fps` | Output frame rate (30) |
| `seconds` or `frames` | Length |
| `width`, `height` | Output size (default the viewport's); independent of the window under `-AgentMode` |
| `outputs` | Any of `exr` (linear, see below), `png16` (display-referred SDR, 16-bit), `png8` (SDR, 8-bit) — default `["png16"]` |
| `warmup_frames` | Frames rendered and discarded before frame 0 at the start pose (default enough for TSR/DLSS history and Lumen to converge; measure and document the default) |
| `setup` | Actions applied before warm-up (start pose, console commands) |
| `steps` | Timed actions, `at` seconds from frame 0 (see below) |
| `encode` | Optional: `prores` (ProRes 4444 from `png16`) or `h264` via `-SCFfmpeg`; frames are kept either way |
| `timeout_s` | Answer deadline (default generous; a render may take minutes) |

Actions (each step has `at`; `setup` items have none):

- `{ "camera": { … } }`: the `/camera` body as is (`mode`, `at` + `look_at` / `rot`, `focus`).
- `{ "console": "sc.Look.SSDO off" }`: runs at the start of that frame.
- `{ "move": { "from": Pose, "to": Pose, "seconds": s, "ease": "linear" | "inOut" } }`: `Pose` = `{ "at": [x,y,z],
  "lookAt": [x,y,z] }` or `{ "at", "rot" }`. Both poses aim the same way. `inOut` is smoothstep `u²(3−2u)`. In
  `setup`, only `from` is applied.
- `{ "orbit": { "center": [x,y,z] | "vehicle", "radius": m, "height": m, "fromDeg": a, "toDeg": b, "seconds": s,
  "ease": … } }`: the camera at `center + (r·cos θ, r·sin θ, height)` looking at the centre, θ eased from `fromDeg` to
  `toDeg` (orbit default ease `linear`). `"vehicle"` is instance 0's position.

Answer: `ok`, `path`, `frames`, `fps`, `outputs`, `warmup_frames`, `render_s` (wall time), `files` (first and last
frame per output), `sidecar`, `encoded` (when `encode`). `ok: false` with `error` on timeout or a failed write, with
the frames written so far kept.

`/render` with `action: "status"` while one runs returns progress (`frame`, `of`, `render_s`). A second `/render`
while one is running is refused. `/quit` during a render stops after the current frame and finishes the sidecar.

Sidecar `render.json`: the request as received, engine/branch/commit, `r.*` values that affect the image
(screen percentage, AA method, Lumen, the look's settings), and per frame: `index`, `t` (seconds), camera `at`,
`rot` (as rendered), `fov`.

## Colour rules

- **`png16` / `png8` (display-referred SDR).** The image as the viewer shows it on an SDR display: the scene through
  the active look and the E1-4 display curve (`HuePreservingShoulder`, or UE's tonemapper under `sc.Look.Display
  ue`), sRGB encoded. Never through the HDR output-device path (E1-4: "HDR output devices remap the SDR result
  through ACES"). `png8` is the same image quantised to 8 bits.
- **`exr` (scene-referred).** Linear scene colour in Rec. 709 / sRGB primaries, after exposure and before the
  tonemapper / display curve, 16-bit half float. The editor grades it (an SDR master or an HDR master from the same
  frames). Record the exposure per frame in the sidecar.
- **Independence from the display.** The same request renders identical images with Windows HDR on and off. If the
  route has to change `r.HDR.EnableHDROutput` (or equivalent) to get there, it restores it afterwards.
- `/record` (H1) keeps its 8-bit path. It must report `source_format` and log a warning when the source is HDR
  (`PF_A2B10G10R10` with an HDR output device, or `PF_FloatRGBA`) instead of silently clipping. Its docs say
  "preview only" and point here.

## Determinism

- **Fixed timestep.** World time (and everything ticked from it: animation, Mannequin fragments, Niagara, lights
  with authored states, the first-person body) advances by exactly `1 / fps` per output frame; no real time is read
  while rendering.
- **Camera per frame.** The camera pose for frame *n* is evaluated at `t = n / fps` on the game thread before that
  frame's view is set up. Animated actions run from their `at` for their `seconds`; instant actions fire on the first
  frame with `t ≥ at`.
- **Complete frames.** Before frame 0 (after `setup`): the H1 `settled` condition, then `warmup_frames`. During the
  render, a frame whose textures or Nanite pages are still streaming in is held: rendered again, not written, until
  complete, without advancing time. Count holds per frame in the sidecar.
- **No drops.** The writer applies back-pressure: if readbacks or writes are behind, the game thread waits. Frame
  count = `seconds × fps` exactly.
- **Repeatability.** The same request on the same build renders the same frames (allowing for GPU non-determinism in
  temporal effects; measure it and state the bound).

## Decision to make first (record it as a plan decision)

Spike both, at most a day each, then pick one and write down why in `docs/02-implementation-plan.md` (Architecture
decisions):

- **A. Movie Render Queue / Graph at runtime.** Enable the MovieRenderPipeline plugins; render through
  `UMoviePipelineQueueEngineSubsystem` in `-game` (also under `-AgentMode`). A transient level sequence with a camera
  cut to a camera actor that the viewer drives per frame from the path (or keys the path into the sequence). This
  gives MRQ's warm-up, temporal and spatial sample accumulation (real motion blur, AA without history artefacts), EXR
  / PNG writers and render passes. The render passes (base colour, normals, lighting-only) would be footage for the
  frame-anatomy episode. Check: runtime-spawned content (the imported ship, environment and lights are not in any
  saved level) renders, the viewer's look and E1-4 curve apply, and the runtime path works in the fork.
- **B. Own fixed-step writer.** `FApp::SetUseFixedTimeStep(true)` + `SetFixedDeltaTime(1 / fps)` for the render's
  duration. Reuse the H1 scene-view-extension readback (`FSCRecordExtension`), but read the post-tonemap scene colour
  (SDR) and the pre-tonemap scene colour (EXR) instead of the family's render target. Writer tasks for PNG/EXR via
  `IImageWrapper`, and back-pressure instead of drops. Smaller and fully under our control; no sample accumulation.

Acceptance for the spike: a 5 s orbit of `DRAK_Cutlass_Black` in the default hangar meets the frame count, evenness
and HDR-independence checks below. Prefer A if it passes them without fighting the runtime; else B. Engine changes,
if any, follow the WP-E1 rules (fork branch `scviewer/5.9`, UE behaviour as the default).

## Data and code

- `Private/SCRpcRender.{h,cpp}` (new): the route, the path evaluation, the frame loop, the sidecar. The path
  evaluation is a pure function (`FSCRenderPath::PoseAt(t)`) so it is unit-testable.
- `Private/SCRpcSubsystem.cpp`: bind `/render`; `/health` and the state log show a running render
  (`reasons: rendering`).
- `Private/SCRpcVideo.cpp`: `source_format` and the HDR warning (Colour rules).
- `ue/scripts/rpc/SCRpc.psm1`: `Invoke-SCRender` helper; `ue/scripts/rpc/render_check.ps1` runs the verification
  below and writes `out/h2/<tag>/report.json`.
- Docs: this brief's status; `docs/work-packages/WP-H1-rpc-harness.md` API table (`/render`, `/record` preview
  note); `ue/README.md` RPC section; `docs/03-unreal-export-design.md` if the player or exposure behaviour changes
  under a fixed timestep.

## Tests (automation, failing first)

- `SCBridge.RpcRender.Path`: `move` (linear, inOut end points and midpoint), `orbit` (radius, height, look-at, angle
  at start / middle / end), mixed `lookAt` / `rot` refused, instant actions fire on the first frame with `t ≥ at`.
- `SCBridge.RpcRender.Schedule`: frame times are `n / fps` with no drift over 10 000 frames; frame count =
  `round(seconds × fps)`; `frames` overrides `seconds`.
- `SCBridge.RpcRender.Request`: body validation (unknown output, `at` outside the length, both `seconds` and
  `frames`), refusal while a render runs.
- `SCBridge.RpcRender.Encode`: a synthetic float frame round-trips through the EXR writer within half-float precision;
  the PNG16 path produces the same 8 MSBs as PNG8.
- `SCBridge.RpcVideo.SourceFormat`: `/record` reports the source format and flags HDR sources.

## Verification (GPU, in the PR description)

1. **Frame count and timing.** `/render` 5 s at 30 fps, 1920×1080, orbit of `DRAK_Cutlass_Black` (`-SCDefaults`),
   `outputs: ["png16", "exr"]`. Exactly 150 frames per output, sidecar `t` = `n/30`.
2. **Evenness.** The camera's angular step between consecutive frames (from the sidecar and, independently, from an
   image-based check: mean absolute difference between consecutive frames) varies by less than 5 % across the shot,
   with no zero-difference (repeated) frame.
3. **HDR independence.** The same request with Windows HDR on and off: per-pixel difference of the `png16` frames
   ≤ 1/1023 (or bit-identical); highlights not clipped where the HDR-off render isn't.
4. **Look.** Frame 0 `png16` vs a `/screenshot` of the same pose with the HDR device off: same image within TSR/DLSS
   history differences; `sc.Look.SSDO off` / `game` change the render.
5. **Repeatability.** The request twice: state the measured difference.
6. **Off screen.** All of the above under `-AgentMode` (`Start-SCViewer` without `-Visible`).
7. **Cost.** Render wall time per frame for the orbit, with and without the streaming holds.

## Consumer

`../SC_UE_VideoSeries` (`src/pipeline/viewer.ts` `captureShot`) moves its `record` shots to `/render` when this
lands: it sends the shot's `setup` and `steps` as they are and takes `png16` or `exr` per episode format. Note in the
PR what that repo needs to change (field names, defaults), so its pipeline can follow in the same week.

## Out of scope

- Audio in renders (the edit carries sound; WP-A1 is separate).
- Rendering the live game (Star Citizen) or Nsight captures: those are hand-captured footage in the video repo (OBS).
- Shipping builds (`/render` lives with the RPC harness, desktop non-Shipping only).
- Replacing `/record` for stability runs (`explore.ps1 -Video` keeps it).

## First implementation and runtime checks (2026-11-20)

The fork's Movie Pipeline PNG writer routes PNG through `FAsyncImageQuantization` and calls
`QuantizeImagePixelDataToBitDepth(..., 8, ...)` in `MoviePipelineImageSequenceOutput.cpp`. The runtime
spike uses two persistent 16-bit float scene captures. The fork explicitly maps `SCS_FinalToneCurveHDR`
and `SCS_FinalColorHDR` to separate tonemapper output devices before checking the physical render target.
Scene captures reset Lumen GI, reflections and surface-cache resolution by default. The capture extension
restores those three settings from the game view.

Win64 Development build passed. In one NullRHI session, `SCBridge.RpcRender` (Path, Request, Schedule,
Encode) and `SCBridge.RpcVideo.SourceFormat` passed, including decoded half-float EXR values above 1
and PNG16 / PNG8 high-byte agreement. Offscreen `render_check.ps1` on the procedural hangar wrote eight
frames each of PNG16, PNG8 and EXR at 320×180 in 1.092 seconds. Pose steps and frame times matched, with
zero streaming holds and zero identical neighbouring images. Mean absolute image differences varied by
about 9% across this short orbit. The numeric 5% image-difference gate remains open.
