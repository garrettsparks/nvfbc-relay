# WGC Capture Spec (Windows.Graphics.Capture as the ring's capture source)

Status: **built, behind `-wgc`, off by default** (the user's decision of 2026-10-14, section 8).
Six streams have run on it, from 2026-10-11 to 2026-10-14, and in steady play it behaves as
NvFBC does. While a menu was open on the first one, an extra frame arrived beside the pairs
(section 5): a frame Windows delivers for each move of the mouse pointer while WGC is capturing,
a copy of the frame before it, reported as changed only in a pointer-sized rectangle (5.9).
Since `407caaf` those frames are left out before the ring by default, since `c448312` the
pointer is in the picture by default, and since `700bb9d` the line that picks those frames sits
at a twentieth of the picture (section 2). The streams on them held their menus at 60 pairs a
second (5.10, 5.12). The sittings of 2026-10-14 (5.13) found that a moving pointer costs the game
about 145 points while WGC captures, which is Windows' compositor drawing the pointer and was
accepted, and that about 58 of WGC's points were its frame pool sharing the present's device.
With the pool on a device of its own and its textures opened by the present, WGC cost about 192
points where it cost 251 in the same launch, under OBS's game capture as measured in other
launches (202 to 216). That is the default inside `-wgc` since 2026-10-15, with `-nowgcopen` to
keep the pool on the present's device and a fall-back to it where a device of its own does not
work (section 2). In one launch beside OBS's game capture it cost the game 17.1 +- 2.3 points
less than game capture (5.15). Two streams with the pool on its own device found no torn or stale picture
(5.14). Since `4d7ba03` the drop is sized from the game's own frames, so that it also works for
a game in a window (sections 2 and 8); it has not yet run in a real game in a window. Since
2026-10-18 a game display of another size
than the capture card's is captured too and scaled by the present (section 2). Last updated
2026-10-18.

This file is the reference for WGC capture: what is built, what has been measured, what is not
known and what to test next. How it got here stays where it was written. The costs, the sittings
and the 1 ms mistake are in `performance-candidates.md` section 9.1, and the session records are
`session-kickoff-performance-pass-5.md` to `-7.md`. Where this file and a newer measurement
disagree, the measurement wins and this file gets corrected.

## 1. What it is

A second way for the capture ring to get the game display's frames, beside NvFBC. Windows
delivers each frame into a pool of textures, the ring's slots name those textures, and the
present samples them where they are: the pool is on a device of its own and the present opens
each texture through a shared handle, or with `-nowgcopen` the pool is on the present's own
device. No frame is copied and nothing is waited for. In the benchmark it cost the game about 47
points less than NvFBC with the pool on the present's device (4.6% against 5.5%, sitting `iv1`,
2026-10-11), and about 192 points, 3.6%, with the pool on its own device (5.13), where NvFBC
cost 287 to 297 in other launches. It needs nothing from NVIDIA, and it is the path the roadmap
wants the app to capture through by default.

The ring still owns time and decisions: the arrival stamp of every wake, which wakes make a
batch, which member of a batch is kept, and the slot a frame goes in (`ICaptureSource.h`). WGC
changes only where the pixels come from. The policy, the lock, the ETW flip pairing and the
present are the same code for both sources.

## 2. As built

**Switch.** `-wgc`, with `b:vsync` only. The help text does not list it.

**Start.** The relay starts on NvFBC as it always does. Once the D3D11 present has its device,
the capture thread opens a WGC session on the source display (`CreateForMonitor`) and the ring
moves to it. NvFBC is let go only once WGC is capturing. The log says each step:

    Capture through Windows.Graphics.Capture asked (-wgc): it takes over from NvFBC once the D3D11 present is up, and NvFBC stays if it cannot start
    WGC: least time between frames 16.000 ms as found, 0.001 ms after asking for 0.001 ms
    CaptureRing: capture moved from NvFBC to Windows.Graphics.Capture after 22 wakes
    WGC: capturing the 2560x1440 source display into a pool of 34 buffers on a device of its own, each texture opened on the D3D11 present's device through a shared handle (-nowgcopen keeps it on the present's device); a ring slot names the pool texture its frame is in, and no frame is copied; cursor on (-nowgccursor leaves it out), border off; wakes under 3000 us apart are one batch

**When it stays on NvFBC.** Each of these leaves the relay on NvFBC with the reason in the log:

- the Windows Runtime entry points or a WGC call fail;
- this Windows has no setting for the least time between frames (it came with Windows 11 24H2,
  build 26100), or Windows keeps that time above 0.001 ms.

**Pool.** `CreateFreeThreaded`, BGRA 8-bit, the source display's size, one buffer for every ring
slot and two more (34 on the rig). A frame stays out of the pool for as long as its slot names
it and goes back when the slot is used again.

**A source display of another size than the output** (2026-10-18). WGC captures it at its own
size and the present scales each frame as it draws it: bilinear by default, as NvFBC's grab
does, or bicubic or Lanczos with `-wgcscale` (not in the help text). A present whose frames are
the output's size draws them as before. Before this a display of another size left the relay on
NvFBC. Design, pictures and costs: `wgc-scaling-spec.md`. The log says the filter at start and
counts both kinds of present at the end:

    D3D11Present: a WGC frame of another size than the output is scaled with bilinear (-wgcscale)
    WGC: the source display is 2560x1440 and the output 1920x1080; the present scales its frames
    WGC scaling summary: N presents scaled with bilinear, M drew frames at the output's size unscaled

**Where the pool lives** (on by default since 2026-10-15; `-nowgcopen` keeps the pool on the
present's device, `-wgcopen` is accepted). The pool is made on a D3D11 device of its own,
created on the adapter the present's device is on, and the present's device opens each pool
texture once through an NT shared handle (`CreateSharedHandle`, `OpenSharedResource1`) and
samples it there. Opening is the only use of the present's device; its context is never
touched from the capture thread. Windows makes the pool's textures shareable (bind flags 0x28,
misc flags 0x802, SHARED and SHARED_NTHANDLE, read on the rig on 2026-10-14) but does not
promise it. So before NvFBC is let go, the relay takes WGC's first frame (it comes about 10 ms
after capture starts on the rig's desktop) and opens its texture on the present's device. A
pool makes every texture alike, so the first answers for all of them. If the device of its own
cannot be made, the session does not start on it, the first frame's texture does not open, or
no frame comes within half a second, the log says which and the session starts again with the
pool on the present's device, the way it ran up to `ab1772e`:

    WGC: with the frame pool on a device of its own, <why>; the pool goes on the D3D11 present's device instead

