# Generated-frame substitution: present the driver's frame instead of blending

Status: BUILT, opt-in behind `-subgen`, TWO field captures. The floor fix is validated;
the readback batching did not work. Output quality is confirmed; the cost to the GAME is
unmeasured. The policy layer and
the replay model are covered by the local suite; `CaptureRing`, `FrameCompositors`,
`TemporalCaptureMode` and `NvFBCR` compile only in CI. Every number here is measured, and the
measurement that produced each one is named so it can be re-run or disputed.

## What it does

At x2 the capture wakes in pairs, `[generated, real]`. Keep-real publishes the real member and
retracts the generated one - but the retracted slot's PIXELS survive until the ring recycles
it. When the relay later finds no real frame near the present target it synthesizes a blend,
which is a lerp of the two bracketing real frames and reads as a double image on moving
content.

That blend is often unnecessary: a real driver-generated frame is already sitting in the ring,
sharp, at almost exactly the right instant. This presents it instead.

**The user's stated bar: blends should be rare and should paper over a single-frame hitch.**

## Placement: nothing here knows the multiplier

A retracted member's stamp is rewritten to a placement between its two real neighbours:

    generated member j of N  ->  before + (after - before) * (j + 1) / N

where N is the number of members the batch carried. Never its own arrival or flip time: the
f/g measurement puts generated content at a CONSTANT phase between its neighbours that does
not track the display (content phase 0.4952 at x2), and at N=2 this rule lands 259 us from
that phase - the estimator's own floor - against 587 us sd and 914 us p90 for the flip time.

**There is deliberately no x2 path and no x3 path.** A batch divides the interval it spans by
the number of frames it carries, so two submissions per source frame and four are the same
arithmetic, and a multiplier that fluctuates needs no detection because nothing detects one.
Only the N=2 value has been measured; the general form is the same physical statement (the
driver divides the interval it is interpolating across).

Every generated member of a batch is re-placed on each arrival, because N is not known until
the batch ends: the first retraction in a 3-member batch legitimately believes N is 2, and the
third member is what corrects it.

**What bounds the damage when the placement is wrong is the passthrough gate, not a regime
test.** If the capture missed one of the driver's submissions, the interval gets divided into
the wrong number of parts and the frame lands somewhere it is not. A frame placed further from
the target than the gate allows is then refused for being too far away, whatever the reason it
got there. See "Regime gates, considered and rejected" below.

## Why the frame is in the right place, provably

A synth fires only when NEITHER bracket endpoint is inside the passthrough gate. For a bracket
one source period wide that confines the target to the middle band, and the generated frame
sits inside that band by construction (worst case 4167 us against a 4166 us gate).

Measured on `get_medieval_2026-08-25_0.log` (ring 32, windowed 200-7900 s), by bracket span in
quarter source periods:

    span            blend-class   substituted    rate
    0.75-1.00           1044          1013      97.0%
    1.00-1.25            939           828      88.2%
    1.25-1.50            548            48       8.8%
    2.75+ (stall)       2567            85       3.3%

    |target - generated stamp|, substituted:  p50 2103 us  p90 3646 us  max 4163 us
    |target - generated stamp|, refused:      p50 41025 us  p90 137719 us  max 391653 us

The geometric claim holds exactly where it applies and falls off a cliff past 1.25 periods.

**So the threshold is free: reuse `cfg.passthroughQpc`. Do not add a tunable.** The refusal
distribution is the argument: refusals sit a p50 of 41 ms from the target, which is not a
threshold being slightly too tight - it is no generated frame existing near that target at
all, because the late batch never paired. Widening the gate would substitute frames 40 ms
stale.

## The three placement rules

`policy::GeneratedCandidateOnTarget` decides on PLACEMENT alone, and `DecideComposite` applies
the identical rule with the content verdict filled in:

1. **Inside the bare gate.** Not the widened Schmitt band: the band exists to stop a parked
   phase chattering between passing and synthesizing on the same endpoint, and a generated
   frame is a different frame each time, so there is no loop to damp.
2. **Strictly newer than the last output.** The composite's own monotone guard permits
   equality because a pull wrap legitimately re-presents one instant per beat; here equality
   means showing the same content twice on purpose. Measured refusals: 7 in 2.1 hours.
3. **Not the frame the previous substitution showed.** A generated frame stays reachable for
   many presents, and showing it twice is a duplicate no content check can catch because the
   pixels are perfectly good both times.

Rule 3 turns out to be belt-and-braces: re-offering the same frame fails rule 2 first, so the
`already shown` counter reads 0 on every capture replayed. It is kept because it states the
intent, and because rule 2 protecting it is a coincidence of the placement, not a contract.

## The content check, and why keep-real being right does not cover it

