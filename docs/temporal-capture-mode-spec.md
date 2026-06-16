# Temporal Capture Mode — spec (nearest-pick foundation for interpolation)

Status: implemented (stage 1). This is the foundation the blend and optical-flow modes build on.

## Purpose

Produce a **smooth, fixed-rate** capture stream from a **variable-rate source** (e.g. a game
running 80/90/100/144 fps captured at a steady 60). Temporal mode does this with the simplest
possible content step — **nearest-frame selection** — so that the *timing/buffering* can be
proven correct before any interpolation is added. The staged roadmap:

1. **Temporal (this mode)** — pick the captured frame nearest the target time. No interpolation.
2. **Blend** — same flow, linearly blend the two bracketing frames (own spec, later).
3. **Optical flow (NVOFA)** — replace the blend with motion-compensated interpolation (later).

Each stage changes only the final "what do we do with the bracketing pair" step; the
scheduling, capture, and buffering below are shared.

## Why the old approach couldn't do this

`NvFBCFrameGrabInfo` exposes **no source-generation timestamp and no new-frame flag**
(`inc/NvFBC/nvFBC.h:125`). The previous modes grabbed with `NVFBC_TODX9VID_NOWAIT` (return the
latest frame immediately), so the only time information available was *our polling clock* —
useless for reconstructing a variable source's true frame timing. You cannot bracket a target
time against source frames you can't timestamp.

## Capture timing — blocking grab

NvFBC's grab flags (`inc/NvFBC/nvFBCToDx9vid.h:106`) include a **blocking** mode. We use
`NVFBC_TODX9VID_WAIT_WITH_TIMEOUT`: the grab returns when NvFBC has a new frame, so the QPC
timestamp at return approximates that frame's arrival time. The timeout (`kGrabWaitMs`, 100 ms)
governs only how fast the capture thread notices a stop/stall — not pacing.

**Measured semantics (2026-06 validation runs): "new frame" = new display scan-out /
composition, NOT new content frame.** On a fixed-refresh source display the grab wakes once
per refresh regardless of the content's framerate — measured `dt` median ≈ 4.2 ms (~240 Hz)
for *both* a 60 fps and an 80 fps UFO source on a 240 Hz monitor. HW cursor moves also wake
the grab (`bWithHWCursor = 1`; observed as `dt` ≈ 100 µs spikes). Implications:

- **Fixed refresh:** the ring holds several timestamped copies of each content frame
  (refresh-rate / content-rate of them), and timestamps are *refresh* times. Nearest-pick
  selection remains content-correct (the nearest capture to the target holds the content the
  display showed then — validated EXCELLENT at 60→60), but the bracket **weights** measure
  position between adjacent *captures*, not between content frames — insufficient for blend.
- **VRR / G-Sync engaged (the primary use case):** scan-out is driven by frame delivery, so
  wakes ≈ content frames and timestamps ≈ true content arrival. No dedup needed. Each
  session's capture log (`dt`) self-verifies which regime is active.
- **Back-pocket mitigation for fixed-refresh sources (not yet implemented):** NvFBC diffmaps
  (`bDiffMap` / `ppDiffMap`, per-block change maps documented in the setup params). The
  capture thread would skip *publishing* unchanged grabs, leaving only content-distinct frames
  in the ring with timestamps quantized to one refresh — accurate enough — and making weights
  meaningful for blend. Build this when a no-VRR use case matters (desktop capture, games
  without VRR support); skip the overhead otherwise. Testing without VRR can instead set the
  monitor refresh equal to the content rate (content == refresh ⇒ wakes == content frames).

**Assumption: source rate ≥ present rate** (no upconversion yet). If the source runs slower,
no frame newer than the target exists, so we present the newest-before (a repeat) and log it;
true upconversion (interpolating to *add* frames) is future work.

## Architecture — two threads, strictly one-way (capture → ring → present)

The shared logic lives in two reusable classes; a mode composes them and adds only its
selection/interpolation step. Vocabulary: the capture side **grabs** (from NvFBC); the present
side **selects** (from the ring).

- **`CaptureRing`** (`CaptureRing.{h,cpp}`, shared with future blend/optical-flow modes): owns
  the capture side — a **private D3D9Ex capture device** (separate from the present device), the
  ring slots, and the capture thread. Each ring slot is a render-target texture **shared across
  both devices** via a D3D9Ex shared handle (created on the capture device, opened on the present
  device), so the present thread reads its alias without touching the capture device. Per source
  frame the capture thread: blocking NvFBC grab into a single capture-target surface (`dwNumBuffers
  = 1`) → `StretchRect` it into the next ring slot on the capture device → **coherency flush** (an
  event query issued and drained before publishing, so the capture GPU's write is complete before
  the present device can read the shared slot — see "B sync fix" below) → QPC-stamp at arrival →
  publish a monotonic count via an **atomic**. Nothing is signaled *back* to this thread.
  `FindBracket(targetQpc)` → before/after frames (present-device aliases) + timestamps + depth +
  the interpolation weight a blending consumer would use.
  *(Direct-write — NvFBC writing each frame straight into a ring slot via `dwBufferIdx`, dropping
  the capture target and that `StretchRect` — is a future perf optimization, see Future work; it
  is NOT the current design.)*
- **`PresentScheduler`** (shared with timer mode): the absolute-QPC present clock — purely
  *when* to act, never *what* to do.
