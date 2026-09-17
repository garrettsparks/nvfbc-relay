# Field validation: the grab-timeout fix and the lock re-engage window

Build: branch `present-path-destination-lock` at `79f33e9` (re-engage convergence window) on top of
`3785f09` (grab-timeout copies not stored), `2ec96a7` (FRUC arm removed, `b:flip` gone) and
`22cebee` (the present-path seam). This is the first hardware run of any of it.

Captures analysed, all `b:vsync`, all recorded on the separate streaming PC with the marker on:

| # | content | flags | log | video |
|---|---|---|---|---|
| A | static screen and menus, then exit | `-src 60 -lock -lag 75 -etw -dejit` | `static_2026-09-16.log` | `static_2026-09-16.mp4` |
| B | Avatar benchmark, 3 runs | `-src 60 -lock -lag 75 -mark -etw -dejit` | `avatar_60x2_vsync_2026-09-16.log` | same name `.mp4` |
| C | Avatar benchmark, 3 runs | `-src 30 -lock -lag 75 -mark -etw -dejit` | `avatar_30x1_vsync_2026-09-16.log` | same name `.mp4` |

Two rows of the plan are user-reported rather than measured here: the exe started with no
`NvOFFRUC.dll` and no `cudart64_110.dll` beside it (all three captures above were taken that way,
which is the validation of `2ec96a7`), and `b:flip` exited with the usage list.

The Get Medieval gameplay capture landed the same night and is section 7: `Get_Medieval_2026-09-16_0`
(`b:vsync -src 60 -lock -lag 75 -etw -dejit -mark 7200`), 190100 presents over 52.8 minutes. It is
the capture that tests the re-engage window in its target regime, which Avatar cannot.

## 1. Did the grab-timeout fix do what it claims?

**Question.** `3785f09` stops the ring storing NvFBC's 100 ms timeout re-delivery as a new frame.
Two things have to be true: the skip has to fire, and no stored arrival may be a re-delivery.

**Method.** The startup line and the exit counter say whether the code is live. The signature of a
stored re-delivery is an arrival whose gap from the previous stored arrival sits at the timeout
length, so the test is the count of `capture` lines with `dt=` in 99-102 ms. That count only means
something against a control, so the same count was taken on pre-fix logs of the same game and the
same present path.

| log | captures | skips at exit | stored `dt=` 99-102 ms | stored `dt=` >= 50 ms |
|---|---|---|---|---|
| A static | 14962 | 62 | 0 | 15 (max 1.74 s) |
| B 60x2 | 37478 | 71 | 0 | 25 (max 1.37 s) |
| C 30x1 | 13326 | 38 | 1 | 25 (max 1.01 s) |
| control `avatar_c_b_flip60x2_gencheck` (pre-fix) | 15788 | n/a | 39 | 53 |
| control `avatar_c_b_sm60x2_gencheck` (pre-fix) | 18151 | n/a | 37 | 53 |
| control `avatar_c_b_flip60x2_lategrabsweep` (pre-fix) | 19202 | n/a | 59 | 88 |
| control `Get_Medieval_2026-09-13_0` (pre-fix) | 849931 | n/a | 360 | 506 |
| control `Get_Medieval_2026-09-14_0` (pre-fix) | 204059 | n/a | 102 | 142 |

The startup line `CaptureRing: a grab that waits out its 100 ms timeout stores nothing` is present
in all three logs (`CaptureRing.cpp:246`), and the ring is 32 slots at `-src 60`, 21 at `-src 30`.

**Reasoning.** On every pre-fix log, 70 to 74% of all capture gaps at or above 50 ms land inside the
99-102 ms band: that is the re-delivery, and it is the shape the fix targets. After the fix the band
is empty on A and B while the skip counter is non-zero, so the wakes still happen and no longer
enter the timeline. The remaining >= 50 ms gaps are real source gaps, and their maxima (1.0 to 1.7 s)
are loading screens and static stretches, which cannot be timeout artefacts because the timeout is
100 ms.

