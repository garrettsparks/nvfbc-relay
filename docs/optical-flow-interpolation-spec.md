# Optical-flow interpolation (`o`), rebuilt on the D3D11 present

Status (2026-10-23): **design; the pure-code part and a probe are built.** The choices here are
defaults for the user to change. Research done 2026-10-23 (sources at the end). The probe ran on
the rig the same day: the 5080's optical flow engine works and its work is not on the game's
graphics engine (6.1).

## 1. Why

When a present's target falls between two captured frames and neither is close enough to pass
through, the blend mode shows `lerp(before, after, w)`. On anything that moves, that is a double
image: two copies of a moving edge at 1-w and w opacity. An interpolator that moves each pixel
along its motion instead shows one edge, at the place it would have been at the target instant.

Synthesis is rare where the relay is used most: at a locked 60 into 60 it ran on 0.7 to 0.8% of
presents in the 2026-10-22 streams. It is common when the source runs at a rate the output does
not divide: a 90 fps source into 60 Hz puts every other present between two frames, so about 30
presents a second are synthesized. That case is what this mode is for, and the one its tests
force.

## 2. What exists today, and why it is rebuilt rather than run

The current `o` (`o`, `o:vsync`, `o:<fps>`; hidden from the help) lives on the D3D9 swapchain: a
D3D11 sidecar device opens the ring slots by shared handle, converts both bracket frames to
8-bit, runs NVOFA forward flow (4x4 grid, medium preset, no cost, no backward flow), warps with
`out(p) = lerp(before(p - w F(p)), after(p + (1-w) F(p)), w)`, spins the CPU until the GPU is done,
and copies the result back into the D3D9 back buffer. Per synthesized present: two conversion
draws, one flow, one warp, two cross-API copies and a CPU spin, all on the present thread, and
the flow is recomputed for every synthesized present even when the pair has not changed.

