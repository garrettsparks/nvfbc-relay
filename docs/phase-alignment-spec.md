# Phase Alignment — Problem + Two Candidate Fixes

Branches: `claude/phase-pull` (option 2, stacked on `claude/blend-mode`) and
`claude/phase-pull-snap` (option 3, stacked on `claude/phase-pull` so its diff is exactly the
snap increment). Built for A/B comparison; option 2 is the favored bet going in.

## The problem (found in review, 2026-07-03)

The selection target sits at `deadline − lag`; the capture timeline runs on its own clock. So
the target's position *within* a source-frame interval — which is exactly the blend weight
`w` — is the **phase offset between two unsynchronized clocks**: it drifts through [0,1] over
each ~33–50 s beat period and is essentially never 0 or 1.

Nearest-pick doesn't care: it shows a real frame regardless of `w`. But an interpolating
compositor (blend today, FRUC later) synthesizes **every** output frame, and for the
mid-phase portion of every beat cycle produces ~50/50 combinations of two real frames — on
*matched-rate content* (60-base → 60 present, the production case) where passthrough would be
pixel-perfect. Blend: uniform softening for ~half of each beat. FRUC: interpolation artifacts
(UI halos) on frames that needed no interpolation at all, plus per-frame latency cost.

The blend spec's matched-rate validation would have caught the symptom
("verify no needless softening") but nothing in the design *prevented* it.

## Option 2 — phase-pull (branch `claude/phase-pull`) — favored

Extend the adaptive-lag machinery with a slow control term that pulls the target *onto* the
real-frame timeline when the rate ratio permits lock.

- Error signal: `beforeDiff` (how far the target sits past the before-frame). Pulling the
  target **back** onto the before frame (increasing lag by the error) is the safe direction —
  it adds bracket margin; pulling forward would erode the after-frame guarantee.
- Filtered: `errEma` (α=1/16) is the phase offset; `devEma` (α=1/16 of |err − errEma|)
  measures phase *stability*. Near-integer ratios (60→60, 240→60) have near-constant phase →
  small devEma → **engage**; non-integer ratios (90→60) sweep → large devEma → **disengage**
  (interpolation is genuinely needed there; the pull returns to zero).
- Engaged: `m_phasePullQpc` slews toward `errEma` at ≤ 25 µs/present (1.5 ms/s — an order
  above the measured inter-crystal drift ~0.12 ms/s, three orders below perception), clamped
  to [0, source period]. Total lag = adaptive base + pull.
- Result at lock: `w → 0`, blend degenerates to presenting the before frame — real pixels,
  zero softening, and (for FRUC later) zero interpolation cost and zero artifact surface at
  integer ratios. At the beat's frame-boundary wrap the error steps by one period, devEma
  spikes, the loop disengages and re-locks — one soft resync per beat instead of a glitch.
- Scope: **blend modes only** (`m_blend`). Nearest modes present real frames regardless of
  `w`; their validated behavior is untouched, and the pull's latency wobble would buy them
  nothing.
- Latency accounting (T10): the pull adds a bounded (≤ 1 source period), slowly-varying
  (≤ 25 µs/present) component — same compliance argument as the adaptive lag, one layer up.

Observability: ` pull=<µs>` appended to the temporal log line. Expected traces: 60→60 —
pull converges to the beat phase within ~seconds and tracks the slow drift, w pinned ≈ 0,
`pick=blend` with w≈0 (or effectively passthrough); 90→60 — pull ≈ 0 throughout, w sawtooths.

## Option 3 — phase-pull + snap anchoring (branch `claude/phase-pull-snap`)

Everything above, plus a per-frame passthrough early-out in the blend compose arm: when a
real frame lies within a threshold of the target, present it directly instead of blending.

- Thresholds derived from the measured source period: latch when
  `min(beforeDiff, afterDiff) < P̂/8`, unlatch when `> P̂/4` — a Schmitt pair, because the
  drifting phase crossing a single threshold would flip-flop between passthrough and blend
  (the same lesson as the selection stickiness band, applied to a new boundary).
- When latched: StretchRect the nearer real frame; `pick=snap`. Otherwise blend as usual.
- Value over option 2 alone: at *non-integer* ratios the pull is disengaged but the sweeping
  phase still passes near real frames once per cycle — snap anchors those frames as genuine
  pixels (periodic ground truth between interpolated spans, like I-frames), and for FRUC it
  trims the per-frame cost at the sweep edges.
- Risk it must disprove: visible texture "breathing" at snap enter/exit on soft content —
  the transition is real-frame-sharp ↔ blend-soft; the hysteresis gap bounds the frequency
  but not the existence of the transition. This is the artifact the A/B exists to judge.

## A/B comparison plan (same captures, both branches)

| capture | option 2 expectation | option 3 delta |
|---|---|---|
| `60_x1_b_60_{pp,pps}_ufo` | pull locks, w≈0, softening gone, ≈ nearest sharpness | identical (snap latched permanently at lock) |
| `240_x1_b_60_{pp,pps}_ufo` | integer 4:1 — also locks; compare vs nearest-sb2 gold | identical |
| `180cap_x2_b_vsync_{pp,pps}_kcd` | pull disengaged; smooth blend (the 1.5:1 judder-killer demo) | periodic `pick=snap` anchors; judge breathing vs anchoring benefit |
| `120cap_x2_b_vsync_{pp,pps}_kcd` | production: FG base ≈ 60 vs card 60 — near-integer, expect lock | identical if locked |

Metrics: `pick=` census (blend/snap/stall ratios), `pull=` convergence trace, `w`
distribution (pinned vs sawtooth), visual sharpness A/B against same-session nearest capture,
and the existing pacing suite (blend must not regress pacing in any variant).

Decision rule: if option 2 locks reliably at near-integer ratios and 90→60 blend looks clean
without anchoring, ship 2 and drop 3 (simpler); 3 wins only if its anchoring visibly improves
non-integer content without visible breathing.

## Implementation notes (branch `claude/phase-pull-snap`)

Snap lives entirely in the blend compose arm: Schmitt pair latch < P̂/8 / release > P̂/4 on
`min(beforeDiff, afterDiff)`; latched → StretchRect the nearer real frame (`pick=snap`),
else blend as usual. One bool of state (`m_snapLatched`). If the source-period estimate is
unavailable (warmup) snap stays off. The snapped side may be the *after* frame — that is
nearest semantics, safe: it exists, it is real, and it is within P̂/8 of the target.
