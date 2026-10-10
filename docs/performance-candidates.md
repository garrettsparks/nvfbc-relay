# Performance candidates for `b:vsync`

What the default mode, `b:vsync`, costs the game, where that cost goes, and what each idea for
lowering it was measured to be worth. Sections 1 to 4 were rewritten on 2026-10-08 from the bench
runs of 2026-10-07 and the replay work of 2026-10-08. Section 9 is ideas nobody has measured,
except NvFBC's CUDA interface, which was built and measured on 2026-10-08 and 2026-10-09 and
dropped (3.4). The decision logic (comb lock, lookahead, late moves, refusal) stays as it is; the
candidates change how frames are captured, copied and drawn.

The goal (user, 2026-10-03): beat OBS's fullscreen projector on both game cost and picture
quality. The cost to beat is the projector's with game capture, 3.76% (user, 2026-10-09). NvFBC
captures the whole display and may not get there, so that may have to wait for a test of WGC or
Desktop Duplication capture (9.1).

## 1. Where things stand

Every number is the Avatar benchmark at 2560x1440 with DLSS frame generation 2X. Uncapped it
keeps the GPU fully loaded, so anything the relay costs shows up as a lower score. A point is one
point of that score. The score with no relay is about 5,380, so 54 points is 1%.

The game's level differs between launches by tens of points (no-relay rows on fresh launches read
5,345 to 5,394 in one afternoon), while runs inside one launch agree within about 6. Since
2026-10-07 rows are therefore compared inside one launch: the game stays running, the relay is
swapped between rows, a reference row and a no-relay row run at both ends, and the first run
after the launch is thrown away. A sitting is one such batch of rows, a row is one configuration,
and a run is one pass of the benchmark. The level still rises inside a launch, steeply for the
first two or three rows and then by 17 to 35 points over an hour, which the rows at both ends
correct for.

| row | cost | measured |
|---|---|---|
| `b:vsync -src 60` | 332 points, 6.2% | one-launch sitting, 2026-10-07 |
| `60` | 4.69% | one-launch sitting, 2026-10-07 |
| `60` with the original relay's timer (32.3 loops a second) | 3.05% | the same sitting |
| `b:vsync -src 60` with the deferred ring copy (the default since 2026-10-08) | 5.1 to 5.4% | three one-launch sittings, 2026-10-08 |
| OBS projector, game capture, 2560x1440 canvas | 3.76% | one-launch sitting, 2026-10-08 |
| OBS projector, display capture, 2560x1440 canvas | 6.23% | the same sitting |
| OBS projector, game capture, 1920x1080 canvas | 4.85% | by hand on 2026-10-03, separate launches |
| `b:vsync -src 60`, game capped at 60 with 2X | no frame rate, about 0.4 points of GPU % | 2026-10-07 |

