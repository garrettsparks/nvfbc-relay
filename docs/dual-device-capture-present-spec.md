# Dual-Device Capture & Present — Implementation Spec

Status: **design — to be implemented on a fresh branch off `dev`**.
Companion documents: `temporal-capture-mode-spec.md` (the complete experimental record, Rounds
1–10 — cited throughout as *R1*–*R10*), `d3d-per-display-vsync-research.md` (documented Windows
present/compositor behavior, adversarially verified).

## Purpose

`dev` today has a solid **single-loop** relay: one D3D9Ex device, NvFBC writes the backbuffer,
one thread grabs-and-presents (`vsync` and `60` modes). That design is proven smooth — and
fundamentally cannot do rate conversion, because it has no frame timeline: no timestamps, no
buffer, no way to choose *which* frame belongs at an output instant.

This spec defines the production implementation of the **dual-device, two-thread
capture→ring→present** mechanism: an event-driven capture thread that builds an accurately
timestamped frame timeline, and a present thread that selects from it on either a high-precision
timer (`t:60`) or the display-synced cadence (`t:vsync`). It is the foundation every later stage
(blend, NVOFA, frame-gen pass-through) builds on, and nothing beyond nearest-frame selection is
in scope.

The design below looks almost obvious. It is not. Nearly every component exists in this exact
form because a simpler alternative was tried and **measurably failed**. Section "Why the obvious
implementations fail" is therefore normative: it is the reasoning that forbids the shortcuts a
future refactor will be tempted to take.

## Scope

**Delivered by this phase:**
- `CaptureRing` — private capture device + shared-surface ring + capture thread + batch collapse.
- `PresentScheduler` — reusable absolute-QPC present clock.
- `FrameTemporalCaptureMode` — rewritten on the ring: nearest-pick + hysteresis selection;
  present timing `t:<rate>` (timer) or `t`/`t:vsync` (INTERVAL_ONE).
- `DiagCaptureMode` — `diag` / `diag:vsync` clock probes (kept as permanent tooling).
- Removal of the superseded modes: the old history-2 `FrameTemporalCaptureMode`,
  `FrameBlendCaptureMode`, `VsyncBlendCaptureMode`.
- The validation suite (below) executed and signed off.

**Explicitly out of scope (successor work, do not let it creep in):** interpolation of any kind
(blend/NVOFA); generated-frame tagging or pass-through (Path B); source-rate-aware bracketing
lag (shipped later as the static `-src` lag; see docs/adaptive-bracketing-delay-spec.md);
direct-write (`dwBufferIdx`) capture; upconversion (source < present); desktop-source display
sync (phase-locked timer); HDR/DX11 output.

## Why the obvious implementations fail (normative background)

Each trap below was implemented, measured, and diagnosed during R1–R10. Each yields a **rule**.
A change that violates a rule needs new evidence, not new optimism.

**T1 — Polling grabs have no timeline.** `NVFBC_TODX9VID_NOWAIT` returns the latest frame at
*our* polling instant; `NvFBCFrameGrabInfo` carries **no timestamp and no new-frame flag**
(re-verified against the header — nothing usable). A polled capture cannot reconstruct source
timing, so it cannot bracket, so it cannot rate-convert.
→ **Rule: capture must be event-driven — blocking grab, QPC-stamped at wake.**

**T2 — A blocking grab on a shared device holds the present hostage.** With one
`D3DCREATE_MULTITHREADED` device, the blocking grab holds the device lock for its entire wait
(up to a full source period). Measured: present jitter = capture period / 2 at every rate;
at 100→60 the inter-present gap quantized to the 10/20 ms capture grid (R2, Discovery 1).
→ **Rule: the blocking grab gets its own private D3D9Ex device. Present-side objects are never
touched by the capture thread and vice versa.**

**T3 — Grab timeouts cannot be used for pacing or staleness.** `WAIT_WITH_TIMEOUT` expiry does
**not** return empty: it re-grabs the unchanged screen and returns SUCCESS (R2, Discovery 2).
A 2 ms timeout turned capture into ~500 Hz polling of duplicates with garbage timestamps.
Timing cannot distinguish a just-arrived frame from a stale re-grab.
→ **Rule: the timeout exists only for shutdown/stall detection (100 ms). Never shorten it for
pacing; never infer staleness from timing.**

