# Lock transitions: easing wraps and re-phasing after phase steps

Draft for sign-off, 2026-09-25. Step 1 is implemented. Steps 2 and 3 are proposals.

## 1. The rule this serves

When the target sits on the comb, the relay shows a real frame. When no real frame exists at the
target time, it shows a blend of the two neighbours. A blend stands in for the optical-flow frame
NVOFA will make later, so a blend is always better than a repeated or skipped frame.

The comb lock keeps the target on the comb. Sometimes it has to move the target to a different
place on the comb, and how it makes that move decides what the viewer sees. This spec covers the
three moves the lock makes today and changes two of them.

| move | cause | today | this spec |
|---|---|---|---|
| wrap | the pull drifts past the edge of its range | an instant jump of one comb: a repeated or skipped frame | step 1: fewer wraps (done); step 2: each wrap eased into blends |
| phase step | the game's frames move to a new phase and stay there | the lock slews at 25 us per present: 40 to 180 presents off the comb | step 3: see it coming in the ring and re-phase at the step |
| stall resume | the source froze, then resumed | an instant snap to the new phase | unchanged: the picture was already frozen |

## 2. Step 1: the wrap band (implemented)

The pull may pass [0, comb) by a band on each side before it wraps by a comb. The band was comb/16
(1.04 ms at 60 fps) on both sides. Measured on the corpus, the pull wandered 1.6 ms past the edge
typically and 4.1 ms at the 90th percentile, so it wrapped back and forth: a repeat and a skip
that cancel.

A pull that has just wrapped sits one band inside the far edge, so it has to cross both bands to
wrap back. Only the sum of the two bands decides how much wander is absorbed. The band below zero
shortens the lag and uses up its margin, so it stays at comb/16. The band above the comb only adds
lag, so it is now comb/8 (`PullWrapBelow`, `PullWrapAbove` in `TemporalPolicy.h`).

| | comb/16 both sides | comb/8 above |
|---|---|---|
| corpus wraps | 441 | 359 |
| back-and-forth pairs within 5 s | 69 | 26 |
| wraps per hour, four corpus streams | 29.5 | 21.4 |
| held-out 09-23 stream: wraps, repeats, skips | 24, 246, 50 | 20, 243, 48 |
| blend share, long runs, worst runs | | unchanged on every fixture |
| average lag | | +0.6 ms |

comb/4 above cut stream wraps by 43% instead of 27%, but on the held-out 09-23 stream it lengthened
one blend run from 87 to 99 presents. The pull sat one comb higher, the target landed on the
previous real frame, and a stall re-seed that had cut a phase-step run short did not fire. That is
luck around the weakness step 3 addresses, but a held-out regression is the signal to stop, so
step 1 uses comb/8.

Step 1 also adds `gm_60x2_card5995_0924`, 30 minutes of the 09-24 stream with the capture card at
59.95 Hz, where every wrap is forced.

## 3. Step 2: easing a wrap (implemented)

### Behaviour

A wrap still happens in the lock's bookkeeping exactly as today. The displayed target stops
jumping: it crosses the comb over N presents instead of one. A wrap that would repeat a frame
becomes N presents that each advance the content by (1 - 1/N) of a frame. A wrap that would skip
becomes N presents that each advance it by (1 + 1/N). The composite decides each of those presents
as usual: a real frame while the target is within the passthrough gate, a blend in between. At 60
fps the gate is a quarter of the comb, so roughly the middle half of the ramp blends.

### Design

- `PhaseLockState` gains `easeQpc`. When `UpdatePhaseLock` wraps the pull by plus or minus a comb,
  it sets `easeQpc` to the opposite amount, so `pull + ease` does not move on the wrap present.
  Every present after that, `easeQpc` moves toward zero by comb/N.
- The target becomes `deadline - (lag + pull + ease)`, in `TemporalCaptureMode` and in the suite's
  `Simulate`, which must mirror production.
- The lock measures its error at the target without the ease, so the ramp is invisible to the
  control loop. The bracket is read at the eased target, so the lock's input becomes
  `WrapHalf(beforeDiff + ease, comb)`. The comb is periodic, so the correction is exact while
  |ease| is under a comb, which it always is.
- A stall resume clears the ease: the snap lands on a frozen picture, and a ramp would only delay
  the resumed phase.
- A disengage does not clear it: the ramp finishes on its own schedule.
- **Blend modes only.** Selection (`t`) cannot show an in-between frame. Tested with easing, each
  wrap's single repeat became a repeat followed by a double advance (7 of them in 7 wraps), so a
  wrap there still jumps. The lock reads `passthroughQpc > 0` to tell.
