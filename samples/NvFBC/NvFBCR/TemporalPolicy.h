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
    // AnchorBatch only: this anchor came from a chain whose stride has held since its last
    // re-acquisition long enough to be trusted for stamp corrections.
    bool chainWarm = false;
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
// wake lands on either side of its own flip; measured at 60x2, 17.7% of batches arrive
// before theirs.
//
// The confidence bound is DERIVED as a quarter of the measured grid step, not passed in.
// Ambiguity only begins at half a step, so a quarter keeps a 2x margin, and deriving it
// makes the rule scale with the frame-generation multiplier without being told about it.
// A fixed bound does not survive contact: 1 ms placed 99.2% of batches on one 60x2 capture
// and only 91.5% on another where the game's render times wobbled more and the G-SYNC grid
// followed. A quarter step placed 99.48-99.66% on all three captures measured, and doubling
// it gains 0.2%, so it sits in a valley rather than on a slope.
//
// A large offset within the bound is late delivery, not an error, and reporting it is half
// the point of doing this at all.
//
// cadenceWindow is how far back to measure the grid step, in the CALLER's time units. It is
// a parameter rather than a constant because this file is unit-agnostic by contract (QPC
// ticks in production, microseconds in tests) and a hardcoded duration silently means two
// different things: 200000 is 200 ms of microseconds but 20 ms of 10 MHz QPC ticks.
FlipPairing PairBatchMember(const FlipHistory& h, uint32_t head, int64_t batchStartTs,
                            int member, int64_t cadenceWindow);

// Anchor continuity across batches. Once one batch is anchored, the next batch's flip is
// PREDICTED rather than searched for. That matters for exactly the batches nearest-anchoring
// cannot place: a batch delivered ~0.9 of a flip step late sits nearer its successor's flip
// than its own, and the spacing/4 confidence bound around the batch start caps the readable
// offset at a quarter step - so the late deliveries that motivate measuring lateness at
// all are the ones a stateless nearest-anchor mis-places.
//
// The stride (flips advanced per batch) is DERIVED, never assumed: batch period over flip
// spacing, both measured. An earlier version hardcoded 2 - correct for the pair submission
// measured at 60x2 AND 60x3, but structurally unable to anchor a 1-member-per-batch regime
// (frame generation off), where it predicted one flip ahead of every batch and never warmed.
// Deriving it also sizes the prediction for MULTI-batch gaps: a dropped batch doubles the
// arrival gap, and predicting round(gap / batchPeriod) batches ahead prevents the one-flip
// alias where a post-gap batch is "corrected" a full period into the past.
struct AnchorChain {
    int64_t lastAnchorTs = 0;         // flip the last anchored batch landed on
    int64_t lastAnchorBatchTs = 0;    // that batch's arrival, the base for gap counting
    int64_t lastSeenBatchTs = 0;      // last batch offered, anchored or not (gap dedup)
    bool valid = false;
    // Batches remaining before the chain is trusted for CORRECTIONS. A re-acquired anchor
    // came from the stateless quarter-step rule during exactly the unsteady delivery that
    // broke the chain, so its first predictions ride an anchor of weaker provenance; the
    // stride must prove itself before stamps move on its word. Measured cost of skipping
    // this: one stall recovery grew from 50 presents to 72, because corrections landed in
    // the window where the lock was re-seeding its phase from the same stamps.
    int warmup = 0;
    // Consecutive anchored batches reading late beyond the correction gate. Genuine
    // lateness is a TRANSIENT - a delivery backlog that drains within a batch or two,
    // late-then-catch-up - because the capture API holds no standing queue. A chain that
    // has slipped one flip reads a constant +one-step lateness forever, which no physical
    // delivery can produce, so a run of them is the chain's own tell: re-acquire through
    // the stateless quarter-step rule, which only accepts on-grid batches and therefore
    // restores the true residue. Measured cost of NOT having this: a mis-locked chain
    // fabricated ~8.3 ms corrections for 11% of a steady walk and read as plausible.
    int lateRun = 0;
    // Recent batch-start gaps; their median is the batch period the stride derives from.
    // A median so the late outliers this machinery exists to measure cannot skew the very
    // cadence they are measured against.
    static const int kGapWindow = 5;
    int64_t gaps[kGapWindow] = {};
    int gapCount = 0;
    int gapHead = 0;
};

