# Investigation: what actually causes the residual long blend runs

Status: five candidate rules measured dead; a SIXTH, step re-seed, is IMPLEMENTED IN THE REPO and
green. It removes four long runs across the corpus with zero fixtures worse, and the 59:60 sweep rows
never fire. It is uncommitted and unblessed: no hardware has run it. See the last two sections, which
supersede an earlier "two operating points" tradeoff that turned out to be an artifact of a bug in
the first implementation rather than a property of the rule. Opened 2026-09-17 as "item 5, phase-step snapping in
the lock", on the strength of four blend runs of 50+ presents in the 52.8 minute field capture
`Get_Medieval_2026-09-16_0`. The original framing was wrong in three separate ways, each killed by
measurement, and the mechanism that survived suggests a different fix than the one the item named.

Material: `Get_Medieval_2026-09-16_0.log` (this build), `Get_Medieval_2026-09-13_0.log` (pre-window,
same present path), `Get_Medieval_2026-09-14_0.log` (`b:dwm`), `kcd2_b_vsync.log`. Instruments:
`frame-drop-analysis/zonecross.py` (classifies each blend run) and `frame-drop-analysis/locksim.cpp`
(drives the REAL `UpdatePhaseLock` over a log's own error sequence and exposes `devEmaQpc`, which no
log records).

**The instrument's own control.** `locksim` predicts the lock's engage flag from the logged error
sequence alone and compares it against the logged `lk=`: 99.64% agreement on 09-16, 99.48% on 09-13,
98.59% on 09-14, 97.75% on KCD. The residual is stall-resume presents, where production passes
`resumedFromStall=true` and the instrument does not. Everything below rests on that agreement.

## What was ruled out