**No `o` frame has ever been rendered on hardware** (the logs, fixtures and docs record none). It
captures only through NvFBC (`-wgc` is turned off for any mode not on the D3D11 present), and it
cannot use the copy-engine present. An earlier CUDA prototype (March 2026, its own branch) did
run on the RTX 5080; its records are the only hardware numbers for optical flow in this project:
flow 0.6 to 0.8 ms on the desktop, 9 to 16 ms of wall time in heavy gameplay ("queued behind the
game"), and stalls traced to CUDA and D3D9 interop and the D3D9 device's lock, both of which a
D3D11-native design avoids.

So the rebuild moves `o` onto the D3D11 present (`b:vsync`'s backend), where the frames already
are D3D11 textures, the warp can draw straight into the present buffer, and `-wgc` and the copy
engine come for free.

## 3. What the research found

### 3.1 How FFmpeg does it (`vf_fruc_vulkan`, merged 2026-08-29, LGPL; read, not copied)

- Flow in **both directions, once per source pair**, on the optical flow engine through
  `VK_NV_optical_flow`; the pair's flow is reused for every output frame between the two.
- Input is 8-bit luma (fixed BT.709 weights) at full size. Grid and preset are options (default
  finest grid and SLOW); its author needed grid 2 and MEDIUM to run 2160p24 comfortably, a pixel
  rate close to 1440p60.
- The warp finds, for each output pixel, the source pixel whose own motion lands there: a damped
  fixed-point iteration `s0 = mix(s0, p - w F_fwd(s0), 0.5)` six times (and `s1` with the backward
  flow and 1-w), then `mix(f0(s0), f1(s1), w)` with bilinear sampling. Our prototype reads the
  flow at the output pixel instead, which misplaces moving edges.
- One artifact guard: where the two frames already agree at the unwarped position (luma
  difference well under 0.03) but the warp would reach more than 5 to 20 px, it falls back to the
  plain crossfade. This also protects an opaque static HUD over a moving scene, since the HUD's
  pixels agree with themselves across the pair.
- No occlusion handling, no forward/backward consistency check, no scene-cut detection (a user
  reports artifacts at cuts). Its author's own comparison with NVIDIA's FRUC library: "dumber, but
  doesn't result in massively worse quality". No published speed or quality numbers.
- NVIDIA's FRUC library adds what FFmpeg lacks: a forward/backward consistency check that rejects
  bad vectors, infill of the rejected ones, image-space hole filling, and a repeat of the previous
  frame when the result misses a quality bar. Its DLL cannot ship in this repository (SDK license;
  FFmpeg's author reached the same conclusion), and it was rejected earlier on quality.
- `vf_minterpolate` (software block matching) contributes one cheap idea worth taking: its
  scene-change score, `min(mafd, |mafd - previous mafd|)` on a 0 to 100 scale with a threshold
  near 10.

### 3.2 NVIDIA hardware against NVIDIA's AI models

- **The Optical Flow SDK is frozen at 5.0 (February 2023) and no NVIDIA document names
  Blackwell.** The RTX Blackwell whitepaper never mentions the optical flow accelerator (the Ada
  one lists it, with its own spec rows), and says DLSS 4 "replaced hardware optical flow with a
  very efficient AI model". The API still works on the 5080 (the CUDA prototype ran it), but
  whether the work runs on a separate engine or on the shader cores the game uses is NOT known.
  It is the premise of the whole cost argument and is tested first (6.1).
- Published Ada numbers, one direction, 1080p: 4x4 grid 536 / 1000 / 1296 flows a second at SLOW /
  MEDIUM / FAST; 2x2 grid 210 / 336 / 711; 1x1 grid 98 / 113 / 222. Scaled by pixel count to 1440p
  (a guess): 4x4 MEDIUM about 1.8 ms a direction.
- The D3D11 interface (our vendored headers, interface 5.0) supports both directions in one
  execute (`NV_OF_PRED_DIRECTION_BOTH`), an 8-bit cost per vector (`enableOutputCost`, UINT8),
  external hints and grid 1/2/4 on Ampere and later. Input formats: GRAYSCALE8, NV12, ABGR8. The
  headers are MIT; the runtime ships in the driver; nothing needs redistributing.
- **DLSS 4 frame generation** (the AI model, on tensor cores) takes the game's depth and motion
  vectors and camera data through Streamline; there is no image-only mode and no API for
  arbitrary frames. NVIDIA attributes transformers to DLSS super resolution and ray
  reconstruction, not to frame generation. **Smooth Motion** is a driver setting with no API: it
  inserts one midpoint frame per rendered pair, which does not fit a 60 Hz output fed at 60.
- **Maxine Video Effects SDK 1.3, Video Frame Generation** is an NVIDIA AI interpolator that does
  take two arbitrary frames and any timestep (Ada and Blackwell). It runs on the shader and tensor
  cores (1.9 ms at 1280x720 on an RTX 5090; a guess of 10 to 15 ms at 1440p on a 5080, taken from
  the game), needs CUDA memory, a 600 MB+ runtime behind an NGC key, and cannot ship in an MIT
  repository. No desktop end-user installer with it was found.

### 3.3 Other ways to get motion or frames

| option | motion from | cost at 1440p | license / runtime | fit |
|---|---|---|---|---|
| NVOFA + own warp | the optical flow engine, image only | ~1.8 ms a direction on Ada, engine time (guess); warp under 1 ms of shader time (guess) | MIT headers, driver runtime | the plan |
| AMD FidelityFX Optical Flow (FSR 3's flow pass) | 8x8 block search on a luma pyramid, image only, with scene-change detection | no NVIDIA timing; AMD's whole FG pass 1.0 ms at 1440p on an RX 7900 XTX | MIT source; D3D12 / SM 6.2 wave ops, no D3D11 backend | the vendor-neutral fallback if NVOFA fails its premise |
| RIFE 4.2x (neural) | learned, image only | guess 4 to 7 ms full scale, 2 to 3 ms lite or half-scale flow, on the shader cores | MIT code and weights; TensorRT or ncnn plus CUDA interop | quality reference, measured offline |
| Maxine VFG (neural) | learned, image only | guess 10 to 15 ms | not redistributable | only if the flow warp's artifacts prove unacceptable |
| FSR 3.1 / XeSS frame generation | the game's motion vectors and depth | | | not usable: the relay has neither |
| Lossless Scaling, AFMF, Smooth Motion, FSR 4 | | | proprietary or Radeon-only | quality references only |
| DIS, Lucas-Kanade, Farneback in compute shaders | classic dense flow | Farneback ~8 ms, LK 20+ ms (NVIDIA's Turing comparison) | own work | not better than the above per unit cost |

### 3.4 The answer to "hardware or the transformer model"

Use the optical flow hardware through D3D11, with our own warp, **if the premise test in 6.1
shows its work does not run on the game's shader cores.** No NVIDIA AI interpolator is both usable
on arbitrary frames and shippable here, and every AI path costs shader time the game pays for.
If the premise fails (the driver runs NVOFA on the shader cores on Blackwell), the comparison
reopens between NVOFA as it then costs, FidelityFX Optical Flow, and RIFE, on the numbers from
section 6.

## 4. Design

### 4.1 The mode

`o:vsync` (and bare `o`) present through the D3D11 flip-model backend, exactly as `b:vsync`
does, with the same policy: `DecideComposite` decides pass-through, hold or synthesize, with the
same passthrough threshold, lock and lookahead. Only the synthesis executor differs. `-wgc`, the
copy engine, `-wgcscale` and the marker work unchanged. The D3D9 `o` path (sidecar, D3D9
compositor) is removed: it never ran, and nothing else uses it. (A decision for the user, 8.1.)

### 4.2 Flow ahead of the present

The relay presents about 95 ms behind capture, so every frame a present will need has arrived
several presents before it is shown, and the targets of the next few presents are known (each is
one output period after the last, give or take the lock's pull). At each present the backend
predicts the brackets of the next two or three presents; for a bracket whose target sits outside
the passthrough gate on both sides (a predicted synthesis) it submits that pair's flow now, unless
the pair is already cached or queued. When the synthesized present comes, the flow is done, and
the present thread only draws the warp.

- **Cache:** flow fields keyed by the pair's two capture stamps, a handful of slots (four is
  enough for a two-to-three-present horizon), least recently used out. A 60 into 60 hole, where
  consecutive presents share one pair, computes its flow once.
- **Late or missing flow:** if the predicted pair was wrong (a stall, a lock move, a late frame)
  the present submits the flow itself and waits a bounded time on an event query; past the bound
  it draws the blend for that present (`sx=blend`, the existing fallback marker), never a repeat
  (a blend is always better than a repeat or a skip).
- **Never for pass-through:** a present predicted to pass through costs no flow. At a locked 60
  into 60 the mode therefore costs almost nothing over `b:vsync`; at 240 into 60 nearly nothing.
- The prediction is pure code in `src/core` (it needs the ring's stamps, the next targets and the
  gate), so the suite tests it on synthetic timelines.

### 4.3 The flow engine

- One NVOFA session on the present's D3D11 device and immediate context (the same device the
  warp draws on; no sidecar, no shared output, no CPU spin).
- `NV_OF_PRED_DIRECTION_BOTH` with `enableOutputCost` (UINT8): forward and backward fields and
  their costs from one execute.
- Grid and preset are measured before they are chosen (6.2). Starting point: grid 4, MEDIUM.
- **Input:** with `-wgc` the frames are BGRA8 textures; registered as ABGR8 they reach the engine
  with red and blue swapped, which motion search does not care about (to be checked, 6.1), so the
  flow needs no conversion draw and, if the engine is separate, takes no turn on the game's
  graphics engine at all. NvFBC's ring slots are 10-bit, which the engine does not take: one
  conversion draw to GRAYSCALE8 per frame, cached per ring slot so each frame converts once.
- **Temporal hints off** unless the previous execute was the adjacent pair (the header warns
  hints from an unrelated pair degrade the result).
- **Scene change:** the mafd score of 3.1 on the two 8-bit inputs is not free on the GPU; the
  first version takes it from the flow's mean cost instead (a cut has a high cost everywhere), and
  falls back to the blend for that pair. To be checked against cuts in the corpus video.

### 4.4 The warp (v2)

One draw into the present buffer, per synthesized present. The math is in
`src/core/FlowWarpMath.h` (a CPU reference the suite tests and the shader is checked against):

1. **Source by fixed point.** For the output pixel `p`, solve `s0 + w F_fwd(s0) = p` from
   `p - w F_fwd(p)` (the first version's reading, exact for uniform motion) by six damped steps
   (FFmpeg's constants), and `s1 + (1-w) F_bwd(s1) = p` likewise. The flow is read at the cell a
   position falls in, not bilinearly: smoothing it across an object's edge invents in-between
   motions that land points where no real point goes, which hides the occlusions of step 2.
2. **Visibility from the residual.** Where a frame holds no point that lands on `p` (behind a
   moving object, the uncovered background in the before frame; ahead of it, the background
   about to be covered in the after frame) the steps cannot settle; a residual over 0.5 to 1.5 px
   drops that side.
3. **Round trip.** Inside a moving object, just behind its trailing edge, both sides can settle:
   the object from the before frame and, from the after frame, the background it uncovers
   later. Content seen in both frames round-trips through the two fields and content seen in one
   only does not (Sundaram et al. 2010's test), so the side that round-trips outweighs the one
   that does not, which still outweighs a side that cannot settle. The fixed point already
   carries the timing: once the object has passed `p`, none of its points lands there.
4. **Static guard.** Where the two frames agree at `p` and at four points 2 px around it, but the
   warp reaches 2 to 8 px or more, the crossfade at `p`. This keeps an opaque HUD sharp even when
   the flow follows the scene under it. FFmpeg asks only `p` and starts at 5 px; one pixel of
   moving texture matches itself by chance often enough to speckle.
5. Sample color from the full-precision frames (the 10-bit ring under NvFBC, the WGC texture
   under `-wgc`), not from the 8-bit flow inputs. Alpha forced to 1.

The suite holds it to: uniform motion at every `w` (exact), a 30 px motion with the guard
silent, a 10% zoom (the fixed point matters: without it the error doubles past the limit), both
endpoints bit for bit, a square moving over a textured background (right on both edges; the
crossfade and the first version both wrong at the leading edge, as controls), an opaque overlay
with its own flow and with flow that ignores it, and extreme flow staying inside the frames'
range. Each of round trip, guard and fixed point was removed in turn and its test failed.
Edges of the picture are out of scope: content arriving from beyond the edge has no source in
one frame and is clamped.

### 4.5 Holds, logging, marker

- A hold redraws what the last present drew from its inputs, as `b:vsync` does today; with the
  pair's flow cached, a held synthesized frame is one more warp draw, no new flow and no snapshot.
- The temporal line keeps `op=` `bw=` `sx=` (`flow-warp`, `blend`, `none`) and `pt=`; `pt=` becomes
  the warp's GPU time from a timestamp query pair, not CPU submission time. New fields at the end
  only: `fl=` the flow's state for this present (`hit` cached, `own` submitted here, `late`
  waited, `miss` fell back). Summary lines: flows run, predicted and used, predicted and unused,
  missed; the flow engine's GPU time per execute (p50/p99/max); the warp's.
- Marker: compositor ID 3, executor 3 on flow-warp presents and 1 on fallback blends, as today.

### 4.6 What it costs, and where

Per synthesized present: one warp draw, which is one turn on the game's graphics engine, the same
turn a blend takes today. Per flowed pair: the NVOFA execute (engine time, if 6.1 holds) and, under
NvFBC only, two conversion draws (one turn each, cached per frame). Pass-through presents cost
what they cost under `b:vsync`, including the copy engine's zero turns under `-wgc`. So against
`b:vsync`, `o:vsync` costs more only on synthesized presents, and its total over a run is roughly
the synthesized rate times (warp minus lerp), plus the flow engine's time if that turns out not to
be separate. At a 90 fps source that is about 30 synthesized presents a second; at a locked 60
into 60, about 0.5 a second.

## 5. Not in this spec

Using the driver's own generated frames (Smooth Motion's or DLSS's) in place of a synthesized one
is `docs/stage7-generated-frame-spec.md`; nothing here depends on it. Motion blur gets its own
spec.

## 6. Tests and measurements

### 6.1 Premise tests (first, on the rig, minutes)

| # | what | how | passes if |
|---|---|---|---|
| P1 | the 5080 exposes NVOFA through D3D11 | a probe: create the session, `nvOFGetCaps` for grid sizes, cost, ROI, max size; register a BGRA8 texture as ABGR8; register an R16G16_SINT flow texture | session created, grid 1/2/4 and cost reported, both registrations accepted |
| P2 | its work does not run on the shader cores | the probe executing bidirectional 1440p flow in a loop under Nsight Systems' GPU context-switch trace, once idle and once beside the Avatar benchmark | the execute shows on its own engine and the game's graphics engine shows no extra turns per execute; control: the same loop with a conversion draw added shows exactly one turn per draw |
| P3 | what it costs | the probe at 2560x1440: grid 1/2/4 x SLOW/MEDIUM/FAST x forward/both x cost on/off, idle and under the benchmark | numbers recorded; the first guess is 2 to 4 ms per bidirectional pair at grid 4 MEDIUM |
| P4 | swapped channels do not change the flow | the same pair as ABGR8 and as GRAYSCALE8 | mean vector difference under 0.25 px |

If P2 fails, stop and put section 3.4's reopened comparison to the user before building the GPU
side.

**Results (2026-10-23, RTX 5080, driver 610.88, build `ofprobe4`).**

- **P1 passes.** Session created on the 5080; grid sizes 1, 2, 4; hint grids 1, 2, 4, 8; regions of
  interest up to 32; 32x32 to 8192x8192. Input formats B8G8R8A8, NV12, R8; flow output R16G16_SINT
  or R16G16_UINT; cost R32_UINT or R8_UINT. Every luma configuration's flow was exact: forward
  (7.00, 3.00) and backward (-7.00, -3.00), 100% of cells right.
- **P3, idle, 2560x1440:** one direction at grid 4: 1.12 / 1.48 / 2.79 ms (FAST / MEDIUM / SLOW);
  grid 2: 1.81 / 4.44 / 7.25; grid 1: 5.84 / 14.64 / 15.64. Both directions about twice that
  (grid 4 MEDIUM both ways, cost on: 2.73 ms; cost off: 2.50). These track NVIDIA's published Ada
  figures scaled by pixel count (grid 1 MEDIUM 15.7 ms, grid 4 MEDIUM about 1.8 ms), which reads as
  the same dedicated unit. Idle, `nvOFExecute` returned only when the flow was done (submit time
  equals completion time in every row; under load it does not, see the 2026-10-24 run below), and
  graphics-queue timestamps around it read 0.
- **P4:** B8G8R8A8 registered as ABGR8 gives exactly the luma result, so WGC's frames go to the
  engine as they are. R8G8B8A8 is not in the engine's list and gives wrong flow (27.94 px for 7).
- **P2, beside the Avatar benchmark (sitting `oftrace1`, one launch):** no relay 5335; the loop of
  60 flows a second 5101 (-4.39%); the same with a draw per flow 5107 (-4.27%). The probe held the
  game's graphics engine 14.8 ms a second in 178 turns (median 0.04 ms), where a flow run on the
  shader cores would hold it about 165 ms a second: the flow's work is not on the game's engine.
  The cost is about 3 turns on that engine per flow, the hand-off between D3D11 and the flow
  engine. Under the benchmark a flow took 15.4 ms to complete (2.7 idle), every second alike,
  worst 89 ms. The draw control added no turns: the draw rode in turns already taken, so it does
  not work as a turn control here. Reading the engine as dedicated rests on NVIDIA's model that
  any shader work from another process takes a turn on the engine; the trace has no other engine.
- **Self-test:** the shader matched the CPU reference exactly (worst 0.0000 on the square at three
  weights, identical frames and `w` = 0); the wrong-sign control missed by 0.42. The comparison on
  the engine's own flow is NOT valid as built: its 4 px exclusions around each edge remove exactly
  the bands where the crossfade fails.

**The loop without its completion query (2026-10-24, sitting `oftrace2`, build `ofprobe5`, one
launch, one run a row).** No relay 5321; `ofprobe:300` 5081 (-4.51%, against 4.39% in `oftrace1`);
`ofprobenowait:300` 5130 (-3.59%). The probe's turns on the game's graphics engine fell from 177.6
to 118.0 a second (about 3 to about 2 per flow) and the game's time off the engine for them from
30.9 to 23.7 ms a second, so the query was one of the three turns. The same run overturns the
reading above that the execute blocks: under the benchmark, without the query, `nvOFExecute`
returned in 0.16 ms (median of the per-second medians; worst single 58 ms), so the 15.4 ms was the
query waiting, not the execute. Idle in the sweep, the execute's return and the query's completion
were the same; under load they are not. When a flow is done under load is therefore not measured,
and a D3D11 query to find out costs a turn. No row closed the sitting with no relay, so the 4.51
and 3.59 rest on the one opening baseline.

**D3D12, CUDA and one direction (2026-10-24, sittings `oftrace3` and `oftrace4`, builds
`ofprobe7` and `ofprobe8`, one launch each, one run a row).** The same loop on the engine's
other interfaces, each checked exact on the generated frames before it loops:

| loop | cost | probe turns a second | game off the engine | done after the call, under load |
|---|---|---|---|---|
| D3D11, no query (`oftrace2`) | -3.59% | 118.0 | 23.7 ms/s | not measured |
| D3D12, both ways (`oftrace3`) | -3.61% | 119.8 | 23.5 ms/s | median 4.15 ms |
| D3D12, both ways (`oftrace4`) | -3.68% | 119.9 | 23.9 ms/s | median 3.97 ms |
| D3D12, forward only (`oftrace4`) | -3.40% | 119.9 | 23.8 ms/s | median 2.10 ms |
| CUDA, both ways (`oftrace3`) | -6.47% | 305.3 | 44.9 ms/s | median 4.22 ms |

Two turns a flow on the game's graphics engine is the same on D3D11 and D3D12 and with one
direction or two, so the turns belong to each call into the engine, not to the API or the work;
each costs the game about 0.2 ms. CUDA takes five and spins the CPU in its stream synchronize.
The `oftrace4` no-relay rows read 5341 and 5361, so the forward-only row's 15 points are inside
the drift. A GPU fault (driver events nvlddmkm 153, LiveKernelEvent 141 and 193) came the second
the benchmark ended during the CUDA row of `oftrace3`; its cause is not known, and no D3D11 or
D3D12 row has faulted.

**A flow merges with a draw (2026-10-24, sitting `oftrace5`, build `ofprobe9`, one launch, one
run a row; no relay 5338 and 5354).** On D3D11 without the query, at 60 a second:

| loop | cost | turns a second | game off the engine |
|---|---|---|---|
| a full-screen draw alone, flushed | -1.83% | 59.8 (1 a draw) | 10.9 ms/s |
| the flow alone, both ways | -3.61% | 118.3 (2 a flow) | 24.5 ms/s |
| the flow alone, forward only | -3.31% | 119.9 (2 a flow) | 22.6 ms/s |
| the draw, then the flow, same context | -3.65% | 118.4 (2 a tick) | 25.0 ms/s |

A draw issued on the flow's context just before the execute rides in the flow's two turns. So
a synthesized present, which draws its warp anyway, takes the flow for a later present for one
turn more than the draw alone, about 0.23 ms of the game's time, where a flow on its own takes
two. Forward only read 0.30 points cheaper here and 0.28 cheaper on D3D12 in `oftrace4`, each a
single run inside a sitting whose no-relay rows differ by 16 and 20 points: not shown, cheap to
run again with repeats.

**What it changes.** The flow is planned ahead (the user, 2026-10-24); the planner's horizon
starts at the present after next. Whether it also needs a thread of its own rested on the
execute blocking 15 ms, which the 2026-10-24 run contradicts. Each flow
costs about 3 turns on the game's engine, so `o` costs what its synthesized rate costs: about
nothing at a locked 60 into 60, roughly 2% at a 90 fps source (an extrapolation from 4.4% at 60
flows a second). Next measurement: the D3D12 and CUDA interfaces, which synchronize with fences,
against the D3D11 loop's 2 turns a flow and 3.59%, each with a way to know when its flow is done.

**The probe is built** (2026-10-23, `src/relay/FlowProbeMode.*`): the hidden mode `ofprobe` runs
P1, P3 and P4 and the warp self-test of 6.3 and exits; `ofprobe:<s>` adds a loop of `<s>` seconds
at 60 bidirectional flows a second for the P2 trace, and `ofprobedraw:<s>` the same loop with one
full-screen draw before each flow, P2's positive control. `ofprobenowait:<s>` (added 2026-10-24)
is the loop without its own completion query after each execute: if the turns per flow drop, the
query was one of them, and the time logged is the execute's return alone. Everything goes to the
log on lines
starting `ofprobe:`, with no popup at any outcome. Launch:
`NvFBCR.exe -source 0 -target 1 -framerate ofprobe`. Each configuration's flow is checked against
the generated frames' known motion (7, 3 px), so a fast configuration that is wrong shows as wrong.

### 6.2 The suite (Mac, every change)

- The flow prediction: synthetic timelines for 90 into 60 (every other present predicted, each
  pair flowed exactly once), a locked 60 into 60 with injected holes (one flow per hole, reused by
  the hole's presents), 240 into 60 (no flows), a stall (predictions dropped, no flow for frames
  that never come), a lock move (a mispredicted pair counts as unused, the present's own pair
  flows). Exact counts pinned.
- The warp math reference: uniform translation reproduced to under 0.01 px at every `w`;
  endpoints at `w` 0 and 1; the static guard on a static region over moving flow; the consistency
  fallback on an occluding square; NaN-free on zero and maximum flow.
- Launch options: `o`, `o:vsync` parse to the D3D11 present, `-wgc` stays on with them, the
  usage registry row.
- The composite policy suites already cover the decisions; nothing in them changes.

### 6.3 GPU self-test (rig, seconds)

Built into `ofprobe` (2026-10-23): the warp shader (`src/relay/FlowWarpShader.h`, its constants
written from `FlowWarpMath.h`) against the CPU reference on the moving square with exact fields
at three weights (worst difference under 0.01), one frame twice with no motion and `w` = 0
(both under 0.001), and wrong-sign fields that must miss by more than 0.2; then the same square on
the engine's own flow at grid 4 and 2, its mean error against the truth logged beside the
crossfade's. The original plan, kept for the cases not yet in it:

The self-test runs the warp shader on generated frames (a textured pattern moved by a known
amount, a square occluding a moving background, a static overlay over a moving background) with
flow from the real engine, and compares each output with the CPU reference. Controls that must
fail loudly when broken: two identical frames give the input back exactly; `w` = 0 gives the
before frame bit for bit; a deliberately swapped flow sign makes the translation case miss by
twice the motion.

**The ordering check (2026-10-24, builds `oorder1` to `oorder4`).** The self-test now drives
the compositor's own `OpticalFlowWarp` at the test size. On the engine's flow at grid 4 MEDIUM
it warps the square with a mean error of 0.106 in the bands the edges sweep, against the
crossfade's 0.333, and 0.005 elsewhere against 0.001 (the band scoring replaced the old
comparison, which left those bands out). **As the relay uses it, D3D11 did not order a draw
after the engine's work:** a warp drawn straight after handing its flow over read the slot's previous flow in 99 of
100 checks idle, and an event query or a polled copy of one cell of each field did not wait for
the engine either (89 of 100 still stale with the copy). Only time did: an event wait plus 50 ms
gave a reference that matched itself exactly, and a warp drawn 17 ms (one present) after the
flow with no sync matched it in 100 of 100 idle and in all 388 checks run beside the benchmark
at frame generation 2X (`ofprobeorder:300`, sitting `oforder1`), while the warp with no wait read
the stale flow in 215 of them. The test frames are 320x96; a full-size flow takes about 4 ms
under load, its worst 69 ms. So the compositor warps only from a flow handed over at an earlier
present. NVIDIA's programming guide (chapter 5) gives D3D11 the same process as CUDA, with
explicit synchronization only for D3D12 and Vulkan, which implies the driver orders D3D11 work
implicitly, so how the relay called the engine was tested before any move to the D3D12
interface.

**It was the fields' bind flags (2026-10-24, builds `ofsync1` to `ofsync3`).** NVIDIA's D3D11
sample runs a compute shader on a flow straight after the execute with no sync, and creates
every engine buffer as render target, shader resource and unordered access. The relay's fields
were shader resource only (its luma frames, the engine's input, were render targets from the
start, which is why the engine always read the right frames). With the render target flag added
to the fields, a warp drawn straight after its flow read the new flow in 20 of 20 checks in each
of: the plain case, luma drawn just before the execute, the directions swapped, timing queries
open around the execute; and a blocking copy and map of a field saw the new flow 20 of 20 (2 of
20 without the flag). Unordered access alone changed nothing (19 to 20 of 20 stale). Controls
held: a warp a present later read the new flow every time, the stale control was far off every
round. At full size (2560x1440, idle, medians from the execute, 20 each):

| fields | event query ended after the execute, done | warp straight after: draw call, flush | event after that warp, done |
|---|---|---|---|
| shader resource | 0.258 ms | 0.004 ms, 0.055 ms | 0.381 ms (read the old flow) |
| + render target | 0.269 ms | 0.004 ms, 0.052 ms | 2.922 ms |

So the order is kept on the GPU: a warp issued before its flow is done waits there for the
engine, about the engine's whole time, and the CPU calls return at once. An event query ended
right after the execute does not wait for the engine with either flag set, so D3D11 orders per
resource and gives no way to ask whether a flow is done without waiting for it. Beside the
benchmark at frame generation 2X (`ofprobeorder:300`, sitting `oforder2`, build `ofsync3`) the
ordering check, now failing on a stale no-wait warp as well, passed 391 of 391, the warp with no
wait reading the new flow in all of them (215 of 388 stale in `oforder1` without the flag). The
fields now carry the render target flag; the compositor still warps only from a flow handed over at an
earlier present, and a flow not done by then holds that present on the GPU instead of being read
stale.

### 6.4 Quality against ground truth (offline)

A capture of real gameplay at 240 fps (the game display at 240 Hz, the game uncapped above that)
gives true in-between frames: frames `n` and `n+4` stand in for a 60 fps source and frames `n+1`,
`n+2`, `n+3` are what `w` = 0.25, 0.5, 0.75 should look like; frames `n` and `n+3` (with `n+1`,
`n+2`) stand in for an 80 fps source, close to the 90 case. Each method synthesizes the same
frames and is scored against the truth with PSNR and SSIM, overall, in the HUD region, and on the
10% of frames with the most motion:

- repeat (the nearer frame) and blend: the controls; every interpolator must beat blend in the
  moving regions, and repeat must lose everywhere but the HUD.
- the old warp (v1) and the new warp (v2), with grid 4 and 2 and each preset.
- RIFE 4.25, run offline on the Mac, as the quality ceiling of an AI interpolator.

The capture is a new relay switch that writes N consecutive frames of the ring to disk
(timestamps in the names), one change of its own.

### 6.5 Live rows (rig, one change at a time)

| row | content | launch | expect |
|---|---|---|---|
| L1 | 90 fps source: Avatar benchmark capped at 90, frame generation off, game display 240 Hz | no relay | the reference |
| L2 | same | `b:vsync -src 90 -mark 5400` | about 30 blends a second |
| L3 | same | `o:vsync -src 90 -mark 5400` | about 30 flow-warps a second, `fl=hit` on nearly all, `sx=blend` near 0 |
| L4 | same | `o:vsync -src 90 -wgc -mark 5400` | as L3, flows with no conversion draw |
| L5 | the uncapped benchmark, frame generation 2X | `b:vsync`, `o:vsync`, each with and without `-wgc` | the cost rows: points against `b:vsync`, GPU % average, Nsight turns per second |
| L6 | the game, Smooth Motion off, capped at 90 | `o:vsync -src 90 -wgc -mark 5400`, recorded on the Mac | the marked row: every synthesized frame decoded from the video, executor cells 3, no repeats |
| L7 | a stream on the build | the user's usual launch with `o:vsync` | streamcheck as always; one change |

L1 to L4 are capped sittings, so they measure pacing and turns, not points; L5 measures points.
Each row reads `fl=` and `sx=` censuses from the log and the summary lines of 4.5.

## 7. Order

1. `src/core`: the flow prediction (`FlowPlan.h`) and the warp math reference (`FlowWarpMath.h`),
   with suite tests. Done 2026-10-23; the suite's output is unchanged by them (tests print only
   on failure). The mode grammar moves to step 3, with the backend it selects.
2. The NVOFA probe (P1 to P4) on the rig. Run 2026-10-23 (results in 6.1): the engine is there,
   its work is not on the game's graphics engine, each flow costs about 3 turns there. Not yet
   built by MSVC.
3. The D3D11 backend: flow engine, flow cache, warp v2, fallbacks, logging; the D3D9 `o` removed.
   Built 2026-10-24 (`src/relay/OpticalFlowWarp.*`, `D3D11Present.*`): `o` and `o:vsync` present
   through the D3D11 backend (`o:<rate>` refused); the loop hands the path the brackets of the
   next two presents (`IPresentPath::PlanAhead`) and the backend runs `FlowPlanner` as it was
   tested; a planned flow is handed to the engine right after a present that draws, or alone when
   the next present needs it; nothing waits on the CPU for a flow, the warp on the same context
   reading the finished fields; a synthesized present without a flow draws the blend (`fl=miss`).
   Input: luma drawn from every frame (both captures, any size; the draw rides the flow's turns);
   `-xofdirect` hands `-wgc` frames at the output's size to the engine as they are (ABGR8), to be
   benched against it (the user, 2026-10-24: "do both and bench them? I want whatever is more
   performant"). Not yet: `pt=` as warp GPU time, the scene-cut fallback, the late-flow bound.
   **First rows (2026-10-24, capped 90, frame generation off, one launch each).** `ofcap1`
   (build `oflow1`): about 3,600 synthesized presents a row, every one warped with luma input
   (`-wgc`: 3562 hit, 33 late, 3 own, 0 miss; NvFBC: 3649, 38, 2, 0); planned and unused about
   5%. Against `b:vsync` in the same launch the game lost about 10 ms a second more to the relay
   (`-wgc` 93.3 against 83.5, NvFBC 104.1 against 93.6). But under `-wgc` the flow handed over
   right after a warp held that warp's copy-engine present: long present gaps 53 against the
   blend's 17 (NvFBC 16 against 15), 443 with `-xofdirect`. **Fixed** in `oflow2`: with the copy
   engine, planned flows go after the present. `ofcap2`: long gaps 22 against the blend's 20
   (the old placement 1075 in the same launch), `-xofdirect` 16; game off the engine 96.8 ms/s
   against the blend's 80.8, `-xofdirect` 99.4 (fewer relay turns, 86.5 against 97.1, but no
   saving for the game).
   **Scored, uncapped, frame generation off (`ofpts1`, `oflow2`):** the game ran 112 to 115 fps,
   so about 1.3 synthesized presents a second; `o` cost 17 points more than `b` (4228, 4230
   against 4244, 4247; no relay 4344, 4360) because 88% of planned flows went unused: the bare
   gate predicted synthesis where the composite's band and the lock held passthrough. **Fixed**
   in `oflow3`: Compose plans after its own decision, predicting each upcoming present with
   `DecideComposite` on a copy of the state. `ofpts2`: `o` 4250, 4245 against `b` 4256, 4243 (no
   relay 4378, 4356), unused 1 of 55; `-xofdirect` 4251, 4254 with 7 or 8 blends for want of a
   flow per run. `ofcap3` (capped 90, `oflow3`): about 3,600 synthesized presents a row, 99%
   warped from a flow planned ahead, 21 to 25 planned flows unused (`oflow1`: 130 to 199), long
   present gaps equal to the blend's (19 against 18 with `-wgc`, 17 against 17 on NvFBC); the game
   lost 12.7 ms/s more than with the blend under `-wgc` (91.3 against 78.6) and 10.8 more on
   NvFBC (102.7 against 91.9), about 0.4 ms per synthesized present. Points at a 90 fps source
   that is also GPU-bound are not measured (the game's 90 is a cap).
   **Trimmed after `d1856fd`:** `-xofdirect` removed (no measured gain, blends for want of a
   flow); the probe keeps `ofprobe` and `ofprobe:<s>` (now without the completion query, as the
   compositor runs); the D3D12 loop, its header and the loop variants were removed (the user,
   2026-10-24: "we can restore it later if we need more experimentation"), and `d1856fd` holds
   them.
4. The GPU self-test (6.3); the frame dump and the quality harness (6.4).
5. The live rows (6.5), then a stream.

## 8. Decisions for the user

1. Remove the D3D9 `o` (never run) in favour of the D3D11 one (recommended), or keep both.
   Decided 2026-10-23: remove it.
2. The flow ahead of the present (recommended) against flow for every captured pair (simpler,
   FFmpeg's way, but at 240 fps it would run 240 flows a second for presents that pass through).
   Decided 2026-10-24: ahead.
3. Grid and preset, after P3 and 6.4.
4. Whether an AI interpolator is worth a live trial after 6.4 (RIFE through TensorRT, or Maxine
   VFG installed by hand), if the flow warp's artifacts are not acceptable.

## Sources

- FFmpeg `libavfilter/vf_fruc_vulkan.c` and `libavfilter/vulkan/fruc_interpolate.comp.glsl`
  (master, 2026-10); review: https://code.ffmpeg.org/FFmpeg/FFmpeg/pulls/23704 ; the earlier
  NvOFFRUC filter: https://patchwork.ffmpeg.org/comment/75490/ ; user report on cuts:
  https://github.com/mpv-player/mpv/issues/11060
- NVOFA: https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note ,
  https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html ,
  https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvfruc-programming-guide/index.html ,
  https://docs.nvidia.com/video-technologies/optical-flow-sdk/license/index.html
- Blackwell: https://images.nvidia.com/aem-dam/Solutions/geforce/blackwell/nvidia-rtx-blackwell-gpu-architecture.pdf ;
  DLSS 4: https://research.nvidia.com/labs/adlr/DLSS4 ,
  https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md
- Maxine VFG: https://docs.nvidia.com/maxine/vfx/1.3.0/Filters/VideoFrameGeneration.html ,
  https://docs.nvidia.com/maxine/vfx/1.3.0/WindowsVFXSDK/PerformanceReference.html
- FidelityFX Optical Flow: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/docs/techniques/optical-flow.md ,
  https://gpuopen.com/fidelityfx-superresolution-3/
- RIFE: https://github.com/hzwer/Practical-RIFE ; timings:
  https://github.com/styler00dollar/VSGAN-tensorrt-docker
