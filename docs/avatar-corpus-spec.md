# Avatar Present-Path Corpus - Test Spec

Status: **ready to capture.** Nothing here is gated on a measurement; the present path it
pins is validated (four captures, two titles, PresentMon-named independent flip). This spec
exists so the pins are taken BEFORE the present-path refactor and the release cut, on the
build that produced the validation, and so the regimes the relay has never been measured in
are characterized on the same build.
Companion documents: `flip-present-spec.md` (the mechanism being pinned),
`stage7-generated-frame-spec.md` (the substitution regime this corpus finally exercises),
`phase-comb-lock-spec.md` (why the lock engages in some rows and refuses in others).

## Purpose

Two products from one capture campaign:

1. **Replay fixtures** for the policy gate. A `b:flip` capture replayed through the real
   policy pins what the relay decided on a sink-locked present cadence. A refactor that moves
   a pin has a bug in it. This guards the DECISION side of the refactor: the two
   `DecideComposite` call sites collapsing into one, config derivation moving, the tooth
   guard's arming, the lag sizing.
2. **Marker references** for the present path. The marker decode of each capture is the
   downstream truth a post-refactor re-capture must match. This guards the PIXEL and PACING
   side, which replay cannot see at all: replay does not draw, mark, or present.

Both are needed. The refactor's acceptance test is a fresh row A1 capture within the A1 pin,
plus the replay suite green on every fixture.

## Why Avatar

It is a true benchmark: two passes of three fixed tests, identical content every run, with
the game's own per-frame CSV as an independent source-side record. The base frame rate is
cappable and DLSS frame generation can be set to off, x2, or x3 (if the hardware offers it).
The source measured 120.00 fps minimum across every test of every run taken so far, so
source-side noise is not a variable. No other title on the rig offers any of this.

## What is already established (do not re-measure)

| fact | evidence |
|---|---|
| `b:flip` runs on the XR1's clock | PresentMon `Hardware: Independent Flip` 99.2%; 60.00 presents/s in every test; present spacing stdev 20-48 us against 575-2076 us on the old path |
| 60x2 output is clean | 0 counter repeats, 0 skips, 0 blends inside all 12 tests across two runs; same-session control measured 3.55 content repeats/s |
| KCD2 is neutral | 0.10 vs 0.30 anomalies/min, both inside the map close |
| the in-process presentation-path reading is calibrated | OVERLAY == PresentMon independent flip on this rig; transitions align to within 20 ms |
| keep-real collapses DLSS pairs | capture pairs ~250 us apart, one real frame per 16.67 ms in the ring, brackets ~16.6 ms |
| the only remaining dupe class is GPU contention | one KCD repeat: a correct blend whose draw missed its vblank behind the game's map-close load |

## Fixed configuration

Every row uses the validated flag string, with `-src` set to the REAL base rate of the row:

```
b:flip -src <base> -lock -lag 75 -mark -etw -dejit
```

Rows marked subgen add `-subgen` (which implies `-diffmap`). Relay runs as admin with a fresh
empty log beside the exe. Start the relay and let it sit on the desktop 20-30 s before the
game takes the display, so every log carries the composed-to-promoted transition. The
recording covers the benchmark only; desktop before and after stays in the log. Sink is the
XR1 at 60 Hz, 8-bit (the swapchain will select `B8G8R8A8`; the log says so on line ~145).

**Three bench runs per row** (nine tests). Pass 1 of run 1 becomes the fixture; all nine tests
feed the marker reference.

### `-src` is the BASE rate, always

`-src` names the rate of REAL frames the game renders, never the displayed rate. Frame
generation does not change it: 60x2, 60x1 and 60x3 are all `-src 60`. The matrix's
"base x FG" column names the value directly.

**Do not read it off the CSV's Fps column.** Under frame generation that column reports the
DISPLAYED rate: measured on the validated 60x2 captures it reads a flat 120.00 while the CPU
and GPU time columns read 16.66 ms, which is the real 60 fps the game renders at. The two
derivations that give the right answer:

| source | at 60x2 | derivation |
|---|---|---|
| CSV `CPU time` / `GPU time` | 16.66 ms | 1000 / 16.66 = 60.0 |
| head-0 flips/s | 120 | 120 / 2 = 60 |

What a wrong `-src` costs is NOT keep-real, which collapses epsilon pairs on a fixed 3 ms
intra-batch wake threshold and never consults the declared rate. It is everything derived
from the declared period. At `-src 120` on a 60x2 source: the stall gate sits at 16.67 ms
(twice the declared period) against real brackets of 16.5-17.3 ms, so roughly half of
presents read as stalled and the comb lock's pull freezes; the comb itself gets 8.33 ms teeth
against frames 16.67 ms apart, so half the teeth are empty; the tooth guard arms but goes
inert, wanting 7/8 of 8.33 ms of target advance where every present advances 16.67 ms; and
`-phasekeep`'s pairing gate is documented to misread a declared rate that is not true.

The remaining question is whether the CAP holds. 30 and 59 are frame-limiter settings, and
the game may deliver 59.94 where 59 was asked for. Read the achieved base rate from the
CPU-time column of the first run and correct `-src` for the remaining runs if it differs.
The comb search resolves 59.00 and 59.94 to different comb spacings (16949 us against
16683 us), so row B1 depends on this.

## The matrix

| row | base x FG | flags | file | class | expect |
|---|---|---|---|---|---|
| A1 | 60x2 | `-src 60` | `avatar_c_b_flip60x2` | pin | 0 repeats, 0 skips, 0 blends per test; `lk=1` 100%; pass 100% |
| A2 | 60x1 (FG off) | `-src 60` | `avatar_c_b_flip60x1` | pin | as A1; single-member batches, keep-real inert, no epsilon pairs |
| A3 | 60x2 | `-src 60 -subgen` | `avatar_c_b_flip60x2_subgen` | pin | substituted 0, offered ~0; marker identical to A1 within bounds |
| B1 | 59x1 | `-src 59` | `avatar_c_b_flip59x1` | discover | lock off; pass ~50%, ~30 blends/s; 0 repeats, 0 skips |
| B1a | 59x2 | `-src 59` | `avatar_c_b_flip59x2` | discover | lock off; pass ~50%, ~30 blends/s; 0 repeats, 0 skips |
| B1a | 59x2 | `-src 59 -subgen` | `avatar_c_b_flip59x2_subgen` | discover | lock off; pass ~50%, ~30 blends/s; 0 repeats, 0 skips |
| B2 | 30x1 | `-src 30` | `avatar_c_b_flip30x1` | discover | lock on (M=2); alternating pass / synth, 30 blends/s at bw ~0.5 |
| B3 | 30x2 | `-src 30` | `avatar_c_b_flip30x2` | discover | policy identical to B2; generated frames retracted, not shown |
| B4 | 30x2 | `-src 30 -subgen` | `avatar_c_b_flip30x2_subgen` | discover | `op=pass-gen` replaces most synth; substituted ~30/s; sharp 60 |
| C1 | 60x3 (if offered) | `-src 60` | `avatar_c_b_flip60x3` | discover | the x3 regime on the new clock; a second run with `-phasekeep` if C1 is unclean |
| C2 | 60x3 (if offered) | `-src 60 -subgen` | `avatar_c_b_flip60x3_subgen` | discover | substitution inert by design at x3 |
| D1 | 90x2 (if offered) | `-src 90` | `avatar_c_b_flip90x1` | discover | the x2 regime on the new clock; a second run with `-phasekeep` if C1 is unclean |
| D2 | 90x2 (if offered) | `-src 90` | `avatar_c_b_flip90x2` | discover | the x2 regime on the new clock; a second run with `-phasekeep` if C1 is unclean |
| D3 | 90x2 (if offered) | `-src 90 -subgen` | `avatar_c_b_flip90x2_subgen` | discover | substitution inert by design at x3 |