**The one event on C** is `capture #13136 arr=360042699us dt=99556us`. It is not a stored
re-delivery, and the code decides this rather than my judgement: the skip at `CaptureRing.cpp:312`
does `continue` before `policy::UpdateBatch`, and `lastArrivalTs` is only ever assigned inside
`UpdateBatch` (`TemporalPolicy.cpp:204`), so `dt=` measures from the last arrival that was actually
stored. A single grab that blocked 99.5 ms would itself have been skipped, so this gap spans either
a dropped wake plus a short real grab, or a grab that blocked just under the 95 ms floor after the
thread was descheduled. Either way the picture stored is a new one. Context supports a regime
change rather than a relay event: the neighbouring lines are a presentation-path change
(`COMPOSED -> OVERLAY` at present 21615, refresh counter re-based) with `pdt=33286us`, i.e. the
source stopped drawing across a test transition.

**Stall ramps.** The class the fix was meant to remove, a ramp of blends over a stall where the
"frames" were copies, does not appear. No blend run in any of the three logs reaches 50 presents
(section 2), and on C every synth run but four is a single present, which is the 2:1 alternation
rather than a ramp.

## 2. Did the re-engage window regress anything, and did it get tested?

**Question.** `79f33e9` opens the convergence window when the comb lock re-engages far from phase,
so that closing the gap does not walk the target between real frames at 25 us a present for a
hundred-plus presents.

**Method.** For each log: blend runs by length with their log time, lock transitions in both
directions, and for every re-engage the longest blend run starting within the next 300 presents plus
how fast `pull=` moved across it. Script kept as the session's scratch `runs.py`; it refuses a log
with under 100 parsed presents and prints its own op census so a parse failure cannot read as a
clean result.

| log | presents | synth runs 8-19 | 20-49 | 50+ | engages | disengages | re-engages followed by a run >= 50 |
|---|---|---|---|---|---|---|---|
| A static | 7398 | 30 | 12 | 0 | 4 | 4 | 0 of 4 |
| B 60x2 | 21272 | 14 | 6 | 0 | 17 | 17 | 0 of 17 |
| C 30x1 | 20973 | 4 | 0 | 0 | 25 | 25 | 0 of 25 |

Longest run in each: A 32 presents at 45.9 s with `lk=0`; B 40 presents at 281.85 s; C 13 presents.
B's is the interesting one and it is not steady gameplay: the inter-test fade sits at 281.1 s, and
across the run `lk` alternates 1/0/1 with `w` between 0.42 and 0.70, which is the lock refusing while
the source is off rate through a fade, not a converged lock sweeping. The longest run after any
re-engage is 14 presents (B, at 142.4 s), and `pull=` moves at 25.0 us/present there, the
steady-state slew. So the pre-fix failure shape (a re-engage followed by 100 to 230 presents of
blend at 25 us/present) occurs nowhere in 49000 presents.

**What this is not.** It is a clean no-regression result and almost no confirmation. The window can
only arm where half a comb exceeds the passthrough threshold, which needs a source near the sink
rate: B sits at 1:1 with the lock engaged 98.5% of in-band presents, and C is a 2:1 whose 25
transitions are the fade re-seeds. Avatar's benchmark never runs the case the window exists for, a
game drifting off 60 fps and settling again. Get Medieval is that case (its 09-13 log carried worst
run 226 at 60 Hz replay), so the window stands unconfirmed in the field until that capture.

## 3. The two pins

Per-benchmark-test windows via `benchtests.py` (marker CSV plus packet sizes for the fades; every
number gated on the source delivering at the declared cadence).

**B, `avatar_60x2_vsync_steady`: pin is 0 synth, 0 repeats.** Test 2, log 42.8..72.9 s, 1807
presents (the fixture cut from it opens later, below): synth 0.0%, holds 0, lock engaged 100.0%, content steps at 1.0 source
period 99.9%. The pin holds exactly. Across all nine tests: 17631 presents, synth 1.6%, holds 0,
lock engaged 98.5%, steps at 1.0 period 99.7%. Test 1 is the span before the recording opened and
carries 11.2% synth; it is not part of the pin.

**C, `avatar_30x1_vsync_steady`: pin is strict alternation.** Test 2, log 45.7..75.2 s, 1771
presents: synth 50.0%, midpoint presents 885 of which 100.0% blended, synth weights in [0.45, 0.55]
99.1%, runs of two or more reals 0, runs of two or more blends 0, holds 0, lock engaged 100.0%.
Strict RBRB for the whole test. Across all nine tests: 17828 presents, synth 49.8%, midpoints
100.0% blended, lock engaged 98.7%, holds 0.

Fixture windows in absolute log microseconds, as `mktrace.py` takes them:

| fixture | `--skip-us` | `--until-us` |
|---|---|---|
| `avatar_60x2_vsync_steady` | 42823102 (cut at 44823102, below) | 72936446 |
| `avatar_60x2_vsync_bench` | 42823102 | 345886531 |
| `avatar_30x1_vsync_steady` | 45735392 | 75252013 |
| `avatar_30x1_vsync_bench` | 45735392 | 351534965 |

The captures were cut as four new fixtures sitting beside the existing pins, not replacing them.
They are the corpus's first traces recorded by the loop that skips grab-timeout re-deliveries;
every other trace predates it. The suite passes with all four registered, 43 fixtures, 0 failures.

| fixture | presents | field | replay | bounds |
|---|---|---|---|---|
| `avatar_60x2_vsync_steady_0916` | 1686 | 0.0%, worst 0 | 0.1%, worst 1 | worst 1, long 0, synth 0.5%, reseeds 0 |
| `avatar_60x2_vsync_bench_0916` | 18184 | 2.0%, worst 40 | 2.4%, worst 47 | worst 47, long 0, synth 2.8%, reseeds 3 |
| `avatar_30x1_vsync_steady_0916` | 1651 | 50.0%, worst 1 | 50.0%, worst 1 | worst 1, long 0, synth 50.4%, reseeds 0 |
| `avatar_30x1_vsync_bench_0916` | 18348 | 49.8%, worst 4 | 49.9%, worst 3 | worst 3, long 0, synth 50.3%, reseeds 0 |

### The 60x2 steady window exposed a replay-model cold start, and that cost a window shift

Cut where `benchtests.py` puts it, 0.3 s after the fade into the test ends, the steady fixture replayed
at 3.8% synth with a worst blend run of 47 presents against a field that blended NOTHING on that
window. The suite's own instruction is that bounds on a material disagreement are theatre, so the
disagreement was chased rather than bounded, and it is the model's, not the relay's:

- ReplayHarness, which drives the real linked policy over the same window with the ring the capture
  actually ran (32 slots), reports 0 synth, 0 holds, before-stamp exact 99.83%, and a predicted-lock
  target error of 0.60 us mean and 2.7 us max. The field and the real policy agree exactly.
- Ring size cannot produce it: the same harness window at ring 16 also reports 0 synth, and ring 8
  starves into 1739 holds with no blends at all. The suite's `SimParams` has no ring field, so this
  was the one thing it provably cannot express and it is not the cause.
- Window start is the cause. Three traces ending at the same log time, differing only in start:
  42.8 s replays 3.8% / worst 47, 44.8 s replays 0.1% / worst 1, 47.8 s replays 0.1% / worst 1.

So the model's cold-start convergence on this capture outlasts the fixed 200-present warmup that
`mktrace.py` and the test share, and at a 0.3 s margin its tail lands inside the counted region. The
fixture therefore opens 2.3 s after the fade ends, once the benchmark is fully running, and still
covers 28 s of one steady test. The 30x1 steady row was re-cut to the same margin even though it
replayed exactly at the default one, so both steady fixtures open where the benchmark is fully
running and neither can fail later for a cold-start reason that is not the relay's. The warmup
constant itself was left alone: raising it would move every fixture's counted region at once, which
is a corpus-wide bound change, not a fix for one window.

On the flag: the four existing Avatar traces do not carry `regrab_copies 1` even though they were
recorded before the fix, because they were cut before `mktrace.py:148` learned to write it. That
omission is inert for them, which was worth checking rather than assuming: neither steady window
contains a single arrival gap in the 99-102 ms band, and the two bench windows hold 2 and 4 gaps at
or above 50 ms in 31860 and 9102 arrivals, so the loader has nothing to drop in them either way.

## 4. Video: the marker says the chain was clean on these runs

`marker.py decode` over the whole of each recording, cross-checked against its log.

| video | frames | marker + checksum | counter repeats (downstream dupes) | skips | provenance agreement |
|---|---|---|---|---|---|
| A static | 3652 | 3652/3652, 100% | 0 | 0 | 3652/3652 |
| B 60x2 | 18446 | 18446/18446, 100% | 1 | 0 | 18446/18446 |
| C 30x1 | 18616 | 18616/18616, 100% | 0 | 0 | 18616/18616 |

