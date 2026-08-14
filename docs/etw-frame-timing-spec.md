# ETW Frame-Timing Spec (reading the driver's real scanout times)

Status: **THE JOIN IS BUILT AND VALIDATED, DIAGNOSTIC ONLY.** The relay places every bracket
frame on the driver's flip grid once per present and logs the verdict; nothing branches on it and
`-etw` off is byte-identical to the pre-ETW build. Measured live: 98.5% of presents placed at 60x2
and 97.9% at 60x3, anchor offset p50 +69 us, and no measurable pacing cost (present jitter p50 4 us
against a 3 us baseline). The flip timestamps are corroborated by a SECOND, independent provider -
see "What the scanout grid actually looks like".

Stage 6 is BUILT (2026-08-10) as DELIVERY-LATENESS CORRECTION behind `-dejit`, replay-validated
against the corpus, awaiting a live capture. It is deliberately NOT the flip-stamping this
document originally specified - see "Stage 6 as built" for why the literal design measured
3-4x worse in replay and what shipped instead. What remains unbuilt is stage 7 (selection
preferring real frames), whose gating question is untouched: whether a generated frame's
CONTENT phase matches its DISPLAY phase. The real-vs-generated label is CLOSED for ETW (the
2026-08-10 multiplier sweep killed both DxgKrnl candidates at the mechanism level, see
"Settled"); at x2 batch position remains the label, and at x3 the rotation phase is readable
from arrival timing (see open question 3).
Companions: `dxgi-native-pipeline-spec.md`, `nvfbc-capture-pacing.md`, `frame-marker-spec.md`.

## The problem

The relay stamps every captured frame with the time NvFBC woke up. That is submission time, and
under frame generation it is wrong in a way that matters. Measured on this hardware:

| | measured |
|---|---|
| gap between real and generated frame ARRIVING at NvFBC | 0.35 - 0.50 ms |
| gap between those frames APPEARING on screen | 8.33 ms |

The driver submits the pair together and scans them out half a source period apart. So the ring
believes two frames representing motion 8.33 ms apart are 0.4 ms apart. Neither NvFBC nor DXGI
exposes the scanout time, and neither labels a frame real or generated.

Today the relay sidesteps this: batch-collapse keep-real discards the generated frame and stamps
the survivor at batch-start. Correct at 60 -> 60, where only real frames are needed.

## Why it is worth fixing

At 90 fps source -> 60 output, 90 does not divide into 60, so roughly half the output frames are
synthesized by the relay (a lerp of two real frames). But Smooth Motion has already produced 180
frames per second, and 180 / 60 = 3 exactly. Every output frame could land on a real captured
frame with no blending at all, IF the generated frames are stamped at the times they actually
appeared.

The comparison is not gen-vs-real (Round 10 settled that: real wins). It is the relay's linear
cross-fade versus the driver's motion interpolation, for the ~50% of output frames that cannot be
a real frame. The driver already paid for those frames; re-synthesizing worse ones is waste.

**There is a second payoff, and it is already costing frames in the daily-driver config.** The
above is about 90 fps. Measured on two 60 fps captures (~30 KCD map cycles each), a large share of
the blends the relay produces TODAY are compensating for a stamping error rather than for real
motion:

| gaps of 1.3 - 2.5 source periods | capture A | capture B |
|---|---|---|
| followed by catch-up, i.e. the pair sums to 2 periods | 36% | 28% |
| consistent with a genuinely dropped source frame (~3 periods) | 2% | 3% |

A dropped frame leaves a gap of ~N periods and the frames after it stay on the shifted grid. A
frame that WAS rendered on time but handed over late leaves a long gap followed immediately by a
short one, because the next frame arrives near its own correct time. Real drops are ~2%. When a
gap does catch up, the delivery was late by a median of 7650 us, which is 0.46 of a source period:
almost exactly the amount that pushes the target past the srcPeriod/4 passthrough threshold and
forces a blend. So the ring records motion that did not happen, and the relay smoothly interpolates
across it.

**Wake stamping cannot even classify these.** A real frame delivered ~8 ms late and a GENERATED
frame whose delivery slipped past the 3 ms batch-collapse window produce identical wake timelines:
one entry, roughly half a source period off the grid, with no way to tell which it was. Note that
0.46 of a source period is also, to within measurement error, the flip separation between a real
frame and its Smooth Motion twin. Both readings fit the same data. A flip token and a true scanout
time separate them immediately, which is something no amount of arrival-time analysis can do.

This does not change the design below. It changes the priority: the payoff is not confined to a
source rate nobody runs yet, and the first probe trace can settle the ambiguity directly.

## Decisions

**Raw ETW, not PresentMon.** The NVIDIA timing data is a single event with four fields, so the
"large fragile surface" argument does not apply. More decisively, PresentMon COLLAPSES exactly the
data we need: `NVTraceConsumer` dedupes by token, keeps only the latest flip time per head, and
folds the result into one `MsFlipDelay` column on the application's present row. The individual
generated-frame flip times never reach its CSV. Reading the provider directly is the only way to
see them.

**ETW is retained and drives alignment continuously.** Not a one-time calibration that bakes
constants into a hardcoded pattern. A model that assumes "2x, inserted at the midpoint" fails
SILENTLY on x3, on MFG, and on any driver change: it produces confident wrong stamps. Reading
actual scanout times derives the cadence instead of assuming it, including for patterns nobody has
characterized yet.

**Correct timestamps are NOT a licence to treat generated frames as interchangeable with real
ones.** This is the constraint the whole design has to respect, and it is easy to get backwards.

Flip-stamping makes the generated frames usable, but selection must still PREFER the real one.
Measured at 60x2: if both members were kept, flip-stamped, and selection simply took
nearest-to-target, it would land on a generated frame **93.2% of the time**. The targets sit almost
exactly half a source period from every real flip (p50 7937 us, and 0.0% of them within the 4166 us
passthrough threshold), because at a 16.67 ms output period against an 8.33 ms flip grid the lock
phase picks a parity and holds it. Nearest-selection would therefore invert the output from
essentially all-real to almost all-generated, which the keep-first vs keep-real A/B (two full
videos, Round 10) established is the visibly worse picture.

