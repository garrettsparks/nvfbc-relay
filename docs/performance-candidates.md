# Performance candidates for `b:vsync`

Ranked ideas for lowering what the default mode, `b:vsync`, costs the game. Nothing here is built
or measured yet. The decision logic (comb lock, lookahead, late moves, refusal) stays as it is; the
candidates change how frames are captured, copied and drawn.

The goal (user, 2026-10-03): beat OBS's fullscreen projector on both game cost and picture
quality.

## 1. Where things stand

All rows are the Avatar benchmark at 2560x1440 with DLSS frame generation x2, uncapped and
GPU-bound, three runs per row. Files are in the analysis folder.

Session 1 (2026-10-03, 18:37 to 19:24):

| row | scores | mean | cost |
|---|---|---|---|
| no relay | 5349, 5361, 5358 | 5356 | |
| original relay (`4e138c6`) at 60 | 5198, 5206, 5215 | 5206 | 2.79% |
| `60` | 5086, 5092, 5094 | 5091 | 4.95% |
| OBS projector | 5084, 5102, 5102 | 5096 | 4.85% |
| `b:vsync -src 60`, defaults | 5004, 5016, 5022 | 5014 | 6.39% |

Session 2 (2026-10-03, 19:53 to 20:10):

| row | scores | mean | cost |
|---|---|---|---|
| no relay | 5378, 5385, 5382 | 5382 | |
| `b:vsync -src 60`, today's build (`5996f29`) | 5040, 5054, 5070 | 5055 | 6.08% |
| `b:vsync -src 60 -lock -lag 75 -etw -dejit`, the 09-18 build (`f953554`, rebuilt from source) | 5038, 5053, 5058 | 5050 | 6.17% |

What these say:

- Today's build costs the same as the 09-18 build. The rise from the 5.6% measured on 09-18 came
  from outside the relay.
- The same build scored 41 points higher in session 2 than in session 1, and the no-relay
  baseline 26 points higher. Compare only rows from one session, against that session's
  baseline. Within a session, three runs resolve about 32 points (0.6%).
- `b:vsync` costs about 6.1 to 6.4%, the projector about 4.85%. The gap is about 1.2 to 1.6
  points.
- On quality, `b:vsync` already leads: in motion, 1.5 repeated frames a second against the
  projector's 3.7 (`mgdupes.py` over ten 30 s windows of each session-1 recording, no marker, so
  the floor of what the chain and the game add is not split out). The repeat count does not see
  blends or skips.

`docs/relay-cost-results.md` (2026-09-18) found capture to be the larger part of the cost:
roughly 4.2 points of the 5.6% for capture at 151 grabs a second, roughly 1.3 for the 60 presents.
It also found that under DLSS frame generation the extra wakes deliver a duplicate of the real
picture, so about 40% of the capture work copies a frame the ring already holds.

## 2. What `b:vsync` does each second

Read from the code at `5996f29`. In the benchmark the capture thread sees about 151 grabs a
second; on capped 60x2 gameplay about 120.

| work | per second | where |
|---|---|---|
| NvFBC grab into one staging surface (10-bit, HDR requested, scaled to 2560x1440) | 151 | `CaptureRing.cpp:296` |
| full-frame StretchRect, staging surface to ring slot (14.7 MB each way) | 151 | `CaptureRing.cpp:334` |
| event query and a busy spin with `D3DGETDATA_FLUSH` until the copy is done | 151 | `CaptureRing.cpp:339-342` |
| retract the previous slot when the wake is a batch's second member | about 60 | `CaptureRing.cpp:526-530` |
| full-screen draw of the blend shader, two slots sampled (passthrough samples one slot twice) | 60 | `D3D11Present.cpp:410-444`, `:561-592` |
| `Present(1, 0)` on a 2-buffer FLIP_DISCARD swapchain, frame latency 1 | 60 | `D3D11Present.cpp:676` |

The retracted member was already copied and flushed. Nothing sets thread or GPU priority. A D3D9
present device with a 2560x1440 back buffer, and 32 ring slot aliases on it, exist in `b:vsync`
but do no per-frame work (`D3D9Setup.cpp:55-65`, `CaptureRing.cpp:166-177`).

The original relay does one grab straight into a D3D9 back buffer and one present per loop, with
no ring, no copy and no flush. Its video shows about 41 new pictures a second (inferred from 19.2
repeated frames a second in motion; its loop rate was never logged).

## 3. Ranked candidates

Ranked by how likely each is to work times how much it could save. Gains are estimates from the
09-18 split and the operation counts above, not measurements.