Nothing is ever named in a slot from a pool the present cannot read. A retry would not help,
since both causes known (Windows making the textures without the sharing flags, two devices on
different graphics cards) are fixed facts of the machine, and the second is ruled out by making
the device on the present's adapter. A later texture that cannot be opened is not expected; it
would leave its slots naming nothing (the present would show the ring's own surface for them)
and is logged once and counted in the closing line:

    WGC summary: 34 pool textures opened on the present's device, 0 could not be opened

Nothing orders Windows' write into a pool texture with the present's read on the other device
(the textures carry no keyed mutex). The present reads a frame about 96 ms after it arrives, and
two streams found no torn or stale picture (5.14). Why the pool's device costs the game 58
points is not known; a device of its own goes around it (5.13). Checked on this code
(`00987d6`): the suite (C++20 and C++17 alike), controls that fail it without the `-nowgcopen`
parse line or with the old default, zig builds, CI, a desktop run of each setting on the rig and
the sitting of 5.15, where every row opened all 34 textures on the present's adapter. The first
version, `-wgcopen` off by default at `ab1772e` with the pool's device on the first adapter and no
fall-back, streamed twice (5.14). The fall-back has not run on the rig and has never been needed.

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
| cursor in the picture (`IsCursorCaptureEnabled`) | `Session2` | on; off with `-nowgccursor`, to match NvFBC's grab | set; a moving pointer brings the same extra frames either way (5.9), and on costs nothing with no pointer on screen (5.11) |
| yellow border (`IsBorderRequired`) | `Session3` | off | set |
| a frame's changed areas (`DirtyRegions`) | `IDirect3D11CaptureFrame2` | nothing; it reads the list of every frame | used to leave out the mouse pointer's frames (below) |
| how changed areas are handled (`DirtyRegionMode`) | not declared | nothing; it is report-only as found | not needed |
| secondary windows | not declared | nothing | a window-capture setting; not used for a display |

The header declares `Session2`, `Session3` and `Session5` by hand. The dirty-region and
secondary-window interfaces and their IDs have to be taken from mingw-w64's
`windows.graphics.capture.h` or Microsoft's windows-rs bindings, as the others were.

**The mouse pointer's frames are left out** (`407caaf`, 2026-10-12; on by default, `-nowgcdrop`
keeps them, `-wgcdrop` is accepted). While WGC is
capturing, Windows delivers a frame for each move of a visible mouse pointer: a copy of the
frame before it, reported as changed only in the pointer's rectangle (5.9). `Wait` gives such a
frame straight back to the pool and goes on waiting, so the ring is never told of it and none
of the ring's, the lock's or the policy's code is involved. The rule, since `4d7ba03`
(`policy::IsPointerFrame`, called by `WgcCaptureSource::PointerOnly`), is sized from the game's
own frames, so it holds for a game in a window as for one that fills the display: the relay
remembers the largest changed area any frame was reported with, forgets it once half a second
passes with no frame at least that large, and leaves out a frame whose changed areas sum to
under a tenth of the remembered one. A frame with no list of changed areas, or an empty one, is
kept and leaves the memory as it is; on a still screen where only the pointer moves, the memory
runs out and the pointer's frames are kept, since they are all the screen has. Up to `4d7ba03`
the line was a share of the whole picture (a hundredth up to `c448312`, then a twentieth) while
a whole-picture frame had arrived in the last half second. Both numbers are defaults the user
owns. Section 8 has how the rule was chosen and checked. The log says which way it is set, and
the closing line counts them:

    WGC: a frame that changed under a tenth of the largest area a frame changed in the last half second, as a move of the mouse pointer makes, is left out (-nowgcdrop keeps them)
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
map, and none were left out. CI 38351906834 built `407caaf` with 0 warnings, the same imports
as `e79a362` and the suite's 652 lines unchanged, and the user streamed on that build the same
night (5.10). Its benchmark rows are in 5.11: leaving the frames out costs the game nothing
measurable.

Windows gives no flag that says a frame came from the pointer. The rule reads the size of the
changed area, and the line is drawn between what was measured: the largest pointer frame in any
log is 2,048 pixels (two 32x32 rectangles, when the pointer jumped far), 0.056% of 2560x1440,
and a game frame is the whole of it. The user, 2026-10-12: "different games have different
pointers", "we need to consider scaling it by resolution or something. but for now we leave
it". A pointer keeps its pixel size when the resolution drops, so its share of the picture
grows on a smaller screen.

The line was a hundredth of the picture up to `c448312` and is a twentieth since. The user,
2026-10-13, after agreeing the rule of section 8: "can we make it like 2% or 5% to give us a
little more wiggle room for now without requiring a ton more testing?", then "go ahead and do
the 5% change". The twentieth covers a pointer about 429 pixels square at 2560x1440, 321 at
1920x1080 and 214 at 1280x720, and about 303, 228 and 152 when the pointer jumps and Windows
reports two rectangles. So it leaves out every pointer size Windows offers at 2560x1440 (the
largest, 256 pixels, is 3.6% of the picture when it jumps), all but the largest jumping at
1920x1080 (6.3%), and up to a 128-pixel pointer jumping at 1280x720 (3.6%). On everything read
so far the hundredth and the twentieth give the same result, since nothing between 0.06% of the
picture and the whole of it has been seen; the benchmark's frames were all the whole picture,
and the work a frame does is the same, so no benchmark row was run for it. Checked: a zig build
(`drop5pct1`). That rule was replaced in `4d7ba03` by the one sized from the game's own frames
(above, and section 8), which decides as the twentieth did on every full-screen frame read.

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
- **WGC does not scale.** A pool's buffers keep their size and bigger content is clipped. The
  present scales instead (section 2).
- **Cost.** About 249 points where NvFBC costs 297, in one launch on 2026-10-11, and 244 and 290
  over two launches on 2026-10-13 (5.11).
- **0.001 ms costs the game no more than 0.01 or 0.1 ms.** Eight rows of each on 2026-10-13:
  0.01 ms read 1.2 points cheaper than 0.001 and 0.1 ms read 0.6 dearer, each good to 2.6 (5.11).
  So the setting stays at 0.001 ms, which leaves the most room under the closest pair seen in
  the user's game (0.037 ms on the 2026-10-12 stream, where 5.9% of pairs came under 0.1 ms
  apart and none under 0.01).
- **Under DLSS frame generation** WGC delivers one frame for each real frame. About two a
  second also bring a second frame 1.9 ms before them: a different picture, the softer of the
  two in 168 of 179, which is what a generated frame followed by its real frame would read as
  (5.11). Capturing the game's window in place of the display gives the same frames.
- **A moving mouse pointer costs the game about 145 points while WGC captures, and it is
  Windows'.** Five sittings on 2026-10-14 (5.13). With a WGC session open and the pointer moving,
  the compositor (`dwm.exe`) presents about 160 times a second where it presents none otherwise;
  OBS's display capture by WGC pays the same, and no WGC setting tried removes it. The user
  accepted it: the pointer is hidden in their game's play.
- **WGC's points split as presenting about 78 (the draw 21, the present itself 57) and about
  58 for the frame pool sharing the present's device** (5.13). A device of its own for the pool
  takes the 58 back: 58.5 +- 4.0 points in one launch, WGC about 192 against 251 (5.13).
- **A copy off the present's device costs more than the device does** (105 points, 5.13), so
  the pool's frames are opened on the present's device and never copied.
