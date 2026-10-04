# The sample check as referee: is the driver's change map true under DLSS-G?

Capture 2026-09-09 (`avatar_c_b_flip60x2_gencheck`), analysis 2026-09-10. Relay build 48faa8a
on `etw-frame-timing`, the first build with a working `-gencheck`. Avatar 60x2 with DLSS frame
generation, one benchmark run of three tests, `b:flip -src 60 -lock -lag 75 -mark -etw -dejit
-subgen -gencheck`. The spec is `docs/gen-frame-sample-check-spec.md`.

## The question

Under DLSS frame generation the driver's change map reports zero changed blocks between the
two members of a capture batch on 99.3% of batches, and the ring refuses those second members
as duplicates, so `-subgen` almost never has a generated frame to substitute. Is the map
telling the truth (the generated frame never reaches NvFBC, and the second grab really is the
same picture twice), or does it report zero on any repeat grab inside a burst regardless of
content? The recording cannot say, because refused members never reach it. The sample check
reads 256 texels of every grab on the capture device and compares them word for word, beside
the map, deciding nothing.

## Answer

The map is true. On 4983 second members where the driver reported zero changed blocks, the 256
samples found the picture identical to the first member's on every one. Zero exceptions.

| second members with a change map: 5018 | samples same | samples differ |
|---|---|---|
| driver blocks = 0 (dupe) | 4983 | **0** |
| driver blocks > 0 (change) | 13 | 22 |

The 22 where both saw a change are what a working referee should show: 16 small changes (49
to 565 blocks, 1 to 13 samples differing, a HUD counter or particles), and 6 where the whole
frame changed (8160 blocks, 230 to 256 samples), which is the second grab landing after the
next real flip. The 13 where the driver saw a change the samples missed sit at 78 to 496
blocks, one to six percent of the frame, which is the sampler's designed blind spot and
nothing else. Wherever the sampler can see, the two instruments agree; where the map says
duplicate, the pixels are duplicate.

This was measured in motion: consecutive real frames differed on a median 248 of 256 samples
across the tests. The generated frame does not reach NvFBC under DLSS frame generation. The
substitution path is dead on this capture path by supply, not tuning, and the release default
of `-subgen` off stands on evidence rather than suspicion.

## How the display can show generated frames the capture never delivers

Frame generation is visibly working on the game's monitor, and under Smooth Motion the relay
has captured generated frames often enough to see their garbled HUD when keep-real picked the
wrong member. Both of those are true and neither contradicts the result above, because the
two generators put their frames into the display pipeline at different points and the capture
engine is notified differently. In a gameplay window of this run (log 50 to 60 s):

| | per second |
|---|---|
| head-0 display flips (ETW scanout times) | 120.0 |
| NvFBC wakes | 115.8 |
| distinct pictures delivered | 60 |

The display flips twice per real frame: the generated frame is on the monitor. NvFBC wakes
about twice per real frame too, but not once per flip: both wakes land 0.5 to 1.2 ms after ONE
of the two flips, 0.67 ms apart, and the other flip in the period wakes nothing. The two grabs
copy the same picture, exactly, 4983 times out of 4983. So under DLSS frame generation at x2
the capture engine is notified twice for each real frame and delivers the real frame twice;
the generated frame's later, metered scanout produces no wake of its own, and the second grab
has already happened by the time it is up. Under Smooth Motion the driver generates the
frame on the display side and presents it through the same path as the game's frames, the
capture engine wakes on it and copies it, and the second member of a pair is a distinct
generated frame on 92% of batches. That difference, 7.5% duplicates against 99.3%, was the
first sign; the sampler now says the 99.3% are real duplicates and not a lazy map.

Neither the change map nor the sampler can see the display. They measure what NvFBC delivers,
and what it delivers under DLSS frame generation is the base rate of real frames. That is why
the relay's output at 60x2 has never shown the generated-frame HUD artefacts Smooth Motion
produces, and why substitution has nothing to substitute.

Outside reports describe the presentation side: since DLSS Frame Generation 310 the
interpolated frames are presented through the display engine's hardware flip metering,
bypassing the desktop compositor, which is why OBS's display capture, DXGI Desktop Duplication
and Windows Graphics Capture record the base rate only (dated to the 580-series drivers) while
ShadowPlay, inside the driver, records the generated frames.

That does not make the generated frames unreachable for NvFBC, and the corpus says so. A
second grab that arrives 1 to 2 ms after the real frame can only hold two pictures, the real
frame or the generated frame built from it; the next real frame does not exist yet at 60 fps.
When such a grab returns a different picture, the driver's change map on the grab that follows
it (the next real frame against that picture) says which:

