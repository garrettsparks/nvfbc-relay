# The 1.5x passthrough threshold: field validation

Captures 2026-09-09, analysis 2026-09-10. Relay build 1c81db3 on `etw-frame-timing`, the
first build carrying `policy::PassthroughThreshold`. Two Avatar captures, `b:flip -src 90 -lock
-lag 75 -mark -etw -dejit`, three benchmark runs each (nine tests), one without frame
generation (90x1) and one with frame generation x2 (90x2). Compared against the two captures
taken 2026-09-06 on the previous build (threshold 4166 us), same flags, same benchmark.

## The question

At 90 fps into a 60 Hz sink the target alternates between a real frame and the midpoint of the
next pair. The ideal output is a strict real / half-blend alternation with every content step
1.5 source periods. On the old rule the threshold floored at a quarter of the PRESENT period
(4166 us, widened to 5166 us by the hysteresis band) and 41% of midpoint presents passed
through it, each pass a 22 ms then 11 ms content step. The new rule sizes a source between one
and two times the present rate at a quarter of its OWN period (2777 us, widened to 3471 us),
so a midpoint present 5555 us from either neighbour always blends. The replay predicted 90x1
going from 29.1% to about 49% synth with lock engagement unchanged. Does the field agree?

## Method

Every number below is windowed to the nine benchmark tests. Fades between tests are runs of
tiny packets in the recording; each fade maps through the burned-in marker counter to a log
present and so to log time, and a test is the span between two fades, shrunk by 300 ms at each
end and restricted to presents where the median batch period over the trailing 500 ms sits
within 20% of the declared 11111 us (which removes the loading screens between bench runs).
The span before the first fade is the game running before the recording began and is
excluded. Batches, not arrivals: the 90x2 source delivers frame-generation pairs a few hundred
microseconds apart, and an arrival-gap median lands on the pair boundary and fragments every
test.

Per present, from the log: the op (pass-before, pass-after, synth, hold), the bracket weight
w, the blend weight, and the lock flag. Content time is the before frame for pass-before, the
after frame for pass-after, the target for synth, and unchanged for a hold; a content step is
the difference between consecutive content times in source periods. A midpoint present has w
in [0.35, 0.65]. Step deviation is the rms of (step minus 1.5) in milliseconds.