- **With the pool on its own device the relay costs the game less than OBS's game capture.** In
  one launch on 2026-10-15 (5.15): 17.1 +- 2.3 points less than OBS's projector with game
  capture, and 64.0 +- 3.5 less than the pool on the present's device.

**Retracted (2026-10-09 readings):** "under Smooth Motion WGC's pairs arrive 1 to 4 ms apart", "a
6 ms pair window pairs every wake". They were one point of a drift that the 1 ms setting made.

## 4. The first stream (2026-10-11)

Get Medieval (KCD at 60 fps with Smooth Motion x2), 2h34m, the CI build of `e79a362`, mode line
`-mark 5400 -wgc`, driver 610.88. The log is `Get Medieval 2026-10-11 0.log` in the analysis
root. Everything below is on the stretch the stream's video shows, relay time 51.4 to 9292.4 s,
lined up by the 2,340 marked frames of its opening (2,282 of them within 0.37 ms at the 90th
percentile). The capture figures are from an analysis tool run over relay time 51.4 to 9292.4 s,
which sorts each second into ordinary (the 60 fps regime), extra (out of the regime
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
stretch is read less 1 s at its start and 0.5 s at its end. The logs, the runner's logs and the
reader are kept with the private analysis tools. Every arm is `-source 0 -target 1 -framerate b:vsync -src
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

Over the route (relay seconds 12 to 75): with the drop on no second is
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

### 5.10 The first stream with the pointer's frames left out (2026-10-12)

Get Medieval, 31 minutes, the CI build of `407caaf`, typed at the prompt with the frame marker on
for the first 5400 presents and `-wgc`. The log is `Get Medieval 2026-10-12 0.log` in the
analysis root. The video pairs with it at log time = video time + 45.155 s, from 2,716 marked
frames within 0.45 ms (p90), and everything below is on the stretch the video shows, relay time
45.2 to 1926.6 s, read from the log and the stream's post-stream report.

| | 2026-10-08, NvFBC | 2026-10-09, NvFBC | 2026-10-11, WGC | 2026-10-12, WGC, pointer frames left out |
|---|---|---|---|---|
| length | 2h16m | 32m | 2h34m | 31m |
| seconds with 70 or more batches | 0 | 0 | 449 | 0 |
| runs of more than 2 s out of the 60 fps regime | 0 | 0 | 74 | 0 |
| lock releases, and an hour | 7, 3.1 | 2, 3.7 | 303, 118 | 1, 1.9 |
| batches of dejitter skipped | 0 | 0 | 148 | 0 |
| blends, share of presents | 0.32% | 0.66% | 0.79% | 0.56% |
| shown frame steps one period, ordinary seconds | 99.73% | 99.43% | 99.41% | 99.31% |
| source gaps over 25 ms, an hour | 128 | 302 | 278 | 231 |
| content repeats an hour | 193 | 322 | 468 | 806 |
| presents blocked over 25 ms, an hour | 29 | 73 | 76 | 80 |
| video freezes a second of motion | 0.73 | 0.46 | 0.51 | 0.37 |
| upstream freezes that could be the relay's, at most | 9 of 1,237 | 2 of 175 | 51 of 1,418 | 9 of 324 |

- **The menus are back in the regime.** 4,338 frames were left out over the run. The game
  display had 140 or more flip requests in 98 seconds, which is the pointer moving in a menu,
  and in none of them did the ring see more than its 60 batches. 111,869 of the ordinary
  seconds' batches were pairs (99.9%), 0.27 ms apart at the median, with 64 batches of one and
  7 of three among them.
- **No game frame was lost to the rule.** Second by second, the wakes stored were never 4 or
  more short of the flip requests the driver logged for the game's pairs (224,199 wakes against
  222,486 such requests over the stretch).
- **The repeats are the game's stalls.** Content repeats an hour are the highest of the four
  streams. The holds all fall in 12 of the 32 minutes, the ones where the game's own request
  rate dipped, and the game stalled for 100 ms or more 68 times an hour here against 23 on
  2026-10-08. The marked opening had 2 freezes in motion, both the game's.
- **It joined the replay corpus** as `gm_60x2_gameplay_2026_10_12_0.trace`, cut at 1908.0 s where
  the video's last 16 s show a menu: no run out of the regime, and the replay matches the field
  (synth 0.5% against 0.5%, worst run 20 against 20 at the same present, pull wraps 29 against
  29, the one lock release at the same present).

Not shown by one stream of 31 minutes: a rate for lock releases worth comparing with the longer
streams, and anything about another game's pointer (section 8).

### 5.11 The benchmark sittings of 2026-10-13

Three sittings in the Avatar benchmark, one launch each, one run a row. The user asked for the
drop's row, the mouse pointer in the picture, and the least time between frames again with
more data. The sittings are `wgcset1`, `wgcset2` and `ww1`, kept with the private analysis
tools.

**The five settings.** Every row is `-source 0 -target 1 -framerate b:vsync -src 60 -wgc` and
the flags shown, on zig builds of the tree with `-wgccursor` in it. The 0.01 and 0.1 ms rows
are builds of the same tree with that one number changed. Each sitting ran the five settings
in four cycles, each cycle the mirror of the one before, with no-relay rows at the start, the
middle and the end. A cost is points under the no-relay level at the row's time. A difference
is from a fit of all 40 rows with a drift curve for each sitting, and its spread is one
standard error. The fit puts the noise at 5.0 points a row.

| setting | flags | rows | mean cost | difference from the first row |
|---|---|---|---|---|
| as it ships: 0.001 ms, pointer frames left out, cursor off | none | 8 | 243.7 | |
| 0.01 ms | none | 8 | 242.1 | -1.2 +- 2.6 |
| 0.1 ms | none | 8 | 243.4 | +0.6 +- 2.6 |
| pointer frames kept | `-nowgcdrop` | 8 | 242.2 | -1.1 +- 2.6 |
| mouse pointer in the picture | `-wgccursor` | 8 | 242.6 | -1.1 +- 2.7 |
| the CI build of `407caaf` | none | 4 | 247.6 | |
| NvFBC, the CI build of `407caaf`, without `-wgc` | none | 4 | 289.8 | |

No setting differs from the first row by more than half of its standard error. The first
launch's no-relay rows read 5357, 5391 and 5379 and the second's 5337, 5350 and 5354, so WGC
costs about 4.5% and NvFBC about 5.4%.

- **The least time between frames.** 0.001, 0.01 and 0.1 ms cost the same. A saving of 10
  points from a longer one is more than three standard errors away for both. 1 ms was not run
  again.
- **Leaving out the pointer's frames.** Reading each frame's changed areas costs nothing these
  sittings can see; `-nowgcdrop` does not read them. A row with the drop on left out no frame or
  one. A readback row (below) found that one: a 32x48 rectangle at the centre of the screen,
  0.36 s after capture started and before the run, a copy of the frame before it. Inside the
  run every one of 7,689 frames was reported as the whole picture.
- **The mouse pointer in the picture.** `-wgccursor` logs `cursor on (-wgccursor)`, stores the
  same frames and costs the same. Corrected 2026-10-14: this said the benchmark shows no
  pointer. The pointer sits at the centre of the card's picture in all 8 `-wgccursor` rows (the
  runner's screenshot is 5120x2880 from 0,-1440, the card the top-right quarter and the game
  display the bottom-left, where a desktop copy never shows the pointer). So these rows price a
  still pointer drawn into the picture. A moving one is priced in 5.13.
- **What every relay row captured and showed.** WGC stored 92.1 to 93.3 frames a second, 3.1 to
  5.6% of them in pairs, and NvFBC 155 to 161 in 90 batches. Every row presented 60.00 to 60.01
  times a second with no hold and 6.1 to 7.8% blends, and the card showed the game in every
  screenshot. Three rows early in the first sitting had one present gap over 25 ms each (29 to
  33 ms); the second sitting had none.
- **Heat.** The first launch's no-relay level rose 34 points in 27 minutes and fell 12 in the
  next 27, and the second launch, started with the card warm, sat 20 to 40 points lower. Heat
  and the ordinary difference between launches are not told apart by this. Seven readings taken
  33 to 38 minutes into the first sitting gave 62 to 64 C under load, 24 to 26 C short of the
  card's limit, with no thermal slowdown.

**Window capture (`ww1`).** The experiment branch has `-xwgcwindow` (`3d6c8cb`, zig build
`wgcwindow-1`): the session is made with `CreateForWindow` on the window at the centre of the
source display. It captured `Avatar: Frontiers of Pandora` (class `AFOPClass`), 2560x1440 at
0,0. Every row is `-source 0 -target 1 -framerate b:vsync -src 60` and the flags shown.

| row | flags | cost | frames stored a second | what it showed |
|---|---|---|---|---|
| display, two rows | `-wgc -xwgcinterval 1` | 251.6 and 241.9 | 92.6 | the game on the card |
| window, two rows | `-wgc -xwgcinterval 1 -xwgcwindow` | 248.4 and 248.1 | 92.8 | the game on the card |
| display, readback | `-xwgc 32 -xwgcmeasure -xwgcdirty -xwgcinterval 1` | not a cost row | 92.4 | picture not valid |
| window, readback | the same and `-xwgcwindow` | not a cost row | 92.6 | picture not valid |

Capturing the window gives the frames display capture gives, at the same cost as far as two
rows each can tell. It does not bring DLSS frame generation's generated frames: the game
showed about 181 frames a second and both captures stored 92 to 93. The readback rows hash the
centre of every frame. Inside the benchmark's three tests every frame was a new picture; the
676 and 688 copies all sit in the two breaks between tests. 179 frames of the display row and
185 of the window row came under 3 ms after the frame before them (1.9 ms at the median). The
two frames of such a pair are different pictures in 177 of 179 and 185 of 185, and the first
has the lower edge energy in 168 and 181, 0.87 of the second's at the median. That is what a
generated frame followed by its real frame reads as under Smooth Motion; the edge test has not
been checked by eye under DLSS. So about two generated frames a second reach WGC under DLSS
frame generation, of about 90.

Not tested: window capture under Smooth Motion, a window that does not fill the display, and a
window that moves, resizes or closes.

### 5.12 The first streams with the pointer in the picture (2026-10-13)

Two streams back to back from one relay launch, the CI build of `700bb9d` (the pointer in the
picture by default, the drop's line at a twentieth of the picture), typed at the prompt as
`-wgc -mark 5400`. The log is `Get Medieval 2026-10-13 0.log` in the analysis root; the start
line reads `cursor on (-nowgccursor leaves it out)`. Stream 0 runs 69.7 minutes and pairs with
the log at log time = video time + 51.200 s, from 2,355 marked frames within 0.40 ms (p90), relay
time 51.2 to 4235.2 s. Stream 1 runs 60.0 minutes with no marker, and streamcheck's game-stall
match was not decisive (ratio 2.5). It is placed from the OBS log's stream start times at log
time = video time + 4247.6 s, good to a few tenths of a second: the held-present check put its
best match 13 frames away, with its thirds disagreeing. So its counts below are per second and
nothing about it is frame-exact. The wake, request and release counts are from the log (the
capture and flip lines, and a count of `lk=1` to `lk=0` on the temporal lines), over each
stream's stretch.

| | 2026-10-12, `407caaf` | 2026-10-13 stream 0 | 2026-10-13 stream 1 |
|---|---|---|---|
| length | 31m | 69.7m | 60.0m |
| seconds with 70 or more batches | 0 | 0 | 0 |
| seconds with 140 or more game-display requests (a pointer moving in a menu) | 98 | 188 | 24 |
| batches in the 60 fps seconds that are pairs | 99.9% | 99.9% | 99.9% |
| seconds where the ring got 4 or more fewer wakes than the game's pair requests | 0 | 0 | 0 |
| singles alone on the cadence, where a pair should be | 10 (19 an hour) | 22 (19 an hour) | 0 |
| lock releases, and an hour | 1, 1.9 | 24, 20.7 | 21, 21.0 |
| video freezes a second of motion | 0.37 | 0.22 | 0.20 |
| frames left out by the drop | 4,338 | 9,986 over both streams | |

- **The pointer is in the stream's picture.** The game's yellow arrow shows on the map at 3:55
  and 4:05 of stream 0 and on a loot screen at 21:42.
- **Menus held their 60 pairs a second** with the pointer drawn, through 212 seconds of a
  pointer moving in a menu over the two streams.
- **No game frame was lost**, and lone singles came at the 2026-10-12 rate.
- **The lock released 45 times, all in 60 fps seconds, and the replay puts them there too.**
  Stream 0 joined the corpus as `gm_60x2_gameplay_2026_10_13_0.trace` (cut at 4213.0 s), and its
  replay releases the lock 24 times at the same moments as the field. So the releases come
  from the frames' arrival times and the policy's answer to them. 27 of the 45 come 0.2 to
  0.4 s after a capture gap of 25 to 50 ms; 3 fall in or beside a menu. Across streams, gaps of
  25 to 50 ms an hour and the share of them a release follows within 0.5 s:

  | stream | gaps of 25 to 50 ms an hour | followed by a release |
  |---|---|---|
  | 2026-10-13 stream 0, WGC | 213 | 5.3% |
  | 2026-10-13 stream 1, WGC | 222 | 6.3% |
  | 2026-10-12, WGC | 124 | 0% |
  | 2026-10-11, WGC | 162 | 6.3% |
  | 2026-10-09, NvFBC | 184 | 2.0% |
  | 2026-10-08, NvFBC | 77 | 4.0% |

  The game had more small gaps on 2026-10-13 than on any stream read before. Whether WGC
  releases on a small gap more often than NvFBC is not settled by four short-gap counts this
  far apart; the 2026-10-08 fixture's index note says its 7 releases follow hitches of about
  30 ms, so the pattern is not new to WGC.
- Stream 1 did not join the corpus: it has no alignment the fixture check accepts.

### 5.13 The sittings of 2026-10-14: the moving pointer, where WGC's points go, the pool's device

Avatar benchmark sittings in one launch each, one run a row, no-relay rows through each, kept
with the private analysis tools. The experiment branch's flags are named as
they were run (`-xwgcinterval 1`, `-xwgcdrop` and `-xwgccursor` there match the user's branch's
defaults).

**A moving pointer** (`ptrset1`, `ptrwhy2`, `rnd1`, `nvc1`, `wnd1`, `ivm1`, `pmptr1`, `pmptr2`;
the runner moving the pointer about 63 times a second through each run):

| capture, pointer moving against still | cost of the moving pointer, points |
|---|---|
| no relay | +0.7 +- 1.4 |
| relay `-wgc` (display) | +142.5 to +158 (five sittings) |
| relay `-wgc -nowgccursor` / `-wgc -nowgcdrop` | +145.5 / +144.5 |
| relay window capture (`-xwgcwindow`) | +158, the same pointer frames |
| WGC's changed-area mode set to ReportAndRender (`-xwgcrender`) | about 12 less |
| least time between frames 4 ms / 16 ms | the same / 75 less, but 16 ms stores 53 frames a second and blends 51% of presents |
| relay on NvFBC, hardware cursor off as shipped | none |
| relay on NvFBC with the hardware cursor (`-xnvcursor`) | +68 (and +25 with the pointer still) |
| OBS display capture by WGC / by Desktop Duplication | +158 / +3.5 |
| OBS game capture | none |

PresentMon (`pmptr2`): `dwm.exe` presents 0 times a second with no relay and the pointer moving,
0 with WGC and the pointer still, 0 with NvFBC's hardware cursor and the pointer moving, and
about 160 with WGC and the pointer moving (101.5 Hardware: Legacy Flip with a median 1.24 ms of
GPU each, 59.4 Composed: Flip). The game stays in Hardware: Independent Flip throughout. While a
WGC session is open Windows draws the pointer in the compositor in place of the hardware cursor
plane, which Desktop Duplication does not do. The user, 2026-10-14: the cost is fine "as long as
the performance actually in game is still good" ("it might matter for like an RTS game"). In the
user's game the pointer is hidden in play and the 2026-10-12 sitting saw no pointer frames
there. Nothing to build.

**Where WGC's points go** (`split1`, 22 rows, four a setting, pointer still; every row
`-source 0 -target 1 -framerate b:vsync -src 60` and the flags shown; paired against wgc):

| setting | flags | mean score | against wgc |
|---|---|---|---|
| no relay | | 5383 | +250.3 +- 5.3 |
| wgc | `-wgc -xwgcinterval 1 -xwgcdrop -xwgccursor` | 5132.5 | |
| nodraw | the same and `-xnodraw` | 5153 | +20.8 +- 2.3 |
| div2 | the same and `-xpresentdiv 2` | 5171.5 | +39.0 +- 3.2 |
| stand | `-xwgc 32 -xwgcinterval 1` | 5201 | +68.5 +- 3.3 |

Presenting is about 78 points (twice what presenting half as often saved), as on NvFBC (75,
2026-10-07): drawing WGC's frame about 21, the present itself about 57. The stand-in (WGC
delivers into a pool on a device of its own and nothing reads it; the present draws ring slots
nothing wrote) presents and blends like the real path. GPU context switch traces on 2026-10-15
(`performance-candidates.md` section 8) found what those parts are on the GPU: every present
takes the game's graphics engine once, with or without a draw, and costs the game about 0.15
to 0.17 ms, two thirds of it switching the engine from the game to the relay and back; the
compositor's 110 visits a second for WGC cost the same each and make up Windows' share.

**The pool's device** (`place1`, 10 rows): no relay 5384 and 5397; wgc 5116 and 5138; the
stand-in 5202 and 5201; the stand-in with its pool on the present's device (`-xwgcpresentdev`)
5141 and 5145. Moving the pool onto the present's device with nothing else changed costs 58.5
points. A copy off it (`copy1`, 10 rows): the stand-in 5184 and 5193, the same copying every
frame on the pool's own device (`-xwgccopy`) 5079 and 5087, so a copy costs about 105 points,
more than the device. A probe read the pool's textures as shareable (usage 0, bind flags 0x28,
misc flags 0x802; `GetSharedHandle` refused, `CreateSharedHandle` worked).

**A device of its own, opened by the present** (`open1`, 14 rows, experiment build `wgcopen-1`,
`-xwgcopen`): no relay 5372, 5397, 5386 and 5390; wgc 5123, 5142, 5134 and 5147; with the pool
on its own device 5188, 5185, 5200 and 5207. It is 58.5 +- 4.0 points cheaper paired (58.7 +-
4.6 fitted): WGC about 192 points against 251. What the rows captured and showed: 94.2 to 94.9
frames stored a second (wgc 92.8 to 93.4), 60.01 presents a second, no holds, no present gap
over 25 ms, blends 4.6 to 5.7% (wgc 6.3 to 7.3%, not read why), all 34 pool textures opened, and
the card showing the game in every screenshot (card against game 6.5 to 6.9 where the control
reads about 9.6, as the wgc rows). OBS's game capture cost 216 (`wg1`, 2026-10-09), 207.6
(`ptrwhy2`, 2026-10-14) and about 202 (`dl3`, 2026-10-08) in other launches; no sitting had both
in one launch until 5.15. Why the pool's device costs 58 points is not known.

**The user's decisions that day.** WGC stays off by default: "let's actually keep it not
default for now". Window capture costs what display capture costs and pays the same for a
moving pointer; its use would be leaving notifications, other windows and the desktop out. The
relay drawing the pointer itself on NvFBC "does seem like it might be useful for a pointer with
nvfbc if someone really wants to use nvfbc" (not built). A hook of the relay's own: "a whole
future project probably. not part of the optimization work". On 2026-10-15, after the streams of
5.14, the pool on its own device became the default inside `-wgc` with the fall-back of section
2.

Under NvFBC the busy core seen on a still desktop is NVIDIA's (a desktop check with
`-xthreadcpu`): one thread starting in `nvd3dumx.dll` holds 93 to 95% of a core and the relay's
own threads stay under 1%; on WGC the whole process uses under 1%.

### 5.14 The first streams with the pool on its own device (2026-10-14)

Two streams, each from a relay launch of its own, the CI build of `ab1772e` typed at the prompt
as `-wgc -wgcopen -mark 5400`. The logs are `Get Medieval 2026-10-14 0.log` and `1.log` in the
analysis root. Each log's start line names the pool on a device of its own and its summary reads
`opened 34 pool textures on the present's device, 0 could not be opened`; no present reported a
problem, no draw failed and no frame wait timed out. Stream 0's video stops at 26.4 minutes,
where OBS lost its connection to Twitch (OBS ran 33.4 minutes); stream 1's runs 39.8 minutes.
Both videos have no holes and pair with their logs through the marker (log = video + 45.655 s,
2,691 frames, p90 0.38 ms; log = video + 52.101 s, 2,300 frames, p90 0.41 ms). The counts are
over the stretch each video shows.

| | 2026-10-13 stream 0 | 2026-10-13 stream 1 | 2026-10-14 stream 0 | 2026-10-14 stream 1 |
|---|---|---|---|---|
| pool | present's device | present's device | its own | its own |
| length | 69.7m | 60.0m | 26.4m | 39.8m |
| wakes a second in steady play | 120 in 60 pairs | the same | the same | the same |
| batches in the 60 fps seconds that are pairs | 99.9% | 99.9% | 99.9% | 99.9% |
| seconds where the ring got 4 or more fewer wakes than the game's pair requests | 0 | 0 | 0 | 0 |
| singles alone on the cadence | 22 | 0 | 0 | 0 |
| lock releases an hour | 20.7 | 21.0 | 4.5 | 9.0 |
| video freezes a second of motion | 0.22 | 0.20 | 0.36 | 0.96 |
| at most the relay's, after the marked opening | 8 of 358 | | 8 of 299 | 2 of 818 |

- **The pairs held** under Smooth Motion with the pool on its own device, with no game frame
  lost.
- **The freezes are the game's scenes.** Stream 1's worst 30 s stretches are the dice game (six
  of eight), a loot screen and a trader's inventory; stream 0's are inventory and loot screens.
  Those are the scenes where the game drew the same picture twice most often on the fully
  marked recording of 2026-10-01. 229 of stream 1's 890 freezes run into the next frame (48 of
  299 on stream 0, 25 of 384 on 2026-10-13 stream 0), pictures held three frames or more, in the
  dice game. That points to the game; it does not rule out the pool's device for freezes.