- **Present thread** (the main thread — owns the HWND, message pump, `PresentEx`): the mode's
  own loop. Each deadline it computes `target = deadline − bracketingDelay`, calls
  `FindBracket(target)`, selects per the mode's policy (temporal: the nearer frame),
  `StretchRect`s to the backbuffer, and presents.

The **only** shared state between threads is the atomic published index (capture writes,
present reads). This is the easy kind of concurrency — one-way producer→consumer.

Both D3D9Ex devices are created with `D3DCREATE_MULTITHREADED` (present device in `InitD3D9`,
capture device in `CaptureRing::Start`). The flag affects only our process's devices, not the
captured game. (With the two-device split each device is touched by a single thread, so the
flag is largely defensive — the cross-device hazard is GPU coherency, handled by the flush, not
CPU-side device-call reentrancy.)

### Buffer rules (no purging, no backward comms)

- **No frame lifetime management.** The fixed ring overwrites the oldest slot as capture
  advances — the overwrite *is* the eviction. Frames older than the presented one are harmless.
  This is what keeps the design one-way: present never tells capture which slots to free.
- **Lock-free safety.** Capture writes a slot fully, *then* publishes its index. Present reads
  only published slots in `[p−(RING_SIZE−1), p−1]` — never the slot capture is writing — and
  only touches the newest few (the bracket sits near the lagging target). At `RING_SIZE = 8`,
  capture cannot wrap onto a slot present is reading during one microsecond-scale `StretchRect`.
- **Ring size.** Start at 8 to validate; shrink later from the logged "before-frame depth"
  (how far back the bracket reached). Realistic floor ~3–4 (not 2 — capture leads the lagging
  target). Size scales with the source/output gap.
- **`bracketingDelay`** = one present period. Lagging the target by ~one period guarantees a
  frame newer than the target has arrived (given source ≥ present), so the pair brackets it.
- If the target falls **older than the whole ring** once warm (`p ≥ RING_SIZE`): logged loudly
  (ring too small / delay too large), never silent.

### Shared scheduler — `PresentScheduler`

`PresentScheduler.{h,cpp}` holds the timer mode's validated absolute-QPC logic: period =
`round(freq/framerate)`, a `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` timer, `Seed()`,
`Deadline()`, `WaitUntilDeadline()` (with the >1-period catch-up clamp), `Advance()`. Both
`TimerCaptureMode` and `FrameTemporalCaptureMode` use it, so present pacing is identical and
drift-free across modes.

### Nearest-pick selection

Among the bracketing frames, pick the one whose timestamp is genuinely closest to `target`
(before *or* after). This fixes the prior bug where selection always preferred the "before"
frame and so never used a future frame (`FrameTemporalCaptureMode.cpp` old `:213`) — see
`temporal-blend-future-frame-findings.md`.

## Logging (the correctness instrument)

Verbose by design for now — this is how we validate the scheduler before adding interpolation.
Logging is enabled only if an empty `NvFBCR.log` exists next to the exe at startup. Two streams
(the logger is mutex-guarded, so concurrent thread logging is safe):

- **Capture thread**, per frame: `capture #<n> arr=<µs> dt=<µs>` — the source arrival timeline;
  `dt` (inter-arrival gap) ≈ the source frame period, confirming the blocking grab returns at
  the true source rate and showing a variable source's real cadence.
- **Present thread**, per present: `temporal dl=<µs> tgt=<µs> before=<µs>(d<depth>) after=<µs>
  w=<weight> pick=<side> jit=<µs> pdt=<µs>` — scheduled deadline, target, the bracket's
  before/after timestamps (all relative to a base QPC) + the before-frame's ring depth, the
  chosen side, the *would-be* interpolation weight (`beforeDiff/(beforeDiff+afterDiff)` — what
  blend will use), present **jit**ter (actual present vs scheduled deadline = scheduler health),
  and **p**resent **d**el**t**a (actual inter-present gap, should hold at the present period).

The bracket timestamps double as the observed source timeline at the present points.

## Verification

- **Timer no-regression**: `60` UFO capture → still 0 dupes, `std(Δ)≈0.43` (matches dev `fb0e6e3`).
- **Temporal, mismatched source (e.g. 90→60 ⇒ 3:2, or 80→60 ⇒ 4:3 as actually run)**: from
  the logs, confirm the bracket **straddles** the target (before ≤ target < after) and present
  pacing is steady (`pdt` ≈ the present period). Output should show the clean, predictable
  cadence judder for that ratio (`detect.py pacing` period-2/3) — correct for nearest-pick and
  proof the bracketing/timing is right. Use the logged before-frame depth to pick the shrunk
  ring size. (Note: on a fixed-refresh display the `w`/`pick` pattern is refresh-quantized —
  see "Measured semantics"; the clean repeating weight pattern only appears when wakes track
  content, i.e. VRR or content == refresh.)
- Capture per `frame-pacing-drift-analysis.md`: CBR ~6 Mbps is fine; verify **0 OBS dropped
  frames**; compare against a same-session baseline.

## Validation results (2026-06-11, copy-path build, 240 Hz fixed-refresh source display)

Three runs (UFO test, OBS CBR ~6 Mbps): timer `60` 5 min; `t:60` with a 60 fps source ~6 min;
`t:60` with an 80 fps source ~6 min. Clips/logs: `frame-drop-analysis/bb_temporal_rework_*`.