This also explains something that looks like a bug and is not. The ring stamps the surviving real
member at BATCH-START, which is the GENERATED frame's display time, roughly 8.3 ms "early". The
comb lock has settled with targets landing on that same instant. The stamp offset and the lock
phase cancel, so selection reliably picks the real frame. **That 8.3 ms offset is load-bearing**;
removing it without re-phasing the lock by half a comb inverts the output.

So the shape of the win is narrow and specific: a generated frame becomes usable when NO real frame
is near the target. That is rate mismatch (90 -> 60), stall resumes, and ragged delivery. It is not
a general upgrade at 60x2, where the existing behaviour is already correct.

**Enrichment, not dependency.** The existing stamping rule (arrival time, batch-start for
intra-batch members, keep-real) stays as the baseline and never goes away. ETW UPGRADES a slot's
timestamp from estimated to measured when the data arrives in time. Nothing branches on "is ETW
available"; the timestamp is either better or it is not. Dropping the lag floor back to 20 ms must
leave the relay working as it does today.

## Architecture

```
 NvFBC capture ─► ring (pixels + arrival-stamped timestamp)   <- works standalone, as today
 NVIDIA DisplayDriver ETW ─► flip history (capture_seq -> true scanout time)
                               │  upgrade slots not yet bracketed
                               ▼
                     policy sees a measured timeline when available,
                     the estimated one when not
```

**The coherence rule: only upgrade slots that have not been bracketed yet.** If a slot's timestamp
changed after the policy bracketed against it, a frame could move from "before" to "after" under
the policy's feet. Comparing the slot's sequence number against the newest sequence the policy has
consumed closes that.

That one rule is also the entire degradation path. At a 90 ms floor an upgrade landing ~30 ms
after capture arrives long before the target (90 ms back) reaches the slot, so nearly everything
upgrades. At 20 ms the upgrade almost always arrives after the slot was used and is declined. No
separate fallback code exists; the ordering rule produces both behaviours.

For generated frames specifically: with ETW they get a measured stamp and are kept; without, no
stamp arrives in time and keep-real retracts them as today. 120 usable frames/s with, 60 without.

**Sizing.** ETW history is metadata, so size it by TIME, not by ring slots:

```
{capture_seq u64, display_qpc u64, token u32, head u32} = 24 bytes
5 seconds at 180 events/s = ~900 records = ~22 KB
```

Keyed by the monotonic capture counter, never by slot index, so recycling cannot corrupt it.

The ring itself must grow only if the lag floor is raised, because the frames have to survive
until the target reaches them:

| capture rate | frames alive for a 40 ms floor + ~2 source periods | RING_SIZE |
|---|---|---|
| 120/s (60x2) | ~73 ms | >= 9, use 16 |
| 180/s (90x2) | ~73 ms | >= 14, use 16 |

16 slots is ~127 MiB at 1920x1080x4B. The 90 ms floor this table originally assumed needed 32;
measuring delivery lag at 8 ms p50 rather than the ~30 ms guessed made the smaller floor viable.

**The lag floor is a runtime knob, not a design commitment.** Because degradation is graceful, the
relay can ship at today's 20833 us and raise the floor per-profile. Relay output latency does not
touch the player's input loop (gameplay happens on the source display; the relay feeds the XR1 ->
OBS PC -> Twitch), so tens of milliseconds cost stream viewers a rounding error on top of seconds
they already have. The lag must stay CONSTANT for audio-sync compensation, which the existing
launch-time-constant design already guarantees.

**Measured in situ, the floor needs no raising at all.** The 40-50 ms figure (itself a correction
of an earlier 90-120 ms guess) came from a desktop probe using delivery lag alone. Under the game
with the relay capturing, the governing quantity is `known - display` (see the provider section),
whose p95 is 9479 us at 60x2. An upgrade must land before the target reaches the slot: the real
member is stamped at batch start and flips 8331 us later, so the budget is
`known - display < 20833 - 8331 = 12502 us`. p95 clears it with 3 ms to spare, so **~99% of
upgrades land at today's 20833 us floor**, and the rest are declined by the coherence rule exactly
as designed.

Consequence: `RING_SIZE` stays at 8 and `LagForSourcePeriod` stays at `srcPeriod * 1.25`. If the
tail is ever worth chasing, ~32 ms suffices. If the floor IS raised later, size the ring by
CAPTURED frames (120/s at 60x2, 180/s at 60x3), not by source frames.

## Stage 6 measured on hardware: harmless, and worth almost nothing on this content

Validated live 2026-08-13 (KCD map-cycle content, `b:vsync -src 60 -lock -etw -mark -tint`,
~3 min). Two things were established, and they point in opposite directions.

**The machinery works, which is the part that matters for stage 7.** The session summary:
`14896 batches measured, 38 late, 36 corrected, 0 fence-blocked, 2 lock-declined, 0 skipped`.
Replay had predicted 28 late / 23 corrected on a sibling capture, so the model is sound at
the order-of-magnitude level. The stride chain held live, the overlay applied corrections
through a real present loop, and the one large lateness reading (7071 us) was LOCK-DECLINED
exactly as designed - large lateness clusters near stalls, which is where corrections were
measured to do harm.

**It removes the one blend class a viewer actually notices.** Aggregate blend share is
9.20%, inside the [8.00%, 9.33%] band two back-to-back runs of the SAME build produced - so
by that metric it looks inert. THAT METRIC IS THE WRONG ONE, and reading it cost this project
a session's worth of wrong conclusions. Aggregate share is dominated by map-transition
bursts (70-75% of synthesized frames), where blends are covering real content discontinuities
on screens nobody is judging motion on. The class that matters is the ISOLATED mid-gameplay
blend, and there are only ~2 per 3 minutes to begin with.

Replaying the live capture through the identical policy with corrections armed and not, and
diffing PER PRESENT:

| | measured |
|---|---|
| presents whose decision changed | 401 of 16221 |
| blends REMOVED | **1** |
| blends ADDED | 0 |
| other transitions (pass-before <-> pass-after, both sharp) | 400 |

