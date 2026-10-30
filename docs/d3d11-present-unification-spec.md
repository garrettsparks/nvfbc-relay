# Every temporal mode on the D3D11 present

Written 2026-10-28, after the present-step refactor (`docs/present-step-spec.md`, stages 1 to 4,
commits 4b34c32 to 982c96b). Implemented as code 2026-10-29, steps 1 to 5 in one change, with the
answers of section 6; every capture gate of section 4 is still owed (section 7).

## 1. Why

Since the present-step refactor the policy never sees the present path: `policy::StepPresent`
takes a deadline, reads the ring and returns a decision, and a present path only waits and draws.
What still differs between modes is how they present, and two of the three ways run on the
D3D9 swapchain:

- **The D3D11 flip-model present** (`b`, `b:vsync`, `o`, `o:vsync`) waits on the swapchain's own
  frame-latency object, which under independent flip is the SINK's vblank: one present per scan,
  phase-locked by construction (`docs/flip-present-spec.md`). Validated 2026-09-03 (0 repeats
  against 3.55 a second on the D3D9 present).
- **The D3D9 present** (`t`, `t:dwm`, `b:dwm`, `t:<fps>`, `b:<fps>`) is a windowed
  `INTERVAL_ONE` that blocks on DWM's compose clock, or an `IMMEDIATE` present after a QPC timer.
  It costs more GPU (the cost ladder of 2026-09-18 put `b:dwm` at 9.4% against about 5.5% for the
  release path), it cannot take `-wgc` (Windows.Graphics.Capture), and it keeps a second graphics
  API in the process for every launch that uses it.

Moving every temporal mode onto the D3D11 present leaves one present path to maintain, lets every
mode take `-wgc` and the copy engine, and is what `o:<fps>` and `t:vsync` need: `o` cannot run on
D3D9 (the flow engine and the warp are D3D11), and a nearest mode on a vblank deadline needs a
nearest compositor on D3D11.

## 2. What exists

| mode string | present path | pacing | compositor |
|---|---|---|---|
| `b`, `b:vsync` (the default) | D3D11 flip model | the swapchain's waitable object (sink vblank under independent flip) | blend |
| `o`, `o:vsync` | D3D11 flip model | the same | optical-flow warp, blend without a flow |
| `t`, `t:dwm` | D3D9 swapchain | windowed `INTERVAL_ONE`: DWM's compose clock | nearest |
| `b:dwm` | D3D9 swapchain | the same | blend |
| `t:<fps>`, `b:<fps>` | D3D9 swapchain | `PresentScheduler` (QPC deadline, high-resolution waitable timer), `IMMEDIATE` present | nearest, blend |
| `o:<fps>` | refused | | |

Not temporal, and out of scope here: `vsync` and a bare number (`60`), the original grab-and-present
modes (`VsyncCaptureMode`, `TimerCaptureMode`, NvFBC straight into D3D9 surfaces, no ring);
`diag`, `diag:vsync`; `ofprobe`. NvFBC capture itself stays a D3D9 device (the capture ring's own
device, NvFBC's `ToDX9Vid` interface); with `-wgc` a launch touches no NvFBC and no D3D9 at all.

Pieces the D3D11 path lacks today:

- **A nearest compositor.** `D3D11PresentBackend` draws passthrough (one slot, weight 0),
  synthesis and holds; "Nearest is not ported". The nearest decision itself (`SelectFrame`) is
  already the step's.
- **Any pacing but the waitable object.** `WaitForFrame` waits on the swapchain's latency object
  or, with the copy engine, on the copy-engine target's buffer; `Present` uses sync interval 1.
- **The `-tint` debug pass** (D3D9 only; the D3D11 path says so at setup).

## 3. Design

### 3.1 Three pacing sources, one present path

The present path waits on one of three clocks and passes the deadline to the step, as the loop
does today:

| pacing | wait | deadline passed to the step | present |
|---|---|---|---|
| sink vblank (`:vsync`, bare letter) | the swapchain's waitable object, as now | QPC "now" after the wait, as now | sync interval 1, as now |
| DWM compose (`:dwm`) | `DwmFlush()` | QPC "now" after the wait | sync interval 0: the newest frame replaces any queued one |
| QPC timer (`:<fps>`) | `PresentScheduler` with the tight wait of 3.3 | the scheduled deadline | sync interval 0 |

The pacing lives beside the present path, not in it: a small `IPacing` (wait, deadline, period)
chosen from the mode string, so the loop reads `pacing.Wait()`, `StepPresent(...)`,
`present.Compose(...)`, `present.Present()`. The D3D9 path's split (it paces inside `Present`) goes
with it.

To measure before relying on it (stage 1 below):

- **`DwmFlush` as DWM's compose clock.** It returns when DWM next composes. Whether that matches
  what the D3D9 `INTERVAL_ONE` present blocked on, regime by regime (composed desktop, fullscreen
  game on the source, in-game frame generation), is measured, not assumed: its interval
  distribution against the `b:dwm` and `t:dwm` captures of 2026-10-28.
- **Sync interval 0 on flip-discard without the tearing flag.** Under composition the newest
  present replaces a queued one at the next compose; under independent flip it waits for the next
  vblank without tearing. The swapchain's frame statistics (the missed-refresh counter the path
  already keeps) show which happened.