| capture | distinct later members | next real frame differs by 6000+ blocks | by 0 blocks |
|---|---|---|---|
| Avatar 60x2 DLSS-G, this run | 5 | 5 | 0 |
| Avatar 60x2 DLSS-G, earlier subgen run | 33 | 31 | 1 |
| Avatar 60x3 DLSS-G | 3341 | 3340 | 0 |
| Avatar 90x2 DLSS-G | 2895 | 2288 | 593 |
| KCD Smooth Motion | 66981 | 66907 | 9 |

A distinct later member is a generated frame essentially every time, under both generators.
The difference between DLSS-G and Smooth Motion at the capture is therefore not routing but
timing: under Smooth Motion the generated frame is already in the buffer when the pair's first
grab lands (the pair arrives generated first, real second, 1.8 ms apart); under DLSS-G at x2
both grabs land within 1.2 ms of the real frame's present, before the metered generated frame
is up, so the second grab copies the real frame again and only a late grab (1.1 to 1.9 ms
against 0.67 ms typical) catches the generated one, 5 times in 5018 here. At x3 the metered
flips come every 5.5 ms and the later members of a batch land after them, so 3340 generated
frames were captured in one run. The earlier x3 finding, that distinct later members arrive
late, is this mechanism seen from the other side.

Two consequences. First, the change map is still true: it said duplicate only where the
pixels were duplicate. Second, the supply problem at x2 is a race, not a wall. The five catches
landed 1.7 to 2.7 ms after the real frame's flip event, so the generated frame is in the
buffer within a couple of milliseconds of the real frame's present, long before its metered
scanout 8 ms later; a third grab issued without waiting for a notification, 2 to 3 ms after
the first member, would be expected to return it. That is untested, and it is the only lever
that does not touch the sink's independent flip (the published registry workaround that forces composition is global
and would return the compose-clock repeats the flip path removed). The consumer is the
substitution path at every ratio where the relay must synthesize, and the clearest case is not
a slow source but 90x2: the 180 per second displayed stream lands on the 60 Hz grid exactly,
every third frame, alternating real and generated, so the generated frame would replace the
half-blend that the 3:2 threshold work made the relay produce, a sharp interpolated picture
where today there is a double image. At 60x2 into 60 Hz nothing is gained, because every real
frame already lands on the grid.

One timing observation stays open: a flip event landed between the two grabs of a pair on 784
of the 4897 duplicate pairs with the pixels still identical. Under hardware flip metering the
flip event the relay records is plausibly the submission to the metering queue rather than the
scanout itself, in which case there is no contradiction; it is noted, not resolved.

## Whether the instrument can be believed

The first field run answered "same" on nothing at all: its self-test compared two different
frames because of an ordering bug in the self-test, and disabled the instrument on the second
wake. The controls exist for exactly that, and they were kept and made fatal (a failure stops
the relay rather than letting a capture run without its instrument). On this run:

- **Self-test:** the one same-slot double gather that completed read identical on 256 of 256.
  The test retired only one of its three passes because of a second scheduling flaw in the
  test itself (a spare gather drawn for the next wake's slot was discarded on the current
  wake's mismatch); fixed after this run, and the one completed pass is the evidence.
- **Degenerate gathers:** 19 of 15787 rows were a single value, all in the black fades.
- **Positive control:** batch-to-batch difference median 248 of 256 in the tests. The
  sampler reads the frame, and reads a different frame every batch.
- **Both-saw-change pairs:** 6 full-frame differences detected at 230 to 256 samples. The
  sampler detects a distinct second member when one occurs.
- **Recording cross-check:** the marker decoded on all 5935 frames, zero counter repeats,
  zero skips, 100% agreement between the marker's synthesized flag and the log. The
  pixel-side scan found zero same-picture consecutive presents in moving scenes.

## The late-grab experiment, and what it taught instead

Capture `avatar_c_b_flip60x2_lategrab1000` (build 6b7b46d, one bench run): after every
batch's second member the capture thread slept 1000 us and issued one no-wait grab into a slot
outside the ring. 5209 grabs, landing 1.67 ms after the second member: 5200 returned the same
picture as the second member, 0 returned a generated frame, 5 returned the next real frame
(it arrived during the sleep), 4 ambiguous. The self-test passed 3 of 3, the marker decoded
every frame of the recording with zero skips, and the output was unchanged. So a no-wait grab
does not take a fresh copy of the display; it returns the last notified frame. Polling cannot
reach the generated frame.

The same logs then answered the better question: when a NATURAL second grab did return the
generated frame, how late had that grab executed after the first member? From the change map
on natural second members, gameplay windows, four DLSS-G captures:

| second grab executed (dt after the first member) | 60x3: n | generated | 60x2 (three runs pooled): n | generated |
|---|---|---|---|---|
| 400 to 800 us | 11921 | 3.4% | 23334 | 0.04% |
| 800 to 1000 us | 124 | 19% | 637 | 2.0% |
| 1000 to 1200 us | 34 | 29% | 116 | 7.8% |
| 1200 to 1400 us | 194 | 97% | 35 | 2.9% |
| 1400 to 2000 us | 2617 | 99.7% | 139 | 15% |

At 60x3 the transition is a wall at about 1.2 ms: a second grab that executes later than that
returns the generated frame essentially every time. The second notification IS the generated
frame's present; the copy NvFBC takes when the grab answers it holds the real frame if it runs
early and the generated frame once the generation pass has landed. Our loop normally answers
0.67 ms after the first member, which is why the pair reads as two copies of the real frame,
and why the rare late answer caught the generated one. At 60x2 the rise is present but
shallower and the samples past 1.2 ms are few; x2 may need more delay than x3.

That suggested delaying the grab that answers the second notification. `-grabdelay` does
that: after a batch's first member is processed, the capture thread sleeps before calling the
next grab, so the pending second notification is answered late. The first member stays the
keeper and the delayed member is discarded, so the screen shows exactly today's real frames
while the change map scores the delayed copy. `-grabdelay sweep` cycled eight delays batch by
batch so one capture mapped the window.

Capture `avatar_c_b_flip60x2_lategrabsweep` (one bench run, 4777 delayed second members,
self-test 3 of 3, zero skips in the recording, output unchanged). The change map of the
delayed second member against the first, by when the second grab executed:

| second grab executed after the first member | n | identical | 1 to 100 blocks | generated frame (7000+) |
|---|---|---|---|---|
| 600 to 900 us (the control) | 141 | 90.8% | 8.5% | 0.0% |
| 1200 to 1500 us | 719 | 94.3% | 5.3% | 0.0% |
| 1500 to 1800 us | 727 | 93.4% | 5.5% | 0.4% |
| 1800 to 2100 us | 720 | 93.3% | 5.4% | 0.6% |
| 2100 to 2400 us | 755 | 92.2% | 6.2% | 0.8% |
| 2400 to 2700 us | 774 | 91.6% | 6.7% | 0.8% |
| 2700 to 3000 us | 673 | 94.1% | 5.1% | 0.4% |

No window. Answering the second notification anywhere up to 3 ms after the first member
returns the real frame again at the same rate as answering it at once, and the generated
frame appears at the same half a percent it always did. So the picture a grab returns is fixed
when the notification is issued, not when the grab executes. The natural late grabs that
returned generated frames were late because the notification itself was late, which happens
on the odd batch at x2 and routinely at x3, where the later presents are issued after their
generation passes complete. Neither of the two levers this side of the API can change that:
polling returns the last notified frame, and a delayed answer returns the notified picture.

The supply of generated frames through this capture path is therefore whatever the driver's
notification timing yields, which is about 0.1% of batches at 60x2, about 11% at 90x2 and
most of them at 60x3, and none of it is under the relay's control. The substitution path
stays dormant under DLSS frame generation at x2 for that reason, and the two experiment flags
stay in the tree as the record of why.

## Smooth Motion on Avatar: the generator is not the variable

Capture `avatar_c_b_sm60x2_gencheck`, one bench run, in-game frame generation off and the
driver's Smooth Motion on at x2, otherwise identical to the DLSS-G referee run. The capture
layer in a gameplay window, against the DLSS-G run and against the KCD Smooth Motion run of
2026-08-29 on the same card:

| | Avatar, Smooth Motion | Avatar, DLSS-G | KCD, Smooth Motion |
|---|---|---|---|
| head-0 display flips | 120.0/s | 120.0/s | 120.0/s |
| NvFBC wakes | 114.8/s | 115.8/s | 118.5/s |
| intra-pair spacing, median | 690 us | 682 us | 1843 us |
| second member is a different picture | 0.2% | 0.0% | 96.2% |
| sampler vs change map | 6850 dupes, 0 disagreements | 4983 dupes, 0 disagreements | |

Frame generation was running (two flips per real frame) and the capture engine was notified
twice per real frame, and both grabs held the real frame, exactly as under DLSS-G. So the
generator does not decide whether the generated frame reaches NvFBC. Either Avatar's
presentation path routes both generators' frames the same way, or a driver change between the
KCD run and this one moved Smooth Motion onto the same metered path; twelve days and one game
separate the two measurements and either could be the variable. A KCD Smooth Motion run on
today's driver separates them: 96% again means the game, near zero means the driver.

The generator for this run was Smooth Motion, confirmed by the person who set it up; the
capture layer could not have told, because the two Avatar patterns are indistinguishable
there, and no benchmark CSV was kept whose fps column would have read 60 rather than 120.

## KCD under Smooth Motion today: unchanged since August

Capture `kcd_c_b_sm60x2_gencheck`, today's driver, driver-side cap at 120. Windowed to the
marked recording, which runs from 72.5 to 210 s of the log and is all gameplay:

| inside the recording, 72 to 210 s | |
|---|---|
| display flips | 120.0/s in every ten-second window |
| first members | 60.0/s, held by the driver cap |
| intra-pair spacing | 700 to 760 us |
| second member a different picture | 6573 of 7524 pairs, 87.4% (80 to 92% per window) |
| sampler against change map | 7524 of 7524 agree; 951 duplicates, 6573 changes, 0 disagreements |
| batch-to-batch repeats | 0 of 8266 first members |
| substitutions inside the recording | 0 (the 1:1 lock never blends at 60x2) |

This is also the spec's second referee measurement, the one where the second member really is
a different picture: the sampler agreed with the change map on every one of 7524 pairs, so it
sees a generated frame whenever one is there, and its zero-disagreement verdict on Avatar was
not an instrument that says "same" to everything.

That is the KCD pattern of the August run, so the driver has not moved Smooth Motion onto the
metered path; the one thing that changed since August is the intra-pair spacing, 700 us
against 1840, which is a driver or app change and does not touch the supply. The 779
substitutions in the exit summary happened before the recording, in the sub-rate menu and
loading segments, and are not gameplay evidence. Neither is the pre-recording phase itself,
though it is worth a line: from 25 to 60 s the game rendered its menu or intro with generation
on at 58 per second, and there the second member was a duplicate at every batch period, the
Avatar pattern, which is at least consistent with the game's own cap being in charge there.

With generator and driver excluded, the next candidate was the limiter: Avatar under its
in-game cap at 60 against KCD gameplay under the driver cap at 120.

## Avatar under Smooth Motion with the driver cap: the limiter is excluded too

Capture `avatar_c_b_sm60x2_drivercap_gencheck`, in-game limiter off, driver cap at 120,
Smooth Motion x2, windowed to the recording (48.6 to 146.7 s):

| inside the recording | |
|---|---|
| display flips | 120.0/s in every window |
| first members | 60.0/s, batch period 16.66 ms median, the same locked cadence as KCD gameplay |
| second member a different picture | 13 of 5429 pairs, 0.0% in every window |
| sampler against change map | 5426 of 5429 agree, 0 disagreements where the map said duplicate |

The driver's limiter was holding the frame exactly as it holds KCD's, and the generated frame
still did not reach the capture. Generator, driver and limiter are all excluded. What remains
is the game's presentation path: the present mode Windows grants it (independent flip,
multiplane overlay, hardware composed, or composed), whether its swapchain is HDR (Avatar runs
HDR and the relay captures it as ten-bit HDR; KCD is presumably SDR), and the API and window
mode. None of that is visible in the relay's log. The next instrument is PresentMon on the game
itself during gameplay, Avatar and KCD side by side, reading the present mode column, together
with the HDR state of each. No relay code is involved, and no relay-side or configuration-side
lever tested so far recovers the supply on Avatar.

