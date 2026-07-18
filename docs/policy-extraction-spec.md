# Policy Extraction Spec — Pure Testable Decision Logic (TemporalPolicy)

Status: DESIGN (not yet implemented). Goal: extract the selection + comb-lock + gate DECISION
logic out of `TemporalCaptureMode` into a pure, D3D-free unit (`TemporalPolicy`) that compiles
and runs on any platform, so behavior is asserted as executable unit tests — the tests become
the spec. Sequenced BEFORE blend, so blend builds on a tested foundation.

## Motivation

The selection mechanisms — hysteresis (stickiness Schmitt band), the advance gate (Schmitt),
and the comb lock — are each validated empirically via Windows capture corners, but their
INTERACTIONS are not reasoned exhaustively. Blend adds a passthrough gate, breathing behavior,
and a compositor on top; built on the current entangled path, a blend bug and a
selection-interaction bug are indistinguishable. Extracting the decision logic into a pure,
tested unit: (1) isolates blend's bugs to blend's new code, (2) creates the `IFrameCompositor`
seam blend needs anyway, (3) freezes the current mechanisms as tested-known-good before blend
perturbs selection, (4) lets `relaylog.py replay` and the C++ unit share one definition of
truth. The logic has ZERO timing dependence (pure arithmetic on passed-in timestamps), so it is
fully deterministic and platform-independent — ideal for fast CI unit tests with no GPU/Windows.

## What to extract (current code → pure)

From `TemporalCaptureMode` (post-v0.0.16), the D3D-free decision logic:

1. **`WrapHalf(LONGLONG d, LONGLONG p)`** — already a pure static function. Move as-is.
2. **The comb-lock control step** — currently `UpdatePhaseLock(LONGLONG beforeDiffQpc)`, which
   mutates members `m_phasePullQpc / m_phaseErrEmaQpc / m_phaseDevEmaQpc / m_lockEngaged /
   m_phaseSeeded` using `m_combQpc / m_phasePullSlewQpc`. Becomes a pure function over an
   explicit state struct.
3. **The selection decision** — the block in `Run()` from `beforeNew/afterNew` through the
   advance gate (`m_advGateOpen`, reopen = band or 2*band) and the pick choice (stickiness bias
   `m_lastPickAfter`), producing one of the pick labels. Becomes a pure function returning a
   pick ENUM + updated selection state; the caller maps the enum to an actual surface.

What STAYS in `TemporalCaptureMode` (the D3D/timing wiring, not extracted): the present-timing
wait (timer vs vsync), `m_ring.FindBracket`, mapping the pick enum → `bracket.*Surface`,
`StretchRect`, `PresentEx`, the estimator-telemetry audit, and logging.

## Target architecture

`TemporalPolicy.h/.cpp` — no `windows.h`, no `d3d9.h`. Use `<cstdint>` `int64_t` (typedef
`LONGLONG` compatibly if convenient). Plain structs + pure functions:

```
enum class Pick { None = 0, Before = 1, After = 2, AfterAdv = 3, BeforeAdv = 4, Repeat = 5 };
// Values are FROZEN as the frame marker's 3-bit pick encoding (frame-marker-spec.md
// cells 34-36) so the marker burns (int)pick directly; note AfterAdv/BeforeAdv order.

struct BracketInfo {          // what FindBracket produced, D3D-free
    bool  hasBefore, hasAfter;
    int64_t beforeTs, afterTs;      // ring-arrival stamps
    int64_t beforeDiff, afterDiff;  // |target - ts|, both >= 0
};

struct SelectionState {       // was m_lastPickAfter / m_advGateOpen + lastShownTs
    int64_t lastShownTs;
    bool    lastPickAfter;
    bool    advGateOpen;
};

struct PhaseLockState {       // was m_phase* / m_lockEngaged / m_phaseSeeded
    int64_t pullQpc, errEmaQpc, devEmaQpc;
    bool    engaged, seeded;
};

struct PolicyConfig {         // fixed at Setup, passed in
    int64_t stickinessQpc, combQpc, phasePullSlewQpc;   // combQpc == 0 => lock off
};

// Pure. Updates lock state for the NEXT present from THIS bracket's beforeDiff.
void UpdatePhaseLock(PhaseLockState& s, const PolicyConfig& cfg, int64_t beforeDiff);

// Pure. Given the bracket at the (already pull-applied) target and current state, returns the
// pick and updates selection state. Caller fetches the surface the Pick names.
Pick SelectFrame(const BracketInfo& b, SelectionState& s, const PolicyConfig& cfg);

int64_t WrapHalf(int64_t d, int64_t p);   // moved as-is
```

