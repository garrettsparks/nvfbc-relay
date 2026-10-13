# WGC Capture Spec (Windows.Graphics.Capture as the ring's capture source)

Status: **built, behind `-wgc`, off by default.** On `performance-optimization` at `e79a362`. One
stream has run on it (2026-10-11, 2h34m). In steady play it behaves as NvFBC does. One finding
from that stream is open: while a menu is open, an extra frame arrives beside the pairs. Between
them it is a single and the lock releases; inside a pair's window it makes a batch of three, and
the ring can keep it in place of the real frame (section 5). A sitting on 2026-10-12 found what
it is (5.9): a frame Windows delivers for each move of the mouse pointer while WGC is capturing,
a copy of the frame before it, reported by Windows as changed only in a pointer-sized
rectangle. What the ring does about it is the user's decision (section 8). Last updated
2026-10-12.

This file is the reference for WGC capture: what is built, what has been measured, what is not
known and what to test next. How it got here stays where it was written. The costs, the sittings
and the 1 ms mistake are in `performance-candidates.md` section 9.1, and the session records are
`session-kickoff-performance-pass-5.md` to `-7.md`. Where this file and a newer measurement
disagree, the measurement wins and this file gets corrected.

## 1. What it is

A second way for the capture ring to get the game display's frames, beside NvFBC. Windows
delivers each frame into a pool of textures made on the D3D11 present's device, the ring's slots
name those textures, and the present samples them where they are. No frame is copied and nothing
is waited for. In the benchmark it costs the game about 47 points less than NvFBC (4.6% against
5.5%, sitting `iv1`, 2026-10-11). It needs nothing from NVIDIA, and it is the path the roadmap
wants the app to capture through by default.

The ring still owns time and decisions: the arrival stamp of every wake, which wakes make a
batch, which member of a batch is kept, and the slot a frame goes in (`ICaptureSource.h`). WGC
changes only where the pixels come from. The policy, the lock, the ETW flip pairing and the
present are the same code for both sources.

## 2. As built (`e79a362`)

**Switch.** `-wgc`, with `b:vsync` only. The help text does not list it.

**Start.** The relay starts on NvFBC as it always does. Once the D3D11 present has its device,
the capture thread opens a WGC session on the source display (`CreateForMonitor`) and the ring
moves to it. NvFBC is let go only once WGC is capturing. The log says each step:

    Capture through Windows.Graphics.Capture asked (-wgc): it takes over from NvFBC once the D3D11 present is up, and NvFBC stays if it cannot start
    WGC: least time between frames 16.000 ms as found, 0.001 ms after asking for 0.001 ms
    CaptureRing: capture moved from NvFBC to Windows.Graphics.Capture after 22 wakes
    WGC: capturing the 2560x1440 source display into a pool of 34 buffers on the D3D11 present's device; a ring slot names the pool texture its frame is in, and no frame is copied; cursor off, border off; wakes under 3000 us apart are one batch

**When it stays on NvFBC.** Each of these leaves the relay on NvFBC with the reason in the log:

- the Windows Runtime entry points or a WGC call fail;
- the source display and the output differ in size (WGC does not scale, and the present samples
  the pool without filtering);
- this Windows has no setting for the least time between frames (it came with Windows 11 24H2,
  build 26100), or Windows keeps that time above 0.001 ms.

**Pool.** `CreateFreeThreaded`, BGRA 8-bit, the source display's size, one buffer for every ring
slot and two more (34 on the rig). A frame stays out of the pool for as long as its slot names
it and goes back when the slot is used again.

**Waiting.** The pool's `FrameArrived` sets an event. The capture thread takes frames with
`TryGetNextFrame` and waits up to 100 ms for one. A frame's arrival stamp is the moment that
wait returns.

**Batches.** Wakes under 3 ms apart are one batch, the same window as NvFBC's. At x2 the ring
keeps the second member of a pair, the real frame (`TemporalPolicy.h`), and a batch of one goes
in as it is.

**Session settings.**

| setting | interface | the relay sets | state |
|---|---|---|---|
| least time between frames (`MinUpdateInterval`) | `Session5` | 0.001 ms, and refuses WGC without it | settled (section 3) |
| cursor in the picture (`IsCursorCaptureEnabled`) | `Session2` | off, to match NvFBC's grab | set; what it does to frame delivery is untested (section 6) |
| yellow border (`IsBorderRequired`) | `Session3` | off | set |
| a frame's changed areas (`DirtyRegions`) | `IDirect3D11CaptureFrame2` | nothing; it reads the list of every frame | used to leave out the mouse pointer's frames (below) |
| how changed areas are handled (`DirtyRegionMode`) | not declared | nothing; it is report-only as found | not needed |
| secondary windows | not declared | nothing | a window-capture setting; not used for a display |

The header declares `Session2`, `Session3` and `Session5` by hand. The dirty-region and
secondary-window interfaces and their IDs have to be taken from mingw-w64's
`windows.graphics.capture.h` or Microsoft's windows-rs bindings, as the others were.