The NvFBC race puts REAL pixels in the generated slot: 15% of epsilon pairs at x2 are `[X,X]`
double-captures of one sharp frame, both flips having landed before the first grab sampled
(2026-07-11 probe), and the later `-fgphase` measurement reads 11.5% globally, 12.8% at
substitution points specifically (n=195, NOT elevated there, so the feared synth/race
correlation does not exist).

**This does not contradict keep-real's accuracy.** Keep-real's job is to publish the real
member, and the pan-run accounting closes exactly: 30.2/s true pairs + 5.5/s dup-pairs +
24.3/s coalesced singles = 60.0 real/s captured, losing nothing. The 15% is about the member
it DISCARDS, which is why the same probe recorded dup-pairs as "harmless to collapse" - true,
and true precisely because they were collapsed. This is the first feature that consumes the
discarded slot, so it is the first thing for which that number is load-bearing.

**The discriminator is the RATIO `gdiff / motion`, never absolute `gdiff`** - 0.55 for a
genuine generated frame, ~0 for a race duplicate, constant across captures with 25x different
motion, bimodal with a near-empty 0.15-0.40 gap. An absolute threshold inverts the answer on
low-motion content: the cut derived from a constant-yaw pan reported a 60% duplicate rate
where the truth was 11.5%. Below a motion floor the frame is static, a repeat cannot be seen,
and the ratio is a division by noise: allow it rather than computing a random answer.

**It runs on the PRESENT thread, lazily, only where a substitution is already on the table** -
measured 0.302 times per second in replay and 0.412/s in the field, about once per 200
presents.

**FIELD CORRECTION, first capture (`subgen_kcd_0`, 2026-08-26).** Two defects, both fixed:

The motion floor was 0.002, chosen as if it were a visibility threshold. Two things were
wrong with it, and the second was hidden by the first.

It was on the wrong scale. The guard normalised luma to 0-1 by dividing by 1023 (full scale
for the 10-bit channels the ring slots use), while the instrument the thresholds came from
reports motion in raw channel levels. So 0.002 here meant 2.05 there, above the median of
natural gameplay (2.84), and the check short-circuited to "allow" on 19.2% of batches. In the
field this showed as a 2.5% rejection rate against a ground truth of 8.9%.

The concept was also wrong: **the floor is a division guard, not a visibility threshold.**
Swept against the ground-truth capture, the measured duplicate rate above the floor is
8.87-8.98% for EVERY floor from 0.0 to 5.0. The floor does not change how well the ratio
discriminates at all; it only changes how many substitutions skip the check, and that grows
monotonically (34 batches skipped at 0.2, 1728 at 2.0, 1807 at 5.0). The bimodal gap is empty
in the lowest motion bands as well (0 of 6 samples below motion 0.1, 0 of 25 between 0.1 and
0.2), so there is no level at which the ratio stops working and has to be replaced by a
default. It is therefore set just above zero, at 0.01.

**The normalisation is gone entirely.** The ratio is scale-free, so dividing by 1023 could
never affect the discrimination - its only purpose was to let the floor be written on a 0-1
scale, and that is precisely what turned the floor into an unreadable constant (0.000196) that
nobody could check against a measurement. Luma is now kept in the instrument's raw units, so
every number here can be compared to fgphase output directly, with no conversion.

CAVEAT on the evidence: all of this comes from one capture, a constant-yaw pan, whose
low-motion population is thin (31 samples below motion 0.2). The flatness of the sweep is what
the choice rests on, not the bottom-end bimodality.

The check issued a blit and immediately read it back, three times, forcing a pipeline drain
per frame. Measured cost: 864 us mean, 26987 us worst, and 375 late presents (jit > 1 ms) in
one hour against a baseline of ONE. The three downscales were changed to three tiles of one
render target read back once.

**That change did not work, and the reason is worth keeping.** Second capture: 776 us mean,
24267 us worst, jit > 1 ms unchanged at 0.0975/s against 0.0982/s. The cost is not
proportional to the number of readbacks - one `GetRenderTargetData` synchronises the device
whatever is queued behind it, and the 24 ms tail is the GPU being busy with the GAME, not with
three 64x36 blits. Batching was the wrong hypothesis. The tiling is harmless and slightly
simpler to reason about, but it bought nothing measurable. The placement test in front of
it is integer arithmetic. A capture-thread check would run ~60/s (every collapsed batch), and
`-fgphase` does exactly that at 113/s and measurably degrades output: motion-gated video
duplicates move from 0.23/s to 0.79/s. Working geometry is 64x36, not the instrument's
320x180, because the question is whether two frames differ at all rather than how far content
moved.