// WHICH MEMBER OF A BATCH HOLDS THE REAL FRAME, when that rotates.
//
// Under frame generation the capture wakes in epsilon-batches that advance a fixed stride
// along the flip grid, so a batch's composition depends on where it lands within a source
// period. At x2 the two coincide - every batch is [generated, real], which is why keep-real
// can simply retain member 1. At x3 the batch stride (2 flips) and the source period (3
// flips) are coprime, so composition ROTATES with period 3: [real,gen], [gen,real],
// [gen,gen]. Retaining member 1 unconditionally therefore keeps the real frame in only one
// of those, and the kept sequence runs gen, real, gen, gen, real, gen - 2 real frames in
// every 6 outputs where 4 are available.
//
// The rotation phase is READ from arrival timing, and the reading is anchored by a control
// rather than assumed. A batch led by a REAL frame wakes slightly BEFORE its flip (the game
// submits on its own render schedule): measured aoff p50 -43/-61/-105 us across three x3
// captures. A batch led by a GENERATED frame wakes just after (+76/+77/+78), on the driver's
// metering schedule. The anchor: at x2 every batch is known to be [gen,real], and five x2
// captures read +72..+74 - the gen-led signature - with nothing resembling the negative
// class. So the class that wakes early is the one led by a real frame, and the tiling fixes
// the rest (+1 rotation step = real-trailing, +2 = all-generated).
//
// ENSEMBLE, NEVER PER-BATCH: the distributions overlap badly (one class's p95 sits above
// another's p05). Only the aggregate separates, so this votes over tens of batches and the
// caller dead-reckons on the stride between re-votes. Until the vote is decisive the caller
// must fall back to plain keep-real, which is what the relay did before this existed.
struct RotationPhase {
    // Rotation length in BATCHES. 1 means composition never rotates (x2, and frame
    // generation off), and this whole mechanism stays inert. Derived, never assumed.
    static const int kMaxPeriod = 8;
    int period = 1;
    int stride = 0;           // flips a batch advances, measured
    int flipsPerSource = 0;   // flips in one source period, measured
    // WHERE IN THE SOURCE PERIOD the last observed batch anchored, 0..flipsPerSource-1.
    // This, not a batch counter, is what composition depends on - and the two drift apart
    // the moment a batch is dropped or the stride hiccups. Measured on a real x3 capture:
    // stride-2 continuity broke every ~89 batches (116 chains in 190 s), and a vote keyed
    // by batch index smeared the three classes to means 36 us apart where keying by grid
    // position separates them by 200 us. The position is carried forward by the MEASURED
    // anchor-to-anchor advance, so a dropped batch moves it by the right number of flips
    // instead of desynchronising it.
    int gridPos = 0;
    int64_t lastAnchorTs = 0;
    bool haveAnchor = false;
    int64_t offsetSum[kMaxPeriod] = {};
    int32_t offsetCount[kMaxPeriod] = {};
    bool valid = false;
    int realPhase = -1;       // grid position whose member 0 carries the real frame
};

// Rotation length for a stride over a source period, in batches: how many batches before
// composition repeats. flipsPerSource and stride are both MEASURED by the caller (source
// period and batch period over the flip spacing), so x2, x3, x4 and frame-generation-off
// all fall out of the same arithmetic instead of being special-cased.
int RotationPeriodBatches(int flipsPerSource, int stride);

// Fold one anchored batch's arrival offset into the vote. batchIndex is the ring's
// monotonic batch counter, which both threads agree on; anchorOffset is batch start minus
// its anchor flip, the same quantity stage 6 measures.

