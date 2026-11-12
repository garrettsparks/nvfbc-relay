# Crossfade instead of warp across a long game stall

2026-11-06. Status, 2026-11-12: Part B is built (section 3) and **OFF by default** (the user,
after the replay's numbers below: "kind of makes me want to hold off on the change now").
`-maxflowgap 100` runs it. Not run on the rig or streamed. What would turn it on is a look at
the video: how many of the stalls are mid-move, and how the warps out of them look.

**What the corpus replay showed, 2026-11-12.** At 100 ms the rule takes 15 to 26% of the
synthesized presents on the long streams (836 of 3775 over 132.6 min), and 7% on 2026-11-05
(106 of 1473, over 26 pairs), the stream this spec was written from. The relay presents about
96 ms behind capture (a source period and a quarter plus the 75 ms extra lag), so a gap up to
about that never holds: the frame after it arrives before the target leaves the frame before
it, and the relay interpolates across the whole gap. Only a longer gap shows a freeze first,
followed by 5 or 6 synthesized presents (the lag's worth) leading out of it. So about the lag
is a principled place for the line, and 100 ms sits on it. Not known: how many of those stalls
are in the middle of a camera move, where the warp may be bridging the jump well and the blend
would be a double image for those presents. The one hitch source characterised on the rig
happens while walking.

**The reasoning it was built on, 2026-11-12, before the replay's numbers.** The rule is worth having on principle, not only for the
dialog: a flow across a stall is where a flow is least reliable, and by the time the frame after
the stall arrives the viewer has watched the freeze, so the presents drawn across it only lead
out of it. They are drawn as the blend. The line is 100 ms, and never less than three source
periods, so a slow source keeps the flow between its own neighbouring frames. It does not follow
the output rate: what breaks a flow is how much the scene changed between the two frames, which
follows the time between them. 100 ms is a judgement: the one sequence looked at was bad at 180
and 203 ms, and nothing between 50 and 180 ms has been graded.

## 1. Why

`o` synthesizes a present when no real frame lands close enough to its refresh, from the two
captured frames around it (the pair), by warping along an optical flow between them. When the
game stalls, the pair can be far apart in time: on the 2026-11-05 stream the game's frames came
48, 180, 203 and 52 ms apart while KCD faded in its "keep your current equipment" dialog, and the
relay warped across the 180 and 203 ms gaps. A fade and a box appearing over 11 or 12 frames'
worth of change is not motion a flow can follow: the warped frames showed the dialog's text torn
and doubled (the user, 2026-11-06: "this sequence wasn't so good"). The relay then held the last
warp for 6 presents while it waited for the game, so a garbled dialog sat on screen for about
0.1 s.

The cut gate (`2044bc4`) already draws the crossfade instead of the warp for one kind of pair it
cannot warp: a cut to or from black, told by the pair's mean luma. This dialog's pairs read 0.73
and 0.80 on that ratio, so the gate let them through. What set them apart is the time between
the two frames.

How often it happens, in the 84 minutes of that stream's video (1468 warped presents):

| gap between the pair's frames | warped presents |
|---|---|
| under 25 ms | 770 |
| 25 to 50 ms | 290 |
| 50 to 100 ms | 327 |
| 100 to 200 ms | 55 |
| 200 ms or more | 26 |

Only one long-gap sequence has been looked at, and it was bad. Whether the other 80 warps across
100 ms or more look bad, and whether the 327 at 50 to 100 ms look fine, is not known. A crossfade
is not free either: across a stall in a camera move it is a double image (the user's word for a
blend frame), where a warp of plain motion may look right even across a long gap.

## 2. Part A: measure first (no relay change)

As of 2026-11-12 the gaps of 100 ms and more are settled by the rule, so what is left to grade
is the sample at 50 to 100 ms against the control under 25 ms: whether the line should come down
toward 50. The steps below are as first written.

From the 2026-11-05 VOD and log, with the corrected mapping (`checks-1105/flowclips/` in the
analysis folder; log time = VOD time + 48.009 s, the video one frame later every 1490 s after
850 s, checked against hold runs):

1. List every warped present with its pair's gap, its weight, the cut gate's ratio, and whether
   the VOD around it moves.
2. Pick clips by gap: every warp at 200 ms or more (26), every one at 100 to 200 ms (55), and a
   sample of 15 at 50 to 100 ms and 10 under 25 ms as the control (warps the user already calls
   good). Group the warps of one stall into one clip.
3. Make each clip plain and marked (`mkclips.py`), and a tiled still of the warped frames at
   full size, so a bad warp is seen on the frame it is on and not as a flash.
4. The user grades each stall: fine, minor, or bad. Claude grades the same frames separately
   first and the two are compared; a grade from a still alone is not enough for a moving
   picture (memory: never judge blends from a still).

The decision rule, written before the grades: if the bad share rises with the gap and sits under
10% below some gap while it is over half above it, that gap is the line and Part B is built with
it. If bad warps are spread across all gaps, the gap is the wrong signal and Part C is the
direction instead. If almost none are bad apart from the dialog, nothing is built and the dialog
is written down as the known case.

Also worth one look: if the user knows what brings this dialog up, it can be made on purpose on
the rig for a marked recording, the way the cut gate's map opens were. What triggers it in KCD
is not known here.

## 3. Part B: the gap rule, if Part A finds a line

**The rule.** A synthesized present whose pair is more than `maxFlowGap` apart is drawn as the
crossfade, the same draw the cut gate falls back to; no flow is planned or run for that pair.

**Where it goes.** Not in the policy step: the step has already decided this present
synthesizes, and how a synthesis is drawn is the compositor's choice, as with the cut gate. So
the replay suite's output does not move, and the refactor gate stays byte-identical.

- `src/core/FlowPlan.h`: the rule, so the suite tests it as it ships:
  `inline bool PairTooFarToFlow(const FlowPair& pair, int64_t maxGap)`, and
  `MaxFlowGap(ticksPerSecond, srcPeriod, ms)`, which sizes the threshold once at setup in the
  stamps' clock with the floor of three source periods.
- `D3D11PresentBackend::PlanFlows` (`src/relay/D3D11Present.cpp`): a predicted synthesis whose
  pair the rule rejects is not given to the planner, so no flow is spent on it (that saves the
  flow engine's work, about 0.27 ms of submit per flow on the 11-05 log, and its GPU time).
- `D3D11PresentBackend::DrawSynthesis`: checks the rule first; a rejected pair draws the blend
  with `fl=gap`, a label of its own beside `hit`, `late`, `own` and `miss`, so the log and the
  analysis tools tell it from a flow that was not ready. The exit summary counts it.
- `TemporalCaptureMode`'s startup line for the compositor names the rule and its threshold.
- A hidden option `-maxflowgap <ms>` (0 to 1000), off by default (0). Hidden as
  `-blend` is; listed only if the user wants it.

**Holds.** After the last synthesized present of a stall the relay holds that output until the
game's next frame arrives; with the rule, what is held is the crossfade. Nothing changes there.

**What it does not touch.** The cut gate (it runs on the pairs that are still warped), the
blend's own shader, `b` (no flow), the timer modes' `o:<fps>` apart from the same rule.

## 4. Part C: the flow's own confidence, if the gap is the wrong signal

The flow engine already returns a cost per block with every flow, its measure of how badly the
two frames matched there; the relay computes it and never reads it (the cut gate's notes,
2026-11-03). A pair whose cost is high over a large share of the picture is one the flow could
not follow, for whatever reason: a long stall, a fade, a lit cut, smoke. Reading it means a GPU
reduction of the cost map per flow, like the cut gate's luma pass, and a threshold found the
same way as in Part A, with the cost recorded beside each warp. More work and more GPU time
than Part B; better aimed if Part A shows bad warps at every gap. Its own spec if it comes to
that.

## 5. Tests for Part B

- **Suite:** the rule at, just under and just over the threshold, with stamps in ticks; 0 turns
  it off. The replay output is byte-identical to the tree before (the rule is the compositor's).
- **Controls that fail loudly:** the flow probe's self-test gets a case of a pair stamped
  200 ms apart, which must draw the crossfade and log `fl=gap`, and one 16.7 ms apart, which
  must warp. NOT built 2026-11-12: the self-test drives the flow engine directly and the rule
  is the present's, so a case there would only test the header function a second time. The
  rig control in its place: the same launch with `-maxflowgap 0` must log no `fl=gap` and
  `Flow gap limit off`; with `-maxflowgap 1` the startup line must read the floor (three source
  periods, 50000 us at a 60 fps source) and every synthesized present whose pair is further
  apart than that must log `fl=gap`, none closer.
- **Rig, KCD, marked, recorded through OBS:** the dialog provoked a few times on the old and the
  new build, `o:vsync -wgc -mark 5400` against the same with the rule; the new build's frames at
  the dialog must be crossfades (`fl=gap` on those presents), the old build's warps.
- **Cost:** Pandora, three rows a build, balanced, the rule written before the data (memory:
  launch luck against the build): the rule should cost nothing and may save a little, since it
  only removes flows.
- **A stream** on the build, then streamcheck and the corpus step as usual (one change per
  stream).

## 6. Decisions for the user

- Whether to do Part A now or after the release. Claude's default, the user's to change: after
  the release; the dialog is rare, and the release's open items come first.
- The threshold, from Part A's grades.
- Whether `-maxflowgap` is listed in the usage text or stays hidden.
