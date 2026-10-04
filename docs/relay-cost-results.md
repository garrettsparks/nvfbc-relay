# Relay cost: results

Answers `docs/relay-cost-spec.md`. Measured 2026-09-18 on the release build (CI run 35306514411,
commit `f953554`, the reverted policy). Files: `frame-drop-analysis/cost_r<row>_<run>.csv` and
`cost_r<row>.log`.

**Headline.** The release configuration (`b:vsync -src 60 -lock -lag 75 -etw -dejit`) costs the game
about 5.5% of its benchmark score, and is the cheapest relay mode measured. `-lock -lag 75` and
`-etw -dejit` each cost nothing measurable, so by the spec's own rule they qualify to be on by
default. The old D3D9 path (`b:dwm`) costs 9.4% and the original `vsync` mode 8.3%. 1440p output
costs no more than 1080p. Logging is treated as having no appreciable cost (section 5).

**What "nothing measurable" means here.** Three runs per row resolve a difference of about 32
points, 0.6% of the baseline, at 95% confidence (pooled run-to-run standard deviation 14.3 points
over rows 3 to 5). The two flag steps moved the mean by 4 points each, about a third of one standard
error. So any real cost they carry is under 0.6%, and the measured best estimate is under 0.1%.

## 1. Method

- **Content.** Avatar: Frontiers of Pandora benchmark, 2560x1440, DLSS frame generation x2,
  uncapped. Row 0 confirms the condition the ladder needs: displayed fps 166 to 228 and the game's
  own GPU utilization at 96%, so it is GPU-bound and any relay cost lands on the score.
