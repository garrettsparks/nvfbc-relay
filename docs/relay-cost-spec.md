# What does running the relay cost the game?

Status: UNMEASURED. This is the methodology, to be run against the Avatar: Frontiers of Pandora
benchmark. Its answer gates the release defaults: a flag goes on by default only when the ladder
row that adds it shows no cost beyond run-to-run noise.

## Why it has never been answered

Every performance number in this project so far is RELAY-SIDE: `jit=` (present vs deadline),
`pdt=` (inter-present interval), `flush=` (the capture thread's wait for its own GPU work).
Those measure how well the relay keeps its own schedule. **They cannot measure GPU time the
relay takes away from the game** - if the game drops 10% and the relay still hits every
deadline, all three look perfect.

The blend-testing kickoff deferred it explicitly: "SERIOUS perf work (pt= distributions, the
FlushD3D11 busy-wait on the present thread - the real cost center) belongs to the NVOFA
session". That session never happened, so the question is still open.

## What the relay actually does per frame

Per capture wake (~116/s at 60x2):

1. `NvFBCToDx9VidGrabFrame` - driver captures 2560x1440, scales to 1920x1080
2. `StretchRect` into the ring slot - a full 1920x1080 copy, ~8.3 MB, so ~960 MB/s sustained
3. `Issue(D3DISSUE_END)` then spin on `GetData(D3DGETDATA_FLUSH)` - **a command-buffer flush
   and wait, ~116 times a second**

Per present (60/s on the sink): one composite plus the present. On `b:vsync`, the D3D11
flip-model path, that is a lerp shader draw into the swapchain's back buffer and a `Present`
whose buffer DWM shares or the display scans out directly. On `b:dwm`, the D3D9 path, it is a
`ps_3_0` lerp or a `StretchRect` into the D3D9 back buffer and a `PresentEx`, which DWM then
copies into the window's redirection surface.

**The prime suspect is (3), not (2).** ~960 MB/s of copy is trivial for this GPU; 116 forced
pipeline flushes per second are exactly what stops a GPU packing the game's work. The flush is
a correctness requirement of the current design: D3D9Ex shared surfaces have no cross-device
sync primitive, so the capture device must be flushed before the present device reads the slot.

## The ladder

Avatar's benchmark is repeatable, so these are clean A/Bs rather than session-to-session
guesses. Each row adds one thing to the row above it, so the difference between neighbouring
rows is the cost of that one thing.

| # | mode and flags | what it adds |
|---|---|---|
| 0 | no relay running | baseline |
| 1 | `vsync` | NvFBC grab + PresentEx. NvFBC writes STRAIGHT into the backbuffer: no ring, no copy, no flush, ONE device |
| 2 | `b:dwm -src 60` | ring + per-wake StretchRect + per-wake flush + a second private D3D9Ex device + the blend, presented through DWM's copy |
| 3 | `b:vsync -src 60` | the same relay on the D3D11 flip-model present instead: a D3D11 device reading the ring, no DWM copy |
| 4 | `b:vsync -src 60 -lock -lag 75` | the comb lock (arithmetic) and +132 MB VRAM (ring 16 -> 32 slots) |
| 5 | `b:vsync -src 60 -lock -lag 75 -etw -dejit` | the ETW consumer thread and the per-present lateness walk. The release default candidate |

Row 0 to 1 is NvFBC's own capture cost - nothing in this codebase can reduce it. Row 1 to 2 is
the relay's architecture. Row 2 to 3 is the present path alone. Rows 3 to 5 are the candidate
default flags, and none of them adds GPU work except row 4's VRAM.

Every row from 3 on carries all the flags of the row above it, so row 5 is the complete release
candidate command and not `-etw -dejit` on their own. That order is forced as well as chosen:
`-dejit` refuses to run without the comb lock (its calm gate reads lock state) and without
`-etw` (it measures lateness against the flip grid), so it cannot be measured on a row that
lacks `-lock`.

**Caveat on row 1 to 2**: it bundles the ring/copy/flush WITH the second device. The two-device
design exists because a blocking grab on a shared device slaves present timing to capture
arrivals (measured: present jitter = capture period / 2). This ladder cannot separate them; if
row 1->2 is where the cost lives, that separation is the next experiment, not a conclusion.

## The session

Three rounds, each round running every row once in ladder order, so thermal and background drift
land on every row equally instead of on whichever row ran last. Restart the relay for every run
with a fresh empty NvFBCR.log beside the exe, and rename the log when the run ends. N is the
round number; eighteen bench runs in all.