The removed blend sat 185 presents clear of the previous synthesized frame and 152 clear of
the next - three seconds of clean gameplay either side, i.e. exactly the isolated artifact
class. It was caused by a 5032 us late delivery, corrected. Against a baseline of ~2 isolated
gameplay blends per 3-minute run, that is roughly HALF the visible artifacts, not the
"nothing" an aggregate read reports.

**Why the earlier analysis missed it, which is the reusable lesson.** An offline pass over
the same logs concluded "0 of 10 late blends would cross the passthrough gate". It used the
`boff=`/`aoff=` fields, which come from the STATELESS nearest-anchor join whose confidence
bound is spacing/4 ~= 2084 us. A 5032 us lateness is structurally invisible to it - it logs
as `none:noanchor`. The stride chain reads up to three quarters of a batch period, which is
why the feature finds cases its own diagnostic fields cannot. Never size a correction's value
from the weaker instrument's fields; replay the chain.

Corrections themselves are modest - 38 events, p50 lateness 1142 us - but the tail is what
does the work.

**Cost: +13 us of present-thread time per present that carries a new batch**, and this is a
real reading rather than noise, because the breakdown separates it cleanly:

| present kind | dejit off | dejit on |
|---|---|---|
| passthrough | jit p50 20 us | 33 us |
| synthesized | 22 us | 30 us |
| **hold (source stalled, NO new batches)** | **17 us** | **17 us** |

Hold presents are unchanged because a stalled source delivers no batches to measure, which
identifies the cost precisely: `MeasureLateness` calls `MedianSpacing` twice per batch, each
insertion-sorting up to 256 gap samples. `pdt` is unaffected (16654 us against 16657), so
pacing never sees it, and the fix if it ever matters is caching the spacing rather than
recomputing it per batch.

**The payoff is small in absolute terms and concentrated where it is visible.** The spec's
justifying statistic (28-36% of gaps in the 1.3-2.5 source-period class are "late then catch
up") REPRODUCES exactly - 29% and 27% on these captures - but it was WRITTEN UP misleadingly:
that class is 0.6-0.7% of all batches, so it is 0.18% of batches overall, not "a large share
of the blends". The correct statement is that late deliveries are rare, and that when one
lands in steady gameplay it produces exactly the artifact this project cares about.

**Verdict: keep it. On by default is defensible once a second capture confirms the rate.**
It is do-no-harm gated (0 blends added across 16221 presents), it costs 13 us of
present-thread time that never reaches `pdt`, it exercises the anchoring and overlay
machinery stage 7 depends on, and it removes roughly half the isolated mid-gameplay blends.
A future title with raggeder delivery raises its value, not lowers it; the corpus pipeline
sizes that in one 2-minute capture.

The passthrough threshold remains a separate and much larger dial for AGGREGATE blend share
(srcP/4 -> srcP/3 would sharpen 15.5% of blends) - but it trades motion accuracy to do it,
and it acts mostly on the map-transition bursts that nobody sees. The two are not
alternatives: dejit makes the timeline more correct, the threshold decides how much
incorrectness to tolerate.

## Stage 6 as built: delivery-lateness correction, not flip-stamping

The architecture section below says "upgrade a slot's timestamp from estimated to measured".
Built literally - every stamp moved onto the flip base - the corpus replay measured synth
going 1.4% -> 4.4% on the clean walk and 1.1% -> 5.0% on the jittery one, without the
predicted selection inversion (the comb lock re-converges across the uniform +8.33 ms shift
on its own) but with two costs the design had not priced: every stamp inherits the flip
grid's real jitter (up to 1.6 ms IQR) where arrival stamps are smooth, and the ~4% of slots
that structurally miss the upgrade window (a member's flip cannot be known before it
displays, and upgrades run at present granularity) sit 8.33 ms off-base among corrected
neighbours, poisoning every bracket they touch.

**What shipped instead** (`-dejit`, requires `-etw` with the join on AND `-lock -src`):
measure each batch's delivery lateness - batch start minus its stride-anchored flip - and
subtract it AT BRACKET-READ TIME through a correction overlay (`policy::StampOverlay`),
keyed by the batch-start stamp every member shares. **Nothing ever mutates a ring slot**:
the capture thread stays the slots' only writer (the first build wrote corrections into
slots from the present thread, one slot of slack from the capture thread's write frontier -
an unsynchronized race an adversarial review caught before any capture was spent on it).
The overlay also removes ordering entirely: a member published after its batch was measured
inherits the correction by lookup, and a recycled slot cannot inherit a stale one because
its stamp is a different key. The walk reads batch starts from a small single-producer ring
the capture thread appends at batch open, never from slot fields. The timeline stays on the
arrival base, so the batch-start/lock-phase cancellation is untouched; on-time batches are
left alone entirely.

The gates, each earned by a measured failure in replay:

- **Stride-chained anchoring** (`policy::AnchorBatch`): the median phantom event is ~7.6 ms
  late = 0.9 flip steps, which the stateless quarter-step nearest-anchor MIS-anchors by
  construction. Prediction from the previous anchor reads the true lateness; the stateless
  rule is only the re-acquisition path.
- **The stride is DERIVED, never assumed**: batch period (median of recent batch-start
  gaps) over flip spacing, both measured. The first build hardcoded 2, which is correct for
  the pair submission at 60x2 and 60x3 but structurally unable to anchor with frame
  generation off - it predicted one flip ahead of every batch and never warmed. Multi-batch
  gaps predict round(gap / batch period) batches ahead, which closes the one-flip alias
  where a post-drop batch was "corrected" a full period into the past.
- **The constant-lateness tell**: genuine lateness is a TRANSIENT (a delivery backlog that
  drains within a batch or two, late-then-catch-up; the capture API holds no standing
  queue). A chain that has slipped one flip reads a constant +one-step lateness forever, so
  three consecutive above-gate readings re-acquire through the quarter-step rule, which
  only accepts on-grid batches and therefore restores the true residue. Without this tell a
  mis-locked chain fabricated ~8.3 ms corrections for 11% of a steady walk - and the
  fabrication was only visible because the replay was cross-checked against the fixture's
  own flip list; the bounds alone had hidden it through a feedback accident.
- **Late-only** (an eighth of a step past the grid): lateness is physically one-sided; an
  "early" reading past the gate is a mis-anchor, and correcting one stretched a 50-present
  synth dwell to 59.
