# Phase Comb Lock — Validated Targeting Design for Interpolating Compositors

Status: mechanism VALIDATED as dead telemetry (branch `shadow-phase-telemetry`, 2026-07-12;
sim → integer-exact replay → hardware, all agreeing). This document is the design the blend
rebase inherits. It supersedes `docs/phase-alignment-spec.md` (claude/phase-pull): the linear
phase-pull described there was measured pathological and is retired; the snap variant
(claude/phase-pull-snap) is subsumed (§7).

## 1. Problem

The selection target sits at `deadline − lag`; the capture timeline runs on its own clock.
The target's position within a source-frame interval — exactly the blend weight `w` — is the
phase offset between two unsynchronized clocks. Un-steered, it drifts through [0,1] per beat
(~51 s measured at 59.98-vs-60.00), so an interpolating compositor synthesizes every output
frame even at matched rates where passthrough is pixel-perfect. The goal is the stated end
state of the temporal project: real frames when we have them, interpolated frames when we
don't.

## 2. Why the linear pull is retired (measured)

The claude/phase-pull design (EMA error, stability gate, asymmetric slew, pull clamped to
[0, srcP]) treats circular phase as linear. Monotonic clock skew walks it into its clamp once
per beat, forcing a saturate/drain cycle forever: measured 21.2–21.3% engaged at 60→60 with
~170 gate transitions per 5-minute run, and a ~10 s mid-range `w` sweep (soft output) every
beat. At the production 120cap×2 regime: 49% engaged, pull pinned at its 16666 µs clamp.
Sim, replay, and hardware agree within a point. Not fixable by tuning; the controller's
domain is wrong.

## 3. The comb

Each present, the target's phase within a source interval advances by (presentP mod srcP).
At a rational source:present rate ratio N:M (reduced), the phase visits exactly M distinct
values spaced srcP/M apart — the comb. Examples: 60→60 and 240→60 give M=1; 90→60 (3:2)
gives M=2, comb 5556 µs; 30→60 gives M=2.

The controller is the circular counterpart of the pull: error = signed distance from the
pulled target to the frame timeline modulo the comb, in (−comb/2, comb/2]; EMAs (α=1/16)
accumulate on wrapped differences; stability gate `devEma < comb/8`; symmetric slew
25 µs/present; the pull wraps modulo the comb behind a hysteresis band of comb/16 (without
the band, arrival jitter chatters the wrap boundary). The loop is closed per instance: the
error is measured at that instance's own pulled target, never the raw target (open-loop, the
pull integrates without bound).

M derives from the DECLARED ratio only: rationalize `-src` fps against the present rate
(denominator scan m=1..8, tolerance 0.02, fallback M=1). Never anchor to the default-60
fallback — anchoring to a guess manufactures false locks. Past M=8 the comb spacing
approaches arrival jitter, the gate cannot close, and the variant refuses: the correct
behavior for effectively-irrational ratios. A wrong `-src` produces a misfit comb and the
same clean refusal.

Locked, one present in M lands on a real frame — the theoretical maximum — and the remaining
M−1 sit at fixed comb offsets (constant blend weights, e.g. 0.5 at M=2): a constant-cadence
pulldown instead of a drifting sweep.

## 4. Measured results (shadow campaign)

| corner | result |
|---|---|
| 60→60 `-src 60` (M=1) | engaged 100%, 0 gate transitions, one 1-frame pull step per 51 s beat, w extreme 97.5% |
| 240→60 `-src 240` (M=1) | engaged 100%, 0 transitions |
| 120cap×2 `-src 60` (M=1, production) | 335 s continuous lock after 40.7 s acquisition, beat ≥ ~220 s |
| 90cap×1 `-src 90` (M=2, comb) | engaged 99.7%, w bimodal 49.3% real-frame / 49.6% at 0.5, sharp-blend alternation 99.1% (clean 2:3 pulldown); unpulled baseline harvested 8.5% |
| 90cap misfit check (pre-comb build, srcP modulus) | 0% engaged over 344 s — refusal corner |

Capture cadence itself was proven in the same campaign: NvFBC batch-start arrivals hold the
source period exactly (90cap: mean 11.111 ms over 30954 grabs, σ 262 µs). Note the capture
log's `dt` measures last-batch-member → next-batch-start and reads short by the intra-batch
ε span under frame generation; batch-start cadence is the true timeline.

## 5. Live-wiring clauses (blend rebase)

1. **Targeting term**: the comb wrap math above, applied as extra lag on the selection
   target. Blend modes only; nearest modes keep their validated selection untouched.