`TemporalCaptureMode::Run` becomes wiring: compute `target = deadline - (lag + lock.pullQpc)`,
`FindBracket(target)` → fill `BracketInfo`, `UpdatePhaseLock`, `SelectFrame` → map `Pick` to a
`bracket.*Surface` (Repeat → `lastShownSurface`), `StretchRect`, present, log. Behavior must be
BIT-IDENTICAL to today.

## Test harness

`PolicyTests.cpp` — a console `main()` (own target, NOT in the Windows .sln): builds `Temporal
Policy` + a tiny assert (macro + failure count + non-zero exit). Drives:
- **Synthetic timelines**: generate source arrivals at a rate with configurable jitter/drops,
  present deadlines at the present rate, feed through `SelectFrame`/`UpdatePhaseLock`, assert.
- **Replayed real logs**: parse a captured `NvFBCR.log`'s before=/after= stamps (the same data
  `relaylog.py replay` uses), feed through the policy, assert the produced pick sequence matches
  the logged `pick=` (regression pin) or the expected invariant.

CI: an ubuntu job compiles `PolicyTests.cpp` + the pure sources with g++ and runs it (seconds,
no GPU/Windows). Green = invariants hold.

## Invariants to assert (tests-as-spec)

Each is a Windows-corner-validated behavior turned into a fast assertion:

- **Monotonic output**: the shown-frame timestamp (Before/BeforeAdv → beforeTs, After/AfterAdv →
  afterTs, Repeat → unchanged) is strictly non-decreasing across ANY sequence, INCLUDING a comb
  pull wrap. (This is the doorway-glitch check, made automatic.)
- **Gate excursion impossible while locked**: no `Repeat` followed within N presents by >=2
  `Before`-rides and then an `After` (the dupe+makeup signature), when the lock is engaged.
- **Pull bounded + one wrap/beat**: `pullQpc` stays in `[-comb/16, comb + comb/16]`; wraps ~once
  per beat; steady-state drift <= slew.
- **Refusal at fine ratios**: at M>=5 (e.g. 144:60) with ~300us jitter, `engaged` stays false and
  `pullQpc` ~0 (comb/8 < jitter → gate can't close).
- **Lock-off == v15**: with `combQpc == 0`, the pick sequence is bit-identical to the pre-lock
  selection on the same inputs (regression pin against v0.0.15 behavior).
- **Hysteresis holds**: at the bracket midpoint with +-300us jitter, the pick does not flip-flop
  (stride stays stable; no period-2 window).
- **Advance gate**: at the w=0 boundary with jitter, no early-advance excursion (the
  boundary-dwell class).
- **(Blend era) dropped frame → isolated blend**: a single missing source frame yields exactly
  one mid-w present classified BLEND, neighbors passthrough.

## Extraction discipline

This is a REFACTOR of production code, not additive — the risk is a behavior change during the
move. Rules: the pure function must return EXACTLY what the inline code computed (same integer
ops, same order, same truncation). Prove unchanged by (a) a Windows corner run showing identical
output, and/or (b) `relaylog.py replay` producing the same pick sequence as before on a real log.
Extract in order of math density / test value: `WrapHalf` + comb lock first, selection/gate
second. Own branch off dev (phase-comb-lock merged as v0.0.16). Keep the temporal log line
append-only (parsers depend on it).

## Blend readiness

The extracted policy layer is where blend plugs in as new TESTED policies rather than new
entangled code:
- The `Pick` enum gains blend outcomes (or a parallel compositor decision) — a passthrough-vs-
  blend function over the same `BracketInfo` + a threshold (srcP/4, see phase-comb-lock-spec.md
  clause 2), unit-tested the same way.
- `IFrameCompositor` (nearest / blend / fruc / flow) becomes the strategy seam; each compositor
  is an isolated, testable unit.
- The frame marker (frame-marker-spec.md) validates the RENDERED output; these unit tests
  validate the DECISION. Together: no gap between decided, rendered, and recorded.

## Gotchas

- `LONGLONG` vs `int64_t`: `LONGLONG` is `long long` on MSVC; typedef so the pure unit compiles
  under g++ without `windows.h`.
- The selection decision must NOT touch D3D — it returns a `Pick`; the caller owns the surface
  fetch and the `Repeat`→`lastShownSurface` rule (which lives in the wiring, since the surface
  is D3D).
- No hidden member mutation in the pure layer — all state in/out via the explicit structs, so
  tests can construct and inspect it.
- Integer division truncation must match the original exactly (the EMA `/16`, the slew, the
  wrap) — the tests pin this, but the extraction must copy the ops verbatim.
- Keep `UpdatePhaseLock` closed-loop semantics: it consumes THIS present's `beforeDiff` (measured
  at the already-pulled target) and updates the pull for the NEXT present.
