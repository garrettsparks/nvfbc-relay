#include "TemporalPolicy.h"

namespace policy {

const char* PickLabel(Pick p) {
    switch (p) {
        case Pick::Before:    return "before";
        case Pick::After:     return "after";
        case Pick::AfterAdv:  return "after-adv";
        case Pick::BeforeAdv: return "before-adv";
        case Pick::Repeat:    return "repeat";
        default:              return "none";
    }
}

// Map a tick offset into [-p/2, p/2): the signed distance to the nearest point on a
// p-periodic timeline. C++ % truncates toward zero, so negative remainders need folding up.
int64_t WrapHalf(int64_t d, int64_t p) {
    int64_t m = (d + p / 2) % p;
    if (m < 0) m += p;
    return m - p / 2;
}

// A stall must last this many presents before the resume re-seeds the pull. Above the
// normal one-dupe drift sweep, so steady-state tracking never triggers it.
static const int kStallRunPresents = 3;

// Post-resume convergence window. The re-seed measures phase from ONE bracket, picked by
// the threshold above, and that bracket's before-frame is frequently a grab-timeout
// re-grab whose timestamp is the timeout instant rather than a source render tick. The
// resulting estimate is often wrong by a few hundred to a few thousand microseconds, and
// at the steady-state slew that residual takes 50-190 presents to bleed off: seconds of
// visible blending after the source has already recovered.
//
// Moving the threshold only changes WHICH resumes draw a bad sample (measured: events
// trade places, the worst case barely moves). So rather than commit to the sample, the
// estimator runs fast for a short window afterwards and the actuator is allowed to keep
// up with it. A bad sample is then corrected within a few presents. The faster actuator
// is safe here precisely because it is paired with the faster estimator; outside the
// window both revert to steady-state values and tracking is unchanged.
static const int kRecoverPresents = 16;   // about 0.27 s at 60 Hz
static const int kRecoverAlpha    = 2;    // error-EMA divisor while recovering (16 otherwise)
static const int kRecoverSlewDiv  = 32;   // slew cap of comb/32 while recovering

// How far past a search boundary the backward flip walks keep looking before giving up.
// Both walks rely on flips on ONE head arriving in display order - measured 0 inversions over
// 400k flips, and counted at runtime by FlipHistory::OutOfOrder() rather than assumed - and
// this is margin against a violation. One 30 Hz source period: ample for any reordering a
// driver could plausibly emit, and cheap because it is a handful of extra entries either way.
//
// UNITS: microseconds, the one place in this file that is not unit-agnostic. That is
// tolerable ONLY because it is slack rather than a threshold: the QPC-tick callers (10 MHz,
// where this reads as 3.3 ms) get a tighter-but-still-ample margin instead of a wrong answer,
// and no corpus capture's output moves when the slack is removed altogether. Widening it
// costs only scan length. If it ever becomes a threshold, it has to become a parameter.
static const int64_t kReorderSlack = 33333;

void FlipHistory::Add(const Flip& f) {
    if (f.head < kMaxHeadsTracked) {
        if (m_lastTsByHead[f.head] != 0 && f.displayTs < m_lastTsByHead[f.head]) m_outOfOrder++;
        m_lastTsByHead[f.head] = f.displayTs;
    }
    if (m_count == kCapacity) {
        m_dropped++;
        m_ring[m_head] = f;
        m_head = (m_head + 1) % kCapacity;
    } else {
        m_ring[(m_head + m_count) % kCapacity] = f;
        m_count++;
    }
}

const Flip& FlipHistory::At(int i) const {
    return m_ring[(m_head + i) % kCapacity];
}

const Flip* FlipHistory::NewestAtOrBefore(int64_t ts, uint32_t head) const {
    // Newest first. Callers ask about recent times, so this normally hits within a few
    // entries instead of walking the whole history. It does NOT stop at the first match
    // by position: it keeps the best by display time, so a head whose flips ever arrive
    // out of order still yields the right answer rather than a plausible wrong one.
    const Flip* best = 0;
    for (int i = m_count - 1; i >= 0; i--) {
        const Flip& f = At(i);
        if (f.head != head) continue;
        if (f.displayTs > ts) continue;
        if (!best || f.displayTs > best->displayTs) best = &m_ring[(m_head + i) % kCapacity];
        // One full source period of older entries is ample slack to cover any reordering
        // the driver could plausibly emit; beyond that, older flips cannot win.
        if (best && f.displayTs < best->displayTs - kReorderSlack) break;
    }
    return best;
}

int FlipHistory::InRange(int64_t lo, int64_t hi, uint32_t head,
                         const Flip** out, int maxOut) const {
    int n = 0;
    for (int i = 0; i < m_count && n < maxOut; i++) {
        const Flip& f = At(i);
        if (f.head != head) continue;
        if (f.displayTs < lo || f.displayTs > hi) continue;
        out[n++] = &m_ring[(m_head + i) % kCapacity];
    }
    return n;
}

int64_t FlipHistory::MedianSpacing(int64_t lo, int64_t hi, uint32_t head) const {
    // Median, not mean: one dropped or duplicated flip would drag a mean toward a cadence
    // the display never ran at, and the whole point of reading these is to stop assuming.
    // Keeps the NEWEST kMaxSpacingSamples gaps, because a cadence that changed mid-window
    // should read as the current one rather than an average of two regimes.
    //
    // NEWEST FIRST, STOPPING EARLY, because every caller asks about a recent window: the
    // forward scan walked all 2048 retained flips to reach the ~24 gaps a 200 ms window holds
    // at 60x2. Measured: 2048 entries touched and 2008 ns a call, against 43 entries and
    // 157 ns here - 12.8x, and the scan (not the sort) was the whole cost.
    //
    // NOT a binary search, which this ring cannot support: flips are stored in ANNOUNCEMENT
    // order with heads interleaved, so displayTs across the whole ring is 17% inverted
    // (measured over 400k flips, worst backward jump 45.7 ms). What the early exit needs is
    // only PER-HEAD monotonicity - 0 inversions over the same 400k, and counted at runtime by
    // OutOfOrder() rather than assumed - because head-matching entries then appear in
    // decreasing displayTs and nothing older can qualify once one falls below the window.
    //
    // FILTERING THE HEAD BEFORE TESTING THE BOUNDARY IS LOAD-BEARING, and the corpus cannot
    // show it: every fixture is single-head (mktrace.py keeps head 0), so no amount of
    // replayed capture exercises an interleaved ring. Live it is interleaved, and a head-1
    // flip is announced beside a head-0 flip displaying up to 42.8 ms later, so a boundary
    // test that ran first would break out on a trailing head-1 entry and abandon head-0 flips
    // still inside the window. test_flip_history pins it.
    //
    // kReorderSlack, by contrast, is DEFENSIVE rather than tuned: removing it entirely leaves
    // all eight captures byte-identical, because per-head inversions do not occur. It stays
    // because it costs nothing and the failure it guards is a quietly short window rather
    // than a loud error.
    int64_t gaps[kMaxSpacingSamples];
    int n = 0;
    int64_t next = 0;
    bool haveNext = false;
    for (int i = m_count - 1; i >= 0; i--) {
        const Flip& f = At(i);
        if (f.head != head) continue;
        if (f.displayTs < lo - kReorderSlack) break;
        if (f.displayTs < lo || f.displayTs > hi) continue;
        if (haveNext && next > f.displayTs) {
            gaps[n++] = next - f.displayTs;
            if (n == kMaxSpacingSamples) break;   // the newest cap-many, same as before
        }
        next = f.displayTs;
        haveNext = true;
    }
    if (n == 0) return 0;
    int64_t sorted[kMaxSpacingSamples];
    for (int i = 0; i < n; i++) sorted[i] = gaps[i];
    for (int i = 1; i < n; i++) {          // insertion sort: n is bounded and small
        const int64_t v = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = v;
    }
    return sorted[n / 2];
}

BatchDecision UpdateBatch(BatchState& s, int64_t arrivalTs, int64_t thresholdTicks) {
    BatchDecision d;
    d.intraBatch = s.started && (arrivalTs - s.lastArrivalTs) < thresholdTicks;
    if (!d.intraBatch) {
        // Batch-start to batch-start, so the submission epsilon between a generated
        // frame and its real twin never pollutes the source-period estimate.
        if (s.started) d.batchGap = arrivalTs - s.batchStartTs;
        s.batchStartTs = arrivalTs;
    }
    // Chain: a third member an epsilon after the second is still inside the batch.
    s.member = d.intraBatch ? s.member + 1 : 0;
    s.lastArrivalTs = arrivalTs;
    s.started = true;
    d.stampTs = d.intraBatch ? s.batchStartTs : arrivalTs;
    d.retractPrevious = d.intraBatch;
    d.member = s.member;
    return d;
}

// MEASURED SPACING IS PASSED, NOT RE-DERIVED. One walk of MeasureLateness used to measure
// the grid step up to three times from identical arguments - here, in AnchorBatch, and again
// in AnchorBatch's re-acquisition call back into this function - and each one is a full scan
// of the flip ring. Measured at 2048 flips: 1693 ns a call, of which the sort is nothing (the
// scan-only InRange over the same window costs 1955 ns) because a 200 ms window holds ~24
// gaps, not the 256 the sample cap allows.
//
// The spacing is a private parameter rather than a public one on purpose. Every caller below
// is inside this file and passes a value measured from the same window on the same unmutated
// history, so the three lookups cannot disagree; a public spacing parameter would let a
// caller pass one measured elsewhere, and a wrong grid step does not fail loudly here - it
// mis-anchors, which is the exact shape of the bugs this file has already paid for (the
// hardcoded stride 2, the fixed 1 ms anchor bound, the mis-locked chain fabricating 8.3 ms
// corrections). The public entry points keep measuring it themselves.
static FlipPairing PairBatchMemberAt(const FlipHistory& h, uint32_t head, int64_t batchStartTs,
                                     int member, int64_t spacing) {
    // Wide enough to hold the anchor, this member's flip, and a flip of slack on each side.
    static const int kLookup = 16;
    FlipPairing r;
    if (member < 0 || member + 2 > kLookup) return r;
    if (spacing <= 0) return r;   // no cadence yet: not an anomaly, just no data
    // Quarter of a step: ambiguity starts at half, so this keeps a 2x margin. See the header
    // for why this is derived rather than configured.
    const int64_t maxAnchorOffset = spacing / 4;
    if (maxAnchorOffset <= 0) return r;

    // NEAREST, on either side. displayTs is a PROPOSED FUTURE scanout time and the driver
    // hands a frame to the capture API before it reaches the screen, so a wake legitimately
    // lands on either side of its own flip: measured at 60x2, 17.7% of batches arrive BEFORE
    // theirs, by a median of well under 100 us. Anchoring strictly at-or-before pushes every
    // one of those a whole grid step into the past, which is a wrong answer wearing the shape
    // of a right one.
    const Flip* buf[kLookup];
    const int64_t lo = batchStartTs - maxAnchorOffset;
    const int64_t hi = batchStartTs + maxAnchorOffset + spacing * (int64_t)(member + 1);
    const int n = h.InRange(lo, hi, head, buf, kLookup);
    if (n == 0) return r;

    int best = 0;
    int64_t bestAbs = -1;
    for (int i = 0; i < n; i++) {
        int64_t d = batchStartTs - buf[i]->displayTs;
        if (d < 0) d = -d;
        if (bestAbs < 0 || d < bestAbs) { bestAbs = d; best = i; }
    }
    // A confidence gate, not an ambiguity one: half a step would never reject anything, since
    // every instant is within half a step of some flip. Batch starts cluster hard on the grid
    // (measured 92.4% within 250 us, 99.2% within 1 ms, then a cliff), so a bound in that
    // valley rejects the fraction that genuinely cannot be placed instead of trimming a
    // continuum.
    if (bestAbs > maxAnchorOffset) return r;
    r.anchorFound = true;
    r.anchorOffset = batchStartTs - buf[best]->displayTs;

    if (best + member >= n) { r.memberAhead = true; return r; }   // not announced yet: normal
    // The member's flip must sit the right number of steps along the grid. Indexing alone
    // would silently skip a missing flip and hand back the one after it, which is a wrong
    // scanout time wearing the same shape as a right one.
    const int64_t span = buf[best + member]->displayTs - buf[best]->displayTs;
    const int64_t want = spacing * (int64_t)member;
    if (span < want - spacing / 2 || span > want + spacing / 2) { r.gridGap = true; return r; }

    r.displayTs = buf[best + member]->displayTs;
    r.paired = true;
    return r;
}

FlipPairing PairBatchMember(const FlipHistory& h, uint32_t head, int64_t batchStartTs,
                            int member, int64_t cadenceWindow) {
    if (cadenceWindow <= 0) return FlipPairing();
    // The cadence is measured, never assumed: the flip rate is the frame-generation
    // multiplier times the source rate and neither is declared here.
    return PairBatchMemberAt(h, head, batchStartTs, member,
                             h.MedianSpacing(batchStartTs - cadenceWindow, batchStartTs, head));
}

// Median of the chain's recorded batch gaps: the measured batch period. Zero until enough
// gaps exist to be a cadence rather than an anecdote.
static int64_t ChainBatchPeriod(const AnchorChain& chain) {
    if (chain.gapCount < 3) return 0;
    int64_t sorted[AnchorChain::kGapWindow];
    const int n = chain.gapCount;
    for (int i = 0; i < n; i++) sorted[i] = chain.gaps[i];
    for (int i = 1; i < n; i++) {
        const int64_t v = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = v;
    }
    return sorted[n / 2];
}

// Spacing passed rather than re-derived; see PairBatchMemberAt for why it stays private.
static FlipPairing AnchorBatchAt(const FlipHistory& h, uint32_t head, int64_t batchStartTs,
                                 AnchorChain& chain, int64_t spacing) {
    static const int kLookup = 16;
    FlipPairing r;
    if (spacing <= 0) return r;

    // Record the batch-start gap once per batch. Guarded on the batch actually changing,
    // because a dataPending retry re-offers the SAME batch and must not re-record it.
    if (batchStartTs != chain.lastSeenBatchTs) {
        if (chain.lastSeenBatchTs != 0 && batchStartTs > chain.lastSeenBatchTs) {
            chain.gaps[chain.gapHead] = batchStartTs - chain.lastSeenBatchTs;
            chain.gapHead = (chain.gapHead + 1) % AnchorChain::kGapWindow;
            if (chain.gapCount < AnchorChain::kGapWindow) chain.gapCount++;
        }
        chain.lastSeenBatchTs = batchStartTs;
    }
    const int64_t batchPeriod = ChainBatchPeriod(chain);

    if (chain.valid && batchPeriod > 0) {
        // The stride is DERIVED: flips advanced per batch = batch period / flip spacing,
        // both measured (2 at 60x2 and 60x3 where submissions pair, 1 with frame
        // generation off). How many batches this one sits past the last ANCHORED batch is
        // measured the same way, so a dropped batch doubles the predicted advance instead
        // of aliasing the prediction one flip short.
        int64_t stride = (batchPeriod + spacing / 2) / spacing;
        if (stride < 1) stride = 1;
        if (stride > 8) stride = 8;
        // KNOWN CEILING, deliberately left symmetric. The gap this divides is measured from
        // ARRIVALS, and a late batch inflates its own gap by the lateness being measured, so
        // a batch late by more than half a batch period reads as the NEXT batch arriving
        // early: the chain predicts two strides ahead, finds nothing, and reports
        // not-yet-announced. Corrections therefore reach roughly half a batch period
        // (~8.3 ms at 60x2), not the 3/4 the acceptance bound suggests.
        //
        // A quarter-period down-bias fixes the synthetic sweep at 9 ms AND REGRESSES THE
        // CORPUS: one fixture went from 0 to 10 blends ADDED, i.e. it starts mis-anchoring
        // real captures. Do not re-apply it without a fixture that proves the wider reach is
        // worth the mis-anchoring risk; the right shape is probably to disambiguate with the
        // flip grid rather than to move the rounding boundary.
        const int64_t nBatches =
            (batchStartTs - chain.lastAnchorBatchTs + batchPeriod / 2) / batchPeriod;
        if (nBatches >= 1 && nBatches <= 4) {
            const int64_t predicted = chain.lastAnchorTs + nBatches * stride * spacing;
            // An empty prediction window means one of two very different things. If the
            // history's newest flip has not REACHED the window, the data is still in
            // flight (flips are known ~1-10 ms after they happen): report
            // not-yet-announced and keep the chain, the caller retries. Only a window the
            // history has moved PAST is evidence the flip never happened.
            const Flip* newest = h.NewestAtOrBefore(INT64_MAX, head);
            if (newest && newest->displayTs < predicted + spacing / 2) {
                r.memberAhead = true;
                return r;
            }
            const Flip* buf[kLookup];
            const int n = h.InRange(predicted - spacing / 2, predicted + spacing / 2, head,
                                    buf, kLookup);
            if (n > 0) {
                int best = 0;
                int64_t bestAbs = -1;
                for (int i = 0; i < n; i++) {
                    int64_t d = predicted - buf[i]->displayTs;
                    if (d < 0) d = -d;
                    if (bestAbs < 0 || d < bestAbs) { bestAbs = d; best = i; }
                }
                // Acceptance is in BATCH periods, not flip steps, so it scales with the
                // multiplier: the measured phantom events sit near half a batch period,
                // and past three quarters a reading is indistinguishable from a
                // mis-anchored prediction. Early is bounded by a half step: arrivals lead
                // their flip by well under 100 us when they lead at all.
                const int64_t lateness = batchStartTs - buf[best]->displayTs;
                if (lateness > -spacing / 2 && lateness < batchPeriod * 3 / 4) {
                    // The constant-lateness tell (see AnchorChain::lateRun): a genuine
                    // backlog drains within a couple of batches, so a third consecutive
                    // above-gate reading means the chain slipped a flip, not that
                    // delivery is late. Re-acquire instead of accepting fiction.
                    if (lateness > spacing / 8) {
                        chain.lateRun++;
                        if (chain.lateRun >= 3) {
                            chain.valid = false;
                            chain.lateRun = 0;
                        }
                    } else {
                        chain.lateRun = 0;
                    }
                    if (chain.valid) {
                        chain.lastAnchorTs = buf[best]->displayTs;
                        chain.lastAnchorBatchTs = batchStartTs;
                        if (chain.warmup > 0) chain.warmup--;
                        r.anchorFound = true;
                        r.paired = true;
                        r.displayTs = buf[best]->displayTs;
                        r.anchorOffset = lateness;
                        r.chainWarm = (chain.warmup == 0);
                        return r;
                    }
                }
            }
            chain.valid = false;
        } else if (nBatches > 4) {
            // Five-plus batch periods with no anchor between is a stall; the chain is
            // fiction across one.
            chain.valid = false;
        }
        // nBatches == 0: an orphan wake inside the previous batch's period (a real frame
        // that slipped the collapse window). It has no grid slot of its own to predict;
        // leave the chain intact for the next real batch and report it unplaced.
        if (nBatches == 0) return r;
    }

    // Re-acquisition: the stateless nearest rule, which is only trustworthy when the batch
    // arrived on time - exactly the batches it accepts (quarter-step bound). Eight batches
    // of proven stride before corrections trust the chain; the gap window refills in
    // parallel and ChainBatchPeriod gates prediction until it has.
    const FlipPairing acq = PairBatchMemberAt(h, head, batchStartTs, 0, spacing);
    if (acq.paired) {
        chain.lastAnchorTs = acq.displayTs;
        chain.lastAnchorBatchTs = batchStartTs;
        chain.valid = true;
        chain.warmup = 8;
    }
    return acq;
}

FlipPairing AnchorBatch(const FlipHistory& h, uint32_t head, int64_t batchStartTs,
                        AnchorChain& chain, int64_t cadenceWindow) {
    if (cadenceWindow <= 0) return FlipPairing();
    return AnchorBatchAt(h, head, batchStartTs, chain,
                         h.MedianSpacing(batchStartTs - cadenceWindow, batchStartTs, head));
}

LateCorrection MeasureLateness(const FlipHistory& h, uint32_t head, int64_t batchStartTs,
                               AnchorChain& chain, int64_t cadenceWindow) {
    LateCorrection out;
    if (cadenceWindow <= 0) return out;
    // Measured ONCE for the whole walk: the anchor lookup, the re-acquisition path and the
    // late-only gate below all read the same grid step, so measuring it per-consumer bought
    // nothing but two extra scans of the flip ring.
    const int64_t spacing = h.MedianSpacing(batchStartTs - cadenceWindow, batchStartTs, head);
    const FlipPairing fp = AnchorBatchAt(h, head, batchStartTs, chain, spacing);
    if (!fp.paired) {
        out.dataPending = fp.memberAhead;
        return out;
    }
    if (!fp.chainWarm) return out;
    if (spacing > 0 && fp.anchorOffset > spacing / 8) out.correctionTicks = fp.anchorOffset;
    return out;
}

// Composition repeats once the batch stride has walked a whole number of source periods:
// period = flipsPerSource / gcd(stride, flipsPerSource). x2 (2 flips, stride 2) gives 1, so
// the mechanism is inert exactly where keep-real is already right; x3 (3, stride 2) gives 3;
// x4 (4, stride 2) gives 2. Frame generation off (1 flip per source period) gives 1.
int RotationPeriodBatches(int flipsPerSource, int stride) {
    if (flipsPerSource <= 1 || stride <= 0) return 1;
    int a = stride, b = flipsPerSource;
    while (b != 0) { const int t = a % b; a = b; b = t; }
    const int period = flipsPerSource / (a > 0 ? a : 1);
    if (period < 1) return 1;
    if (period > RotationPhase::kMaxPeriod) return 1;   // too long to vote on; stay inert
    return period;
}

void RotationReset(RotationPhase& p, int stride, int flipsPerSource) {
    p.stride = stride;
    p.flipsPerSource = flipsPerSource;
    p.period = RotationPeriodBatches(flipsPerSource, stride);
    // flipsPerSource indexes the class array, so it - not just the period - has to fit.
    // A short period can still carry a long grid: -src 15 against an x3 flip rate gives
    // flipsPerSource 12 with period 6, which would leave two thirds of the reachable
    // classes unreadable and unvoted. Stay inert instead.
    if (flipsPerSource > RotationPhase::kMaxPeriod) p.period = 1;
    for (int i = 0; i < RotationPhase::kMaxPeriod; i++) {
        p.offsetSum[i] = 0;
        p.offsetCount[i] = 0;
    }
    p.valid = false;
    p.realPhase = -1;
    p.gridPos = 0;
    p.lastAnchorTs = 0;
    p.haveAnchor = false;
}

// Every path that moves the origin funnels through here, so no caller can forget that the
// evidence goes with it.
//
// THE SAMPLES MUST BE DISCARDED, not merely distrusted. Each class sum is a mean of offsets
// observed at one grid position, and "position" only means anything relative to the origin -
// so after a re-origin the old sums describe DIFFERENT positions than the ones they are
// filed under. Keeping them does two things, both bad: the old verdict is restorable from
// evidence that no longer applies (measured at x3: the pre-stall phase returned one sample
// later, and the next 30 batches kept 0 real frames, 20 generated, dropping 10 whole - with
// nominal telemetry throughout), and even gated, the stale sums pollute the new means for as
// long as the sample window takes to wash them out. Clearing costs a re-convergence, about
// 72 batches or 0.9 s at x3, which is the right price for not steering on fiction.
static void RotationReOrigin(RotationPhase& p, int64_t anchorTs) {
    p.gridPos = 0;
    p.lastAnchorTs = anchorTs;
    p.haveAnchor = true;
    p.valid = false;
    p.realPhase = -1;
    for (int i = 0; i < RotationPhase::kMaxPeriod; i++) {
        p.offsetSum[i] = 0;
        p.offsetCount[i] = 0;
    }
}

bool RotationAdvance(RotationPhase& p, int64_t anchorTs, int flipSteps) {
    if (p.flipsPerSource <= 1) return false;
    if (!p.haveAnchor || anchorTs <= p.lastAnchorTs) {
        // First anchor after a reset or an outage: adopt it as the origin. Position is
        // arbitrary until the vote names a phase against it, which is fine - the vote and
        // the position share this origin, so only their RELATIONSHIP has to be stable.
        RotationReOrigin(p, anchorTs);
        return true;
    }
    // flipSteps is COUNTED by the caller from the flip history, never derived by dividing a
    // time difference by the spacing. That distinction decides whether this works at all: a
    // division makes a rounding decision on every batch, and one wrong rounding rotates the
    // class mapping PERMANENTLY, so the errors accumulate. Measured across five x3 captures,
    // class separation by method:
    //
    //   capture             time-division   flip-count
    //   etw_x3_walk              93 us        122 us
    //   join2_x3_on              69 us        167 us
    //   dxgk_x3_120s            222 us        211 us
    //   baseline_60x3           163 us        185 us
    //   phasekeep_60x3          114 us        225 us
    //
    // Counting also has no tolerance to tune. The division needed one, and sweeping it
    // showed the optimum wandering between 0.25 and 0.50 of a step depending on which
    // capture was measured - a parameter fitted to whichever capture came first.
    //
    // Beyond a handful of steps the batch is on the far side of a stall, an outage or a
    // stride hiccup, and nothing connects it to the last position: the samples already
    // gathered stay (they describe the same grid), but position must be re-established
    // before the vote is trusted again.
    if (flipSteps < 1 || flipSteps > 8) {
        RotationReOrigin(p, anchorTs);
        return false;
    }
    const int next = (int)(((long long)p.gridPos + flipSteps) % p.flipsPerSource);
    // THE LATTICE CHECK. A stride only ever visits positions that are multiples of
    // gcd(stride, flipsPerSource) away from where it started, and the decision loop reads
    // exactly those. An advance that lands OFF that sublattice - one duplicated or dropped
    // flip record at x4, say - would otherwise be permanent: the classes the loop reads
    // freeze at their old counts, stay above the sample floor, and hold the verdict valid
    // forever on evidence that stopped updating, while every new sample lands somewhere
    // nothing reads. Measured consequence at x4: 50% of batches dropped, 50% keeping a
    // generated frame, indefinitely. Treat it as an origin loss instead.
    int reach = p.stride > 0 ? p.stride : 1, f = p.flipsPerSource;
    while (f != 0) { const int t = reach % f; reach = f; f = t; }
    if (reach > 1 && (next % reach) != 0) {
        RotationReOrigin(p, anchorTs);
        return false;
    }
    p.gridPos = next;
    p.lastAnchorTs = anchorTs;
    return true;
}

void RotationObserve(RotationPhase& p, int64_t anchorOffset) {
    if (p.period <= 1 || !p.haveAnchor) return;
    if (p.gridPos < 0 || p.gridPos >= RotationPhase::kMaxPeriod) return;
    const int r = p.gridPos;
    p.offsetSum[r] += anchorOffset;
    p.offsetCount[r]++;

    // Bounded memory, so the vote can still change its mind. An accumulator that never
    // forgets converges once and then ignores the evidence: halving both sum and count at a
    // cap makes this an exponential window (~256 samples a class, about 8 s at x3's batch
    // rate) rather than a lifetime average, and keeps the sum away from overflow for free.
    static const int32_t kSampleCap = 256;
    if (p.offsetCount[r] >= kSampleCap) {
        p.offsetSum[r] /= 2;
        p.offsetCount[r] /= 2;
    }

    // THE DECISION IS ON SHAPE, NOT ON SPREAD, and that distinction was measured rather than
    // chosen. A batch led by a REAL frame wakes BEFORE its flip because the game submits on
    // its own render schedule; every generated-led batch wakes AFTER, on the driver's
    // metering schedule. So the signature is one class below zero and the rest above it -
    // not merely "some class is lower than the others", which noise supplies for free.
    //
    // Across ten captures (five x3 where the rotation exists, five x2 where it cannot):
    //
    //   x3  +87 +92 -30 | +83 -85 +36 | +79 +128 -84 | +91 +98 -87 | +111 +129 -96
    //   x2  -11 +48 +39 | -191 -80 -124 | -36 -180 -104 | -45 -50 -38 | -64 +38 -123
    //
    // Every x3 set has exactly one negative class and the rest positive. Four of the five
    // x2 sets do not (all-negative, or two negative), and the one that does clears its
    // runner-up by only 50 us where every x3 set clears by 117-207. A spread test alone
    // cannot separate these populations - they overlap, 12-161 us against 122-225 - which
    // is why the sign structure carries the decision and the margin only sizes it.
    // Also what makes a re-origin re-earn its verdict: RotationReOrigin clears the class
    // counts, so this floor cannot be met again until every class has been re-observed
    // against the new origin. A separate "samples since origin" counter was tried here and
    // removed - it could not be made to fail a test, because this floor already does its job.
    static const int32_t kMinSamplesPerResidue = 24;
    // Between the widest null (50 us) and the narrowest signal (117 us), in the caller's
    // units: microseconds in tests, QPC ticks live, where 800 ticks is 80 us at 10 MHz.
    static const int64_t kMinSeparation = 80;
    // ONLY THE REACHABLE POSITIONS VOTE. A batch advances `stride` flips, so starting from
    // one position it can only ever land on multiples of gcd(stride, flipsPerSource): at x3
    // that is every position, but at x4 (stride 2 of 4 flips) it is only the even ones, and
    // requiring samples in positions the stride can never visit would starve the vote
    // forever in exactly the regimes with the shortest rotations.
    int reach = p.stride > 0 ? p.stride : 1, f = p.flipsPerSource;
    while (f != 0) { const int t = reach % f; reach = f; f = t; }
    if (reach < 1) reach = 1;
    int best = -1, second = -1;
    int64_t bestMean = 0, secondMean = 0;
    for (int i = 0; i < p.flipsPerSource && i < RotationPhase::kMaxPeriod; i += reach) {
        if (p.offsetCount[i] < kMinSamplesPerResidue) { p.valid = false; return; }
        const int64_t mean = p.offsetSum[i] / p.offsetCount[i];
        if (best < 0 || mean < bestMean) {
            second = best; secondMean = bestMean;
            best = i; bestMean = mean;
        } else if (second < 0 || mean < secondMean) {
            second = i; secondMean = mean;
        }
    }
    if (best < 0 || second < 0) { p.valid = false; return; }
    // One class ahead of its flip, every other behind: the mechanism's own signature.
    const bool shaped = (bestMean < 0) && (secondMean > 0);
    if (!shaped || secondMean - bestMean < kMinSeparation) {
        p.valid = false;         // no rotation to read, or not enough evidence of one
        return;
    }
    p.valid = true;
    p.realPhase = best;
}

int RotationRealMember(const RotationPhase& p, int gridPos) {
    if (!p.valid || p.period <= 1 || p.realPhase < 0 || gridPos < 0) return -1;
    if (p.flipsPerSource <= 1) return -1;
    int m = (p.realPhase - gridPos) % p.flipsPerSource;
    if (m < 0) m += p.flipsPerSource;
    return m;
}

int RotationPositionAt(const RotationPhase& p, int64_t ts, int64_t spacingTicks) {
    if (!p.haveAnchor || p.flipsPerSource <= 1 || spacingTicks <= 0) return -1;
    const int64_t delta = ts - p.lastAnchorTs;
    // BOUNDED, because this is the one place in the design that divides a time difference by
    // the spacing - the operation every other path avoids precisely because a rounding error
    // rotates the mapping. Over the two batches it is designed to span the arithmetic is
    // exact: both ends sit within a quarter step of a real flip, against a half-step
    // rounding window. Over an unbounded gap it is not - a 300 ms unanchored run at a
    // spacing read 3% low misrounds by 2 flips on a VRR panel, silently. Past the bound this
    // reports "no position", which the caller reads as plain keep-real.
    static const int kMaxExtrapolationSteps = 8;
    if (delta < 0 || delta > (int64_t)kMaxExtrapolationSteps * spacingTicks) return -1;
    int64_t steps = (delta + (delta >= 0 ? spacingTicks / 2 : -spacingTicks / 2)) / spacingTicks;
    int64_t pos = ((long long)p.gridPos + steps) % p.flipsPerSource;
    if (pos < 0) pos += p.flipsPerSource;
    return (int)pos;
}

KeepDecision DecideKeep(const BatchDecision& batch, int realMember, int64_t spacingTicks,
                        bool havePrevSlot) {
    KeepDecision d;
    d.stampTs = batch.stampTs;
    if (realMember < 0) {
        // No rotation guidance: exactly the pre-rotation rule. Keep this member, stamp it
        // at batch start, and take back the predecessor an intra-batch wake displaced.
        d.keepThis = true;
        d.retractPrev = batch.retractPrevious && havePrevSlot;
        d.collapsed = d.retractPrev;
        return d;
    }
    // Each member on its own flip, so content and stamp agree whichever member survives.
    if (spacingTicks > 0 && batch.member > 0) {
        d.stampTs = batch.stampTs + (int64_t)batch.member * spacingTicks;
    }
    d.keepThis = (batch.member == realMember);
    // Retract the predecessor only when this member is the keeper and one is still
    // standing. A member that is itself discarded takes nothing back: the keeper may be
    // earlier in the batch and must survive.
    d.retractPrev = d.keepThis && batch.retractPrevious && havePrevSlot;
    d.collapsed = d.retractPrev || !d.keepThis;
    return d;
}

bool BracketIsStalled(const BracketInfo& b, const PolicyConfig& cfg) {
    if (!b.hasBefore || !b.hasAfter) return true;
    if (cfg.stallSpanQpc <= 0) return false;
    return (b.afterTs - b.beforeTs) > cfg.stallSpanQpc;
}

bool UpdateStallRun(PhaseLockState& s, const PolicyConfig& cfg, const BracketInfo& b) {
    if (BracketIsStalled(b, cfg)) {
        s.stallRun++;
        return false;
    }
    const bool resumed = s.stallRun >= kStallRunPresents;
    s.stallRun = 0;
    return resumed;
}

void UpdatePhaseLock(PhaseLockState& s, const PolicyConfig& cfg, int64_t beforeDiff,
                     bool resumedFromStall) {
    // Closed loop: the pull is already inside the target this error was measured at, so
    // want = pull + errEma converges instead of integrating. Error and EMAs live on the
    // circular comb domain; a linear controller here saturates against clock skew and
    // drains through a disengaged sweep every beat (measured - see the comb-lock spec).
    const int64_t err = WrapHalf(beforeDiff, cfg.combQpc);
    // Re-seed on stall-resume treats the resumed phase like a fresh acquisition: the fresh
    // err (not the /16-lagged EMA) both drives dev to zero (so the lock stays engaged
    // through the resume instead of flapping) and lets want reflect the true new phase.
    if (!s.seeded || resumedFromStall) {
        s.errEmaQpc = err;
        s.seeded = true;
        // Only a stall resume opens the window. Cold acquisition converges from a clean
        // timeline and has its own pinned behaviour.
        if (resumedFromStall) s.recoverRun = kRecoverPresents;
    } else {
        const int alpha = (s.recoverRun > 0) ? kRecoverAlpha : 16;
        s.errEmaQpc += WrapHalf(err - s.errEmaQpc, cfg.combQpc) / alpha;
        s.errEmaQpc = WrapHalf(s.errEmaQpc, cfg.combQpc);
    }
    int64_t dev = WrapHalf(err - s.errEmaQpc, cfg.combQpc);
    if (dev < 0) dev = -dev;
    s.devEmaQpc = (s.devEmaQpc * 15 + dev) / 16;

    // Stability gate: stable phase = near-rational ratio, lock possible; sweeping phase =
    // genuine rate conversion, the pull decays to zero and selection proceeds unlocked.
    // Deliberately a bare comparator, no hysteresis: every lockable ratio holds devEma far
    // below the gate, so engage/disengage chatter requires a declared -src about a percent
    // off the true rate (steady-state dev parks at the threshold). If in-regime lk flapping
    // ever shows up in real logs, give this the selection-stickiness Schmitt treatment
    // (engage below comb/8, release above comb/6) rather than tightening the -src tolerance.
    s.engaged = s.devEmaQpc < cfg.combQpc / 8;
    const int64_t want = s.engaged ? s.pullQpc + s.errEmaQpc : 0;
    int64_t delta = want - s.pullQpc;
    // Snap the full correction on an engaged stall-resume; otherwise slew-limit it so
    // steady-state tracking stays gentle (no abrupt phase jumps). A disengaged resume
    // has want == 0 and just decays the pull, so it needs no snap.
    // Inside the convergence window the actuator may move faster, matched to the faster
    // estimator above; outside it steady-state tracking is untouched.
    const int64_t slew = (s.recoverRun > 0) ? (cfg.combQpc / kRecoverSlewDiv)
                                            : cfg.phasePullSlewQpc;
    if (!(resumedFromStall && s.engaged)) {
        if (delta > slew) delta = slew;
        else if (delta < -slew) delta = -slew;
    }
    if (s.recoverRun > 0) s.recoverRun--;
    s.pullQpc += delta;

    // Wrap hysteresis: the pull may overshoot the [0, comb) domain by a band before
    // wrapping, so jitter-scale wander at the boundary cannot chatter one-frame slips.
    const int64_t band = cfg.combQpc / 16;
    if (s.pullQpc < -band) s.pullQpc += cfg.combQpc;
    else if (s.pullQpc >= cfg.combQpc + band) s.pullQpc -= cfg.combQpc;
}

Pick SelectFrame(const BracketInfo& b, SelectionState& s, const PolicyConfig& cfg) {
    // Select: nearest-to-target frame, with HYSTERESIS - present frames in strictly
    // increasing timestamp order. Among the bracket frames NEWER than the last presented
    // one, pick the nearest to target. If neither is newer (capture produced no new frame
    // this period - the genuine rate-matching stall at the drift boundary), repeat the
    // last frame: that is the one unavoidable dupe per drift sweep.
    //
    // STICKINESS BAND (Schmitt trigger): when both frames are candidates, the threshold
    // depends on which side the LAST pick took - stay on that side unless the other frame
    // is closer by more than the band. Plain nearest-pick lets capture jitter flip the
    // choice every present while the target dwells near the bracket midpoint (a visible
    // period-2 judder window every drift sweep). The state bit is what makes this
    // hysteresis: jitter must cross the full 2-band gap to flip the pick (a memoryless
    // bias just relocates the flip-flop boundary - measured, it did not shrink the
    // windows), while slow drift still crosses the gap exactly once per sweep: one clean
    // single-frame slip instead of seconds of judder.
    const bool beforeNew = b.hasBefore && b.beforeTs > s.lastShownTs;
    const bool afterNew  = b.hasAfter  && b.afterTs  > s.lastShownTs;

    // ADVANCE GATE (Schmitt): when only the after-frame is newer than the last shown,
    // advance UNLESS the target is still on the shown frame (beforeDiff inside the band).
    // The ungated advance boundary sits exactly where before == lastShown begins (w = 0);
    // the clock beat parks the target phase there periodically and arrival jitter
    // flip-flops the crossing, early-advancing a full source period each flip. Healthy
    // operating points keep beforeDiff far above the band in every regime, so the gate is
    // inert outside the crossing; the state bit widens the reopen threshold so a crossing
    // costs one clean flip. A midpoint comparison is WRONG here: matched-rate steady
    // state operates at the midpoint, and any threshold at the operating point
    // flip-flops on jitter regardless of margin (the stickiness-band lesson).
    bool advance = afterNew;
    if (advance && b.hasBefore && !beforeNew) {
        const int64_t reopen = s.advGateOpen ? cfg.stickinessQpc : 2 * cfg.stickinessQpc;
        s.advGateOpen = b.beforeDiff >= reopen;
        advance = s.advGateOpen;
    }

    if (beforeNew && afterNew) {
        const int64_t bias = s.lastPickAfter ? -cfg.stickinessQpc : cfg.stickinessQpc;
        if (b.beforeDiff <= b.afterDiff + bias) {
            s.lastShownTs = b.beforeTs;
            s.lastPickAfter = false;
            return Pick::Before;
        }
        s.lastShownTs = b.afterTs;
        s.lastPickAfter = true;
        return Pick::After;
    }
    if (advance) {
        s.lastShownTs = b.afterTs;
        s.lastPickAfter = true;
        return Pick::AfterAdv;
    }
    if (beforeNew) {
        s.lastShownTs = b.beforeTs;
        s.lastPickAfter = false;
        return Pick::BeforeAdv;
    }
    // Nothing eligible to advance to: either a genuine stall (no frame newer than the
    // last shown) or a newer after-frame the target has not reached yet. Repeat leaves
    // lastShownTs untouched; the caller re-presents the last SHOWN surface (which is not
    // always bracket.before: after an after-pick the target can still trail the shown
    // frame at sub-rate sources, leaving before one frame BEHIND the screen).
    return Pick::Repeat;
}

const char* CompositeLabel(CompositeOp op) {
    switch (op) {
        case CompositeOp::PassthroughBefore: return "pass-before";
        case CompositeOp::PassthroughAfter:  return "pass-after";
        case CompositeOp::Synthesize:        return "synth";
        default:                             return "hold";
    }
}

CompositeDecision DecideComposite(const BracketInfo& b, CompositeState& s,
                                  const PolicyConfig& cfg) {
    // PASSTHROUGH ELIGIBILITY: a real frame close enough to the target that blending
    // would only trade sharpness for sub-frame timing. The gate is a Schmitt trigger,
    // not a bare compare. Threshold placement alone covers the designed operating
    // points (locked presents sit well under it, hole/mid-gap presents well over it),
    // but an unlocked source whose clock is coherent with the present clock PARKS at
    // an arbitrary phase; parked within jitter of the threshold, a memoryless gate
    // flips on jitter tails - isolated synths in a sharp stream, one content-time
    // hitch per flip. The band is one-sided, widening only the hold-onto-passthrough
    // exit: killing the chatter loop needs a single widened edge, and widening the
    // synth side instead would make a parked phase's stable regime depend on which
    // state the startup transient happened to visit last (soft lock-in at phases
    // where the frame is genuinely inside the threshold). Passing resumes at the
    // bare threshold; it is surrendered only a full band beyond it. The band reuses
    // the side-choice stickiness, clamped for thresholds too small to hold the loop.
    const int64_t band = (cfg.stickinessQpc < cfg.passthroughQpc / 4)
                             ? cfg.stickinessQpc : cfg.passthroughQpc / 4;
    const int64_t gate = s.lastSynth ? cfg.passthroughQpc
                                     : cfg.passthroughQpc + band;
    const bool eligibleBefore = b.hasBefore && b.beforeDiff < gate;
    const bool eligibleAfter  = b.hasAfter  && b.afterDiff  < gate;

    CompositeDecision d;
    int64_t outputTs = 0;
    bool passAfter = s.lastPassAfter;
    bool synth = false;
    if (eligibleBefore && eligibleAfter) {
        // Both real frames are on target (the normal case when the source oversamples
        // the present: the bracket spans less than two thresholds). Side choice holds
        // a Schmitt band exactly like selection stickiness: bare nearest-pick lets
        // capture jitter flip the side every present while the target dwells at the
        // bracket midpoint (period-2 judder), while the band costs one clean slip
        // per sweep crossing.
        const int64_t bias = s.lastPassAfter ? -cfg.stickinessQpc : cfg.stickinessQpc;
        if (b.beforeDiff <= b.afterDiff + bias) {
            d.op = CompositeOp::PassthroughBefore;
            outputTs = b.beforeTs;
            passAfter = false;
        } else {
            d.op = CompositeOp::PassthroughAfter;
            outputTs = b.afterTs;
            passAfter = true;
        }
    } else if (eligibleBefore) {
        d.op = CompositeOp::PassthroughBefore;
        outputTs = b.beforeTs;
        passAfter = false;
    } else if (eligibleAfter) {
        d.op = CompositeOp::PassthroughAfter;
        outputTs = b.afterTs;
        passAfter = true;
    } else if (b.hasBefore && b.hasAfter) {
        // No real frame near the target but both endpoints exist: synthesize at the
        // target time (synthesis is constant-latency by construction). Holes classify
        // here with no detection needed: a dropped source frame widens the bracket,
        // the hole present reads mid-w, and neighbors are untouched.
        d.op = CompositeOp::Synthesize;
        synth = true;
        const int64_t span = b.beforeDiff + b.afterDiff;
        if (span > 0) {
            d.weight = (double)b.beforeDiff / (double)span;
        } else {
            d.weight = 0.0;
        }
        outputTs = b.beforeTs + b.beforeDiff;
    } else {
        // One-sided bracket with the lone frame off target: the after endpoint of a
        // hole has not arrived yet (recovery depth is what the lag buys) or the
        // target fell off the ring. Re-present the last output.
        return d;
    }

    // MONOTONE OUTPUT GUARD: the composite counterpart of SelectFrame's newer-than-
    // lastShown constraint. Output content time (frame ts for a passthrough, target
    // for a blend) may repeat - a pull wrap legitimately re-presents one instant per
    // beat, the same slip nearest mode pays - but never regress. Passthrough
    // quantizes output time by up to the threshold, so a threshold wider than the
    // present period (sub-quarter-rate sources) could otherwise step backward.
    if (outputTs < s.lastOutputTs) {
        d.op = CompositeOp::Hold;
        d.weight = 0.0;
        return d;
    }
    s.lastOutputTs = outputTs;
    s.lastPassAfter = passAfter;
    s.lastSynth = synth;
    return d;
}

}  // namespace policy