**The mouse pointer's frames are left out** (in the user's working tree on 2026-10-12, after
`e79a362`; on by default, `-nowgcdrop` keeps them, `-wgcdrop` is accepted). While WGC is
capturing, Windows delivers a frame for each move of a visible mouse pointer: a copy of the
frame before it, reported as changed only in the pointer's rectangle (5.9). `Wait` gives such a
frame straight back to the pool and goes on waiting, so the ring is never told of it and none
of the ring's, the lock's or the policy's code is involved. The rule
(`WgcCaptureSource::PointerOnly`): the frame's changed areas sum to under a hundredth of the
picture, and a frame that changed the whole picture arrived in the last half second. A frame
with no list of changed areas, or an empty one, is kept, and so is a small change on a screen
where nothing has redrawn the whole picture for half a second. Both numbers are defaults the
user owns. The log says which way it is set, and the closing line counts them:

    WGC: a frame that changed only a small area, as a move of the mouse pointer makes, is left out while frames that change the whole picture are arriving (-nowgcdrop keeps them)
    WGC summary: N wakes stored, M frames came with no texture and were dropped, K frames that changed only a small area were left out

Checked on this code so far: the suite (652 lines, equal to the baseline, C++17 the same), a
zig build `pointerdrop-1`, and three launches on the rig's desktop with no game (it starts,
moves to WGC and logs the lines above; with `-nowgcdrop` it leaves out none; on NvFBC it
captures as before). On the desktop the 76 frames it left out all came in the first half second
after WGC's first whole-screen frame, and the two that came later were stored. Then the route
of 5.9 in the user's game on `pointerdrop-1` (`c_main_wgc.log`, `c_main_wgc_keep.log`, runner
log `mouse5.run.log`): by default every stretch reads 120 wakes in 60 batches a second with no
batch of one or of three, the inventory scrolling, the map zooming and the map dragged
included, and 1,432 frames were left out; with `-nowgcdrop` the same stretches read 178 to 180
wakes, with 59 batches of one a second in the inventory and 58 to 60 batches of three in the
map, and none were left out. Not yet: a CI build, a stream, a benchmark row.

**Lines that mean trouble.** `WGC: frames are now WxH, and the pool's buffers WxH` (the content
changed size), `WGC: a frame came with no texture and was dropped`, and `WGC: a frame came with
no list of changed areas, and such a frame is never left out`.

## 3. Settled

Each of these is measured. The logs and the sittings are named in `performance-candidates.md`
section 9.1 and in `session-kickoff-performance-pass-7.md`.

- **The least time between frames has to be far under 1 ms.** As found it is 16 ms, and a 185
  fps game arrived at about 53 frames a second. At 1 ms Windows delivered Smooth Motion's
  generated frame on the game display's flips and held the real frame for the card display's
  clock, so the relay showed generated frames for long stretches, and when the two met Windows
  dropped the real one. At 0.001 ms both arrive together. The user's decision, 2026-10-11: "it
  makes sense to do 0.001ms".
- **In steady play a Smooth Motion pair arrives as NvFBC's does.** Generated then real, after
  one flip. In the user's game at 60 x2 over 175 s: 10,514 of 10,514 batches were pairs, the gap
  inside a pair was 0.27 ms at the median and 1.46 ms at the most, and all 10,512 presents
  stepped one period with no repeat and no blend.
- **The first frame of a pair is the generated one.** Edge energy read back from every frame
  (about 800 pairs to 20 in each 15 s), and the user's blind eye test.
- **The pool queues frames.** A capture thread busy for 1.5 to 2.5 ms a frame lost none of
  92,470.
- **`SystemRelativeTime` is no use as a clock.** Which clock it follows changes with how the
  game's frames reach the compositor. Dropped; the ETW flips are the clock.
- **WGC does not scale.** A pool's buffers keep their size and bigger content is clipped.
- **Cost.** About 249 points against NvFBC's 297 in one launch. The 1 ms, 0.01 ms and 0.001 ms
  rows could not be told apart; up to about 10 points for 0.001 ms is not ruled out.
- **Under DLSS frame generation** WGC delivers one frame for each real frame.

**Retracted (2026-10-09 readings):** "under Smooth Motion WGC's pairs arrive 1 to 4 ms apart", "a
6 ms pair window pairs every wake". They were one point of a drift that the 1 ms setting made.

## 4. The first stream (2026-10-11)

Get Medieval (KCD at 60 fps with Smooth Motion x2), 2h34m, the CI build of `e79a362`, mode line
`-mark 5400 -wgc`, driver 610.88. The log is `Get Medieval 2026-10-11 0.log` in the analysis
root. Everything below is on the stretch the stream's video shows, relay time 51.4 to 9292.4 s,
lined up by the 2,340 marked frames of its opening (2,282 of them within 0.37 ms at the 90th
percentile). The capture figures are from `wgcsingles.py <log> 51.4 9292.4`, which sorts
each second into ordinary (the 60 fps regime, `regimegaps.py`'s rule), extra (out of the regime
with 70 or more batches) and other (hitches, stalls, loading). That gave 8,620, 449 and 172
seconds.

Against what `session-kickoff-performance-pass-7.md` Part B expected:

| read | expected | the stream |
|---|---|---|
| the move to WGC and the interval line | present, once | present, once: moved after 22 wakes, 16.000 ms as found, 0.001 ms after asking |
| wakes and batches a second | 120 and 60 | 120 and 60 at the median in ordinary seconds |
| batches that are pairs | 100% in steady play; hitches and menus unknown | 512,099 of 516,815 in ordinary seconds (99.09%). Of the other 4,716, the menus' extra frame makes 3,844 batches of three and about 550 singles (section 5.3), 279 singles are halves of a pair wider than the 3 ms window, which follow hitches, and 41 sit beside a gap. Two singles in the whole stream stood alone where a pair should be, so WGC did not lose members of pairs (NvFBC delivers one wake for a cycle far more often: 14,937 such singles on 2026-10-08) |
| gap inside a pair | median 0.27 ms, under 1.5 ms; anything near 3 ms is a finding | median 0.27 ms, p99 1.41 ms. 3,383 of 512,077 pairs were over 1.5 ms and 77 over 2.5 ms, the widest 2.98 ms. NvFBC on 2026-10-09 had more of that tail: median 0.36 ms, p99 1.55 ms, 1,401 of 109,922 over 1.5 ms |
| size or texture lines | none | none. `WGC summary: 1136533 wakes stored, 0 frames came with no texture and were dropped` |
| holds, repeats, blends, present gaps | as the 2026-10-08 stream | the table below |

| | 2026-10-08, NvFBC, `b043b35` | 2026-10-09, NvFBC, `b043b35` | 2026-10-11, WGC, `e79a362` |
|---|---|---|---|
| length | 2h16m | 32m | 2h34m |
| source gaps over 25 ms, an hour | 128 | 302 | 278 |
| holds an hour | 193 | 320 | 468 |
| content repeats an hour | 193 | 322 | 468 |
| blends, share of presents | 0.32% | 0.66% | 0.79% |
| presents blocked over 25 ms, an hour | 29 | 73 | 76 |
| shown frame steps one period, ordinary seconds | 99.73% | 99.43% | 99.41% |
| lock releases, and an hour | 7, 3.1 | 2, 3.7 | 303, 118 |

Against the six NvFBC streams from 2026-09-30 on, not only the two above: holds and repeats an
hour are inside their range (168 to 501) and near its top. Blends at 0.79% are the highest of
the seven, just over 2026-10-01's 0.77%, a stream with 411 source gaps an hour; about 400 of the
4,401 blends fall in the extra seconds of section 5. The lock releases are far outside
anything before. Presents blocked for each source gap were 0.27, against 0.22 to 0.32 on those
six, so nothing here shows WGC loading the GPU more. The present statistics read
0.024 refreshes a second with no new frame, the same as 2026-10-09, with no present reporting a
problem. In the stream's video, freezes ran at 0.51 a second of motion (0.36 to 0.93 on the
NvFBC streams) and at most 51 of 1,418 upstream freezes could be the relay's, an upper bound
with a window of 477 ms by the end. The marked opening had no motion, so it gave no exact split.

Two things the stream showed that have not been looked at: the dejitter summary counted 148
skipped batches (0 on 2026-10-09), and 38 lock releases started in ordinary seconds, 15 an hour
against 3 to 4. Some of those seconds are menus the regime rule cannot see (section 5.3).

**The stream did not join the replay corpus.** The fixture check that a cut holds one regime
found 621 seconds out of the gameplay rate in 173 runs, 74 of them longer than 2 s, from 8
minutes in to the end. The NvFBC streams of 2026-10-06, -08 and -09 had no run longer than 2 s.
The long runs are the menus of section 5.

## 5. Finding: an extra frame beside the pairs while a menu is open

Part B's unknown ("hitches and menus are the unknown") has an answer for menus.

### 5.1 What was on screen

The extra seconds group into 46 stretches of 3 s or longer, 502 s in all, the longest 32 s. One
frame of the video from the middle of each: all 46 are menus (the map, the inventory, the trade
and loot panels, the pause menu). A frame every two minutes across the whole video, 77 of them:
about 20 dialogue and cutscene frames, the dice game, lockpicking, riding and walking were all in
ordinary seconds, and of 11 menu frames 7 fell in an out-of-regime second. So it is menus, not
cutscenes, and not every second of a menu: the extras come and go while a menu stays open. With
the trade screen up from 53:04 to 53:20 of the video, the last four seconds were ordinary.

The extra seconds are not all of it. Another 197 seconds count as ordinary, with 60 batches,
and hold the same frame as a third member of a pair's batch (5.3). They make 46 runs, and a
frame of the video from each of the 18 longest is a menu too.

### 5.2 What the log shows

In an extra second the median is 149 wakes in 88 batches where an ordinary second has 120 in 60.
A third of the batches are singles (33.3%; 65.6% pairs). The pairs keep coming, 60 a second, and
the single lands between them (log lines in time order, fields trimmed):

    capture #1026448 arr=8460001309us dt=7040us      pair, first member
    capture #1026449 arr=8460001835us dt=525us       pair, second member
    flip disp=8460001340us evt=8460000921us head=0   the pair's two flip requests on the game display,
    flip disp=8460008824us evt=8460001469us head=0     announced together, scheduled 7.5 ms apart
    flip disp=8460010274us evt=8460010275us head=1   the relay's own present on the card
    flip disp=8460010518us evt=8460010518us head=0   one more request on the game display, at once
    capture #1026450 arr=8460010622us dt=8787us      the single, 0.1 ms after it
    capture #1026451 arr=8460017984us dt=7361us      the next pair
    capture #1026452 arr=8460018144us dt=160us