- **The copy engine** presents through the presentation manager, always on a vblank. With a DWM or
  timer pacing it is either the same (the newest buffer wins) or it queues; its buffer count and
  the present statistics show which. If it queues, the copy engine stays a vsync-only option at
  first.

### 3.2 The nearest compositor on D3D11

The step returns a `Pick`; the path draws one slot with weight 0, as passthrough already does.
`Repeat` re-draws the last shown slot (kept as `m_lastSlotA`, as holds do now), with the fallback
chain the D3D9 compositor has (last shown, then before, then after). Outcome fields as the D3D9
`NearestCompositor` fills them, so the temporal line keeps `pick=` and no `op=`, and the marker's
pick cells and compositor ID are unchanged. With the copy engine every nearest present is one frame
as it is, so it is copied on the copy engine.

### 3.3 The timer

What the 2026-10-28 `b:60` recording measured (5.3 minutes, D3D9 timer present):

- deadline spacing 16,666.7 us = 59.99988 Hz (the period rounds to whole QPC ticks: 0.12 ppm);
- the card's display at 60.000199 Hz (the 2026-10-20 custom mode);
- the timer woke late by p50 0.32 ms, p90 0.53, p99 0.84, max 2.2 ms;
- 0 capture repeats, 1 capture skip, 0 relay repeats in the video.

Why the 2026-08-30 rejection of timer presents (`docs/flip-present-spec.md`) may not hold now: the
card then ran at 59.9505 Hz, so a 60 Hz timer slipped a whole refresh about every 20 seconds.
Against 60.000199 Hz it slips about once every 52 minutes (an estimate from one 5-minute run; the
two clocks run on different crystals and drift with temperature).

What still matters is the wake jitter. When the timer's phase reaches the card's refresh edge it
drifts past it at about 0.09 us per present, so presents near the edge fall either side of the
refresh for as long as the jitter band takes to cross: at plus or minus 1 ms, about 22,500
presents (about 6 minutes) of repeats and skips, once an hour. An estimate, to be measured.
Measured 2026-10-30 (7.2): no such band at 10 and 100 times the real drift; a slip was one
repeated refresh.

The tight wait: the high-resolution waitable timer to about 1 ms before the deadline, then a spin
on QPC to the deadline (yielding with `SwitchToThread`). Cost: up to about 1 ms of one core per
present. Expected jitter: tens of microseconds, which would shrink the edge crossing to seconds.
The wake lateness is logged as `jit=` already; the change is judged on its distribution.

### 3.4 `o:<fps>`

Accepted once the D3D11 path paces on a timer: the step with timer deadlines. The flow planner's
deadlines (`policy::PlanNextPresents`) are the timer's own, exactly one period apart, which removes
the open question of present-step spec 6 for this mode. The planner's period under DWM pacing stays
open (the mean of recent presents, `PresentHistory`, is the candidate).

### 3.5 As built (2026-10-29, after the rig; this replaces the planned table)

The mode string's letter picks what the policy step decides; the part after the colon picks the
clock; together they pick the present path. Every row runs the one `policy::StepPresent`.