B's single repeat is at 2:37.40 (video frame 9444, counter 11971 twice), content-identical at
MAD < 0.15 against a motion median of 3.9, and the log shows a plain `op=pass-after` there. That is
one downstream duplicate in 18446 frames, 0.005%, against 2.54% on `Get_Medieval_2026-09-13_0` and
2.74% on `Get_Medieval_2026-09-14_0`. Zero presents went missing from any of the three files.

This is evidence about the chain, not about the relay: the same OBS and capture-card path that
duplicated 2.5% of Get Medieval frames duplicated 0.005% of Avatar's. Whatever produces the Get
Medieval duplicates is not a property of this build, and the pending gameplay capture should be read
with that in mind.

Marker offsets (log time of the first decoded counter, which is video frame 0 of each recording):
B 42.020 s (counter 2528), C 44.919 s (counter 2707), A 57.565 s (counter 3427). A's recording
covers presents 3427 to 7078 of 7398, so its blend activity sits mostly outside the video.

## 5. Log behaviour and the health lines

**`no after-frame` is per episode, by construction.** `TemporalCaptureMode.cpp:616` logs only when
`m_noAfterRun == 0` and pairs with `after-frame back after N presents` at line 621. A shows 11
episodes and 11 back-lines over 7398 presents, the longest 98 presents (1.6 s). The static run
therefore proves the per-episode behaviour up to 1.6 s of no new source frame, not longer: the menu
kept redrawing, so a true minute-long freeze never happened.

**Exit is immediate.** A's last present is at 123.765 s, the last capture at 123.767 s, then
`Cleanup started`, `NvFBC library closed`, `Cleanup completed`, end of file.

| measure | A static | B 60x2 | C 30x1 |
|---|---|---|---|
| presents | 7399 | 21275 | 21774 |
| refreshes with no new frame | 10 (0.081/s) | 44 (0.124/s) | 18 (0.050/s) |
| presentation path | 99.4% overlay | 99.4% overlay | 99.4% overlay |
| present gaps >= 45 ms | 3 (max 50.0 ms) | 3 (max 66.6 ms) | 2 (max 66.6 ms) |
| `jit=` p50 / p95 | 17 / 34 us | 21 / 36 us | 16 / 28 us |
| `flush=` p50 / p95 | 97 / 165 us | 58 / 138 us | 97 / 195 us |
| dejit late / corrected | 432 / 432 | 272 / 271 | 406 / 8 |

The 50 ms present gaps that `Get_Medieval_2026-09-13_0` showed at 14.2/h are effectively absent
here (three events in six minutes on B, two on C), consistent with them being a hitch-driven
behaviour rather than a steady-state one.

One line worth understanding before the defaults are set, not a defect on this evidence: dejit
corrected 271 of 272 late batches on B but only 8 of 406 on C, declining 398 to the lock. The
mechanism was not chased in this pass.

## 6. Verdicts

| claim | verdict |
|---|---|
| grab-timeout copies no longer enter the timeline | CONFIRMED, against a positive control on the same content |
| the skip does not eat real frames | supported: skips 38-71 per run, no stored gap in the band, no blend ramps |
| exe runs with no NvOFFRUC.dll or cudart64_110.dll present | user-reported across all three captures |
| `b:flip` refused with usage | user-reported |
| `no after-frame` logs per episode | CONFIRMED in code and in the log, up to 1.6 s episodes |
| re-engage window causes no regression | CONFIRMED on 49000 presents, 46 re-engages, no run >= 50 |
| re-engage window fixes the long runs | CONFIRMED in the field on Get Medieval: worst run 226 -> 65, runs >= 50 13 -> 4, re-engages followed by a 50+ run 3 of 46 -> 1 of 17 (section 7) |
| `avatar_60x2_vsync_steady` pin (0 synth, 0 repeats) | HOLDS |
| `avatar_30x1_vsync_steady` pin (strict alternation) | HOLDS |
| video duplicates are downstream | supported again: 1 dupe in 18446 frames on the same chain that gave 2.5% on Get Medieval |
| the replay model tracks this capture | YES once the window clears its cold start; the 200-present warmup is too short at a 0.3 s fade margin, which is a model limit worth remembering when any future fixture is cut |

## 7. Get Medieval, 2026-09-16: the gameplay capture

`Get_Medieval_2026-09-16_0.log` plus the stream-side video. 190100 presents over 52.8 min, ring 32,
marker on the first 7200 presents. The video is HEVC 2560x1440 at 60 fps, 185221 frames over
3087 s, and it did NOT come from a local recording: the OBS session logged only
`rtmp multitrack video`, so the file is the stream path, as 09-13 and 09-14 were.