**T4 — Cross-device shared surfaces have no implicit GPU coherency.** D3D9Ex shared handles
synchronize *existence*, not *writes*. Without an explicit flush the present device reads stale
pixels from freshly written slots — measured as periodic whole-old-frame episodes invisible to
dupe detection (caught by scroll-direction analysis: 177 reversals → 0 after the fix; R3).
→ **Rule: after writing a ring slot, issue a `D3DQUERYTYPE_EVENT` on the capture device and
drain it (`D3DGETDATA_FLUSH`) *before* publishing. The wait lands on the capture thread —
constant pipeline offset, never present jitter.**

**T5 — Relative waits drift.** Sleeping "one period" per frame accumulates wake latency into
rate error. The fix that made the single-loop `60` mode smooth: an **absolute** QPC schedule
(`deadline += period` against a fixed origin; high-resolution waitable timer; catch-up clamp
only when >1 period behind).
→ **Rule: all timer pacing goes through `PresentScheduler` (absolute QPC). No ad-hoc sleeps.**

**T6 — Memoryless nearest-pick flip-flops at boundaries.** At matched rates the target phase
dwells near the midpoint between two frames for ~1 s per drift sweep; capture jitter (~300 µs)
flips the pick back and forth → re-shown frames (86 dupes/5 min measured). This is the
sample-rate-conversion problem; proper ASRC is monotonic by construction.
→ **Rule: selection is nearest-to-target **with hysteresis** — never present a frame older than
the last shown; repeat only when nothing newer exists (the fundamental ~6/5 min floor).**

**T7 — The HW cursor destroys the capture timeline.** `bWithHWCursor=1` wakes the blocking grab
on every cursor *move* — at mouse polling rate (125–1000+ Hz), decoupled from the display.
Measured: hands-off = flat content rate, 0% sub-2 ms gaps; mouse moving = +50–60% sub-2 ms wakes
carrying duplicate content under false timestamps (R9).
→ **Rule: `bWithHWCursor = 0` in the CaptureRing session.** (Plain modes keep `=1`: NOWAIT never
waits, so cursor moves cannot inflate their rate. Note: `=1` is untested in-game — games hide
the OS cursor during gameplay, so it may be wake-free there; test before ever relying on it.)

**T8 — Windowed vsync belongs to DWM, and DWM's clock moves.** Windowed `INTERVAL_ONE` blocks on
DWM's compose clock. On a composed desktop that clock is the **primary/source** display (t:vsync
@240 presented at 240/s — the "wrong display" bug, R5). Under a **fullscreen game** the game
takes the primary via independent flip, DWM composes only the card's display, and the same
INTERVAL_ONE present becomes **card-locked 60 Hz** (unblock at card scanline 0; pdt 16 667 µs,
jit 3 µs — R8 "ANSWERED", R10). Attempts to beat this failed definitively: `WaitForVBlank` as a
present gate paced the loop on a clock that doesn't deliver the flips (two-crystal beat, 6→102
dupes); exclusive fullscreen isn't honored by the capture-card driver (INTERVAL_ONE returns in
~200 µs) and dies on `S_PRESENT_OCCLUDED` the moment the relay loses foreground.
→ **Rule: `t:vsync` = plain windowed `INTERVAL_ONE`, and its correctness is a property of the
gaming use case (game fullscreen on source). Never pace a loop with a clock other than the one
delivering the flips. No exclusive fullscreen, ever, for a background relay.**