### What each row answers

**A1** is the reference everything else is measured against and the refactor's acceptance
test. Run 1 of the validation had one blend in six tests (a source flip 11 ms late at a test
start); run 2 had none. The pin allows for that.

**A2** removes frame generation entirely. Batches are single-member, keep-real has nothing to
collapse, and the ring holds exactly the source's 60. If A1 and A2 differ in anything but the
capture-line structure, keep-real is doing something at 60x2 that it should not be.

**A3** is the default-on question for `-subgen`. At 60x2 the tooth guard already removed the
mid-tooth blends substitution used to replace, and it measured 0.04-0.07/s on the old path.
On the new path there are zero synths in gameplay, so there is nothing to substitute: the
expected result is a run indistinguishable from A1. If it is, `-subgen` is safe as a default
because the one regime where it does anything (B4) is also the one where it is the whole
answer.

**B1** is the near-rate question raised at the KCD loading screen. At 59 real frames against
60 presents the target-to-frame offset sweeps the whole gap once every 59 presents. The
passthrough gate is a quarter period, which covers about half the sweep, and blend mode
blends the other half by design: roughly 30 soft frames per second. The alternative stance
is a half-period gate, which passes everything sharp and lets the slip surface as one
repeated frame per second. B1 is the evidence for that decision, not a bug report. The lock
is expected NOT to engage (the phase drifts 280 us per present against a 25 us slew); if it
reads engaged, that is itself a finding.

**B2** is sub-sink rate conversion with no help. The tooth guard disarms (source below sink is
genuine rate conversion), the gate is 8.3 ms, and targets alternate between landing on a real
frame and landing at the midpoint. Expect exactly 30 passthroughs and 30 blends per second at
weights near 0.5. The question this row asks is whether that looks better or worse than a
30 fps repeat, which is the same question as B1 from the other side.

**B3** must match B2 in every policy metric. The generated frames sit at the midpoints, but
without `-subgen` the policy never sees them. A difference between B2 and B3 means the
generated frames are leaking into decisions they should not touch.

**B4** is the row stage 7 was built for and has never been captured. Every midpoint target
has a driver-generated frame sitting on it, so substitution should replace nearly every blend
with a sharp generated frame: `op=pass-gen` at ~30/s, marker interp flag 0, executor real.
Two things to watch. First, `skippedUnscreened` in the subgen summary: the D3D11 path has no
GPU content check and trusts the ring's change-map screening only, so if `-diffmap` is
unavailable on this rig the substitution silently never happens and B4 collapses to B3.
Second, the content-race rate the change map refuses (`refused by the change map`), which
is the 12% capture race measured in the stage 7 work.

**C1 and C2** only if the game offers x3. The x3 regime has known structure problems (epsilon
pairs stride 2 over a 3-flip grid, the relay sees 2 of 3 residues) and is documented as
unsupported. One look on the new clock is cheap; if C1 is unclean, a second run with
`-phasekeep` (the x3-specific keep-real) is the follow-up. Substitution is x2-only by design,
so C2 pins that it stays inert.

## Analysis procedure (identical for every capture)

1. **Init check.** Lines ~7-25 and ~140-150 of the log: `present path D3D11 flip-model
   swapchain`, `D3D9 devices hosted on a hidden window`, the swapchain line naming the format
   and `frame latency 1`, `opened N ring slots`, marker `drawn by the present backend`, comb
   lock ACTIVE with the row's modulus, tooth guard ACTIVE (rows A, B1) or off with the
   rate-conversion reason (rows B2-B4), dejit ACTIVE and not REFUSED.
2. **Plateaus.** Bin head-0 flips at 2 s; a test is a stretch of at least 10 s at or above
   the displayed rate less 7% (112 for 120, 56 for 60, 28 for 30). Every number below is
   windowed to these. Loading fades between tests are excluded by construction.
