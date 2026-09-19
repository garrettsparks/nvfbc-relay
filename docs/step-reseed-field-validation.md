# Field validation: the step re-seed rule

Build under test: `2ee7c67` (`3fe2ace`, the rule, plus its log evidence), CI run 35292662876, both
jobs green. Everything below `3fe2ace` was already validated in
`docs/capture-and-lock-field-validation.md`; this report decides only whether the step rule ships.

Status: **both captures analysed, the rule reverted (`c7d67d4`), and the decision re-examined on a
third log (section 2.6).** On the risk capture (KCD map cycles) it did no harm because it never fired.
On the benefit capture (KCD gameplay, streamed) it fired six times, four of them on phase transients
the game reverted about 0.65 s later, and each of those four turned a run the old policy would have
held to 40 presents into 42 to 100. The paired replay of that log confirmed it. A later 90-minute
capture on the reverted policy, replayed with the rule on, showed the other side: over three logs
the rule halves the number of long runs and cuts the time spent in them by 39 percent, at the cost
of a fatter tail (one run of 112 against a worst of 64 without it). The revert stands as the release
position on the tail-risk ground; the benefit is real and is the case for a seventh candidate with an
undo path. Section 3 has the corrected verdict.

## What is being decided

The step re-seed rule (`docs/blend-zone-crossing-investigation.md`) re-seeds the comb lock's error
estimator when, after a quiet engaged stretch of 60 presents, the phase error sits at least comb/4
from the estimator and stays off a real frame for four settled presents. In replay it removed four
long blend runs over 43 fixtures with zero fixtures worse and fired zero times on the eight 59:60
sweep rows and on both KCD map-cycle fixtures. Every one of those numbers was replay.

Two captures were specified, on the same build, `b:vsync -src 60 -lock -lag 75 -etw -dejit`:

| content | marker | tests | files |
|---|---|---|---|
| KCD, 30 map open/close cycles, not streamed | `-mark` (every present) | the RISK: stall-heavy content, where an earlier version of the rule misfired | `kcd_mapcycles_2026-09-17.{log,mp4}` |
| KCD, about an hour of normal play, streamed | `-mark 7200` | the BENEFIT: the residual 50+ present blend runs, four in 53 min on 09-16 | pending |

The failure mode the risk capture looks for is a fire with no disturbance, or a fire that lengthens a
blend run at a map event. Both would show in the rule's own log lines, which is why `2ee7c67` exists.

## 1. KCD map cycles, 2026-09-17

Log: 27021 presents over 452 s, 51100 captures. Video: 21804 frames at 60 fps, 363.4 s, counters
4942 to 26648, which is log time 82.8 s to 446.2 s. The first 60 s of the log are the desktop and the
game loading; the map cycles run from about 84 s to 441 s. Every number below is windowed and says
which window it uses.

### 1.1 The rule's own evidence: zero fires

```
lock summary: 0 confirmed phase steps re-seeded the estimator
```

No `lock: step re-seed` line anywhere in the log. Replay predicted zero fires on both map-cycle
fixtures at the comb/4 floor; the field gave zero. That is agreement on the one number the rule
reports about itself.

Why zero, from the timeline: every phase disturbance on this content is a stall. A map open stalls
the source for about 265 ms and then again for about 185 ms; a map close stalls it for about 153 ms.
Each resume takes the existing stall path (`resumedFromStall`), which re-seeds the estimator and
resets the quiet-engaged counter the step rule needs, so the step rule never opens a candidacy. The
rule cannot have misfired here because it never had a candidate. This is a no-harm result, not a
measurement of benefit; the benefit is the gameplay capture's to show.

### 1.2 Blend runs and the lock, against the fixture

The fixture `kcd_60x2_mapcycles_postfix` records the field behaviour of the same content on
2026-08-01 (D3D9 path, before the grab-timeout fix). Its window includes its own startup, so the
whole-log column is the like-for-like one.