## PresentMon on the game: the mechanism from the operating system's side

`pm_avatar_dlssg_60x2.csv`, PresentMon capturing Avatar's own process under DLSS-G at x2, 107
seconds, steady state from 50 s on (8397 presents):

| | |
|---|---|
| present mode | Hardware: Independent Flip, 100% (no overlay plane, not composed) |
| present spacing | pairs: 0.175 ms between the two presents of a pair, 16.48 ms between pairs |
| display change | 8.32 ms median: every frame shown once at 120 Hz |
| flip delay, first present of a pair | 0.00 ms, flips at once |
| flip delay, second present of a pair | 8.45 ms, held by the hardware flip metering |
| sync interval / tearing | 0 / allowed |

This is the capture-side picture seen from the other end. The game submits the two frames of a
period within 0.175 ms of each other; the first flips immediately and the second is held by the
metering hardware for half a period. NvFBC wakes on both submissions, 0.5 to 1.2 ms later, when
only the first has flipped, so both grabs return the same picture, and the held frame's flip
half a period later announces nothing. Avatar is not on an overlay plane during gameplay, so
the composed-flip workaround would not even apply here.

And KCD under Smooth Motion, `pm_kcd_sm_60x2.csv`, 12969 presents in steady state, looks the
same from the operating system's side in every column that matters: Hardware: Independent
Flip on 100%, presents in pairs 0.175 ms apart and 16.5 ms between pairs, display change 8.34
ms, the first present of a pair flipping at once and the second held 8.21 ms by the metering
hardware, sync interval 0, tearing allowed. The one difference is how long after its present
call each frame reaches the screen, 3.3 and 11.4 ms on KCD against 8.25 and 16.4 on Avatar,
which is pacing, not routing. So the game whose second member NvFBC delivers as a distinct
generated frame and the game whose second member it delivers as a duplicate present
identically as far as Windows can see. The split is inside the driver's capture engine, in
what the second notification lets it copy before the held flip, and across every variable
tested today it correlates with the game and nothing else: not the generator, not the driver,
not the limiter, not the present mode. What is left on the game's side is what PresentMon does
not show, the swapchain's format and bit depth, runtime details, buffer count. Avatar likely
presents ten-bit even in SDR and KCD eight-bit, which is the last concrete difference on the
table. The relay's decision does not depend on it: substitution is live under Smooth Motion on
KCD and dormant on Avatar under either generator, and no lever the relay or its configuration
controls has moved that.