Compared with OBS's fullscreen projector showing the same 2560x1440 picture, `b:vsync` costs
about 1 point less than OBS with display capture and about 1.4 points more than OBS with game
capture. How the picture is captured decides it: game capture takes the game's own frames from
inside the game's process, where display capture and NvFBC take the finished desktop. OBS's logs
of those two rows show the methods. Its display capture used DXGI Desktop Duplication (`method:
DXGI`). Its game capture loaded a hook into the game and shared the game's back buffer (`d3d12
shared texture capture successful`). Only cost was measured for OBS on 2026-10-08; its pacing
and picture were not. A second sitting on 2026-10-09 ran OBS with each of its capture methods
beside the relay in one launch (9.1): game capture 4.0%, the relay 5.3%, window capture by WGC
5.4%, display capture by WGC 5.5% and display capture by Desktop Duplication 6.8%.

The 6% exists only when the GPU has nothing to spare. Capped, the relay takes no frame rate.

On quality `b:vsync` was ahead on 2026-10-03, when the projector ran game capture on a 1080p
canvas. In motion the relay showed 1.5 repeated frames a second where the projector showed 3.7
(`mgdupes.py` over ten 30 s windows of each recording, no marker, so what the chain and the
game add is not split out). The repeat count does not see blends or
skips.

The zig cross-build used for experiments costs the same as the MSVC build as far as two launches
each can tell (4 points apart), and logging costs nothing measurable.

The hand sessions of 2026-10-03 (`b:vsync` 6.1 to 6.4%, `60` 4.95%, the original relay 2.79%)
and `docs/relay-cost-results.md` (2026-09-18) compared rows from separate launches. Where a
one-launch row exists it replaces them.

## 2. What `b:vsync` does each second

Read from the code at `5996f29`. In the benchmark the capture thread sees about 151 grabs a
second; on capped 60x2 gameplay about 120.

| work | per second | where |
|---|---|---|
| NvFBC grab into one staging surface (10-bit, HDR requested, scaled to 2560x1440) | 151 | `CaptureRing.cpp:296` |
| full-frame StretchRect, staging surface to ring slot (14.7 MB each way) | 151 | `CaptureRing.cpp:334` |
| event query and a busy spin with `D3DGETDATA_FLUSH` until the copy is done | 151 | `CaptureRing.cpp:339-342` |
| retract the previous slot when the wake is a batch's second member | about 60 | `CaptureRing.cpp:526-530` |
| full-screen draw of the blend shader, two slots sampled (passthrough samples one slot twice) | 60 | `D3D11Present.cpp:410-444`, `:561-592` |
| `Present(1, 0)` on a 2-buffer FLIP_DISCARD swapchain, frame latency 1 | 60 | `D3D11Present.cpp:676` |

The retracted member was already copied and flushed. Nothing sets thread or GPU priority. A D3D9
present device with a 2560x1440 back buffer, and 32 ring slot aliases on it, exist in `b:vsync`
but do no per-frame work (`D3D9Setup.cpp:55-65`, `CaptureRing.cpp:166-177`).

The original relay does one grab straight into a D3D9 back buffer and one present per loop, with
no ring, no copy and no flush. Its loop runs 32.3 times a second (section 4).

Where the 332 points go was priced in two sittings on 2026-10-07 with switches that each leave
one piece of work out. Some of them show a wrong picture and exist only to price that work.

| part | points | how it was priced |
|---|---|---|
| presenting, 60 a second | about 75 | half the presents saved 38; a row that captured nothing cost 77 |
| the ring copy and the wait after it | about 80 | no copies at all +81; a pair's first copy skipped and no wait +77 to +89 |
| NvFBC delivering each frame | about 175 | what is left; no switch moved it |

The grab of a frame generation duplicate is cheap and the grab of a new frame is dear. Pausing
8 ms before each grab removed 43% of the wakes, the pairs' extra members, and saved 75 points,
about what skipping only those members' copies saved (64). Inside the present's 75 points the
draw is small: presenting with no draw at all saved 9 to 31. A ring copy completes in about
0.1 ms when the game is capped (p95 0.14 ms) and takes 10 ms at p95 when the GPU is saturated.

## 3. Candidates

Gains are in points over the reference row of the same sitting with the drift removed. Rows are
two runs each and repeat within about 6 points; the early rows of the second sitting are good to
about 15.

| candidate | measured | status |
|---|---|---|
| Copy only the kept member (the deferred ring copy) | priced at +64; built, about +44 (2026-10-08) | on by default; `-nodefercopy` turns it off (3.1) |
| Stop waiting for the remaining copies (`-nowaitcopy`) | priced at +13 to +25 on top of the row above; built, about +11 on top of it (2026-10-08) | built on a local experiment branch (3.2) |
| Skip the extra wake (`-grabphase`) | 8 ms before each grab: +75 for 43% fewer wakes | helps only when the game outruns the card (3.3) |
| NvFBC's own grab | about 175 points, by subtraction | open; one profiled look planned (section 8) |
| NvFBC's CUDA interface in place of the D3D9 one | with nothing copied, 223 points dearer than the D3D9 grab with nothing copied, in 10-bit and in 8-bit (2026-10-09) | dropped (3.4) |
| Presenting | about 75 points for 60 presents | no cheap lever found |
| Plain copy in place of the shader draw, on an 8-bit ring | +4 to +22 alone; added to the copy-side pair, less than the pair alone | dropped (3.4) |
| A grab without scaling, full-size or crop | +8 to +22, and -14 to +11; full-size again on 2026-10-08, about +4 | full-size committed on 2026-10-08 as the more correct request, used when both displays are the same size (3.4) |
| Low GPU priority for capture | -7 starved capture to 18 frames a second; -2 did nothing | dropped for a GPU-bound game; untested capped (3.4) |
| 8-bit capture and ring alone | +4 | dropped |
| 1x1 back buffers on the idle D3D9 devices | +5 | committed on 2026-10-08 as a tidy-up (about 29 MB of video memory) |

The first two rows are the copy side, about 80 points in all when it was priced. As built they
take `b:vsync` from about 6.1% to about 4.9% (2026-10-08).

The full-size grab and the 1x1 back buffers gained too little to tell from zero in rows that
were good to about 15 points. Small gains add up (user, 2026-10-08), and both cost nothing, so
both went in.

### 3.1 Copy only the kept member (`-defercopy`)

Under frame generation x2 each source frame wakes the grab twice, under 3 ms apart, and the
relay keeps the second. Today both wakes are copied and flushed, and the first is retracted a wake
later. Deferring the copy until the next wake shows which member a frame is removes the wasted
copy and flush.

As built: two NvFBC output buffers are taken in turn (`dwBufferIdx`; SetUp accepts two on driver
610.88, and eight crashed inside NvFBC in July, `8eb5e70`). While batches average 1.6 members or
more, a batch's first member is not copied and waits in its buffer. If the next wake is the same
batch's second member, the waiting frame is never copied. If the next wake opens a new batch, or
the grab times out, it was a batch of one and is copied then. Its slot is published with `valid`
off and turned on when the pixels are there. The decision is in the policy layer
(`policy::DecideDefer`), so the replay tests run it on recorded captures. Without frame
generation nothing waits. `-fgphase` and `-phasekeep` need every member's pixels and switch it
off, and generated-frame substitution would too if it returns. NVOFA interpolation works from
real frame pairs and is unaffected.

The first version saved about 30 points on single profiled runs on 2026-10-07 (5,074 where the
references read 5,040 and 5,050, and PresentMon's game frame time agrees). 35 to 39% of stored
frames are never copied and 2 to 5% are copied a wake late.

In the benchmark that version's pacing matched the reference rows, uncapped and capped: no
repeats, no holds, no present gap over 25 ms, and capped the shown-content steps were identical.
The blends at the benchmark's two scene changes range from 30 to 96 a run in the capped
reference rows alone, and its capped rows fell inside that range.

The benchmark never pauses, and streams do. Replayed over the 39 streams captured at the 75 ms
extra lag (10.1 hours, 2,165,858 presents), that first version shows 204 more repeats, nearly all
of them where the relay would have blended, and a blend is always better than a repeat. The
cause, worked out on 2026-10-08: a present first needs a frame one bracketing lag after the
frame before it arrived. A frame that follows a pause is needed sooner the longer the pause was,
and at once when the pause was longer than the lag, while the first version could leave it
waiting for the next wake or for the whole 100 ms grab timeout.

A shorter wait alone does not fix that, and more lag alone does not either:

| change, alone | repeats added in 10.1 hours |
|---|---|
| first version (a wait of up to 100 ms, 75 ms extra lag) | 204 |
| a waiting frame copied after at most 60 ms | 145 |
| after at most 20 ms | 78 |
| after at most 4 ms | 26 |
| 100 ms extra lag | 177 |
| 125 ms extra lag | 145 |
| 150 ms extra lag | 123 |

Three rules together remove all of them:

1. A frame waits only when the gap before it, the longest it can wait and a margin fit inside
   the bracketing lag (`policy::DeferMaxGap`). The margin is one refresh of the output, the time
   a ring copy can take on a saturated GPU.
2. While a frame is waiting the grab waits a shorter time than its usual 100 ms, sized from the
   lag (`policy::SizeDeferLimits`): the longest wait from about 55 ms down to 40 that still
   leaves rule 1 room for the source's own cadence. At 60 fps that is 58.3 ms (3.5 source
   periods) from `-lag 75` up, where a frame may wait after a gap of up to 20.8 ms, and 41.7 ms
   from `-lag 57` to `-lag 74`. Below `-lag 57` no wait fits, and the relay runs without the
   deferral and says so in the log.
3. The lookahead reads a waiting frame's timestamp, which is known when the frame arrives,
   before its pixels are copied. Without this the lookahead plans its moves a wake late, which
   adds 41 blends and makes 1,893 presents pass the other neighbouring frame through.

With all three the replay shows the same content on every present of every fixture as it does
without the deferral, at every extra lag tried from 25 to 125 ms. Of the 1,934,639 copies the
first version skipped, 1,931,685 are still skipped at the default 75 ms and 1,885,287 at 57 ms.
On the fixtures captured without extra lag nothing waits. The suite fails if any present
differs.

So the switch follows `-lag` by itself, which is what it needs to become a default (user,
2026-10-08: "it should default to on, scale with lag, and if lag is too low, disable
automatically").

The shorter wait has one cost, and it is why the wait stops at 40 ms. The capture loop tells a
grab that timed out from one that brought a frame only by how long it blocked, so a frame that
arrives in the last 5 ms of the wait is taken for the timeout and lost. After a pause the source
resumes a whole number of its periods after the waiting frame, so the wait always ends on a half
period. In the corpus 2 wakes in 10.1 hours fall in that window with a 58.3 ms wait and 7 with
41.7 ms. With 25 ms it is 598 to 2,300, because an ordinary small hitch puts the next frame 20
to 26 ms after the last.

It is on by default in every temporal mode, and `-nodefercopy` turns it off. `-fgphase` and
`-phasekeep` turn it off too, and so does a GPU or driver that refuses two NvFBC buffers: the
relay then sets up again with one buffer and copies every frame as it arrives.

It was benched on 2026-10-08 as a switch, `-defercopy`, on the experiment branch's build. In a
one-launch sitting (three reference rows and two `-defercopy` rows alternating, two runs each,
a no-relay row at both ends) it gained 43.5 and 44.25 points over the reference rows on either
side, which takes `b:vsync` from 6.2% to about 5.4%. Inside the benchmark's test segments the
`-defercopy` rows showed no repeats, no holds and no present gap over 25 ms, like the reference
rows. They stored 4% more wakes and blended less (about 7% of presents where the reference rows
blended 8.5 to 9.0%); why the blends fall is not established. 36% of stored frames were never
copied and 5% were copied a wake late.

Before it ships it still needs a capped sitting and one stream with `-defercopy` alone: the
benchmark's frame generation is DLSS, whose extra wake is a duplicate, while Smooth Motion's
generated frames do reach NvFBC.

The capped sitting ran on 2026-10-08 (`cap3`): the benchmark capped at 60 with frame generation
2X, one launch, the zig build of `f046ae2`, three rows with `-nodefercopy` and three with the
default in turn, one run a row. Every run scored 3400 at 120 fps. Inside the benchmark's test
segments (84 s and about 5,000 presents a row) no row had a repeat, a hold or a present gap over
25 ms (the longest was 19 ms), and every row presented 60.01 times a second. The rows with the
deferral blended 39, 64 and 37 times and the rows without it 31, 45 and 78. With the deferral
40.6 to 40.8% of stored wakes waited: 38.9 to 39.3% were never copied, 1.3 to 1.9% were copied a
wake late and none after a grab timeout. Over each whole log 1 or 2 refreshes showed no new
frame, with the deferral and without it.

The stream ran the same night: 136 minutes of Get Medieval with Smooth Motion x2 on the CI build
of `b043b35`, the deferral on by default. That build also has the full-size grab and the 1x1 back
buffers, so it was not the deferral alone. Of 971,591 stored wakes, 492,591 waited (50.7%):
476,493 were never copied, 16,098 were copied a wake late, and none were copied after a grab
timeout. Smooth Motion skips more copies than the benchmark's DLSS did (49% never copied, where
the benchmark had 36%) and copies fewer late (1.7%, where it had 5%). Replayed from the capture
(`gm_60x2_gameplay_2026_10_08_0.trace`, cut to the 136.4 minutes its video shows), the deferral
waits on 489,413, never copies 473,413 and copies 16,000 late; the log's totals also cover the
68 seconds outside the cut, where 7,107 more wakes were stored. Nothing downstream got worse.
Compared with the four streams before it (2026-09-30 to 2026-10-07, all without the deferral),
it had the lowest blend share (0.32%; the others 0.39 to 0.77%), the fewest presents blocked over
25 ms per source gap over 25 ms (0.22; 0.26 to 0.32), the fewest refreshes with no new frame
(0.009 a second; 0.015 to 0.031) and the fewest late dejitter batches (0.35%; 0.41 to 0.45%).
Holds ran 192 an hour, inside the earlier streams' 168 to 494. The game's content moves all of
these, so the stream says only that nothing regressed.

The replay cannot show the capture loop reaching its next grab sooner, because every fixture's
wake times were recorded by a loop that copied and waited on every wake. Replayed from their own
logs, two benchmark captures made with `-defercopy` give the same number of waiting frames as
the relay counted (9,732 and 7,188) and split them the same way to within 7 and 9 frames.

### 3.2 The copies still waited for

The rest of the copy side needs the remaining copies not waited for at all. The wait is there for two reasons that stay true. The present side reads ring slots from
another device, and D3D9 shared surfaces have no lock or fence, so a slot may be published only
once its copy has run on the GPU. And NvFBC writes the next frame into the same capture buffer.
The design keeps both: hand the copy to the GPU with one flush, go on, turn the slot on when a
later wake finds the event query done, and block only when that buffer is about to be grabbed
into again. With two buffers a copy has one source period to finish.

A politer wait (one flush, then reads of the query without the flush flag) saved nothing, so
waiting at all is the cost. Not waiting saved 47 to 62 points.

In the replay, with every copy's slot turned on a wake late, which is the worst case, and the
three rules of 3.1, no present past the replay's cold start shows different content on any
fixture, with 2,055,731 copies not waited for. Without the gap rule the same model adds 1,885
repeats at the 75 ms lag, and without the lookahead reading timestamps 12,922 presents differ.

Built on the experiment branch as `-nowaitcopy` and benched on 2026-10-08 in one launch
(reference, both switches, `-defercopy`, both switches, reference, a no-relay row at both ends,
two runs a row). It adds about 11 points to `-defercopy`: the reference cost 6.07%, `-defercopy`
5.12% and both switches 4.91%. Pacing inside the benchmark's test segments matched the other
rows (no repeats, no holds, no present gap over 25 ms).

That is less than first expected, for two reasons. Priced properly, not waiting was only ever
worth 13 to 25 points on top of the skipped copies, and `-defercopy` alone now gets 44 to 52. And
on a saturated GPU a copy takes 10 ms at p95 of a 12 ms source period, so about a tenth of the
copies are still waited for before their buffer is grabbed into again (the longest 11.6 ms), and
nearly every slot is turned on one wake late. A third NvFBC buffer would give a copy more time.
It was thought unavailable when this was built; a probe on 2026-10-09 showed NvFBC accepts three
(section 6), and `-nowaitcopy` has not been tried with three. The copy side as built gives 60 to 65 of its 80 points.

### 3.3 Skip the extra wake (`-grabphase`)

Not built. A stand-in was measured on 2026-10-07: pausing 8 ms before each grab removed 43% of
the wakes and saved 75 points, about what `-defercopy` was priced at for skipping the copies of
those same wakes (64). So the grab of a duplicate costs little, and skipping the wake adds
little to skipping its copy. It would save more when the game outruns the card and whole frames
go unshown. The idea as first written:

The only candidate that also cuts NvFBC's own grabs. The capture loop already sees NvFBC return
the newest frame in a single wake when the thread was busy through the generated flip (the
coalesced single, `CaptureRing.cpp:488-496`: singles are 7 times as common in that class, 18%
against 2.5%). Doing that on purpose, sleeping after each real member until just before the next
real flip and then grabbing, would cut grabs, copies and flushes from about 151 to about 90 a
second.

The unknown that decides it: when a flip happened while the thread was not inside the grab call,
does the next grab return that frame at once, or wait for the following flip? First step is an
instrument: log each grab's blocking time and check for returns under about 200 us after a known
busy stretch.

Risks: the arrival stamp becomes the time the relay asked, which the flip-time correction would
have to cover; oversleeping into the next generated flip loses a real frame; an uncapped base rate
that wanders makes the wake time imprecise. Capped 60x2 gameplay, with its steady 16.7 ms between
real frames, is the easier case.

### 3.4 Dropped

**A plain copy in place of the shader draw, on an 8-bit ring.** The capture card reports 8 bits
per colour and the swapchain is B8G8R8A8, so with NvFBC capturing 8-bit ARGB the ring matches the
swapchain, and a present that shows one frame unchanged can be a `CopyResource` from the slot to
the back buffer. Built as a switch and measured: 13,117 of 14,403 presents were plain copies and
the row gained 4 to 22 points. Added to the copy-side pair it gave less (63 to 65) than the pair
alone (77 to 89). Presenting with no draw at all saved only 9 to 31, so the present itself is
most of the 75 points and no replacement for the draw can save much. The 8-bit capture and ring
alone gained 4.

**A grab without scaling.** The grab asks NvFBC to scale to 2560x1440 (`SOURCEMODE_SCALE`) even
when the game display already is 2560x1440. Full-size mode gained 8 to 22 points and crop mode
-14 to +11 on 2026-10-07, both inside those rows' uncertainty. Measured again on 2026-10-08 in
settled rows, alternating the same build with and without it, full-size mode gained about 4
points (7.8 and 0.4), which is still inside the noise. It costs nothing and its pacing
matches, so it was committed on 2026-10-08 as the more correct request; it falls back to the
scaled grab if NvFBC refuses it. The first attempt at full-size mode left the scaled mode's
target size set, every grab failed with result -2, and the row scored 255 points
better while capturing nothing. A row's log has to show frames stored.

**Low GPU priority for capture.** `IDirect3DDevice9Ex::SetGPUThreadPriority` on the capture
device. At -7 in the GPU-bound benchmark the copy waited 10 to 276 ms and capture fell to 18
frames a second, which is where its 192 points came from. At -2 nothing changed. Whether a mild
level helps a capped game, where capture has the whole lag as slack, is untested. Raising the
present device's priority is still the roadmap's idea for the present that waits 33 or 50 ms at
a game's resume, which is a quality question.

**1x1 back buffers on the idle D3D9 devices.** 5 points on 2026-10-07. Committed on 2026-10-08
all the same, since nothing draws to those back buffers and they hold about 29 MB of video
memory.

**NvFBC's CUDA interface.** The CUDA interface takes its output buffer on every grab, so a grab
could write straight into a ring slot with no ring copy (9.1). It was built for real on
2026-10-08 behind a switch and never committed: the ring slots were Direct3D 11 buffers
registered with CUDA, each grab mapped the next slot and NvFBC wrote the frame into it, and the
present shader read the slot by address. On the desktop it worked and the picture matched the
D3D9 path's. Under game load the driver reset a GPU engine twice (`nvlddmkm` event 153), each
time within a minute of the relay starting. With the hand-off to Direct3D 11 taken out (the grab
writing into one buffer of CUDA's own, nothing copied) it ran without errors, and that form was
priced on 2026-10-09 in one launch beside the D3D9 grab also copying nothing. The D3D9 grab cost
about 260 points (263 and 258) and the CUDA grab about 483 (485 and 482 in 10-bit, 484 in 8-bit).
So the CUDA grab is about 223 points dearer before any hand-off, and the whole ring copy and its
wait are worth about 80, which no way of sharing the buffer can make up. In those rows the game's
own passes took the same time (9.9 to 10.0 ms) while its frame time rose from 11.0 to 11.5 ms;
why is not established. A CUDA session writes nothing, while every grab reports success, when it
is created on one thread and grabbed on another. The 10-bit CUDA session delivers one frame and
then nothing for about 4 seconds after it starts.

## 4. The original relay

The original relay cost 2.79% by hand on 2026-10-03 where `60` cost 4.95%. `60` is the original
loop with the high-resolution absolute timer the user added in `fb0e6e3` (2026-06-08), plus
`D3DCREATE_MULTITHREADED`, a window that is really topmost, and PresentEx flags 0. Four switches
that each put one of those back the way the original had it were benched in one launch on
2026-10-07, three runs a row with runs 2 and 3 counted:

| switches on, after `60` | cost | loops a second |
|---|---|---|
| all four | 3.05% | 32.31 |
| all but the timer | 4.68% | 59.99 |
| all but the device flag | 2.96% | 32.33 |
| all but the window | 2.86% | 32.33 |
| all but the present flags | 3.07% | 32.34 |
| none (plain `60`) | 4.69% | 59.99 |

The timer accounts for all of it. Each relative 16.7 ms wait on a default-resolution timer ends
on the second 15.6 ms tick, so the original loops 32.3 times a second and repeats a frame about
every other refresh. The other three switches moved the score by -5, -10 and +1 points, inside
the drift of that sitting. Nothing in the original is worth copying, and the switches were
removed again; the `timer:` line that logs the loop rate stays. The original exe itself read
about 24 points better than the all-four row in hand rows on separate launches, which may be
nothing.

Cost does not simply follow presents a second: `40` cost 0.86 of what `60` cost on 2026-10-06,
where the rate alone predicts 0.67.

## 5. CPU-only cleanups

Not expected to move the score on a GPU-bound game. They may matter on a game that is limited
by the CPU.

- Cache the back buffer's render target view. It is recreated every present
  (`D3D11Present.cpp:394-403`) because the comment at `:390` says flip model changes the back
  buffers' identities each present. That is D3D12's model; under D3D11, buffer 0 should always
  name the current back buffer (from the Fable review's reading of the DXGI docs, not checked
  here). Check on the rig that a cached view still draws to the screen.
- Yield in the flush wait. The spin at `CaptureRing.cpp:340-342` holds a core for the whole wait
  (p95 about 10 ms in GPU-bound rows). A wait that flushes once and then only reads the query
  was measured on 2026-10-07 and did not move the score, as expected on a GPU-bound game. Section
  3.2 would remove the wait.
- DONE 2026-10-08: the idle D3D9 back buffers at 1x1 in `b:vsync` (the present device's and
  the capture device's own, about 29 MB of video memory). Measured at 5 points, so it is only a
  memory saving.
- Drop `D3DCREATE_MULTITHREADED` where one thread uses the device.

## 6. Ruled out

- The candidates section 3.4 lists as dropped, which were measured.
- NvFBC writing straight into ring slots: NvFBC registers at most 3 output buffers, and a ring
  needs about 34. Probed on driver 610.88 on 2026-10-09: 2 and 3 are accepted and grabbed into,
  4 and 5 are refused (result -5), 8 and 34 crash inside `NvFBC64_.dll` (`0xc0000409`), as 8 did
  in July (`8eb5e70`).
- Replacing the flush with a fence or keyed mutex: D3D9Ex has neither for these shared surfaces,
  so the event drain stays the only guarantee that a published slot is complete.
- Doing the capture copy on the D3D11 side: the copy still has to happen before the next grab
  overwrites the staging surface, and NvFBC's write still needs the D3D9 query.
- Skipping the draw on holds: FLIP_DISCARD rotates buffers, and holds are rare.

## 7. Open question: what the game's GPU % shows

The game's GPU % falls from about 96 with no relay to 87 to 91 with one, and reads about 63 when
the game is capped at 60 with 2X, with or without the relay. Its definition is unknown:
the CSV's "GPU time" equals the frame time in every row, so it is not the game's busy time.
Neither "the game's own share" nor "the whole GPU's busy time" fits every row. PresentMon's
per-frame GPU busy for the game, plus `nvidia-smi --query-gpu=timestamp,utilization.gpu
--format=csv -lms 100`, during one `b:vsync` run and one no-relay run, would show whether the
relay adds work to a busy GPU or leaves it idle between the game's frames.

## 8. Measuring where the GPU time goes

The copy and the present were priced by leaving them out (section 2). NvFBC's grab was not: its
175 points are what is left after the other two, and worked back from PresentMon that is about
0.4 ms of GPU time per captured frame, about one full-frame copy on a saturated GPU. PresentMon's
GPU busy figure for the relay is no measure of it. It overlaps the game's own work and read 0.6
to 5.6 ms between identical runs.

A GPU timing trace of one benchmark run would show which engine the grab runs on, how long each
packet takes and whether it preempts the game. That decides whether anything about the grab can
be moved, and which option in section 9 is worth building.

- On the rig: `wpr -start GPU -filemode`, run the benchmark with `b:vsync -src 60`, `wpr -stop
  relay.etl`, then open it in GPUView or Windows Performance Analyzer. A second trace with no relay
  gives the game's own picture.
- Or extend EtwProbe, which already reads the graphics kernel's provider, to log per-process GPU
  work so the analysis can run on the Mac. Which events carry per-packet GPU timing has not been
  checked.

## 9. Architectural options

Bigger changes than section 3, each removing work the current design cannot avoid. Only NvFBC's
CUDA interface has been built, and it was dropped.

### 9.1 Keeping frames without copying them

**NvFBC's CUDA interface.** The D3D9 interface registers its output buffers once, at SetUp, which
is where direct write crashed. The CUDA interface takes its output buffer on every grab call:
`NVFBC_CUDA_GRAB_FRAME_PARAMS::pCUDADeviceBuffer` (`v0.0.16:inc/NvFBC/nvFBCCuda.h:74`; formats ARGB
and ARGB10 only, `:59-60`). Each grab could write straight into its ring slot, removing the ring
copy and its flush. CUDA hands out linear device memory, so the D3D11 side would read the slots as
buffers, not textures (point sampling at 1:1 needs no filtering). CUDA comes from the driver's
`nvcuda.dll` through the driver API, so nothing new ships. A probe on 2026-10-08 (`CudaProbe`,
on the experiment branch) showed the interface works on driver 610.88: a session creates and
sets up with a CUDA context current, and grabs deliver the 2560x1440 picture into device memory
the caller allocated. A context synchronize after each grab took about 0.2 ms on an idle GPU,
which suggests the write is still in flight when the grab returns. Measured on 2026-10-09 and
dropped: with nothing copied, the CUDA grab costs the game about 223 points more than the D3D9
grab does (3.4).

**Windows.Graphics.Capture (WGC).** A different API from DXGI Desktop Duplication (the one usually
called "DXGI capture"). Desktop Duplication hands over one frame at a time: asking for the next one
without releasing the previous fails with `DXGI_ERROR_INVALID_CALL` (Microsoft's
`IDXGIOutputDuplication::AcquireNextFrame` page), so keeping frames needs a copy, as today. WGC
delivers frames into a frame pool whose size the app picks (`Direct3D11CaptureFramePool.Create`,
`numberOfBuffers`; no maximum documented), so the pool itself could be the ring with no copy. Each
frame carries `SystemRelativeTime`, "the QPC time at which the compositor rendered the frame": a
frame time from the system, where NvFBC gives only arrival time. It is D3D11 like the present path,
needs nothing from NVIDIA, and matches the roadmap's plan for the app to capture through Windows by
default. An earlier experiment exists: the remote branch `dxgi-native-pipeline-probe` (tip
`de0b1f4`, 2026-07-23, "add dxgi probe", 9 commits past `dev`'s old tip); read it before any WGC
or Desktop Duplication prototype. Unknown: whether WGC pulls a fullscreen game out of independent
flip (that timestamp is the compositor's, which suggests composition) and what that costs the
game; whether it sees Smooth Motion's generated frames; how long a pool frame stays valid while
held; and the cost of holding about 30.

**What each API can capture.** Desktop Duplication captures a whole display. WGC captures a
whole display or one window, and gets a window's picture from the compositor; OBS's window
capture can use it. OBS's game capture, the 3.76% row, uses neither. It loads a hook into the
game and shares the game's back buffer when the game presents.

**What each method costs inside OBS.** A sitting on 2026-10-09 (`wg1`) ran OBS's projector with
each capture method in one launch, two runs a row, with the relay beside them and a no-relay row
at both ends. Every OBS row shares OBS's own drawing, so the differences between them belong to
the capture methods. OBS's log of each row names the method that ran, and a screenshot of both
displays during each row's first run shows the game on the card display in every row.

| row | cost |
|---|---|
| OBS, game capture (the hook) | 216 points, 4.0% |
| OBS, display capture by Desktop Duplication | 365 points, 6.8% |
| OBS, display capture by WGC | 295 points, 5.5% |
| OBS, window capture by WGC | 291 points, 5.4% |
| `b:vsync -src 60`, the CI build of `b043b35` | 287 points, 5.3% |

WGC costs about 70 points less than Desktop Duplication. Capturing the game's window costs the
same as capturing the whole display. OBS with WGC costs what the relay costs with NvFBC, and the
hook is about 75 points cheaper than either. So neither Windows API, as OBS uses it, reaches game
capture's cost. A relay on WGC differs from OBS's WGC rows in one way that matters: OBS copies
every frame out of WGC's pool into a texture of its own (`libobs-winrt/winrt-capture.cpp`,
`on_frame_arrived`), and a relay that keeps frames in the pool does not.

**WGC in the relay (2026-10-09, on a local experiment branch, not in this tree).** Built as
`-wgc`: the D3D11 present hands its device to the capture ring, the capture thread opens a WGC
frame pool on that device with a buffer for every ring slot, each stored frame stays in the pool
for as long as its slot names it, and the present samples the pool's texture with the shader it
already has. No frame is copied and nothing is waited for. On the desktop the card display
matches the game display exactly. Two things had to be learned first. As Windows has it, WGC
delivers a 185 fps game at about 53 frames a second: its least time between frames
(`MinUpdateInterval`, Windows 11 24H2 and later) is 16 ms, and the relay sets it to 1 ms. And a
row that only priced the capture, with a wrong picture, read 59 points cheaper than the real
path, so only the real one is quoted here. One launch, two runs a row, a no-relay row at both
ends, rates inside the benchmark:

| row | cost | frames stored a second | repeats | holds | blends |
|---|---|---|---|---|---|
| `b:vsync -src 60` (NvFBC, the deferred copy) | 288 points, 5.3% | 160.6, in 90.4 batches | 0 | 0 | 7.7% |
| `b:vsync -src 60 -wgc`, 1 ms | 241 points, 4.5% | 93.1, in 92.0 batches | 0 | 0 | 6.6% |
| `b:vsync -src 60 -wgc`, Windows' 16 ms | 189 points, 3.5% | 52.8 | 0 | 0 | 53.6% |

At 1 ms WGC stores one frame for each of the game's real frames and paces like NvFBC: 60.01
presents a second, no present gap over 25 ms, and the same spread of shown-content steps. It is
47 points cheaper. At Windows' own 16 ms it is cheaper than OBS's game capture, and more than
half of its presents are blends, which is a worse picture. WGC does not deliver DLSS frame
generation's second wake; NvFBC's second wake there is a duplicate.

Under Smooth Motion (a 60 fps game at x2, about 20 seconds a run, standing and walking) WGC
delivers all 120 frames a second, as NvFBC does, and they still come as a pair every 16.7 ms.
The two members arrive 1 to 4 ms apart, where NvFBC's two wakes come 0.4 ms apart, so the
relay's 3 ms pair window caught only 29% of them. With a 6 ms window under `-wgc` every wake is
in a pair (60.1 batches a second), and the shown content steps exactly one 60 Hz period on
every present, with no repeat and no blend, as it does with NvFBC in the same spot. The second
member of a WGC pair is the real frame, as it is with NvFBC: reading back the centre of every
frame for 27 seconds of standing, walking and strafing, the 3,103 frames were 3,101 different
pictures, and in 1,484 of 1,490 pairs the first frame had the softer edges (77 to 92% of the
second's edge energy while moving, 99.3 to 99.5% while standing), which is what an interpolated
frame looks like. The relay keeps the second. ETW flip pairing works on WGC's arrival times
(1,414 batches paired in one run, none late). WGC's own per-frame timestamp is the compositor's
and is no use as a clock: under Smooth Motion it ticked with the capture card display's refresh.
Still untested: a whole stream, the pair window at other rates and multipliers, and HDR sources
(the pool is 8-bit).

**Presenting a ring slot by reference (composition swapchain).** Windows 11's composition
swapchain registers up to 31 textures with a presentation manager and presents any of them, in any
order, with no copy; each present can carry a target time on the QPC clock (Microsoft's composition
swapchain programming guide). Passthrough and hold presents would need no draw, and blends would
draw into a spare registered buffer. Target-time presents match how the relay already chooses
frames. Requirements: Windows 11 build 22000.194 or later with WDDM 2.0; independent flip and
direct scanout need WDDM 3.0 and textures created as displayable, which the D3D9-made ring slots
are not. So this pairs with the WGC option, whose frames are created on the D3D11 side.

### 9.2 Doing less work

**Copy only frames that will be shown.** The relay knows its present schedule about 100 ms ahead:
each present's target is the vblank minus the lag and the pull, and the pull moves slowly. At
capture time it can tell which frames can never be a passthrough or a blend partner and skip their
copies. `-defercopy` drops only frame generation's extra wakes; this also saves when the game
outruns the card (about a third of the copies at 90 fps into 60, half at 120). Risk: a pull move,
wrap or stall changes which frames are needed after they were skipped, so the margin has to cover
the lock's largest move.

**Grab only at real flips.** The comb lock predicts when each real frame flips. A no-wait grab just
after that time catches each real frame once and never wakes for frame generation's extra flip,
and ETW's flip times stamp it with its real flip time. The ring stays: each grab is stored with its
stamp as today, and the present side still selects and blends from it with the same lag. What
changes is only when the capture thread asks. The risk is a wrong prediction: after a hitch or a
rate change a grab can come early (the previous frame again), late (the generated frame, or the
next real one), or miss a frame, leaving a gap the bracket covers with a wider blend. Steady capped
content suits it; an uncapped, wandering rate does not. This is the cleaner form of `-grabphase`.

**Low GPU priority for the capture device.** Measured and dropped for a GPU-bound game (section
3.4): a saturated GPU leaves no gaps for a low-priority copy to wait for, so capture starves.

**NV12 ring.** The D3D9 interface can output NV12 (`NVFBC_TODX9VID_NV12`,
`v0.0.16:inc/NvFBC/nvFBCToDx9Vid.h:80`): 1.5 bytes a pixel where the ring holds 4, about 60% less
to copy and store. The stream is encoded at 4:2:0 anyway, but converting to 4:2:0 twice (here and
in the encoder) can soften coloured edges, so it needs a quality check on text and HUD.

### 9.3 Moving work off the game's GPU

**Present from a second GPU.** With the capture card on the motherboard's output (the CPU's
integrated graphics), the ring, the blends and the presents all run there. The game's GPU keeps
only NvFBC's grab and one copy per frame across PCIe, which its copy engine can run beside the
game. The largest possible cut, and the most hardware-dependent: it needs integrated graphics and a
motherboard output that drives 2560x1440 at 60 Hz, and a cross-adapter copy path.

## 10. Order

The user's order (2026-10-04): release first, then this pass on its own branch. Why performance
comes before NVOFA (user, 2026-10-04): "I'm worried nvofa interp frames are going to be expensive
to generate, so we need more performance headroom before we even enable that." The deferred copy
alone does not make a release (user, 2026-10-09: "deferred copy is good, but it's not worthy of a
full release I don't think").

NVOFA's cost has never been measured. The interp compositor runs flow and warp only on presents
that synthesize (`SynthCompositorBase::RenderSynthesis`, `src/relay/FrameCompositors.h:58`), so its
cost follows the synthesis rate: on capped 60x2 gameplay about 1,050 to 1,200 blends an hour (about
0.3 a second; `docs/lock-transitions-spec.md` section 8, five streams), far more in the uncapped
benchmark. The worry there is a spike on one present more than the average. The hidden `o` mode
(`o`, `o:vsync`: interp on the D3D9 swapchain on DWM's clock, a D3D11 sidecar for flow and warp)
can be benched now against `b:dwm` (the same swapchain with the lerp), which isolates the
interpolation's cost. It has not been validated on Windows on this branch, so a first run may find
problems before it finds a number.

0. ON HOLD (user, 2026-10-04): sizing NVOFA with `o` against `b:dwm`. "I think we'll want to hold
   on anything with o mode for now. We'll need to rebuild it in the future I think. comparing cuda
   vs dx11 mechanisms." Measure NVOFA's cost on the rebuilt mode.
1. DONE 2026-10-07: the calibration (section 4), the split of the cost (section 2), and a row
   each for capture priority, the 8-bit ring, the plain-copy present and the grab without
   scaling (section 3.4).
2. DONE 2026-10-08: `-defercopy` with its three rules (section 3.1): a sitting on the rig for
   cost and pacing, a capped sitting, and one stream (with the full-size grab and the 1x1 back
   buffers too). It is on by default.
3. The copies still waited for (section 3.2): build it, then a sitting with a reference row,
   `-defercopy` and the new switch in one launch.
4. One profiled look at NvFBC's grab (section 8), then decide whether anything about it can be
   moved.
5. DONE 2026-10-08: OBS's projector as two rows in a one-launch sitting (section 1), and on
   2026-10-09 as four rows, one for each capture method (9.1). Its pacing and picture with each
   capture method are still to be measured the way the relay's are.
6. One architectural prototype: WGC with the pool as the ring (also the app's default capture
   path), or Desktop Duplication, with the composition swapchain as a later step. NvFBC's CUDA
   interface was the other choice; it was built, measured and dropped on 2026-10-09 (3.4).
7. The CPU-only cleanups, together, checked by the suite and one stream.

Every row is compared inside one launch with a no-relay row and `b:vsync -src 60` at both ends.

Sources for section 9: Microsoft Learn,
[Direct3D11CaptureFramePool.Create](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.create),
[Direct3D11CaptureFrame.SystemRelativeTime](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframe.systemrelativetime),
[IDXGIOutputDuplication::AcquireNextFrame](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-acquirenextframe),
[Composition swapchain programming guide](https://learn.microsoft.com/en-us/windows/win32/comp_swapchain/comp-swapchain).
