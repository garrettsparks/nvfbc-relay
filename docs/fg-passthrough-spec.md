# Frame-Gen Passthrough (Path B) — Feature Spec

Branch: `claude/fg-passthrough` (stacked on `claude/adaptive-bracketing-delay` — it needs the
source-period EMA). Status: implemented, uncharacterized. The baseline spec's successor
item 4, deliberately not pre-built into the baseline; built now that its prerequisites exist.

## What it does

`-fg keep` switches the CaptureRing from keep-real collapse (default, validated) to
**publishing every frame-gen batch member**, re-stamped and tagged:

- Wakes arrive submission-batched (members ε apart at batch start), so raw wake times are
  *false display times* for generated members. Member *i* of a batch is re-stamped at
  `batchStart + i·(P̂/k̂)` — spread across the base period at the display's actual cadence.
- `P̂` = the adaptive-delay branch's source-period EMA (batch-start gaps, FG-proof by
  construction). `k̂` = predicted batch size: **mode of the last 8 completed batch sizes**.
  The current batch's size is unknowable until it ends, so we predict — this is the
  predicted-k̂ publish design chosen over deferred publish (no added latency) and over
  retro-restamping (forbidden: published timestamps are immutable, hysteresis depends on it).
  Mispredicts (multiplier regime changes) misplace stamps within one base period for one
  batch and self-heal.
- Members `i < k̂−1` are tagged `generated` (measured order is [gen…, real] — last member
  real); the tag is **advisory** (capture-log `g=` field) — selection deliberately ignores
  it. No retraction; `col` in the log becomes the cumulative re-stamp count in this mode.

## Why (the product case)

Play at 90 base × 2 = 180 displayed; the ring then carries a 180 Hz-effective timeline of
honest timestamps. `t:60` (or `b`/FRUC later) selects whichever member lands nearest each
target — **the driver's own interpolation becomes the rate converter**, with zero
interpolation work on our side and gen-frame quality equal to what the player sees on the
main display. This competes with Path A (collapse + our blend/NVOFA) for FG titles; the two
now coexist behind one flag, so the comparison is a pair of captures.

## What selection needs: nothing

Gen frames are legitimate candidates with honest interpolated stamps. Nearest-pick,
hysteresis, the Schmitt band, adaptive lag — all operate on timestamps and are agnostic.
That was the point of "timestamps are the product."

## Known interactions and limits

- **Ring pressure**: publishes at wake rate (k × base). 8 slots at ×2/90-base = 180/s
  ≈ 44 ms span vs the ~16.7 ms lag — adequate; at ×3+ pair with the sibling
  `claude/ring-capacity-config` branch (`-ring 4k`). The branches are independent; merge
  both for high-multiplier use.
- **Stamp granularity**: re-stamps assume even display pacing of members within the base
  period (i·P̂/k̂). SM presents gen/real evenly at the display; if a future FG tech paces
  unevenly, stamps inherit that error — characterization first, per the standing rule.
- **In-game DLSS-FG remains uncharacterized** (successor item 5): wake pattern may differ
  from Smooth Motion; do not assume the [gen…, real] order or the ε-batching transfers.
- **Tag semantics**: `generated` is prediction-based (k̂), so the last member of a
  *mispredicted* batch can be mislabeled for one batch. Consumers that ever act on the tag
  (FRUC real-only pairing) must tolerate that or the tag must graduate to batch-end
  confirmation.

## Validation / characterization

| run | expectation |
|---|---|
| `120cap_x2_t_60_fgkeep_kcd` | capture `g=` alternates 1/0; stamps land ~8.3 ms apart (stride.py source period ≈ 8.3 ms); video smoothness ≥ collapse baseline; reticle shows SM's own gen-frame quality (ghosting on gen members = SM's artifact, faithfully relayed — compare against the main display, not against collapse) |
| `120cap_x2_t_vsync_fgkeep_kcd` | production present path; pdt/jit unchanged |
| A/B vs same-session `-fg collapse` runs | the Path A vs Path B product comparison: smoothness vs crispness, per title |
| `240_x1_t_60_fgkeep_ufo` | no-FG regression guard: batch size 1 → k̂=1 → behavior identical to collapse (stamps = arrival, no tags) |

Log assertions: `g=` never 1 outside FG sessions; re-stamp count ≈ (k−1)/k of wakes under
FG; stamp monotonicity (stride.py runs clean, no negative strides).