// Drop the vote. The caller MUST do this whenever the grid measurement itself changes
// (a different multiplier, a mode switch). Continuity breaks WITHIN a regime do not need
// it - RotationAdvance carries the grid position across them by measuring the advance -
// and cheap though a rebuild is, throwing the vote away every time a batch is dropped is
// what starves it: at x3 a chain runs ~89 batches and the vote needs ~72 to converge.
void RotationReset(RotationPhase& p, int stride, int flipsPerSource);

// Carry the grid position forward to a newly anchored batch, from the MEASURED advance
// since the last one. Returns false when the advance is not a recognisable number of flip
// steps (a stall, an outage, a re-acquisition), which drops the vote's confidence without
// discarding its samples; the next recognisable advance re-establishes position.
//
// This is what makes the vote survive a broken stride. Counting batches cannot: two
// batches either side of a dropped one are 4 flips apart, not 2, and a counter that does
// not know it has skipped a beat keeps voting into the wrong class from then on.
// flipSteps is the number of flip records between the previous anchor and this one, COUNTED
// from the history by the caller. Counting, never dividing a time difference by the spacing:
// a division makes a rounding decision per batch and one wrong rounding rotates the mapping
// permanently. Measured across five x3 captures, counting beat dividing on four and tied the
// fifth, and it needs no tolerance - the division's optimum tolerance wandered between 0.25
// and 0.50 of a step depending on which capture it was fitted to.
bool RotationAdvance(RotationPhase& p, int64_t anchorTs, int flipSteps);

// Fold the current batch's arrival offset into the vote, at the grid position
// RotationAdvance established. anchorOffset is batch start minus its anchor flip.
void RotationObserve(RotationPhase& p, int64_t anchorOffset);

// WHICH MEMBER of a batch anchored at gridPos carries the real frame: 0, 1, ... or a value
// past the batch's member count when that batch has NO real frame at all. -1 when the vote
// is not trustworthy, which means the caller must fall back to plain keep-real.
//
// Real frames sit at flips congruent to one phase P modulo flipsPerSource, and member m of
// a batch anchored at grid position g sits at flip g+m, so the real member is (P - g) mod
// flipsPerSource. At x3 that cycles 0, 1, 2 - and the 2 is the [gen,gen] batch, whose
// "real member" does not exist, so a caller with 2 members keeps NOTHING from it. That is
// the point: those two frames are both generated, dropping them leaves the ring holding
// exactly the real frames at exactly their own flip times, and a 60 fps output then finds
// one real frame per present instead of a 90/s mixture.
int RotationRealMember(const RotationPhase& p, int gridPos);

// The grid position this batch anchors at, for a caller that knows only a time - the
// capture thread, which must decide retraction before the batch's own flips have been
// delivered. Extrapolates from the last position the vote established, so it is exact
// while the flip step is and degrades only with the spacing estimate over a few
// milliseconds. Returns -1 when there is no position to extrapolate from.
int RotationPositionAt(const RotationPhase& p, int64_t ts, int64_t spacingTicks);

// What one capture wake does to the ring: which stamp to publish it with, whether it
// survives, and whether it takes back its predecessor.
//
// This lives here, in the layer with no windows.h and no D3D, because it is pure
// arithmetic that decides what the relay SHOWS - and while it was inlined in the capture
// loop nothing could test it. The corpus cannot: the replay models the batch timeline but
// not the ring's per-wake retraction, so a change here moves no corpus number at all.
struct KeepDecision {
    int64_t stampTs = 0;      // publish the slot with this
    bool keepThis = true;     // this member survives as a bracket candidate
    bool retractPrev = false; // hide the previous member
    bool collapsed = false;   // a member was hidden (this one or the previous), for col=
};