3. **Log, per test.** Presents/s, present spacing stdev and 5th-95th percentile, `blk`
   median, `lk=1` fraction, op mix as fractions, synth/s, hold-comb/s, hold count.
4. **Marker.** `uv run marker.py decode <mp4> --log <log> --csv <out>`. Per test: counter
   repeats and skips (exact at one present per video frame, which every row here is), blend
   frames, pass-gen frames, provenance agreement. Whole-video checksum must be 100%.
5. **Source.** The game CSV's Fps min per section must equal the DISPLAYED rate (base x FG)
   and its CPU-time reciprocal must equal `-src`. Head-0
   flip gaps over 1.5x the flip period per test, with their timestamps.
6. **Path.** `d3d11 presentpath` lines: OVERLAY through every test, transitions only at load
   and exit. A transition inside a test voids that test.
7. **Fixture.** `uv run testdata/mktrace.py <log> testdata/avatar_<row>_flip.trace "<desc>"
   --skip-us <test-1 start> --until-us <test-3 end>` on pass 1 of run 1. Add to `index.txt`.
   Run the suite with `--ring 32` (the `-lag 75` ring depth) and paste the bounds it prints.
8. **Reference.** Per-row numbers into `validation-reference.json` under a new
   `avatar_flip_corpus` key, each with the run identifiers as provenance.

## Pass criteria

**Pins (A1-A3), per test:**

| metric | bound |
|---|---|
| counter repeats | 0 |
| counter skips | 0 |
| blend frames | 0 (at most 1 across all nine tests) |
| `lk=1` | >= 99% |
| presents/s | 60.00 +/- 0.05 |
| present spacing stdev | < 100 us |
| provenance agreement | 100% |
| presentation path | OVERLAY throughout |

A3 additionally: `substituted` = 0 in the subgen summary.

**Discover rows (B, C):** no pass/fail. The deliverable is the measured table and two
decisions: the near-rate gate (B1, B2) and the `-subgen` default (A3, B4).

**Validity gates, every row:** the game CSV holds the configured rate in every section, and
the path stays OVERLAY through every test. A row that fails either is re-run, not analysed.

## Fixture design

One fixture per row, from pass 1 of run 1, trimmed to the start of test 1 and the end of
test 3. The two loading fades inside that span stay in: they are real stall-recovery content
(the source drops to 60 Hz for 85-220 ms on a fade to black, and the relay blends across the
hole), and excising them would splice the timeline into a fake 6-second stall at each seam.
The cost is that the fixture's synth share (~0.8%) is dominated by the fades rather than the
tests, exactly the effect the `mktrace.py` docstring warns about. If a bound ends up too
loose to catch a steady-state regression, split into one fixture per test; each is ~26 s and
~1600 presents.

`mktrace.py` keeps head 0 only. Fixtures are ~300 KB each.

## Order

A1, A2, A3 first: they are the refactor guard and the reason the campaign runs before the
refactor. Then B4, which is the single most informative unmeasured row. Then B1, which
decides the near-rate gate. Then B2 and B3 together. C1 and C2 last, and only if x3 exists.

## Risks and open questions

- **Does the cap hold the exact rate?** 30 and 59 are frame-limiter settings; the CSV's
  CPU-time column says what the game actually rendered at. `-src` follows that reciprocal,
  not the setting and not the CSV's Fps column.
- **Is `-diffmap` available on this rig?** B4 depends on it entirely on the D3D11 path. The
  init log says whether the change map is active; if not, B4 measures the fallback (nothing)
  and the content check needs porting before stage 7 is real on this path.
- **Does Avatar offer x3 on this GPU?** Determines whether rows C exist.
- **Replay fidelity should rise.** The per-present agreement of ~96% on the old corpus was
  partly present jitter, which is now 20-48 us. Measure it on these fixtures; if it does not
  rise, the divergence was something else.
- **The near-rate decision is a viewing decision.** B1 and B2 produce numbers, but whether
  30 soft frames per second beats one repeat per second is answered by watching the two
  recordings, not by the table.