| rank | candidate | removes per second | possible gain | plausibility | effort |
|---|---|---|---|---|---|
| 1 | Copy only the kept member (`-defercopy`) | about 60 copies and 60 flushes | 0.3 to 1 point | medium | medium |
| 2 | Skip the extra wake (`-grabphase`) | about 60 grabs, copies and flushes | up to about 1.7 points | low | high |
| 3 | 8-bit ring and plain-copy passthrough (`-8bit`) | a format conversion per grab; the draw on most presents | small, unknown | medium | low to medium |
| 4 | Full-resolution grab mode (`-grabfull`) | a scaling pass per grab, if NvFBC does one at 1:1 | unknown | low | low |
| 5 | GPU priority (`-gpuprio`) | nothing; changes who waits | none expected for cost | n/a | low |

Before any of them, one calibration row costs no code (section 4).

### 3.1 Copy only the kept member (`-defercopy`)

Under frame generation x2 each source frame wakes the grab twice, under 3 ms apart, and the
relay keeps the second. Today both wakes are copied and flushed, and the first is retracted a wake
later. Deferring the copy until the next wake shows which member a frame is removes the wasted
copy and flush.

How: register two NvFBC output buffers (`dwNumBuffers = 2` at `CaptureRing.cpp:220-237`) and
alternate `dwBufferIdx` per grab, so a frame waits in its buffer while the next grab writes the
other. At each wake, the policy's batch decision (`policy::UpdateBatch`) already says whether this
wake is intra-batch. If it is, the waiting frame is the generated member and is dropped uncopied;
the new wake is the real member and is copied at once. If it is not, the waiting frame was a lone
real frame and is copied now.

It is the keep-real rule (`policy::DecideKeep`) applied before the copy instead of after: the
ring gets exactly the frames and stamps it gets today. Without frame generation every wake is a
real frame, nothing is skipped and nothing is saved; a lone frame is only published one wake
later. To leave runs without frame generation exactly as today, defer only while recent batches
have had a second member.

No extra thread: the second NvFBC buffer is what lets the capture thread hold one picture while
the next grab writes the other. Grabs alternate A, B, A; each copy is flushed before the grab that
would overwrite its buffer, so the coherence argument is unchanged.