**T9 — Frame generation batches wakes and poisons timestamps.** NVIDIA Smooth Motion delivers
**[generated, real]** wake pairs per base frame, ε (<2 ms) apart, batched at the **base** rate
(cap applies post-generation: cap/N = base). NvFBC *does* capture generated-frame content (the
old "doesn't see FG" belief was a latest-wins illusion). Wake order was confirmed
[gen, real] three ways, ending in a definitive A/B: keep-first → ghosting throughout; keep-real
→ crisp throughout (R10). Untreated, the ε-pairs make bracket weights garbage and cause visible
before/after selection dither.
→ **Rule: batch-collapse in the capture thread — a wake <3 ms after the previous one is the REAL
member of a frame-gen pair: publish it stamped with the batch-start time, then RETRACT the
previous slot (the generated frame). 3 ms is safe: real cadences would need a 333+ fps base to
produce such gaps (non-FG 240 Hz gaps are 4.17 ms).**

**T10 — The present thread's ceiling is the reason for two threads.** A single-thread design
(capture pumped between deadlines) scored well at nearest-pick, but any present-side work
exceeding the present period — a blocking vsync present, future NVOFA flow compute — starves
capture. The two-device split converts present-side cost into a constant pipeline offset.
→ **Rule (latency policy): constant pipeline offset is acceptable without limit, in service of
pacing. Variable per-frame latency is a stutter and is not. Judge every timing decision by
pacing uniformity, never by absolute latency.**

## Design principles (distilled)

1. **One-way dataflow.** capture → ring → present. The only shared state is one atomic published
   counter (capture writes, present reads) plus per-slot `valid` flags (capture writes, present
   reads). Present never signals capture; nothing is ever "freed" — ring overwrite is eviction.
2. **Publish-then-never-touch.** A published slot's *pixels* are immutable until the ring wraps
   onto it. The single sanctioned mutation is the retraction flag (`valid=false`), which hides a
   slot without touching its content — safe against a concurrent read by construction.