It also raises a question the earlier sections assumed an answer to. Interpolation-based frame
generation displays the generated frame before the real one it interpolates towards, and the
frame that flips immediately on submission is the one the capture copies. If the copy is of
what is on screen, the relay has been receiving the generated frames under DLSS-G on Avatar
and the real frames are the unreachable ones, the reverse of the description above; the output
would look clean either way, because DLSS-G masks the interface, unlike the driver-side
generator whose garbled interface KCD exposed. KCD under Smooth Motion argues the other way,
since both of its members arrive distinct, which fits a copy of the presented buffer rather
than the screen. Neither reading is proven and nothing in the relay's behaviour depends on
which is true at 60x2, where one frame per period is shown either way; it matters for what a
captured "real" frame's content time is, and it is recorded here as open.

## The batch-to-batch counter

The whole-log figure (1546 repeats of 10768 first members) is the desktop and the loading
screens, where consecutive grabs are legitimately the same picture, plus the black fades. In
the tests, with a motion gate (the surrounding batches differing on more than 32 samples),
the count is 4 in 2.1 minutes, all at fade edges. A 60 fps base that holds its rate does not
deliver the same picture twice; the 47 in five minutes seen on 90x2 belong to a base that
could not hold 90. The counter works, and it needs the gate; the same rule the video-side
scan needed.

## Cost on the capture thread

| | instrument on | reference (same flags, no instrument, 2026-09-06) |
|---|---|---|
| batches with a second member | 67.3% | 71.0% |
| intra-pair spacing, median / p95 | 666 / 825 us | 583 / 710 us |
| flush, median / p95 | 150 / 211 us | 118 / 184 us |
| readback, median / p95 / worst | 33 / 78 / 11880 us | |

The gather adds about 30 us to the flush and the readback about 33 us to the wake, and the
run paired 3.7 points fewer batches than the reference. The reference is a different day, and
pairing varies day to day by a few points, so this is a bound rather than a measurement; a
same-session on/off pair would settle it. It is small, but it is not nothing, and it is on the
capture thread between pair members.

## What this changes

- The DLSS-G question is closed. Subgen has no supply on this path; the release default stays
  off, and the subgen code can be treated as Smooth Motion only.
- The remaining referee capture (KCD, Smooth Motion, where the map is already trusted) would
  confirm the instrument where the second member is a real frame; useful, no longer decisive.
- The cost runs (source-side GPU, three sets of three) only matter if the sampler is to
  replace the map in production, and with subgen off by default and dead under DLSS-G the
  only production consumer would be a Smooth Motion substitution that is also off. That is a
  decision, not a measurement.
- The instrument earns its place as an opt-in diagnostic regardless: the batch-to-batch
  counter is the log's first source-hitch detector that reads pixels, and it agrees with the
  recording.