- **Chain warm-up** (8 batches after re-acquisition) and **lock calm** (no corrections
  while the phase lock rides out or converges from a stall; `-dejit` without the lock is
  REFUSED because the calm gate would be vacuously open): the same dwell reached 72 without.
- **The coherence rule**, unchanged: a correction is inserted only while both the batch
  stamp and its corrected value are strictly newer than the newest target the policy has
  consumed; after insertion the effective stamp never changes again.

**The sim and production share the same walk semantics by construction** (same
`MeasureLateness`, same overlay, same fence order; the overlay design has no settlement
timing to diverge on). This matters because the first build's sim and production DID
diverge - production had a growing-batch wait the sim didn't model, which pushed ~95% of
its corrections into the coherence fence; the corpus gate was green on behavior the binary
could not reproduce. Every batch verdict is counted and logged (measured / late / corrected
/ fence-blocked / lock-declined / skipped), so a live A/B can distinguish "no late
deliveries" from "corrections measured and discarded".

Replay-validated on the full corpus - SEVEN fixtures spanning three multiplier regimes
(`kcd_60x2_join_events` carries real late-delivery events; `kcd_60x3_walk` and
`kcd_60x1_fgoff` exist so an x2 assumption breaks a test instead of sitting silently inert
elsewhere): clean walk 1.4% -> 1.3% synth (31 corrections, all isolated genuine events),
jitter walk 1.1% -> 1.1%, event fixture 8.4% -> 8.3% with the worst synth run 50 -> 45 and
no runs over 50 - on 25 corrections, because the tell refuses the chain through stalls and
the handful of decisive corrections carry the whole win. At x3 the stride derives to 2 on
the 5.6 ms grid (99.3% of 16835 batches anchored, 104 corrections, synth unchanged at
19.0% - and that 19% IS the known x3 keep-real defect, now recorded as a bound a
phase-aware keep-real must beat). At FG-off the stride derives to 1 (97.7% anchored, 24
corrections) - the regime a hardcoded stride was structurally blind to. The FG-off fixture
also owns two per-fixture gate overrides with reasons in its header: its field numbers are
absent (nearest-mode log carries no op= labels) and its pairing bound is 95% (a stalled
FG-off game stops presenting, so the flip grid pauses WITH it - transition batches have
nothing to pair against). `test_anchor_chain` pins the same three regimes synthetically:
FG-off stride derivation, the dropped-batch alias, and mis-lock recovery.

## What the scanout grid actually looks like

This section replaces an assumption the rest of this document was built on. Everything below is
measured in-game, marker-anchored to video-verified gameplay, over five 2-minute relay captures
and two 10-minute probe runs.

**There is no 8.33 ms grid. There is an 8.33 ms AVERAGE.** At 60x2 the mean inter-flip interval is
8.333 ms (120.0 Hz exactly), but individual intervals run 6.5-10.0 ms in a broad unimodal hump.
The mode bucket holds only ~21% of samples and it takes a 3.5 ms-wide window to capture 94%:

| interval | share |
|---|---|
| 6.5-7.0 ms | 5.6% |
| 7.0-7.5 | 12.5% |
| 7.5-8.0 | 16.0% |
| **8.0-8.5** | **20.8%** |
| 8.5-9.0 | 20.2% |
| 9.0-9.5 | 14.4% |
| 9.5-10.0 | 7.9% |

**This is real, and a second provider proves it.** Microsoft-Windows-DxgKrnl's
`VSyncDPC.FrameQPCTime` - a completely independent event from a provider that DOES register a
manifest, so TDH resolves it by name - reproduces that distribution bucket-for-bucket to within
0.2 percentage points over ~72000 samples, and sits at a rigid constant offset from the NVIDIA
stream (p50 +3785 us, p95 +3805 us, a 30 us spread). The two are the same display events at two
stages of one pipeline.

So the hypothesis that `FlipRequest` is a mere INTENT that hardware corrects is dead. It was worth
testing - the event's own name says "Request" and its field is documented as a PROPOSED time - but
the graphics kernel's scanout DPC agrees with it.

**Why the grid is uneven: VRR.** Scanout is not quantized to the panel's 240 Hz lattice (residual
of each interval mod 4166.6 us has p50 628 us, where a fixed-refresh panel gives ~0). Windows
reports `CurrentSmoothenedVSyncPeriodQpc` = 4166.6 us as the panel's base. So the panel is
240 Hz-capable and G-SYNC refreshes it as frames arrive, at whatever spacing they arrive.

**How much is the source and how much is frame generation. ANSWERED 2026-08-10: it is Smooth
Motion, essentially all of it.** With frame generation OFF, same game, same walk, same panel, the
grid is a metronome: 99.6% of intervals in a single 0.5 ms bucket, sd **48-54 us** on a 16.666 ms
mean, on both heads. So the game's own cadence is even and G-SYNC reproduces even input evenly;
the 6.5-10 ms smear at x2/x3 is Smooth Motion's placement of its metered flips. The earlier
paragraph here read the pair-sum sd (714 us) as "ordinary game frame-time variance" - wrong,
because pair sums were measured THROUGH Smooth Motion, whose real-frame placement wanders too.
`VSyncSmoothenedTime`'s mean correction tracks the same thing: +0.006 ms FG-off, -0.319 ms at x2,
-0.893 ms at x3.

**The tight-spike readability criterion is RETIRED.** The EtwProbe section used to say a smeared
histogram "kills the arithmetic model outright". That criterion was met exactly once, on an idle
desktop at 240 Hz (95.7% in a single 0.5 ms bucket), and has NEVER been met in-game. Meanwhile the
relay runs at 1.1% gameplay synth on this grid with the comb lock engaged 90% of presents. The
criterion was wrong about what mattered.

**An unexplained two-regime oscillation, which costs nothing.** Flip intervals sit in one of two
states, stable for minutes at a time: sd ~900 us with lag-1 autocorrelation ~-0.67 (alternating),
or sd ~350 us with correlation ~-0.09. Established about it:

- NOT caused by the flip join: a pre-join build produced the high state, and a `-nojoin` run
  produced it too.
- NOT event loss: 0 lost events, 0 realtime buffers, 0 log buffers, 0 decode failures, 0
  out-of-order across every capture.