2. **Compositing consumes lock state, not raw w.** Per present, with the bracket at the
   pulled target: if `min(beforeDiff, afterDiff) < comb/8`, present that real frame
   directly; else blend at the bracket weight. The threshold sits at a boundary far from
   both operating points (locked presents measure ≤ ~0.65 ms from a real frame; hole/off-comb
   presents ≥ ~⅓ comb), so it cannot flip-flop — the gate-placement lesson. Which side the
   lock settles on (w≈0 vs w≈1) is thereby irrelevant, and instantaneous-w jitter never
   reaches the output.
3. **Dropped frames need no detection.** A missing source frame is invisible to the lock
   (the wrapped error is invariant modulo the comb) and visible to the bracket (it widens):
   the hole present reads w≈0.5 and clause 2 classifies it BLEND — an interpolated
   replacement frame, neighbors unaffected. Sim: 25 injected drops during lock → exactly 25
   isolated blend presents, all w 0.49–0.51, zero lock disturbance. Recovery depth is set by
   the lag: a hole present interpolates only if its after-endpoint has arrived by pick time,
   so with lag = c·P the last ⌊c⌋ presents of any hole interpolate and the earlier ones
   hold. Default 1.25× lag = one frame of lookahead: single-frame holes recover fully; a
   two-frame hole yields one held frame then one ⅔ blend. Deeper recovery is purchasable at
   one source period of constant lag per additional frame of coverage (2.25× turns the
   two-frame hole into clean ⅓/⅔ blends) — a deliberate per-run constant, consistent with
   the static-lag philosophy; a lag knob for hitch-prone titles is future work needing no
   new mechanism.
4. **Latency policy (T10)**: a live pull makes latency a slow sawtooth — ramping at
   clock-drift rate, stepping back one comb spacing per beat. Bounded by design at ≤ 1 comb
   spacing peak-to-peak (16.7 ms at M=1, 5.6 ms at 90→60), deterministic and
   content-independent, under AV-sync perception thresholds. ACCEPTED as a documented trade
   alongside the static lag. No quantization: steady-state pull movement measures
   0.6–5.6 µs/present, orders below perception; quantizing adds discrete steps for nothing.
   The wrap step itself costs one 1-frame slip per beat — the same slip nearest mode already
   pays at the drift boundary.
5. **The ring estimator stays telemetry.** The estimator-fed shadow variant beat-cycles like
   the anchored one and adds hitch coupling (gaps < 125 ms enter the EMA and inflate the
   gate); declared-rate anchoring is strictly cleaner. Estimator audits the declaration, as
   in the static-lag design.
6. **Acquisition** completes within ~40 s of regime start (measured worst case); during
   acquisition clause 2 degrades to plain blend — acceptable, no cover mechanism needed.
7. **The ring holds real frames only — synthesized frames never enter it.** For blend this
   is lossless (lerps compose: any later present can reproduce the same output from the real
   endpoints, which the 8-slot ring retains across any spannable hole); for NVOFA it is
   load-bearing (warps do not compose — re-warping synthetic frames accumulates flow error,
   and write-back makes output recursive in its own artifacts). It also preserves the ring's
   single-writer design and the estimator's real-cadence timeline. The layering: ring =
   source truth (real only); present-side state = what was shown (may be synthetic —
   lastShownSurface may re-present a composed frame during a stall, but that surface must
   have stable ownership and never flow back into ring, bracket, or estimator).
8. **Snap (claude/phase-pull-snap) is subsumed.** Its target case — anchoring real frames at
   non-integer ratios — is what the comb harvests deterministically (49.3% vs the ~8.5% a
   sweep grazes), without snap's sharp/soft breathing risk (a held lock has no enter/exit
   transitions). Residual cases (undeclared sources; comb-refused ratios) are deliberately
   not built: the workflow declares `-src` per title, no current title runs a refused ratio,
   and the opportunistic win there is small. If snap-like logic ever returns, it must guard
   lastShownTs monotonicity (invariant-review flag).

## 6. Validation corners for the live blend build

Beyond the blend branch's existing visual/pacing suite: 60→60 lock = passthrough
sharpness parity with nearest (same-day A/B); 90→60 = 2:3 cadence with constant-0.5 blends,
no breathing; refused ratio (e.g. uncapped) = clean full-blend behavior; dropped-frame
content = interpolated replacements at holes (log: isolated mid-w BLEND presents during
lock). Hitch survival during an engaged lock remains UNTESTED (never captured while locked);
score it in the first hitchy run.
