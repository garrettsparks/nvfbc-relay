# Phasekeep Replay Harness Spec

Status: UNBUILT. This document is the work order for a new session.

## The two questions this exists to answer, in order

1. **THE FLOOR (answer this first, it can kill the project cheaply).** With PERFECT
   steering, how many holes per second does x3 phasekeep leave on a real capture? A hole is
   a source period with no real frame in the ring: a repeat in nearest mode, a blend in
   blend mode. The user's bar: **one blend every couple of seconds is acceptable; multiple
   per second is not.** If the oracle-steered floor on the real captures exceeds ~0.5/s of
   isolated holes, x3 is tabled and NOTHING ELSE IN THIS SPEC IS BUILT - the ceiling is
   NvFBC delivery (real frames that never arrived), which no relay code can fix.

2. **THE UPTIME FIX.** The live vote steered only 70% of batches (11508 of 16525 on
   `x3_phasekeep_survive_stalls.log`), and the 30% downtime puts ~18 generated frames per
   second into the ring - the reticle ghosting the user sees. The gap is 118 vote resets x
   a ~72-batch re-earn window. The harness must say WHICH resets those are (in-region grid
   flapping vs legitimate desktop/loading transitions vs something unmodelled), evaluate
   candidate fixes against the same capture, and validate the winner offline BEFORE any
   capture is recorded. Candidates, in rough order of likelihood:
   - debounce grid-change resets (require the new (stride, flipsPerSource) to persist N
     batches before resetting);
   - cache the verdict per grid and restore it when the same grid returns;
   - cheaper re-earn (lower kMinSamplesPerResidue - RISKY, see the null-margin note below).

## Why this harness and not another capture

Every capture this project records costs the user a session on the Windows rig, and this
week burned three of them on failures a replay would have caught: a vote that asked for
flip data before it was delivered (98.2% unplaceable), a class key that drifted from grid
position, and a reset storm from EMA pollution. Each was diagnosed OFFLINE afterwards from
the log in minutes. The harness moves that diagnosis before the capture instead of after.

## Inputs

The captures already on disk in /Users/gsparks/dev/frame-drop-analysis - no new recordings:

| log | why it matters |
|---|---|
| `x3_phasekeep_survive_stalls.log` | the CURRENT build's run: 70% steered, 118 resets, 326 reclaims. THE VALIDATION TARGET |
| `x3_phasekeep_fix_reverse.log` | the reset-storm run (127 steered, 224 resets): the harness must reproduce the collapse when configured with the old constants |
| `x3_phasekeep.log` | the ring-8 run (95% starved): must reproduce starvation with ringSlots=8 |
| `phase_aware_baseline_60x3.log`, `etw_x3_walk`, `join2_x3_on`, `dxgk_x3_120s` | more x3 grids for the floor measurement |
| every x2 log | null controls: the replayed vote must steer 0% on all of them |

Parse per line: `capture #N arr=<us> dt=<us>` (wakes) and
`flip disp=<us> evt=<us> lag=<us> head=0` (flips). **Flips enter the model when KNOWN
(evt+lag), never when they happened** - modelling display order instead of delivery order
is the exact mistake that cost the first capture.

## The model chain (replicate production, cite the file)

All from `samples/NvFBC/NvFBCR/`, sharing the real policy code by linking
`TemporalPolicy.cpp` - never reimplementing it:

1. **Batches**: wakes split at the 3 ms epsilon (`CaptureRing::CaptureLoop`,
   `policy::UpdateBatch`).
2. **Grid derivation**: clamped batch-period EMA (fold only gaps within half-to-double;
   see `m_rotPeriodEma` and WHY in the wake loop comment), `MedianFlipSpacing` over the
   200 ms window, stride/flipsPerSource by rounding. The clamp is load-bearing: without it
   154 stall gaps produced ~183 spurious resets.
3. **The vote**: 2-batch lag (`kVoteLagBatches` - a batch's own flips are ~6 ms
   undelivered at open), `PairBatchMember` for the anchor, `CountFlipsBetween` semantics
   (DISTINCT display times; a duplicate record rotates the mapping permanently),
   `RotationAdvance` (counted, bound 24), `RotationObserve`, `RotationPositionAt`
   (bounded extrapolation).
4. **Keeps**: `DecideKeep` with the per-REGIME stamp convention (spacing passed whenever
   period > 1, NEVER per-batch - mixing conventions was the backwards-video bug), the
   coalesced-single reclaim (steered, keeper==1, single-member -> re-validate at
   bs+spacing), and undecided-drop (vote valid, position lost -> keep nothing).