| | fixture field (474 s, whole) | this log, whole (452 s) | this log, block (60 s to end, 392 s) |
|---|---|---|---|
| presents | 29747 | 27021 | 23449 |
| worst blend run | 34 | 40 | 11 |
| runs >= 50 | 0 | 0 | 0 |
| runs 20-49 | not recorded | 17 | 0 |
| runs 8-19 | not recorded | 29 | 24 |
| synth share | 6.0% | 5.18% | 3.15% |
| holds | not recorded | 863 | 628 |
| lock disengaged | not recorded | 4.18% | 0.00% |
| lock engages / disengages | not recorded | 5 / 5 | 0 / 0 |

Synth share is under the fixture's field figure on the like-for-like window and no run reaches 50.
The worst run, 40 presents, and all 17 runs of 20 to 49 sit inside the first 46 s of the log, with
`lk=0` on nearly every present inside them: the desktop-to-game transition, where the source ran at
19 to 59 frames a second and the lock had no phase to hold. Minute 0 of the 09-16 gameplay capture has
the same shape (833 synth presents, lock off 47.7%). The video begins at 82.8 s, after all of it.

Inside the block the lock is engaged on every present and the 24 runs of 8 to 11 presents each sit at
a map event. The raw timeline at the 96.6 s open shows what they are: the first frame after a 268 ms
stall arrives, the target lands between the pre-stall frame and it, and the compositor blends across
that gap for seven presents with the weight ramping 0.59 to 0.97, then holds through the second stall,
then converges. Same shape at every close. They are the stall path working as designed, not the
class the step rule targets (that class has `lk=0` inside the run and no stall before it).

### 1.3 Holds are honest source stalls

Of the block's 628 holds, 628 fall inside a capture gap of 50 ms or more or within 0.5 s after one;
515 (82%) are at the 200 ms plus map-open stalls and the rest at the 140 to 200 ms stalls of the
second open phase and the closes. The capture summary reports 181 grabs that waited out the grab
timeout and stored nothing, which is the 09-16 grab-timeout fix behaving: before it, those would have
entered the timeline as new frames.

### 1.4 The map events as the source delivered them

Capture-arrival gaps in the block, by size:

| gap | count | what |
|---|---|---|
| >= 200 ms (254 to 378 ms) | 31 | the 30 map opens, plus one at 84.6 s as play begins |
| 140 to 200 ms | 59 | the second stall of each open (about 185 ms) and each close (about 153 ms) |
| 50 to 140 ms | 4 | ordinary hitches at 84.7, 309.3, 322.3 and 382.2 s |

So the block holds 30 opens, 30 closes, 90 stalls, and about 60 resumes that matter for the next
section.

### 1.5 Three-channel attribution, then the video

Before any video claim, the relay-side channels:

**Channel 1, the relay log.** 89 presents blocked for more than one refresh period, missing 107
refreshes in total (the `blk=` field, which is the wait inside the present path; a normal present
waits one period). The `d3d11 presentstats summary` independently reports 89 refreshes that showed no
new frame, 0.198/s. Inside the block there are 81 such presents and every one lies within 0.5 s
after a source stall resume. Their wake jitter is 18 to 61 us: the thread was on time and the present
call blocked, for 33 ms (63 of them) or 50 ms (17), never longer.

**Channel 2, capture arrivals.** Capture-side GPU flush over the block: p50 99 us, p95 169 us. For
captures within 150 ms after a resume: p50 107 us, p95 11.9 ms, max 31 ms. In both events printed in
full (the 96.6 s open and the 90.6 s close) the blocked present's deadline falls inside a 24 ms flush
on the capture thread. The game's own GPU spike at the map transition delays the relay's capture
copy, and the present thread blocks behind it.

**Channel 3, ETW head 1 (the sink).** 27042 flips, 141 gaps over 20 ms, 40 over 40 ms, in pairs
0.3 s after each open and at each close, matching channel 1 event for event.

**The video.** Marker present and checksum-valid on 21804 of 21804 frames. Counter steps: +1 on
21706, repeat 97, skip 0, backward 0, misread 0. All 97 repeats are content-identical (mean absolute
difference under 0.15 against a motion median of 8.1). Exact join to the log: the 80 repeated
counters each have a present that blocked exactly two counters later (the flip queue holds one frame
between the present call and scanout), and the blocked duration predicts the repeat count exactly
(33 ms gives one repeat, 50 ms gives two) for all 80. In reverse, all 80 blocked presents inside the
video span have their repeat. Nothing is left over.