- **Only a wrap taken while the target is on the comb.** When the target is already between
  frames, the output is already blending, and jumping with the pull reaches real frames sooner
  than a ramp. Tested three ways on the corpus (N = 12):

  | rule | repeats + skips removed | blends added | runs >= 50 | worst runs worse / better |
  |---|---|---|---|---|
  | every wrap | 309 | 1,224 | 30 | 3 / 3 |
  | steady tracking only | 158 | 711 | 30 | 2 / 2 |
  | target on the comb | 266 | 1,149 | 29 (as today) | 1 / 0 |

  Easing every wrap lengthened blend runs where a wrap lands inside a convergence (the 90-minute
  stream's worst run 60 to 67): the jump was taking the target to where the lock was heading. The
  one worse worst run under the chosen rule is a steady walk whose longest blend run is now a
  ramp, 2 to 6.
- `PolicyConfig.wrapEasePresents` holds N, defaulting to `kWrapEasePresents` (12). The suite's
  `Simulate` sets it to 1 through `noWrapEase` for controls that must show the old behaviour.

### Constraints

- **Tooth guard.** With the source at the sink rate, the composite refuses to blend unless the
  target has advanced at least 7/8 of a source period since its last decision, and re-presents the
  last frame instead (`SynthWouldManufactureTooth`). A repeat-direction ramp advances
  (1 - 1/N) of a period per present, so N below 8 turns the ramp back into repeats, and N at 8 sits
  on the cut. Either N is at least 12 with the guard untouched, or presents inside a ramp are
  exempt from the guard. The first keeps a proven guard unchanged, and the sweep below decides
  whether 12 to 16 presents is acceptable.
- **Output never runs backwards.** The target advances every present in both directions, so the
  monotone output guard is not touched.
- **Combs finer than a frame.** At 90 fps into 60 Hz the comb is half a source period. A wrap there
  moves the target by half a frame, and easing it is still a ramp across the comb. The 90 fps
  fixtures must show no change for the worse.
- **Ramp length.** The sweep settled N = 12:

  | N | repeats + skips removed | blends added | runs >= 50 |
  |---|---|---|---|
  | 8 | 155 (the tooth guard turns many ramps back into repeats) | 709 | 30 |
  | 12 | 309 | 1,224 | 30 |
  | 16 | 321 | 1,589 | 31 |
  | 24 | 326 | 2,288 | 31 |

  These rows ease every wrap, and the on-the-comb rule above then trims them. 12 removes almost all
  of what longer ramps do, for the fewest blends.

### Log and tools

- The temporal line gains `ease=`.
- `pull=` still wraps, so `mktrace.py` and the wrap counts keep working.
- The wrap fingerprint used to align unmarked recordings (a repeat or a skip at each wrap) goes
  away, because there is nothing to see. `-mark` recordings are unaffected.

### Measured before any code

A scratch sweep of N in {8, 12, 16, 24, 32} on the corpus and on the held-out logs:

- repeats and skips at wraps (content steps of 0 or 2 periods within N presents of a wrap), which
  should reach zero once N clears the tooth guard;
- blends added per wrap;
- worst runs, long runs and stall re-seeds, which should not move.

### Checks

- **Unit tests** (`test_wrap_easing`):
  - on the beat in both directions, no content step of 0 or 2 periods after warmup, output
    monotone, 2 to N blends per wrap;
  - the same timelines with `noWrapEase` must show a repeat or skip per wrap, the control;
  - a stall resume clears a ramp, and selection mode never starts one.
- **Tests whose premises changed.** The locked-regime and hole tests skip presents whose target
  carries an ease (`EasedTarget`), and their census pins now include the ramp blends, each with
  the old pin in a comment. The composite-config differential compares a timeline that never
  wraps on every present, and a beating one up to its first ramp, since a ramp reads a different
  frame pair and the two runs part by microseconds from there. The dejitter diff skips presents
  inside a ramp in either arm.
- **Corpus bounds re-set,** each with the reason in the fixture: `kcd_60x2_join_events` worst
  run 50 to 51 (a wrap at the tail of a run, eased one present longer), `gm_60x2_card5995_0924`
  synth 1.2% to 1.7% (the forced wraps now blend), `avatar_60x2_subgen_bench` re-seeds 13 to 12
  (two borderline re-seeds, identical blends around them), and the lock-wrap fixtures' wrap
  bounds tightened to 1 and 4 (step 1's gain).
- **Held out:** the 09-23 stream reads repeats 243 to 239, skips 83 to 77, blends 1,174 to 1,229,
  worst run and long runs unchanged. The 09-24 stream at 59.95 Hz skips 770 to 408.
- **Field:** a stream on the new build with the card at its custom 60 Hz mode. From the log, count
  content steps of 0 or 2 periods at wraps taken on the comb: the target is zero.
- **Undo:** `kWrapEasePresents` = 1 is the instant wrap.

## 4. Step 3: re-phasing quickly after a phase step (implemented)

### As built

The sections after this one are the plan. What was built differs from it in these ways, each for
a reason the corpus showed:

- **The comparison.** The phase the target is on now is the median of the three real frames at
  or before the target, not the lock's error EMA. The first detector compared the newest frames
  with the EMA and fired thousands of times, steady fixtures included: while the lock is still
  converging, its EMA can sit far from the frames the target is on (-0.7 ms against a target 8 ms
  off, measured). A per-frame noise study showed the signal itself was clean (median 0.16 to
  0.34 ms, under 3 ms on 99% of frames, the newest frames no noisier than older ones).
- **Consistency.** The newest four frames must agree with each other within an eighth of a comb.
  A messy stretch of mixed phases never plans.
- **Guards.** Only with the lock engaged, outside a stall run, outside the recovery window, and
  only while the target is on a real frame.
- **Thresholds,** as fractions of the comb: a step is more than a fifth of a comb (3.33 ms at 60
  fps), and must hold for two new frames. Swept against 3 ms, comb/6 and 2 ms of spread: all
  within a few blends of each other; comb/5 makes the fewest moves.
- **No re-seed.** A move lands exactly as the step reaches the target, so the lock never sees an
  error to re-seed.
- **Wrap easing learned to see the after frame.** A move can push the pull past a band edge, so it
  wraps on the same present, and step 2 judged "on the comb" from the before frame alone. At the
  late frame of a step that frame is more than a period back, so the wrap jumped and repeated a
  frame. `UpdatePhaseLock` now takes the bracket's after side too, which on its own removes 4
  repeats and 5 skips across the corpus.
- **The ring reader.** `CaptureRing::ReadRecentFrames` reads the same window and dejitter
  corrections as `FindBracket`, with the slot's write index as each frame's sequence.
- **Not built:** the reactive fallback for `-lag 0`. With no extra lag the ring holds about one frame
  ahead of the target, so the lookahead never plans there and the lock behaves as before.
- **Known gap:** a step of about half a comb. Its frames straddle the half-comb fold, so they never
  agree and the lookahead leaves the step to the slew. A circular median fixed the arithmetic but
  made the rest of the corpus worse (long runs 20 to 25), so it was left out.

Results, dejitter replay (how the relay runs), against step 2:

| | step 2 | with the lookahead |
|---|---|---|
| corpus blends | 117,167 | 114,785 |
| corpus runs >= 50 | 31 | 20 |
| corpus repeats / skips | 11,541 / 31,559 | 11,537 / 31,557 |
| moves (planned, cancelled) | | 204 (212, 8) |
| steady fixtures, plans | | 0 |
| held-out 09-23: blends, worst run, runs >= 50 | 995, 87, 3 | 600, 47, 0 |

The suite's `test_phase_lookahead` runs synthetic steps with a lookahead-off control each: a
step that holds (1 move, 0 to 5 blends against 46 to 67), one that swaps back after 0.7 s (2 moves,
10 against 41), one that swaps back inside the lookahead (no move, output identical), an
alternating phase (one move per step, 98 blends against 1,049), jitter alone (no plan), and
selection mode and a 90 fps comb (never armed). The 59.95 Hz fixture is the one whose bounds
loosen: its worst run 44 to 49, at a half-comb step the lookahead does not catch, and one more
wrap.

### What the captures show

These numbers are replays under the step 1 rule of 326 minutes of gameplay: the four corpus
streams, the held-out 09-23 stream, and seven shorter KCD and Avatar fixtures. A phase step is
counted when the error has been within 1.5 ms for 30 presents and then sits 3 ms or more off the
comb, the same sign, on 7 of the next 10 presents.

- **Count:** 66 steps, all in the streams, 12 an hour. None in the shorter fixtures.
- **Size** falls into two groups:
  - 33 are 3 to 4.17 ms, under the passthrough threshold. The relay shows real frames up to 4 ms
    off time until the lock catches up, a median 105 presents. No blends.
  - 33 are 4.17 to 8 ms. The relay blends from the first present, 40 to 55 blends each.
- **Lock state:** it stays engaged through almost all of each step (median 96% of the event). In 18
  of the 33 large steps it releases briefly, about 9 presents in.
- **How they end:**
  - 37 end when the lock catches up, a median 105 presents later;
  - 29 end when the phase jumps back by itself, a median 44 presents later (about 0.7 s).
- **Cost:** phase steps produce 1,006 of the 8,571 blends in the stream replays (12%), about 205 an
  hour.
- **One pattern to handle:** on the 09-17 stream from 1116 s to 1225 s the phase alternates between
  +3.7 and -4.0 ms every 4 to 7 s, and the lock chases each one.

### The ring already knows

The lock reads one number per present: the distance from the target back to the frame before it.
That frame arrived a whole bracketing lag ago, 95.8 ms at the default 75 ms of extra lag. The ring
meanwhile holds every frame captured since, about six of them at 60 fps. So a phase step is in
the ring before the target gets there.

Measured on the 09-23 step at 1110.7 s, comparing the lock's error at the target with the median
phase of the ring's newest four real frames, both against the same target grid:

| time | error at the target | newest four frames | composite |
|---|---|---|---|
| 1110.546 s | +0.01 ms | -0.01 ms | real frame |
| 1110.580 s | +0.12 ms | -0.54 ms (first new frame) | real frame |
| 1110.613 s | +0.12 ms | -6.05 ms | real frame |
| 1110.680 s | -7.29 ms | -5.96 ms | blend, the first of 44 |

The newest frames showed the new phase four presents early and held it. The first new-phase frame
was in the ring six presents early.

### Behaviour

Watch the ring's newest frames. When they show a new phase that holds across several frames, the
relay knows where the step is: the stamp of the first new-phase frame. It moves the pull by the
step's size on the present where the target reaches that frame.

Both sides of the step are real frames, so every frame is shown once, with no blends and no
repeats. The one irregularity left is the game's own: a single frame interval that was short or
long by the step's size, which no relay can undo. A step that jumps back later gets the same
treatment in reverse.

This only works while the extra lag gives the ring frames beyond the target. At `-lag 0` the lag is
1.25 source periods and the ring sees about one frame ahead, so there the relay falls back to
reacting at the target: detect the step from the lock's own error over K presents, then move at the
recovery window's rate (comb/32 per present, 0.52 ms at 60 fps). A 6 ms step then costs about K + 4
blends against about 45 today.

### Design

- **Signal (lookahead).** The median phase, against the target grid, of the newest few real frames
  in the ring, after batch collapse and dejitter corrections. A step is that median sitting more
  than D from the lock's current phase, on the same side, for F consecutive new frames.
  - The median of several frames rejects a single late or early delivery.
  - Requiring F frames rejects a one-frame hitch.
- **Better signal, if the timing work allows.** The ETW flip history holds the game's own scanout
  times, known 5 to 10 ms after each flip (p50 5.3 ms), which is still about 85 ms ahead of the
  target. Flip times carry no delivery jitter. The capture stamps come first, because the ring is
  what the replay corpus models end to end.
- **Action.** Record the step as a planned move: its size, and the stamp of its first frame. When
  the target reaches that stamp, move the pull by the size in one present and re-seed `errEma` to
  the new phase, the way a stall resume does, so the loop does not fight the move.
- **Guards.**
  - Only with the lock engaged and outside a stall run.
  - Only where half a comb exceeds the passthrough threshold (a source near the sink rate), the same
    condition as the re-engage window.
  - A planned move cancels if the newest frames return to the old phase before the target reaches
    the step. A swap-back shorter than the lookahead then costs nothing at all.
- **Interplay.** The lock releases briefly in about half of the large steps today, 9 presents in.
  With the move made at the step, the error never builds up, so the release should not happen. The
  sweep checks that.

### Risks

- **Firing on a delivery burst.** NvFBC delivers in bursts, and the newest stamps are the least
  settled, since dejitter corrects them once their flips are known. The median, the F-frame rule and
  the steady fixtures (Avatar `_steady`, KCD walks, which must see zero detections) guard against
  this.
- **The alternating pattern.** On the 09-17 stream the phase alternates between +3.7 and -4.0 ms
  every 4 to 7 s. Moving at each step shows every real frame once, but the output's timing then
  follows the game's alternation. The sweep has to show that is no worse than today, where the lock
  chases each alternation for about 100 presents.
- **A step that is really a slow drift.** A drift never reads as a new phase that holds, so the
  lookahead leaves it to the normal slew.
- **Lesson from the re-engage window work:** rules keyed on elapsed time traded one fixture against
  another. The rule that won keyed on the error's own stability, and this one keys on the new
  phase's consistency across frames for the same reason.

### Measured before any code

- **Lookahead:** a scratch sweep of D in {3 ms, comb/6, the passthrough threshold} and F in {3, 4,
  5}, on the corpus and held-out logs. For each setting:
  - blends and repeats inside the 66 step events;
  - the total blend share, long runs, worst runs and stall re-seeds;
  - detections on the steady fixtures, which must be zero;
  - how early each step was seen, and how many planned moves cancelled.
- **The `-lag 0` fallback:** K in {3, 4, 6} on the fixtures captured without extra lag.

### Checks

- **Unit tests:**
  - a synthetic step that holds is met on the present the target reaches it: no blend, no
    repeat, every frame shown once;
  - a step that returns after 0.7 s is met both ways;
  - a step that returns inside the lookahead cancels and costs nothing;
  - the alternating ±4 ms pattern does not flap;
  - jitter and single late deliveries never fire;
  - at `-lag 0` the reactive fallback converges within a bound.
- **Corpus:** blends and repeats per step event fall, and nothing else gets worse. Bounds are re-set
  only with the reason in each fixture.
- **Field:** a stream on the new build. From the log, blend runs after phase steps, and the new
  planned-move lines (`rephase:` with size, lead time and whether it cancelled).
- **Undo:** D = 0 disables it. D is one constant.

## 5. Order and gates

1. **Step 1:** commit, CI, then a stream with the card on its custom 60 Hz mode. Wraps per hour
   from the log, against the 09-22 and 09-23 streams.
2. **Step 2:** the N sweep, then code, suite, corpus and held-out replays, CI, and a stream.
3. **Step 3:** the D and F lookahead sweep and the K fallback sweep, then the same sequence.

Where the output's repeats and skips come from, measured after step 2 on the four current-setup
streams (0917, 0917_1, 0922 and the held-out 0923, 3.75 hours, `b:vsync` at 75 ms of extra lag):

| source | per hour before easing | after easing |
|---|---|---|
| game stalls: held frames while the game rendered nothing (its flips stop) | 298 | 298 |
| game stalls: the jump when the game resumes | 131 | 132 |
| lock wraps | about 17 | about 1 |
| tooth guard, 98% within 20 presents of a stall resume | 16 | 16 |
| everything else | about 7 | about 5 |

About 49 stalls an hour, mostly 150 to 500 ms, produce 91% of it, and no relay can show frames a
game never rendered. The tooth guard's repeats extend a stall's freeze by one frame about one stall
in three; exempting the first present after a stall resume from the guard would blend there
instead, a gain too small to see next to the freeze. With step 2 in, the relay's own repeats and
skips outside stalls are about 7 an hour, so the next gain is step 3, which is about blends: the
40 to 180 present runs after phase steps that could be real frames. A wider passthrough threshold
inside ramps (5 ms) was also measured: the same repeats and skips removed, about 20 fewer blends an
hour, at the cost of real frames up to 6 ms off time. Step 2 keeps the normal threshold, because
once NVOFA replaces blends with interpolated frames a correctly timed frame beats a late real one.

Each step gets its own commit, its own field check and its own undo constant, so each effect can
be measured separately. The next stream is the held-out test for the step in flight, and it joins
the corpus afterwards if it covers something new. The 09-23 stream holds three of the clearest
large steps (1110.7 s, 1908.4 s, 1916.5 s), so it joins the corpus when step 3 starts, as design
data.

## 6. Decisions

1. Tooth guard: N at least 12 with the guard untouched. Taken: N = 12.
2. Easing in every mode, or blend modes only. Blend modes only: testing showed a ramp in
   selection mode turns each wrap's repeat into a repeat and a double advance.
3. Step 3 only for a source near the sink rate, like the re-engage window (recommended).
4. Step 3 reads the ring's capture stamps first, and the ETW flip times only if the stamps prove
   too noisy (recommended), or flip times from the start.
5. Step 2 before step 3 (recommended). After step 1, wraps still cost about 21 repeats or skips an
   hour on stream, the worst outcome, and easing turns each into blends. Phase steps cost about
   205 blends an hour, and step 3 turns those into real frames, a smaller gain by the rule in
   section 1. Both add a planned change to the target at a chosen present, so whichever lands first
   sets the pattern for the other.
6. The 09-23 stream joins the corpus when step 3 starts (recommended).
7. From step 1: tighten `max_pull_wraps` on `kcd_60x2_lockwrap_0923` from 3 to 1 and on
   `kcd_60x2_lockwrap_hitch_0923` from 6 to 4. Done with step 2.

## 7. Out of scope

- Optical-flow frames (NVOFA). They replace blends later without touching the timing in this spec.
- The forced wraps at 59.95 Hz. The fix is the card's custom 60 Hz mode. The 09-24 fixture only
  pins that the relay behaves sensibly there.
- Stall resumes. They keep the instant snap.