| hypothesis | measurement | verdict |
|---|---|---|
| the target crossing the zone while engaged (the item's premise) | `zonecross.py`: engaged crossings are 24 runs / 251 presents = 0.13% of presents on 09-16, and only 1 of the 12 longest runs. On KCD, zero | DEAD: the long runs are not this |
| a snap to the nearer real frame | worst distance to a real frame inside the long runs is 0.48-0.50 of a source period on nearly every run, so a snap must move content by up to 8.3 ms, making one present advance 8.3 or 25 ms instead of 16.7, and repeat about every 45 presents as the phase drifts back | DEAD: trades a ghost for a recurring cadence break |
| marginal flapping a Schmitt band would hold | every lock release in all four logs had `devEma` below comb/6 (53/53, 145/145, 45/45, 12/12), but the episodes a comb/6 release threshold would ELIMINATE are the short ones: 28 of 53 episodes covering 300 of 2652 presents on 09-16, touching 6.8% of long-run presents (9.0% on 09-13, 7.5% on 09-14, 0% on KCD) | DEAD as a cure for the long runs; a 0.16% tidy-up at best |
| a dejit feedback loop (jitter releases the lock, dejit needs lock state, declines, jitter persists) | 20 of the 21 `lock-declined` verdicts on 09-16 are at 2.8-3.1 s (startup acquisition); exactly one (2452.8 s) falls inside a long run, and dejit corrected 1148 of 1169 late batches | DEAD |
| source delivery jitter | arrival-gap p10-p90 inside the 28 long runs against the log's global spread of 1304 us: runs land both above (2184, 2264, 2366 us) and well below (394, 625, 818, 940 us). The batch COUNT is 60.00/s inside the long runs, same as a control minute | DEAD: not a discriminator |
| source scanout jitter | ETW head-0 flip spread: 738 us inside the longest run against 711, 919 and 2458 us in three controls. The widest control has no long run; the tightest-jitter run is the longest one. (Head 0 flips at 8.34 ms, so it is the source display at 120 Hz, not the game's frame pacing) | DEAD |

## The mechanism that survived

Traced present by present through the three long gameplay runs on 09-16, and consistent in all of
them:

1. The lock is engaged and healthy, `devEma` around 260-375 us, which is comb/50 or better (steady
   state on this build is comb/97, p50 172 us).
2. A PHASE STEP arrives. `devEma` climbs from ~300 to ~3100 us in about 18 presents. Blending starts
   while the lock is still engaged.
3. `devEma` passes the stability gate at comb/8 (2083 us) and the lock releases.
4. While disengaged `want = 0`, so the pull decays AWAY from the phase it was holding, at the
   steady 25 us/present. Measured excursions on this build: 400 to 4245 us. On the pre-window build
   the same excursions ran 9180 to 14946 us, i.e. most of a comb.
5. Blending continues for as long as the target sits off the real frames, which is set by how long
   `devEma` takes to decay back under the gate: an EMA time constant, not anything about the pull.
6. The lock re-engages, and once `devEma` falls under comb/16 the confirmed convergence window arms
   and returns the pull at comb/32 (521 us/present), which is what ends the run.

So the run length is governed by the deviation EMA's decay after a step, and the pull excursion is a
round trip it did not need to make. The re-engage window already cut both hard (excursions 9-15 ms
to 0.4-4.2 ms, worst run 226 to 65); what remains is the decay time.

A second, separate class exists and must not be touched: runs where the lock is long since out and
the pull is parked at 0, with the source off the declared rate. That is all of KCD's long runs and
the pre-recording startup stretch on 09-16 (44.8 s, 40.6 s). There blending is the correct output of
rate conversion, and any rule that fires there is a regression.

## The candidate this points at

A phase step is a stall resume without the stall, and the code already has the machinery: on
`resumedFromStall` the policy re-seeds `errEmaQpc` from the fresh error, which drives `devEma` to
zero "so the lock stays engaged through the resume instead of flapping", and opens the convergence
window. Applying the same treatment to a detected step would collapse the deviation instead of
waiting out its decay, hold the lock engaged, and converge the pull at 521 us/present, which covers
a half-comb step in about 16 presents with every content step within 3% of nominal.

The discriminator is the whole risk, and the earlier six-rule sweep already found that freezing the
pull while disengaged "traded one set of long runs for another". A step leaves the error at a NEW
CONSTANT offset; a sweep keeps moving. So the rule has to require that the error has settled at its
new offset before re-seeding, which is the same shape of confirmation the re-engage window uses
(`kEngageStableDiv`), not an elapsed-time test.

## Kill criteria, to be applied in replay before any capture

1. Long runs must fall on the Get Medieval logs (worst run and count of runs >= 50).
2. Total blend share must not rise on any fixture or log.
3. The 59:60 rows must not move: `avatar_59x1_vsync_*`, `avatar_59at60_vsync_*`, `avatar_59x2_vsync_*`
   run the genuine sweep at 44-45% synth and carry `min_synth_pct` floors. A rule that fires through
   a sweep breaks them.
4. KCD's long runs (lock out, pull parked at 0) must be unchanged.
5. No new content-step irregularity: inside the current long runs 76 of 77 steps sit within
   15.0-18.5 ms, and that must survive.
6. Re-seed counts must not rise materially on any fixture.

If it fails any of these in replay, it dies there and costs no hardware time.

## The A/B that closed it: freeze the pull while disengaged

Built as a one-line scratch variant (`want = s.engaged ? pull + errEma : pull`, against production's
`: 0`), compiled with a scratch copy of `PolicyTests.cpp` so the quoted include picks the scratch
header, and run from the repository root so the real 43-fixture corpus replays. Baseline passes; the
variant fails 37 assertions and is worse where it matters:

| fixture | baseline | variant |
|---|---|---|
| KCD 60x2 map cycles | synth 3.1%, runs>=50 2, worst 68 | synth 3.5%, runs>=50 4, worst 66 |
| KCD 60x2 map cycles, post-convergence-window | synth 3.4%, runs>=50 0, worst 34 | synth 3.6%, runs>=50 1, worst 59 |
| KCD 60x2 join events | synth 4.9%, runs>=50 0, worst 49 | synth 5.1%, runs>=50 1, worst 73 |
| KCD 60x1 FG off | synth 6.2%, runs>=50 1, worst 65 | synth 6.2%, runs>=50 1, worst 72 |
| Get Medieval gameplay hour | synth 0.5%, runs>=50 1, worst 50 | synth 0.5%, runs>=50 2, worst 52 |
| Avatar 60x2 vsync bench | synth 1.7%, runs>=50 1, worst 55 | synth 1.9%, runs>=50 1, worst 73 |
| Avatar 60x1 vsync bench | synth 2.8%, runs>=50 2, worst 143 | synth 3.6%, runs>=50 3, worst 143 |
| the 59:60 rows (six fixtures) | 43.5-45.1% synth | 43.7-45.3% synth, unchanged in kind |
| KCD 60x3 walk | synth 16.2%, worst 46 | synth 8.9%, worst 44 |

The 59:60 rows staying put is the one good sign: the rule does not misfire in the sweep regime it
must not touch. But the disturbance response gets worse, and `locksim` shows why: with the pull
frozen it can sit most of a comb from the phase when the lock re-engages, so the excursions GREW
rather than shrank (the 1898.7 s run on 09-16: 3860 us baseline, 12551 us variant; the 226-present
run on 09-13: 10920 to 14606 us). One fixture improves (KCD x3) and it does not buy the rest.

This is the same outcome the earlier six-rule sweep recorded for freezing ("traded one set of long
runs for another"), now reconfirmed WITH the convergence window in place, which was the one reason to
think it might land differently.

## Verdict

| candidate | why it is dead |
|---|---|
| snap to the nearer real frame | needs up to 8.3 ms of content jump, recurring roughly every 45 presents |
| faster slew while engaged | the engaged-crossing class is 0.13% of presents (09-16), 0.35% (09-13), 0% on KCD |
| Schmitt release band at comb/6 | eliminates only short episodes: 6.8% / 9.0% / 7.5% / 0% of long-run presents across the four logs |
| dejit feedback loop | 20 of 21 lock-declines are startup acquisition; dejit corrected 1148 of 1169 late batches |
| freeze the pull while disengaged | 37 suite failures, worse on seven fixtures, excursions grew (above) |

What remains untested is collapsing the deviation on a DETECTED step (re-seeding `errEmaQpc` the way
`resumedFromStall` does). It is not obviously safe: a step and a sweep both leave the error away from
its EMA, the discriminator would have to be that the error settles at a new constant offset, and
every cousin of that rule has failed. It is not worth a session on the current evidence, because the
artifact it would shorten is ~1 second of ghosting with cadence intact, four times an hour, and the
thing that actually removes that artifact is NVOFA.

The release proceeds on the build validated in `docs/capture-and-lock-field-validation.md`
regardless: nothing below has been applied to the repo.

## The sixth candidate: step re-seed, built and measured 2026-09-17

The one idea the verdict above left open, implemented after the user asked for a pass at it. Patch
kept at `frame-drop-analysis/step-reseed-experiment.patch` (against `79f33e9`); the scratch tree it
was built in does not survive the session.

**The rule.** A phase step leaves the error at a new constant offset that the /16 estimator needs a
dozen presents to follow, and the deviation it reports meanwhile releases the lock. So: after a
stretch of quiet engaged presents, if the error sits at least comb/8 from the estimator, open a
candidacy; if the error is still off the real frame and has stopped moving monotonically for four
presents, re-seed the estimator exactly as `resumedFromStall` does, which drops the deviation to zero,
keeps the lock engaged, and lets the existing convergence window close the offset at comb/32.

**What the measurements forced, in order.** Each of these was a failure first:

1. Direction alone is not a discriminator: a jittering sweep flips direction constantly, so the first
   version fired in the 59:60 regime, cost those rows 3 to 20 points of synth share, grew up to 11
   long runs, and failed 41 assertions.
2. Requiring a quiet ENGAGED stretch first fixed that completely (those rows now fire zero times),
   because at 59:60 no comb holds and engagement never persists. 41 failures became 3.
3. Arming on the smoothed deviation is impossible alongside that gate: the gate needs the deviation
   low, so arming at comb/8 made the rule inert on every fixture. Arming and confirming had to be
   separated into a candidacy that survives the gate's reset.
4. A guess that the remaining regression came from stale history across a map-open stall was WRONG:
   adding that guard changed exactly one unrelated fixture by 0.2 points and left the regression
   untouched.

**Corpus result** at STEP_STABLE=60, STEP_CONFIRM=4, STEP_CAND_MAX=24. 21 fires over 43 fixtures:

| fixture | fires | effect |
|---|---|---|
| `gm_60x2_gameplay_hour` | 10 | runs >= 50: 1 -> 0, worst 50 -> 45, synth 0.5 -> 0.4% |
| `avatar_60x2_vsync_steady` | 2 | runs >= 50: 1 -> 0, worst 55 -> 23, synth 1.7 -> 1.1% |
| `avatar_60x1_vsync_bench` | 4 | runs >= 50: 2 -> 1, synth 2.8 -> 1.9% |
| `avatar_60x3_vsync_bench` | 2 | runs >= 50: 1 -> 0, worst 74 -> 26 |
| `avatar_60x2_vsync_bench`, `avatar_60x2_subgen_*` | 1 each | no change to runs or worst |
| the eight 59:60 rows | 0 | untouched, which is the point |
| `kcd_60x2_mapcycles_postfix` | 1 | runs >= 50: 0 -> 1, worst 34 -> 54. THE REGRESSION |
| the other 36 fixtures | 0 | unchanged |

**The open question is that single fire.** It fails two assertions, and those bounds are pinned to
the fixture's own field behaviour (`max_worst_run 34` against `field_worst_run 34`,
`max_long_runs 0`), so there is no slack by construction. The fixture's source log (KCD map cycles,
2026-08-01) is no longer on disk, so the event cannot be inspected and a targeted guard would be a
guess: one guess has already been made and measured wrong (point 4 above).

### The sensitivity sweep, and why there are exactly two operating points

Neither timing knob moved the misfire (quiet-stretch 60 or 120, confirm delay 2, 4 or 8), so the two
SENSITIVITY knobs were swept: how far the error must sit from the estimator to qualify, and how much
monotonic movement is still called settled.

| error floor | direction tolerance | failures | fires | wins kept |
|---|---|---|---|---|
| comb/8 | 3 | 2 | 21 | all four (GM hour, Avatar 60x2 steady 55 -> 23, 60x1, 60x3 74 -> 26) |
| comb/6 | 3 | 2 | 19 | all four |
| comb/5 | 3 | 2 | 18 | all four |
| comb/5 | 2 or 1 | 2 | 17 | three (60x3 lost) |
| comb/8 | 1 | 2 | 20 | all four |
| **comb/4** | **3 or 1** | **0** | **11** | **two: GM hour 50 -> 44 and runs 1 -> 0, Avatar 60x1 runs 2 -> 1** |

**The misfire and two of the four wins die at the same threshold**, somewhere between comb/5 and
comb/4 (3.3 to 4.2 ms of error). The map-cycle event is the same SIZE as the disturbances worth
catching, so this knob cannot separate them, and the direction tolerance barely matters. All eight
59:60 sweep rows fire zero times at every point tested, so the regime gate holds throughout.

At the green point the rule still fires on four fixtures (6 on the Get Medieval hour, 3 on Avatar
60x1, 1 each on Avatar 60x2 steady and 60x3) and lowers synth share on all of them, but only removes
long runs on the first two.

### The confirmation was cancelling itself, and fixing it removed the tradeoff

Everything in the table above was measured with a bug. The confirmation re-tested the ARMING
condition every present: the error had to stay at least comb/kStepErrFloorDiv from the estimator for
the whole four-present wait. But the estimator closes on the error at 1/16 a present, so the
deviation decays as `D * (15/16)^n`. At a comb/4 floor that means only a step above 5754 us can still
be far enough after four presents, against 8334 us for the largest error a comb can hold: the top 31%
of the possible range, and the candidacy was abandoned on the very present it would have fired.

Traced present by present on a synthetic 6000 us step, which is why this was found at all: the
candidacy opened, counted 0, 1, 2, 3, 4, and died at the next present with the instantaneous deviation
at 4236 falling to 3947 against the 4166 floor. `monoRun` sat at 1 the whole time, so the direction
test was never the problem.

The fix is one line: the floor decides whether a disturbance is worth re-seeding when the candidacy
OPENS, and the confirmation then tests only whether the TARGET is still off a real frame. With the two
gates no longer fighting, the floor comparison inverts:

| floor | failures | fires | result |
|---|---|---|---|
| **comb/4** | **0** | **19** | GM hour runs 1 -> 0 worst 50 -> 45; Avatar 60x2 steady 1 -> 0 worst 55 -> 23; Avatar 60x1 2 -> 1; Avatar 60x3 1 -> 0 worst 74 -> 26. **Zero fixtures worse** |
| comb/8 | 5 | 27 | the same four wins plus one trivial gain, but the map-cycle regression returns (worst 34 -> 54) and the dejit-event fixture regresses (59 -> 62) |

So comb/4 is not a compromise: every win comb/8 buys is already there, without the harm. The earlier
question "how would we decide whether 8 is better" is answered without needing the KCD capture.

### What is in the repo now

Committed as `3fe2ace`: `TemporalPolicy.h` (the state fields), `TemporalPolicy.cpp` (the rule and its
constants at comb/4) and `PolicyTests.cpp` (`test_lock_step_reseed`), 166 lines. Uncommitted on top:
21 lines of log evidence across `TemporalPolicy.h`, `.cpp` and `TemporalCaptureMode.h`, `.cpp`.

**The rule logs itself**, because a policy change that silently alters the lock's mind cannot be
validated from a capture, and inferring fires from how fast the pull moved afterwards is inference
rather than evidence. Each confirmed re-seed writes `lock: step re-seed, phase error <n>us corrected
at pull=<n>us`, and the exit summary carries `lock summary: <n> confirmed phase steps re-seeded the
estimator`. That is what makes a blessing capture readable: fires can be counted, placed in time, and
checked against the blend runs at those moments.
The suite passes 43 fixtures with `-Werror`, and the new test's four cases pass: a settled step fires
exactly once and reaches the passthrough threshold at present 8 where the steady slew needs 73, while
a monotonic sweep, a one-present spike, and a step arriving before the quiet engaged stretch is earned
all fire zero times.

**It is not blessed.** Every number is replay. Before it ships it needs a CI build and a field
capture, and that capture invalidates nothing already recorded: `docs/capture-and-lock-field-validation.md`
describes `79f33e9`, which is still what the repo would tag if this change is dropped or deferred.

### Superseded: two ways forward, when the rule still had a misfire

1. **The green point (comb/4).** Suite passes with no bound moved and no new capture needed. It
   removes a long run on the 69-minute Get Medieval capture (worst 50 -> 44) and one of two on Avatar
   60x1, and trims synth share on four fixtures. Modest, and it ships clean.
2. **The bigger win (comb/8 or comb/5)**, which additionally removes Avatar 60x2 steady's 55-present
   run (-> 23) and Avatar 60x3's 74 (-> 26), but needs either one fixture's bound moved on the user's
   sign-off with the reason written in (`kcd_60x2_mapcycles_postfix`: worst 34 -> 54, long runs
   0 -> 1) or a fresh KCD map-cycle capture so the misfire can be inspected and guarded precisely.
   That capture is about four minutes of map open/close cycles.

Either way the remaining work to ship is the same: apply to the repo, write the rule's own suite test
with near and flapping controls (the shape `test_lock_engage_window` uses), a CI build, and a field
capture, because every number here is replay. Parking it is also legitimate: the artifact is about a
second of ghosting roughly four times an hour, and the user's own viewing found one of three runs
noticeable and "not awful", the other two invisible or explained by a source stall.