If the check's resources cannot be created, `Setup` disarms the substitution rather than
running unguarded. Unguarded is a bad trade: duplicates 0.012/s -> 0.042/s on the daily
driver, a 3.5x increase in the artifact the user ranks worst, to remove 0.3/s of blends.

## Regime gates, considered and rejected

Three gates were built and then removed: a pairing-EMA test (are epsilon pairs arriving), a
session batch-cadence test with a dwell (is this x2), and a per-batch test that the interval
being divided is one source period.

They were removed because they cost more than they bought and because they are the wrong
shape. Measured cost of the cadence gate alone on `get_medieval_2026-08-25_0.log`: **381
substitutions, 23% of the population**, to partially refuse Smooth Motion x3 - a regime that
does not work for reasons no gate fixes, and which leaked through anyway (1451 of 18585
batches passed a 64-batch dwell, because an EMA of a mixed distribution sits in the accepting
band for long stretches).

The principle that replaced them: if a generated frame exists and it is in a valid spot that
needs a frame we do not have a real one for, use it. Regime classification is a maintenance
liability that has to be re-derived for every new multiplier, and the gate already answers the
only question that matters.

## What it is worth, measured

`get_medieval_2026-08-25_0.log`, ring 32, windowed 200-7900 s, replayed through the real
policy:

    without -subgen:  synth 5737 (0.75/s)   holds 2
    with -subgen:     synth 3407 (0.44/s)   holds 4   substituted 2328 (0.302/s)

40.6% of everything that would have blended is replaced by a sharp driver-rendered frame. The
ring depth reached for the substituted frame is p50 11, p90 13, max 15 wakes of 32, so
retaining these frames costs no additional ring: a retracted slot already occupies its
position (the search window is counted in WAKES, not valid frames).

**The two extra holds are the monotone guard doing its job.** A blend reports its output at
the target; a substitution reports the frame's own content time, up to 4.1 ms away. That value
feeds the guard, so two later presents read as regressions and hold instead. Two events in 2.1
hours.

Of sampled synth clusters, 82% sit in static or black content (map opens, transitions) where a
blend is invisible; by synth FRAMES the split is 60% static / 40% moving. So the
visible-and-eligible slice is roughly **0.12/s on this game** - one every eight seconds. On 85
seconds of continuous motion (a horse race, log 2958-3043 s) the relay blends LEAST exactly
where a blend would be most visible: 5101 presents, 99.92% passthrough, 4 synths, 0 holds.
Sustained motion means the source renders steadily and frames arrive on time; blends cluster
around source hiccups, which are the static content where a blend cannot be seen.

## Other regimes, replayed

    dxgk_x1_120s_NvFBCR.log   FG off   285 substitutions (11.2% of the blend class)
    dxgk_x2_120s_NvFBCR.log   SM x2    794 substitutions (45.7%)
    dxgk_x3_120s_NvFBCR.log   SM x3    961 substitutions (60.7%)

