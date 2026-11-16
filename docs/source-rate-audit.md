# Source-rate audit: what in the policy assumes a 60 fps source with frame generation

Status, 2026-11-15: inventory done and the first measurements taken on two streams. No
setting changed and no replay arm built yet; the test plan below is the next step.

## Why

Almost every capture the policy was tuned on is one game at 60 fps with Smooth Motion x2: the
replay corpus is that game's streams plus the Avatar benches (30 to 90 fps, x1 to x3). The
first stream of a 120 fps native source with `-src 120` declared
(`spiderman_120x1_ovsync_gameplay_2026_11_15_1.trace`) came out clean on video (0.05 freezes a
second of motion, no repeats, no blocked presents), but the relay's own numbers differ from
the 60x2 streams in ways that point at thresholds sized for 60x2:

| | 60x2 stream, 2026-11-15 0 | 120x1 stream, 2026-11-15 1 |
|---|---|---|
| lock releases an hour | 6 | 503 |
| lookahead moves an hour | 37 | about 540 |
| synthesized presents | 1.1% | 2.2% |
| presents whose step is off by over 2 ms | 0.65% | 2.0% |
| dejitter: batches read late | 0.3% | 11% |

The question for each setting: what was it measured on, what does it scale with, and is that
the right thing for it to scale with?

## What the first measurements found

### The lock's stability gate scales with the source period; the jitter it gates does not

`UpdatePhaseLock` keeps the lock engaged while `devEmaQpc < combQpc / 8`. The comb is one
source period at an integer ratio, so the gate is 2083 us at 60 fps and 1042 us at 120. What
`devEma` measures is the wobble of the target's phase against the frames, which comes from the
game's frame times and the capture's timing, in microseconds that do not halve when the rate
doubles. The comment beside the re-engage rule says where the scale came from: "Steady engaged
deviation sits near comb/100 on every x2 capture measured."

Rerunning the lock's EMAs over the field log (matching the field's `lk=` on 98 to 99% of
presents; the model leaves out the stall re-seed and the convergence window):

| | gate | median devEma | time above the gate |
|---|---|---|---|
| 60x2 stream | 2083 us | 172 us | 0.9% |
| 120x1 stream | 1042 us | 242 us | 6.2% |