- **No torn or stale picture.** A picture the present read before Windows finished writing it
  would most likely show an older frame, whole or in part, between two frames that match each
  other. Every frame whose differences to both neighbours are large against the motion either
  side was listed from the post-stream report's cache (factor 2), and each was decoded with its
  neighbours: it reads apart when the frames before and after are
  closer to each other than either is to it. Controls, planted in 6 s of stream 0's fastest
  steady motion: a picture 17 frames old reads 0.32, half the picture 17 frames old 0.76, every
  other 32-pixel tile 17 frames old 0.54, an untouched frame 1.70, and real menu-cadence frames
  1.36 and 1.74; the clean clip lists none, and at factor 4 the scan missed the half and the
  tiles. Results:

  | | candidates | apart | what they are |
  |---|---|---|---|
  | 2026-10-13, pool on the present's device, 129.7 min | 150 | 4 | a list scroll step, a one-frame shot in a dialogue camera cut, the hand on a polearm for one frame (66:42.15), a dark band across a tent for one frame (66:56.05) |
  | 2026-10-14, pool on its own device, 66.2 min | 93 | 5 | a one-frame black blink of the loading screen (0:58.433 of stream 0), four list scroll steps |

  At each one-frame picture the relay showed one new capture a present in arrival order. On
  2026-10-13 the pool shared the present's device, where Windows orders its write before the
  present's read, so the game draws such frames by itself. At the loading-screen blink the
  painting had been on screen unchanged for over 2 s, so every pool texture held it and a stale
  texture would have shown the painting. Not caught by the scan: a picture only a frame or two
  old (it reads as a freeze or a small jerk), and anything in still footage.
