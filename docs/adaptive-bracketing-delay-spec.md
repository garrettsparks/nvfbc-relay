# Adaptive Bracketing Delay — Feature Spec

Branch: `claude/adaptive-bracketing-delay` (off dev @ v0.0.11). Prerequisite for blend
(stacked branch `claude/blend-mode`). Status: implemented.

## Problem

The present target lags "now" by a fixed one present period (16.7 ms) so that a frame *newer*
than the target exists — bracketing. That assumption fails whenever the source runs slower
than the present rate (30-base frame gen, heavy scenes, menus): the after-frame usually does
not exist yet, selection degrades to repeats, and blend — which cannot degrade gracefully,
having nothing to blend toward — is blocked on exactly this.

## Design

**One number crosses the capture→present seam**: the capture thread's estimate of the source
period.

- **Capture side** (`CaptureRing`): `m_srcPeriodEmaQpc`, an EMA (α = 1/8) over **batch-start
  to batch-start** gaps. Batch-start gaps make the estimate frame-gen-proof by construction —
  intra-batch ε wakes never enter it. Gaps > 125 ms (stalls, grab-timeout re-grabs) are
  excluded as non-cadence. Written with relaxed atomics by the capture thread; read by the
  present thread via `EstimatedSourcePeriodQpc()` (0 until warmed).
- **Present side** (`TemporalCaptureMode`): per present,
  `desired = max(presentPeriod, 1.25 × P̂)`, then the actual lag *slews* toward desired at
  ≤ 100 µs per present. Seed = one present period (baseline behavior until the estimate
  warms).

## Latency policy analysis (T10)

The lag IS output latency, so an adaptive lag is variable latency — the thing T10 forbids —
*unless bounded and slow*. The slew limit is that bound: 100 µs/present = 6 ms/s of drift.
A 60→90-base regime change moves the lag ~7 ms over ~1.2 s — a controlled content-time ramp,
imperceptible, instead of a step (visible) or per-frame jitter (stutter). In steady state the
EMA is effectively constant and the lag freezes; V-suite `jit`/`pdt` metrics are untouched
because the lag shifts *which* frame is picked, never *when* the present fires.

## Constants

| constant | value | why |
|---|---|---|
| EMA α | 1/8 | stable ≈ 8 source frames after a regime change; jitter-immune steady-state |
| headroom | 1.25× | one full source period + 25% for arrival jitter (successor-spec value) |
| slew | 100 µs/present | 6 ms/s ramp; regime transitions complete in ~1–2 s |
| stall cutoff | 125 ms | excludes grab-timeout re-grabs (100 ms) and load hitches from cadence |

## Observability

`lag=<µs>` appended to the per-present `temporal` log line (append-only: existing parsers and
stride.py are unaffected). Expected: 16667 steady at sources ≥ 60 Hz (identical to baseline);
~41.7 ms at 30-base; smooth ramps at regime changes.

## Validation

| run | pass |
|---|---|
| `240_x1_t_60_adl_ufo` | numbers identical to v0.0.11 (source ≥ present → lag pinned at 16667 µs; log confirms) |
| `60_x1_t_60_adl_ufo` | lag ≈ max(16.7, 1.25×16.7) = **20.8 ms** — note: slightly higher than baseline at matched rates; floor must stay ≤ baseline (~4–5 repeats) and no-after ≈ 0 |
| `30cap` or `30`-base FG KCD `t:60` | the payoff: `no after-frame` log lines ≈ 0 vs ~constant at baseline; picks healthy instead of repeat-degraded |

The matched-rate case deserves attention: at 60→60 the adaptive formula raises lag from 1.0
to 1.25 present periods (+4 ms constant latency). That is the designed trade (T10: constant
offset is acceptable without limit, in service of pacing) — but it is a *behavior change* at
the config the floor was calibrated on; the run above guards it.