- NOT duplicate or revised events: 0 exact duplicate display times, 0 backward deltas.
- NOT `VsyncState`: it cycles 0->1->2 every few seconds and the regime does not track it
  (sd 913 us / corr -0.625 in state 0 vs 855 us / -0.644 in state 2).
- NOT warm-up or capture-start alignment: it switches mid-capture, in both directions.
- Does NOT reach relay output: a clean-grid and a jittery-grid walk produce identical results
  (synth 1.1%, worst run 7, both). Both are in the corpus as `kcd_60x2_walk` and
  `kcd_60x2_walk_jitter`.

`FlipQueueIntervalTarget` is always 0 on this hardware, so there is no published target to compare
intent against outcome. Leave this open; it is a VRR-scheduling curiosity, not a blocker.

## The hardware flip-queue completion: a queue marker, NOT a real-frame label

DxgKrnl event 505 `VSyncHwFlipQueueLogUpdate` looked like a real-frame label at x2 and the
multiplier sweep killed it. Measured 2026-08-10 over three 120 s captures, same content (KCD
steady walk), same relay config (`t:vsync -src 60 -lock -etw`), identically windowed; head 1 is
the relay's own FG-free 60 Hz output and reads ratio 1.000 in every run:

| head 0 (game) | FG off | 60x2 | 60x3 |
|---|---|---|---|
| flips | 60.0/s | 120.0/s | 180.0/s |
| 505 completions | 60.0/s | 58.4/s | 89.2/s |
| flips per completion | **1.000** | **2.053** | **2.017** |
| a real-frame label predicts | 1.0 | 2.0 | **3.0** |
| marked-flip stride on the grid | 1 (100%) | 2 (98.4%) | **2 (99.1%)** |
| `done=` (FlipsCompletedCount) | 1 always | 1 always | 1 always |
| `CompletionTimeStamp` on a flip | 100% | 100% | 99.9% |

**What 505 actually is: the completion of a flip that WAITED in the hardware flip queue.** Under
frame generation, submissions arrive in pairs at every multiplier - one member flips immediately
(`ahead` ~0), the other is held one grid step (`ahead` ~ one flip interval) - and 505 fires for
the held member only, hence stride 2 regardless of multiplier. With FG off every flip is
vsync-paced through the queue, hence stride 1. `done=` is 1 in every event across all three
modes, and with FG off `sum(done)` equals the flip count exactly with `PresentId` 100%
contiguous, so it is one event per queued flip, not a batched report.

**Why that is not a label.** At x2 the held member happens to be the real frame (interpolation
cannot produce the between-frame until the real one exists, so the generated one flips first) -
but that is the batch-position fact the relay already uses, seen from the kernel side. At x3 the
held members alternate stride 2 across a 3-flip grid, so the marks rotate through all three
residues (marked-stream mean interval 11.2 ms = 2/3 of a source period) and cannot name the real
frame, which lives at ONE residue at 60/s. A 60/s truth cannot be read from a 90/s marker.

**The excess over 2.000 at x2 (2.053) is missed events, not structure**: the stride-gap
distribution has runs of 4, 6, 8, 10 with exactly matching `PresentId` jumps - occasional held
flips whose completion never got logged.

**Reading a 505 stream, practical notes.** It is logged per plane (desktop carries multiple
planes; fullscreen gameplay is a single `PlaneId` 0 sequence). The probe's detail lines for ids
259/266/505 share ONE `--events` budget, so when it runs out the DxgKrnl lines stop while the
NVIDIA flip lines carry on - which reads as flips with no completion. The ratio counters in the
probe summary sit OUTSIDE that budget for exactly this reason; the first pass at the x2
measurement reported 56.8/s instead of 60.0/s off truncated detail lines.

## The provider

```
NVIDIA DisplayDriver {AE4F8626-8265-40D1-A70B-11B64240E8E9}
FlipRequest (Id 1, level 0x04, keyword 0x1000000000000000)
fields: alloc (u64), vidPnSourceId (u32), ts (u64), token (u32)
```

`ts` is the PROPOSED FLIP TIME in QPC ticks. Consecutive `ts` values on one head are the true
scanout cadence. GUID and descriptor come from the manifest PresentMon embeds, cross-checked
against its consumer.

**The wire layout is now recovered from hardware, not taken from that field list.** TDH cannot
resolve field names here: NVIDIA's provider does not register a manifest (10800 of 10800 events
failed on a current driver), which is why PresentMon ships a reverse-engineered copy of the schema
rather than asking the system. Decoding is positional, from a layout read off real payloads:

```
+0  u64   per-head counter, +1 per event
+8  u64   second counter, +1 per event
+16 u64   PROPOSED FLIP TIME (QPC)
+24 u32   vidPnSourceId (head)
+28 u32   token
+32       12 bytes, zero on every payload observed
44 bytes total, descriptor ver=0 opcode=10
```

`+16` is identified positively rather than by elimination: its per-head deltas land on the 8.33 ms
half-period a 60 fps source with 2x frame generation produces, and on a 240 Hz desktop they land on
4.167 ms with 95.7% of gaps in a single bucket - a rate known independently of the measurement.
Decoding is guarded three ways (payload size, descriptor version, and the flip time landing within
a second of the event that announced it) so a driver that moves the layout stops decoding rather
than reporting confident nonsense.

**Two heads appear, not one.** The gaming display at 120 flips/s under 2x and the relay's own XR1
output at 60. Filter by head; never aggregate them.

**`ahead` is ~0 at the median and up to a full flip interval in the tail.** The earlier reading
(0-880 us, "no advance notice") was the median of a desktop probe and understates the tail badly.
Measured in situ over 14332 head-0 flips at 60x2 and 21290 at 60x3:

| | 60x2 | 60x3 |
|---|---|---|
| ahead (event before display) p50 | 228 us | 239 us |
| ahead p95 | **8205 us** | **5445 us** |
| ahead max | 8647 us | 5900 us |

The p95 is one flip interval (8331 us at x2, 5559 at x3): the driver queues the next flip while
the current one is still scanning out, so a large minority of flips are announced a full frame
early.