| content | mode | flags | files | expect |
|---|---|---|---|---|
| Avatar bench, 3 tests | relay off | | `cost_r0_N.csv` | baseline score |
| Avatar bench, 3 tests | `vsync` | | `cost_r1_N.csv`, `.log` | NvFBC's own cost |
| Avatar bench, 3 tests | `b:dwm` | `-src 60` | `cost_r2_N.csv`, `.log` | the largest step |
| Avatar bench, 3 tests | `b:vsync` | `-src 60` | `cost_r3_N.csv`, `.log` | score at or above row 2 |
| Avatar bench, 3 tests | `b:vsync` | `-src 60 -lock -lag 75` | `cost_r4_N.csv`, `.log` | same as row 3 |
| Avatar bench, 3 tests | `b:vsync` | `-src 60 -lock -lag 75 -etw -dejit` | `cost_r5_N.csv`, `.log` | same as row 4 |

**Decision rule.** A row costs something when its three scores sit outside the spread of the
row above's three. Rows 4 and 5 decide the defaults: a flag whose row costs nothing goes on, and
a flag whose row costs something comes back here with the number before anyone decides.

## Hold constant

Same benchmark, same settings, same resolution, same DLSS/FG configuration. **No frame-rate
cap**: the game must be GPU-bound, or a cap hides any cost the GPU has the headroom to absorb
and every row reads the same. Frame generation stays as it is normally played; uncapped it
produces more capture wakes per second than capped play does, so the measured cost is an upper
bound on what a capped stream pays. No `-mark` (burns a strip every present). **No `-fgphase`** -
it degrades output ~3.4x and its per-wake readback would dominate exactly the measurement being
made. Watch VRAM headroom: Avatar at 1440p with FG is VRAM-hungry and row 4 adds 132 MB, which on
a full card produces eviction stutter that looks like a relay problem and is not one.

**`-src 60` on an uncapped source is a wrong declaration on purpose.** Uncapped, the game runs
well above 60, so every row that carries `-src 60` is told the wrong rate. It stays anyway: it
is the release default, rows 4 and 5 cannot run without it (`-lock` derives the comb from the
declared rate and `-dejit` refuses without the lock), and in rows 2 and 3 it changes nothing
because the relay assumes 60 when `-src` is absent. It moves no GPU work. Capture work follows
the game's actual rate whatever is declared, present work follows the sink, and with `-lag 75`
the ring sits at its 32-slot ceiling at any declared rate of 60 or faster. What it does ruin is
pacing, so the blend share, holds and lock engagement in these logs, and the output video, are
not evidence of anything. No recording is needed; if the capture PC records anyway it costs the
game nothing, being a separate machine. `-mark` stays off either way.

**Row 2 does more work uncapped than on a capped stream.** The D3D9 present follows DWM's
compose clock, which under in-game frame generation runs at the displayed rate (117 to 120
presents a second on the capped 60x2 captures). Uncapped, the displayed rate is higher, so row 2
should present more often still and composite that many more frames. That is how `b:dwm` really
behaves, but it inflates row 2 relative to real use, so read row 2 against row 3 with the present
rate from row 2's log (`pdt=`) in hand.

## What each source of evidence can say

- **Benchmark score and the CSV's GPU time** - the only things that measure cost to the game.
  Not the CSV's Fps column: under in-game frame generation it reports the displayed rate.
  Everything else is supporting.
- **`flush=` in the log** - GPU contention on the capture side. Runs 86-97 us p50 on the Get
  Medieval captures; if it climbs under Avatar the flush is contending, and if it climbs
  *further* at row 4 that is the VRAM answer.
- **`jit=` / `pdt=`** - whether the relay still hits its own deadlines under load. A relay that
  stays perfect while the game drops is the expected shape of a GPU-contention cost.
- **capture wake rate** - whether NvFBC keeps up.

## Predictions, so the test can falsify them

1. **Row 4 -> 5 costs nothing measurable.** ETW consumption is a CPU thread reading driver
   events, and the lateness walk is arithmetic on the present thread.
2. **Row 3 -> 4 costs nothing measurable**, unless VRAM is tight. The lock is arithmetic and
   extra lag adds no GPU work.
3. **Row 3 is no slower than row 2.** Flip model removes DWM's full-frame copy of every present,
   and the lerp draw is the same work on either device.
4. **Row 1 -> 2 is where the cost is**, and more of it is the flush than the copy.

If (4) holds, the fix is not on the NvFBC side. **Direct-write is already dead** (see
`direct-write-dead` memory: `dwNumBuffers > ~2` crashes inside NvFBC64_.dll), and it would have
removed the copy while keeping the flush anyway. The flush only goes away with a real
cross-device sync primitive - D3D11 fences or keyed mutexes - which makes this a PERFORMANCE
argument for the DXGI backend, entirely separate from the capture-quality argument in
`etw-frame-timing-spec.md`. That would be a new and much stronger case for it.

If instead row 0 -> 1 dominates, the relay's architecture is not the problem, NvFBC capture is,
and no amount of restructuring on our side helps.