| Run | detect.py | Key log stats |
|---|---|---|
| timer 60→60 | GOOD; 112 dupes all in two environmental bursts, else clean | (no per-frame logging) |
| t:60, 60 src | **EXCELLENT**, 97% smooth | `pdt` mean 16,666 µs; brackets straddling; 0 no-after |
| t:60, 80 src | POOR (98% bad) — **expected**: nearest-pick 4:3 cadence | same healthy `pdt`/brackets; **0 dupes** (always a fresh frame) |

Conclusions:

- **Stage 1 validated.** Scheduler extraction unregressed (timer matches baseline); the
  two-thread ring + bracketing work (60→60 EXCELLENT); the 80→60 period-3 judder is exactly
  what nearest-pick *should* produce and is the "before" picture that blend must beat.
- **Discovery:** grab wakes are scan-out-paced, not content-paced (see "Measured semantics"
  above) — `dt` ≈ 4.2 ms in both temporal runs, independent of content rate.
- **Known defect (~1%):** `target older than ring window` on ~250 of ~25k presents per run.
  At ~240 Hz wakes the 8-slot ring spans only ~34 ms, and capture bursts (cursor wakes,
  back-to-back returns) occasionally flushed it past the 16.7 ms-lagged target, so the mode
  presented a too-new frame. Fixes available: VRR/dedup (fewer publishes) and/or a larger
  ring. Revisit after the G-Sync / matched-refresh tests show the realistic wake rate.
- **`jit` (deadline → Present call) median ≈ 2.3 ms** in this round's *single-device* build —
  dominated by device-lock contention with the capture thread (which shared the present device).
  `pdt` held 16,666 µs mean, so pacing was unaffected. (Superseded: the **two-device B** rework
  eliminated this contention by giving capture its own device — `jit` on the current B build is
  ~270 µs at 60→60. Direct-write is a *separate* future perf optimization for the capture-side
  copy, not the fix for this `jit`.)

## Validation rounds 2–3 (2026-06-12, matched-refresh) — two NvFBC discoveries, three fix experiments

Round 2 re-ran the tests with the monitor refresh set to the content rate (60/100 Hz), making
capture wakes ≈ content frames for real. The output got *worse*, and the logs found why; the
attempted fix then exposed a second API discovery.

### Discovery 1 — the blocking grab holds the device lock for its entire wait

With wakes at the true source rate, present jitter (`jit` = deadline → Present call) measured
**exactly half the capture period at every rate**: 2.3 ms @ 240 Hz, 5.2 ms @ 100 Hz, 8.4 ms @
60 Hz. At 100→60, `pdt` (inter-present gap) quantized to the 10/20 ms capture grid instead of
16.7 ms. Conclusion: with `D3DCREATE_MULTITHREADED`, the blocking grab holds the shared D3D9
device lock for its whole wait (up to a full source period), so the present thread's
StretchRect/Present stall until the next source frame arrives — **presents were paced by
capture arrivals, not by the scheduler**. (The earlier 240 Hz "EXCELLENT" result was partly
luck: 4 ms max stalls rarely crossed scan-out boundaries.) Result at 60→60: 259 dupes / 2 min
of visible hitching.

### Discovery 2 — WAIT_WITH_TIMEOUT expiry returns SUCCESS with a stale re-grab

The attempted fix (timeout 100 ms → 2 ms, to bound each lock hold) backfired: NvFBC's timeout
expiry does **not** return empty — it re-grabs the unchanged screen and returns success. There
is no empty-return path. The 2 ms timeout therefore turned capture into ~500 Hz polling of
duplicate content with meaningless timestamps: 162k captures in 5.5 min at a 60 fps source
(`dt` ≈ 2.3 ms, no 16.7 ms gaps), ring window shrunk below the bracketing lag, 20,882
"target older than ring window" errors vs 24 successful brackets, 931 dupes.

Corollary: **timing cannot distinguish** a frame arriving within ~1 ms of expiry from a stale
re-grab (the expiring grab consumes it). At rate-matched sources, drift makes the arrival
phase *dwell* at any fixed boundary for seconds per sweep cycle, so timing-based stale
detection would drop frames in multi-second stretches. Stale detection must be by **content**
— NvFBC's diffmap (`bDiffMap`/`ppDiffMap`; all-zero diff = unchanged).

### Three fix experiments (local branches off `temporal-capture-mode-rework`)

| Branch | Mechanism | Notes |
|---|---|---|
| `temporal-a-singlethread` | **One thread**: present loop pumps the ring between deadlines; grab timeout = time-to-deadline (the structural max for a single thread — the same thread must present); diffmap classifies the per-interval boundary expiry (~120 grabs/s) | No lock contention by construction; frames arriving during present work stamp ≤ ~2–3 ms late |
| `temporal-b-two-devices` | **Two threads, two devices**: NvFBC session rebound to a private capture device; ring slots are D3D9Ex shared textures opened on both devices; long event-driven grabs (100 ms), no polling, no diffmap | Purest timestamps. Risk: D3D9Ex shared surfaces have no cross-device sync — watch for intra-frame tearing |
| `temporal-c-diffmap` | **Two threads, one device**: keep 2 ms polling (bounds each lock hold), diffmap dedups the ~500 Hz stale flood, Sleep(1) after each stale skip hands the lock to the present thread (anti-convoy) | Smallest delta from current code. The timeout is the present-jitter ceiling (lock hold ≈ timeout, regardless of source rate): 4 ms is a viable trade, 8 ms re-creates the disease. Tune/configure only after validation |