5. **Ring**: 16 slots, invalid slots consume space, window = newest 15 wakes.
6. **Selection**: `policy::SelectFrame` with production stickiness, target = present - lag
   (model the lock pull as a parameter; 20833+12000 reproduced the starved run's reach).
7. **An ORACLE mode**: steering forced correct on every batch, from ground truth computed
   the way `x3residue.py` does (anchor flip index mod 6, class alignment). This is the
   floor measurement: everything else identical, only the vote made perfect.

## Outputs, per run

- steering % and a timeline of unsteered stretches with their cause (reset id, re-earn
  window, placement failure) - the diagnosis for question 2;
- the pacing census (`pacing.py` in frame-drop-analysis prints the same table from a live
  log, so replayed and live runs are directly comparable): steps/s in the classes
  zero / flip-size / full-period / 1.5-2.5x / >42ms / backward;
- gen-frames-entering-ring per second and gen-shown per second;
- holes per second, split isolated vs the >42 ms stall class (the stall class runs 0.7/s
  at BASELINE TOO - it is not an x3 regression and does not count against the floor).

## Validation gates - the harness is not trusted until all pass

The sim-fidelity lesson was paid for repeatedly this week: a model validated only against
its own assumptions hides exactly the bugs it was built to find. Before any conclusion is
drawn from the harness:

1. Replaying `x3_phasekeep_survive_stalls.log` reproduces the live run within tolerance:
   steered 11508 (+-5%), resets 118 (+-10), reclaimed 326 (+-10%), undecided ~4, and the
   live pacing census within ~10% per class.
2. Replaying `x3_phasekeep_fix_reverse.log` WITH the old constants (unclamped EMA, advance
   bound 8) reproduces the collapse (~127 steered); with current constants it recovers.
3. Replaying `x3_phasekeep.log` with ringSlots=8 reproduces starvation (~95% no-before);
   with 16 it does not.
4. Every x2 log steers 0%.

Gate 1 is the hard one. If it cannot be met, whatever the model is missing is ITSELF the
next finding - do not loosen the tolerance to pass.

## Decision procedure

1. Build, validate (gates above).
2. Run the ORACLE floor on all x3 captures. Floor > ~0.5/s isolated holes -> **table x3**,
   write the number into the spec and memory, stop.
3. Floor acceptable -> diagnose the 118 resets, implement the winning uptime fix, validate
   offline: steered >= 95%, gens-entering-ring < 1/s, pacing census clean.
4. ONE confirmation capture. If it matches the replay, x3 ships behind `-phasekeep`; if it
   does not, the divergence is the next finding.

## Warnings for the builder (each of these cost real time this week)

- The vote's phase is ORIGIN-RELATIVE. Never compare absolute positions across a
  re-origin; compare the code-vs-truth OFFSET (see the Harness struct in
  `test_rotation_phase`).
- Tests must carry their own ground truth. Two generations of tests read expectations out
  of `p.gridPos` and were tautologies.
- Count flips, never divide time by spacing. The one bounded division that remains
  (`RotationPositionAt`) is bounded BECAUSE it divides.
- The corpus fixtures are single-head and their field numbers came from ring-8 builds
  (`kRingSlots` stays 8 in PolicyTests.cpp for that reason).
- `phasekeep summary` counts are whole-run; window before comparing against anything
  (the divergence-that-wasn't was a whole-run-vs-trimmed comparison).
- Reclaimed singles' pixels are ASSUMED real via the x2-proven coalesced mechanism,
  unverified at x3. If the confirmation capture still ghosts at ~1.7/s events, suspect
  the reclaims first; `-fgphase`-style gdiff on reclaimed slots would settle it.

## The dejitter overlay (added 2026-08-29)

The harness originally left the stage-6 overlay out, on the stated grounds that no capture it
replayed used `-dejit` and that the correction threshold sat below the log's resolution. That
was true when written and quietly stopped being true: every `-subgen` capture runs `-dejit`.

The cost was not the aggregate, which stayed inside the ~4% the harness claims. It was a blind
spot exactly where a bug shipped. `CaptureRing::FindBracket` corrected bracket endpoints and
skipped generated slots, so generated frames sat on the raw timeline while their neighbours
moved - and a model that ignores corrections on BOTH cannot see the difference. The same
omission hid a second defect in the harness itself: it published raw `beforeTs`/`afterTs`
where the relay publishes corrected ones.

It now feeds the REAL `policy::StampOverlay` from the log's own
`dejit: batch arr=<us> late by <us>, corrected` lines - dejitter replayed, not modelled -
inserted in present order, because the relay computes corrections DURING the present loop and a
present must not see one that did not exist yet. No new logging was needed; those lines have
been there since dejitter shipped.

    capture                    substitutions        blend class
    implied_diffmap (2.6%)     -7.0% -> 0.0%      +15.5% -> +0.1%
    improved_gen_selection     +1.4% -> +1.8%      +3.4% ->  0.0%
    subgen_kcd_0               +0.6% -> +0.3%      +1.7% -> -0.3%

**Read that agreement carefully: it is partly circular.** The harness now replays the relay's
own corrections instead of deriving them, so it can no longer check the correction
COMPUTATION, only its application. The independent evidence is a quantity the overlay was not
built from - the harness's bracket endpoint scored against the log's `before=` field - where
mean error fell 2320 us -> 171 us and exact matches rose 84.6% -> 97.0%. A fitted model would
not have moved that.

Consequences for anyone using it:

- Captures without `-dejit` are unaffected: output is byte-identical to the pre-overlay build
  on `dxgk_x1`, `dxgk_x2` and the x3 fixture.
- **`--ring 32` is mandatory on any `-lag 75` capture.** Ring size is not recorded in the log,
  the harness defaults to 16, and a 32-slot capture replayed at 16 reports thousands of holds
  the run never had. The header prints the ring for this reason.
- The next agreement should be scored BLIND on a capture the model has never seen, because
  every number above is on captures it was tuned against.