**The FG-off number is not a bug and the old prediction of ZERO was wrong.** Keep-real's
retraction is not frame-generation-specific: it retracts the older member of any sub-3 ms
pair, and NvFBC delivers those with frame generation off too (366 two-member plus 6
three-member batches in 226 s, matching the log's own `col=` exactly). Those retracted members
are real earlier grabs. Under the placement rules they are legitimate candidates - a real
frame in a valid spot - and the ones that are re-grabs of identical content are exactly what
the content check rejects. The replay cannot model that check (a log carries no pixels), so
285 is an upper bound on what ships.

**The x3 number is placed as if N=2** when the driver made three frames and NvFBC handed over
two, an error of one sixth of a source period, about 2.8 ms. The gate still requires the frame
within 4166 us of the target, so the worst case is a sharp frame ~7 ms off rather than a
double image. Whether that trade is good at x3 is untested and x3 is already broken upstream
of this: the reason is the same one, NvFBC never delivers the third submission. If a DXGI
backend ever delivers all submissions, the placement rule above is already correct for that
case with no code change.

## First field capture, 2026-08-26 (`subgen_kcd_0`, 65.5 min)

    relay:    1618 substituted, 1659 offered, 41 rejected on content, 5 holds
    harness:  1628 predicted on the same log, 5 holds
    same log without the feature: synth 7958 (2.02/s), 2 holds
    same log with it:             synth 6327 (1.61/s), 1628 substituted

Model and implementation agree within 0.6% on the substitution count and exactly on holds.
Trimmed of the loading and exit stretches (which carry 700 of the 1618) the gameplay rate is
0.247/s against the 0.254/s predicted ceiling.

The marked window decoded perfectly: 1448 consecutive presents, 0 counter repeats, 0 skips, 0
presents missing from the file, 100% provenance agreement. The video/log offset is
`log_time = video_time + 44.548 s`.

**The visible-duplicate question is still open and the field capture cannot close it.** A
within-capture control (substitution-dense windows against adjacent quiet ones, normalized
for motion) found no relationship, but the expected effect is ~0.037/s against window-to-
window variance of 0.10-0.79/s: underpowered by an order of magnitude. Settling it needs a
paired same-content test.

## Second field capture, 2026-08-27 (`subgen_kcd_improved_gen_selection`, 89 min)

Both fixes in. Whole run: 2670 substituted, 2930 offered, 260 rejected on content = **8.9%**,
matching the independently measured ground-truth race rate exactly (it was 2.5% before the
floor fix). Trimmed to gameplay (log 120-5160 s, 84 min at a locked 60.0 presents/s):

    presents 302381   holds 0   backward 0.00
    synth       1676  0.333/s
    pass-gen    1814  0.360/s     51% of everything that would have blended

Blends more than halved. The harness predicted 1840 offers on the same window against 1814
substituted plus 26 rejected: exact.

**The rejections are almost all outside gameplay.** 26 of 1840 in the gameplay window (1.4%),
234 in the ~5 minutes of loading and desktop at the ends. That is the guard behaving
correctly - no frame generation runs there, so every retracted slot is a paired real grab -
but it means the 8.9% whole-run figure is an average over two very different regimes, and in
gameplay the guard prevents about one duplicate every 200 seconds.

Downstream chain clean again: 842 consecutive marked presents, 0 counter repeats, 0 skips, 0
presents missing from the file, 100% provenance agreement. Offset `log_time = video_time +
45.951 s`, holding to +-1 frame for 13 minutes and drifting to -9 frames by 50.

**Visual check: the substituted frames were inspected in the video and look correct.** That
closes the quality question the statistics could not reach.

## The cost question, still open

Every cost number in this document is RELAY-side. jit and pdt say whether the relay kept its
own schedule, and by those measures the check is invisible: pdt is statistically identical to
a no-substitution baseline (p50 16660 vs 16664, p99 16955 vs 17006) and this capture has FEWER
double-interval presents than the baseline (26 vs 46). The late presents land on the same
vsync because a blocked INTERVAL_ONE present absorbs them.

**That does not mean it is free.** A `GetRenderTargetData` sync stalls the GPU pipeline the
GAME is using, and nothing measured here would see it. The method for measuring it is
`docs/relay-cost-spec.md`. Until that number exists, the honest position is that the guard's
cost to the game is UNKNOWN, and its measured benefit in gameplay is 0.005/s of duplicates
avoided.

## Validation plan

1. **The same-log self-check, which needs no cross-capture comparison at all.** Capture one
   KCD2 run with `-subgen`, then replay THAT log through the harness with `--sub-gen`. Same
   content, same timeline, same window: the relay's `subgen summary:` substituted count should
   match the harness's. Model against implementation on identical input, with none of the
   windowing or motion-gating traps.
2. **The artifact question needs video.** Motion-gated duplicates must stay at 0.012/s. This
   is what the content check exists to protect and the log cannot answer it.
3. **The content check's cost is instrumented, not assumed.** The summary line reports
   `content check <worst> us / <mean> us`. If it is large enough to push a present past vsync
   it costs a hold, which is the artifact being removed. The fix if so is caching the bracket
   pair's luma by ring slot, cutting the common case from three readbacks to one.
4. `jit` and `pdt` against the no-`-subgen` baseline, since `-subgen` is a clean A/B.

## Known limitations

- Placement is inferred from arrival structure because `NvFBCToDx9Vid` provides no frame
  timestamp and no frame identity. The content check is a pixel-level test standing in for
  metadata the API does not expose.
- NvFBC's generated-frame capture ceiling is ~70-75% at default priority (measured; the
  priority chapter is closed in both directions), so a quarter of generated frames never reach
  a slot. That is the "no generated frame reachable" population, not a defect here.
- Both of the above dissolve under DXGI Desktop Duplication, which taps the scanout plane
  downstream of flip metering and carries `LastPresentTime`. Under that backend the content
  check and the placement rule both become deletable rather than tunable.

## Do not re-litigate

- **The lag fix is done and validated.** `-lag 75` (total 95.8 ms) with a 32-slot ring took
  relay-side holds from 5639 to 2 on a same-content replay. Holds are not this feature's
  problem.
- **The residual ~1/s of visible duplication is DOWNSTREAM**, in the OBS chain, proven by the
  frame marker (counter repeats, 12/12 content-identical, plus 11 presents that never reached
  the file in 12 seconds). No relay change touches it.
- **x3 phasekeep is tabled.** Ghosting 2.31/s against a 0.5/s bar with the vote working.
- **Regime gates are rejected.** See above: measured cost 23% of the population, and they leak
  anyway.
