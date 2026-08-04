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

// Batch-collapse memory across capture wakes. Under driver-level frame generation the
// grab wakes about twice per base frame, the pair submitted together and arriving a
// fraction of a millisecond apart while their display flips are half a base period
// apart. Collapsing the pair is what keeps the ring timeline at base cadence.
struct BatchState {
    int64_t batchStartTs = 0;    // arrival of the current batch's first member
    int64_t lastArrivalTs = 0;   // previous wake, batch member or not
    // Position of the last wake inside its batch: 0 opens a batch, 1 is the next member.
    // Needed to place a member on the flip grid, because the members of a batch arrive
    // within a submission epsilon of each other but scan out one flip apart.
    int member = 0;
    // Explicit rather than inferring "no history" from a zero timestamp: replayed and
    // simulated timelines legitimately start at 0, and a sentinel would silently treat
    // the second wake of such a run as batch-opening.
    bool started = false;
};

// What one capture wake does to the ring. stampTs is the timestamp to publish the slot
// with: the intra-batch member takes BATCH-START so the ring timeline stays at base
// cadence rather than recording the submission epsilon. batchGap is batch-start to
// batch-start (0 when this wake continues a batch or there is no history), which is the
// source-period estimate with frame-generation gaps already excluded.
struct BatchDecision {
    bool intraBatch = false;
    bool retractPrevious = false;
    int64_t stampTs = 0;
    int64_t batchGap = 0;
    // 0 for the wake that opens a batch, 1 for the next, and so on. Carried on the ring
    // slot so a later flip lookup can ask which flip this frame actually scanned out on.
    int member = 0;
};

// Fold one capture wake into the batch timeline. Wake order is measured
// [GENERATED, REAL], so the intra-batch member is the REAL frame: it publishes and the
// previous slot (the generated member) is retracted. Callers apply retractPrevious only
// when a previous slot exists.
BatchDecision UpdateBatch(BatchState& s, int64_t arrivalTs, int64_t thresholdTicks);

// A flip the display driver intends to scan out, as plain data. Deliberately free of any
// ETW or Windows type: the platform layer decodes the driver's events, this is what the
// policy is allowed to reason about, and a simulated timeline is therefore indistinguishable
// from a captured one. That is the whole point - the interesting cases (jittery capture
// wakes against an evenly paced flip grid) can be built in a test instead of hunted for in
// a capture.
struct Flip {
    int64_t displayTs = 0;   // when this frame is intended to reach the screen
    int64_t eventTs   = 0;   // when the driver announced it; displayTs minus this is lead time
    uint32_t head     = 0;   // which display
    uint32_t token    = 0;
};

// Recent flips, kept by TIME rather than by slot count, because the useful history is "the
// last few source periods" and the event rate changes with the frame-generation multiplier.
// Fixed capacity, oldest evicted first, no allocation: this is written from a callback.
//
// NOTHING IN THE POLICY READS THIS YET. It exists so the plumbing, the sizing and the tests
// are in place before any decision depends on it; wiring behaviour to a data source that has
// never been exercised is how the silent-wrong failures in this project have started.
class FlipHistory {
public:
    // 24 bytes a record, so this is ~48 KB: irrelevant beside the frame ring, and worth
    // spending to keep history well past anything the policy could want. ~17 s at 120
    // flips/s, ~11 s at 180.
    static const int kCapacity = 2048;
    // Cadence is a RECENT property, so the median samples at most this many gaps (the
    // newest ones in the window). Bounding it keeps the sort cheap and the stack small
    // however wide a window a caller asks for.
    static const int kMaxSpacingSamples = 256;

    void Clear() { m_count = 0; m_head = 0; m_dropped = 0; m_outOfOrder = 0; }
    void Add(const Flip& f);

    int Count() const { return m_count; }
    long long Dropped() const { return m_dropped; }
    // Flips whose display time went BACKWARD against the previous one on the same head.
    // Expected to be zero: the driver schedules a head in order. Counted rather than
    // assumed, because "the history is sorted" is exactly the kind of assumption that
    // would let a later binary search silently return the wrong flip.
    long long OutOfOrder() const { return m_outOfOrder; }
    // Oldest-first indexing, so i == 0 is the oldest retained flip.
    const Flip& At(int i) const;