**The quantity that governs the design is neither `ahead` nor `lag` but their difference:
`(event + lag) - display`, how long after a flip appeared the relay could first know about it.**

| | 60x2 | 60x3 |
|---|---|---|
| known after display p50 | +1353 us | +2965 us |
| p95 | +9479 us | +9578 us |
| known BEFORE it displayed | **37.2%** | 23.3% |

So delivery lag is NOT the whole budget. Storing lag alone in a replay fixture is wrong by a
median of ~3.8 ms and by a full flip interval in the tail, which is why the fixture format
records `known - display` per flip rather than the lag it was derived from.

## EtwProbe

Standalone exe, in the solution so CI builds it. Requires elevation. Reports:

- **dts histogram** - gaps between consecutive proposed flip times on a head. Expect a BROAD hump
  centred on 8.33 ms in-game, not a spike: see "What the scanout grid actually looks like". The
  old criterion here ("a tight spike means the grid is readable, smeared kills the model") has
  been retired; it only ever held on an idle desktop. Read the histogram, never the mean - the
  mean is 8.333 ms in every capture including the ones where intervals swing +-20%, so it hides
  exactly what this histogram exists to show.
- **ahead** - `ts` minus the event stamp. ~0 at the median, up to a full flip interval at p95;
  report the distribution, not a single number, and see the provider section for why the figure
  that actually matters is `(event + lag) - ts`.
- **lag** - callback time minus event stamp: raw ETW delivery latency. This sizes the floor.
- **session losses** - events, real-time buffers and log buffers dropped, read from the session at
  stop. Mandatory next to any latency figure: latency bought by discarding events looks identical
  to latency earned, and this project's recurring failure is the silently wrong measurement.
- **hexdump of the first 8 payloads**, always, decode success or not. It is the only route to
  re-deriving the layout when a driver update moves it.
- **`--dxgk`** - enables Microsoft-Windows-DxgKrnl. Still the liveness control (NVIDIA silent +
  DxgKrnl alive = the provider is the problem, not the session), but now also the corroboration
  path. It prints a census of every event kind that arrives with TDH-resolved task, opcode and
  field names, then decodes the ones that matter. DxgKrnl registers a manifest, so unlike NVIDIA's
  provider its fields are read BY NAME rather than positionally.

  Decoded ids `{17,181,259,266,273,458,502,503,505,506}`:

  | id | event | what it gives |
  |---|---|---|
  | 17 | `VSyncDPC` | **`FrameQPCTime`** + `VidPnSourceId` - the independent scanout stream |
  | 181 | `VSyncInterrupt` | its event-header stamp is the raw vblank, no payload decode needed |
  | 259 | `MMIOFlipMultiPlaneOverlay` | submission; count matched NVIDIA FlipRequest EXACTLY (72056 both) |
  | 266 | `IndependentFlip` | `PresentAtQpc` |
  | 273 | `VSyncDPCMultiPlane` | `FlipQueueIntervalTarget` - always 0 on this hardware |
  | 184 | `Present` | the APPLICATION's own Present() call, with **ProcessId in the event header** |
  | 458/503 | VRR state | `VsyncState`; cycles 0->1->2 every few seconds |
  | 502 | `VSyncSmoothenedTime` | `OriginalDpcFrameTime` vs `SmoothenedDpcFrameTime` |
  | 505 | `VSyncHwFlipQueueLogUpdate` | `CompletionTimeStamp` + `PresentId` + `PlaneId` - completion of a QUEUED flip, see its own section; NOT a real-frame label |

  Requested through an `EVENT_FILTER_TYPE_EVENT_ID` filter. UNFILTERED THIS PROVIDER IS A
  FIREHOSE: 27.4 million events in 600 s, which pushed ETW delivery latency to 738 ms p50.
  Filtered it is ~500/s. `--dxgkall` drops the filter to re-run the full census after a Windows
  update; do that rather than assuming the event ids above survived.

## Required session configuration

Any consumer, probe or relay, needs all of these. Each was established by measurement and three of
them are the difference between an 8 ms pipeline and a 1.5 second one.

```
StartTrace:
  Wnode.ClientContext = 1                              // QPC, so flip times and NvFBC arr= share a clock
  LogFileMode = EVENT_TRACE_REAL_TIME_MODE
              | EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING // removes ~14 ms of pipeline delay
  BufferSize = 64 KB, MinimumBuffers = 256, MaximumBuffers = 1024   // PresentMon's sizing

OpenTrace:
  ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME
                   | PROCESS_TRACE_MODE_EVENT_RECORD
                   | PROCESS_TRACE_MODE_RAW_TIMESTAMP  // without this EventHeader.TimeStamp is
                                                       // system time, not the QPC asked for above

Consumer thread:
  ControlTrace(EVENT_TRACE_CONTROL_FLUSH) every ~10 ms // delivery is otherwise on a ~1 s cadence
```

`RAW_TIMESTAMP` is the subtle one: `Wnode.ClientContext` alone does NOT make the event header
arrive in QPC, and mixing a QPC payload against a system-time header silently rejects or corrupts
every comparison. Both PresentMon and this probe set it.

Read the session's loss counters at stop and log them. Every configuration above was verified to
lose zero events across seven runs, but that is a property of a measured workload, not a guarantee.

## Open questions

0. **THE ONE THAT GATES STAGE 7: does a generated frame's CONTENT phase match its DISPLAY
   phase?** Nothing measured so far speaks to this, and every other question here is now closed
   enough to proceed without.

   A generated frame's content is an interpolation at some fraction between two real frames. Its
   display time is wherever the driver's metering put it, and that is measurably NOT the midpoint:
   placement wanders by hundreds of microseconds and, in the high-jitter regime, by over a
   millisecond. If the driver interpolates at fraction f and displays at fraction g with f != g,
   then flip-stamping a generated frame places its motion at the wrong instant by (g - f) of a
   source period, silently.

   The likely answer is that they AGREE: a frame-generation implementation has every reason to
   generate content for the time it intends to display, and "insert a midpoint frame" would be the
   naive design. But that is reasoning, and this project has been burned by reasoning that sounded
   this good.

   It costs nothing today. At 60x2 keep-real discards the generated frame, so its placement never
   reaches the ring. It becomes load-bearing the moment selection starts USING generated frames.

   How to settle it without new instrumentation: content phase is measurable from pixels. A
   generated frame sits at some fraction between its neighbours, and the July `-probe` work
   already measured generated-frame content (archived at `archive/fg-and-dupe-content-probe`).
   Measure content fraction against ETW display fraction on the same frames; if they track, stage
   7 is safe.

