# Static Bracketing Lag (-src) — Feature Spec

Branch: `claude/adaptive-bracketing-delay` (off dev @ v0.0.12). Prerequisite for blend
(stacked branch `claude/blend-mode`). Status: implemented (rework of the original
continuous-adaptation design; see History).

## Problem

The present target lags "now" so that a frame *newer* than the target exists at pick time —
bracketing. A lag of one present period fails whenever the source runs slower than the
present rate (30-base frame gen, heavy scenes, matched-rate beat): the after-frame usually
does not exist yet, selection degrades to repeats, and blend — which cannot degrade
gracefully, having nothing to blend toward — is blocked on exactly this.

## Design

**The lag is a launch-time constant**: `lag = max(presentPeriod, 1.25 × assumedSrcPeriod)`.

- The source-rate assumption defaults to **60 fps** — the slowest source served without
  configuration — and `-src <fps>` overrides it in either direction. Slower sources need
  more lag (`-src 30` → 41.7 ms); faster ones can ride the present-period floor
  (`-src 240` → 16.7 ms at 60 Hz present). At 60 Hz present the default yields 20.8 ms.
- The lag never moves during a run. It is output latency, and only a constant can be
  compensated for downstream (T10): the operator sets one audio-delay per title config.
  Forgetting `-src` never produces artifacts for sources ≥ 60 fps — worst case is latency
  left on the table.
- The capture-side source-period estimator (`m_srcPeriodEmaQpc`, EMA α = 1/8 over
  batch-start gaps, stall gaps > 125 ms excluded) is retained as **telemetry only**: every
  ~10 s the present thread audits the assumption. Measured slower than assumed (> 9/8×)
  → error line with the suggested `-src` (starvation warning). Measured at least 2× faster
  with a real win available (> 2 ms) → info line with the lower-latency `-src` suggestion.

**Advance gate** (same rework): when only the after-frame is newer than the last shown,
advance UNLESS the target is still on the shown frame (`beforeDiff` inside the stickiness
band), with Schmitt state (reopen at 2x band) so a crossing costs one clean flip. The
ungated advance boundary sits exactly where `before == lastShown` begins (w = 0); the
present/source clock beat parks the target phase there periodically and arrival jitter
flip-flops the crossing for seconds — a cluster of early-advance cadence glitches. Healthy
operating points keep `beforeDiff` far above the band in every regime, so the gate is inert
outside the crossing. A midpoint comparison is deliberately NOT used: matched-rate steady
state operates at the midpoint, and any threshold at the operating point flip-flops on
jitter regardless of margin (measured: ~280 dupe+skip pairs per 60→60 run, replay-simulated
and confirmed; the proximity gate replays bit-identical to ungated there).

## Constants

| constant | value | why |
|---|---|---|
| headroom | 1.25× | one full source period + 25% for arrival jitter |
| default assumption | 60 fps | slowest source served without configuration, at any present rate |
| slow-source warning | est > 9/8 × assumed | beyond what the headroom absorbs; 59.98 Hz vs 60 stays quiet |
| fast-source suggestion | est × 2 < assumed, win > 2 ms | only flag meaningful latency savings |
| telemetry cadence | 600 presents | ~10 s at 60 Hz; wrong -src caught within the first minute |
| midpoint gate margin | stickiness band (1 ms) | same anti-flip-flop constant as the nearest-pick Schmitt band |

## Observability

`lag=<µs>` on the per-present `temporal` log line (append-only) — now a constant; analysis
treats any change mid-run as a defect. Setup logs the fixed lag and the assumption it came
from. `temporal telemetry:` lines carry the estimator audit.

## Validation

| run | pass |
|---|---|
| `240_x1_t_60_ufo` | reference-identical to baseline (lag pinned at floor 16667 µs with `-src 240`; 20833 µs default — both static) |
| `60_x1_t_60_ufo` | lag constant 20833 µs from first present; no ramp; floor ≤ baseline; no-after ≈ 0 |
| 30-base KCD `t:60` with `-src 30` | lag constant 41.7 ms; no-after ≈ dips only; boundary-dwell early-advance clusters (1-3-2 cadence) GONE |
| 30-base KCD `t:60` without `-src` | telemetry error line suggesting `-src 30` within ~1 min |

## History

The original design adapted the lag continuously toward 1.25× the live period EMA with a
100 µs/present slew. Validated mechanics survive in the estimator and the headroom constant.
Demoted after measurement: gameplay lag wandered 23.8–49 ms at 30-base KCD and 20.0–26.6 ms
at 120cap×2 (the EMA rides content dips), which is dynamic output latency — impossible to
compensate downstream — and the moving target phase interacted with regime shifts (ring-window
misses at menu transitions). Static-by-declaration deletes the class instead of tuning it.