3. **Event-driven capture, scheduled present.** Capture wakes when the source produces; present
   fires on its own clock (QPC timer, or DWM's via the blocking present). Neither paces the other.
4. **Timestamps are the product.** The ring's value is "coherent frames with honest timestamps."
   Everything that corrupts timestamps (cursor wakes, FG pairs, stale re-grabs) is handled in
   the capture thread so consumers never see it.
5. **Sync to the clock that delivers the flips** (T8). Timer mode's clock is QPC; vsync mode's
   clock is DWM's compose clock via the blocking present. There is no third option windowed.
6. **Logging is the correctness instrument.** The per-frame log lines are part of the design —
   every validation criterion below is checked against them. Logging is enabled only when an
   empty `NvFBCR.log` sits beside the exe; steady-state volume is ~1 line per capture + 1 per
   present; anomaly logs fire on transition only.

## Architecture

### Components

```
                    (source adapter)                                (same adapter)
  NvFBC session ──► private CAPTURE device ──► ring slots ──shared──► PRESENT device ──► DWM ──► card
      ▲                    ▲                  (8 shared RT              ▲
      │   blocking grab    │  StretchRect      textures +               │  StretchRect to backbuffer
      └── capture THREAD ──┴── flush ── stamp ── publish                └── present THREAD (main)
                                                                            PresentScheduler / INTERVAL_ONE
```

- Both devices are created on the **source adapter** (windowed present pacing is DWM's business;
  the adapter move was measured irrelevant — R7). Both get `D3DCREATE_MULTITHREADED`
  (defensive: each device is single-thread-owned by design, so it is *possibly* unnecessary —
  but every validated result was measured with it on. Keep it; removing it is a separate,
  low-priority experiment requiring the full validation suite, not a cleanup.)
- **`CaptureRing`** owns: the capture device, the NvFBC session (rebound from WinMain's), the
  capture-target surface (`dwNumBuffers=1`), the 8 shared ring slots (created on the capture
  device with shared handles, opened on the present device), the event query, the capture
  thread, `Published()`, `HasStopped()`, and `FindBracket()`.
- **`PresentScheduler`** owns: QPC frequency, rounded period, high-resolution waitable timer,
  `Seed() / Deadline() / WaitUntilDeadline() / Advance() / PeriodQpc() / Freq()`. Pure "when",
  never "what". `TimerCaptureMode` adopts it too (removing its inline copy) so all timer pacing
  is one code path.
- **`FrameTemporalCaptureMode`** owns: the mode loop, selection policy, logging. Composes the
  other two.

### Capture thread procedure (per wake)

1. Blocking grab: `NVFBC_TODX9VID_WAIT_WITH_TIMEOUT`, `dwWaitTime = 100 ms` (T3). On timeout
   status: loop (re-check stop). On `INVALIDATED_SESSION`: set stop, exit.
2. `QueryPerformanceCounter` → `now` = this frame's arrival timestamp (T1).
3. **Batch decision** (T9): `intraBatch = (now − lastArrival < 3 ms)`. If not intra-batch,
   `batchStart = now`. Update `lastArrival` unconditionally (a 3rd ε-member chains correctly).
4. `StretchRect` capture-target → `ring[writeCount % 8]` on the capture device.
5. **Coherency flush** (T4): issue event query, drain with `D3DGETDATA_FLUSH` (log the blocked
   time as `flush=` — expected ~130–350 µs, near-constant).
6. Stamp the slot: `timestamp = intraBatch ? batchStart : now` (base-cadence timeline under FG);
   `valid = true`.
7. Publish: `writeCount++`, then the atomic store. **Order matters**: the write is complete and
   GPU-coherent before the index moves.
8. **Retraction** (T9): if `intraBatch`, set `ring[(count−1) % 8].valid = false` — the previous
   member was the generated frame. (Disclosure: a present that started reading the gen slot in
   the ~ε window still shows it once — rare, content-coherent, benign. And because the retracted
   twin shares the real frame's timestamp, hysteresis will show that base period via the gen
   frame rather than double-presenting — self-correcting by the next batch.)
9. Log: `capture #<n> arr=<µs> dt=<µs> flush=<µs> col=<cumulative retractions/skips>`.

### FindBracket contract

Inputs: `targetQpc`. Scans published slots `[p−7, p−1]` skipping `!valid`; returns
nearest-before (+ its ring depth) and nearest-after (+ diffs) and the blend weight
`beforeDiff/(beforeDiff+afterDiff)` a future interpolating consumer will use. Borrowed
present-device aliases — never Released by the caller.

### Present thread loop (per output frame)

1. **Pace** — timer variant: `WaitUntilDeadline()`, `deadline = Deadline()`. Vsync variant: no
   wait here; the blocking present *is* the pace, and `deadline = now` (anchors selection to the
   display clock, T8).
2. `target = deadline − bracketingDelay` where `bracketingDelay = one present period` — lag the
   content target so a frame *newer* than it exists (bracketing). Known limitation: this assumes
   source ≥ present rate; at lower base rates (e.g. 30-base FG) the after-frame often doesn't
   exist and selection degrades gracefully to repeats. The successor fix (a **blend**
   prerequisite, not a nearest-pick one) shipped as the static `-src`-declared lag; a
   continuously-adaptive lag was implemented first, measured, and rejected for wandering
   output latency — see docs/adaptive-bracketing-delay-spec.md History before revisiting.
3. **Select with hysteresis** (T6): among bracket frames *newer than lastShownTs*, pick the
   nearest to target; if both are older, repeat the last frame (`pick=repeat`). Track
   `lastShownTs` monotonically.
4. `StretchRect` chosen slot alias → backbuffer; `PresentEx` (`INTERVAL_IMMEDIATE` for timer,
   `INTERVAL_ONE` for vsync).
5. Timer variant: `Advance()`.
6. Log: `temporal dl= tgt= before=(d<depth>) after= w= pick= jit= pdt=`; loud `LOGERR` if the
   target ever falls off the back of the ring after warm-up.
7. Pump messages; exit on `WM_QUIT` or `HasStopped()`.

### Ring sizing and safety (no locks)

8 slots. Present touches only the newest few (the bracket sits near the lagged target); capture
cannot wrap onto a slot being read within one µs-scale StretchRect at ≥3 slots of separation.
Under FG the retractions halve the *valid* population — 8 slots = 4 valid base frames ≈ 133 ms
at 30-base, still ≫ the 16.7 ms lag. Depth is logged (`d<n>`) so the margin is observable in
every run.

### Mode framework after this phase

**Four capture modes ship: `vsync`, `60`, `t:vsync`, `t:60`** (the numeric forms are generic —
`59.94` / `t:59.94` parse for free). Everything else that exists on `dev` today and does not
actually work is removed: the current `FrameTemporalCaptureMode` (vestigial history-2/NOWAIT
implementation — functionally a worse TimerCaptureMode, see
`temporal-blend-future-frame-findings.md`) and both blend modes.

| mode | present timing | device path | status |
|---|---|---|---|
| `vsync` | INTERVAL_ONE, single device | NvFBC → backbuffer direct | untouched (gold standard) |
| `60` / `<rate>` | absolute-QPC timer, single device | NvFBC → backbuffer direct | untouched; adopts `PresentScheduler` |
| `t:<rate>` | absolute-QPC timer | dual-device ring | this spec |
| `t` / `t:vsync` | INTERVAL_ONE (DWM/card under game) | dual-device ring | this spec |
| `diag` / `diag:vsync` | probe modes | own raster device on target adapter | kept (tooling) |
| `b`, `b:vsync`, `b:<rate>` | — | — | **removed** (pre-ring implementations, superseded; ring-based blend is future work with its own spec) |

## Code changes from the `dev` baseline

*(Descriptive — implementation follows this spec. **Reference implementation:
`temporal-b-two-devices` @ `2d943e5`** — consult it; with the trial-and-error done the port is
expected to be a small, clean diff. Files to port and their state at that commit:*

| file @ `2d943e5` | port guidance |
|---|---|
| `CaptureRing.{h,cpp}` | Port; **hardcode keep-second retraction** (the commit still has the 3-way `g_collapsePolicy` — the toggle was agreed removed but never committed; drop the extern/branches, keep the retraction path + `col=` counter). |
| `PresentScheduler.{h,cpp}` | Port as-is. |
| `FrameTemporalCaptureMode.{h,cpp}` | Port as-is (hysteresis selection, `vsyncPresent` flag, logging). Fix any stale "capture-card vblank" comments to say DWM/compose clock (T8). |
| `DiagCaptureMode.{h,cpp}` | Port as-is (`g_targetAdapterIndex` global comes with it). |
| `NvFBCR.cpp` | Do NOT port wholesale — apply this spec's item 6 to the dev baseline (routing, MULTITHREADED, remove blends, remove `-collapse` arg + console prompt). |

*Key history commits if archaeology is needed: `4e86a43` (dual-thread ring + select), `fe5a199`
(coherency flush), `6d3ddf6` (hysteresis), `5ab190b`/`f494511` (cursor), `16b9bf5` (batch
collapse), `9fc8eb4` (policy toggle — superseded), `2d943e5` ([generated, real] docs). The port
must follow THIS spec where they differ.)*

1. **New `PresentScheduler.{h,cpp}`** — extract the absolute-QPC logic currently inlined in
   `TimerCaptureMode` (period rounding `round(freq/rate)`, `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`,
   seed/deadline/wait/advance, >1-period catch-up clamp). Non-copyable.
2. **`TimerCaptureMode`** — swap inline scheduling for `PresentScheduler`. Behavior-identical
   (validated by the plain-`60` regression run).
3. **New `CaptureRing.{h,cpp}`** — as specified above: private device, shared ring, event-query
   flush, blocking grab, batch-collapse keep-real with retraction (hardcoded — no policy toggle;
   future gen-frame work writes new code anyway), `bWithHWCursor=0`, `FindBracket`,
   `Published()/HasStopped()/Stop()`. `Start()` releases the WinMain NvFBC session and rebinds
   to the capture device (updates the global so Cleanup releases the right one).
4. **Rewrite `FrameTemporalCaptureMode.{h,cpp}`** — delete the history-2 single-device
   implementation wholesale; new implementation = ring + scheduler + hysteresis selection +
   `vsyncPresent` flag as specified.
5. **New `DiagCaptureMode.{h,cpp}`** — port as-is (QPC/IMMEDIATE and INTERVAL_ONE variants;
   DWM_TIMING_INFO + GetRasterStatus on a private target-adapter device; `dwmapi.lib` pragma).
6. **`NvFBCR.cpp`** — add `D3DCREATE_MULTITHREADED` to `InitD3D9`; add `g_targetAdapterIndex`
   (diag); routing: `t`/`t:vsync`/`t:<rate>`/`diag`/`diag:vsync`; **remove** blend includes,
   routing, and help lines; update help/console text; plain modes keep `bWithHWCursor=1`.
7. **Delete** `FrameBlendCaptureMode.{h,cpp}`, `VsyncBlendCaptureMode.{h,cpp}`; vcxproj updated
   (mind CRLF — edit entries, don't rewrite the file).
8. **Logging** — exactly the two line formats above (they are load-bearing for validation), plus
   one-time setup lines (mode, ring init, collapse note). No logging inside any wait/poll loop.
9. **Divergence audit (final step before building):** diff the ported files against
   `temporal-b-two-devices@2d943e5`. Every difference must be one of: (a) mandated by this spec
   (toggle removal, comment fixes, blend deletion), or (b) deliberately justified in the commit
   message. Any unexplained difference is a port bug until proven otherwise — the reference
   branch is the validated implementation; this spec only sanctions specific departures from it.

## Validation suite (sign-off gates)

Methodology: UFO test or KCD2 as noted; OBS CBR ~6 Mbps (verify **0 dropped frames**); empty
`NvFBCR.log` beside the exe; ~3–5 min runs unless noted; same-session baseline comparisons where
specified; analysis via `detect.py` (`reverse`, `dupes --mad --max-dupes 0`, `pacing` incl.
Roughness) + log assertions.

| # | run | config | pass criteria |
|---|---|---|---|
| V1 | plain `vsync` 60→60 | UFO, source monitor 60 Hz | **no regression vs dev**: ≈0–30 dupes, std(Δ) ≤ 0.7, EXCELLENT, reverse 0. Gold standard must be untouched. |
| V2 | plain `60` 60→60 | same session as V1 | matches V1-class results — proves the `PresentScheduler` extraction is faithful. |
| V3 | `t:60` @240, no FG | UFO, source 240 Hz fixed (desktop is fine — timer mode is DWM-independent) | reverse 0; 0 dupes; pacing GOOD+ (known, accepted period-2 ≤ ~3% — quantization, interpolation's job); capture dt ≈ 4.17 ms clean; `col=0`; pdt ≈ 16 667 µs; no ring misses. |
| V4 | `t:vsync` in-game + SM FG | KCD2, G-Sync, e.g. 120 cap ×2 (60 base) | **the production config, never before captured with collapse**: pdt ≈ 16 667 µs steady with jit ≤ ~10 µs once in-game; capture batches at base rate with `col` ≈ batch count; crisp reticle throughout swings (no gen frames); reverse 0; no ring misses; regime transitions at game start/quit visible and clean. |
| V5 | `t:60` in-game + SM FG | same session as V4 | crisp throughout (matches the keep-real A/B result); base-cadence ring timeline; healthy picks; dupes explained by 30/60-base repeats only. |
| V6 | cursor sanity | any t-mode run, deliberately wiggle mouse throughout | sub-2 ms capture gaps ≈ 0% (cursor fix holds under input). Can be folded into V3. |
| V7 | 60→60 `t:60` + `t:vsync` | UFO, source 60 Hz | hysteresis floor: ~4–10 dupes, EXCELLENT, reverse 0 (regression net for the selection layer). |

Log assertions common to all t-mode runs: correct mode/init lines; `flush` p95 < 500 µs and
stable; no `LOGERR` besides expected startup notes; `pick=repeat` consistent with the
source/present ratio; bracket depth `d` comfortably inside the ring.

**Sign-off:** all seven pass → tag the branch as the new baseline. Any failure → fix on this
branch before *any* successor work (direct-write, blend) begins, because every successor's
acceptance test is "matches this baseline."

## Rollback stance

`vsync` and `60` — the modes in daily use — are **byte-untouched by this phase except for the
`PresentScheduler` extraction in `60`** (validated by V1/V2 before anything else lands). Worst
case at any point: stop using the `t:` modes; the relay is exactly as good as `dev` today. There
is no migration, no config, no shared state between the old and new paths beyond
`InitD3D9`'s added `D3DCREATE_MULTITHREADED` flag (covered by V1). If V1 or V2 regress, the
branch does not merge — full stop.

## Test-artifact naming convention

`<source>_<fg>_<mode>[_<variant>]_<content>.{mp4,log}` where:
- `<source>` = source rate/cap (e.g. `240`, `90`, `60cap`)
- `<fg>` = `x1` (FG off) or `x2`/`x3` (Smooth Motion multiplier)
- `<mode>` = `vsync` / `60` / `t_60` / `t_vsync` / `diag` / `diag_vsync`
- `<variant>` = optional experiment tag (e.g. `directwrite`)
- `<content>` = `ufo` or game tag (e.g. `kcd`)

Examples: `240_x1_t_60_ufo.mp4`, `120cap_x2_t_vsync_kcd.log`, `60_x1_vsync_ufo.mp4`.
The validation table's runs map to: V1 `60_x1_vsync_ufo`, V2 `60_x1_60_ufo`, V3
`240_x1_t_60_ufo`, V4 `120cap_x2_t_vsync_kcd`, V5 `120cap_x2_t_60_kcd`, V7
`60_x1_t_60_ufo` + `60_x1_t_vsync_ufo`.

## Successor work (ordered, post-sign-off)

1. **Direct-write** (`dwNumBuffers=RING_SIZE`, `dwBufferIdx`, drop the capture-side StretchRect,
   flush moves after the grab). Either-works-or-doesn't risk: NvFBC may reject shared-handle
   surfaces as outputs (loud, at setup) or corrupt subtly (caught by `reverse`/Roughness).
   Acceptance = V3/V4 numbers unchanged with lower `flush`+copy cost. Light perf check only.
2. **Adaptive bracketing delay** (blend prerequisite — lag ≥ source period).
3. **Blend** (own spec), then **NVOFA**.
4. **Gen-frame utilization (Path B)** — the headline use case: play at 90 base × 2 = 180
   displayed, capture a clean 180/3 = 60 with generated frames *included* and re-stamped
   (member *i* of *k* at `batchStart + i·(P̂/k)`; multiplier = batch-size mode). Requires
   tagging + timestamp reconstruction — new code, deliberately not pre-built into this baseline.
5. **In-game DLSS-FG characterization** (wake pattern may differ from Smooth Motion — do not
   assume the 3 ms/pair model transfers).

## Risk register → detection instrument

| regression class | instrument |
|---|---|
| stale/out-of-order pixels (coherency) | `detect.py reverse` (scroll-direction) |
| re-shown frames (selection/pacing) | `dupes --mad`, `pick=repeat` vs dupes reconciliation |
| wrong-time frames, periodic beats | `pacing` Roughness (periodicity warning) |
| capture timeline corruption | capture `dt` histogram (sub-2 ms %, cadence buckets) |
| present cadence drift/jitter | `pdt` median/p5/p95, `jit` |
| FG mishandling | `col` counter + reticle crispness frame-step |
| silent present failure | per-frame `PresentEx` HRESULT + `CheckDeviceState`, logged on transition (add to any risky present path — this caught the FS occlusion) |

## Glossary (log fields)

`arr` capture arrival (µs, rel. base QPC) · `dt` inter-arrival gap · `flush` GPU-coherency wait ·
`col` cumulative collapsed (retracted) FG members · `dl` present deadline · `tgt` selection
target (= dl − lag) · `before/after` bracket timestamps (`d<n>` = ring depth of before) · `w`
would-be blend weight · `pick` selection outcome (before/after/±adv/repeat) · `jit` deadline→
present-call latency · `pdt` inter-present gap.

## Previous conversations

* claude --resume 701f75e0-aac1-48dd-9de5-5e623b0ce24f