1. **Raw ETW delivery lag. MEASURED - and the session must be configured for it.** Left at
   defaults, real-time ETW delivers on a ~1 s cadence and hands over the PREVIOUS period's buffer,
   so even the freshest event in a delivery is ~1 s old (p50 1487 ms, max 2025 ms). That is 70x
   past the relay's bracketing lag and would have killed the design. Two settings fix it, and both
   are needed:

   | configuration | lag p50 | p95 | max |
   |---|---|---|---|
   | defaults | 1487 ms | 1961 ms | 2025 ms |
   | forced flush every 10 ms | 23.7 ms | 30.7 ms | 35.9 ms |
   | `EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING` | 20.2 ms | 36.8 ms | 37.5 ms |
   | **both** | **8.0 ms** | **15.0 ms** | **30.4 ms** |

   The two are not the same mechanism. A forced flush (`ControlTrace` with
   `EVENT_TRACE_CONTROL_FLUSH` on a timer) delivers OFTEN but each delivery is stale; consolidating
   the per-processor buffers makes each delivery FRESH by removing ~14 ms of pipeline delay.
   Combining them gives frequent delivery of fresh data. The relationship is simply
   `p50 ~= flush interval / 2`.

   Two practical notes. `Sleep()` is quantised by the 15.6 ms system timer, so a 10 ms request
   produces a 16 ms interval and 20 ms and 30 ms requests are indistinguishable (both measured
   15.7 ms p50); a high-resolution waitable timer would take p50 to ~2.5 ms if anything ever needs
   it. And zero events were lost in any configuration, including the consolidated one that
   Microsoft documents as able to lose them - verified across seven runs, not assumed.

   PresentMon independently confirms the approach: it uses `PROCESS_TRACE_MODE_RAW_TIMESTAMP` and
   exposes a `SetEtwFlushPeriod` API for exactly this, defaulting to off. It does NOT consolidate
   per-processor buffers, and it sizes its pool at 64 KB x 256-1024 buffers, which this probe now
   matches.

   **Now measured in situ, and the table above is the pessimistic case.** Under the game with the
   relay capturing, delivery is FASTER than on the idle desktop, and x2 and x3 agree within 4% at
   every percentile despite x3 carrying 50% more events - confirming delivery is flush-dominated
   rather than event-rate dependent:

   | | 60x2 walk | 60x3 walk | desktop probe |
   |---|---|---|---|
   | p50 | 5286 us | 5496 us | 8000 |
   | p95 | 9974 us | 10024 us | 15000 |
   | p99 | 10867 us | 10898 us | - |
   | max | 22671 us | 31173 us | 30400 |

   Consequence for the lag floor: none. See the sizing section - the budget is governed by
   `known - display`, not by lag, and ~99% of upgrades land at today's 20833 us floor. The
   40-50 ms figure is superseded and RING_SIZE stays at 8.
2. **Pairing. Much easier than this section assumed, but the RULE is still unvalidated.** The
   pessimism here predated knowing both streams would share a clock. They do: the probe requests
   QPC (`Wnode.ClientContext = 1` plus `PROCESS_TRACE_MODE_RAW_TIMESTAMP`) and the relay logs its
   QPC origin at startup, so the two logs join by arithmetic with no fingerprinting. Against a real
   capture, every other head-0 flip coincided with an NvFBC wake to within **+73, +62 and -40 us**.
   That is identification, not structural matching.

   What that establishes and what it does not:

   | claim | status |
   |---|---|
   | the first wake of a batch coincides with a flip | measured, p50 223-244 us, 93-94% within 1 ms |
   | the second wake belongs to the NEXT flip | **measured indirectly by tiling**, 2026-08-03 |

   The second was reasoning when this was written; it is now supported by a structural
   measurement. Consecutive batches advance by **exactly 2 flip indices** - 94.9% at 60x2
   (7348 batches) and 96.4% at 60x3 (10795 batches) - while carrying 2 members each. Two members
   per batch advancing 2 flips per batch means members tile the flip grid one-to-one in order,
   with no gaps and no double-coverage, which is only consistent with the second member belonging
   to the next flip. Not a direct per-frame proof, but a whole-session one over ~18000 batches.

   The gate for the in-code rule is reproducing the offline join: 22942 captures at 60x2 and
   32249 at 60x3 already have verdicts computed outside the relay, so the implementation can be
   checked against them rather than against a fresh capture. An unpairable wake must still come
   out explicitly unpairable rather than silently matched to a neighbouring flip; at a 1 ms gate
   that is ~7% of wakes.

   One consequence already visible: batch-collapse stamps the surviving REAL frame at batch-start,
   but the flips say that frame does not reach the screen until ~8.33 ms later. A constant offset
   is harmless; it stops being constant when the batch structure varies.
3. **Is the grid readable at x3? YES - and the rotation PHASE is readable from arrival timing
   (2026-08-10, two captures).** The 505 rate test does not answer it (completion marks rotate
   through all residues), but the batch classes do. Classifying batches by anchor flip index
   mod 6 over stride-2 chains, both x3 captures show the same three-class cycle on two
   independent features:

   | class | anchor offset p50 | single-wake share | composition |
   |---|---|---|---|
   | real-led | **-43 / -61 us** (leads its flip) | normal | **[real, gen]** |
   | real-trailing | +76 us | **elevated (4.0% / 7.5%)** | **[gen, real]** |
   | all-generated | +77 us | low (2.0-2.5%) | **[gen, gen]** |

   The naming rests on the x2 calibration (gen-led batches wake +74 us after their flip) plus
   the late-real single-wake mechanism, consistently across both captures; a marked-video check
   would make it ground truth. Per-batch classification is weak (~125 us shift against ~200 us
   spread) - the phase is an ENSEMBLE property: vote over a few hundred batches, dead-reckon on
   the 96%-rigid stride. The mod-6 phase held globally for the full 120-160 s of both captures.

   **This also names today's x3 failure**: batch-collapse keep-real retains member 1, which
   keeps the real frame only in [gen, real] batches - one source period in two - and DISCARDS
   the real in [real, gen] batches. A phase-aware keep-real (member 0 in real-led batches,
   member 1 in real-trailing ones) keeps exactly one real frame per source period. Analysis:
   `x3residue.py` against `etw_x3_walk` and `join2_x3_on`.