| mode | the step decides with | clock (`Pacing.h`) | present path and target | draws | `-wgc` |
|---|---|---|---|---|---|
| `b`, `b:vsync` | `DecideComposite` | `PathPacing`: the D3D11 waitable object, the card's vblank | D3D11, flip-model swapchain; with `-wgc` the composition swapchain, 2 buffers | passthrough or the blend (`-blend` shader if typed) | yes, frames shown as they are copied on the copy engine |
| `o`, `o:vsync` | `DecideComposite`, plus the flow planner (`PlanNextPresents`) | as `b:vsync` | as `b:vsync` | passthrough, the flow warp, the blend when no flow | yes |
| `t:<fps>` | `SelectFrame` | `TimerPacing`, tight wait | D3D11, composition swapchain, 3 buffers | the picked frame as is | yes |
| `b:<fps>` | `DecideComposite` | `TimerPacing` | D3D11, composition swapchain, 3 buffers | as `b:vsync` | yes |
| `o:<fps>` | `DecideComposite` + planner (period = the timer's) | `TimerPacing` | D3D11, composition swapchain, 3 buffers | as `o:vsync` | yes |
| `t`, `t:dwm` | `SelectFrame` | `PathPacing`: the D3D9 present blocks on DWM's compose clock | D3D9 swapchain, `NearestCompositor` | `StretchRect` of the picked frame | ignored |
| `b:dwm` | `DecideComposite` | as `t:dwm` | D3D9 swapchain, `BlendCompositor` | passthrough or the ps_3_0 blend | ignored |
| `t:<fps> -d3d9`, `b:<fps> -d3d9` | as without | `TimerPacing` | D3D9 swapchain, IMMEDIATE present | as `t:dwm` / `b:dwm` | ignored |
| `t:vsync`, `o:dwm` | refused | | | | |

What each decider uses, from `PolicyConfig` (set in `TemporalCaptureMode::Setup`):

- Every mode: the static lag (`-lag`), the comb lock and its pull (`-nolock` off), the bracket,
  stage-6 dejitter (with flip timing).
- `DecideComposite` only (`b`, `o`): the passthrough gate (show a real frame within a quarter
  period sharp), the phase-step lookahead with late moves and its refusal (when the comb is one
  source frame), the tooth guard and its stall-resume exemption. They exist to keep blends rare
  and useful; `SelectFrame` never blends, so it needs only its 1 ms stickiness band against
  flip-flopping at the midpoint.

`-tint` borders every synthesized frame on both paths. Code each axis reuses: one policy step and
state for every mode; one D3D11 backend drawing all
three compositors (`D3D11PresentBackend`), whose target is chosen once in its constructor
(`UsesComposition`, which the window setup asks too); the composition swapchain
(`CopyEngineTarget`) shared by `-wgc`'s copies and the timer; two pacings. The D3D9 path keeps its
own compositors (`NearestCompositor`, `BlendCompositor`) until it is removed.

## 4. Order, each step with its gate

Every step keeps the suite byte-identical to its baseline: the policy does not change, and the
corpus replays decisions, which a present path cannot change. The gates that matter are captures.

1. **Pacing measured.** A probe (a `diag`-style mode or a launch option on the D3D11 path) logs the
   intervals of `DwmFlush`, of the tight timer and of the waitable object, and the present
   statistics under sync interval 0, on the rig, in a game and on the desktop. Gate: the
   distributions written into this spec; the `DwmFlush` intervals against the D3D9 `:dwm` captures
   of 2026-10-28.
2. **`IPacing` and the tight timer**, the D3D9 path still in use. Gate: a `b:60` capture (D3D9,
   tight timer) with `jit=` well under today's p50 0.32 ms, and the line formats unchanged.
3. **The nearest compositor on D3D11**, behind the mode table (t modes move). Gate: a `t:dwm` and a
   `t:60` capture each: the stream's health check, the replay against the field as the `kcd_60x2_tdwm` fixture
   reads (pick census, repeats), the line templates against the 2026-10-28 `t:dwm` log.
4. **`b:dwm` and `b:<fps>` on D3D11.** Gate: the same against the `kcd_60x2_bdwm` and
   `kcd_60x2_b60` fixtures' captures.
   **The cost bench**, after steps 3 and 4: every moved mode against its D3D9 row, in one bench
   run of the leanest design (one launch per row, a no-relay row, the minutes said first): `t:dwm`,
   `t:60`, `b:dwm`, `b:60`, each on D3D9 (the commit before the move) and on D3D11, GPU % and the
   relay's CPU. What it must answer (see 4.1).

### 4.1 What the bench checks, and why

The move adds no copy between the two APIs: the D3D11 path opens the D3D9 ring slots as shared
textures and samples them in its draw, as `b:vsync` does today. What changes per present:

| | D3D9 today | D3D11 after |
|---|---|---|
| a nearest present | `StretchRect` of one slot | a full-screen draw of one slot (a copy-engine copy with `-wgc`) |
| a synthesized present | the blend, then a `StretchRect` into the hold surface | the blend; a hold re-draws it |
| the present | D3D9 `PresentEx` | the D3D11 flip present (`b:vsync`: about 5.5% against `b:dwm`'s 9.4% on the 2026-09-18 cost ladder) |
| a timer present | the waitable timer | the waitable timer plus up to ~1 ms of spin on one core |

So the bench answers: does the nearest draw cost more than `StretchRect` did (the one row that
could get worse), and what the timer's spin costs in CPU. A mode that gets more expensive goes to
the user before it ships.
5. **`o:<fps>` accepted.** Gate: a `o:60 -wgc` capture with stutters (the forge walk): the `fl=`
   shares and the planner line, as the stage-3 smoke test read them.
6. **The long timer capture.** About an hour of `b:60` (or `o:60`) on the D3D11 path: whether the
   predicted slip shows, how long it lasts, against the estimate of 3.3.
7. **Removing the D3D9 present path**, if the user chooses it (6).

Each moved mode joins the corpus as a fixture named for its mode (`kcd_60x2_<mode>_<date>_<n>`),
replacing the D3D9 one only if the user says so.

## 5. Not in this spec

- `vsync`, `60`, `diag`: the original modes stay on D3D9 as they are (they are the baseline the
  cost experiments of 2026-10-03 to 10-07 measured against).
- NvFBC capture's D3D9 device.
- Locking a timer's phase to the sink (head-1 flips): modelled worse on 2026-08-30 for a noisy
  sensor; not revisited here.
- `t:vsync` (the user's item, after this).

## 6. Decisions for the user

Each with Claude's default, which the user owns. Answered 2026-10-29: every default taken, the
spin on before it is measured (the user's choice between that and off until measured).

- **The D3D9 present path once the modes have moved.** Default: keep it one release as a hidden
  fallback (it is today's refusal advice when the D3D11 path cannot start), then remove it.
  TAKEN: hidden `-d3d9`, which puts `t`, `t:dwm`, `b:dwm`, `t:<fps>` and `b:<fps>` back on the
  D3D9 present (`launch::MovesToD3D9`) and is ignored, with a log line, for every other mode.
- **`b:dwm` as the fallback the refusal advice names.** Once `b:dwm` is D3D11 it is no fallback for
  a D3D11 failure. Default: the advice names `-nocopyengine` first, and the hidden D3D9 option while
  it exists. TAKEN: a copy-engine failure names `-nocopyengine`; any other D3D11 failure, and the
  stalled-output popup, name `b:dwm -d3d9`.
- **`o:dwm`.** Default: refused, as today; nothing has asked for it. TAKEN.
- **The tight timer's spin.** Default: on for every timer mode once stage 2 shows it working, as
  tested defaults are; its CPU cost is measured in stage 4. TAKEN as on from the start, with a
  hidden `-notightwait` to turn it off.
- **`-tint` on D3D11.** Default: not ported; it is a debug aid the marker's executor cells replace.
  TAKEN at first, then REVERSED by the user 2026-10-29 ("if we have a gap now with d3d11 modes,
  we should add back the tint behavior"): `-tint` works on every path. On D3D11 it is a pass of
  its own (`DrawTint`, four border strips as viewports) after whichever executor drew the frame
  (blend, `-blend` shader, scaler, warp), on every frame whose pixels are not one real frame,
  holds included; the D3D9 blend keeps its in-shader border. Colours on D3D11 (the user, the same
  day, since `o` mixes warps and blends in one video): red a blend, green a flow warp, blue a hold
  of either; the D3D9 blend only blends, so it stays red. Seen on the card display in Pandora
  (screenshots, border strips read as pixel means against shots without `-tint`, which read as
  game picture): red on `b:60 -tint` blends, green on 3 of 11 shots of `o:vsync -wgc -tint` (the
  rest real frames, unbordered). Blue not seen: holds of a synthesized frame ran 2 in about 7,400
  presents.
- **Step 1's probe** (asked 2026-10-29, since the whole spec was built at once): no separate mode.
  Every temporal run logs its pacing's statistics at exit, so step 1 is running the moved modes
  on the rig (section 7).

## 7. As built, and what is owed

The code (2026-10-29, uncommitted when written):

- `launch::ParseMode`: `t`, `t:dwm`, `b:dwm` set `dwmClock` and `d3d11Present`; `t:<fps>`,
  `b:<fps>` and the new `o:<fps>` set `d3d11Present`. `t:vsync` and `o:dwm` stay refused. Hidden
  options `-d3d9` and `-notightwait`. The suite pins the grammar and `MovesToD3D9`.
- `src/relay/Pacing.h`: `IPacing` with `PathPacing` (the D3D11 waitable object, or under `-d3d9`
  the blocking INTERVAL_ONE present), `DwmPacing` (`DwmFlush`, then the D3D11 path's own wait for
  room) and `TimerPacing` (`PresentScheduler`, tight wait unless `-notightwait`, then the same
  wait for room). The loop reads `Wait`, steps, composes, presents with `OnVblank()` as the sync
  interval, `Advance`. Each logs at exit `pacing summary (<clock>)`: the interval between wakes
  (p50, p90, p99, max, mean and its rate), the timer's wake lateness, and the D3D11 path's wait
  for room after DWM's clock or the timer. That last figure is the answer to 3.1's question of
  whether the waitable object turns a DWM or timer pacing back into the output's vblank.
- `PresentScheduler::SetTightWait`: the timer to 1 ms before the deadline, then QPC with
  `SwitchToThread` between reads. Off for `TimerCaptureMode` (`60`) and `diag`.
- `D3D11PresentBackend` draws the nearest selection (3.2): the picked slot as a passthrough, a
  repeat from the last slot shown, `pick=` and no `op=` on the line, marker compositor ID 0; with
  the copy engine every nearest present is a copy.
- The mode name ends `, paced by <clock>)`; `mktrace.py` reads only its `Temporal[ kind] (`
  prefix. The resolved-options line is unchanged (its "(b:vsync)" names the D3D11 path).
- The suite's policy output is unchanged: the grammar and options tests only add checks.

### 7.1 The functional check, 2026-10-29

A build of the working tree above, on the rig, one launch per row: 60 s over the desktop (a 240 Hz
source display), then 90 s of walking in KCD's save room (no forge, so `o:60` was checked for
working, not for planning under stutter).

The summary's controls all passed:

| control | row | read |
|---|---|---|
| a known clock | `b:vsync`, desktop | interval p50 16,666 us, p99 16,726, mean 60.008 Hz |
| a faster clock | `t:dwm -d3d9`, desktop | p50 4,165 us, mean 239.99 Hz |
| timer lateness against `jit=` | `b:60 -notightwait`, desktop | woke late p50 269 us, p99 924; `jit=` p50 283, p99 935 (the compose between wake and present, about 14 us) |

What the rows measured (wake = the pacing's own clock; room = the D3D11 present's wait after it):

| row | where | wake interval p50 / p99 | timer late p50 / p99 | room p50 / p99 | refreshes with no new frame |
|---|---|---|---|---|---|
| `t:dwm` | desktop | 16,665 / 16,854 us | | 16,436 / 16,533 us | |
| `b:60 -notightwait` | desktop | 16,627 / 17,367 | 269 / 924 us | 1 / 5 us | |
| `b:60` | desktop | 16,666 / 16,713 | 0 / 47 us | 0 / 1 us | |
| `b:vsync` | game | 16,666 / 16,761 | | | 1 of 5,805 |
| `t:dwm` | game | 16,666 / 16,776 | | 1 / 16,477 us | 3 of 5,782 |
| `b:dwm` | game | 16,666 / 16,775 | | 1 / 16,474 us | 3 of 5,826 |
| `b:60` | game | 16,666 / 16,765 | 9,482 / 9,699 us | 16,578 / 16,654 us | 2 of 5,869 |
| `b:60 -notightwait` | game | 16,635 / 17,565 | 370 / 1,205 us | 15,435 / 15,932 us | 1 of 5,791 |
| `o:60 -wgc` (copy engine) | game | 16,666 / 16,667 | 0 / 0 us | 5,163 / 5,400 us | 1 (copy engine: 0 skipped) |

Every row presented about 60 a second with 99.9% of presents flipped straight to the display
(overlay), and the card pictures (screenshots) were right for the nearest selection and the flow.

Findings:

- **On the D3D11 flip-model swapchain, the output's vblank takes over the other clocks.** The
  waitable object only gives room once the previous frame is on screen, so after `DwmFlush` or the
  timer the present waits for the card's vblank. On the 240 Hz desktop `t:dwm` presented at the
  card's 60.03 Hz, not DWM's 240 (the D3D9 `t:dwm` read 240). In game, DWM's clock is the card's
  anyway.
- **The timer is overridden in game without the copy engine.** `b:60` waited a whole refresh for
  room (p50 16.6 ms), so every wake came 9.5 ms after its deadline and the timer never waited:
  the loop ran on the card's vblank, and `jit=` read a constant 26 ms (p50 26,081 us). On the
  desktop the same build waited about 0 us for room, and the timer kept its phase. Both read
  99.9% overlay, so composition does not explain the difference: NOT EXPLAINED.
- **With the copy engine (`o:60 -wgc`) the timer keeps its phase in game:** it woke on time and
  the buffer came back 5.2 ms later, under a refresh.
- **The tight wait works where the timer actually waits:** desktop p99 lateness 47 us against
  924 us. Its CPU was not read (the runner's per-relay CPU figure is dominated by the capture).

What this means for the design, for the user to decide: on this swapchain a timer or DWM pacing is
the card's vblank whenever room is not yet there, so `b:60` without the copy engine behaves like
`b:vsync` with a deadline offset in game. Options: the tearing-allowed flag with sync interval 0
(the frame shows at the timer, possibly tearing on the card), a frame latency of 2 (room one
present sooner, one frame more latency), or accepting it (timer modes become vblank modes on the
swapchain path; the copy engine keeps true timer pacing).

**Decided by the user 2026-10-29:** "if b:60 waits on vblank, then it's not b:60. we need a
different timer mechanism", then option (Claude's recommendation) "the composition swapchain the
copy engine already uses, for every timer mode": "that seems reasonable", asking that its cost be
measured. Built the same day:

- A mode on a timer presents through `CopyEngineTarget` whatever the capture
  (`D3D11PresentBackend`'s `composition` flag, `TemporalCaptureMode::PresentsThroughComposition`
  for the output window); frames are copied there only with `-wgc` and the copy engine, drawn
  otherwise. `SetTargetTime` is not used.
- `TimerPacing`: a present held for room past half a period hands the step the time it was let
  go, not the timer's deadline, and the summary counts those (`presents were held for room`).
- Gate: in game, `b:60`, `t:60`, `o:60` (NvFBC, every present drawn) and `b:60 -wgc` (copies):
  timer lateness near 0, wait for room under a refresh, held count 0, refreshes with no new frame
  as on `b:vsync`; then the cost of drawn presents through the composition swapchain in the
  bench (with and without `-wgc`), the user's ask.

**Measured 2026-10-29 on Pandora** (the built-in benchmark, uncapped, frame generation off, about
68 fps GPU-bound; one launch per sitting, one run per row; the first two sittings below are
called A and B):

- 2 buffers (sitting A): the composition swapchain did NOT free the timer under this load. Every
  timer row waited p50 16.1-16.3 ms for a buffer and 89-98% of presents were held past half a
  refresh (`b:60` 6,534 of 7,369; `-wgc` 7,165 of 7,455): the card paced it again, now without the
  26 ms offset (the held rule). KCD's room had let it run (`o:60 -wgc`, 5 ms): the load decides.
- 3 buffers for timer modes (sitting B; `CopyEngineTarget::Setup(..., buffers)`; `b:vsync -wgc`
  keeps 2): held 0 on every timer row; woke late p99 75-83 us; wait for a buffer p50 1 us (`b:60`,
  `-wgc`), 0.9 ms (`t:60`), 5.6 ms (`o:60`), all under a refresh; wakes at the timer's rate, not
  the card's (the 60.0023 Hz read here was the summary's rounding; the true rate is 59.99988 Hz,
  7.2). Refreshes with no new frame: 0 on `b:60`, `t:60`, `o:60`, 1 on `b:60 -wgc`, against 8 on
  `b:vsync` in the same sitting (the timer rows' counts are not evidence, 7.2).
- Cost (score lost against the mean of the no-relay rows at both ends; sitting A / B):
  `b:vsync` 3.13 / 3.29%, `b:60` (drawn through the composition swapchain) 4.18 / 4.29%, `t:60`
  4.23 / 4.29%, `o:60` 5.30 / 5.33%, `b:60 -wgc` (two thirds copied) 2.09 / 2.24%. The third buffer
  costs nothing measurable. A drawn timer present costs about 1 point (about 60 of 5,890) more
  than `b:vsync`'s flip-model present; whether that is the composition swapchain (the cross-device
  fence on every drawn present) or the timer is NOT separated, and the D3D9 rows (`-d3d9`) of the
  4.1 bench are still owed.

**The matched and old-against-new sittings, 2026-10-29 (Pandora as above; C and D):**

| mode | old: `982c96b`, D3D9 | new: the 3-buffer tree, D3D11 | same launch |
|---|---|---|---|
| `b:60` | 4.87% | 4.19% | D |
| `t:60` | 4.73% | 4.10% | D |
| `b:dwm` | 4.64% | 2.96% | D |
| `t:dwm` | 4.73% | 3.01% | D |
| `b:vsync` / `b:60` | | 2.94% / 4.12% | C |
| `b:vsync -wgc` / `b:60 -wgc` | | 1.95% / 1.88% | C |

- The timer modes are cheaper on D3D11 than they were on D3D9 (about 0.65 points, 40 points of
  5,878) and now keep time: held 0 on `b:60`, 6 of 7,386 on `t:60` (its wait for a buffer ran
  p50 7.6 ms that launch, near the half-period rule's 8.3 ms), 0 refreshes with no new frame.
- With `-wgc` the timer costs nothing over `b:vsync` (1.88 vs 1.95%); without it a drawn timer
  present costs 1.2 points more than `b:vsync`'s (4.12 vs 2.94%), so that point is drawing into
  the composition swapchain, not the timer.
- **The `:dwm` modes on D3D11 lost frames on the card in Pandora:** 167 (`b:dwm`) and 284 (`t:dwm`)
  refreshes showed no new frame over about 7,000 presents, 1.4 and 2.4 a second, against 0.065 on
  `b:vsync`. `DwmFlush` woke at p50 15.2 ms, p90 30 ms (DWM composing at the game's rate, about 68
  fps here, unlike KCD's fullscreen where it ran at the card's), the wait for room then lined
  each up with the card, and they presented 57 a second. Their lower cost is fewer presents (the
  D3D9 rows presented about 8,630 times, the game's rate). The D3D9 `:dwm` rows report no present
  statistics (bitblt), so whether they showed fewer repeated refreshes is not measured here.
- **Decided by the user 2026-10-29:** "t:dwm and b:dwm can go back to d3d9", "keep timers on
  d3d11"; bare `t` stays `t:dwm` ("we'll make it t:vsync once we add t:vsync for real"). Built
  the same day: `t`, `t:dwm`, `b:dwm` parse to the D3D9 present again (their clock is its
  blocking present); `DwmPacing` and the `DwmFlush` import are gone; `-d3d9` now moves only the
  timer modes of `t` and `b`; the refusal and stalled-output popups name `b:dwm` again. So the
  `:dwm` modes keep their old behaviour and cost until the D3D9 present is removed, and `t:vsync`
  is the nearest selection's way onto the card's vblank.

### 7.2 The tight timer on D3D9, the slip, and two instruments, 2026-10-30

Pandora as in 7.1, one launch per sitting, rows on the CI build of e17dd87 or on builds of the tree
with the two fixes below; desktop rows 45 s to 2 minutes.

**The tight timer on the D3D9 path (step 2, in Pandora rather than a capture):**

| row | `jit=` p50 / p90 / p99 / max | timer late p50 / p99 | cost |
|---|---|---|---|
| `b:60 -d3d9` | 21 / 30 / 161 / 3,174 us | 0 / 79 us | 4.58% |
| `b:60 -d3d9 -notightwait` | 296 / 516 / 853 / 3,522 us | 272 / 801 us | 4.63% |
| `t:60 -d3d9` | 18 / 25 / 58 / 533 us | 0 / 32 us | 4.48% |
| `b:60` | 203 / 321 / 453 / 4,098 us | 0 / 72 us | 3.98% |
| `b:60 -notightwait` | 7,642 / 7,791 / 7,901 / 8,532 us | 263 / 777 us | 3.81% |
| `t:60` | 6,314 / 6,464 / 6,592 / 8,068 us | 0 / 71 us | 4.02% |

- The `-notightwait` row reproduces the 2026-10-28 p50 of 0.32 ms under a different game, so the
  14-fold drop is the wait's. Step 2 is done.
- On D3D11 `jit=` includes the wait for room, so the D3D9 rows are the ones to read against 0.32 ms;
  on D3D11 read the summary's timer lateness.
- D3D11 stays cheaper than `-d3d9` in one launch: 0.60 points on `b:60`, 0.46 on `t:60`, as the
  old-against-new sitting read (0.68, 0.63).
- The tight wait costs nothing that shows: +0.17 points on D3D11, -0.05 on D3D9, the game's CPU %
  column 10.3-10.4 on all six. One run per row, so a few tenths are noise.

**Two instruments were wrong, and both are fixed in the tree:**

- **The summary's mean rate.** It summed whole microseconds, so a 16,666.7 us period read 16,666.0
  and 60.0023 Hz. The mean now comes from the first and last wake in QPC ticks. Control: `b:60` reads
  16,666.700 us, 59.999880 Hz, the period of 3.3.
- **"Refreshes showed no new frame" on the timer modes.** It timed the end of the buffer wait, and
  a timer that finds its buffer back already only reads its own period. Under `b:59.98`, where a
  repeat must happen every 50 s, it read 0 or 1 on rows that held 2 or 3. So every timer row's zero
  in 7.1 says nothing about repeats, with `-wgc` or without. The vblank modes are not affected,
  since their wait really waits. The new count uses `GetDisplayedTime` of the independent-flip
  statistics: `copy engine displayed:` at exit and a `display gap:` line per gap, with the old count
  left beside it. It runs wherever the composition swapchain does, so not on `b:vsync` without
  `-wgc`, and frames composed by Windows are not read (their next gap is dropped).

| control of the new count | expected | read |
|---|---|---|
| `b:60`, desktop, 45 s | 0 | 0 |
| `b:30`, desktop, 45 s | a repeat on every present | 1,384 of 1,389 presents |
| `b:59.98`, desktop, 2 minutes | 2.4 from the rates | 2 |
| `b:vsync -wgc`, Pandora | as the old count (7) | 8 |

**The slip.** A timer slower than the card was run off-rate, so slips come at a known rate:

| row | slips expected from the rates | slips read | each slip |
|---|---|---|---|
| `b:59.98`, Pandora, 123 s | 2.5 | 2, 49.6 s apart | one repeat, 2.00 refreshes |
| `t:59.98`, Pandora, 123 s | 2.5 | 2 | one repeat |
| `b:59.98 -wgc`, Pandora, 123 s | 2.5 | 2 | one repeat |
| `b:59.998`, Pandora, 18 minutes | 2.1 | 2, 415 s apart (505 predicted) | one repeat |

Those rows each also show a gap in the first 7 s; `b:60` has one there too, so it belongs to the
start. `o:59.98 -wgc` held more (four gaps, 623 presents held) because its flow work keeps the wait
for room long; it was not read further.

- A slip is one repeated refresh, with 0 skipped presents and no band of repeats and skips, at 10
  and 100 times the real drift, against the 6 minutes that 3.3 estimated. The real drift itself was
  not run. A reading, not traced: the presentation queue is first in, first out, so a frame that
  misses one refresh moves every later frame by one and the jitter has nothing left to flip.
- The 415 s against 505 s is not explained.
- A timer faster than the card (`b:60.02`) falls onto the card: 5,286 of 7,390 presents held, wait
  for room p50 16.2 ms, one gap, 0 skipped. Nothing breaks; it is `b:vsync` with a late decision.

**The rates, read 2026-10-30.** The card fits 60.000208 Hz on the desktop (the wakes of `b:vsync`,
residual sd 14 us), matching the CRU mode's 60.000199 and a slip every 51 minutes. In Pandora it
fits 60.000057 Hz, and 60.000035 Hz on 2026-10-29, a slip every 94 to 108 minutes. A second way
agrees: under load the wait for room is the distance from the timer's deadline to the card's
next refresh, and it fell 3.0 and 3.1 us a second on `b:60 -notightwait` and `t:60`. The desktop
reading came between those two game readings of 2026-10-30, so this is not a trend in time; why
load moves the card 2.5 ppm against QPC is not known.

**The wait for room reads the phase.** In one launch the D3D11 rows settled at wait p50 0.2 ms, 6.3
ms and 7.3 ms, and at about 1 us in another: the timer's phase against the card is set per run. The
step from no wait to a wait came 7 and 27 s into a run on the two rows that had one. It looks like
the queue going one frame deeper, which would add up to a refresh of latency to that run; not
traced.

**`o` on a timer, in Pandora, not at the forge:** `o:60 -wgc` fl=hit 2,489, fl=late 14, fl=own 0,
cost 3.48%, held 0. The stutter question was not run (see the owed list).

Owed, in the order of section 4, each on the rig:

1. DONE 2026-10-29 (7.1), except the copy engine's statistics under DWM pacing. Step 1 as runs:
   `t:dwm`, `b:dwm` and `b:60` on the D3D11 present, in a game and on the
   desktop, reading the `pacing summary` lines against the D3D9 captures of 2026-10-28 and the
   present statistics under sync interval 0 (the copy engine's skipped and composed counts with
   `-wgc`). Write the distributions into 3.1 and 3.3. The summary's controls, read first: a
   `b:vsync` run must read its interval p50 at the card's 16,667 us with a narrow spread (a known
   clock); a `b:60 -notightwait` run must reproduce the timer's wake lateness of 2026-10-28 (p50
   about 0.32 ms) from the summary, and its `jit=` lines must agree with it; and a `t:dwm -d3d9`
   run on the desktop of the 240 Hz source display must read about 4,167 us, the composed
   desktop's clock its blocking present follows, which a summary that only ever read the card's
   rate would fail. (`t:dwm` on the D3D11 present waits for room after `DwmFlush`, so whether it
   reads 4,167 or 16,667 there is the question, not a control.)
2. DONE 2026-10-30 (7.2), in Pandora. Step 2: the tight timer on the D3D9 path, `jit=` p50 21 us
   against the 2026-10-28 p50 0.32 ms.
3. Steps 3 and 4: `t:60`, `b:60` captures, the health check and the replay against the `b60`
   fixture, line templates against the 2026-10-28 logs. The cost bench of 4.1 is DONE for `t:60` and
   `b:60` (7.2); the `:dwm` modes stayed on D3D9.
4. Step 5: DROPPED by the user 2026-10-30 ("i think we skip the forge test for these"). `o:<fps>`
   was checked in Pandora only (7.2); the planner under stutter on a timer stays unmeasured.
5. Step 6: the hour-long timer capture, now only to confirm one repeat at the real drift (7.2).
6. Step 7: removing the D3D9 present after a release.
