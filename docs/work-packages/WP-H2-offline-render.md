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
| `warmup_frames` | Frames rendered and discarded at the start pose before frame 0 (default 256) |
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
`rot` (as rendered), `fov` and `ray_tracing_allowed` for the PNG capture view (or the EXR view for EXR-only runs).

## Colour rules

- **`png16` / `png8` (display-referred SDR).** The image as the viewer shows it on an SDR display: the scene through
  the active look and the E1-4 display curve (`HuePreservingShoulder`, or UE's tonemapper under `sc.Look.Display
  ue`), sRGB encoded. Never through the HDR output-device path (E1-4: "HDR output devices remap the SDR result
  through ACES"). `png8` is the same image quantised to 8 bits.
- **`exr` (scene-referred).** Linear scene colour in Rec. 709 / sRGB primaries, after exposure and before the
  tonemapper / display curve, 16-bit half float. The editor grades it (an SDR master or an HDR master from the same
  frames). Record the exposure per frame in the sidecar.
- **Independence from the display.** With scene-colour input frozen, both production capture sources return
  bit-identical RGB half-float samples when the render target advertises HDR10/PQ or SDR/sRGB. If the route
  has to change `r.HDR.EnableHDROutput` (or equivalent), it restores it afterwards.
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
- **View history.** The capture's renderer clock starts at one fixed step and advances once per sample, including
  warm-up. A held sample reuses the same clock value. Materials using world time still start at shot time zero
  and stay there through warm-up. Materials that explicitly use the real-time clock see the fixed sample time,
  including warm-up.
- **No drops.** The writer applies back-pressure: if readbacks or writes are behind, the game thread waits. Frame
  count = `seconds × fps` exactly.
- **Repeatability.** Repeating the request on the same build and display mode uses the same frame times, camera
  poses and look settings. The live PNG16 images must stay within the calibrated temporal variation limits below.

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
and live repeatability checks, and the frozen chart passes the HDR-independence check. Prefer A if it passes
them without fighting the runtime; else B. Engine changes,
if any, follow the WP-E1 rules (fork branch `scviewer/5.9`, UE behaviour as the default).

## Data and code

- `Private/SCRpcRender.{h,cpp}` (new): the route, the path evaluation, the frame loop, the sidecar. The path
  evaluation is a pure function (`FSCRenderPath::PoseAt(t)`) so it is unit-testable.
- `Private/SCRpcSubsystem.cpp`: bind `/render`; `/health` and the state log show a running render
  (`reasons: rendering`).
- `Private/SCRpcVideo.cpp`: `source_format` and the HDR warning (Colour rules).
- `viewer/scripts/rpc/SCRpc.psm1`: `Invoke-SCRender` helper. `viewer/scripts/rpc/render_check.ps1` captures the orbit and
  writes `out/h2/<tag>/report.json`. `viewer/scripts/rpc/render_repeat.ps1` scores two completed live captures using
  `viewer/scripts/rpc/h2-live-tolerance.json` and writes `repeat-report.json` to the candidate directory.
- Docs: this brief's status; `docs/work-packages/WP-H1-rpc-harness.md` API table (`/render`, `/record` preview
  note); `viewer/README.md` RPC section; `docs/03-unreal-export-design.md` if the player or exposure behaviour changes
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
2. **Evenness.** Require consecutive frame indices and timestamps at `n / fps`. Derive the camera's angular
   position from its recorded world position and the independently queried orbit centre. Both that step and the
   recorded yaw step must be within 5% of the authored step. Decode the PNG16 sequence and reject adjacent frames
   with identical RGB. The adjacent-image mean absolute differences in the report describe changes in scene
   content. The same-mode repeat in step 5 also checks whether the image at each index matches its independent
   reference, including held pixels that differ slightly in lighting.
3. **HDR independence.** `SCBridge.RenderGpu.FrozenOutputDevice` renders a fixed GPU colour chart in D3D12.
   Every RGB half-float code must match exactly between the advertised SDR/sRGB and HDR10/PQ targets for each
   production capture source (PNG and EXR). The ordinary display-referred path must differ by more than 1/1023
   at some RGB sample as a positive control. Physical Windows HDR on/off comparisons can provide integration data.
4. **Look.** Compare frame 0 `png16` with a UI-free viewport `/screenshot` at the same pose. For the reference,
   disable engine HDR output and DLSS, set 100% screen percentage and use TSR in both views. Verify the same
   lighting, shadows, reflections and display curve within temporal-history differences. Switch
   `sc.Look.SSDO off` / `game` to confirm that the capture follows the active look.
5. **Live repeatability.** Keep the same viewer session, Windows display mode, render settings and five-second
   TSR Cutlass request. The acceptance fixture uses the production default warm-up and ray-traced capture.
   Calibrate from the first two captures, adding 25% headroom independently to the shot-wide
   and worst-frame mean absolute RGB16 error, then rounding each limit up to a whole 16-bit code (minimum one).
   Compare a third capture against the first using those fixed limits. Both metrics, normalized to full scale,
   must be at or below their respective limits. Report the peak individual channel error for diagnosis.
   Repeat acceptance runs use the checked-in limits. The provided limits were calibrated at 32 warm-up frames
   and also apply to the 256-frame default. Use `-Calibrate` when deliberately establishing a new baseline for
   this fixture and fix the limits before scoring a held-out capture.
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

The full offscreen Cutlass run at 1920×1080 on commit `43f77b9` produced 150 PNG16 and 150 EXR files
in each of three passes. The 149 angular steps were 0.6° each and times were n/30. HDR-on took 0.641 s
per output frame, repeated HDR-on took 0.704 s, and HDR-off took 0.639 s, including warmup. All three
had zero streaming holds. First-pass consecutive-image mean differences ranged from 1324.83 to 1893.17
of 65535 (37% of the mean), so the image-evenness check failed despite the uniform motion.

The Windows display target was verified at 10 bpc with HDR on and 8 bpc with HDR off, then rechecked
at its initial HDR-on, 10 bpc setting. Cross-mode mean absolute error across all RGB channels and all
150 images was 0.000285 full scale. Repeating with HDR still on yielded 0.000317. Both comparisons had
the same peak, 0.516075, in the blue channel of frame 17 at (799,413). Its 16-bit values were 48736,
14915, and 14915 respectively. This exceeded the original 1/1023 live peak gate. The two later runs differed by
0.000318 on average, with a peak of 0.358068. Astra found their cockpit rendering and lighting visually
consistent across display modes, while the SDR viewport had different framing and darker cockpit and
floor reflections than the capture. SSDO's three recorded weights changed from 1 to 0 when disabled.
The five focused automation tests completed in 8.89 s. Local output is under `out/h2/full1`.

A 20-frame repeat control with `-SCShowcase` removed the viewer chrome from the reference. Both
images then framed the ship identically. The viewport still showed a darker cockpit and underbody and
different lamp reflections. With fixed material time and per-view temporal indices the peak RGB error
was 0.508324. The offline view reported FXAA; component registration had reapplied its legacy show
flags. The capture now sets the viewport's temporal AA, motion blur and separate translucency flags
after registration. A four-frame 320×180 H.264 encode was decoded successfully. Local output is under
`out/h2/focused1`. Use `render_check.ps1 -Acceptance -FrameLimit 20` to reproduce only the first 20
samples at the same speed as the full orbit.

The next Win64 build succeeded in 9.96 s of executor time. Two short 320×180 procedural-hangar captures
at the same pose then reported `AAM_TSR` and `HuePreservingShoulder` for both frames and wrote EXR
and PNG8. Their mean RGB errors were 0.027 and 0.062 of 255, with individual channel peaks of 19 and 6.
There was no display-mode change in this control. Output is in `out/h2/aa1` and `out/h2/aa1-repeat`.
At this stage the Cutlass HDR, repeatability, viewport appearance and image-evenness gates were still open.

`render_check.ps1` also checks the authored orbit geometrically. For the Cutlass it queries the actor's
world centre from `/state` after loading. It recovers each angle from the camera's XY position, then
checks the signed step, absolute angle, radius and height against the request, independently of the
reported camera rotation. Against the original 150-frame sidecar and centre (0, 0, 5.427) m, maximum
step error was 0.00220° for a 0.6° step; maximum absolute angle error was 0.00132°, and combined
radius/height error was 0.524 mm. Substituting the previous frame's position into one smoke-test sample
correctly failed this check. The separate live-image difference gate remains unchanged.

`SCBridge.RenderGpu.FrozenOutputDevice` runs a 64×32 GPU colour chart through the stock view-family
post-processing pipeline in a D3D12 game world. The input scene colour is fixed before depth of field;
the float render target advertises SDR/sRGB or HDR10/PQ. Across every RGB sample, the production PNG
and EXR capture sources each had zero difference. Its positive control, the ordinary display-referred
source, produced a peak difference of 0.356934 on the same chart. The six renders took 0.259 s inside
the test, with no physical display changes or imported assets. Run in `-game -AgentMode` with
`-ExecCmds="Automation Now; RunTests SCBridge.RenderGpu.FrozenOutputDevice"` and
`-TestExit="Automation Test Queue Empty"`. This isolates the renderer's output-device selection from
scene history. The real Cutlass HDR result above remains the integration measurement.

After registering the capture with TSR, the 1920×1080 Cutlass test repeated the first 12 samples of the same
authored orbit in one viewer with Windows display mode unchanged. Both sidecars recorded `AAM_TSR`, zero
holds, maximum step error 0.001624° per 0.6° step and maximum radius/height error 0.492 mm. Capture took
6.860 s and 6.911 s. Across all decoding-matched RGB16 samples, normalized absolute repeat error averaged
0.001090499 and peaked at 0.392919814 (25,750/65,535). Both image-evenness tests failed. The first
offline frame against the UI-free viewport at the same recorded camera pose had mean absolute error
0.056784791 and peak 0.861951629, expanding each viewport RGB8 code to RGB16 by multiplication by 257.
Visual review found matching ship silhouette and hangar framing, with stronger grouped wing lamps in the
viewport, different floor shadows and more colourful cockpit highlights in the offline frame.
This exceeded the original 0.0001 live peak limit. At that point the viewport appearance gate remained open. The comparison
files, camera sidecars and per-frame metrics are in `out/h2/tsr1080`.

## Frozen HDR and calibrated live rerun (2026-11-21)

The Win64 build, D3D12 frozen chart and focused NullRHI automation passed. The frozen chart matched all 2,048
RGB half-float pixels for each production source. The display-referred control differed at all 2,048 pixels and
had a peak error of 0.356934. The chart took 0.084 s inside the test. The five NullRHI tests ran in one session.

All three TSR Cutlass captures used one offscreen D3D12 viewer session with Windows display mode held constant
on an RTX 4090 (driver 621.24). Each wrote 150 PNG16 and 150 EXR frames at 1920×1080, with 32 warm-up frames,
zero holds and 0.002203° maximum camera step error against the 0.6° authored step. Capture took 70.21, 71.78
and 70.86 s. The separate consecutive-image check still fails. Differences spanned 53.1–53.3% of the mean in
the three captures.

| Normalized RGB16 error | Calibration pair | Fixed limit | Held-out repeat |
| --- | ---: | ---: | ---: |
| Shot-wide mean absolute error | 0.000799492 | 0.001007095 | 0.000911014 (pass) |
| Worst-frame mean absolute error | 0.001109958 | 0.001388571 | 0.001536248 (fail, frame 4) |

The held-out peak individual channel error was 0.638178. The live tolerance stays at the calibrated values.
That live repeatability run failed. At that point the image-evenness and viewport appearance checks were still open.
Logs, sidecars and the scorer report are under `out/h2/acceptance-hdr`.

### Frame 4 investigation

Twelve frames in the original held-out pair exceeded the fixed worst-frame limit, all between frames 4 and 16.
Frame 4 had the largest mean absolute RGB16 error, 0.001536248. The sidecars agreed on camera pose, time, TSR and
streaming holds. Capture exposure differed by 0.082%. Astra found matching geometry and localized the most
visible change around the left wing lamp. That region contributed 5.4% of the frame's RGB error. The hangar
walls also varied.

In a serialized viewer session, repeating the first 12 frames with Lumen diffuse GI disabled brought frame 4
to zero in all three pair comparisons. The worst error at any frame in those controls was 0.000000023. With
Lumen enabled, the spike persisted after disabling one-pass virtual-shadow projection, MegaLights, bloom or
lens flare. Disabling Lumen screen-probe screen traces reduced the early error. Forcing full Lumen surface-cache
lighting updates during warm-up and serializing its CPU updates both left over-limit pairs. These controls
used modified lighting and did not run as acceptance captures.

With the production look, doubling the start-pose warm-up reduced the three short pair comparisons' worst
errors from 0.001373–0.001748 at 32 samples to 0.001001–0.001051 at 64. Astra found matching scene appearance
at normal viewing scale, with small differences in the wing lamp halo, cockpit reflections and specular edges.
The short captures took about 0.8 s longer. That revision defaulted to 64 and honoured explicit counts,
including zero. The acceptance script followed the runtime default. The original calibration values and source
hashes remained fixed.

Short controls and regional measurements are under `out/h2/frame4-diagnostics`, `out/h2/frame4-controls`,
`out/h2/frame4-lumen` and `out/h2/acceptance-hdr/frame4-regions.jsonl`.

### Full rerun with the 64-frame default

All three full TSR Cutlass captures ran in the same offscreen D3D12 viewer with the display mode held constant.
Each wrote 150 PNG16 and 150 EXR frames, passed the camera and `t = n/30` checks, and reported zero holds.
Capture times were 80.72, 79.94 and 80.21 s. Every pair passed both original RGB16 limits.

| Full capture pair | Shot MAE (limit 0.001007095) | Worst-frame MAE (limit 0.001388571) | Frame 4 MAE |
| --- | ---: | ---: | ---: |
| First / second | 0.000776574 | 0.001083233 (frame 6) | 0.000902020 |
| First / third | 0.000829478 | 0.001120893 (frame 6) | 0.000947411 |
| Second / third | 0.000804702 | 0.001063072 (frame 13) | 0.000929853 |

Peak individual RGB channel differences ranged from 0.687434 to 0.732341. The former consecutive-image
spread rule failed, with differences spanning 53.3–53.5% of the mean. Viewport appearance remained open.
The Win64 build, frozen HDR chart and all five focused NullRHI tests passed with builds and sessions serialized.
The scheduling test failed at 32 before the fix and passed at 64 afterward, including the override assertions.
Logs, sidecars and numerical reports are under `out/h2/frame4-warmup-fix`.

### Judder metric and viewport lighting investigation

The original adjacent-image rule rejected a geometrically uniform 0.6° orbit because the consecutive RGB errors
spanned 53.3% of their mean, even though every timestamp was correct and every adjacent frame differed. A second
control used a held camera followed by a double step on a brightness ramp. One frame's brightness offset made
all differences equal, so the original test passed the faulty motion.

The revised orbit test rejected an injected held position, a held yaw and a timestamp slip in separate controls.
The RGB check caught an exact repeated frame. The same-mode repeat scorer rejected both that repeated frame and
a repeat brightened by one 16-bit code, in each case at the injected index 4. These controls are under
`out/h2/evenness`.

With engine HDR output disabled, matching the viewport's AA to the capture's TSR reduced the visible difference
around the wing lamps. It left the dark viewport canopy and broad under-hull shadow missing from the offline
view. Astra confirmed that enabling ray tracing in the capture restored both, and that disabling MegaLights
made both views use the brighter lighting. Disabling bloom removed the lamp halos. Disabling Lumen GI,
reflections or SSDO separately left the dominant shading mismatch.

Unreal's scene-capture component defaults to opting out of ray tracing. The MegaLights hardware path checks
that permission. Both H2 capture components now opt in when ray tracing is available, following the same
path as the viewport. With the runtime CVar test the matched-TSR mean absolute RGB difference dropped from
0.05629 to 0.02021. Astra still saw a grainier offline floor shadow and softer wall lighting in the viewport.
The lighting controls are under `out/h2/appearance` and `out/h2/appearance-ray`.

The compiled change reproduced the appearance improvement at 64 warm-up frames (mean absolute RGB error
0.02000), and the 256-frame stationary control reduced it to 0.01735. Consecutive viewport snapshots differed
by about 0.0015. Astra confirmed persistent offline floor-shadow grain and brighter viewport wall lighting
behind the rear pods. Disabling Lumen reduced the cross-path error to 0.01256 and largely removed the
wall-lighting mismatch. The floor grain persisted. The remaining look differences stayed open at this stage. These
controls are under `out/h2/appearance-fixed`.

At 64 warm-up frames, the first full ray-traced repeat exceeded both original live limits (shot MAE
0.001109399 and worst frame 0.001812741 at frame 5). The first 16 frames averaged 0.001686, and every
frame from 40 onward passed the per-frame limit. In short repeats, 128 warm-up frames still produced
two over-limit pairs. At 256 all three worst-frame errors were between 0.000710 and 0.000912 with the
default MegaLights samples. The 256-frame short control took about three seconds longer than the
128-frame control. The runtime now defaults to 256. Callers can still request their own count, and
the quick test uses two. The fixed thresholds and calibration hashes stayed unchanged.

The serialized Win64 build, frozen HDR chart, five focused NullRHI tests and full 256-frame-warm-up rerun
passed. All three 150-frame captures reported zero timestamp error, a maximum angular step error of 0.002203°
against the 0.6° target, zero streaming holds and no repeated images. Capture times were 84.01, 84.78 and 84.55 s.

| Full ray-traced capture pair | Shot MAE (limit 0.001007095) | Worst-frame MAE (limit 0.001388571) |
| --- | ---: | ---: |
| First / second | 0.000698408 | 0.000870318 (frame 33) |
| First / third | 0.000780572 | 0.001003696 (frame 19) |
| Second / third | 0.000759471 | 0.000975883 (frame 19) |

Astra confirmed the floor grain and rear-wall differences in the final matched-TSR pair. Its cross-view
mean RGB error was 0.01735, compared with 0.00139 between two viewport screenshots. Switching SSDO off changed
all three recorded weights from 1 to 0. Logs, captures and reports are under `out/h2/evenness-look-256`.

The final metadata build passed a 320×180 procedural-hangar smoke run. The two-frame warm-up and eight-frame
orbit took 1.67 s with no repeated images. The PNG+EXR sidecar recorded ray-tracing permission enabled at
the default and disabled under the engine override. The EXR-only sidecar also recorded the default correctly.
The repeat scorer rejected missing, disabled and mid-shot-changing permission in the ray-traced fixture.

### View history and the remaining floor and wall differences

In the same-pose baseline, four fixed 1920×1080 regions isolated two effects. The offline floor boxes had
about 0.025–0.027 mean absolute high-pass luma error against their local 5×5 average, versus about 0.003
in the viewport. The viewport wall boxes averaged 0.658–0.664 luma, while the offline boxes averaged
0.339. Repeating the stock capture reproduced both. Extending MegaLights history and forcing more Lumen
surface-cache work left the differences visible.

Astra verified that disabling Lumen screen traces dimmed the viewport wall halos to match the original
offline wall, and that disabling MegaLights smoothed the floor by removing the intended broad shadow.
These controls changed the lighting of the reference scene. The renderer also caps this build's MegaLights
per-pixel sample CVar at four effective samples, so setting it to 16 served as a repeat control.

The shared cause was H2's use of shot time for both world and renderer clocks. Warm-up kept both at zero.
Unreal treats a near-zero previous renderer timestamp as an uninitialized view. It discards previous-view
data such as the scene colour and depth that Lumen screen traces need, and its reset flag prevents
MegaLights from using temporal history. A deterministic positive renderer clock now advances during
warm-up. The authored clock and output timestamps retain their fixed-step schedule.
`SCBridge.RpcRender.Clock` failed on the old clock and exercises the transition into frame zero.

| Matched-TSR frame 0 | Viewport | Offline before | Offline after |
| --- | ---: | ---: | ---: |
| Under-wing floor high-pass luma, left / right | 0.00308 / 0.00287 | 0.02484 / 0.02659 | 0.00306 / 0.00284 |
| Rear-wall luma, left / right | 0.659 / 0.665 | 0.339 / 0.339 | 0.658 / 0.666 |

Disabling Lumen screen traces after the fix dimmed both views' rear-wall boxes to 0.337–0.342. With the
production settings, whole-image normalized RGB error fell from 0.01735 to 0.00428. Two consecutive
viewport screenshots in the new run differed by 0.00143.

**Viewport appearance passes for the matched-TSR Cutlass fixture.** Astra verified that both floor shadows
are smooth and the wall halos match both the new and original viewport references. Minor tonal and sampling
differences remain around bright gradients. Future comparisons can use the two viewport screenshots to
establish the reference's own variation. The older controls are in `out/h2/appearance-residuals`, and the
fixed captures and regional metrics are in `out/h2/appearance-render-clock`.

The serialized Win64 build, exact frozen HDR chart and all six focused NullRHI tests passed. Three
150-frame ray-traced orbits at 1920×1080 passed cadence and PNG16/EXR frame-count checks with zero timestamp
error, zero holds and no repeated RGB frames. Capture times were 87.24, 89.02 and 88.48 s.

| Full capture pair | Shot MAE (limit 0.001007095) | Worst-frame MAE (limit 0.001388571) |
| --- | ---: | ---: |
| First / second | 0.000718650 | 0.000897713 (frame 0) |
| First / third | 0.000730857 | 0.000943380 (frame 27) |
| Second / third | 0.000728906 | 0.000904639 (frame 0) |

All comparisons used the unchanged checked-in limits and the 256-frame warm-up.