It conflicts with anything that needs the generated member's pixels: `-phasekeep` and `-fgphase`
today, and generated-frame substitution (`-subgen`, removed on this branch; Smooth Motion only,
since DLSS's generated frames never reach NvFBC) if it returns. Those refuse `-defercopy`, or it
keeps a generated member when they ask for it. NVOFA interpolation works from real frame pairs and
is unaffected.

What can go wrong:

- **Two buffers may not pass SetUp.** Eight crashed inside NvFBC (`8eb5e70`, July; the commit
  records only that); the triage results were never written down. The user recalls (2026-10-04)
  that two worked and it failed beyond three. Whether the cap depends on the GPU is unknown; the
  crash is a stack buffer overrun inside NvFBC's user-mode DLL during SetUp. The first test is only whether SetUp with two
  buffers succeeds on driver 610.88. If it crashes, this candidate falls back to one buffer and a
  short second grab (wait 3 ms; a return means the waiting frame was generated), which is fragile:
  the relay never raises the timer resolution, a 3 ms wait can fire much later, and what NvFBC
  does to the buffer on a timeout is not known.
- **A lone frame is published one wake later**, up to one source period, inside the 75 ms of
  extra lag. At a stall the next wake can be up to the 100 ms grab timeout away, so the waiting
  frame must be copied on the timeout return as well.
- `-phasekeep` and `-fgphase` (development flags) need every member's content and must refuse it.
- Field logs need a field for when a slot was published, so the replay can model it.

Test: one bench row against the same session's `b:vsync -src 60`, plus a capped 60x2 gameplay
stream for pacing (one change per stream).

### 3.2 Skip the extra wake (`-grabphase`)

The only candidate that also cuts NvFBC's own grabs. The capture loop already sees NvFBC return
the newest frame in a single wake when the thread was busy through the generated flip (the
coalesced single, `CaptureRing.cpp:488-496`: singles are 7 times as common in that class, 18%
against 2.5%). Doing that on purpose, sleeping after each real member until just before the next
real flip and then grabbing, would cut grabs, copies and flushes from about 151 to about 90 a
second.

The unknown that decides it: when a flip happened while the thread was not inside the grab call,
does the next grab return that frame at once, or wait for the following flip? First step is an
instrument: log each grab's blocking time and check for returns under about 200 us after a known
busy stretch.

Risks: the arrival stamp becomes the time the relay asked, which the flip-time correction would
have to cover; oversleeping into the next generated flip loses a real frame; an uncapped base rate
that wanders makes the wake time imprecise. Capped 60x2 gameplay, with its steady 16.7 ms between
real frames, is the easier case.

### 3.3 8-bit ring and plain-copy passthrough (`-8bit`)

The capture card reports 8 bits per colour and the swapchain is B8G8R8A8 (10-01 log), so the
10-bit ring buys nothing on this card. NvFBC is asked for ARGB10 with HDR (`CaptureRing.cpp:235-237`)
from an 8-bit desktop, which may cost it a conversion, and every present converts back to 8 bits
in the draw.

With NvFBC capturing 8-bit ARGB (`NVFBC_TODX9VID_ARGB = 0` in the SDK header at
`v0.0.16:inc/NvFBC/nvFBCToDx9Vid.h:79`; the vendored `src/common/NvFBCApi.h:90-93` keeps only the
10-bit value), the ring matches the swapchain format, and a passthrough or hold can be a
`CopyResource` from the slot to the back buffer instead of the full-screen draw. A plain copy
moves the same bytes as the draw, so the saving is uncertain: it helps if NVIDIA runs it on the
copy engine alongside the game's rendering, and does nothing if it runs on the 3D engine like the
draw. Blends keep the draw.

Keep the 10-bit path when the output reports 10 bits or more.

### 3.4 Full-resolution grab mode (`-grabfull`)

The grab asks NvFBC to scale to 2560x1440 (`SOURCEMODE_SCALE`, `NvFBCSession.cpp:104-110`).
When the game display is already 2560x1440, `SOURCEMODE_FULL` (0 in the SDK header) may skip a
scaling pass. Whether NvFBC scales at 1:1 at all is unknown. Cheap to try; only valid when the
two displays have the same resolution.

### 3.5 GPU priority (`-gpuprio`)

`IDirect3DDevice9Ex::SetGPUThreadPriority` on the capture device and
`IDXGIDevice::SetGPUThreadPriority` on the D3D11 device, -7 to 7. Lowering it cannot reduce the
relay's GPU work; it makes the relay wait longer behind the game, and presents would miss
refreshes. Raising it is the roadmap's idea for the known issue at a game's resume (the present
waiting 33 or 50 ms). A quality lever, listed so it isn't mistaken for a cost one.

## 4. Calibration: does cost follow operations per second?

The original relay costs 2.79% and today's `60` 4.95%. `60` is the original loop with the
high-resolution absolute timer the user added in `fb0e6e3` (2026-06-08), plus
`D3DCREATE_MULTITHREADED`, a window that is really topmost, and PresentEx flags 0. The original
presents only about 41 times a second; `60` presents 60.

One row settles how much of that gap is the rate: today's `60` mode typed as `41`. If it costs
about 2.8%, cost follows grabs and presents per second, the other differences don't matter, and
the ranking above (which counts operations) holds. If it stays near 5%, the other differences
carry the cost and are worth a row each.

## 5. CPU-only cleanups

Not expected to move the score on a GPU-bound game. They may matter on a game that is limited
by the CPU.

- Cache the back buffer's render target view. It is recreated every present
  (`D3D11Present.cpp:394-403`) because the comment at `:390` says flip model changes the back
  buffers' identities each present. That is D3D12's model; under D3D11, buffer 0 should always
  name the current back buffer (from the Fable review's reading of the DXGI docs, not checked
  here). Check on the rig that a cached view still draws to the screen.
- Yield in the flush wait. The spin at `CaptureRing.cpp:340-342` holds a core for the whole wait
  (p95 about 10 ms in GPU-bound rows). With `-defercopy`, the check can move to the next wake.
- Shrink the idle D3D9 back buffers to 1x1 in `b:vsync` (the present device's and the capture
  device's own, about 29 MB of video memory).
- Drop `D3DCREATE_MULTITHREADED` where one thread uses the device.

## 6. Ruled out

- NvFBC writing straight into ring slots: crashes at SetUp beyond about 2 buffers (`8eb5e70`).
- Replacing the flush with a fence or keyed mutex: D3D9Ex has neither for these shared surfaces,
  so the event drain stays the only guarantee that a published slot is complete.
- Doing the capture copy on the D3D11 side: the copy still has to happen before the next grab
  overwrites the staging surface, and NvFBC's write still needs the D3D9 query.
- Skipping the draw on holds: FLIP_DISCARD rotates buffers, and holds are rare.

## 7. Open question: what the game's GPU % shows

The game's GPU % falls from 95.7 with no relay to 87.5 to 91 with one. Its definition is unknown:
the CSV's "GPU time" equals the frame time in every row, so it is not the game's busy time.
Neither "the game's own share" nor "the whole GPU's busy time" fits every row. PresentMon's
per-frame GPU busy for the game, plus `nvidia-smi --query-gpu=timestamp,utilization.gpu
--format=csv -lms 100`, during one `b:vsync` run and one no-relay run, would show whether the
relay adds work to a busy GPU or leaves it idle between the game's frames.

## 8. Measuring where the GPU time goes

Every ranking above is inferred from operation counts. A GPU timing trace of one benchmark run
would show how long each piece of the relay's GPU work takes (NvFBC's grab, the ring copy, the
flush, the draw, the present) and how often the relay's work interrupts the game's. It decides
which candidate, and which option in section 9, is worth building.

- On the rig: `wpr -start GPU -filemode`, run the benchmark with `b:vsync -src 60`, `wpr -stop
  relay.etl`, then open it in GPUView or Windows Performance Analyzer. A second trace with no relay
  gives the game's own picture.
- Or extend EtwProbe, which already reads the graphics kernel's provider, to log per-process GPU
  work so the analysis can run on the Mac. Which events carry per-packet GPU timing has not been
  checked.

## 9. Architectural options

Bigger changes than section 3, each removing work the current design cannot avoid. None is
prototyped.

### 9.1 Keeping frames without copying them

**NvFBC's CUDA interface.** The D3D9 interface registers its output buffers once, at SetUp, which
is where direct write crashed. The CUDA interface takes its output buffer on every grab call:
`NVFBC_CUDA_GRAB_FRAME_PARAMS::pCUDADeviceBuffer` (`v0.0.16:inc/NvFBC/nvFBCCuda.h:74`; formats ARGB
and ARGB10 only, `:59-60`). Each grab could write straight into its ring slot, removing the ring
copy and its flush. CUDA hands out linear device memory, so the D3D11 side would read the slots as
buffers, not textures (point sampling at 1:1 needs no filtering). CUDA comes from the driver's
`nvcuda.dll` through the driver API, so nothing new ships. Unknown: whether the CUDA interface still
works on Windows with driver 610.88, and what handing a buffer between CUDA and D3D11 costs per
frame (map and unmap, or shared memory with an external semaphore).

**Windows.Graphics.Capture (WGC).** A different API from DXGI Desktop Duplication (the one usually
called "DXGI capture"). Desktop Duplication hands over one frame at a time: asking for the next one
without releasing the previous fails with `DXGI_ERROR_INVALID_CALL` (Microsoft's
`IDXGIOutputDuplication::AcquireNextFrame` page), so keeping frames needs a copy, as today. WGC
delivers frames into a frame pool whose size the app picks (`Direct3D11CaptureFramePool.Create`,
`numberOfBuffers`; no maximum documented), so the pool itself could be the ring with no copy. Each
frame carries `SystemRelativeTime`, "the QPC time at which the compositor rendered the frame": a
frame time from the system, where NvFBC gives only arrival time. It is D3D11 like the present path,
needs nothing from NVIDIA, and matches the roadmap's plan for the app to capture through Windows by
default. An earlier experiment exists: the remote branch `dxgi-native-pipeline-probe` (tip
`de0b1f4`, 2026-07-23, "add dxgi probe", 9 commits past `dev`'s old tip); read it before any WGC
or Desktop Duplication prototype. Unknown: whether WGC pulls a fullscreen game out of independent
flip (that timestamp is the compositor's, which suggests composition) and what that costs the
game; whether it sees Smooth Motion's generated frames; how long a pool frame stays valid while
held; and the cost of holding about 30.

**Presenting a ring slot by reference (composition swapchain).** Windows 11's composition
swapchain registers up to 31 textures with a presentation manager and presents any of them, in any
order, with no copy; each present can carry a target time on the QPC clock (Microsoft's composition
swapchain programming guide). Passthrough and hold presents would need no draw, and blends would
draw into a spare registered buffer. Target-time presents match how the relay already chooses
frames. Requirements: Windows 11 build 22000.194 or later with WDDM 2.0; independent flip and
direct scanout need WDDM 3.0 and textures created as displayable, which the D3D9-made ring slots
are not. So this pairs with the CUDA or WGC option, whose slots are created on the D3D11 side.

### 9.2 Doing less work

**Copy only frames that will be shown.** The relay knows its present schedule about 100 ms ahead:
each present's target is the vblank minus the lag and the pull, and the pull moves slowly. At
capture time it can tell which frames can never be a passthrough or a blend partner and skip their
copies. `-defercopy` drops only frame generation's extra wakes; this also saves when the game
outruns the card (about a third of the copies at 90 fps into 60, half at 120). Risk: a pull move,
wrap or stall changes which frames are needed after they were skipped, so the margin has to cover
the lock's largest move.

**Grab only at real flips.** The comb lock predicts when each real frame flips. A no-wait grab just
after that time catches each real frame once and never wakes for frame generation's extra flip,
and ETW's flip times stamp it with its real flip time. The ring stays: each grab is stored with its
stamp as today, and the present side still selects and blends from it with the same lag. What
changes is only when the capture thread asks. The risk is a wrong prediction: after a hitch or a
rate change a grab can come early (the previous frame again), late (the generated frame, or the
next real one), or miss a frame, leaving a gap the bracket covers with a wider blend. Steady capped
content suits it; an uncapped, wandering rate does not. This is the cleaner form of `-grabphase`.

**Low GPU priority for the capture device.** Section 3.5 treats priority as quality only. For the
capture device it may also be a cost lever: capture has the whole 75 ms lag as slack, so at low
priority its grab and copy could wait for gaps in the game's GPU work instead of preempting it. If
preemption is a real part of the cost (section 8 would show it), this lowers it. The present device
stays at normal priority so it still makes each refresh. One line of code and one bench row.

**NV12 ring.** The D3D9 interface can output NV12 (`NVFBC_TODX9VID_NV12`,
`v0.0.16:inc/NvFBC/nvFBCToDx9Vid.h:80`): 1.5 bytes a pixel where the ring holds 4, about 60% less
to copy and store. The stream is encoded at 4:2:0 anyway, but converting to 4:2:0 twice (here and
in the encoder) can soften coloured edges, so it needs a quality check on text and HUD.

### 9.3 Moving work off the game's GPU

**Present from a second GPU.** With the capture card on the motherboard's output (the CPU's
integrated graphics), the ring, the blends and the presents all run there. The game's GPU keeps
only NvFBC's grab and one copy per frame across PCIe, which its copy engine can run beside the
game. The largest possible cut, and the most hardware-dependent: it needs integrated graphics and a
motherboard output that drives 2560x1440 at 60 Hz, and a cross-adapter copy path.

## 10. Order

The user's order (2026-10-04): release first, then this pass on its own branch. Why performance
comes before NVOFA (user, 2026-10-04): "I'm worried nvofa interp frames are going to be expensive
to generate, so we need more performance headroom before we even enable that."

NVOFA's cost has never been measured. The interp compositor runs flow and warp only on presents
that synthesize (`SynthCompositorBase::RenderSynthesis`, `src/relay/FrameCompositors.h:58`), so its
cost follows the synthesis rate: on capped 60x2 gameplay about 1,050 to 1,200 blends an hour (about
0.3 a second; `docs/lock-transitions-spec.md` section 8, five streams), far more in the uncapped
benchmark. The worry there is a spike on one present more than the average. The hidden `o` mode
(`o`, `o:vsync`: interp on the D3D9 swapchain on DWM's clock, a D3D11 sidecar for flow and warp)
can be benched now against `b:dwm` (the same swapchain with the lerp), which isolates the
interpolation's cost. It has not been validated on Windows on this branch, so a first run may find
problems before it finds a number.

0. ON HOLD (user, 2026-10-04): sizing NVOFA with `o` against `b:dwm`. "I think we'll want to hold
   on anything with o mode for now. We'll need to rebuild it in the future I think. comparing cuda
   vs dx11 mechanisms." Measure NVOFA's cost on the rebuilt mode.
1. The `41` calibration row (section 4, no code), first (user, 2026-10-04: "starting with 41 mode is
   a good place to start to classify how much of a performance improvement we get").
2. The GPU timing trace (section 8), with and without the relay.
3. Low GPU priority for the capture device, and `-defercopy` (first the two-buffer SetUp check),
   one bench row each.
4. The `-grabphase` instrument (grab blocking times), then decide between it and "grab only at
   real flips".
5. `-8bit` and `-grabfull`, one row each.
6. One architectural prototype, chosen with the trace: WGC with the pool as the ring (also the
   app's default capture path) or NvFBC's CUDA interface (NvFBC-native), each with the composition
   swapchain as a later step.
7. The CPU-only cleanups, together, checked by the suite and one stream.

Every row against the same session's no-relay baseline and `b:vsync -src 60`.

Sources for section 9: Microsoft Learn,
[Direct3D11CaptureFramePool.Create](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.create),
[Direct3D11CaptureFrame.SystemRelativeTime](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframe.systemrelativetime),
[IDXGIOutputDuplication::AcquireNextFrame](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-acquirenextframe),
[Composition swapchain programming guide](https://learn.microsoft.com/en-us/windows/win32/comp_swapchain/comp-swapchain).