From the recording: `marker.py decode` on every frame, cross-checked against the log
(counter repeats are downstream duplication, counter skips are presents that never reached the
file, and the interp flag and weight cells are compared with the log's op and blend weight).

Source rate from the benchmark CSVs' CPU time, not the Fps column (which floors).

## 90x1: source without frame generation

Source held 90.0 fps on all six runs (CPU time 11.11 ms), benchmark score 3400 on all six.

| nine tests, 17.5k presents each | old (4166 us) | new (2777 us) |
|---|---|---|
| synth share | 28.0% | 48.4% |
| midpoint presents that blended | 59.2% | 99.8% |
| content steps at 1.0 / 1.5 / 2.0 periods | 21.8 / 56.2 / 21.9% | 1.9 / 96.0 / 2.0% |
| steps within 0.1 period of 1.5 | 54.1% | 85.3% |
| step deviation from ideal, rms | 3.51 ms | 1.15 ms |
| lock engaged | 89.0% | 88.1% |
| consecutive-pass runs (RR+) | 1774 | 212 |
| consecutive-blend runs (BB+) | 17 | 88 |
| blend weights in [0.45, 0.55] | 96.5% | 72.8% |
| marker: counter repeats / skips | 0 / 0 | 1 / 0 |
| marker vs log provenance agreement | 100% | 100% |

Per test on the new build:

| test | presents | synth | midpoints blended | steps at 1.5 | within 0.1 | rms ms | lock | RR+ | BB+ |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1833 | 49.4% | 99.8% | 98.8% | 93.0% | 0.85 | 99.0% | 11 | 2 |
| 2 | 1900 | 47.7% | 99.5% | 94.5% | 78.7% | 1.39 | 78.3% | 34 | 10 |
| 3 | 1836 | 49.2% | 99.9% | 98.1% | 90.8% | 0.86 | 91.6% | 15 | 6 |
| 4 | 2225 | 48.0% | 99.9% | 94.3% | 84.7% | 1.40 | 90.2% | 28 | 19 |
| 5 | 1918 | 47.8% | 100.0% | 94.7% | 75.1% | 1.47 | 75.8% | 32 | 8 |
| 6 | 1845 | 49.2% | 99.9% | 98.6% | 92.4% | 0.75 | 93.5% | 14 | 3 |
| 7 | 2250 | 49.6% | 99.9% | 97.7% | 87.2% | 1.03 | 93.0% | 22 | 18 |
| 8 | 1911 | 45.7% | 99.6% | 89.8% | 74.9% | 1.78 | 80.3% | 42 | 12 |
| 9 | 1835 | 49.4% | 100.0% | 98.3% | 92.0% | 0.81 | 91.4% | 12 | 6 |

The op pattern inside every test reads `RBRBRBRB...` for as far as the sample runs. On the old
build the same sample read `RRRBRRRBRRRRRBRBRB...`.

Reading the table:

- **The midpoint question is closed.** 99.8% of midpoint presents blend. The 15 that passed in
  20k presents sat at 3.1 to 6.2 ms from the nearest frame, which is the lock re-phasing after
  a hitch, not the steady state.
- **The blend weights spread because the gate widened, not because the lock loosened.** On
  the old build only midpoints within about 0.035 of w = 0.5 blended, so the weights that
  survived were all near 0.5; the off-centre midpoints passed instead. Now every midpoint
  blends, including the ones the lock parked at w = 0.40 or 0.60, so the weight histogram
  shows the lock's phase wobble for the first time. The median is 0.500 on both builds.
- **Step deviation tracks lock engagement, test by test.** Tests at 91 to 99% engagement sit
  at 0.75 to 0.86 ms rms; tests at 76 to 80% sit at 1.4 to 1.8 ms. A blend places content at
  the exact target instant, so its step error is the phase error of the target, which is what
  the lock controls. This is now the limiting factor at 3:2, and it is a property of the comb
  lock, not the threshold.
- **The one marker repeat is downstream, and the relay's own count is zero.** The counter
  is burned once per present and never presented twice, so a repeated counter in the file
  can only be the recorder chain copying a frame; the content check on it guards against a
  misread, it is not the attribution. The relay's way of repeating is a fresh counter over
  unchanged content (a hold, or a pass that re-selects the frame just shown), and that is
  counted from the log directly: 0 such presents in 17553 across the nine tests, on both
  the new and the old capture. The recording adds one more case the log cannot see: one
  pair of consecutive presents 11 seconds in showed the same picture on two DIFFERENT
  captured frames 14.7 ms apart, a game-side repeated frame at a hitch (see the section on
  the marked video below). One chain duplicate and one source repeat in five minutes.

## 90x2: source with frame generation x2

The base cannot hold 90 here. Real frames arrived at 88.3 fps on the new capture and 88.7 fps
on the old (CPU time 11.33 vs 11.27 ms); scores 5002 to 5003 against 5021 to 5032, half a
percent apart. The comb follows this source poorly on both captures, and the two captures'
engagement differs by the source's wobble on the day, not by anything the relay did.

| nine tests, 17.5k presents each | old (4166 us) | new (2777 us) |
|---|---|---|
| synth share | 14.1% | 43.4% |
| midpoint presents that blended | 38.8% | 98.8% |
| content steps at 1.0 / 1.5 / 2.0 periods | 37.2 / 28.6 / 33.8% | 9.9 / 82.4 / 7.6% |
| steps within 0.1 period of 1.5 | 23.0% | 51.2% |
| step deviation from ideal, rms | 4.62 ms | 2.32 ms |
| lock engaged | 41.9% | 35.0% |
| consecutive-pass runs (RR+) | 1303 | 1096 |
| consecutive-blend runs (BB+) | 27 | 491 |
| blend weights in [0.45, 0.55] | 86.5% | 31.7% |
| marker: counter repeats / skips | 0 / 0 | 0 / 0 |
| marker vs log provenance agreement | 100% | 100% |

Per test on the new build:

| test | presents | synth | midpoints blended | steps at 1.5 | within 0.1 | rms ms | lock | RR+ | BB+ |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1805 | 45.5% | 99.5% | 83.8% | 51.1% | 2.18 | 23.2% | 159 | 97 |
| 2 | 1894 | 45.5% | 98.3% | 89.0% | 62.3% | 1.90 | 50.7% | 70 | 20 |
| 3 | 1838 | 41.1% | 97.4% | 79.2% | 44.0% | 2.61 | 27.1% | 109 | 39 |
| 4 | 2292 | 44.9% | 99.4% | 82.9% | 53.2% | 2.20 | 28.0% | 188 | 102 |
| 5 | 1880 | 42.7% | 99.4% | 82.5% | 46.7% | 2.40 | 34.5% | 91 | 29 |
| 6 | 1838 | 40.5% | 99.2% | 76.9% | 44.5% | 2.68 | 34.1% | 112 | 40 |
| 7 | 2456 | 44.3% | 99.3% | 82.8% | 49.7% | 2.29 | 35.5% | 171 | 102 |
| 8 | 1886 | 43.3% | 97.9% | 83.8% | 62.9% | 2.20 | 45.7% | 83 | 28 |
| 9 | 1846 | 41.7% | 99.0% | 80.4% | 46.3% | 2.47 | 37.2% | 111 | 33 |

Reading the table:

- **The threshold effect is separated from the lock effect, which is what this pair was
  captured to do.** Midpoints blended went from 39% to 99% while lock engagement went DOWN
  (42% to 35%, the source's doing). On the old build a wandering phase landed in the narrow
  blend band even less often than an alternating one; the new band spans w in [0.31, 0.69]
  and catches it.
- **The remaining step spread is the source's wobble, not quantization.** On the old build
  the steps sat in three spikes at 1.0, 1.5 and 2.0 periods (a pass snaps content to a real
  frame). Now 82% sit at 1.5 and the rest are spread continuously around it, because a blend
  at w = 0.35 followed by a pass makes steps of 1.35 and 1.65. The rms fell by half, from 4.6
  to 2.3 ms; the 90x1 figure of 1.15 ms is the floor a comb that could follow this source
  would reach.
- **BB+ runs rose from 27 to 491.** Two blends in a row happen when the phase drifts past a
  real frame between presents; on the old build that present passed instead. Both frames
  land at their target instants, so this is not a pacing defect, but it is more blended
  frames back to back. A 50/50 blend is a double image whose two copies sit one source
  period of motion apart (11 ms worth, two thirds of the 16.7 ms the content moves between
  output frames). At small displacements that reads as motion blur; at large ones it is a
  doubled edge on every other frame, a 30 Hz alternation the eye can read as judder even
  though the content placement is exact. The relay's own same-content presents inside the
  tests are 0 on both captures.

## What the marked video found that the log could not

The marker counter advances once per present, so the recording itself can attribute a
repeated picture: identical content under a repeated counter is the chain copying a frame,
identical content under an advancing counter is two presents that showed the same picture.
The log cannot see the second case when the two presents showed two different captured
frames, because it reasons in timestamps and the timestamps advanced.

Method: decode the recording once at reduced resolution with the marker strip cropped off,
compute the mean absolute difference between every adjacent pair of frames, and flag a pair
whose difference is under a tenth of the median over its 30 neighbours while that
neighbourhood is moving. The relative test is essential: 2894 pairs in the 90x1 recording are
pixel-identical in absolute terms, and nearly all of them sit in static scenes where the
encoder quantized a small blend difference to nothing. Fades are excluded for the same reason.

| recording | same-picture pairs, counter advanced | counter repeated | per minute of tests |
|---|---|---|---|
| 90x1 new | 1 | 1 | 0.2 |
| 90x1 old | 7 | 0 | 1.4 |
| 90x2 new | 47 | 0 | 9.7 |
| 90x2 old | 193 | 0 | 40 |

In the 90x2 new recording every flagged pair is two different captured frames, each the first
member of its batch, one source period apart (23 of the first 30 at 11 ms, the rest 12 to 18
ms), both passed as real frames. The game delivered the same picture on two consecutive real
frames; the relay honoured it. Without frame generation (90x1) this happens about once in
five minutes; with frame generation x2 at a base that cannot hold 90 it happens every six
seconds on the new build and four times as often on the old. The relay's own contribution,
a hold or a re-selected frame, is zero on all four recordings. Why the old build's recording
shows four times as many is not settled: strict alternation on the new build blends a
duplicated frame with its neighbour half the time, which changes the picture and hides the
repeat, and the old 90x2 log also carries 85 pass-after to pass-before transitions whose two
frames sit only 6 to 9 ms apart (none in the new), which reads as pair members admitted as
frames of their own. Both are hypotheses.

Each such pair is a 16.7 ms hold in the output, a visible judder event that no log metric in
this document counts. The tool is `picturerepeats.py` in the analysis repository.

## The replay agrees with the field, and the corpus now describes the shipping relay

The four fixtures `avatar_90x1_vsync_{steady,bench}` and `avatar_90x2_vsync_{steady,bench}`
were rebuilt from the new captures at the test windows above (steady = the first benchmark
test with no fade at either end; bench = first test start to last test end, fades kept). The
replay lands on the field on every one:

| fixture | window | replay synth / worst | field synth / worst |
|---|---|---|---|
| 90x1 steady | 38.7 to 69.3 s, 1833 presents | 49.0% / 3 | 49.4% / 2 |
| 90x1 bench | 38.7 to 340.5 s, 18107 presents | 48.4% / 5 | 48.4% / 7 |
| 90x2 steady | 43.0 to 73.1 s, 1805 presents | 44.1% / 4 | 45.2% / 5 |
| 90x2 bench | 43.0 to 347.3 s, 18256 presents | 44.6% / 8 | 43.3% / 7 |

Each carries a `min_synth_pct` floor so a return to passing midpoints fails the suite, and the
90x2 pair carries `min_placed_pct 92` because one capture in fourteen on that source has no
flip to anchor to (a property of the source; 90x1 anchors 99.5%). The other 35 fixtures print
identical lines before and after the rebuild.

## What the rule does elsewhere

Driving the real composite decision across source rates on an exact locked lattice: the rule
changes nothing at or below 1x or at 2x and above. Between 1x and 2x it is never worse than
the old rule. Ratios whose comb is M=2 or M=3 (80, 90, 100 into 60) reach exactly uniform
motion; M=4 ratios (75, 105 into 60) are half solved because the quarter-period threshold
equals the quarter-phase lattice distance, so one of the two off-centre presents still passes
after a pass; near-integer ratios (61, 118, 119) blend through the periodic skip and their
worst placement error falls from a source period to about a quarter of one. On the exact
lattice at 90 both rules already produce strict alternation, because a true midpoint sits
outside even the old widened gate; what the field needed was margin against the lock parking
the phase off centre, and the margin went from 389 us to 2084 us.

## What this changes downstream

- At 3:2 the placement question is answered by blending; the remaining step error is the
  lock's phase error and is under 1 ms rms when the lock holds. What remains visible is the
  blend frame's content: a double image separated by one source period of motion, which
  reads as blur when the scene moves slowly and as a doubled edge at 30 Hz when it moves
  fast. An optical-flow interpolation in the blend slots would replace that double image
  with a single displaced one, so its value scales with the motion in the scene rather than
  being a fixed sharpness gain; on slow content it buys little, on fast pans it removes a
  motion artifact.
- The 90x2 source's wobble is the next lever for that regime, and it is a comb question, not
  a threshold question.
- The marked recording should be treated as the primary record and the log as its
  explanation, not the other way round. Today the marker carries the counter, the
  synthesized flag, the weight, the compositor and executor ids and the pick code, which is
  enough to attribute repeats and skips but not to reconstruct the decision: the op (pass
  before, pass after, synth, hold, hold-comb), the lock flag and an identity for the source
  frame shown are in the log only. An extension row carrying those would let the recording
  alone say which frame each output came from, count content steps without timestamps, and
  catch the source-side repeats above without a pixel comparison. The layout is frozen and
  extensions are provided for; this is a candidate for the analysis-suite work.
- The old captures' numbers live in this document, in `docs/session-kickoff-1p5x-threshold.md`
  and in the git history of the four fixture files.