Logging is the referee (upgraded on all three): capture lines fire **only on published,
content-distinct frames** (`dt` ≈ source period = healthy) and carry cumulative `skip=`;
present lines add `pub=`/`skip=`. The round-3 failure mode is now directly visible — `dt`
collapsing to ~2 ms or `skip` staying at ~0 (diffmap not flagging) would expose itself
immediately. Decision metrics per branch: `dt` ≈ source period, `jit` (predicted A ≈ sub-ms,
B ≈ sub-ms if sharing behaves, C ≲ 2–3 ms), `pdt` pinned at the present period, 60→60 dupes
at drift level only, ring-window errors ≈ 0, and (B) no tearing.

Test matrix: per branch, 60-source 5 min (rate-matched stress: drift sweep, dwell, the
original hitching) + 100-source 2 min (mismatch check: `dt` ≈ 10 ms, 5:3 bracketing,
un-quantized `pdt`).

## Round-3 experiment results (2026-06-13, 60→60, matched 60 Hz refresh)

All three built; `detect.py` (now with the roughness detector, see below) + log analysis:

| Branch | dupes/5min | jit median | Verdict | Finding |
|---|---|---|---|---|
| A (single-thread pump) | 159 (drift-level) | 266 µs | EXCELLENT 98% | Clean. Residual roughness = inherent nearest-pick drift slips (scattered, not periodic) — the artifact blend removes. Cleanest capture timing (dt p95 17.3 ms). |
| B (two devices) | 1 | 268 µs | "EXCELLENT" but **wrong** | Timing flawless, but **periodic ~25 s visual roughness** the dupe/pacing tools rated 100% smooth. |
| C (2-thread, 2 ms poll + diffmap) | 764 | 324 µs (p95 2 ms) | GOOD 50% | Roughest. Capture-side jitter (dt p95 28 ms); not chosen. |

### The roughness detector (new in `detect.py`)