    // Newest flip on a head at or before ts, or null when the history does not reach it.
    // Returning null rather than a nearest guess is deliberate: a caller that cannot tell
    // "no data" from "data" would silently treat a stale flip as current.
    const Flip* NewestAtOrBefore(int64_t ts, uint32_t head) const;

    // Flips on a head within [lo, hi]. Returns how many were written to out.
    int InRange(int64_t lo, int64_t hi, uint32_t head, const Flip** out, int maxOut) const;

    // Median spacing between consecutive flips on a head inside the window, or 0 when
    // fewer than two are present. The measured cadence, never an assumed one.
    int64_t MedianSpacing(int64_t lo, int64_t hi, uint32_t head) const;

private:
    Flip m_ring[kCapacity];
    int  m_count = 0;
    int  m_head = 0;          // next write position
    long long m_dropped = 0;  // evicted before anyone read them, for telemetry
    long long m_outOfOrder = 0;
    static const int kMaxHeadsTracked = 8;
    int64_t m_lastTsByHead[kMaxHeadsTracked] = {};
};

// Where one captured frame actually scanned out. Every field is meaningful on failure too:
// a caller has to be able to tell "no flip data yet" from "flip data says this is unpairable",
// because the first is the normal early-capture state and the second is a real anomaly.
struct FlipPairing {
    bool paired = false;       // displayTs is meaningful ONLY when this is true
    int64_t displayTs = 0;     // true scanout time of THIS member's frame
    int64_t anchorOffset = 0;  // batch start minus its anchor flip, signed: delivery lateness
    bool anchorFound = false;  // a flip sat near the batch start
    bool memberAhead = false;  // anchor found, but this member's flip is not announced yet
    bool gridGap = false;      // anchor found and the member's flip is missing from the grid
};

// Place a batch member on the flip grid.
//
// The batch, not the member, is what lands on a flip: members arrive within a submission
// epsilon of each other (measured 351 us) while the grid steps 5.6-8.3 ms, so every member
// of a batch has the same nearest flip and position alone would collapse them onto one
// scanout time. Measured instead: consecutive batches advance by exactly 2 flip indices
// (94.9% at 60x2 over 7348 batches, 96.4% at 60x3 over 10795) while carrying 2 members
// each, so members tile the grid one-to-one in order and member k scans out k flips after
// the anchor.
//
// The anchor is the flip NEAREST the batch start, on either side. displayTs is a proposed
// FUTURE scanout time and the driver hands a frame over before it reaches the screen, so a
// wake lands on either side of its own flip; measured at 60x2, 17.7% of batches arrive before
// theirs. maxAnchorOffset is a confidence bound, not an ambiguity one - half a step would
// never reject anything, because every instant is within half a step of some flip. Measured,
// batch starts sit within 250 us of a flip 92.4% of the time and within 1 ms 99.2% of the
// time, with a clear cliff after, so a bound near 1 ms rejects what genuinely cannot be
// placed. A large offset within the bound is late delivery, not an error, and reporting it
// is half the point of doing this at all.
FlipPairing PairBatchMember(const FlipHistory& h, uint32_t head, int64_t batchStartTs,
                            int member, int64_t maxAnchorOffset);

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
    int stallRun = 0;      // consecutive presents whose bracket carried no phase information
    int recoverRun = 0;    // presents left in the post-resume convergence window
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
    int64_t stallSpanQpc = 0;   // bracket width above which the source reads as stalled; 0 = off
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

// True when a bracket carries no usable phase information: one-sided, or spanning far
// more than a source period. The wide case is what a frozen source produces, because the
// capture API keeps re-delivering STALE frames at its grab timeout instead of starving
// the ring, so the bracket stays complete while its endpoints straddle the freeze.
bool BracketIsStalled(const BracketInfo& b, const PolicyConfig& cfg);

// Advances the stall run and reports whether THIS present is the resume: the first
// informative bracket after a stall. The distinction matters because the bracket that
// first closes after a freeze still SPANS it (before-frame captured before, after-frame
// after), so its beforeDiff describes the old phase and lands an exact multiple of the
// comb, i.e. zero error and nothing to correct. Spending the one-shot re-seed there
// leaves the pull stranded and slewing back at 25 us/present for seconds. Holding it
// until both endpoints are post-resume frames is what makes the re-seed effective.
bool UpdateStallRun(PhaseLockState& s, const PolicyConfig& cfg, const BracketInfo& b);

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