### 7.1 Marker offset, and what it can and cannot reach

Video frame 0 carries counter 4199, which the log presents at `dl=70389563us`, so **log time = video
time + 70.390 s**. The marked span is marks 4199 to 7200, which is only the first 50 s OF VIDEO.
Counter-based attribution therefore covers 50 seconds, not the session: over the whole file the
confirmation pass could decode a counter for 41 of 33057 content-identical pairs. In the marked
window the marker is unambiguous: 3004 frames, 100% checksum, provenance 3004/3004 agreeing with
the log's own `op=`/`bw=`, 13 counter repeats (all inside 0-30 s) and 10 log counters that never
reached the file. **If whole-session video attribution is wanted, `-mark` has to cover the session.**

### 7.2 The grab-timeout fix, against its pre-fix control

| | 09-13 (pre-fix) | 09-16 |
|---|---|---|
| captures | 849931 | 371108 |
| stored gaps in 99-102 ms | 360 | 4 |
| largest capture gap | exactly 100 ms | 288 ms in-body (1.74 s with desktop) |
| in-band holds | none (43 `hold-comb` only) | 338 |
| source gaps >= 25 ms per minute | 6.7 | 3.6 |

The 100 ms ceiling on 09-13 is the artefact itself: a timed-out grab stored the re-delivered picture
and reset the arrival clock, so no gap could ever be recorded longer than the timeout. After the fix
the true gaps are visible, and the relay holds through them instead of presenting a stale picture as
new. That is why holds go UP while the source hitches LESS: the same events now cost an honest hold
rather than a silent stale frame. The four remaining 99-102 ms gaps each span a dropped wake, which
the code forces (the skip returns before `UpdateBatch`, and `lastArrivalTs` only advances on a
stored arrival).

### 7.3 The re-engage window, tested in its own regime

Same instrument on both logs, same present path (`b:vsync`), both at release settings:

| | 09-13 (2.0 h, pre-window) | 09-16 (0.88 h) |
|---|---|---|
| lock engages | 46 | 17 |
| engages followed by a run >= 50 | 3 | 1 |
| blend runs >= 50 | 13 (6.5/h) | 4 (4.5/h) |
| worst blend run | 226 | 65 |
| blend runs >= 8 | 139 (1.16/min) | 62 (1.17/min) |
| in-band synth share | 1.1% | 0.9% |
| lock engaged | 99.5% | 99.0% |

The rate of ordinary blend runs is unchanged; what the window removes is the TAIL. The replay sweep
predicted 09-13's worst run would fall 226 -> 63 with the window, and a fresh field capture came in
at 65.

Source delivery in the 6 s before each remaining long run: 59.00, 59.83, 60.00 and 59.67 batches/s.
Three are the drift-off-60 regime the window targets. The fourth, at 2433.4 s, is the RESIDUAL
class and not a re-engage failure: `lk` stayed 1 with no re-engage while the pull slewed steadily at
21 us/present from 13011 to 11267 us, until the target crossed into the blend zone. That is the
phase-step case already queued behind the release, and the window by construction does not touch it.

### 7.4 Health lines