- **Lock releases fell** from about 21 an hour to 4.5 and 9.0. Stream 1's six follow capture
  gaps of 29 to 41 ms by 0.2 to 1.3 s. Not read further; the content differs from 2026-10-13's.
- **Both joined the corpus** as `gm_60x2_gameplay_2026_10_14_0.trace` (25.4 min, log 105.0 to
  1628.4 s) and `gm_60x2_gameplay_2026_10_14_1.trace` (39.1 min, 91.0 to 2439.6 s), each cut to
  gameplay; their replays release the lock at the same moments as the field.

### 5.15 The sitting of 2026-10-15: the default beside OBS's game capture

Sitting `obsopen1`: one launch, 18 rows, one run a row, the CI build of `00987d6`, four cycles
mirrored in pairs of no relay, OBS's projector with game capture (scene collection
`bench-game`), `-wgc` (the pool on its own device, the default) and `-wgc -nowgcopen`, with
no-relay rows at both ends. Every relay row was `-source 0 -target 1 -framerate b:vsync -src 60`
and the flags shown. It was the first sitting on a new capture card, set to a custom 2560x1440
mode that the relay's presents measured at 60.0003 Hz.

| setting | flags | mean score | paired against wgc | fitted |
|---|---|---|---|---|
| no relay | | 5374.75 | -195.5 +- 3.6 | -196.3 +- 2.9 |
| OBS game capture | | 5162.25 | +17.1 +- 2.3 | +16.7 +- 2.8 |
| wgc | `-wgc` | 5179.25 | | |
| wpres | `-wgc -nowgcopen` | 5115.25 | +64.0 +- 3.5 | +64.2 +- 2.8 |