The flip lines are the NVIDIA driver's FlipRequest events (`FlipDecode.h`), so the extra request
is the driver's own record and not something WGC reports. On the game display:

| | 2026-10-08, NvFBC | 2026-10-09, NvFBC | 2026-10-11, WGC |
|---|---|---|---|
| seconds with 70 or more batches | 0 | 0 | 449 |
| seconds with 140 or more flip requests on the game display | 0 | 0 | 435 |
| batches of three or more in ordinary seconds | 4 | 1 | 3,844 |
| in those seconds, requests under 4.17 ms from the one before, a second | - | - | 36.2 |
| the same in seconds with 110 to 130 requests | 0.1 | 0.1 | 0.4 |

All three streams ran driver 610.88.

**The extra request follows the relay's own present** (measured 2026-10-12). The tool sorts the
game display's requests into members of a pair and requests that belong to no pair. The extra
seconds have 32.0 a second in no pair (14,363 in all) and the ordinary seconds 1.4. Of the
14,363, 10,920 come within 1 ms after the relay asked for its present on the card, 0.25 ms after
it at the median, and 10,731 of those are followed by a WGC wake inside 0.5 ms.

| | extra seconds | ordinary seconds | 2026-10-09, NvFBC |
|---|---|---|---|
| relay presents | 26,914 | 517,103 | 114,279 |
| followed within 1 ms by a request in no pair | 10,932 (40.6%) | 2,937 (0.57%) | 36 (0.03%) |

The 2026-10-09 column is that stream's whole log less a minute at each end. It was not cut to
its video.

They come in runs. Relay presents in a row that drew one: 3 at the median, 24 at the 90th
percentile, 110 at the most. In a row that drew none: 4, 26 and 600. The other 3,443 requests in
no pair sit 11 to 15 ms after the relay's request and keep no fixed time from the relay's flip
being shown or from either member of the pair; 1,922 of them are followed by a wake within 1 ms.

The time is taken from the moment the relay asked for its present. The relay's flip is often
asked for about 7.6 ms before it is shown. Timed from when it was shown, the tool's older line
reads lower: in the 435 seconds an at-once request (announced and scheduled within 50 us)
follows a relay present within 0.6 ms 21.7 times a second, and 1.6 to 1.8 on the NvFBC streams.

### 5.3 Where the extra lands: a single, or a third member

The same extra frame shows up two ways, by where it falls against the pair at that moment.

- **Between two pairs** it is a batch of one. That is the extra seconds: 13,374 singles, a third
  of their batches.
- **Within 3 ms of a pair** it joins the pair's batch and makes three. The second still has 60
  batches, so the regime rule calls it ordinary. 3,844 batches of three sit in ordinary seconds,
  in 309 of the 8,620, up to 55 in one; 3,633 of them are in the 197 seconds with five or more.
  Another 433 are in the extra seconds. Three flip requests are announced with a batch of three
  in 3,793 of the 3,844, where a pair has two. The NvFBC streams had 4 and 1 batches of three
  in their ordinary seconds.

Inside a run the extra mostly keeps to one side of the pair: first in 506 of the 515 in one
22 s run, last in all 325 of a 20 s run. Over the ordinary seconds it is first in 1,991 and last
in 1,853.

**The ring keeps the last member of a batch.** At x2 there is no rotation guidance, so
`DecideKeep` keeps each new member and takes back the one before it, and the log's `col=`
counter steps on both the second and the third wake of a three. With the extra first the ring
keeps the real frame, as it should. With the extra last it keeps the extra and takes the real
frame back: 1,853 batches in ordinary seconds and 196 in extra seconds.

### 5.4 What the extras are, and are not