- **Unit.** One row is one relay launch and three benchmark runs back to back, giving three CSVs
  (each a whole run of the benchmark's three tests, one `Score` line) and one log. Rows 0 and 6
  have no log by design.
- **Cost measure.** The benchmark score. The CSV's Fps column reports the displayed rate under frame
  generation and is not used. GPU % is the per-run check that the game stayed GPU-bound.
- **Decision rule, from the spec.** A row costs something when all three of its scores sit outside
  the spread of the row it is compared with.

**Two deviations from the spec, and what they cost.**

1. **Rows were run in order in two sessions, not interleaved across three rounds.** Session 1 ran
   rows 0 to 5 between 17:07 and 17:58; session 2 ran rows 6 to 9 between 21:20 and 21:50.
   Comparisons within a session are sound. Comparisons across sessions are not, because nothing in
   session 2 re-measured the no-relay baseline, so a shift in the machine's state between sessions
   is indistinguishable from a relay effect. Section 5 says which conclusions this touches.
2. **The first benchmark run of a row is usually its lowest.** Nine of ten rows show it, by about
   18 points on average. It is systematic, so it cancels in row means, but it widens the spreads the
   decision rule reads and is the same size as the smallest effects in question here. A discarded
   warm-up run before each session would remove it.

## 2. Session 1: the ladder

| row | mode | flags | scores | mean | spread | GPU % | cost vs row 0 |
|---|---|---|---|---|---|---|---|
| 0 | relay not running | | 5364, 5367, 5368 | 5366 | 4 | 96.0 | |
| 1 | `vsync` | | 4902, 4925, 4935 | 4921 | 33 | 88.9 | 8.3% |
| 2 | `b:dwm` | `-src 60` | 4861, 4870, 4860 | 4864 | 10 | 88.1 | 9.4% |
| 3 | `b:vsync` | `-src 60` | 5059, 5088, 5080 | 5076 | 29 | 90.5 | 5.4% |
| 4 | `b:vsync` | `-src 60 -lock -lag 75` | 5056, 5071, 5088 | 5072 | 32 | 90.5 | 5.5% |
| 5 | `b:vsync` | `-src 60 -lock -lag 75 -etw -dejit` | 5054, 5074, 5074 | 5067 | 20 | 90.5 | 5.6% |

Relay-side numbers from the logs. Row 1's log is header-only because `vsync` mode writes nothing per
frame.

| row | presents/s | captures/s | capture flush p50 | flush p95 | present interval p50 | log lines/s |
|---|---|---|---|---|---|---|
| 2 | 136.8 | 148.1 | 219 us | 10.7 ms | 6.2 ms | 286 |
| 3 | 60.0 | 151.3 | 138 us | 10.1 ms | 16.7 ms | 213 |
| 4 | 60.0 | 151.9 | 136 us | 10.1 ms | 16.7 ms | 212 |
| 5 | 60.0 | 151.0 | 136 us | 10.1 ms | 16.7 ms | 435 |

### Verdict per rung

| step | measured | verdict |
|---|---|---|
| 0 to 1 | 5366 to 4921, every row 1 score below row 0's range | costs 8.3% |
| 1 to 2 | 4921 to 4864, every row 2 score below row 1's range | costs a further 1.2% |
| 2 to 3 | 4864 to 5076, every row 3 score above row 2's range | row 3 is 4.4% **cheaper** |
| 3 to 4 | 5076 to 5072, two of three inside row 3's range | nothing measurable |
| 4 to 5 | 5072 to 5067, two of three inside row 4's range | nothing measurable |

**Why `b:dwm` costs more than `b:vsync`.** Both present on the capture card's adapter and capture at
the same rate (148 and 151 per second). The difference is the present: the D3D9 windowed present is
paced by DWM's compose clock, which under an uncapped game on the 240 Hz source monitor runs fast,
so `b:dwm` presented 136.8 times a second against exactly 60 for `b:vsync`. The 60 Hz card shows 60
of those, so about 77 blend-and-present passes a second are discarded work. Each D3D9 present is also
dearer: DWM copies it into its composition surface, while 99.6% of `b:vsync` presents reported as a
hardware overlay with no copy. Capture flush rose with the load, 219 against 138 us at the median.

**Why the original `vsync` mode costs more than `b:vsync`.** Its device sits on the 240 Hz source
adapter and presents through DWM the same way `b:dwm` does, and its own log flags a format
conversion on every present. It logs nothing per frame, so its present rate is not measured, but its
pacing is the same mechanism that made row 2 expensive. The full blend relay on `b:vsync` does more
work per frame and still costs less, because it presents only as often as the card can show.

**Logging inside session 1.** Row 5 writes 435 log lines a second against row 4's 212, because
`-etw` logs one line per source flip, and still costs nothing measurable. So the per-flip line has no
detectable performance cost. Its case for being gated is disk volume (about 157 MB an hour on a
long capture), not speed.

## 3. Session 2: logging, 1440p, and the present-cost line

| row | mode | flags | output | log | scores | mean | spread | GPU % |
|---|---|---|---|---|---|---|---|---|
| 6 | `b:vsync` | `-src 60 -lock -lag 75 -etw -dejit` | 1080p | absent | 5088, 5098, 5103 | 5096 | 15 | 90.3 |
| 7 | `b:vsync` | `-src 60 -lock -lag 75 -etw -dejit` | 1440p | present | 5084, 5108, 5108 | 5100 | 24 | 90.2 |
| 8 | `b:60` | `-src 60` | 1080p | present | 5002, 5016, 5021 | 5013 | 19 | 87.8 |
| 9 | `b:20` | `-src 60` | 1080p | present | 5109, 5131, 5133 | 5124 | 24 | 90.8 |

| row | presents/s | captures/s | capture flush p50 | flush p95 | present interval p50 | log lines/s |
|---|---|---|---|---|---|---|
| 7 | 60.1 | 151.8 | 155 us | 10.0 ms | 16.68 ms | 439 |
| 8 | 60.0 | 122.3 | 144 us | 10.0 ms | 16.67 ms | 184 |
| 9 | 20.0 | 123.8 | 141 us | 9.8 ms | 50.0 ms | 144 |

**1440p output and logging both cost nothing appreciable.** Row 7 carries both 1440p output and full
logging; row 6 carries neither; they score level (5100 against 5096, row 7 four points higher). The
two effects are not separable from this pair alone, but neither can plausibly be negative. Logging
only adds work, and 1440p should cost at least as much as 1080p: a native 1440p grab reads and writes
14.7 MB where a scaled 1080p grab reads 14.7 MB and writes 8.3 MB, and the ring copy, blend and
present all move 1.78 times the pixels. So their sum, at most about 28 points at 95% confidence, bounds
each of them: both are under about half a percent, measured within one session. The larger copies do
show in capture flush, whose median rose from 136 to 155 us.

**A D3D9 present is expensive.** Rows 8 and 9 share a present path and capture at the same rate
(122 and 124 per second) and differ only in present rate. Going from 60 to 20 presents a second
gained 111 points, every row 9 score above row 8's range: about 2.8 points per present per second, so
60 D3D9 presents a second cost about 3% of the score.

The D3D9 timer rows capture at about 122 a second where every other row captured about 150. The
reason is not known and is recorded rather than explained.

## 4. Present or capture: where the shipping path's 5.5% goes

Probably mostly capture, but the number leans on comparing across sessions.

Extrapolating row 9 to zero presents at 2.8 points each gives about 5180. Against session 1's
baseline of 5366, that leaves about 186 points, 3.5%, for capture, selection and logging at 124
captures a second, or roughly 4.2% scaled to the 151 a second the flip path runs. That leaves
roughly 1.3% of `b:vsync`'s 5.6% for its 60 overlay presents. If session 2 sat higher than session
1 for reasons unrelated to the relay, the capture share is larger still, so the ordering is robust
even though the split is not: on the shipping path, capture is the larger cost.

That points any later optimization at the capture side. The concrete lead: captures ran at about
151 a second against a real base rate of about 90 (GPU time 11.15 ms per real frame), and the
frame-generation work established that the extra wakes deliver a duplicate of the real picture. So
roughly 40% of capture work copies a frame the ring already holds. Whether NvFBC can report those
wakes without performing the copy is unknown.

Capture flush p95 sat near 10 ms in every relay row, against 187 us on capped gameplay: under a
GPU-bound game the capture thread waits behind the game's own work. That wait is CPU time on the
relay's thread and does not by itself take GPU time from the game, so it is a symptom of contention
here rather than a measured cost.

## 5. Logging: no appreciable cost

Decided 2026-09-18: logging is treated as costing nothing appreciable. The evidence, in order of
strength:

1. **Same session, clean.** Rows 4 and 5 differ in log volume, 212 against 435 lines a second, and
   the 223 extra lines cost 4 points, 0.4 standard errors, under the 0.6% resolution. The logger's
   cost is per line (a format and a ring copy on the calling thread, batched disk writes every 10 ms
   on its own), so the first 212 lines cannot plausibly cost much more than the next 223.
2. **Same session, clean, given one premise.** Row 7 (1440p with a log) against row 6 (1080p
   without) scores level, four points in row 7's favour. Since 1440p costs at least as much as 1080p
   (section 3), logging's cost is bounded by that same comparison: under about half a percent. This
   is independent of the first line of evidence and comes from the other session.
3. **Cross-session, explained.** Row 6 (no log) beat row 5 by 29 points, about 2.5 standard errors,
   which read as logging costing 0.5%. Given the two lines above, it is session drift: row 7 in
   session 2 also beat row 5 in session 1, by 33 points, while running at least as much work. Session
   2 simply ran about 30 points higher, which is also why no row in it can be compared with session 1.

The bridge that would pin the exact number, if it is ever wanted: one session with a discarded
warm-up run, then rows 0, 5 and 6. It is not needed for any current decision.

Logging is already off for anyone who does not create `NvFBCR.log` beside the exe, so the release
needs no change to turn it off.

## 6. The spec's four predictions

| prediction | result |
|---|---|
| 1. Row 4 to 5 costs nothing measurable | **Survived**, including the doubled log volume |
| 2. Row 3 to 4 costs nothing measurable unless VRAM is tight | **Survived**; no eviction symptom in the scores |
| 3. Row 3 is no slower than row 2 | **Survived** by a wide margin: row 3 is 4.4% faster |
| 4. Row 1 to 2 is where the cost is, more flush than copy | **Failed.** The largest step is 0 to 1, 8.3%, and 1 to 2 adds only 1.2%. Row 1 was also not the clean capture-only rung the spec assumed: it presents through DWM on the source adapter at an unlogged rate. Session 2's timer rows are what locate the cost, and they point at capture |

## 7. For the release defaults

By the spec's decision rule, rows 4 and 5 qualify `-lock`, `-lag 75`, `-etw` and `-dejit` to be on
by default: none of them has a measurable cost. The decision itself is the user's (Part 3).

## 8. The 1440p capture: what was and was not 1440p

- **The relay output was genuinely 2560x1440.** Three log lines agree: `Buffer size: 2560x1440`, the
  card's display mode `2560x1440 @59Hz`, and `flip-model swapchain 2560x1440`. `Buffer size` is the
  one the old scaling bug shrank: before the process was made per-monitor DPI aware it reported the
  desktop divided by Windows' scale factor, 1707x960 at 150%. The check for any future log is that
  those three lines match.
- **The Mac received 1080p.** OBS's capture source is pinned to a saved `1920x1080` format, and its
  scene item is scaled by 1.333 to fill the 2560x1440 canvas. So the card delivered 1080p over USB
  and OBS upscaled it: the recording is 1440p in size and 1080p in detail. The user confirmed this
  was an OBS misconfiguration; the relay's output is what this row measured.
- **The card runs 1440p at 59.9505 Hz, and 1080p at exactly 60.** Measured over the middle 80% of
  each row's presents: 59.9999 Hz at 1080p (mean interval 16666.7 us, the standard 1080p60 timing)
  and 59.9505 Hz at 1440p (16680.4 us). 59.95 is the standard reduced-blanking timing most 1440p
  displays call "60 Hz". It is not the broadcast 59.94 (60000/1001 = 59.9401), so setting OBS or the
  stream to 59.94 matches it no better than 60 does. Against a 60 fps game and a 60 fps stream it
  costs two hitches every 20.2 seconds: the relay must skip one game frame to fit 59.95, and the
  card or OBS must repeat one to fill 60. The fix is upstream: a custom 2560x1440 mode for the card
  whose timing lands on exactly 60.000 Hz, if the card accepts it. The relay log verifies it in one
  line, `Target display refresh: 60 Hz`, and a present interval of 16666.7 us.
- **OBS logged 686 frames lagged to rendering (3.8%) on this local recording.** The 2026-09-17 local
  map-cycle recording logged none. This belongs to the capture PC's open rendering-lag item and is
  recorded as UNEXPLAINED.