Against the no-relay rows the default costs the game about 196 points (3.6%), OBS's game capture
about 213 (4.0%) and the pool on the present's device about 260 (4.8%). The device of its own
saved 64.0 points on the user's branch, where the experiment build read 58.5 +- 4.0 (5.13).

What the rows captured and showed: wgc stored 93.9 to 94.2 frames a second and wpres 92.5 to
93.0; both presented 60.01 times a second with no hold and no present gap over 25 ms (the longest
17.3 to 18.1 ms); blends were 5.6 to 5.9% with the pool on its own device against 6.4 to 7.5%
without, as in `open1` (not read why). Every wgc row logged the start line of a device of its own
and opened all 34 pool textures with none failing; no row fell back. OBS hooked the game in every
row (`d3d12 shared texture capture successful`). The card showed the game in every screenshot:
card against game 6.3 to 7.1 for the relay's rows and 0.6 to 2.6 for OBS's, where the control
reads about 9.5 and a row with no relay 42 to 50. The relay's larger figure is most likely its
delay: it presents a frame about 96 ms after it arrives (section 2), so a screenshot of a moving
game catches the two displays at different frames. OBS's delay was not measured. The no-relay
rows read 5342 (the first after the launch), then 5364 to 5381, level from minute 17 to the end.

So like for like and in one launch, the relay on WGC with its pool on its own device costs the
game about 17 points less than OBS's projector with game capture, the target the performance
pass set for the capture path. The relay's default capture, NvFBC, was not in this sitting; it
cost 287 to 297 points in other launches.

## 6. The cursor under each source

- **NvFBC.** The ring's grab session sets `bWithHWCursor = 0` (`CaptureRing.cpp`). That is T7 of
  `dual-device-capture-present-spec.md`: with it on, the blocking grab woke on every cursor
  move, at the mouse's polling rate, with duplicate content. It is a setting of the grab. It
  does not turn the hardware cursor off on the display. T7's own note says `= 1` was never
  tested in a game; the user, 2026-10-12: "I'm not convinced it does anymore with a game in
  fullscreen mode".
