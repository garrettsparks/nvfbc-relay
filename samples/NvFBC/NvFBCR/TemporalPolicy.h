#pragma once

#include <cstdint>

// Pure temporal-selection policy (design: docs/policy-extraction-spec.md). The complete
// DECISION logic of the temporal mode - nearest-frame selection with hysteresis, the
// advance gate, the comb-lock control step, and the blend-mode composite decision - as
// plain arithmetic over explicit state structs. No windows.h, no d3d9.h, no clocks:
// fully deterministic and compiles anywhere. TemporalCaptureMode owns the wiring
// (timing waits, ring, surfaces, present, logging) and consumes this layer; nearest
// behavior is identical to the pre-extraction inline code by construction (ops copied
// verbatim), and blend mode adds DecideComposite alongside without touching it.

namespace policy {

// Values are FROZEN: they are the frame marker's 3-bit pick encoding (cells 34-36,
// docs/frame-marker-spec.md) burned into every -mark recording.
enum class Pick : int {
    None      = 0,
    Before    = 1,
    After     = 2,
    AfterAdv  = 3,   // only the after-frame was newer and the advance gate passed
    BeforeAdv = 4,
    Repeat    = 5,   // nothing newer than last shown: re-present it
};

static_assert((int)Pick::None == 0 && (int)Pick::Before == 1 && (int)Pick::After == 2 &&
              (int)Pick::AfterAdv == 3 && (int)Pick::BeforeAdv == 4 && (int)Pick::Repeat == 5,
              "Pick values are the frozen frame-marker pick encoding (cells 34-36): "
              "recordings already exist, so new outcomes may only append");

// The log's stable pick= label for each value (the temporal line's pick= field is a
// frozen format consumed by offline analysis).
const char* PickLabel(Pick p);

// What FindBracket produced, D3D-free. Diffs are distances from the (already
// pull-applied) selection target: beforeDiff = target - beforeTs, afterDiff =
// afterTs - target, both >= 0 when the side exists.
struct BracketInfo {
    bool hasBefore = false;
    bool hasAfter = false;
    int64_t beforeTs = 0;
    int64_t afterTs = 0;
    int64_t beforeDiff = 0;
    int64_t afterDiff = 0;
};

// Selection memory across presents. lastShownTs strictly advances except on Repeat;
// the two bools are the Schmitt states (stickiness side, advance gate).
struct SelectionState {
    int64_t lastShownTs = 0;
    bool lastPickAfter = false;
    bool advGateOpen = true;
};

// Comb-lock control state. pull is the extra lag holding the target on the comb.
struct PhaseLockState {
    int64_t pullQpc = 0;
    int64_t errEmaQpc = 0;
    int64_t devEmaQpc = 0;
    bool engaged = false;
    bool seeded = false;
};

// Fixed at Setup. combQpc == 0 disables the lock entirely (selection then equals the
// pre-lock v0.0.15 behavior). passthroughQpc is the blend-mode passthrough threshold
// (read only by DecideComposite; nearest selection never sees it). Units are whatever
// the caller's timestamps use; the policy is unit-agnostic (QPC ticks in production,
// microseconds in tests/replay).
struct PolicyConfig {
    int64_t stickinessQpc = 0;
    int64_t combQpc = 0;
    int64_t phasePullSlewQpc = 0;
    int64_t passthroughQpc = 0;
};

// The signed distance to the nearest point on a p-periodic timeline, in [-p/2, p/2).
int64_t WrapHalf(int64_t d, int64_t p);

// One comb-lock step, closed-loop: consumes THIS present's beforeDiff (measured at the
// already-pulled target, so the pull is inside the error) and updates the pull for the
// NEXT present. EMA filter, stability gate, symmetric bounded slew, wrap modulo the
// comb behind a hysteresis band. Call only with a complete bracket (both sides): the
// pull freezes across gaps rather than integrating a one-sided error.
// resumedFromStall: this present is the first complete bracket after a multi-present source
// stall (map open/close, alt-tab). The frozen pull is now far from the resumed phase, so
// snap the correction in one present (re-seed the error EMA, apply the full delta unclamped)
// instead of crawling back at the steady-state slew. Perceptually free: the snap lands on the
// stall's own content discontinuity.
void UpdatePhaseLock(PhaseLockState& s, const PolicyConfig& cfg, int64_t beforeDiff,
                     bool resumedFromStall = false);

// The per-present selection decision: nearest-to-target among frames NEWER than the
// last shown (monotonic output), stickiness Schmitt band at the bracket midpoint,
// advance gate in the afterNew-only arm. Updates state; the caller maps the Pick to a
// surface (Repeat means the last shown surface, which only the caller holds).
Pick SelectFrame(const BracketInfo& b, SelectionState& s, const PolicyConfig& cfg);

// ---------------------------------------------------------------------------------
// Blend-mode composite decision.
// Runs ALONGSIDE SelectFrame, never on top of it: blend mode calls DecideComposite
// and nearest mode calls SelectFrame, sharing only the bracket and the lock. Layering
// the composite on SelectFrame's pick was rejected because SelectFrame's monotonic
// lastShownTs advances on a blend present too, which reclassifies the recovery
// present after every hole as Repeat (a forced re-present of the synthesized frame:
// one output dupe per hole, where a real frame sits at the target and should pass
// through sharp). Values are NOT marker-encoded (provenance rides the marker's
// interp-flag and compositor-ID cells); the log's op= labels are append-only once
// shipped.
// ---------------------------------------------------------------------------------

enum class CompositeOp : int {
    Hold = 0,               // nothing presentable at the target: re-present last output
    PassthroughBefore = 1,  // the real before-frame is on target: present it sharp
    PassthroughAfter = 2,
    Synthesize = 3,         // no real frame near the target: make one there from the
                            // bracket pair at the bracket weight (lerp, flow warp - the
                            // executor is the compositor's business, not the policy's)
};

// The log's stable op= label for each value.
const char* CompositeLabel(CompositeOp op);

struct CompositeDecision {
    CompositeOp op = CompositeOp::Hold;
    double weight = 0.0;    // synthesis weight; meaningful only when op == Synthesize
};

// Composite memory across presents. lastOutputTs is the content time of the last
// composed output (frame timestamp for a passthrough, target for a blend); INT64_MIN
// until the first output so early sim/replay timelines with negative timestamps
// cannot false-Hold. lastPassAfter is the passthrough side-choice Schmitt state;
// lastSynth is the pass/synth gate Schmitt state (see DecideComposite).
struct CompositeState {
    int64_t lastOutputTs = INT64_MIN;
    bool lastPassAfter = false;
    bool lastSynth = false;
};

// The per-present composite decision for blend mode. A real frame within the
// passthrough gate of the target is presented sharp (PASSTHROUGH). The gate is
// cfg.passthroughQpc held in a ONE-SIDED stickiness Schmitt band (passing is
// surrendered only a full band beyond the threshold, and resumes at the bare
// threshold): an unlocked source whose clock is coherent with the present clock
// parks at an arbitrary phase, and parked within jitter of a bare threshold the
// decision flips on jitter tails - isolated synths in a sharp stream, one timing
// hitch per flip. With both bracket frames inside the gate the side choice holds
// its own band, because bare nearest-pick lets capture jitter flip the side every
// present while the target dwells at the bracket midpoint, and when the source
// oversamples the present both frames are always eligible, making that dwell
// permanent.
// Otherwise a complete bracket SYNTHESIZES at the bracket weight; an incomplete one HOLDS
// (the hole-recovery depth contract: a hole present interpolates only once its after
// endpoint has arrived). A candidate whose output time would regress on the last
// output (pull wraps, pathological ratios) demotes to Hold: output content time is
// non-decreasing by construction, the composite counterpart of SelectFrame's
// monotonic lastShownTs.
CompositeDecision DecideComposite(const BracketInfo& b, CompositeState& s,
                                  const PolicyConfig& cfg);

}  // namespace policy