Dupe/pacing analysis only sees *near-zero* deltas (duplicates); it is structurally blind to
*wrong-order / stale* frames, which are different from their neighbours (non-zero delta) but
from the wrong time. `analyze_roughness()` flags windows whose inter-frame delta-variance
spikes above the smooth floor and warns when episodes recur periodically (a timing/sync-defect
tell). It reproduced B's ~25 s episodes at timestamps matching the eyeball report, while
correctly leaving A unflagged-for-periodicity (A's slips are scattered/inherent). This is the
only instrument besides the eye that catches B's class of defect.

### B investigation — what the data eliminated, and the surviving diagnosis

B's glitch is "whole old frames," not tearing. Three mechanisms were proposed and **two were
refuted by data**:

1. *Same-slot write/read collision* — refuted by ring arithmetic: at 60→60 with 8 slots the
   write head and read position stay ~1–2 slots apart; a slot is re-used only ~8 frames
   (~133 ms) later. No collision.
2. *Reading a too-fresh slot before coherency settles* — refuted by age analysis: in rough
   windows the picked frame is *older* (~21 ms, all depth 1), never fresh.
3. **Confirmed diagnosis — cross-device pixel coherency.** Two independent measurements:
   (a) the picked-frame timeline is **100 % monotonic in rough windows** (0 backward jumps in
   the log's selected timestamps) and capture timing is a clean 60 Hz there — CPU-side
   selection/bookkeeping is correct; (b) per-frame scroll-direction analysis of the *video*
   shows **10 backward-scroll jumps in a 4 s rough window vs 0 in a smooth window** — the
   displayed pixels momentarily move to an earlier scroll position. Selection-forward +
   pixels-backward proves the present device reads **stale pixels** from a shared slot the
   capture device wrote: the `m_published` atomic synchronises the CPU side, but nothing
   synchronises GPU work across the two D3D9Ex devices. B-specific (single-device A cannot have
   it). The exact reason it concentrates at the ~25 s drift phase isn't modelled, but the
   stale-pixel mechanism itself is confirmed by the backward-scroll measurement.

**Fix under test (`temporal-b2-sync`):** after each capture-side `StretchRect`, issue a
`D3DQUERYTYPE_EVENT` and drain it (`D3DGETDATA_FLUSH`) **before** publishing the slot, so the
write is GPU-complete before the present device can read it. The wait is on the capture thread
(off the present path) → a near-constant pipeline offset, not present jitter. Validation:
re-run 60→60; success = the roughness detector reports **no periodic episodes** and `pdt` stays
pinned. If roughness persists, coherency is eliminated and the "older-pick" timing pattern is
the next lead.

### Why pursue B over the cleaner-scoring A

A scored better here, but its single thread is a structural ceiling: vsync-blocking present or
heavy NVOFA flow compute on the present path would **starve capture** (a single frame's work
exceeding the present period = a stutter — the exact failure mode to avoid). B's independent
capture thread keeps filling the ring regardless of present-side work, so interpolation/vsync
cost becomes a *constant pipeline offset* rather than per-frame jitter. B is the better
foundation for blend → NVOFA; the coherency fix is the price of its two-device split.

### Latency policy (clarified)

Constant pipeline offset (uniformly older presented content) is acceptable without limit, in
service of pacing. Variable per-frame latency — any single frame's work exceeding the present
period — is *not*; that is what a stutter is. Every timing decision is judged by pacing
uniformity (steady `pdt`, no roughness episodes), never by absolute latency.

## B sync fix — validated (2026-06-13, 60→60)

The capture-thread flush before publish (`temporal-b-two-devices` + sync commit) **fixed the
stale-pixel reads**: `detect.py reverse` went **177 → 0** reversals. The cross-device coherency
diagnosis is confirmed and resolved.

**Performance (the gating question): capture is light.** `flush=` median **247 µs**, p95 287 µs,
p99 354 µs, max 743 µs — sub-millisecond and consistent. Capture `dt` held 16661 µs median (no
stalls). So the flush costs ~1.5% of the capture thread's budget at 60 fps (~6% at a 240 fps
source), off the present path = constant pipeline offset, not present jitter. Nothing here is
heavy before blend/NVOFA; the expensive future work (flow compute) lives on the present thread,
decoupled.

**Residual = inherent nearest-pick drift slip, not a bug.** 86 dupes/5min, classified by
periodicity: **81 in ~25 s-periodic clusters (drift slips)** + **5 isolated (environmental)**.
The drift sweep is the source-vs-present clock difference (~0.07%); vsync baseline shows ~0–27
because it is single-clock (can't drift-slip). The *fundamental* floor for a decoupled mode is
~12/5min (one rate-matching correction per ~25 s sweep) — so the ~69 excess over that floor is
selection-policy wobble, not fundamental (see always-future below).

This is the **success criterion for the temporal stage**: zero reversals, zero stale reads,
sub-ms capture cost, residual = the inherent rate-matching artifact the next stages address.

### Decision: adopt B (two devices) over A (single thread)

A scored cleaner here (159 vs 86 dupes is misleading — A's were also drift slips) but A's single
thread is a structural ceiling: vsync-blocking present or heavy NVOFA flow compute on the present
path would **starve capture** (a single frame's work exceeding the present period = a stutter).
B's independent capture thread keeps filling the ring regardless of present-side cost, so
interpolation/vsync becomes a constant pipeline offset, not per-frame jitter. B is the foundation
for blend → NVOFA; the (now-fixed) coherency flush is the price of its two-device split.

### Always-future selection (experiment, in progress)

Nearest-pick holds the same frame for ~6 presents at each slip (81 dupes vs the ~12 floor),
likely boundary wobble. Experiment: always pick the first frame **at/after** the target (stable,
monotonic) instead of the nearest. Expected to collapse each slip to a single clean dupe (~81 →
~17, near baseline). Cannot reach true zero — two drifting clocks force ~12 corrections/5min
regardless of policy; only frame *generation* removes those. Likely the default selection policy
for streaming use going forward; hysteresis is a fancier alternative if needed.

## Round 4 (2026-06-14) — hysteresis, always-future, 240→60, and the present-clock question

New `detect.py` instrument added: **`reverse`** subcommand — per-frame scroll-direction via
column-profile cross-correlation. On directional-motion clips a backward scroll = a stale /
out-of-order frame; catches what dupe and roughness detection miss. (Also added: the
`pacing` "Roughness" metric in round 3.)

Selection-policy experiments on the B-sync build (all 60→60 unless noted; reversals 0 / capture
clean throughout):

| Build | dupes/5min | roughness | note |
|---|---|---|---|
| always-future @60 | 66 | ~7 episodes, ~50s period | each slip = ~1s cluster of single dupes (boundary wobble) |
| always-future @240 | 1 | 9 episodes, ~34s periodic (GOOD) | no dupes (4× oversample); mild period-2 motion judder |
| **plain vsync @240** | 1 | ~0 (EXCELLENT) | control — source is uniform; vsync downsamples 240→60 cleanly |
| **t:60 = nearest + hysteresis @60** | **4** | 4 single blips, 0s total (EXCELLENT) | 6 `pick=repeat` logged = the floor |
| t:vsync = hysteresis + vblank @60 | 7 | 6 episodes, 2s, ~50s periodic (EXCELLENT) | vsync present did NOT help at 60 |

**Hysteresis is the 60→60 fix — confirmed:** nearest *without* hysteresis was 86 dupes; nearest
*with* hysteresis is **4** (EXCELLENT, roughness ~0, 6 logged repeats = the fundamental floor).
The improvement is specifically the monotonic constraint.

**Why it's principled, not a patch — it's asynchronous sample-rate conversion (ASRC).** Resampling
a source onto a drifting present timebase is the same problem audio resamplers solve across
44.1↔48 kHz clocks. The textbook-correct ASRC is a **phase accumulator** (hold a fractional
source position, advance by `source_rate/present_rate` each output, pick/interpolate there) —
which is **inherently monotonic by construction**. Our nearest-timestamp pick was a *memoryless*
approximation that dropped that monotonicity; "nearest" is genuinely ill-posed at a tie. The
mechanism is quantified: at 60→60 the phase creeps ~5.6µs/present (near-equal rates), so for ~1s
per drift sweep the target sits within the **319µs capture-jitter band** of the midpoint between
two frames, and 319µs ≫ 5.6µs means jitter flip-flops the memoryless pick → predicted flip-flop
window 319/5.6 ≈ 57 presents ≈ **1.0s, matching the observed ~1s clusters exactly.** Hysteresis
*restores* the monotonicity proper ASRC always has (and for an unknown/variable source rate,
timestamp-nearest + monotonic-constraint *is* the variable-rate phase accumulator). It is a
**no-op at 240→60** (picks already advance), so it doesn't confound the 240 tests — and indeed
t:vsync@60 (which adds vblank present on top) was no better, slightly worse, since at 60→60 the
residual is rate-matching that *selection* fixes, not present timing.

### Architecture status (what's solid vs open)

- **Capture / ring population: solid and locked.** Clean capture `dt`, accurate arrival
  timestamps, 0 reversals everywhere (cross-device coherency fixed by the flush), uniform
  bracketing at both rates. The hard, concurrency-and-cross-device-heavy layer is done; the ring
  is now a stable interface ("timestamped, bracketable, coherent frames").
- **Present side: where the remaining work lives — and that's for the best.** Selection policy,
  present timing, and (next) blend/NVOFA are all single-threaded, deterministic consumers of the
  ring interface. 60→60 is solved by hysteresis (selection). The one open item is the 240→60
  micro-judder: confirmed *not* capture, *not* selection-timestamps, *not* present-timing-by-pdt
  — yet the video micro-judders period-2. Unexplained; the t:60-vs-t:vsync @240 discriminator
  localizes it (present clock vs the StretchRect/display path).

**The 240→60 judder — present clock vs selection (open):** vsync@240 is clean, so the source is
uniform and 240→60 *can* be downsampled cleanly; the judder is in our temporal pipeline. But it
is **not** simply "QPC-present-vs-vblank drift," because plain `60` (TimerCaptureMode: latest +
QPC present) was smooth — QPC present is fine with show-latest. The two clean modes (plain 60,
vsync@240) both show *latest*; the juddery one (temporal@240) does *target-based ring selection*.
So the judder correlates with target-selection, and its mechanism is not yet confirmed
(measurements show uniform selection timestamps AND uniform present timing, yet the video
micro-judders period-2 — unexplained). **`t:vsync` is the discriminator:** it keeps target
selection but moves present to the vblank. Clean → present clock was it; still juddery → the
ring/selection path is, independent of present clock. Test pending.

## Round 5 (2026-06-14) — vsync present targets the WRONG display (bug)

**Finding:** with the source monitor at 240Hz and the capture-card/target at 60Hz, `t:vsync`
**still presents at 240/s** (log `pdt` ≈ 4166µs) — confirmed on a fresh capture with the capture
card verified at 60Hz. Root cause: the present device is created on **`source.dxAdapterIndex`**
(`NvFBCR.cpp:585`), so D3D9 `Present(INTERVAL_ONE)` blocks on the *source* display's vblank, not
the target's. (Windowed DWM present compounds it — DWM composites at the highest-refresh attached
display, also the 240Hz source.) The window being on the target display doesn't change which
adapter's vblank the device syncs to.

**Plain `VsyncCaptureMode` has the same bug** — same device (source adapter) + `INTERVAL_ONE`.
Masked because normal streaming is source==target rate (both 60Hz), where syncing to the source
60Hz coincidentally yields a correct 60Hz present. Only surfaces when source ≠ target. NOTE:
plain vsync has been the user's smoothest result for a long time at 60→60 — the fix must be
**A/B'd against it and must not regress it**.

**Consequence:** the `t:vsync` 240→60 discriminator is still **unanswered** — vsync forces the
240Hz source rate, so select-from-240/present-at-60 was never actually tested.

**Fix — IMPLEMENTED 2026-06-14, see Round 6 below.** Replace `INTERVAL_ONE` with an explicit
`IDXGIOutput::WaitForVBlank` on the TARGET output (located by HMONITOR), then `Present(IMMEDIATE)`.
Locks present to the *named* display regardless of device adapter / DWM.

## 240-content-on-60Hz caveat — and its correction (2026-06-14 recapture)

**Original caveat (the Round-4-era "60 source" clips):** those were **240fps content on a 60Hz
monitor**, not a true 60fps source. Motion was equivalent (median MAD 13.63) but NvFBC partly woke
on the 240fps content → content-duplicate frames seeded in the ring, polluting dupe counts. Use a
true 60fps source (60Hz monitor + 60fps content), or diffmap dedup (which B lacks), for clean
60→60 dupe counts.

**Correction — the 2026-06-14 recaptured clips ARE true 60fps.** Verified by capture-`dt`
histogram, not by count alone: **86.9%** of capture gaps sit at 14–19ms (the 60Hz period), only
~11% are sub-2ms. (A 240-content file looks completely different: 96.5% at 2–6ms — that's the
240 `t:60` clip.) So the recaptured `t:60@60`/`t:vsync@60` are genuine 60→60. The ~74 captures/s
(vs an ideal 60) is the **HW-cursor-wake tail** (`bWithHWCursor=1`, sub-2ms wakes), present
equally in both 60-source files — *not* content over-wake. **Lesson: confirm source rate by the
`dt` histogram (where the mass sits), not by captures/s or dupe count.**

Residual puzzle on the true-60 clips: `t:60@60` still logged **61 dupes** (only 6 genuine
`pick=repeat`; the rest bursty/clustered with 18 roughness episodes), while `t:vsync@60` logged a
clean **6** (evenly ~50s apart = the drift floor). And `t:60@60` swung 4 (Round 4) → 61 (this
run). So the timer-present path looks **run-condition-sensitive** at 60→60 (QPC-vs-display drift
phase) where vsync-present is stable — but these were separate captures, not a same-session A/B,
so it is not yet a clean verdict.

## Round 6 (2026-06-14) — full t:60/t:vsync re-analysis (and a failed WaitForVBlank experiment)

### Full re-analysis of the recaptured clips (all `reverse` = 0)

| Run | dupes (MAD) | pick=repeat | capture dt median | present pdt median | pacing |
|---|---|---|---|---|---|
| t:60 @60 | 61 (bursty) | 6 | 16662µs ✓60Hz | 16616µs ✓60Hz | EXCELLENT (P2/P3 = 0%) |
| t:60 @240 (real 240→60) | **0** | 0 | 4176µs ✓240Hz | 16641µs ✓60Hz | GOOD — P2 2%, P3 3%, 16 rough windows / 6 spots |
| t:vsync @60 | 6 (clean ~50s) | 2 | 16663µs ✓60Hz | 16673µs ✓60Hz | EXCELLENT |

Both `t:60` and `t:vsync` work at 60→60. `t:60@240` is clean on dupes (always a fresh frame) but
carries the open **mild period-2 micro-judder** (clustered, ~8% of windows). See the
"240-content-on-60Hz caveat — and its correction" section for why `t:60@60`'s 61 (vs `t:vsync`'s
6) is a run-sensitivity artifact, not content pollution, and the histogram proof the sources are
true 60.

### Failed experiment — `VBlankWaiter` (reverted)

Tried a `VBlankWaiter` helper (DXGI `IDXGIOutput::WaitForVBlank` on the target output, matched by
HMONITOR) to pace the present on the *target* vblank: `Wait()` then `PresentEx(IMMEDIATE)`, device
created `IMMEDIATE`, applied to both plain `vsync` and `t:vsync`. It targeted the right display but
**regressed pacing** (decoupled present → DWM beat). **Reverted in Round 7**, which keeps
`INTERVAL_ONE` and instead moves the present *device* to the target adapter. Recorded here only as
the path not taken.

## Round 7 (2026-06-15) — WaitForVBlank regressed pacing; pivot to present-on-target-adapter

`VBlankWaiter` correctly targeted the capture card (`bound to target output (2560,-1080)-(4480,0)`,
pdt 16.6ms = 60Hz), **but it regressed pacing** — and the same-session A/B proved it:

| 60→60 | OLD `INTERVAL_ONE` (source) | NEW WaitForVBlank (target) |
|---|---|---|
| plain vsync | 6 dupes, std 0.496, 100% smooth | **102 dupes, std 1.154**, ~25s periodic defect |
| t:vsync | 6 dupes, std 0.496 | **38 dupes, std 0.777** |

**Root cause — decoupling.** `INTERVAL_ONE`'s present blocks until the frame is consumed at vblank;
that backpressure phase-locks the loop. `Wait()` → `PresentEx(IMMEDIATE)` decouples them — the
immediate present is picked up by DWM at a variable time → the two clocks beat → periodic defect.

**Deeper insight — the 6-dupe cleanliness came from presenting on the *source* clock, not from
INTERVAL_ONE.** Old t:vsync/plain-vsync presented on the *source* vblank (the Round-5 bug), so at
60→60 present-clock == source-clock → no drift → selection degenerated to clean 1:1 passthrough (6).
The moment you present on the *target* clock (the goal), source-vs-target drift returns: new t:vsync
(same selection code, present on target) got **38, not 6**, and its picks flipped before-adv→after-adv.
So **the matched-rate dupes are a selection-vs-drift problem inherent to targeting the card's clock —
they are NOT fixed by the present mechanism.** (At 240→60 this is moot: t:60@240 = 0 dupes, always a
fresh frame. The dupe issue is matched-rate-only.)

**Decision: drop `VBlankWaiter`; present the temporal mode on the TARGET adapter with `INTERVAL_ONE`.**
This keeps INTERVAL_ONE's clean backpressure-locked present *and* lands it on the capture-card vblank —
the best of both. Only the temporal mode can do this (its NvFBC is rebound to the ring's own capture
device, so the present device is free to move); plain vsync stays single-device on source (reverted to
`INTERVAL_ONE`, correct at matched rate, which is its only job).

Implementation:
- `IFrameCaptureMode::PresentsOnTargetAdapter()` (default false; `FrameTemporalCaptureMode` → true).
- `NvFBCR.cpp`: present device created on `target.dxAdapterIndex` for temporal, else source; expose
  `g_sourceAdapterIndex`.
- `CaptureRing`: capture device pinned to `g_sourceAdapterIndex` (was: inherited the present device's
  adapter). Shared ring slots are created on the source-ordinal device and opened on the target-ordinal
  present device — fine because both ordinals are the **same physical GPU** (D3D9Ex has no cross-GPU
  sharing, but cross-ordinal-same-GPU works).
- `FrameTemporalCaptureMode` vsync path reverts to `INTERVAL_ONE` (no change vs pre-VBlankWaiter); the
  fix is purely *where the present device lives*.

Open risks to validate on the rig:
- **(a)** WinMain still creates its initial NvFBC session on the present device (now target-ordinal)
  before `CaptureRing.Start` rebinds it to the source capture device. Same GPU, released before any
  grab — *should* create fine; if NvFBC create fails on the target ordinal, relocate that session.
- **(b)** Cross-ordinal-same-GPU shared-handle open (capture slot on source device → present device).
- **(c)** **The gating question:** does windowed `INTERVAL_ONE` on a target-adapter device actually
  pace at the target 60Hz, or does DWM still composite at the source 240Hz? Only a **240→60** capture
  answers it. If DWM wins, the adapter-move doesn't fix the rate and we reconsider.

Validation: **60→60** must match the old EXCELLENT (std ~0.5) — though note matched-rate dupes won't
drop to 6 (that needs the source clock, i.e. passthrough); the win is correct rate + clean pacing.
Then **240→60**: pdt should read 60Hz, and compare the period-2 judder against `t:60@240`.

## Mode framework: `<selection>:<present_timing>`

The mode string is two orthogonal axes (already reflected in `t:60` vs `t:vsync` parsing):

- **selection** — how the output frame is chosen/built: `t` temporal (nearest/future-pick),
  later `b` blend (pixel cross-fade of the bracket — *not* motion interpolation), `o`
  optical-flow / NVOFA (true motion-compensated in-between frame).
- **present_timing** — when to present: `60` (absolute-QPC timer, current), `vsync` (block on
  the target/capture-card vblank), later arbitrary CFR.

**`t:vsync` (current mechanism — Round 7):** temporal selection with vsync-locked present. The
`FrameTemporalCaptureMode` takes a `vsyncPresent` flag: `GetPresentationInterval()` returns
`INTERVAL_ONE`, and the **present device is created on the TARGET adapter**
(`PresentsOnTargetAdapter()`), so the `INTERVAL_ONE` present blocks on the *capture-card* vblank
(not the source's). The present loop skips the QPC-timer wait and lets that blocking present be the
pacing wait, anchoring the selection target to "now" (just after the vblank) so selection runs on
the target's display clock. The capture thread/ring/bracketing/hysteresis are unchanged; NvFBC stays
on the ring's source-adapter capture device. `t` and `t:vsync` route here; `t:59.94` routes to the
same mode with timer present. (The `VBlankWaiter` WaitForVBlank approach from Round 6 was reverted —
it regressed pacing; see Round 7. The old `VsyncTemporalCaptureMode` was removed earlier.)

**Clarification — blend ≠ interpolation.** Blend is pixel averaging/cross-fade of the bracketing
pair (softens a slip into a brief ghost/double-image); it does not synthesize motion. NVOFA is
the only stage that produces a geometrically-correct in-between frame (true smooth motion).
Blend's role is the POC that validates the bracket/weight pipeline NVOFA later plugs into.

## Future work

- **Blend mode** (next): identical capture/scheduler/ring/bracket; replace nearest-pick with a
  weighted blend of the pair (its own spec).
- **Optical flow (NVOFA)**: replace the blend with motion-compensated interpolation. NVOFA is a
  CUDA hardware path (flow vectors + a warp kernel), not a D3D9 shader swap — see the archived
  `garrett-nvofa-optical-flow-interpolation` branch, deliberately set aside as too complex until
  this foundation is solid.
- **VSync-driven present** — DONE (`t:vsync`, see Mode framework). Pending validation that it
  resolves the 240→60 judder (the discriminator test above).
- **Confirm the hysteresis 60→60 floor** — DONE: 4 dupes / EXCELLENT (see Round 4).
- **Resolve the 240→60 judder mechanism** — run the t:vsync@240 discriminator; if still juddery,
  investigate the target-selection path (uniform selection timestamps + uniform present timing
  but period-2 video judder is currently unexplained).
- **Direct-write capture (perf optimization, against the clean t:60 baseline).** Goal: eliminate
  the per-grab capture-side `StretchRect` (one full-res copy/frame — significant at 240fps).
  Change on the two-device B `CaptureRing`:
  - In `Start`: instead of one capture-target surface + `dwNumBuffers = 1`, register **all
    RING_SIZE shared slot surfaces** as NvFBC output buffers — `dwNumBuffers = RING_SIZE`,
    `ppBuffer` = array of `NVFBC_TODX9VID_OUT_BUF` pointing at each slot's capture-device surface.
  - In `CaptureLoop`: set `grabParams->dwBufferIdx = count % RING_SIZE` before each grab so NvFBC
    writes the frame **directly** into the next slot; drop `m_captureTarget` and the `StretchRect`.
  - **Keep the coherency flush, but move it to after the grab** (it currently follows the
    StretchRect): NvFBC's write must still be GPU-complete before the present device reads the
    shared slot, so issue+drain the event query between grab and publish.
  - **Untested wrinkle / main risk:** this points NvFBC's output at a **D3D9Ex shared-handle
    surface**. NvFBC-ToDx9Vid writing into a cross-device shared surface is unverified — it may
    reject it or behave oddly. If so, fallbacks: keep a per-slot non-shared capture target and
    direct-write there then StretchRect (no gain), or revisit. Validate first that NvFBC accepts
    shared surfaces as outputs.
  - **Acceptance:** same result as the t:60 hysteresis baseline (0 reversals, ~6 dupes/5min,
    EXCELLENT) with lower capture-side cost (watch `flush` and capture `dt`).
  - NOTE: the earlier single-device stash does NOT apply — re-implement on the shared-surface ring.

## Out of scope

Upconversion (source < present); HDR/DX11 output; the CUDA FRUC pipeline; multi-target output.