- **WGC.** `put_IsCursorCaptureEnabled(1)` by default, logged as "cursor on (-nowgccursor leaves
  it out)", and the pointer is in the captured picture. `-nowgccursor` sets it to 0, logged as
  "cursor off (-nowgccursor)", and leaves the pointer out as NvFBC's grab does. `-wgccursor` is
  accepted. Up to `f530b08` the pointer was left out unless `-wgccursor` was typed. A moving pointer brings its extra
  frames with the setting on or off (5.9), and they are left out the same (5.9, the run with
  cursor capture on). In the benchmark a still pointer drawn into the picture costs nothing
  (5.11), and a moving one costs about 145 points with the setting on or off (5.13).
- **In the stream's picture.** One inventory frame of the 2026-10-11 video (2:24:32) shows no
  pointer. The game's menu pointer is the system's pointer with the game's own shape (5.9: it
  appears on the card with cursor capture on and not with it off), so it has been missing from
  every stream on either source.

## 7. Tests still to run

In this order. Each is a rig job in the user's game with the user playing, a minute or two, and
each is read from its log unless it says otherwise. They are numbered W
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

**W1 to W5 ran on 2026-10-12; the results are in 5.9. W7 ran on 2026-10-13 (5.11): the drop
and the cursor setting cost nothing measurable.** W6 has not run. As W1 to W5 were prepared:

W1 to W5 are prepared as one rig job, kept with the private analysis tools: a
menu open (the inventory, if its centre moves, so that W4's hash can tell a copy), and for each
arm 10 s with the pointer still and 10 s with the runner moving it (`sweep`, relative mouse
input on a timer), twice. The arms are the stream's build with `-wgc`; the experiment build
with `-wgc -xwgcinterval 1`; the same with `-xwgccursor`; with `-xwgcdirty`; with `-xwgcinterval
1000` in place of 1; the readback (`-xwgc 32 -xwgcmeasure -xwgcinterval 1`); and the experiment
build on NvFBC. A last stretch has the user move the mouse by hand, in case input made by the
runner does not behave as a hand does. Not yet run.

Not a test, but owed from Part B: the short-run tool on the singles around hitches, if any are
to be classed.

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
- **How the drop is sized (agreed 2026-10-13, built 2026-10-15 as a patch, below).** The user asked what the size should
  be and called this answer reasonable. A frame is measured by the game's own frames. The relay
  remembers the largest changed area any frame was reported with, and forgets it when half a
  second passes with no frame at least that large. A frame is left out when its changed area is
  under a tenth of the remembered one. A frame with no list of changed areas, or an empty one,
  is kept as now, and `-nowgcdrop` keeps every frame as now. The user, afterwards: "I
  think we keep it at 10% for now. we'll adjust later if we need to", and then asked for 2% or
  5% of the picture "without requiring a ton more testing". So the rule as built moved from a
  hundredth to a twentieth of the picture (section 2), and this rule stays agreed and unbuilt.

  | case | the rule as built (a twentieth of the picture) | the rule sized from the game's frames |
  |---|---|---|
  | a game that fills the display, a pointer of the size measured | left out | left out |
  | the same, a pointer over a twentieth of the picture | kept | left out, up to a tenth of the picture |
  | a still screen where only the pointer moves | kept after the first half second | the same |
  | a game in a window smaller than the display (not seen) | kept, since no frame is the whole picture | left out |
  | a notification or overlay that updates between game frames (not seen) | kept unless under a twentieth of the picture | left out when under a tenth of a game frame |

  The reasons. A game frame was reported as the whole picture in both games read so far (the
  user's game in 5.9, and Avatar in 5.11: 7,689 of 7,689 frames and 7,706 of 7,706), while a
  pointer's size changes with the game, with Windows' scaling and with the resolution. The
  largest pointer frame seen is 0.06% of the picture and no frame between that and the whole
  picture has been seen, so the line can sit far from both. A WGC frame is a complete picture,
  since the changed areas are a report and the session's mode is left at report-only. So a
  frame that is left out loses nothing for good: what changed in it is in the next frame the
  relay keeps, at most one game frame later while the game is drawing.

  The tenth. The first write-up said a quarter, and the user asked whether that was too much.
  It was more than the pointers need. The largest pointer in Windows' own settings is 256
  pixels square at 100% display scaling (size 15 of 15; read from forum posts and a Microsoft
  Q&A answer, not checked on the rig), where the default is 32. When it jumps, its two
  rectangles are 6.3% of 1920x1080 and 3.6% of 2560x1440, and a notification of about 400 by
  150 pixels is 3% of 1920x1080, so a tenth covers them and is ten times smaller than a game
  frame. At 1280x720 that pointer's jump is 14% and would be kept. The number is kept as low as
  the pointers allow because the two mistakes cost differently. A pointer frame that is kept gives the menu behaviour of section 5. A frame of
  real content that is left out is a picture that does not change: an application that redraws
  only parts of its picture would have every update under the line left out for half a second
  after each whole redraw. The rule as built does that already for updates under a twentieth.
  On everything measured so far a tenth and a quarter give the same result, so the number is a
  judgement about cases not yet seen. The user accepted the tenth on 2026-10-13, and of the
  largest pointer on a 1280x720 display said "this seems unlikely".

  The changed area is Windows' own report, the `DirtyRegions` list each frame carries. The
  relay reads no pixels for it. The readbacks of 5.9 and 5.11 are the check that the report
  matched the pictures: every frame reported as a small change was a copy of the frame before
  it, and every frame inside the benchmark's tests was reported as the whole picture and was a
  new picture.

  Not seen, and to be read with `-xwgcdirty` on the experiment build before or beside building
  it: a third game's frames, a notification or overlay over the game, and a game in a window.
  The rule would lose frames in a game whose own frames are reported with areas more than ten
  times apart inside half a second. No game read so far does that.

  For a game that fills the display, a fixed share of the picture in `PointerOnly` gives the
  same result with one number, which is what was done (a twentieth). The remembered area is
  what makes the rule work for a game in a window.

  **Built 2026-10-15** at the user's word ("we'll add it in as the standard behavior" if it
  costs nothing), as `policy::IsPointerFrame` in the policy layer with `WgcCaptureSource`
  calling it in place of the twentieth, `-nowgcdrop` unchanged; committed as `4d7ba03`.
  Checked: unit tests in the suite (a full-screen game loses every pointer frame and no game
  frame; a game in a 1280x720 window, which the twentieth let through, the same; a frame of
  exactly a tenth kept and one just under left out; a 400x150 notification over a full-screen
  game left out; a still screen keeps the pointer's frames; frames with no list or an empty one
  kept and not refreshing the memory; and the agreed cost, a 640x360 window losing its own
  frames for 510 ms after a full-screen frame). Both rules replayed over the changed areas
  logged with `-xwgcdirty` on 2026-10-12 to -14 (seven logs, 62,718 frames, the user's game in
  its menus with the pointer moving and the Avatar benchmark): they agree on every frame, so in
  a full-screen game nothing changes. Live on the rig's desktop, a 1280x720 window redrawing at
  60 Hz with the pointer moving outside it (541 moves in 15 s): the twentieth left out 59 frames
  and stored 104 to 108 wakes a second, the new rule left out 1,391 and stored 61 to 62, the
  window's own rate. The same check found the documented cost in practice: while the window was
  the largest thing changing, small changes elsewhere were left out too (a taskbar button's
  animation, 2,248 pixels, 10 to 25 times a second; 154 of 196 frames of 0.1 to 5% of the
  picture while the pointer moved). Each is in the next frame kept, since a WGC frame is a
  whole picture. Benchmark, one launch (sitting `pdrop1`, `b:vsync -src 60 -wgc`, the rule as
  it was against the rule sized from the game's frames): with the pointer still, four rows
  each, -1.5 +- 1.9 points paired (-1.2 +- 4.2 fitted); with the pointer moving through the
  run, two rows each, -4.0 +- 3.0 (-3.7 +- 5.9). Both rules left out none with the pointer still
  and the same with it moving (4,928 and 4,921 against 4,927 and 4,930 a run); every row stored
  91 to 94 frames a second, presented 60.01 times a second with no hold and no present gap over
  25 ms, and the card showed the game. It costs nothing, and in a full-screen game it decides as
  the twentieth did.
- **Whether the pointer goes back in the stream's picture.** The user, 2026-10-12: "it's not a
  huge deal, although I would like to turn mouse back on at some point". On WGC that is cursor
  capture on, and 5.9 shows no cost in extra frames beyond what a moving pointer brings anyway.
  On NvFBC it is the hardware cursor setting of section 6, which in the game's menus brought
  170 wakes a second with nothing to tell them from frames (5.9). The user, 2026-10-13: "part of
  this benchmark should also be with the cursor re-enabled just for wgc mode? Since that's what
  we really want ideally". The switch `-wgccursor` is built and its benchmark rows cost nothing
  (5.11). DECIDED 2026-10-13: "let's make wgccursor the default now", the user streaming on that
  build next. The pointer is in the picture by default with `-wgc`, and `-nowgccursor`
  leaves it out. The default's rows are the `-wgccursor` rows of 5.11, so no new row is owed.
  Streamed the same night (5.12): the pointer shows in the menus and the menus held.
- **Which member the ring keeps from a batch of three.** Today it is the last, which is the
  extra about half the time. Neither "the last" nor "the second" is right for both orders, so
  the ring would have to tell the extra from the pair. W4 says how much it matters.
- **How a WGC stream joins the replay corpus.** As the regime check stands, every WGC stream
  with menu time fails it. Either the check learns what a menu looks like under WGC, or the
  singles stop reaching the ring.
- **Whether `-wgc` becomes the default.** DECIDED 2026-10-14, not for now. The user first said
  "seems fine to make wgc default first", then "let's actually keep it not default for now".
- **Where the frame pool lives.** DECIDED 2026-10-15: on a device of its own by default inside
  `-wgc`, with `-nowgcopen` to keep it on the present's device, after the 58 points of 5.13 and
  the streams of 5.14. The user asked for the device made on the present's adapter ("let's do
  the clean thing") and for the fall-back to the present's device where opening fails ("falling
  back to the previous behavior makes sense to me in this case"); a retry was asked about and
  is no use, since nothing about it is a race (section 2). Its benchmark on the user's branch ran
  on 2026-10-15 beside OBS's game capture (5.15) and saved 64.0 +- 3.5 points, where the
  experiment build had saved 58.5. The torn or stale frame question
  has two further answers if the scan of 5.14 is not enough: an exact check inside the relay (a
  few tiles' checksum on the pool's device at arrival against the present's device before
  drawing, logged with its time) or a fence (a shared fence the present waits on before
  sampling). Neither is built.

## 9. Untested elsewhere

Carried from `performance-candidates.md` section 9.1 and `session-kickoff-performance-pass-7.md`.
"A whole stream" is done (section 4).

- Capturing the game's window in place of the display (`CreateForWindow`). Run in the benchmark
  on 2026-10-13 (5.11): the same frames and the same cost as display capture under DLSS frame
  generation, and on 2026-10-14 the same cost for a moving pointer (5.13). Still unknown:
  whether window capture delivers Smooth Motion's generated frames, and everything a window can
  do that a display cannot (move, resize, minimise, close).
- The pool on a device of its own with the two displays on different graphics cards. The
  pool's device is made on the present's adapter, so the present opens its textures on one
  card; whether the session delivers there as it does on one card is not known.
- The fall-back to the present's device (section 2) has not run on any machine.
- Smooth Motion x3 and other source rates. `-fgphase` and `-phasekeep` on WGC.
- HDR sources. The pool is 8-bit.
- What WGC drops when a stall outlasts the pool's two spare buffers. The stream's log has not
  been read for it.
- The pairs under DLSS frame generation, 1.1 to 1.4% of wakes on 2026-10-11 and 3 to 6% on
  2026-10-13 at 0.001, 0.01 and 0.1 ms alike. The readback of 5.11 reads them as a generated
  frame followed by its real frame. Why only about two a second come through is not known.
- Displays of different sizes: scaled by the present since 2026-10-18 (section 2), measured only
  at 2560x1440 into 1920x1080. Not measured: a smaller source than the output, other ratios
  such as 4K into 1080p, ringing at mid-grey edges (`wgc-scaling-spec.md`).
- Any GPU but the rig's. On a Windows before build 26100 WGC stays off, since the setting it
  needs is not there.
- 0.001 ms compared with 1 ms. 0.001, 0.01 and 0.1 ms cost the same (5.11); 1 ms was not run
  again, and it is not a setting the relay can use (section 3).

## 10. Where things are

- **Code.** `src/relay/WgcCaptureSource.h` (the session, the pool, the settings, the pool's
  device and its fall-back),
  `src/relay/ICaptureSource.h` (what the ring asks of a source), `src/relay/CaptureRing.cpp` (the
  move from NvFBC, and NvFBC's cursor setting), `src/relay/FlipDecode.h` (what a flip line is).
- **Logs, sittings and tools** are kept with the private analysis tools: the logs of the WGC
  streams and of the NvFBC streams they are compared to, the short runs of section 3, the
  sittings of 5.11 and 5.13, and the tools that read them. One tool prints the capture, flip,
  lock and step counts of sections 4 and 5: batch sizes and gaps inside a pair, singles and
  batches of three and where the odd member sits, flip requests a second on the game display,
  the requests that belong to no pair and how many follow the relay's own present, the lock, the
  step of the shown frame and the batch it came from, and the seconds after each stretch of
  extras. Others list the seconds out of the gameplay rate, compare wakes with the game
  display's pair requests second by second, and, for a video's frames, list frames apart from
  both neighbours, decode each with its neighbours, and map where freezes fall (5.14). The rows
  an hour in section 4 are from each stream's post-stream report, and what was on screen (5.1)
  was read by eye from the video.