4. **Does DLSS-FG look structurally different** from driver-level Smooth Motion? It is an in-game
   SDK path rather than a driver one, so it may not emit FlipRequest with the same shape, or at
   all. Everything below assumes it does until measured.

4b. **Can the multiplier be DERIVED rather than configured?** A "-fg x2 / x3 / x4" flag would be
   one more thing to get wrong, and would be silently wrong the moment the user changes a driver
   setting without changing the flag. Both quantities needed appear to be measurable at runtime:

   | quantity | how it is read |
   |---|---|
   | multiplier | flips per capture batch, or source period / flip period |
   | which member is real | position in the batch |

   At 60x2 the batch holds 2 wakes and the interval carries exactly 2 flips in 99.96% of cases, so
   the multiplier falls straight out of counting. The real member is the SECOND of two, which the
   two-video A/B established independently of any of this.

   The rule that would generalise without configuration is **"the real frame is the LAST member of
   the batch"**, which follows from how interpolation must work: the driver cannot produce frames
   between N-1 and N until it has N, so the generated ones are submitted first. If that holds, x3
   is [gen, gen, real] and x4 is [gen, gen, gen, real], and nothing needs declaring.

   UNVERIFIED beyond x2. Profiling x3, x4 and DLSS-FG is what settles it, and the failure mode if
   the rule does not generalise is severe rather than subtle: picking the wrong member outputs
   almost entirely generated frames. The offset from the nearest flip is a weaker independent
   signal (74 us for the generated member, 411 us for the real one at x2, separating 99% / 76%),
   and it is the only signal available for the ~17% of batches that arrive with a single wake,
   where position cannot say anything.
5. **Lost events fail silently.** An estimator fed incomplete data is the silent-wrong failure this
   project keeps hitting. Whatever consumes ETW must log lost-event counters.
6. **Two behaviour modes is two test surfaces.** The more the policy exploits ETW when present, the
   more the modes diverge, including at the boundary where upgrades land intermittently. The
   trace-replay harness is what makes that testable without a capture cycle.
7. **Late real frame, or a generated frame that missed the batch window?** The wide brackets that
   force blends at 60 fps sit a median of 0.46 source periods off the grid, which fits BOTH a real
   frame handed over late and a Smooth Motion twin whose delivery slipped past the 3 ms collapse
   threshold. The wake timelines are identical. The x2 baseline trace answers it: count the flips
   per source period and compare their scanout spacing against the wake spacing over the same
   interval. If the answer is "generated frame", the 3 ms threshold is the thing to fix and it is
   cheaper than consuming ETW at runtime. Still open: the traces so far are desktop-only, and this
   question needs the game running with the relay capturing.

## Settled, do not re-litigate

- **PresentMon as the consumer.** Rejected: it collapses per-flip data, and its `FrameType` enum
  has no NVIDIA member (`NotSet, Unspecified, Application, Repeated, Intel_XEFG, AMD_AFMF`), so it
  cannot label Smooth Motion frames anyway. `--track_frame_type` requires instrumentation via the
  Intel-PresentMon provider, which the NVIDIA driver does not emit.
- **Real-vs-generated labelling from any ETW source.** Not available for Smooth Motion.
  REOPENED 2026-08-04 when DxgKrnl turned out to be reachable and manifest-backed;
  **RE-CLOSED 2026-08-10 on a three-mode measurement (FG off / x2 / x3), and this time the
  MECHANISMS are known, not just the absence.** Both candidates died:

  **Event 505 `VSyncHwFlipQueueLogUpdate`** fires once per flip that WAITED in the hardware flip
  queue - the held member of each submission pair under FG, every flip with FG off. At x2 the
  held member coincides with the real frame, which is the batch-position fact the relay already
  uses; at x3 the marks alternate stride 2 across the 3-flip grid and rotate through all
  residues. Ratios measured 1.000 / 2.053 / 2.017 against a real-frame label's 1 / 2 / 3. See
  "The hardware flip-queue completion".

  **Event 184 `Present`** is the kernel-level present behind EVERY flip, generated ones
  included: the game's pid presents at exactly the flip rate at every multiplier (60.0/120.0/
  180.0 per second), from exactly two alternating `hSrcAllocHandle` values at half the flip rate
  each, on one kernel context. The two-handle ping-pong exists with FG off too (the game's own
  presents alternate two handles at 30/s each), so it is flip-model buffer parity, not a
  real/generated asymmetry. At x3 real:generated is 60:120 while the handles split 90:90, which
  no per-frame labelling could produce.

  Re-reopening this needs a NEW event source, not a new reading of these two. What the relay
  actually has: batch position at x2 (proven by the Round 10 A/B), nothing from ETW at x3 -
  x3's residue question stays open and content analysis is the remaining route.

  The batch-position label remains what the relay actually uses, and the keep-first vs keep-real
  A/B (Round 10) established it visually on real output - stronger evidence than a driver label
  would be. But batch position does NOT generalise to x3, where the pair boundary rotates through
  the source period and the real frame is the first member of one pair, the second of the next,
  and absent from the third.

  What has changed since that entry was written: the label is now REQUIRED rather than merely
  available. Correct timestamps make generated frames usable, and a selection that cannot tell the
  two apart lands on generated frames 93.2% of the time at 60x2. See the constraint under
  Decisions. So "not needed either" was true only while generated frames were being discarded.
- **ETW as offline-only calibration.** Too weak. It discards the data's main value, which is
  aligning to patterns no built-in heuristic covers.
- **"Per-frame lookup is impossible."** It is viable given a raised lag floor and unambiguous
  pairing. Both are measurements, not assumptions.
- **Elevation.** Admin or the Performance Log Users group. Moot here: the relay already runs as
  admin.