- **They are flip requests, not refreshes.** About 36 a second sit under 4.17 ms from the
  neighbouring request, which is less than a 240 Hz panel's refresh time, so they cannot each be
  a refresh of their own. The user's G-SYNC monitor shows its own frame-rate graph, and it does
  not go far above 120 in a menu or the map (the user, 2026-10-12: "it's as close to a perfect
  representation of what the monitor is outputting as we can get"). W1 reads it with `-wgc`
  running.
- **They are not game frames.** The game's 60 pairs a second do not change.
- **WGC hands the relay a frame for each one.** That is the single, or the third member.
- **NvFBC streams had next to none:** no second with 70 batches or with 140 flip requests, and 4
  and 1 batches of three. No run has put the two sources through the same menu, so that is two
  streams against one and not an A/B (test W2).

### 5.5 What the relay did with them

| | ordinary seconds | extra seconds |
|---|---|---|
| presents | 517,201 | 26,890 |
| unlocked (`lk=0`) | 0.17% | 18.24% |
| shown frame steps one period | 99.41% | 98.65% |
| the same frame again | 0.35% | 0.34% |
| a step under 12 ms, or of 21 ms or more | 0.25% | 1.02% |
| pass-before / pass-after / blend | 43.2 / 56.1 / 0.6% | 61.6 / 36.8 / 1.5% |
| the shown frame came from a pair | 99.20% | 95.14% |
| from a single | 0.03% | 3.06% |
| from a batch of three or more | 0.75% | 1.59% |

So the relay stays on the pairs for 95% of the presents in an extra second, and it showed a
single on about 820 presents in the stream. (On NvFBC a single is ordinary, 3.7% of its batches
on 2026-10-09, and the code's note says that lone frame is the real one.) A frame from a batch
of three is the extra itself whenever the extra came last (5.3). The ordinary column includes
the 197 menu seconds that 5.3 describes.

232 of the 303 lock releases start in extra seconds, 33 in other seconds and 38 in ordinary
ones. A release lasts 17 presents at the median, 39 at the 90th percentile and 161 (2.7 s) at
the most. The relay's own telemetry reads the extras as a faster source: four of its five
`source measuring ~120 fps; -src N would lower lag` lines inside the stream came during a menu
stretch.

### 5.6 After a menu closes

The 39 stretches with 10 s or more before the next one, and the presents of each kind in each
of the first seconds after a stretch ends, averaged over them:

| second after | 1 | 2 | 3 | ordinary seconds away from menus |
|---|---|---|---|---|
| unlocked | 5.7 | 0.5 | 0.0 | 0.08 |
| steps off one period | 1.7 | 0.6 | 0.1 | 0.14 |
| the same frame again | 3.8 | 1.8 | 0.1 | 0.16 |

The lock is back within two seconds. The repeats in the first two seconds are not separated from
the game's own stall when a menu closes, which NvFBC streams have too.

### 5.7 Not known

- What makes the driver put the extra request on the game display, and whether it needs WGC to
  be capturing. Three quarters of them follow the relay's own present (5.2). Why a present on
  the card would bring a request on the game display, and only while a menu is open, is not
  known. Whether another program presenting on the card would do the same is not known either.
- What picture the extra frame carries: a copy of the generated frame, of the real one, or
  something else. It decides what the ring held in the 2,049 batches that kept it, and what the
  820 presents that showed a single put on the card (test W4).
- Whether showing one costs a frame in the stream's video. Menus were also where upstream
  freezes bunched on NvFBC (the 2026-10-01 marked recording: the inventory screen and the dice
  game), so the video of this stream cannot say.
- Whether other games' menus do it. Only this game's have been seen.

### 5.8 A guess, tested on 2026-10-12 and right (5.9)

The trigger is the mouse pointer moving. The menus are where the game shows one, and the extras
come and go inside one open menu, in runs a few presents long (5.2), as a pointer that moves and
stops would. Not explained by it: three quarters of the extra requests follow the relay's own
present by a quarter of a millisecond.

### 5.9 The mouse sitting (2026-10-12): the pointer is the trigger

One sitting in the user's game at 60 fps with Smooth Motion x2, a menu open, nobody touching
the mouse. Each arm is a relay launch: 10 s with the pointer still, 10 s with the bench runner
moving it (`sweep`: about 63 relative moves of 6 pixels a second, left and right), twice. A
stretch is read less 1 s at its start and 0.5 s at its end. The logs are
`rigbench/checks-1012/out/m_<arm>.log` in the analysis folder, the runner's logs
`mouse1.run.log` and `mouse2.run.log` beside them, and the reader is
`rigbench/checks-1012/stretches.py`. Every arm is `-source 0 -target 1 -framerate b:vsync -src
60` and the flags shown. The inventory unless it says the map.

| arm | build | flags | still: wakes, requests in no pair, a second | moving: wakes, requests in no pair, a second | moving: relay presents that drew one |
|---|---|---|---|---|---|
| `wgc_ci` | CI `e79a362` | `-wgc` | 119.8, 0.1 | 179.1, 59.1 and 179.4, 59.5 | 504 of 512, 505 of 510 |
| `wgc` | zig `wgccursor-1` | `-wgc -xwgcinterval 1` | 120.0, 0.0 | 179.8, 59.8 and 179.1, 59.0 | 509 of 511, 501 of 510 |
| `wgc_cursor` | zig `wgccursor-1` | `-wgc -xwgcinterval 1 -xwgccursor` | 120.0, 0.0 | 178.0, 57.9 and 179.4, 59.5 | 493 of 510, 506 of 511 |
| `wgc_dirty` | zig `wgcdirty-1` | `-wgc -xwgcinterval 1 -xwgcdirty` | 120.1, 0.0 | 179.0, 59.1 and 179.5, 59.4 | 503 of 510, 504 of 511 |
| `wgc_dirty`, the map | zig `wgcdirty-1` | `-wgc -xwgcinterval 1 -xwgcdirty` | 120.0, 0.0 | 179.2, 59.2 | 503 of 511 |
| `measure` | zig `wgcpatch-2` | `-xwgc 32 -xwgcmeasure -xwgcinterval 1 -xwgcpatch 1640,300` | 120.1, 0.0 | 179.8, 59.9 and 179.5, 59.5 | 511 of 512, 505 of 510 |
| `wgc_1ms` | zig `wgccursor-1` | `-wgc -xwgcinterval 1000` | 120.0, 0.2 | 120.0, 59.0 and 120.0, 59.5 | 502 of 510, 507 of 511 |
| `nvfbc` | zig `wgccursor-1` | none | 117.8, 0.0 | 115.0, 0.0 and 114.1, 0.0 | 0 of 510, 0 of 511 |

What it settles:

- **W1: the moving pointer is the trigger.** With the pointer still there is no request outside
  the pairs and no extra wake. While it moves, each relay present on the card is followed by one
  extra request on the game display and one extra WGC frame, about 60 a second, in the inventory
  and in the map. The stream's build and the experiment build do the same.
- **W2: the extra requests need WGC.** On NvFBC, with its hardware cursor setting off as
  shipped, the game display has 120.0 requests a second with the pointer still or moving, and no
  relay present drew one. NvFBC's wakes fall a little while the pointer moves (115 against 118,
  with more cycles arriving as one wake); that has not been looked at.
- **W3: cursor capture on does not remove them.** The count is the same with
  `put_IsCursorCaptureEnabled(1)`. What it changes is the picture: the game's menu pointer is the
  system pointer with the game's own shape, and with cursor capture on it is in the picture on
  the card (`m_wgc_cursor.jpg` against `m_wgc.jpg`). With it off the stream has no pointer in
  menus.
- **W4: the extra frame is a copy of the frame before it.** The readback, pointed at the
  character's model (every one of 1,022 still frames a different picture): 508 of 508 and 500 of
  500 extra frames carry the same picture as the frame delivered just before them. Cursor
  capture was off.
- **W5: Windows marks them.** Every game frame is reported as one rectangle, the whole 2560x1440
  (1,022 of 1,022 with the pointer still; 1,020 of 1,022 and 1,022 of 1,026 with it moving).
  Every extra frame is reported as one small rectangle at the pointer (501 of 501, 502 of 502
  and, in the map, 499 of 499; median area 1,184 pixels, the first one seen 32x32). Nothing lies
  between the two sizes. The session's dirty-region mode was left as found (0, report only). The
  reading cost 1 us at the median and 37 us at the most over 7,386 frames; that is the call
  alone, and no benchmark row has been run (W7).
- **1 ms does not help.** With `-xwgcinterval 1000` the extra requests still come, 59 a second.
  WGC then delivers 120.0 frames a second in both stretches and nearly every batch is a single,
  which is the broken pairing section 3 describes. Which frames it leaves out was not read.

**The label and the picture on the same frames, with the screen moving** (the user: "what if I'm
like moving the mouse and scrolling at the same time?", "are we really sure they're alwasy
duplicates"). One launch of zig build `wgcboth-1` (experiment branch `06a7bca`), `-xwgc 32
-xwgcmeasure -xwgcdirty -xwgcinterval 1`, log `m_both.log`, runner log `mouse3.run.log`. Every
one of its 9,469 frames has both its rectangles and a hash of the screen's centre 512x512. The
runner turned the wheel and held the left button while it moved the pointer (`sweepwheel`,
`drag`).

| what was on screen | marked pointer-sized, and the same picture as the frame before | marked whole screen, and a new picture | any other combination |
|---|---|---|---|
| inventory, the list scrolling while the pointer moves | 388 | 784 | 0 |
| map, zooming while the pointer moves | 390 | 781 | 0 |
| map, dragged with the left button | 391 | 781 | 0 |
| gameplay, the camera turning with the mouse | 0 | 782 | 0 |
| pointer still (four stretches) | 0 | 2,161 | 0 |

So a game frame that carries scrolled or dragged content is still reported as the whole screen,
and the pointer's extra is still a copy. In gameplay the mouse turning the camera brought no
extra frame and no request outside the pairs (120.0 wakes a second, 0 of 391 presents drew one).

**NvFBC with its hardware cursor on** (`-xnvcursor`, experiment branch `bf03b51`, zig build
`nvcursor-1`, log `m_nv_cursor.log`, the inventory): 118.9 wakes a second with the pointer
still and 170.5 with it moving, in 97.9 batches (43.0 of one, 17.7 of three or more), while the
game display keeps 119.9 requests a second with none outside a pair. The pointer is in the
picture on the card. So T7's note holds in this game's menus, and NvFBC gives nothing to tell
those wakes from frames.

**The drop, tried the same night** (`-xwgcdrop`, experiment branch `2d0382c`, zig build
`wgcdrop-1`; the user's branch is untouched). Inside the WGC wait, a frame goes straight back to
the pool when Windows reports its changed area as under a hundredth of the picture and a
whole-picture frame has arrived in the last half second. The ring never sees it: no arrival
stamp, no batch, no keep decision. A frame with no list of rectangles, or an empty one, passes.
One route in one game launch, run three times on that one build (`f_wgc_drop.log`,
`f_wgc_nodrop.log`, `f_wgc_dropcur.log`, runner log `mouse4.run.log`): the inventory with the
list scrolling, the map zooming, the map dragged, then gameplay with the camera turning, the
pointer moving throughout, 10 s each.

| stretch | drop on: wakes, batches of one, of three, a second | drop off | drop on, cursor capture on |
|---|---|---|---|
| inventory, list scrolling | 120.0, 0.0, 0.0 | 180.0, 60.1, 0.0 | 119.9, 0.0, 0.0 |
| map, zooming | 119.8, 0.0, 0.0 | 179.8, 0.0, 59.6 | 120.0, 0.1, 0.0 |
| map, dragged | 120.0, 0.0, 0.0 | 179.5, 0.0, 59.5 | 120.1, 0.0, 0.0 |
| gameplay, camera turning | 120.1, 0.0, 0.0 | 120.1, 0.0, 0.0 | 120.1, 0.0, 0.0 |
| frames let go in the launch | 1,798 | none | 1,805 |

Over the route (relay seconds 12 to 75, `wgcsingles.py`): with the drop on no second is
out of the 60 fps regime and 99.97% of presents show a frame from a pair; with it off 10 seconds
are out of the regime and 39% of the presents in the others show a frame from a batch of three.
With cursor capture on the pointer is in the picture on the card and the drop works the same
(`f_wgc_dropcur.jpg`). The frames the ring did see were all reported as the whole screen. The
game display still gets its extra requests, 180 a second; the relay no longer takes a frame for
them. Repeats were the same in the three arms, 38 to 40, at the menus opening and closing. The
lock did not release in any arm, the one without the drop included, so this run does not show
the stream's lock releases going away; a stream or a long sitting in menus would.

Not yet run: a benchmark row for the drop (W7); a stream on it; a hand on a real mouse, which
reports far more often than the runner's 63 moves a second (whether it stays at one extra for
each present is the question); which frame the extra copies when it falls between a pair's two
members; what label a notification or an overlay drawn over the game gets; any other game.

## 6. The cursor under each source

- **NvFBC.** The ring's grab session sets `bWithHWCursor = 0` (`CaptureRing.cpp`). That is T7 of
  `dual-device-capture-present-spec.md`: with it on, the blocking grab woke on every cursor
  move, at the mouse's polling rate, with duplicate content. It is a setting of the grab. It
  does not turn the hardware cursor off on the display. T7's own note says `= 1` was never
  tested in a game; the user, 2026-10-12: "I'm not convinced it does anymore with a game in
  fullscreen mode".
- **WGC.** `put_IsCursorCaptureEnabled(0)`, logged as "cursor off". It leaves the pointer out of
  the captured picture. Whether a moving pointer still produces a frame, or a flip request, with
  it off has not been tested. T7's mouse test was never run on WGC.
- **In the stream's picture.** One inventory frame of the 2026-10-11 video (2:24:32) shows no
  pointer. The game's menu pointer is the system's pointer with the game's own shape (5.9: it
  appears on the card with cursor capture on and not with it off), so it has been missing from
  every stream on either source.

## 7. Tests still to run

In this order. Each is a rig job in the user's game with the user playing, a minute or two, and
each is read with `wgcsingles.py` on its log unless it says otherwise. They are numbered W
so as not to be taken for the T rules of `dual-device-capture-present-spec.md`.

| | test | read | settles |
|---|---|---|---|
| W1 | `-wgc`. Open the map: 10 s hands off the mouse, 10 s moving it, twice. The same in the inventory. Note the times | batches, singles, batches of three and game-display flip requests a second, still against moving; the monitor's graph during it | whether mouse movement is the trigger. Extras only while moving: it is. Extras in both: the menu itself. None at all: not reproduced, and what else differed on the stream is the next question |
| W2 | W1 without `-wgc`, the same build | the same | whether the extra requests need WGC. If NvFBC shows the requests but no extra wakes, the difference is only what each source delivers |
| W3 | W1 with cursor capture on (`put_IsCursorCaptureEnabled(1)`): `-xwgccursor` on the experiment branch (`a6996b8`, zig build `wgccursor-1`). The user, 2026-10-12, asked for it in the same sitting as W1 | the same, and a screenshot of the card display for the pointer | whether leaving cursor capture on removes the extras, at the price of the system pointer in the picture |
| W4 | The readback that is already on the experiment branch (`-xwgcmeasure`), in a menu with the mouse moving | the centre of every frame and its edge energy: the extra against the two members of the pair before it. A hash of the centre would make a copy exact | what picture the extra carries, and so what the ring shows when it keeps one |
| W5 | Dirty regions, report-only: `-xwgcdirty` on the experiment branch (`ec5a096`, zig build `wgcdirty-1`) logs each frame's region count, summed area, first rectangle and the time the reading took, after the arrival stamp. It reads `IDirect3D11CaptureFrame2` (ID from mingw-w64's header) and sets nothing on the session. Run W1 with it. The user asked about it on 2026-10-12 | the region on the extra against the region on pair members | whether the extra is marked as nothing changed (droppable on arrival) or as a full frame (dirty regions cannot separate it). Also shows what a fullscreen game's own frames are reported as |
| W6 | A local recording with `-mark` on for the whole run and `-wgc`, a minute in a menu and a minute walking, three times | streamcheck's count of each freeze by marker counter in the menu minutes | whether the extras cost shown frames, and whose the menu freezes are |
| W7 | A benchmark row for whatever W3 or W5 would turn on by default | the sitting, beside a `-wgc` row | its cost. The user on reading dirty regions every frame: "sounds costly from a performance perspective". It should be one property read a frame, and it has not been measured |

**W1 to W5 ran on 2026-10-12; the results are in 5.9.** W6 and W7 have not run. As it was
prepared:

W1 to W5 are prepared as one job, `rigbench/checks-1012/mouse1.json` in the analysis folder: a
menu open (the inventory, if its centre moves, so that W4's hash can tell a copy), and for each
arm 10 s with the pointer still and 10 s with the runner moving it (`sweep`, relative mouse
input on a timer), twice. The arms are the stream's build with `-wgc`; the experiment build
with `-wgc -xwgcinterval 1`; the same with `-xwgccursor`; with `-xwgcdirty`; with `-xwgcinterval
1000` in place of 1; the readback (`-xwgc 32 -xwgcmeasure -xwgcinterval 1`); and the experiment
build on NvFBC. A last stretch has the user move the mouse by hand, in case input made by the
runner does not behave as a hand does. Not yet run.

Not a test, but owed from Part B: `tools-1010/kfshown.py` on the singles around hitches, if any
are to be classed.

## 8. Decisions that wait on the tests

Each is the user's.

- **What the ring does with the extra frame.** After 5.9 the candidates are: leave it (the lock
  is back within two seconds, the steps hold at 98.65% and 95% of the presents in an extra
  second still show a pair's frame); drop it on arrival by its dirty region, since a game frame
  is reported as the whole screen and an extra as a pointer-sized rectangle, with nothing
  between in the sitting, and the extra's picture is a copy of the frame before it; or drop it
  by what the ring already has (a batch of one arriving mid-cycle between pairs, a third member
  of a batch), with no new call a frame. Cursor capture on is out: W3 showed the same count.
  Tuning the least time between frames is out: at 1 ms the requests still come. A drop by dirty
  region is a new call for every frame and a changed default, so it gets a benchmark row (W7)
  and its own off switch. Not known: what the dirty region of a frame is when the game does not
  fill the screen, in other games, or with cursor capture on. The user, 2026-10-12: "I guess we
  can build the drop rule, but I don't want this to cause issues with the other logic just for
  this kind of pathological issue in menus and such." So the drop belongs inside the WGC
  source, before a frame becomes a wake: the ring, the lock and the policy never see the frame
  and none of their code changes. It passes a frame whenever Windows gives no list, whenever
  the changed area is anything but tiny, and whenever no whole-screen frame has come lately (a
  still screen where the pointer is the only thing moving).
- **Whether the pointer goes back in the stream's picture.** The user, 2026-10-12: "it's not a
  huge deal, although I would like to turn mouse back on at some point". On WGC that is cursor
  capture on, and 5.9 shows no cost in extra frames beyond what a moving pointer brings anyway.
  On NvFBC it is the hardware cursor setting of section 6, untested in a game.
- **Which member the ring keeps from a batch of three.** Today it is the last, which is the
  extra about half the time. Neither "the last" nor "the second" is right for both orders, so
  the ring would have to tell the extra from the pair. W4 says how much it matters.
- **How a WGC stream joins the replay corpus.** As the regime check stands, every WGC stream
  with menu time fails it. Either the check learns what a menu looks like under WGC, or the
  singles stop reaching the ring.
- **Whether `-wgc` becomes the default.** Not before the first decision.

## 9. Untested elsewhere

Carried from `performance-candidates.md` section 9.1 and `session-kickoff-performance-pass-7.md`.
"A whole stream" is done (section 4).

- Capturing the game's window in place of the display (`CreateForWindow`), cost included.
  Unknown going in: whether window capture delivers Smooth Motion's generated frames at all.
- Smooth Motion x3 and other source rates. `-fgphase` and `-phasekeep` on WGC.
- HDR sources. The pool is 8-bit.
- What WGC drops when a stall outlasts the pool's two spare buffers. The stream's log has not
  been read for it.
- The pairs under DLSS frame generation at 0.001 ms, 1.1 to 1.4% of wakes: what they are.
- Displays of different sizes: WGC refuses, and a scaler would be needed.
- Any GPU but the rig's. On a Windows before build 26100 WGC stays off, since the setting it
  needs is not there.
- The up-to-10-point cost of 0.001 ms against 1 ms.

## 10. Where things are

- **Code.** `src/relay/WgcCaptureSource.h` (the session, the pool, the settings),
  `src/relay/ICaptureSource.h` (what the ring asks of a source), `src/relay/CaptureRing.cpp` (the
  move from NvFBC, and NvFBC's cursor setting), `src/relay/FlipDecode.h` (what a flip line is).
- **Logs.** The stream's is `Get Medieval 2026-10-11 0.log` in the analysis root, with the
  NvFBC streams it is compared to beside it. The short runs of section 3 are named in
  `session-kickoff-performance-pass-7.md`.
- **Tools.** `wgcsingles.py <log> <from_s> <to_s>`, in the analysis folder, prints the capture,
  flip, lock and step counts of sections 4 and 5: batch sizes and gaps inside a pair, singles
  and batches of three and where the odd member sits, flip requests a second on the game
  display, the requests that belong to no pair and how many follow the relay's own present, the
  lock, the step of the shown frame and the batch it came from, and the seconds after each
  stretch of extras. The rows an hour in section 4 are from each stream's streamcheck
  report, and what was on screen (5.1) was read by eye from the video. In the analysis root,
  `regimegaps.py` lists the seconds out of the gameplay rate, and `tools-1010/kfstats.py` and
  `kfshown.py` are the short-run tools of Part B.
