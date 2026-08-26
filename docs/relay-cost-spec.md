# What does running the relay cost the game?

Status: UNMEASURED. This is the methodology, to be run against the Avatar: Frontiers of Pandora
benchmark.

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

Per present (60/s): one composite (a `ps_3_0` lerp in `b:` mode, a `StretchRect` in `t:`) plus
`PresentEx`.

**The prime suspect is (3), not (2).** ~960 MB/s of copy is trivial for this GPU; 116 forced
pipeline flushes per second are exactly what stops a GPU packing the game's work. The flush is
a correctness requirement of the current design: D3D9Ex shared surfaces have no cross-device
sync primitive, so the capture device must be flushed before the present device reads the slot.

## The ladder

Avatar's benchmark is repeatable, so these are clean A/Bs rather than session-to-session
guesses. Run each several times; report the benchmark's own average FPS AND keep the relay log.

| # | condition | adds over the row above |
|---|---|---|
| 0 | no relay running | baseline |
| 1 | `vsync` | NvFBC grab + PresentEx. NvFBC writes STRAIGHT into the backbuffer: no ring, no copy, no flush, ONE device |
| 2 | `t:vsync -src 60 -lock` | ring + per-wake StretchRect + per-wake flush + a second private D3D9Ex device |
| 3 | `b:vsync -src 60 -lock` | blend shader on synth presents |
| 4 | `b:vsync -src 60 -lock -lag 75` | +132 MB VRAM (ring 16 -> 32) |

Row 0 to 1 is NvFBC's own capture cost - nothing in this codebase can reduce it. Row 1 to 2 is
the relay's architecture. Row 3 to 4 is purely VRAM, since extra lag adds no GPU work at all.

**Caveat on row 1 to 2**: it bundles the ring/copy/flush WITH the second device. The two-device
design exists because a blocking grab on a shared device slaves present timing to capture
arrivals (measured: present jitter = capture period / 2). This ladder cannot separate them; if
row 1->2 is where the cost lives, that separation is the next experiment, not a conclusion.

## Hold constant

Same benchmark, same settings, same resolution, same DLSS/FG configuration. No `-mark`
(burns a strip and costs ColorFill calls). **No `-fgphase`** - it degrades output ~3.4x and its
per-wake readback would dominate exactly the measurement being made. Watch VRAM headroom:
Avatar at 1440p with FG is VRAM-hungry and row 4 adds 132 MB, which on a full card produces
eviction stutter that looks like a relay problem and is not one.

## What each source of evidence can say

- **Benchmark FPS** - the only thing that measures cost to the game. Everything else is
  supporting.
- **`flush=` in the log** - GPU contention on the capture side. Runs 86-97 us p50 on the Get
  Medieval captures; if it climbs under Avatar the flush is contending, and if it climbs
  *further* at row 4 that is the VRAM answer.
- **`jit=` / `pdt=`** - whether the relay still hits its own deadlines under load. A relay that
  stays perfect while the game drops is the expected shape of a GPU-contention cost.
- **capture wake rate** - whether NvFBC keeps up.

## Predictions, so the test can falsify them

1. **Row 3 -> 4 costs nothing measurable**, unless VRAM is tight. Extra lag adds no GPU work.
2. **Row 2 -> 3 is small.** The lerp is bandwidth-twin to the StretchRect it replaces, and
   synth is under 2% of presents.
3. **Row 1 -> 2 is where the cost is**, and more of it is the flush than the copy.

If (3) holds, the fix is not on the NvFBC side. **Direct-write is already dead** (see
`direct-write-dead` memory: `dwNumBuffers > ~2` crashes inside NvFBC64_.dll), and it would have
removed the copy while keeping the flush anyway. The flush only goes away with a real
cross-device sync primitive - D3D11 fences or keyed mutexes - which makes this a PERFORMANCE
argument for the DXGI backend, entirely separate from the capture-quality argument in
`etw-frame-timing-spec.md`. That would be a new and much stronger case for it.

If instead row 0 -> 1 dominates, the relay's architecture is not the problem, NvFBC capture is,
and no amount of restructuring on our side helps.