Releases an hour on the 120x1 stream, by gate: 646 at 1042 us (the field had 503), 265 at
1562 us, 58 at 2083 us. The same gate in microseconds as the 60x2 streams removes about 90%
of the releases. The model overcounts on the 60x2 stream (107 an hour against the field's 6),
which stalls often and where the left-out re-seed matters, so these are directions, not
predictions.

### The lookahead's thresholds are fractions of the comb too

`LookaheadStep` comb/5, `LookaheadSpread` and `LookaheadLead` comb/8, the late-move drift
comb/16. Its comment gives the reasoning in absolute terms ("a single gameplay frame strays
past 3 ms on under 1% of presents"), which comb/5 matched at 60 (3.3 ms) and does not at 120
(1.7 ms). The 120x1 replay plans 479 moves in 54 minutes against the field's 491, and 217 of
480 land more than 2 ms from a real frame.

### Dejitter's late gate is an eighth of the flip step, and on-time delivery is wider at 120x1

A batch is corrected when it arrives more than `spacing / 8` after its anchor flip. Both
streams flip every 8.33 ms, so the gate is 1.04 ms on both. What differs is the spread of
on-time delivery (the suite's pairing line, capture arrival after its flip):

| | p50 | p95 |
|---|---|---|
| 60x2 stream | 438 us | 591 us |
| 120x1 stream | 538 us | 1393 us |

The pairing's confidence gate cites the same 60x2 measurement ("92.4% within 250 us, 99.2%
within 1 ms, then a cliff"). On the 120x1 stream the late readings sit tightly at 1.1 to 2.6 ms
(p50 1.36 ms, 43,997 of them), where on the 60x2 stream they spread from 1 to 12 ms: the gate
cuts through the normal spread rather than past it. A corrected batch moves to its flip
(offset 0) while its on-time neighbours keep their +0.5 ms, so the correction moves stamps by
about the spread it was meant to remove. Even so, the dejitter arm reads better than the plain
arm on the 120x1 replay (evenness rms 579 us against 640, steps off by over 2 ms 1.81%
against 2.03%), so it is not shown to do harm; that measure reads the stamps dejitter moves.

## Inventory

Scales with: **src** the declared source period (or the comb), **flip** the measured flip
step, **present** the present period (always 60 Hz on the card), **fixed** a fixed time,
**count** a count of presents or frames. Risk is at 120x1 unless a row says otherwise.

| setting | where | value | scales with | measured on | risk | 120x1 reading |
|---|---|---|---|---|---|---|
| lock stability gate | `UpdatePhaseLock` | comb/8 | src | x2 captures | **high** | 503 releases/h; see above |
| re-engage confirm | `kEngageStableDiv` | comb/16 | src | x2 captures | high | follows the gate |
| re-engage wait | `kEngageConfirmPresents` | 120 presents | count | x2 | low | |
| error EMA | `UpdatePhaseLock` | 1/16 a present | count | | low | |
| lock slew | `phasePullSlewQpc` | 25 us a present | fixed | | medium | twice as fast against a 120 comb |
| recover window | `kRecoverPresents` and kin | 16 presents, comb/32 | count, src | stall resumes, 60x2 | low | |
| pull wrap bands | `PullWrapBelow/Above` | comb/16, comb/8 | src | | medium | 171 wraps against 153 in a third of the time |
| wrap ease | `kWrapEasePresents` | 12 presents | count | 60x2 | low | |
| lookahead step, spread, lead, drift | `Lookahead*` | comb/5, /8, /8, /16 | src | one 60x2 stream | **high** | about 540 moves/h |
| stall span | `BuildPolicySetup` | 2 source periods | src | | medium | 16.7 ms at 120: one skipped frame sits at the edge; 0.10% of presents read stalled, 21 resumes |
| stall resume | `kStallRunPresents` | 3 presents | count | | low | |
| bracketing lag | `LagForSourcePeriod` | 1.25 src, floored at a present | src, present | | low | 16.7 ms + 75 at 120 |
| passthrough threshold | `PassthroughThreshold` | a quarter of the present period at 2x or faster, else a quarter of the source period | src, present | the 1.5x regime | medium | 4166 us, half a 120 frame: every target has a real frame inside it |
| stickiness | `stickinessQpc` | 1 ms | fixed | 240 to 60 nearest mode | low | under a 120 period |
| tooth guard | `ToothGuardPeriod` | 7/8 of a source period | src | | medium | armed at 120; synth needs 7.3 ms of target advance |
| step tolerance | `StepTolerance` | 18% of the present period (3000 us) | present | 60x2 fights | medium | judged against 120 frames 8.3 ms apart |
| step offset decay | `kStepOffsetDecayPercent` | 3% (500 us) | present | | low | |
| batch window | `BatchWindowQpc` (WGC, NvFBC) | 3 ms | fixed | Smooth Motion pairs at 60x2 | low at 120 | 37 joins in 392k captures; breaks above about 333 fps |
| split-pair window | `kSplitPairWindowMs` | 6 ms | fixed | one 60x2 stream | off by default | sized under half a 120 Hz flip pair |
| split-pair evidence | `kSplitPairRun`, cap | 8, 24 pairs | count | 60x2 | off by default | inert without pairs |
| copy deferral | `kDeferPairedQ8` | 1.6 members a batch | count | 60x2 | none | inert without pairs |
| deferral wait | `kDeferWait*Ms` | 40 to 55 ms | fixed, src | 10 h of 60x2 streams | low | inert at x1 |
| dejitter late gate | `MeasureLateness` | flip step / 8 | flip | 60x2 | **high** | 11% late, all just past the gate |
| dejitter late run | `AnchorChain::lateRun` | 3 batches | count | 60x2 | medium | with 11% late, runs of 3 re-acquire the chain |
| anchor confidence | `PairBatchMemberAt` | flip step / 4 | flip | 60x2 ("92.4% within 250 us") | medium | p95 1.39 ms against a 2.08 ms bound |
| anchor acceptance | `AnchorBatchAt` | 3/4 batch period late, half a step early | batch period | 60x2 | low | |
| chain warm-up | `AnchorChain::warmup` | 8 batches | count | | low | |
| flip cadence window | `m_flipCadenceWindowQpc` | 200 ms | fixed | | low | 24 flips at 120 |
| reorder slack | `kReorderSlack` | 33.3 ms | fixed | | none | slack, not a threshold |
| long-gap rule | `MaxFlowGap` | 100 ms, at least 3 source periods | fixed, src | 60x2 fights | low | never fired: no stall over 100 ms |
| slow-source telemetry | `TemporalCaptureMode` | 600 presents | count | | low | one warning (105 fps) in 54 min |
| flow warp gates | `FlowWarpMath.h` | pixels, ratios | content | 60x2 frame pairs 16.7 ms apart | unknown | pairs 8.3 ms apart move half as far |

Not yet read for this audit: the present pacing (`Pacing.cpp`, `PresentScheduler.cpp`), the
compositor shaders' radii and strides, and the ETW consumer's caps.

## Test plan

Each change goes in behind a switch, default off, so one replay arm compares it with what
ships and the code stays when the switch is not wanted.

1. **Fixtures.** The 120x1 fixture with `-src 120` is in the corpus. The 2026-11-14 120 fps
   stream without `-src` failed fidelity and is kept out. More non-60x2 captures would test
   the same settings from other sides: 120 fps with frame generation, 60 fps native, a
   variable rate such as 90 to 120. Whole streams like the others.
2. **Arms, one setting at a time, in risk order:** the lock gate with a floor in
   microseconds (the gate never under 2083 us), then the lookahead thresholds the same way,
   then the dejitter late gate measured against the stream's own on-time spread instead of a
   fixed share of the flip step. Each arm is replayed over the whole corpus.
3. **What passes:** no 60x2 fixture gets worse on its bounds, and the 120x1 fixture improves on
   releases, moves, synthesized presents and evenness. Then a stream with the switch on,
   checked with the frame marker as usual. One change per stream.
4. **What would stop a change:** a 60x2 fixture that regresses, or a 120x1 improvement in the
   replay that the stream does not show.

The medium rows follow once the high ones are settled; the low and none rows stay as they are
unless a new capture says otherwise.