// realMember is which member of this batch carries the real frame, from
// RotationRealMember, or NEGATIVE when the rotation is unknown or does not exist. Negative
// is the ordinary case - x2, frame generation off, an unconverged vote - and it must behave
// exactly as keep-real always has: keep this member, stamp it at batch start, retract the
// predecessor an intra-batch wake displaces. test_keep_decision pins that exhaustively.
//
// When the rotation IS known, the batch keeps only the member named real, which may be no
// member at all (the all-generated batch at x3), and each member is stamped at its own flip
// rather than at batch start. The stamp offset matters because it varies: with a fixed kept
// member the batch-start stamp is a CONSTANT offset the comb lock absorbs, but a varying
// one is content jitter no lock can cancel.
KeepDecision DecideKeep(const BatchDecision& batch, int realMember, int64_t spacingTicks,
                        bool havePrevSlot);

// Stage-6 corrections as metadata BESIDE the ring, never as slot mutation. FindBracket
// subtracts CorrectionFor(stamp) at read time, so the ring slots keep exactly one writer
// (the capture thread) and a recycled slot can never inherit a stale correction - its
// stamp is a different key. Keyed by the batch-start stamp value, which every member of a
// batch shares, so a member published AFTER its batch was measured inherits the correction
// with no ordering requirement at all. Written and read on the present thread only.
struct StampOverlay {
    static const int kEntries = 16;   // twice the ring's batch capacity
    int64_t keys[kEntries] = {};
    int64_t corr[kEntries] = {};
    int head = 0;
    void Insert(int64_t batchStartTs, int64_t correctionTicks) {
        keys[head] = batchStartTs;
        corr[head] = correctionTicks;
        head = (head + 1) % kEntries;
    }
    int64_t CorrectionFor(int64_t stampTs) const {
        for (int i = 0; i < kEntries; i++) {
            if (keys[i] == stampTs) return corr[i];
        }
        return 0;
    }
};

// Anchor a batch on the flip grid with stride prediction, falling back to the stateless
// nearest-anchor rule (and re-acquiring the chain) when prediction finds no flip. The
// prediction window is a half step - wider than PairBatchMember's quarter step because the
// predicted position already removes the batch-start ambiguity the quarter step guards
// against, and two flip steps of real grid jitter must fit inside it. anchorOffset is
// batch start minus the anchored flip and is the batch's DELIVERY LATENESS, readable up to
// three quarters of a BATCH PERIOD (derived, so it scales with the multiplier: 12.5 ms at
// 60x2, 8.3 ms at 60x3); beyond it the reading is indistinguishable from a mis-anchored
// prediction and the chain re-acquires. displayTs is the anchor flip itself. A stall
// invalidates the chain and reports unpaired, which is the correct answer during one.
FlipPairing AnchorBatch(const FlipHistory& h, uint32_t head, int64_t batchStartTs,
                        AnchorChain& chain, int64_t cadenceWindow);

// The stage-6 decision: the delivery lateness to subtract from a batch's stamps (via the
// overlay - see StampOverlay; nothing subtracts from slots directly).
struct LateCorrection {
    // Flip data for this batch has not been delivered yet. The chain is sequential state,
    // so the caller must retry THIS batch next present rather than skipping past it.
    bool dataPending = false;
    int64_t correctionTicks = 0;   // subtract from the batch's stamps; 0 = leave them
};

// Measure a batch's delivery lateness and decide whether its stamps should move. LATE
// ONLY, an eighth of a step past the grid: lateness is physically one-sided (a frame
// cannot reach the capture API much before its flip), so a batch reading "early" past
// the gate is a mis-anchored prediction and correcting it fabricates jitter - measured
// as a passthrough-gate dwell entered 9 presents early. Corrections are also refused
// until the chain's stride has held for a warm-up after re-acquisition. The caller owns
// two further gates this layer cannot see: the coherence rule (never move a stamp across
// a target the policy has consumed) and lock calm (never move stamps while the phase
// lock is riding out or converging from a stall).
LateCorrection MeasureLateness(const FlipHistory& h, uint32_t head, int64_t batchStartTs,
                               AnchorChain& chain, int64_t cadenceWindow);

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