In-band (52.5 min): synth 0.9%, holds 0.2%, lock engaged 99.0%, content steps at 1.0 source period
99.7%, midpoint presents 98.3% blended. Whole run: 63 refreshes showed no new frame over 190100
presents (0.020/s), presentation path 99.9% overlay with 2 changes, dejit 1169 late / 1148 corrected
/ 21 lock-declined, ETW 570021 flips with 0 decode failures, present gaps >= 45 ms 11 (12.5/h
against 09-13's 14.2/h), `flush` p50 97 / p95 187 us, `jit` p50 20 / p95 30 us.

### 7.5 The video: what is in the file, and who put it there

**Frame accounting first.** 185158 log presents fall inside the video's span against 185221 frames in
the file: 63 frames MORE than the relay presented, which is CFR 60.00 padding against the relay's
59.98/s. Nothing was lost.

**Then instrument validity, because the obvious measure is wrong here.** Packet size shortlists
duplicates with perfect recall: all 13 marker-confirmed repeats are 367-401 byte packets against
neighbours of 780-2500, caught at 45% of a local median. Its PRECISION is poor, and its controls say
so: 2.04% of frames flagged on the Avatar recording whose marker proves 1 repeat in 18446 frames,
and 20.7% on the all-static capture whose marker proves zero. Static content is small-packet content.
So packet rates are upper bounds only. The measure with working controls is the motion-gated MAD
test: 0.00/s on the static capture, 0.12/s on the Avatar recording.

Motion-gated duplicates in 60 s windows:

| | 09-13 (12 windows) | 09-16 (15 windows) |
|---|---|---|
| typical | 0.00-0.55/s | 0.18-0.42/s |
| worst | 0.72/s | 5.13/s at 1020 s, then 2.85 / 2.03 / 1.37 / 1.12 |

**The baseline is the relay's own holds and is not a fault.** 646 holds across the span is 0.21/s,
which is the 0.18-0.42/s baseline. Those are honest re-presents where the game drew nothing: map
opens, menus and static screens, exactly the frame-time spikes this game is known for.

**The elevated stretches are downstream, on three independent channels.** Exact 60 s log windows
against the video windows they cover:

| window | relay presents | holds | source gaps >= 25 ms | ETW head-1 scanout | video dupes |
|---|---|---|---|---|---|
| video 2400 s | 3597 | 21 | 6 | 3596 flips, 4 gaps > 20 ms | 1.37/s |
| video 2700 s | 3600 | 0 | 4 | 3599 flips, 2 gaps | 2.03/s |
| video 3000 s | 3600 | 0 | 0 | 3599 flips, 0 gaps, max 16.7 ms | 1.12/s |
| control 600 s | 3595 | 48 | 13 | 3594 flips, 10 gaps | 0.38/s |

The video-3000 minute settles it: the relay presented 3600 frames, held nothing, the source never
hitched, its scanout was a metronome, and the file still duplicated 67 frames. The control minute
carries MORE relay activity and FEWER video duplicates.

**Two duplicates the user found by eye** (video frames 22836 and 22845, log 450.990 s and 451.140 s)
are the same story at single-frame resolution. Across 450.80-451.35 s the relay made 33 presents,
every one `pass-before` or `pass-after` of distinct consecutive source frames, zero holds, zero
synth, `lk=1`, `w` either 0.93-1.00 or 0.007-0.06, so the target sat ON real frames and there was
nothing to blend. The packets say who duplicated: 1104 and 1220 bytes at those two frames against
8.5-19 KB neighbours, each followed by an oversized frame (15211 and 19232 bytes) carrying two
frames of motion. That pair, an empty frame then a double-motion frame, is a compositor repeat and
catch-up. A blend there would have been the defect, softening motion that was correctly sharp.

**OBS's own counters agree.** This session: 1830 frames lagged to rendering (1.0%), against 762
(0.2%) on 09-13 across twice the duration, with encoding-lag skips of 7/185355 (0.0%) and no network
drops. The rendering-lag counter is the only one that moved, it is cumulative and cannot be placed
in time from the log, and it is the right order of magnitude for the excess duplication measured
above.

## 8. Release readiness

The relay side of this build is better than 09-13 on every metric that moved and clean in absolute
terms: the tail of long blend runs cut 226 -> 65, runs >= 50 halved per hour, synth share down,
frames lost to the file zero, no dead-swapchain hang, clean exit, ETW without a single decode
failure, and the suite green at 43 fixtures. The grab-timeout fix is confirmed against a positive
control, and the re-engage window is now confirmed in the field rather than only in replay.

Two open items, neither of which blocks the cost ladder or the defaults decision:

1. **The residual blend-zone crossing.** Four runs >= 50 presents in 52.8 minutes, one of them with
   the lock engaged and no re-engage at all (7.3). This is the queued phase-step item; the defaults
   decision does not change it either way.
2. **The capture PC duplicated 2-7x more than on 09-13** and the cause is not identified. It is
   downstream of the relay on every channel measured, so it does not gate the build, but it does
   mean this video is a poor baseline for judging output quality, and it should be chased before any
   release claim about stream quality rests on a recording from this machine.

So this build is the right one to run the relay-cost ladder on and to decide the release defaults
from. It carries the release shape already (`b:vsync` the default present path, `b:dwm` the D3D9
path, `b:flip` refused, no NvOFFRUC dependency, subgen and fgphase gone), and the only uncommitted
work is fixtures and documentation, which do not touch the executable.