OBS wrote the file through the local recording output (`adv_file_output`, H.264 VideoToolbox
hardware), 21805 frames output against 21814 drawn, and printed no lagged-frames line, which OBS
writes only when the count is non-zero.

So every duplicate frame in this recording is the relay's present thread blocking behind a
capture-side GPU flush at a stall resume. The chain added none. Nothing is UNEXPLAINED, and nothing
is assigned to the capture PC.

The motion-gated census reads 0.43/s (157 events, 166 frames) over the recording. That figure is not
comparable to gameplay recordings: it counts the relay's holds at source stalls (fresh counter, same
picture, the correct output for a source that drew nothing) together with the 97 chain-visible
repeats, and map-cycle content is nothing but stalls. The marker is what separates the two here.

**This is not the step rule's.** The rule is arithmetic on the present thread and adds no GPU work.
The same signature is on the 09-16 gameplay log (60 blocked presents, 71 refreshes missed in 53 min,
0.020/s, at hitches) and on the 09-13 D3D11 log (170 in 2 h). What this capture adds is a dense,
marker-confirmed measurement of it: about 1.3 blocked presents and 1.6 repeated frames per map event,
with the mechanism visible in the raw timeline. It belongs on the post-release list beside the
flush cost the relay-cost ladder measures.

Provenance cross-check (video interp flag and weight against the log's `op=` and `bw=`): 98.16%
agree. The 401 disagreements are all holds of a synthesized frame, where the marker inherits the
executor of the frame being held while the log says `op=hold`. That is the marker spec's stated
behaviour, not a defect.

### 1.6 Verdict for the risk half

| kickoff expectation | measured |
|---|---|
| `lock summary` present | present, 0 fires |
| no blend run >= 50 after a map open | none anywhere; block worst 11 |
| synth share no worse than `kcd_60x2_mapcycles_postfix` | 5.18% whole log against 6.0%; 3.15% in the block |
| holds no worse | 863 whole, 628 block, 100% at source stalls; the fixture predates the grab-timeout fix and records no hold count, so this is read against the 09-16 report's finding that the fix converts stored copies into honest holds |
| every fire at a disturbance | vacuous: no fires |
| replay against field | replay predicted 0 fires on this content, field gave 0 |

The rule did no harm on the content where an earlier version misfired. It also did nothing at all,
by construction, because stall resumes take the older re-seed path first. Whether it does the good
the corpus predicts is the gameplay capture's question.

## 2. KCD gameplay, streamed, 2026-09-17

Log: 117505 presents over 1959 s (32.7 min), 233237 captures; the game is up from about 80 s and the
session was cut short of the planned hour. Video: 110333 frames, 1840.4 s, HEVC through the stream
output (`rtmp multitrack video`). Marker: the video's first frame is present 4952, so log time equals
video time plus 83.0 s; the marked span (`-mark 7200`) ends at video 34 s, inside the desktop-to-game
transition, so counter-level video attribution reaches none of the gameplay. Every number below says
its window.

### 2.1 The rule's own evidence: six fires, four followed by a long run

```
lock summary: 6 confirmed phase steps re-seeded the estimator
```

| fire (log s) | error corrected | pull at fire | what the source did | run that followed | lock inside it |
|---|---|---|---|---|---|
| 241.3 | +6420 us | 7385 | one frame 6.9 ms early at 241.11 (arrival 9.8 ms, source flips 0.7 / 5.1 / 13.1 ms), phase taken back at 241.78 (arrival 23.3 ms) | 42 presents at 241.9 | released for 15 |
| 288.0 | -5701 us | 10350 | a dropped frame at 287.75 (arrival 33.3 ms) with 8.1 and 9.8 ms catch-ups, phase held afterwards | 9 presents, clean | engaged |
| 298.0 | +5946 us | 6874 | one frame 6.1 ms early at 297.78 (arrival 10.6 ms, flips 3.2 / 5.7 / 11.5 ms), phase taken back at 298.43 (arrival 22.1 ms) | **100 presents at 298.5** | engaged throughout |
| 1351.2 | +7436 us | 818 | a late frame at 1350.80 (23.5 ms) with 12.1 and 12.5 ms follow-ups, an early one at 1350.97 (8.1 ms), another late one at 1351.64 (25.8 ms) | 62 presents at 1351.7 | released for 30 |
| 1911.5 | +6781 us | 3487 | one frame 7.0 ms early at 1911.31 (arrival 9.7 ms, flips 0.4 / 5.0 / 13.1 ms), phase taken back at 1911.97 (arrival 24.0 ms) | 44 presents at 1912.1 | released for 16 |
| 1931.3 | +6520 us | 4552 | one frame 6.9 ms early at 1931.13 (arrival 9.8 ms); 0.6 s later the session leaves the game (a 143 ms stall, then 240 Hz) | 10 presents, clean | engaged |

Every fire sits on a real disturbance in the source's own delivery and scanout, so the failure mode
the kickoff named (a fire in steady play with nothing under it) did not occur. A different one did.
Four of the six fires sit on a phase TRANSIENT: the game presents one frame 6 to 7 ms early, holds the
new phase for about 0.65 s (about 40 presents), then presents one frame late and is back where it was.
The two clean fires sit on the other shape, a dropped frame whose phase change persisted, which is the
shape the rule was built on (section 2.4).

### 2.2 What the rule does to a transient, present by present

The 298 s event, from the log. Before it the lock is engaged, the pull is slewing at 25 us a present
and every present is a passthrough. At 297.886 the target is suddenly 6 ms past the before frame
(blend weight 0.36) and stays there: the phase has stepped. The rule opens a candidacy, waits its four
settled presents, and fires at 297.969. The recovery window then slews the pull at comb/32, 521 us a
present, from 6874 to 12603 in twelve presents; the target is back on a real frame by 298.036 and the
blend run that the step caused is nine presents long. That is the rule working exactly as designed.

At 298.433 the game takes the phase back. The target is now 6 ms off in the other direction (weight
0.70 to 0.75, four to five ms short of the after frame, inside the blend zone). Three things now hold
it there:

- The recovery window has closed (sixteen presents, ending at 298.24), so the pull is back on the
  steady slew of 25 us a present, and it has 2.5 ms to travel before the target is within the
  passthrough threshold of the after frame: about 100 presents.
- The estimator absorbed the reverted error quickly (the recovery alpha of 2 was still in force for
  part of the revert, and the lock's deviation gate never opened), so the lock stays ENGAGED with
  nothing to release it. `lk=1` on all 100 presents.
- The rule cannot fire again: it requires 60 quiet engaged presents after its last fire before a new
  candidacy can open, and the revert came 28 presents after the fire.

So the correction became the error. Without the rule, the transient's own 40 presents of blend would
have ended when the game took the phase back. With it, nine presents of blend and then 100.

The 241, 1351 and 1911 events differ only in that the lock released for part of the following run
(the second disturbance arrived while the estimator was still fast, and the deviation crossed the
gate), which is why `zonecross.py` files them as disengaged sweeps and 298.5 as an engaged crossing.

### 2.3 Blend runs against the 09-16 baseline

| | 09-16 (52.8 min, `79f33e9`) | 09-17 (32.7 min, `2ee7c67`) |
|---|---|---|
| presents | 190097 | 117505 |
| blend runs >= 50 (whole log) | 4 | 3 |
| of which in gameplay (after the first two minutes) | 3 (65, 59, 52) | 2 (100, 62) |
| worst blend run | 65 | 100 |
| blend runs 20-49 | 24 | 15 |
| blend runs 8-19 | 34 | 35 |
| synth share, whole log | 1.03% | 1.22% |
| lock engages / disengages | 17 / 17 | 12 / 12 |
| holds | 646 | 484 |
| source gaps >= 25 ms | 190 (3.6/min) | 144 (4.4/min) |

The kickoff's expectation was the 50+ runs gone or shorter than 09-16's worst of 65. Measured: not
gone, and the worst grew to 100. The startup runs (61 at 70.7 s, 48 at 11.9 s, and 17 more of 20 to 49
inside the first 80 s, all with the lock disengaged during the desktop-to-game transition) are the same
population 09-16's first minute has and are excluded from the gameplay row.

### 2.4 The paired replay: what this log would have done without the rule

The corpus replay (`PolicyTests.cpp`, `Simulate`) was run on a fixture cut from this log with
`mktrace.py` in two scratch copies of the policy: the tip as committed, and the tip with the rule's
quiet-stretch constant set unreachable so it never arms. The same two arms were run on a fixture cut
from the 09-16 log (120 s to 3160 s, 182337 presents).

The window matters and the first cut was wrong. The game exits at 1931.9 s and the source then runs
the desktop at 240 arrivals a second, which is the regime `mktrace.py` warns to trim: the relay keeps
presenting at 60 because the sink is 60 Hz, so the tail is invisible in a present count and has to be
found in the arrival rate. The committed fixture is cut 120 s to 1925 s (108241 presents): 40 s past
the last acquisition disturbance at the head, and 6.9 s before the exit inside a 17 s stretch of pure
passthrough at the tail. It keeps all four transients and drops only the clean sixth fire at 1931 s.
Trimming those last seconds moved no bound, which is the evidence they were inert. The arms give the
same numbers on either window.

**Control first.** With the rule on, the replay of the 09-17 log fires six times at 241.2, 287.9,
297.9, 1351.2, 1911.4 and 1931.3 s, against the field's 241.3, 288.0, 298.0, 1351.2, 1911.5 and
1931.3 s, and produces runs of 42, 112, 60 and 47 presents at 241.8, 298.5, 1351.7 and 1912.0 s
against the field's 42, 100, 62 and 44 at 241.9, 298.5, 1351.7 and 1912.1 s. Same count, same
places, lengths within ten percent. The model tracks this log.

| arm | log | fires | runs >= 50 | worst | runs >= 40 (length at log s) | synth |
|---|---|---|---|---|---|---|
| rule ON | 09-17 | 6 | 2 | 112 | 42 @ 241.8, 112 @ 298.5, 60 @ 1351.7, 47 @ 1912.0 | 0.6% |
| rule OFF | 09-17 | 0 | **0** | **40** | 40 @ 241.2, 40 @ 287.8, 40 @ 1351.0 | 0.5% |
| rule ON | 09-16 | 7 | 1 | 65 | 65 @ 2457.4, 42 @ 2666.6 | 0.4% |
| rule OFF | 09-16 | 0 | 3 | 64 | 59 @ 1898.6, 58 @ 2433.3, 48 @ 2451.9, 45 @ 2456.6, 43 @ 2828.9, 64 @ 2832.6 | 0.5% |

On the 09-17 log the policy the release would otherwise ship (`79f33e9`, the OFF arm) has no run of
50 or more in 30.6 minutes and a worst of exactly 40, which is the transient's own duration. The rule
made every one of its four long runs: 40 became 42, a run under 40 became 112, 40 became 60, a run
under 40 became 47.

On the 09-16 log the rule does what the corpus said it would, mostly: it removes four of the six runs
of 40 or more (the 59, 58, 48 and 64, each sitting on a dropped-frame hitch whose phase change
persisted), but it lengthens one (45 at 2456.6 becomes 65 at 2457.4, two fires 4.7 s apart) and
creates one (a run under 40 at 2666 becomes 42). Across these two logs together the count of runs of
50 or more is unchanged, three with the rule and three without, and the worst run goes from 64 to
112. That two-log statement is what the revert decision rested on, and section 2.6 shows it was
incomplete.

### 2.6 The third log, replayed after the decision

The revert was already committed when a 90-minute capture on the reverted policy arrived
(`Get_Medieval_2026-09-17_1`, now `gm_60x2_gameplay_0917_1.trace`). Because its field ran without
the rule, the OFF arm of the same paired replay is the fidelity control here (it reproduces the field:
eight runs of 50 or more against eight, worst 63 against 62) and the ON arm shows what the rule would
have done on a third log it had never seen. Same tally on all three:

| log | arm | fires | runs >= 40 | presents inside them | runs >= 50 | worst | synth |
|---|---|---|---|---|---|---|---|
| 09-17_0, 30.6 min | ON | 6 | 4 | 261 | 2 | **112** | 0.6% |
| | OFF | 0 | 3 | 120 | 0 | 40 | 0.5% |
| 09-16, 50.7 min | ON | 7 | 2 | 107 | 1 | 65 | 0.4% |
| | OFF | 0 | 6 | 317 | 3 | 64 | 0.5% |
| 09-17_1, 89.7 min | ON | 24 | 9 | 431 | 2 | 67 | 0.6% |
| | OFF | 0 | 18 | 877 | 8 | 63 | 0.7% |
| **all three, 171 min** | **ON** | 37 | **15** | **799** | **5** | **112** | |
| | **OFF** | 0 | **27** | **1314** | **11** | **64** | |

On the 90-minute log the rule removes eleven of the eighteen runs of 40 or more, creates two of 43
and 44 (fires at 319.8 and 480.0 s where the old policy had nothing that long) and lengthens one from
43 to 67 (the fire at 5372.2 s). Its fires sit on the old policy's own long runs: eleven of the
eighteen have a fire within a second of their start.

So over the three logs the rule cuts the number of runs of 50 or more from 11 to 5, the number of 40
or more from 27 to 15, and the presents spent inside long runs by 39 percent. What it costs is the
tail: one run of 112 in 171 minutes where the old policy's worst is 64, and by the mechanism in
section 2.2 the ceiling for that class is the crawl-back distance divided by the steady slew, about
4.2 ms at 25 us a present, or roughly 170 presents (2.8 s) with the lock engaged. The two-log verdict
that the rule "leaves the count of long runs where it was" was wrong as a general statement; it held
on the two logs it was measured on and not on the third. The rule trades frequency for tail size.

Why replay called it clean: the 43-fixture corpus holds hitches, stalls and sweeps but no reverting
transient of this shape. The rule's confirmation delay (four presents, 67 ms) is an order of magnitude
shorter than the transient (about 40 presents), so it cannot tell the two apart, and its own quiet
requirement then forbids the undo. That is a design property, not a tuning problem: any confirmation
short enough to save presents on a sustained step is short enough to commit to a transient.

### 2.5 Three-channel attribution and the video

The relay-side channels, whole log unless stated:

- **Relay log.** 57 presents blocked past one refresh (50 of them in gameplay), 71 refreshes missed;
  `d3d11 presentstats summary` reports 63 refreshes with no new frame (0.032/s, against 0.020/s on
  09-16 and 0.198/s on the map-cycle run). Holds 484, synth 1.22%, lock off 1.02%, 836 late batches
  all corrected by the dejitter.
- **Capture arrivals.** 144 source gaps of 25 ms or more (4.4 a minute), 24 of 200 ms or more, most in
  minutes 0 to 12 and a cluster at 1541 s; minutes 13 to 31 have zero to three each. 110 grabs waited
  out the grab timeout and stored nothing.
- **ETW head 1 (the sink).** 117510 flips at 16.68 ms mean, 474 gaps over 20 ms, 24 over 40 ms.

The video: 110333 frames delivered through the stream path. OBS reports 106828 frames drawn of 110593
attempted, **3765 frames lagged to rendering (3.4%)**, 231 dropped to the connection (0.1%), 12 to 14
skipped to encoding. The 09-16 stream logged 1830 lagged (1.0%). This is the capture PC's own item,
still unexplained and now larger; OBS's counter is direct evidence that it happened, but it cannot be
placed in time, and the marker covers only the first 34 s of video. Per the standing rule no video
event outside the marked span is assigned to any party: the relay's channels account for 71 missed
refreshes and the rest is UNEXPLAINED at the frame level. The packet-size shortlist flags 3.13% of
frames, which is a shortlist and not a rate. The motion-gated census is recorded in section 4 when it
completes.

## 3. Verdict

| kickoff expectation (gameplay) | measured |
|---|---|
| `lock summary` present | present, 6 fires |
| 50+ present runs gone or shorter than 09-16's worst of 65 | 3 runs >= 50 in 32.7 min (2 in gameplay), worst 100 |
| each fire at a disturbance, none in steady play | all six on a real source phase event; four of the six on a transient the game reverted within 0.65 s |
| replay against field | ON arm reproduces the field's fires and runs; OFF arm shows the same log with no run >= 50 and worst 40 |

**What the rule is, on three logs.** It is safe on stall-heavy content only because it never arms
there. On gameplay it does two things at once: it removes the sustained-step runs it was built for,
which over 171 minutes halves the number of runs of 50 or more (11 to 5) and cuts the presents spent
in long runs by 39 percent; and on the reverting-transient class it commits to a phase the source
takes back, which produced one run of 112 against the old policy's worst of 64, with a mechanism
ceiling near 170. The two-log verdict written first (section 2.4) said the count was unchanged; the
third log (section 2.6) shows that was not general. The honest statement is a trade of frequency for
tail size, and which side of it to be on is a product decision, not a measurement.

**The revert stands as the release position, on the tail-risk ground.** `c7d67d4` reverted `3fe2ace`
and the code of `2ee7c67` (its two docs were kept), and the release proceeds on the `79f33e9` policy,
which `docs/capture-and-lock-field-validation.md` already validates and whose worst case is bounded
by the transient's own length. The rule's benefit is real and is not thrown away: the three fixtures
now in the corpus cover both regimes (transients on `gm_60x2_gameplay_0917`, sustained steps on
`gm_60x2_gameplay_0917_1` and the hour fixture), so a seventh candidate with an undo path can be
measured against all of them in replay before a capture is spent. If the frequency side of the trade
is preferred for the release instead, reinstating the rule means moving `max_worst_run` on both
09-17 fixtures with the reason written in (112 on the first, 67 on the second) and accepting that
the third log's result is replay, not field.

**Two things worth keeping from this.** The 09-17 gameplay fixture is the corpus member that would
have caught it, and it is now in the tree as `samples/NvFBC/NvFBCR/testdata/gm_60x2_gameplay_0917.trace`
with its index entry: 108241 presents, bounds `max_worst_run 40`, `max_long_runs 0`,
`max_synth_pct 0.9`, `min_reseeds 34`, measured on the reverted policy. Verified both ways: the suite
passes at 44 fixtures with the rule reverted, and fails three assertions on this fixture with the rule
still in (worst 112 against 40, long runs 2 against 0, and the stage-6 arm). Because of that second
result the fixture must land with or after the revert, not before it.

And any future step rule needs an undo path (an error that returns to about minus the corrected amount
inside the transient window should re-seed straight back) or a confirmation longer than the transient,
which forfeits most of the benefit. Both are post-release questions.

## 4. Video census, gameplay (motion-gated)

`mgdupes.py` over the whole 1840 s stream recording: 1727 motion-gated duplicate events, 0.94/s,
3103 frames, with 52.4% of the recording counted as moving. Controls for the instrument: 0.00/s on
the static capture, 0.12/s on a recording whose marker proves one repeat in 18446 frames. The 09-16
stream recording, same output path, sat at 0.18 to 0.42/s in typical 60 s windows with a worst window
of 5.13/s.

What the relay's channels can account for, whole log: 484 holds (0.25/s, inside the 09-16 baseline
band, all honest re-presents where the game drew nothing) and 71 refreshes missed by blocked presents.
Together that is at most about 555 frames against 3103 counted, so the census reads well above what
the relay did. OBS's own counter for this session, 3765 frames lagged to rendering, is direct evidence
of a lag of the same scale on the capture PC, but it carries no time placement and the marker reaches
only the first 34 s of video, so no frame outside that span is attributed to anyone. Per the standing
rule the excess is UNEXPLAINED at the frame level, and the capture PC's rendering lag, now 3.4%
against 1.0% on 09-16, remains the open item on the post-release list.

None of this touches the verdict in section 3, which rests on the relay log and the paired replay.

## Instruments

Repo-independent, in `/Users/gsparks/dev/frame-drop-analysis`: `runs.py`, `zonecross.py`,
`loghitches.py`, `marker.py decode --log --csv`, `mgdupes.py`, `pkdupes.py`. Session scratch scripts
used for the windowing and the exact join: `winruns.py` (blend-run histogram inside a log-time
window), `holdsvsmap.py` (holds against source stalls), `repeatjoin.py` (video counter repeats joined
to blocked presents in both directions, with the offset census as its control).
