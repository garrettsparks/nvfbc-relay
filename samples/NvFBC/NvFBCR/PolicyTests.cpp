// Tests-as-spec for TemporalPolicy (docs/policy-extraction-spec.md). Console target,
// deliberately NOT in the Windows solution: pure C++17, no GPU, no timing dependence.
//
//   g++ -std=c++17 -O2 -o policytests PolicyTests.cpp TemporalPolicy.cpp
//   ./policytests                       run the synthetic invariant suite
//   ./policytests --replay NvFBCR.log [--comb <us>] [--passthrough <us>]
//                                       pin the policy against a real log: nearest-mode
//                                       logs replay the pick= sequence, blend/interp
//                                       logs replay op=/bw= (threshold self-configures
//                                       from the Setup line; --passthrough overrides),
//                                       and --comb adds the pull=/lk= lock pin
//
// All synthetic tests run in the microsecond domain; the policy is unit-agnostic.

#include "TemporalPolicy.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <string>
#include <vector>

using policy::BracketInfo;
using policy::PhaseLockState;
using policy::Pick;
using policy::PolicyConfig;
using policy::SelectionState;

static int g_failures = 0;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            g_failures++;                                             \
            std::printf("FAIL %s:%d: ", __func__, __LINE__);          \
            std::printf(__VA_ARGS__);                                 \
            std::printf("\n");                                        \
        }                                                             \
    } while (0)

// The microsecond-domain test convention for the policy config: production sizes these
// from the QPC frequency (Freq()/1000 and Freq()/40000); every synthetic suite and the
// replay must run the same convention or the suite validates an inconsistent config.
static const int64_t kStickinessUs = 1000;
static const int64_t kSlewUs = 25;
// The rotation vote's class-mean margin, in this suite's microseconds. Production derives
// the same 80 us from its QPC frequency. It is a PARAMETER precisely because this suite runs
// in a different clock from the relay: as a literal inside the vote it meant 80 us here and
// 8 us in the field, so the suite could not see the field's real null gate at all.
static const int64_t kVoteMinSeparationUs = 80;

// The ring depth the CORPUS FIXTURES were captured under - deliberately NOT mirroring the
// shipping depth: every fixture's field numbers came from ring-8 builds, and the replay
// must model the relay that produced them. It is the DEFAULT for SimParams::ringSlots, not
// a global: the shipping relay allocates 16 and sizes up to 32 when the lag asks for it, so
// a suite that could only run 8 would never exercise the depth the daily driver uses.
// Fixtures captured on deeper builds would warrant revisiting this default.
static const int kRingSlots = 8;

// Deterministic LCG so every run exercises identical timelines. Every suite RESEEDS it
// (SeedRng, called from Simulate and from the suites that draw directly), so suites are
// independent of each other and of their order in main(). Without that the stream position
// leaks between suites and the census pins below become constants of the CALL ORDER as much
// as of the policy: appending a suite silently shifts every pin after it.
static const uint64_t kRngSeed = 0x2545F4914F6CDD1Dull;
static uint64_t g_rng = kRngSeed;
static void SeedRng() { g_rng = kRngSeed; }
static int64_t JitterUs(int64_t amplitude) {
    g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull;
    if (amplitude == 0) return 0;
    return (int64_t)((g_rng >> 33) % (2 * amplitude + 1)) - amplitude;
}

// ---------------------------------------------------------------------------------
// Synthetic timeline simulator: source arrivals at srcPeriod (+jitter), present
// deadlines at presentPeriod, static lag sized as production does. Mirrors the Run()
// wiring: pulled target -> bracket -> UpdatePhaseLock (complete brackets only) ->
// SelectFrame. Records everything the invariants need.
// ---------------------------------------------------------------------------------

struct SimResult {
    std::vector<Pick> picks;
    std::vector<int64_t> shownTs;     // after each present (Repeat carries forward)
    std::vector<int64_t> pull;        // lock pull after each present's update
    std::vector<bool> engaged;
    std::vector<int64_t> beforeDiff;  // at the pulled target (when hasBefore)
    std::vector<size_t> wrapAt;       // present indices where the pull wrapped
    int wraps = 0;
    std::vector<bool> snapped;        // presents the lock treated as a stall resume
    std::vector<int64_t> span;        // bracket width (afterTs - beforeTs), -1 if one-sided
    // Composite decision alongside (recorded when passthroughQpc > 0).
    std::vector<policy::CompositeOp> ops;
    std::vector<double> weights;
    std::vector<int64_t> outTs;       // composite output content time (Hold carries forward)
    std::vector<int64_t> minDiff;     // nearest-real-frame distance at the target (-1 if none)
    // Stage 6 telemetry, mirroring production's dejit counters one for one so the corpus
    // gate and a live capture's summary line are directly comparable.
    long long measuredBatches = 0;
    long long lateBatches = 0;
    long long correctedBatches = 0;
    long long fenceBlockedBatches = 0;
    long long lockDeclinedBatches = 0;
    long long anchoredBatches = 0;
};

struct SimParams {
    int64_t srcPeriod;
    int64_t presentPeriod;
    int64_t arrivalJitter;
    int64_t combQpc;         // 0 = lock off
    int64_t presents;
    int64_t phaseOffset;     // shifts arrival phase vs deadlines
    int64_t passthroughQpc;  // 0 = composite decision off
    int64_t lagOverride;     // 0 = size from srcPeriod as production does; else this lag
    std::vector<int64_t> drops;  // sorted arrival indices to drop (hole injection)
    // A source stall, as the ring actually sees one: from arrival index stallAtArrival,
    // stallArrivals arrivals come stallGap apart instead of srcPeriod. This is the shape a
    // frozen game produces, because NvFBC's grab times out and re-delivers STALE content
    // rather than starving the ring: the bracket stays COMPLETE and merely grows WIDE.
    // Note a stall alone is often phase-neutral: 200 ms of frozen 60 fps source is exactly
    // 12 frame periods, so the comb phase comes back where it left. What actually strands
    // the pull is the source resuming on a NEW phase, because the game restarts its frame
    // clock after the hitch. postStallPhase is that jump, applied once at the resume.
    int64_t stallAtArrival = -1;
    int64_t stallArrivals = 0;
    int64_t stallGap = 0;
    int64_t postStallPhase = 0;
    // Non-empty: arrival gaps cycle this list instead of srcPeriod (display-quantized
    // cadences, e.g. a fixed-refresh 240 Hz panel flipping a 90 fps source 2-3-3);
    // srcPeriod stays the DECLARED rate that sizes lag/threshold/comb.
    std::vector<int64_t> periodPattern;
    // Non-empty: replay these arrival times verbatim instead of synthesizing a cadence.
    // A generated timeline can only contain the shapes someone thought to model; a
    // captured one carries whatever the hardware actually did, including transients
    // nobody has characterized. srcPeriod still sizes lag, threshold and comb.
    std::vector<int64_t> explicitArrivals;
    // Non-empty: use these present deadlines instead of a uniform grid. The relay's
    // present clock is DWM's, which skews against the source clock by tens of us per
    // present; the comb lock exists to track exactly that, so a synthetic grid removes
    // the phenomenon under test.
    std::vector<int64_t> explicitPresents;
    // Batch-collapse window. Production sizes this at 3 ms; frame generation submits a
    // pair well inside it and no real content cadence produces gaps that short.
    int64_t batchThresholdQpc = 3000;
    // Stage 6: subtract each batch's measured DELIVERY LATENESS (batch start minus its
    // stride-anchored flip) from its slots' stamps, under the coherence rule. The timeline
    // stays on the arrival base - on-time batches are left alone entirely - so the lock
    // phase the ring settled on is untouched; only the late outliers that force phantom
    // blends move, back onto the grid where their frames were actually rendered.
    // flipDisplay/flipKnown are the fixture's head-0 scanout stream in delivery order (see
    // TraceFixture); empty leaves the feature inert. flipDejitter arms it.
    std::vector<int64_t> flipDisplay;
    std::vector<int64_t> flipKnown;
    bool flipDejitter = false;
    // Slots the ring searches. Defaults to the corpus depth so every pinned census is
    // unmoved; set it to run a timeline at the depth a given configuration ships with.
    int ringSlots = kRingSlots;
    // Disable the composite tooth guard (production arms it whenever the comb is on and
    // the source is at or above the SINK rate). Only for differential tests that
    // reproduce the pre-guard decision rule.
    bool noToothGuard = false;
    // The SINK period: the target display's refresh, which is what production compares the
    // source against when arming the guard. presentPeriod above is the tick spacing the
    // present loop actually runs at - DWM's compose clock under a vsync present (twice the
    // sink under in-game frame generation, four times it on a composed desktop), or the
    // declared rate under a timer present. The two coincide only at 1x.
    int64_t sinkPeriod = 16667;
};

static SimResult Simulate(const SimParams& p) {
    SeedRng();   // each simulation owns its jitter stream; see kRngSeed
    PolicyConfig cfg;
    cfg.stickinessQpc = kStickinessUs;
    cfg.combQpc = p.combQpc;
    cfg.phasePullSlewQpc = kSlewUs;
    cfg.passthroughQpc = p.passthroughQpc;
    cfg.stallSpanQpc = p.srcPeriod * 2;   // production sizes this from the declared source rate
    // The tooth guard arms by production's own rule (comb on, source at or above the
    // OUTPUT rate) rather than by a test knob, so every composite test in this file
    // exercises the guarded decision path and a pin failure is the guard changing a
    // regime it was proven not to change. noToothGuard reproduces the pre-guard rule.
    if (!p.noToothGuard) {
        cfg.srcPeriodQpc =
            policy::ToothGuardPeriod(p.srcPeriod, p.sinkPeriod, cfg.combQpc > 0);
    }

    int64_t lag = p.srcPeriod + p.srcPeriod / 4;
    if (lag < p.presentPeriod) lag = p.presentPeriod;
    if (p.lagOverride > 0) lag = p.lagOverride;   // a mis-declared source rate, in effect

    // Pre-generate arrivals covering the whole run. Drops are filtered after
    // generation so the jitter stream (and every surviving arrival) is identical
    // with and without hole injection.
    std::vector<int64_t> arrivals;
    const int64_t horizon = (p.presents + 4) * p.presentPeriod;
    int64_t stallShift = 0;   // accumulated delay the stall pushes onto later arrivals
    if (!p.explicitArrivals.empty()) arrivals = p.explicitArrivals;
    for (int64_t t = p.phaseOffset, i = 0; p.explicitArrivals.empty() && t < horizon; i++) {
        arrivals.push_back(t + JitterUs(p.arrivalJitter));
        const bool inStall = p.stallAtArrival >= 0 && i >= p.stallAtArrival &&
                             i < p.stallAtArrival + p.stallArrivals;
        if (inStall) stallShift += p.stallGap - p.srcPeriod;
        if (p.stallAtArrival >= 0 && i == p.stallAtArrival + p.stallArrivals - 1) {
            stallShift += p.postStallPhase;
        }
        if (p.periodPattern.empty()) {
            t = p.phaseOffset + (i + 1) * p.srcPeriod + stallShift;
        } else {
            // The stall overrides the cadence rather than riding on top of it: NvFBC's
            // grab times out and re-delivers stale content at the timeout period no
            // matter what pattern the source was producing before the freeze. Without
            // this the pattern branch silently ignores the stall entirely, so a stall
            // and a non-uniform cadence cannot be simulated together.
            int64_t step = p.periodPattern[(size_t)(i % (int64_t)p.periodPattern.size())];
            if (inStall) step = p.stallGap;
            if (p.stallAtArrival >= 0 && i == p.stallAtArrival + p.stallArrivals - 1) {
                step += p.postStallPhase;
            }
            t += step;
        }
    }
    if (!p.drops.empty()) {
        std::vector<int64_t> kept;
        size_t di = 0;
        for (size_t i = 0; i < arrivals.size(); i++) {
            if (di < p.drops.size() && (int64_t)i == p.drops[di]) { di++; continue; }
            kept.push_back(arrivals[i]);
        }
        arrivals.swap(kept);
    }

    // Fold the wake list through the REAL batch-collapse before any bracket sees it.
    // The ring publishes one slot per wake and retracts the generated member, so the
    // visible timeline is stamps[] filtered by valid[], not the raw wake list. Modelling
    // the wakes directly is what made an earlier replay disagree with the hardware.
    std::vector<int64_t> stamps(arrivals.size(), 0);
    std::vector<char> valid(arrivals.size(), 1);
    std::vector<int64_t> batchStart(arrivals.size(), 0);
    std::vector<int> member(arrivals.size(), 0);
    {
        policy::BatchState bs;
        for (size_t i = 0; i < arrivals.size(); i++) {
            const policy::BatchDecision bd =
                policy::UpdateBatch(bs, arrivals[i], p.batchThresholdQpc);
            stamps[i] = bd.stampTs;
            batchStart[i] = bd.stampTs;   // batch-start IS the pre-upgrade stamp, both members
            member[i] = bd.member;
            if (bd.retractPrevious && i >= 1) valid[i - 1] = 0;
        }
    }
    // Stage 6 state, mirroring production's wiring exactly: flips enter history when they
    // became KNOWN (never when they happened), batches are walked strictly in arrival
    // order (the chain is sequential state), corrections live in the same StampOverlay
    // production uses and are applied at bracket-read time, never by mutating stamps. An
    // earlier version of this block mutated stamps[] behind a growing-batch guard the
    // production side also had; an adversarial review caught the two sides modelling
    // DIFFERENT timings, which made the gate meaningless. The overlay design has no
    // settlement timing to diverge on: a correction is keyed by the batch-start stamp, so
    // members published after their batch was measured inherit it by lookup.
    const bool flipDejitter = p.flipDejitter && !p.flipDisplay.empty() && !p.flipKnown.empty();
    policy::FlipHistory fliph;
    policy::AnchorChain chain;
    policy::StampOverlay overlay;
    size_t nextFlip = 0;
    int64_t maxTarget = INT64_MIN;
    const int64_t kCadenceWindowUs = 200000;   // production's m_flipCadenceWindowQpc
    // First slot index of each batch, in arrival order (a batch is a maximal run of equal
    // batchStart) - the sim's stand-in for the ring's batch-start history.
    std::vector<size_t> batchFirst;
    for (size_t i = 0; i < arrivals.size(); i++) {
        if (i == 0 || batchStart[i] != batchStart[i - 1]) batchFirst.push_back(i);
    }
    size_t nextBatch = 0;

    SelectionState sel;
    PhaseLockState lock;
    policy::CompositeState comp;
    SimResult r;
    size_t published = 0;
    int64_t prevPull = 0;
    const int64_t presentCount =
        p.explicitPresents.empty() ? p.presents : (int64_t)p.explicitPresents.size();
    for (int64_t k = 1; k <= presentCount; k++) {
        const int64_t deadline = p.explicitPresents.empty()
                                     ? k * p.presentPeriod
                                     : p.explicitPresents[(size_t)(k - 1)];
        const int64_t target = deadline - (lag + lock.pullQpc);

        // Frames visible to the bracket: arrived by pick time (the ring can't contain
        // the future) AND still inside the ring window (each publish evicts the slot
        // p.ringSlots back). Both constraints are inert for well-configured timelines -
        // the lag sizing keeps the bracket well inside the window - but load-bearing
        // for hole recovery depth (a widened bracket's after endpoint may not have
        // arrived yet: those presents must hold, not synthesize) and for underruns
        // (a lag past the ring window loses the before-frame entirely).
        while (published < arrivals.size() && arrivals[published] <= deadline) published++;
        const size_t oldest =
            published > (size_t)p.ringSlots ? published - (size_t)p.ringSlots : 0;

        // Stage 6, the same sequence production runs before its bracket read: measure each
        // batch's delivery lateness against its stride-anchored flip, gate on lock calm
        // and THE COHERENCE RULE (both the batch stamp and its corrected value strictly
        // newer than the newest target the policy has seen), insert survivors into the
        // overlay. A blocked batch keeps today's late stamp - that is the entire
        // degradation path, no separate fallback exists.
        if (flipDejitter) {
            if (target > maxTarget) maxTarget = target;
            while (nextFlip < p.flipDisplay.size() && p.flipKnown[nextFlip] <= deadline) {
                policy::Flip f;
                f.displayTs = p.flipDisplay[nextFlip];
                f.eventTs = p.flipKnown[nextFlip];
                f.head = 0;
                fliph.Add(f);
                nextFlip++;
            }
            while (nextBatch < batchFirst.size() &&
                   arrivals[batchFirst[nextBatch]] <= deadline) {
                const int64_t bs = batchStart[batchFirst[nextBatch]];
                const policy::LateCorrection lc =
                    policy::MeasureLateness(fliph, 0, bs, chain, kCadenceWindowUs);
                // Data in flight: stop and retry this batch next present (the chain is
                // order-dependent, so later batches wait behind it).
                if (lc.dataPending) break;
                r.measuredBatches++;
                if (chain.valid) r.anchoredBatches++;
                const bool lockCalm = (lock.stallRun == 0 && lock.recoverRun == 0);
                if (lc.correctionTicks != 0) {
                    r.lateBatches++;
                    if (!lockCalm) {
                        r.lockDeclinedBatches++;
                    } else if (bs <= maxTarget || bs - lc.correctionTicks <= maxTarget) {
                        r.fenceBlockedBatches++;
                    } else {
                        overlay.Insert(bs, lc.correctionTicks);
                        r.correctedBatches++;
                    }
                }
                nextBatch++;
            }
        }

        // Nearest valid slot on each side of the target, which is what production's
        // ring scan returns. Retracted slots still occupy their ring position (the
        // write counter advanced), so the window is p.ringSlots WAKES wide while only
        // the surviving members are eligible. When everything visible is newer than
        // the target there is no before-frame and the nearest-after is the oldest
        // visible slot, the same result the production scan gives while the display
        // is pinned at the ring's tail.
        BracketInfo b;
        for (size_t i = oldest; i < published; i++) {
            if (!valid[i]) continue;
            int64_t ts = stamps[i];
            if (flipDejitter) ts -= overlay.CorrectionFor(ts);
            if (ts <= target) {
                if (!b.hasBefore || ts > b.beforeTs) { b.hasBefore = true; b.beforeTs = ts; }
            } else {
                if (!b.hasAfter || ts < b.afterTs) { b.hasAfter = true; b.afterTs = ts; }
            }
        }
        if (b.hasBefore) b.beforeDiff = target - b.beforeTs;
        if (b.hasAfter) b.afterDiff = b.afterTs - target;

        // Mirrors Run()'s lock wiring, INCLUDING the stall-run counter that arms the
        // re-seed: the trigger is part of the behavior under test, so the simulator has
        // to drive it the same way production does rather than assume the flag.
        bool resumedFromStall = false;
        if (cfg.combQpc > 0) {
            resumedFromStall = policy::UpdateStallRun(lock, cfg, b);
            if (!policy::BracketIsStalled(b, cfg)) {
                policy::UpdatePhaseLock(lock, cfg, b.beforeDiff, resumedFromStall);
                if (lock.pullQpc - prevPull > cfg.combQpc / 2 ||
                    prevPull - lock.pullQpc > cfg.combQpc / 2) {
                    r.wraps++;
                    r.wrapAt.push_back((size_t)(k - 1));
                }
                prevPull = lock.pullQpc;
            }
        }
        r.snapped.push_back(resumedFromStall);
        r.span.push_back((b.hasBefore && b.hasAfter) ? (b.afterTs - b.beforeTs) : -1);

        r.picks.push_back(policy::SelectFrame(b, sel, cfg));
        r.shownTs.push_back(sel.lastShownTs);
        r.pull.push_back(lock.pullQpc);
        r.engaged.push_back(lock.engaged);
        r.beforeDiff.push_back(b.hasBefore ? b.beforeDiff : -1);

        int64_t md = -1;
        if (b.hasBefore) md = b.beforeDiff;
        if (b.hasAfter && (md < 0 || b.afterDiff < md)) md = b.afterDiff;
        r.minDiff.push_back(md);
        if (cfg.passthroughQpc > 0) {
            const policy::CompositeDecision cd = policy::DecideComposite(b, comp, cfg);
            r.ops.push_back(cd.op);
            r.weights.push_back(cd.weight);
            r.outTs.push_back(comp.lastOutputTs);
        }
    }
    return r;
}

static bool IsPass(policy::CompositeOp op) {
    return op == policy::CompositeOp::PassthroughBefore ||
           op == policy::CompositeOp::PassthroughAfter;
}

// Gate-excursion signature: a Repeat whose recovery jumps the shown timestamp by well
// over one source period (the dupe+makeup pair the advance gate exists to prevent).
static int CountExcursions(const SimResult& r, int64_t srcPeriod,
                           const std::vector<bool>* requireEngaged = nullptr) {
    // A pull wrap legitimately produces one Repeat plus a comb-spacing jump (the beat
    // slip, the same slip an unlocked beat pays); relaylog.py classifies it separately
    // from the gate-excursion class. Exclude the wrap neighborhood.
    // Skip startup: lastShownTs seeds at 0, so the first real pick is a giant jump by
    // construction (and the lock gate starts optimistically engaged on zero history).
    int count = 0;
    for (size_t i = 100; i + 3 < r.picks.size(); i++) {
        if (r.picks[i] != Pick::Repeat) continue;
        if (requireEngaged && !(*requireEngaged)[i]) continue;
        bool nearWrap = false;
        for (size_t w : r.wrapAt) {
            if (i + 10 >= w && i <= w + 10) { nearWrap = true; break; }
        }
        if (nearWrap) continue;
        for (size_t j = i + 1; j <= i + 3 && j < r.picks.size(); j++) {
            if (r.shownTs[j] - r.shownTs[i] >= srcPeriod + srcPeriod / 2) {
                count++;
                break;
            }
        }
    }
    return count;
}

// ---------------------------------------------------------------------------------
// Invariant tests
// ---------------------------------------------------------------------------------

// v0.0.15 selection, transcribed independently: the regression pin proving the
// extracted SelectFrame is the same decision procedure when the lock is off.
static Pick SelectFrameV15Ref(const BracketInfo& b, SelectionState& s, int64_t band) {
    const bool beforeNew = b.hasBefore && b.beforeTs > s.lastShownTs;
    const bool afterNew = b.hasAfter && b.afterTs > s.lastShownTs;
    bool advance = afterNew;
    if (advance && b.hasBefore && !beforeNew) {
        const int64_t reopen = s.advGateOpen ? band : 2 * band;
        s.advGateOpen = b.beforeDiff >= reopen;
        advance = s.advGateOpen;
    }
    if (beforeNew && afterNew) {
        const int64_t bias = s.lastPickAfter ? -band : band;
        if (b.beforeDiff <= b.afterDiff + bias) {
            s.lastShownTs = b.beforeTs; s.lastPickAfter = false; return Pick::Before;
        }
        s.lastShownTs = b.afterTs; s.lastPickAfter = true; return Pick::After;
    }
    if (advance) { s.lastShownTs = b.afterTs; s.lastPickAfter = true; return Pick::AfterAdv; }
    if (beforeNew) { s.lastShownTs = b.beforeTs; s.lastPickAfter = false; return Pick::BeforeAdv; }
    return Pick::Repeat;
}

static void test_lock_off_matches_v15() {
    // Differential over jittered mismatched-rate brackets: every pick and every state
    // field identical to the v15 reference when combQpc == 0.
    SeedRng();   // draws jitter directly rather than through Simulate
    PolicyConfig cfg;
    cfg.stickinessQpc = kStickinessUs;
    SelectionState a, b;
    int64_t src = 16672, present = 16667, t = 0, arrival = 0;
    for (int k = 0; k < 200000; k++) {
        t += present;
        while (arrival + src <= t) arrival += src;
        BracketInfo br;
        br.hasBefore = true;
        br.beforeTs = arrival + JitterUs(300);
        br.beforeDiff = t - br.beforeTs;
        if (br.beforeDiff < 0) br.beforeDiff = 0;
        br.hasAfter = (k % 97) != 0;   // occasional one-sided brackets
        br.afterTs = arrival + src + JitterUs(300);
        br.afterDiff = br.afterTs - t;
        if (br.afterDiff < 0) br.afterDiff = 0;
        const Pick pa = policy::SelectFrame(br, a, cfg);
        const Pick pb = SelectFrameV15Ref(br, b, cfg.stickinessQpc);
        CHECK(pa == pb, "pick diverged at k=%d (%d vs %d)", k, (int)pa, (int)pb);
        CHECK(a.lastShownTs == b.lastShownTs && a.lastPickAfter == b.lastPickAfter &&
              a.advGateOpen == b.advGateOpen, "state diverged at k=%d", k);
        if (g_failures) return;
    }
}

static void test_monotonic_and_pull_bounds_across_wrap() {
    // 59.98-vs-60.00-style mismatch, lock on at M=1: the pull ramps and wraps roughly
    // once per beat. Output timestamps must never step backward, wraps included, and
    // the pull must respect its wrap-hysteresis bounds every present.
    SimParams p{};
    p.srcPeriod = 16672;
    p.presentPeriod = 16667;
    p.arrivalJitter = 300;
    p.combQpc = 16672;
    p.presents = 12000;   // ~200 s
    SimResult r = Simulate(p);
    for (size_t i = 1; i < r.shownTs.size(); i++) {
        CHECK(r.shownTs[i] >= r.shownTs[i - 1],
              "shown ts stepped back at present %zu (%" PRId64 " -> %" PRId64 ")",
              i, r.shownTs[i - 1], r.shownTs[i]);
        if (g_failures) return;
    }
    const int64_t band = p.combQpc / 16;
    for (size_t i = 0; i < r.pull.size(); i++) {
        CHECK(r.pull[i] >= -band && r.pull[i] < p.combQpc + band,
              "pull %" PRId64 " out of bounds at present %zu", r.pull[i], i);
        if (g_failures) return;
    }
    // drift ~5 us/present -> one comb traversal per ~55 s -> 2-5 wraps in 200 s
    CHECK(r.wraps >= 1 && r.wraps <= 6, "expected 1-6 wraps per 200 s beat, got %d", r.wraps);
}

static void test_refusal_at_fine_ratio() {
    // 144:60 (M=5): comb/8 = 173 us sits under the ~300 us arrival jitter, so the
    // stability gate must refuse and the pull must stay near zero.
    // Refusal requires the phase noise to exceed the gate: comb/8 = 173 us, so model
    // arrival jitter whose mean absolute deviation (~300 us at uniform +-600) clears it.
    SimParams p{};
    p.srcPeriod = 6944;
    p.presentPeriod = 16667;
    p.arrivalJitter = 600;
    p.combQpc = 6944 / 5;
    p.presents = 6000;
    SimResult r = Simulate(p);
    int engagedCount = 0;
    for (size_t i = 0; i < r.engaged.size(); i++) {
        if (r.engaged[i]) engagedCount++;
    }
    CHECK(engagedCount < (int)(r.engaged.size() / 20),
          "lock engaged %d/%zu presents at M=5 (must refuse)", engagedCount, r.engaged.size());
}

static void test_hysteresis_no_flip_flop() {
    // 240:60 with the target sweeping through bracket midpoints. Judder is measured on
    // STRIDES (steps of the shown-frame chain), not pick labels: at the alignment phase
    // the same shown frame legitimately relabels between Before and After (bd~0 and
    // ad~0 select the same arrival), so labels alternate while the output is steady.
    // The validated invariant (stride.py, v0.0.11): >=99% stride-4, off-strides are
    // isolated lone slips at sweep crossings, and no period-2 alternation window.
    SimParams p{};
    p.srcPeriod = 4167;
    p.presentPeriod = 16667;
    p.arrivalJitter = 150;
    p.combQpc = 0;
    p.presents = 7200;   // ~120 s, ~2 sweeps at 1 us/present drift
    SimResult r = Simulate(p);
    int off = 0, altWindow = 0, worstAltWindow = 0, strides = 0;
    int prevStride = 4;
    for (size_t i = 1; i < r.shownTs.size(); i++) {
        const int64_t delta = r.shownTs[i] - r.shownTs[i - 1];
        if (delta == 0) continue;   // repeat carries forward
        const int s = (int)((delta + p.srcPeriod / 2) / p.srcPeriod);
        strides++;
        if (s != 4) {
            off++;
            if (prevStride != 4 && s != prevStride) {
                altWindow++;
                if (altWindow > worstAltWindow) worstAltWindow = altWindow;
            }
        } else {
            altWindow = 0;
        }
        prevStride = s;
    }
    CHECK(off <= strides / 100, "%d/%d off-strides (>1%%)", off, strides);
    CHECK(worstAltWindow <= 1, "period-2 stride window of %d alternations", worstAltWindow);
}

static void test_advance_gate_no_excursion() {
    // Matched rates with the target parked at the w=0 boundary (phase offset makes
    // beforeDiff ~ 0 +- jitter): the gate must hold the boundary to isolated repeats,
    // never the repeat-then-double-advance makeup pair.
    SimParams p{};
    p.srcPeriod = 16667;
    p.presentPeriod = 16667;
    p.arrivalJitter = 300;
    p.combQpc = 0;
    p.presents = 7200;
    p.phaseOffset = -(16667 + 16667 / 4);   // arrivals land on the unpulled target
    SimResult r = Simulate(p);
    const int excursions = CountExcursions(r, p.srcPeriod);
    CHECK(excursions <= 3, "%d gate excursions while parked at w=0", excursions);
}

static void test_no_excursion_while_locked() {
    // With the comb lock engaged the selection target sits on the comb, so the
    // boundary-dwell class is structurally unreachable: zero excursions while engaged.
    SimParams p{};
    p.srcPeriod = 16672;
    p.presentPeriod = 16667;
    p.arrivalJitter = 300;
    p.combQpc = 16672;
    p.presents = 24000;   // ~400 s, several beats
    SimResult r = Simulate(p);
    const int excursions = CountExcursions(r, p.srcPeriod, &r.engaged);
    CHECK(excursions == 0, "%d gate excursions while lock engaged", excursions);
}

// Re-seed vs slew after a source stall. A stall freezes the pull; if the phase parked mid-gap
// on resume, the steady-state slew crawls the pull back over ~a hundred presents (the "couple
// seconds of blend" seen after a map open/close), while the stall-resume re-seed snaps it in
// one. This reproduces the slow recovery with the flag off, then confirms the fix with it on -
// no game-specific tuning: any coherent source (source period == present period, so the phase
// parks) whose big drop leaves the phase mid-gap. Oversampled sources recover instantly either
// way, so this is the regime where the recovery cost actually shows.
static void test_lock_reseed_recovery() {
    PolicyConfig cfg;
    cfg.stickinessQpc = kStickinessUs;
    cfg.combQpc = 16667;              // 60 fps source, M=1
    cfg.phasePullSlewQpc = kSlewUs;   // 25 us/present
    cfg.passthroughQpc = 4166;
    const int64_t comb = cfg.combQpc;
    // The present target parks a half-comb past the nearest source frame (deepest blend); the
    // resulting beforeDiff is a pure function of the pull the lock still has to work off.
    const int64_t parkedPhase = comb / 2;
    auto beforeDiffAt = [&](int64_t pull) -> int64_t {
        return ((parkedPhase - pull) % comb + comb) % comb;
    };
    auto passing = [&](int64_t pull) -> bool {
        const int64_t bd = beforeDiffAt(pull);
        const int64_t d = bd < comb - bd ? bd : comb - bd;   // distance to the nearest frame
        return d < cfg.passthroughQpc;                       // a real frame back on target
    };
    auto settledLock = []() {
        PhaseLockState s;
        s.seeded = true; s.engaged = true; s.devEmaQpc = 0; s.errEmaQpc = 0; s.pullQpc = 0;
        return s;
    };

    int slewPresents = 0;
    for (PhaseLockState s = settledLock(); !passing(s.pullQpc) && slewPresents < 10000; slewPresents++)
        policy::UpdatePhaseLock(s, cfg, beforeDiffAt(s.pullQpc), /*resumedFromStall=*/false);

    int snapPresents = 0;
    for (PhaseLockState s = settledLock(); !passing(s.pullQpc) && snapPresents < 10000; snapPresents++)
        policy::UpdatePhaseLock(s, cfg, beforeDiffAt(s.pullQpc), /*resumedFromStall=*/snapPresents == 0);

    std::printf("  reseed recovery: slew=%d presents (~%.2fs of blend) vs snap=%d present\n",
                slewPresents, slewPresents * 16667.0 / 1e6, snapPresents);
    CHECK(slewPresents > 100, "un-fixed slew recovered in %d presents (expected the slow >100)", slewPresents);
    CHECK(snapPresents <= 3, "re-seed recovered in %d presents (expected <= 3)", snapPresents);
    CHECK((int64_t)snapPresents * 20 < slewPresents,
          "re-seed (%d) not >=20x faster than slew (%d)", snapPresents, slewPresents);
}

// A real source stall, as the ring sees it. A frozen game does NOT starve the ring:
// NvFBC's grab times out and returns the SAME frame, so arrivals keep landing at the
// timeout period and the bracket stays COMPLETE, just wide. The re-seed trigger counts
// consecutive INCOMPLETE brackets, so it never arms here and the pull crawls back at
// the steady-state slew, blending the whole way. Measured in the field at ~8% of
// stalls, up to 175 presents (~2.9 s) of continuous synth.
static void test_lock_reseed_wide_bracket_stall() {
    SimParams p;
    p.srcPeriod = 16667;
    p.presentPeriod = 16667;
    p.arrivalJitter = 0;
    p.combQpc = 16667;
    p.presents = 700;
    p.phaseOffset = 0;
    p.passthroughQpc = 4166;
    p.lagOverride = 0;
    p.stallAtArrival = 300;      // let the lock settle first
    p.stallArrivals = 2;         // 2 timeout re-grabs = ~200 ms frozen
    p.stallGap = 100000;         // NvFBC's grab timeout
    p.postStallPhase = 8333;     // the game resumes half a comb away
    const SimResult r = Simulate(p);

    // The stall region is everything abnormal: a wide bracket or a one-sided one.
    size_t stallEnd = 0;
    int wide = 0, oneSided = 0;
    for (size_t i = 0; i < r.span.size(); i++) {
        const bool abnormal = r.span[i] < 0 || r.span[i] > p.srcPeriod * 2;
        if (abnormal) { stallEnd = i; }
        if (r.span[i] > p.srcPeriod * 2) wide++;
        if (r.span[i] < 0) oneSided++;
    }
    CHECK(wide > 0, "no wide bracket produced: the stall was not simulated");

    int snaps = 0, snapApplied = 0;
    for (size_t i = 0; i < r.snapped.size(); i++) {
        if (!r.snapped[i]) continue;
        snaps++;
        // A snap that actually moved the pull shows up as a step past the slew clamp.
        if (i > 0 && (r.pull[i] > r.pull[i-1] ? r.pull[i]-r.pull[i-1] : r.pull[i-1]-r.pull[i]) > kSlewUs) snapApplied++;
    }
    int synthAfter = 0;
    for (size_t i = stallEnd + 1; i < r.ops.size() && i <= stallEnd + 200; i++) {
        if (r.ops[i] == policy::CompositeOp::Synthesize) synthAfter++;
    }
    std::printf("  wide-bracket stall: %d wide, %d one-sided, re-seed armed %d / applied %d, "
                "%d synth in the 200 presents after\n",
                wide, oneSided, snaps, snapApplied, synthAfter);
    for (size_t i = 0; i < r.snapped.size(); i++) {
        if (!r.snapped[i]) continue;
        std::printf("    armed at present %zu: span=%" PRId64 " beforeDiff=%" PRId64
                    " pull %" PRId64 " -> %" PRId64 " engaged=%d\n",
                    i, r.span[i], r.beforeDiff[i], i ? r.pull[i - 1] : 0, r.pull[i],
                    (int)r.engaged[i]);
    }

    // Arming the re-seed is not enough: the snap is gated on the lock being engaged at
    // that instant, and devEma only decays 15/16 per present after a stall drives it up.
    CHECK(snaps > 0, "lock never detected the stall resume (no re-seed armed)");
    CHECK(snapApplied > 0, "re-seed armed but the pull never moved past the slew clamp");
    CHECK(synthAfter <= 10,
          "slow recovery: %d synth presents after the stall (expected <= 10)", synthAfter);
}

// The flip history the platform layer will feed from the display driver's scanout events.
// Tested here rather than only in a capture because the entire reason it is a plain
// structure with no ETW types is so the awkward cases can be BUILT: a capture timeline
// whose wakes jitter badly while the flip grid underneath stays evenly paced is the
// scenario that motivates reading flips at all, and it is nearly impossible to provoke on
// demand with a game.
static void test_flip_history() {
    policy::FlipHistory h;
    CHECK(h.Count() == 0, "a fresh history is empty");
    CHECK(h.NewestAtOrBefore(1000, 0) == NULL, "empty history must report no data, not a guess");
    CHECK(h.MedianSpacing(0, 1000000, 0) == 0, "no cadence can be derived from nothing");

    // Two heads at once, which is what the hardware actually produces: the source display
    // running frame generation at twice the base rate, and the relay's own output.
    const int64_t kSrc = 16667, kHalf = 8333;
    for (int i = 0; i < 40; i++) {
        policy::Flip a;
        a.displayTs = 100000 + (int64_t)i * kHalf;   // 120/s on head 0
        a.eventTs   = a.displayTs - 5000;
        a.head = 0;
        a.token = (uint32_t)i;
        h.Add(a);
        if (i % 2 == 0) {
            policy::Flip b;
            b.displayTs = 100000 + (int64_t)(i / 2) * kSrc;   // 60/s on head 1
            b.eventTs   = b.displayTs - 5000;
            b.head = 1;
            h.Add(b);
        }
    }
    CHECK(h.Count() == 60, "40 flips on one head and 20 on the other, got %d", h.Count());

    // Cadence is MEASURED per head, never assumed from a multiplier.
    CHECK(h.MedianSpacing(0, 10000000, 0) == kHalf,
          "head 0 cadence should be the half period %lld, got %lld",
          (long long)kHalf, (long long)h.MedianSpacing(0, 10000000, 0));
    CHECK(h.MedianSpacing(0, 10000000, 1) == kSrc,
          "head 1 cadence should be the source period %lld, got %lld",
          (long long)kSrc, (long long)h.MedianSpacing(0, 10000000, 1));

    // The cadence walk runs NEWEST-FIRST and stops early, so it depends on flips of one head
    // arriving in display order - which is a property of the ring, not an assumption the
    // walk may make for free. These pin the two halves of that.
    //
    // (a) A WINDOW ENTIRELY IN THE PAST STILL RESOLVES. This is the one that bites: the walk
    // starts at the NEWEST flip, so entries above the window must be skipped on their way
    // down while only entries below it end the walk. A walk that broke on either side reads
    // an empty window for every historical query - mutation-tested, it fails 38 assertions
    // here and changes the replayed output of all eight captures.
    CHECK(h.MedianSpacing(100000, 100000 + 10 * kHalf, 0) == kHalf,
          "an old window must measure its own cadence, got %lld",
          (long long)h.MedianSpacing(100000, 100000 + 10 * kHalf, 0));

    // (b) THE HEAD FILTER MUST PRECEDE THE EARLY EXIT, and only a unit test can say so: every
    // corpus fixture is SINGLE-HEAD (mktrace.py keeps head 0 only), so replaying captures
    // cannot exercise this no matter how many hours of them there are. Live, the ring is
    // interleaved, and a head-1 flip is announced beside a head-0 flip whose display time is
    // up to 42.8 ms newer (measured over 200k flips; median 2.9 ms). So a walk that tested
    // the window boundary BEFORE the head would break out on a head-1 entry that is merely
    // trailing, abandoning head-0 flips it still needs - and would report a cadence built
    // from too few samples, or none.
    //
    // The 45 ms lag and the narrow window below are the combination that makes the mistake
    // visible: a wide window hides it, because dropping the oldest one or two gaps leaves the
    // median unmoved. That is exactly why this must be pinned rather than reasoned about.
    policy::FlipHistory heads;
    const int64_t kBase = 400000, kLag = 45000;
    for (int i = 0; i < 20; i++) {
        policy::Flip a;
        a.displayTs = kBase + (int64_t)i * kHalf;
        a.eventTs = a.displayTs - 5000;
        a.head = 0;
        heads.Add(a);
        policy::Flip b;                      // announced next, but displaying well earlier
        b.displayTs = a.displayTs - kLag;
        b.eventTs = b.displayTs - 5000;
        b.head = 1;
        heads.Add(b);
    }
    const int64_t kNewest = kBase + 19 * kHalf;
    CHECK(heads.MedianSpacing(kNewest - 2 * kHalf, kNewest, 0) == kHalf,
          "a narrow head-0 window must measure %lld across interleaved head-1 flips that "
          "trail it past the reorder slack, got %lld",
          (long long)kHalf, (long long)heads.MedianSpacing(kNewest - 2 * kHalf, kNewest, 0));

    // (c) A REORDERED FLIP MUST NOT MOVE THE CADENCE. Reordering is also COUNTED rather than
    // assumed away - see the OutOfOrder checks further down.
    policy::FlipHistory rewound;
    for (int i = 0; i < 8; i++) {
        policy::Flip a;
        a.displayTs = 100000 + (int64_t)i * kHalf;
        a.eventTs = a.displayTs - 5000;
        a.head = 0;
        rewound.Add(a);
    }
    policy::Flip back;
    back.displayTs = 100000 + 3 * kHalf;   // backwards against the newest
    back.eventTs = back.displayTs - 5000;
    back.head = 0;
    rewound.Add(back);
    CHECK(rewound.OutOfOrder() == 1, "a backward flip must be counted, got %lld",
          rewound.OutOfOrder());
    CHECK(rewound.MedianSpacing(100000, 100000 + 8 * kHalf, 0) == kHalf,
          "a single reordered flip must not change the measured cadence, got %lld",
          (long long)rewound.MedianSpacing(100000, 100000 + 8 * kHalf, 0));

    // Lookup must not leak one head's timeline into the other's.
    const policy::Flip* f = h.NewestAtOrBefore(100000 + 5 * kHalf + 10, 0);
    CHECK(f && f->displayTs == 100000 + 5 * kHalf, "wrong flip for head 0");
    f = h.NewestAtOrBefore(100000 + 5 * kHalf + 10, 1);
    CHECK(f && f->displayTs == 100000 + 2 * kSrc, "head 1 lookup must not return a head 0 flip");
    CHECK(h.NewestAtOrBefore(99999, 0) == NULL, "a time before all history has no flip");

    const policy::Flip* got[8];
    const int n = h.InRange(100000, 100000 + 3 * kHalf, 0, got, 8);
    CHECK(n == 4, "four head-0 flips in the first three half periods, got %d", n);

    // Eviction is oldest-first and counted: silently losing history is how an estimator
    // ends up confidently wrong about a cadence it can no longer see.
    policy::FlipHistory small;
    for (int i = 0; i < policy::FlipHistory::kCapacity + 25; i++) {
        policy::Flip x;
        x.displayTs = (int64_t)i * 1000;
        x.head = 0;
        small.Add(x);
    }
    CHECK(small.Count() == policy::FlipHistory::kCapacity, "history must cap at its capacity");
    CHECK(small.Dropped() == 25, "evictions must be counted, got %lld", small.Dropped());
    CHECK(small.At(0).displayTs == 25 * 1000, "oldest retained flip should be the 26th added");
    CHECK(h.OutOfOrder() == 0, "an in-order feed must report no reordering, got %lld",
          h.OutOfOrder());
    // A driver that ever emits a head out of order must be visible, not silently absorbed:
    // MedianSpacing's newest-first walk stops early on the assumption that a head's flips
    // arrive in display order, so this counter is how a violation becomes findable rather
    // than showing up as a quietly short cadence window. (Ordering holds only PER HEAD - the
    // ring interleaves heads and is heavily inverted overall, which is why no lookup here
    // may binary search it.)
    policy::FlipHistory ooo;
    policy::Flip p1; p1.displayTs = 2000; p1.head = 0; ooo.Add(p1);
    policy::Flip p2; p2.displayTs = 1000; p2.head = 0; ooo.Add(p2);
    CHECK(ooo.OutOfOrder() == 1, "a backward display time must be counted");
    const policy::Flip* q = ooo.NewestAtOrBefore(3000, 0);
    CHECK(q && q->displayTs == 2000,
          "lookup must return the newest by DISPLAY time, not the last inserted");
    std::printf("  flip history: per-head cadence, lookup isolation, counted eviction, "
                "reorder-safe lookup\n");
}

// Placing captures on the flip grid. The cases that matter are the FAILURES: a pairing
// that cannot be made must say so, because a confident wrong scanout time is worse than
// none at all - the estimated stamp it would replace is at least honest about being an
// estimate.
// The anchor chain's derived stride and its failure tells, on synthetic grids where the
// truth is known exactly. Three regimes the corpus (all 60x2 KCD) cannot gate: a 1-member-
// per-batch cadence (frame generation off), a dropped batch (the one-flip alias trap), and
// a chain deliberately mis-locked one flip (the constant-lateness tell).
static void test_anchor_chain() {
    const int64_t kWindow = 200000;

    // A generic regime builder: flips every `grid`, batches every `batchPeriod`, batch
    // starts sitting `offset` past their flip. Feeds the chain the way the walk does and
    // returns the last verdict.
    struct Runner {
        policy::FlipHistory h;
        policy::AnchorChain chain;
        int64_t grid, base;
        Runner(int64_t g) : grid(g), base(1000000) {
            for (int i = 0; i < 200; i++) {
                policy::Flip f;
                f.displayTs = base + (int64_t)i * g;
                f.eventTs = f.displayTs + 1300;
                f.head = 0;
                h.Add(f);
            }
        }
        policy::LateCorrection Offer(int64_t bs) {
            return policy::MeasureLateness(h, 0, bs, chain, 200000);
        }
    };
    (void)kWindow;

    // FG off: stride must derive to 1. Batches on-grid, then one late by 5 ms: the chain
    // has to warm up (8 accepted predictions past re-acquisition + 3 gaps) and then read
    // the lateness. The hardcoded-stride-2 version of this chain could never warm here.
    {
        Runner r(16667);
        policy::LateCorrection last;
        for (int i = 2; i < 30; i++) {
            last = r.Offer(r.base + (int64_t)i * 16667 + 80);
            CHECK(!last.dataPending, "x1 steady offer %d should not be pending", i);
            CHECK(last.correctionTicks == 0, "x1 on-grid batch %d must not correct (got %lld)",
                  i, (long long)last.correctionTicks);
        }
        CHECK(r.chain.valid, "x1 chain should be locked after a steady run");
        last = r.Offer(r.base + 30 * 16667 + 5000);
        CHECK(last.correctionTicks >= 4500 && last.correctionTicks <= 5500,
              "x1 late batch should read ~5 ms of lateness, got %lld",
              (long long)last.correctionTicks);
    }

    // x2 with a dropped batch: the gap doubles, the prediction must advance FOUR flips,
    // and the post-gap on-time batch must NOT read a full period of fictional lateness
    // (the alias the derived nBatches exists to prevent).
    {
        Runner r(8333);
        for (int i = 2; i < 30; i++) {
            (void)r.Offer(r.base + (int64_t)i * 16666 + 80);
        }
        CHECK(r.chain.valid, "x2 chain should be locked");
        // Batch 30 dropped; batch 31 arrives on time.
        const policy::LateCorrection lc = r.Offer(r.base + 31 * 16666 + 80);
        CHECK(!lc.dataPending && lc.correctionTicks == 0,
              "post-gap on-time batch must not correct (alias), got pending=%d corr=%lld",
              (int)lc.dataPending, (long long)lc.correctionTicks);
        CHECK(r.chain.valid, "a single dropped batch must not break the chain");
    }

    // The constant-lateness tell: force the chain one flip behind, then feed on-grid
    // batches. Without the tell it reads +one-step lateness forever and corrects
    // everything; with it, the run of identical readings kills the chain and the
    // re-acquisition (quarter-step, on-grid only) restores the true residue.
    {
        Runner r(8333);
        for (int i = 2; i < 30; i++) {
            (void)r.Offer(r.base + (int64_t)i * 16666 + 80);
        }
        r.chain.lastAnchorTs -= 8333;   // the mis-lock, injected
        long long corrected = 0;
        for (int i = 30; i < 44; i++) {
            const policy::LateCorrection lc = r.Offer(r.base + (int64_t)i * 16666 + 80);
            if (lc.correctionTicks != 0) corrected++;
        }
        CHECK(corrected <= 2,
              "a mis-locked chain corrected %lld on-grid batches; the constant-lateness "
              "tell should have re-acquired within 3", corrected);
        const policy::LateCorrection lc = r.Offer(r.base + 44 * 16666 + 80);
        CHECK(!lc.dataPending && lc.correctionTicks == 0 && r.chain.valid,
              "chain should be re-locked on the true residue after the tell fired");
    }

    std::printf("  anchor chain: derived stride (x1 lock + lateness), dropped-batch alias "
                "guard, mis-lock tell + re-acquisition\n");
}

// Stage 6 end to end on a timeline where the truth is CONSTRUCTED: a steady 60x2 source
// whose flips sit exactly on an 8.33 ms grid, with chosen batches handed over late. A frame
// rendered on time but delivered late is precisely that - its arrival moves, its FLIP does
// not - so the correction has a ground truth to be measured against, which no capture can
// provide (the live run gave exactly one such event, and only its existence, not its size).
//
// Sweeping the lateness answers the question captures cannot: how late must a delivery be
// before correcting it changes an output decision? Below the passthrough gate the ring is
// still picking the same frame either way, so the honest answer is expected to be "several
// milliseconds", and a regression that silently narrows the correction's reach shows up
// here as a sweep row going quiet.
// The rotation-phase vote: does it find the real-led class, refuse when there is nothing to
// find, and stay silent until it is sure? Built from the MEASURED signature (real-led
// batches wake ~-50 us before their flip, generated-led ~+75 us after, with distributions
// that overlap heavily), because the whole point is that no single batch is decisive.
// What one wake does to the ring. THE FIRST HALF IS AN EQUIVALENCE PROOF, not a behaviour
// test: with no rotation guidance this must do exactly what the capture loop did before any
// rotation code existed, and that is checked by exhausting every input combination rather
// than by reading the code and reasoning about it.
//
// Why exhaustion: the default path is the daily driver, the corpus cannot see this decision
// at all (the replay models the batch timeline, not the ring's per-wake retraction), and the
// rule it replaced was three lines that had been correct for months. A restructure that
// silently altered one case would show up as soft frames in x2 output and nothing else.
static void test_keep_decision() {
    const int64_t kBase = 1000000, kSpacing = 5556;

    // The rule as it stood before the rotation existed, written out independently so the
    // comparison is against a statement of the old behaviour rather than against a
    // refactoring of the new code.
    struct Old { int64_t stamp; bool keep; bool retract; };
    auto oldRule = [&](const policy::BatchDecision& b, bool havePrev) {
        Old o;
        o.stamp = b.stampTs;                        // batch start, every member
        o.keep = true;                              // this member always survives
        o.retract = b.retractPrevious && havePrev;  // intra-batch wake displaces its predecessor
        return o;
    };

    // THE x2 CONTRACT: spacing 0 (composition cannot rotate - the caller zeroes it whenever
    // period is 1, which is every x2 and FG-off configuration). Every combination must match
    // the pre-rotation rule exactly.
    int cases = 0;
    for (int member = 0; member <= 2; member++) {
        for (int intra = 0; intra <= 1; intra++) {
            for (int havePrev = 0; havePrev <= 1; havePrev++) {
                policy::BatchDecision b;
                b.member = member;
                b.intraBatch = (intra != 0);
                b.retractPrevious = (intra != 0);
                b.stampTs = kBase;
                const policy::KeepDecision got =
                    policy::DecideKeep(b, /*realMember=*/-1, 0, havePrev != 0);
                const Old want = oldRule(b, havePrev != 0);
                CHECK(got.stampTs == want.stamp,
                      "no-rotation stamp must be batch start (member %d): got %lld want %lld",
                      member, (long long)got.stampTs, (long long)want.stamp);
                CHECK(got.keepThis == want.keep,
                      "no-rotation must keep every member (member %d)", member);
                CHECK(got.retractPrev == want.retract,
                      "no-rotation retraction must match keep-real (member %d, intra %d, "
                      "havePrev %d): got %d want %d",
                      member, intra, havePrev, (int)got.retractPrev, (int)want.retract);
                // col= is the observable witness of this decision in every capture log,
                // so its accounting has to match too or an A/B against an old capture
                // would read as a behaviour change.
                CHECK(got.collapsed == want.retract,
                      "no-rotation collapse count must match keep-real (member %d)", member);
                cases++;
            }
        }
    }
    CHECK(cases == 12, "expected 12 exhaustive x2-contract cases, ran %d", cases);

    // THE ROTATING-REGIME FALLBACK: spacing set, but no verdict for this batch. Keep-real
    // member selection with CONTENT-ALIGNED stamps - the stamp convention follows the
    // regime, never the per-batch verdict. Mixing the two was the 2026-08-20 field failure:
    // steered and unsteered frames one flip step apart on the same timeline.
    for (int member = 0; member <= 2; member++) {
        policy::BatchDecision b;
        b.member = member;
        b.intraBatch = (member > 0);
        b.retractPrevious = (member > 0);
        b.stampTs = kBase;
        const policy::KeepDecision got = policy::DecideKeep(b, -1, kSpacing, member > 0);
        CHECK(got.keepThis, "rotating-regime fallback still keeps every member");
        CHECK(got.retractPrev == (member > 0),
              "rotating-regime fallback still retracts like keep-real");
        CHECK(got.stampTs == kBase + (int64_t)member * kSpacing,
              "rotating-regime fallback stamps member %d on its own flip, got %+lld",
              member, (long long)(got.stampTs - kBase));
    }

    // With the rotation known: exactly the named member survives, and it is stamped on its
    // own flip so content and stamp agree whichever member that is.
    {
        policy::BatchDecision m0;
        m0.member = 0; m0.stampTs = kBase;
        policy::BatchDecision m1;
        m1.member = 1; m1.stampTs = kBase; m1.intraBatch = true; m1.retractPrevious = true;

        const policy::KeepDecision k0 = policy::DecideKeep(m0, 0, kSpacing, false);
        CHECK(k0.keepThis && k0.stampTs == kBase, "member 0 named real: keep, stamp at start");
        const policy::KeepDecision k1 = policy::DecideKeep(m1, 0, kSpacing, true);
        CHECK(!k1.keepThis, "member 1 must be dropped when member 0 is the real one");
        CHECK(!k1.retractPrev,
              "a DISCARDED member must not retract the keeper that came before it - this is "
              "the case that would silently empty the ring at x3");
        CHECK(k1.collapsed, "dropping a member still counts as a collapse for col=");

        const policy::KeepDecision j1 = policy::DecideKeep(m1, 1, kSpacing, true);
        CHECK(j1.keepThis && j1.retractPrev,
              "member 1 named real: keep it and retract member 0");
        CHECK(j1.stampTs == kBase + kSpacing,
              "a kept later member must be stamped on its own flip, got %+lld from base",
              (long long)(j1.stampTs - kBase));

        // The all-generated batch: the named member does not exist, so nothing survives.
        const policy::KeepDecision e0 = policy::DecideKeep(m0, 2, kSpacing, false);
        const policy::KeepDecision e1 = policy::DecideKeep(m1, 2, kSpacing, true);
        CHECK(!e0.keepThis && !e1.keepThis,
              "a batch whose real member is past its members must keep nothing");
        CHECK(!e1.retractPrev, "an empty batch has no keeper to retract toward");
    }

    std::printf("  keep decision: 24 exhaustive no-rotation cases match keep-real exactly; "
                "rotation keeps one named member, stamps it on its own flip\n");
}

// THE x3 PHASEKEEP FIELD FAILURE, reproduced end to end, then shown fixed. The 2026-08-20
// capture produced output that ran BACKWARDS on 19% of moving steps (+20.6 px, -21.5 px,
// +37.7 px around each event - temporal ping-pong), with 95% of presents finding no
// before-frame in the ring at all. This models that pipeline - batches with known
// composition and CONTENT, DecideKeep, a slot ring, bracket search, SelectFrame - and
// measures the two symptoms directly: shown-content inversions and before-frame misses.
// Parameterized by the three suspected causes so the same harness is both the repro and
// the proof of fix; if the "fixed" configuration still inverted content, the fixes would
// be theater and this test is what would say so.
struct X3SimResult {
    int inversions = 0;      // consecutive shown frames whose CONTENT went backwards
    int noBefore = 0;        // presents with no before-frame in the window (starvation)
    int genShown = 0;        // presents showing a generated frame's pixels
    int presents = 0;
};
// genContentBias models WHERE a generated frame's content actually sits relative to its
// flip. Zero is the interpolation-at-flip assumption; positive is the warp-overshoot
// hypothesis the FG characterization suspects at x3 (gens there are hf-SHARP, unlike x2's
// blends, and warp extrapolation overshoots). This is deliberately a PARAMETER because it
// is unmeasured at x3 - the same open f/g question the fgphase instrument exists for - and
// the fix must hold under EVERY hypothesis, which it does by never showing gens at all.
static X3SimResult SimulateX3Keep(int ringSlots, bool mixedConvention, bool reclaimSingles,
                                  int64_t effectiveLag, int64_t genContentBias) {
    // x3 grid: real frames flip every 3rd step; batches stride 2 with 2 members; the vote
    // steers 2 of every 3 batches (66%, the measured share) when mixedConvention is on,
    // and every batch when off - because the FIX is not "steer more", it is "stamp the
    // same way whether or not this batch was steered".
    const int64_t kSpacing = 5556, kSrc = 3 * kSpacing;
    const int kFrames = 2000;
    struct Slot { int64_t stamp; int64_t content; bool valid; bool gen; };
    std::vector<Slot> ring;   // grows forever; the WINDOW emulates the real ring's reach

    policy::SelectionState sel;
    policy::PolicyConfig cfg;
    cfg.stickinessQpc = 1000;
    X3SimResult r;
    int64_t lastContent = -1;

    // Batch composition rotates [real,gen] / [gen,real] / [gen,gen] over anchor flips
    // f, f+2, f+4. Real member of a batch anchored at position g is (0 - g) mod 3.
    int64_t nextPresent = 40000;
    size_t nextBatch = 0;
    std::vector<std::array<int64_t, 4>> batches;   // open time, anchor flip, class, single?
    for (int b = 0; b * 2 < kFrames * 3; b++) {
        const int64_t anchor = (int64_t)b * 2;
        const int cls = (int)(anchor % 3);
        // Singles cluster in [gen,real] (measured 18% there, ~2.5% elsewhere).
        const bool single = (cls == 1) ? (b % 6 == 1) : (b % 40 == 7);
        batches.push_back({anchor * kSpacing + 80, anchor, (int64_t)cls, (int64_t)single});
    }

    for (int present = 0; present < kFrames; present++) {
        const int64_t now = nextPresent;
        nextPresent += kSrc;
        // Deliver every batch that opened before this present.
        while (nextBatch < batches.size() && batches[nextBatch][0] <= now) {
            const int64_t bs = batches[nextBatch][0];
            const int64_t anchor = batches[nextBatch][1];
            const int cls = (int)batches[nextBatch][2];
            const bool single = batches[nextBatch][3] != 0;
            const int realMember = (3 - cls) % 3;          // (0 - g) mod 3
            // Steering held at the MEASURED 2/3 share in both configurations: the fix is
            // not "steer more", and a sim that steers everything would hide the residual
            // generated keeps on unsteered batches - the honest limit of this fix set.
            const bool steered = (nextBatch % 3) != 2;
            const int members = single ? 1 : 2;
            const size_t firstSlot = ring.size();
            for (int m = 0; m < members; m++) {
                policy::BatchDecision bd;
                bd.member = m;
                bd.stampTs = bs;
                bd.intraBatch = (m > 0);
                bd.retractPrevious = (m > 0);
                // The fix set, as production implements it: spacing passed for EVERY
                // batch of the rotating regime (one stamp convention), and an unsteered
                // batch while the vote is live keeps NOTHING (member index past the batch)
                // rather than falling back to keep-real and risking a generated frame.
                int member = steered ? realMember : -1;
                if (!mixedConvention && !steered) member = 3;   // vote live, position lost
                const policy::KeepDecision keep = policy::DecideKeep(
                    bd, member,
                    (steered || !mixedConvention) ? kSpacing : 0, m > 0);
                Slot s;
                s.stamp = keep.stampTs;
                // CONTENT: a real member shows its own flip; a generated member is Smooth
                // Motion's interpolation for its flip (between the neighbouring reals).
                s.gen = ((anchor + m) % 3) != 0;
                s.content = (anchor + m) * kSpacing + (s.gen ? genContentBias : 0);
                s.valid = keep.keepThis;
                if (keep.retractPrev && m > 0) ring[firstSlot + m - 1].valid = false;
                ring.push_back(s);
            }
            // The reclaim fix: a single-member batch whose named keeper never arrived kept
            // nothing, but the lone wake's pixels are the FRONTBUFFER AT GRAB - the newest
            // flip - which in [gen,real] is the real frame (the x2-proven coalesced single).
            if (reclaimSingles && steered && single && realMember == 1) {
                ring[firstSlot].valid = true;
                ring[firstSlot].stamp = bs + kSpacing;
                ring[firstSlot].content = (anchor + 1) * kSpacing;
                ring[firstSlot].gen = false;
            }
            nextBatch++;
        }
        if (ring.size() < 8) continue;
        // Bracket within the ring window, exactly as FindBracket reaches p-1..p-(N-1).
        const int64_t target = now - effectiveLag;
        const size_t lo = ring.size() > (size_t)(ringSlots - 1)
                              ? ring.size() - (ringSlots - 1) : 0;
        policy::BracketInfo b;
        size_t befIdx = 0, aftIdx = 0;
        for (size_t i = lo; i < ring.size(); i++) {
            if (!ring[i].valid) continue;
            const int64_t d = target - ring[i].stamp;
            if (d >= 0) { if (!b.hasBefore || d < b.beforeDiff) { b.hasBefore = true; b.beforeTs = ring[i].stamp; b.beforeDiff = d; befIdx = i; } }
            else { if (!b.hasAfter || -d < b.afterDiff) { b.hasAfter = true; b.afterTs = ring[i].stamp; b.afterDiff = -d; aftIdx = i; } }
        }
        r.presents++;
        if (!b.hasBefore) r.noBefore++;
        const policy::Pick pick = policy::SelectFrame(b, sel, cfg);
        size_t shown;
        if (pick == policy::Pick::Before || pick == policy::Pick::BeforeAdv) shown = befIdx;
        else if (pick == policy::Pick::After || pick == policy::Pick::AfterAdv) shown = aftIdx;
        else continue;   // repeat: same content as last, no inversion possible
        if (lastContent >= 0 && ring[shown].content < lastContent) r.inversions++;
        if (ring[shown].gen) r.genShown++;
        lastContent = ring[shown].content;
    }
    return r;
}

static void test_x3_phasekeep_field_failure() {
    // WHAT THE SIM TAUGHT before any fix was written: with gen content AT its flip
    // fraction, the shipped configuration starves (924/2000 before-frames missing) but
    // CANNOT invert content - consecutive batches sit two flip steps apart, so a
    // one-step stamp-convention skew never reorders them. The field's 19% backward steps
    // therefore require generated content OFF its flip fraction, which is the warp
    // hypothesis the FG characterization already suspected at x3 (hf-sharp gens). The
    // backward mechanism is: starvation forces after-adv picks onto GENERATED frames,
    // whose overshot content then steps BACK when the true real frame follows.
    const int64_t kLag = 20833 + 12000;   // lock pull carries the target deep, as captured
    // Gen content past its flip fraction. The sweep over this parameter put the inversion
    // ONSET at ~8000 us (1.4 flips) in the broken configuration and the field's backward
    // magnitude at ~1.7 source periods, so the hypothesis constant sits at the onset: if
    // x3's generated frames are at least this far off their flip, the field reproduces.
    const int64_t kOvershoot = 8000;

    // Repro 1: starvation is unconditional in the shipped config.
    const X3SimResult broken0 = SimulateX3Keep(8, true, false, kLag, 0);
    CHECK(broken0.noBefore > broken0.presents / 10,
          "the field configuration must reproduce ring starvation (got %d of %d)",
          broken0.noBefore, broken0.presents);
    CHECK(broken0.genShown > broken0.presents / 10,
          "starvation must force generated frames into the output (got %d of %d) - they "
          "are the backward-step carrier under the warp hypothesis",
          broken0.genShown, broken0.presents);
    CHECK(broken0.inversions == 0,
          "with gen content AT its flip fraction the shipped config must NOT invert - "
          "this null is what proves the backward steps came from gen content, not from "
          "stamp order (got %d)", broken0.inversions);

    // Repro 2: under the warp hypothesis the shipped config shows the field's symptom.
    const X3SimResult broken = SimulateX3Keep(8, true, false, kLag, kOvershoot);
    CHECK(broken.inversions > 0,
          "under the warp hypothesis the field configuration must reproduce backward "
          "content, got %d inversions in %d presents",
          broken.inversions, broken.presents);

    // THE FIX: ring 16, ONE stamp convention regardless of per-batch steering, coalesced
    // singles reclaimed. It must be clean under BOTH gen-content hypotheses, because it
    // wins by never showing generated frames at all - the only strategy that does not
    // depend on the unmeasured answer to the x3 f/g question.
    for (int64_t bias : {(int64_t)0, kOvershoot}) {
        const X3SimResult fixed = SimulateX3Keep(16, false, true, kLag, bias);
        CHECK(fixed.inversions == 0,
              "the fixed configuration must never show backward content (bias %lld), got %d",
              (long long)bias, fixed.inversions);
        CHECK(fixed.noBefore <= fixed.presents / 100,
              "the fixed configuration must not starve the ring (bias %lld, got %d of %d)",
              (long long)bias, fixed.noBefore, fixed.presents);
        CHECK(fixed.genShown * 20 < fixed.presents,
              "the fixed configuration must show almost no generated frames (bias %lld, "
              "got %d of %d)", (long long)bias, fixed.genShown, fixed.presents);
    }

    // Each fix alone is NOT sufficient - pinned so nobody ships one and calls it done.
    const X3SimResult ringOnly = SimulateX3Keep(16, true, false, kLag, kOvershoot);
    CHECK(ringOnly.inversions > 0 || ringOnly.genShown * 20 >= ringOnly.presents,
          "ring size alone must not read as a full fix");

    const X3SimResult fixed = SimulateX3Keep(16, false, true, kLag, kOvershoot);
    std::printf("  x3 phasekeep field failure: broken(ring8,mixed) %d/%d starved, %d gen "
                "shown, %d inversions under warp hypothesis; fixed(ring16,uniform,reclaim) "
                "%d starved, %d gen, %d inversions\n",
                broken.noBefore, broken.presents, broken.genShown, broken.inversions,
                fixed.noBefore, fixed.genShown, fixed.inversions);
}

static void test_rotation_phase() {
    // Period arithmetic first: derived from measured stride and flips-per-source-period, so
    // every multiplier falls out of one rule instead of being special-cased.
    CHECK(policy::RotationPeriodBatches(2, 2) == 1, "x2 (2 flips, stride 2) must not rotate");
    CHECK(policy::RotationPeriodBatches(3, 2) == 3, "x3 (3 flips, stride 2) rotates over 3");
    CHECK(policy::RotationPeriodBatches(4, 2) == 2, "x4 (4 flips, stride 2) rotates over 2");
    CHECK(policy::RotationPeriodBatches(1, 1) == 1, "FG off cannot rotate");
    CHECK(policy::RotationPeriodBatches(9, 2) == 1,
          "a rotation longer than the vote can hold must report inert, not overflow");

    // A stand-in for the MEASURED signature, not for a convenient one. Across ten captures
    // the real-led class sits below zero (-30 to -96) and every generated-led class above
    // it (+36 to +129), with the per-batch spread far wider than the gap between class
    // means - so only the aggregate separates and a per-batch rule would be wrong about
    // half the time. The sign structure is what the decision reads; these numbers put a
    // synthetic class in each measured position.
    const int64_t kRealLed = -60, kGenLed = 90, kSpread = 400;
    // THE HARNESS CARRIES ITS OWN GROUND TRUTH. It tracks truePos independently and plants
    // the real-led signal there, then asserts the code agrees. Two earlier versions of this
    // test did not, and each hid a bug in the thing it was meant to prove: the first passed
    // the class in directly (so it could not see that batch index is not grid position),
    // and the second planted the signal at p.gridPos - asking the code where it thinks it
    // is, which makes every phase assertion a tautology. Injecting an off-by-one into
    // RotationAdvance passed both.
    // The code's origin is ARBITRARY - RotationAdvance adopts its first anchor as position
    // 0 - so absolute positions cannot be compared. What must hold is that the offset
    // between the two frames of reference stays CONSTANT: that is exactly the property that
    // breaks when position tracking loses a step, and it is invisible to a test that reads
    // its expectations out of p.gridPos.
    struct Harness {
        policy::RotationPhase p;
        int truePos = 0;          // the grid position the TEST is counting, from its own origin
        int truePhase = 0;        // where the test planted the real-led class, in ITS frame
        int offset = -1;          // (code position - test position) mod fps, once established
        int64_t t = 100000;
        int64_t spacing = 5556;
        int fps = 3;
    };
    auto init = [](Harness& h, int stride, int fps, int truePhase, int64_t spacing) {
        policy::RotationReset(h.p, stride, fps, kVoteMinSeparationUs);
        h.truePos = 0;
        h.truePhase = truePhase;
        h.offset = -1;
        h.t = 100000;
        h.spacing = spacing;
        h.fps = fps;
    };
    // One batch: advance the test's own position, advance the code's, then plant the signal
    // where the TEST says we are. Nothing here reads p.gridPos to decide what to plant.
    auto step = [&](Harness& h, int steps, int i) {
        h.t += (int64_t)steps * h.spacing;
        h.truePos = (int)(((long long)h.truePos + steps) % h.fps);
        if (!policy::RotationAdvance(h.p, h.t, steps)) return false;
        if (h.offset < 0) {
            h.offset = ((h.p.gridPos - h.truePos) % h.fps + h.fps) % h.fps;
        }
        const int64_t mean = (h.truePos == h.truePhase) ? kRealLed : kGenLed;
        policy::RotationObserve(h.p, mean + ((i % 2) ? kSpread : -kSpread));
        return true;
    };
    // Is the code still in step with the test, whatever origin each started from?
    auto inStep = [&](const Harness& h) {
        if (h.offset < 0) return true;
        return ((h.p.gridPos - h.truePos) % h.fps + h.fps) % h.fps == h.offset;
    };
    // The real member is FRAME-INDEPENDENT - it is a difference of two positions - so this
    // is the assertion that means the same thing on both sides of the origin question.
    auto trueMember = [&](const Harness& h) {
        int m = (h.truePhase - h.truePos) % h.fps;
        if (m < 0) m += h.fps;
        return m;
    };

    {   // x3, the regime this exists for. THE CASE THAT DECIDES THE DESIGN is the third
        // class: both its members are generated, so the real member index runs past the
        // batch and the caller keeps NOTHING. Keeping one anyway leaves the ring holding
        // frames 3, 2 and 1 flips apart; dropping it leaves exactly the real frames, one
        // per source period, uniformly spaced - which is what a 60 fps output wants.
        Harness h;
        init(h, /*stride=*/2, /*fps=*/3, /*truePhase=*/1, 5556);
        policy::RotationPhase& p = h.p;
        CHECK(p.period == 3, "x3 must rotate over 3 batches, got %d", p.period);
        CHECK(!p.valid, "a fresh vote is not valid");
        for (int i = 0; i < 200; i++) step(h, 2, i);
        CHECK(p.valid, "200 batches must decide a 3-class vote");
        // THE ASSERTION THE TAUTOLOGY HID: the code's position must match the test's own
        // independent count, and the phase it names must be where the test planted it.
        CHECK(inStep(h),
              "the code's grid position (%d) has drifted from the test's independent count "
              "(%d) against the offset established at the origin (%d)",
              p.gridPos, h.truePos, h.offset);
        CHECK(policy::RotationRealMember(p, p.gridPos) == trueMember(h),
              "reported real member %d, test's model says %d",
              policy::RotationRealMember(p, p.gridPos), trueMember(h));
        // The algebra, stated relative to whatever phase the code named - the origin is
        // arbitrary, the STRUCTURE is not: at the real phase member 0 is real, one flip
        // earlier member 1 is, two flips earlier no member of a 2-member batch is.
        const int rp = p.realPhase;
        CHECK(policy::RotationRealMember(p, rp) == 0,
              "a batch anchored at the real phase keeps member 0, got %d",
              policy::RotationRealMember(p, rp));
        CHECK(policy::RotationRealMember(p, (rp + 2) % 3) == 1,
              "one flip earlier the real frame is member 1, got %d",
              policy::RotationRealMember(p, (rp + 2) % 3));
        CHECK(policy::RotationRealMember(p, (rp + 1) % 3) >= 2,
              "the all-generated position must name no member a 2-member batch has, got %d",
              policy::RotationRealMember(p, (rp + 1) % 3));

        // Over one full rotation exactly the real frames are kept, one per source period -
        // the property that makes the cadence uniform instead of 3/2/1 flips.
        int kept = 0, lastFlip = -1, firstGap = -1;
        for (int k = 0; k < 6; k++) {          // six consecutive batches, stride 2
            const int m = policy::RotationRealMember(p, (rp + k * 2) % 3);
            if (m < 0 || m >= 2) continue;
            const int flip = k * 2 + m;
            if (lastFlip >= 0 && firstGap < 0) firstGap = flip - lastFlip;
            if (lastFlip >= 0) {
                CHECK(flip - lastFlip == firstGap,
                      "kept frames must be evenly spaced, saw %d then %d flips",
                      firstGap, flip - lastFlip);
            }
            lastFlip = flip;
            kept++;
        }
        CHECK(kept == 4, "6 x3 batches carry 4 real frames, kept %d", kept);
        CHECK(firstGap == 3, "kept frames must sit one source period apart, got %d", firstGap);
    }

    {   // NEGATIVE CONTROL, the one that matters: at x2 every batch is [gen,real], so there
        // is no real-led class to find. A vote that "finds" one anyway would retain the
        // wrong member on a third of x2 batches - actively worse than what it replaces.
        policy::RotationPhase p;
        policy::RotationReset(p, 2, 3, kVoteMinSeparationUs);          // force a 3-class vote over uniform data
        int64_t t = 100000;
        for (long long i = 0; i < 400; i++) {
            t += 2 * 5556;
            policy::RotationAdvance(p, t, 2);
            policy::RotationObserve(p, kGenLed + ((i % 2) ? kSpread : -kSpread));
        }
        CHECK(!p.valid, "a uniform population must never decide a winner");
        CHECK(policy::RotationRealMember(p, 0) < 0, "undecided must read as plain keep-real");
    }

    {   // THE OTHER HALF OF THAT CONTROL, and the one a spread test fails: classes that DO
        // differ but are all on the same side of zero. Measured on four of the five x2
        // captures - means like -191/-80/-124 - where a lowest-class rule would happily
        // crown a winner. The mechanism says a real-led batch wakes BEFORE its flip, so
        // "lowest" is not enough; it has to be negative with the others positive.
        policy::RotationPhase p;
        policy::RotationReset(p, 2, 3, kVoteMinSeparationUs);
        int64_t t = 100000;
        for (long long i = 0; i < 400; i++) {
            t += 2 * 5556;
            if (!policy::RotationAdvance(p, t, 2)) continue;
            const int64_t mean = (p.gridPos == 0) ? -190 : (p.gridPos == 1 ? -80 : -124);
            policy::RotationObserve(p, mean + ((i % 2) ? kSpread : -kSpread));
        }
        CHECK(!p.valid,
              "all-negative classes must not decide: the signature is one AHEAD of its "
              "flip and the rest behind, not merely one lower than the rest");
    }

    {   // Inert wherever composition does not rotate: x2 and frame generation off.
        policy::RotationPhase p;
        policy::RotationReset(p, 2, 2, kVoteMinSeparationUs);
        CHECK(p.period == 1, "x2 must not rotate");
        int64_t t = 100000;
        for (long long i = 0; i < 300; i++) {
            t += 2 * 8333;
            policy::RotationAdvance(p, t, 2);
            policy::RotationObserve(p, kRealLed);
        }
        CHECK(!p.valid && policy::RotationRealMember(p, 0) < 0,
              "period 1 must stay inert: there is no rotation to read");
    }

    {   // Silence before confidence: a partial vote must not commit. One class is starved
        // here, which is exactly the state just after position is re-established.
        policy::RotationPhase p;
        policy::RotationReset(p, 2, 3, kVoteMinSeparationUs);
        int64_t t = 100000;
        for (long long i = 0; i < 60; i++) {
            t += 2 * 5556;
            if (!policy::RotationAdvance(p, t, 2)) continue;
            if (p.gridPos == 2) continue;        // starve one class
            const int64_t mean = (p.gridPos == 1) ? kRealLed : kGenLed;
            policy::RotationObserve(p, mean + ((i % 2) ? kSpread : -kSpread));
        }
        CHECK(!p.valid, "an under-sampled class must block the decision");
    }

    {   // THE VERDICT IS NOT LATCHED. A vote that converged once must give the verdict back
        // when the evidence goes away, or a regime change leaves it steering on history.
        Harness h;
        init(h, 2, 3, /*truePhase=*/1, 5556);
        for (int i = 0; i < 200; i++) step(h, 2, i);
        CHECK(h.p.valid, "precondition: the vote should have decided");
        for (int i = 0; i < 2000; i++) {
            h.t += 2 * h.spacing;
            policy::RotationAdvance(h.p, h.t, 2);
            policy::RotationObserve(h.p, kGenLed + ((i % 2) ? kSpread : -kSpread));
        }
        CHECK(!h.p.valid,
              "a decided vote must lapse once the classes stop separating, not latch");
    }

    {   // Reset really resets: a stale winner surviving a mode change would confidently
        // retain the wrong member.
        Harness h;
        init(h, 2, 3, /*truePhase=*/0, 5556);
        for (int i = 0; i < 200; i++) step(h, 2, i);
        CHECK(h.p.valid && policy::RotationRealMember(h.p, h.p.gridPos) == trueMember(h),
              "precondition: the vote should have decided and agree with the test's model");
        policy::RotationReset(h.p, 2, 3, kVoteMinSeparationUs);
        CHECK(!h.p.valid && policy::RotationRealMember(h.p, 0) < 0,
              "reset must clear the verdict");
    }

    {   // A BROKEN STRIDE MUST NOT DESYNCHRONISE THE VOTE. Real captures break stride-2
        // continuity constantly - measured, every ~89 batches - and the advance is COUNTED
        // from the flip history precisely so a batch that skipped a beat moves the position
        // by the right number of flips instead of silently rotating the mapping. Here every
        // seventh batch advances 4 flips rather than 2, and the harness tracks the truth
        // independently so a desynchronised mapping is visible rather than assumed away.
        Harness h;
        init(h, 2, 3, /*truePhase=*/2, 5556);
        for (int i = 0; i < 400; i++) step(h, ((i % 7) == 6) ? 4 : 2, i);
        CHECK(h.p.valid, "a counted advance must survive a broken stride");
        CHECK(inStep(h),
              "position must survive the breaks: code %d, truth %d, origin offset %d",
              h.p.gridPos, h.truePos, h.offset);
        CHECK(policy::RotationRealMember(h.p, h.p.gridPos) == trueMember(h),
              "across broken strides the reported member (%d) must still match the test's "
              "model (%d)",
              policy::RotationRealMember(h.p, h.p.gridPos), trueMember(h));
    }

    {   // THE STALE-ORIGIN REGRESSION. A stall past the step guard re-origins the grid
        // position, and the evidence gathered against the OLD origin must not be allowed to
        // restore the old verdict - the phase would then be expressed against an origin that
        // no longer exists. Measured consequence when it did: 0 real frames kept, 20
        // generated frames kept and 10 whole batches dropped over the following 30 batches,
        // with the telemetry reading perfectly normal throughout (PhaseKeepEmpty at exactly
        // the designed 1/3, no resets logged). Strictly worse than the keep-real rule this
        // replaces, for ~2.4 s per stall.
        Harness h;
        init(h, 2, 3, /*truePhase=*/1, 5556);
        for (int i = 0; i < 300; i++) step(h, 2, i);
        CHECK(h.p.valid && inStep(h), "precondition: converged and in step");

        // A 200 ms stall: 36 flip steps, well past the 8-step guard. Note this does NOT
        // trigger RotationReset in production - the source-period EMA excludes gaps over
        // 125 ms, so stride and flipsPerSource are unchanged and the histogram survives.
        h.t += 36 * h.spacing;
        h.truePos = (h.truePos + 36) % h.fps;
        CHECK(!policy::RotationAdvance(h.p, h.t, 36), "a 36-step advance must be refused");
        CHECK(!h.p.valid, "the refusal must drop the verdict");

        // One batch later. The old verdict must NOT come back: its phase was measured
        // against an origin that has been discarded.
        h.t += 2 * h.spacing;
        h.truePos = (h.truePos + 2) % h.fps;
        policy::RotationAdvance(h.p, h.t, 2);
        policy::RotationObserve(h.p, kGenLed + kSpread);
        CHECK(!h.p.valid,
              "a re-origin must not restore a verdict keyed to the old origin after ONE "
              "sample: the evidence has to be re-gathered against the new origin");

        // And once it does decide again, it must agree with the test's own model.
        for (int i = 0; i < 400; i++) step(h, 2, i);
        CHECK(h.p.valid, "the vote must re-converge after a stall");
        CHECK(policy::RotationRealMember(h.p, h.p.gridPos) == trueMember(h),
              "post-stall member %d must match the test's model %d",
              policy::RotationRealMember(h.p, h.p.gridPos), trueMember(h));
    }

    {   // x4: a shorter rotation, two classes, and only one of them carries a real frame.
        // Exercises the same arithmetic at a multiplier nothing has been captured at.
        Harness h;
        init(h, /*stride=*/2, /*fps=*/4, /*truePhase=*/0, 4167);
        CHECK(h.p.period == 2, "x4 must rotate over 2 batches, got %d", h.p.period);
        for (int i = 0; i < 200; i++) step(h, 2, i);
        CHECK(h.p.valid, "x4 vote must decide");
        CHECK(policy::RotationRealMember(h.p, 0) == 0, "x4 real-led batch keeps member 0");
        CHECK(policy::RotationRealMember(h.p, 1) >= 2,
              "x4's other batch is all generated, got member %d",
              policy::RotationRealMember(h.p, 1));

        // THE LATTICE TRAP. Stride 2 over 4 flips only ever visits even positions, so the
        // decision loop reads only those - but an odd advance moves the position off that
        // sublattice permanently, and nothing brings it back. Left undetected, the even
        // classes freeze at their old counts, stay above the sample floor, and hold the
        // verdict valid FOREVER on evidence that stopped updating - while every new sample
        // lands in a class nothing reads. Consequence measured: 50% of batches dropped,
        // 50% keeping a generated frame, indefinitely.
        step(h, 3, 0);            // one odd advance
        for (int i = 0; i < 50; i++) step(h, 2, i);
        CHECK(!h.p.valid || h.p.gridPos % 2 == 0,
              "an odd advance must not leave the vote valid on a position the decision "
              "loop never reads (gridPos %d, reach 2)", h.p.gridPos);
    }

    {   // A LONG COUNTED ADVANCE PRESERVES PHASE. Counting certifies position exactly
        // across any gap whose flips reached the history, so a 20-step stall must carry
        // the vote across intact - no re-origin, no evidence loss, same verdict after.
        // The old bound of 8 re-origined here, and real captures exceed 8 flips 137 times
        // in two minutes: the vote never survived long enough to steer (127 of 15366
        // batches on the 2026-08-20 fix_reverse run). Gaps past the count cap (24) are
        // zero in the same capture, and past the cap the count could truncate, so THAT is
        // where the re-origin belongs - pinned by the 36-step stall test below.
        Harness h;
        init(h, 2, 3, /*truePhase=*/1, 5556);
        for (int i = 0; i < 300; i++) step(h, 2, i);
        CHECK(h.p.valid, "precondition: converged");
        CHECK(step(h, 20, 0),
              "a 20-step counted advance must be accepted, not treated as an origin loss");
        CHECK(h.p.valid, "the verdict must survive a 20-step counted advance");
        for (int i = 0; i < 10; i++) step(h, 2, i);
        CHECK(h.p.valid && policy::RotationRealMember(h.p, h.p.gridPos) == trueMember(h),
              "phase must be INTACT after the gap: reported member %d, truth %d",
              policy::RotationRealMember(h.p, h.p.gridPos), trueMember(h));
    }

    {   // THE EXTRAPOLATION IS BOUNDED. RotationPositionAt is the one place in the design
        // that divides a time difference by the spacing, which every other path avoids
        // because a misrounding rotates the mapping. Over the ~2 batches it is designed to
        // span it is exact; over an unbounded gap it is not - a 300 ms unanchored run at a
        // spacing read 3% low misrounds by 2 flips on a VRR panel, silently and confidently.
        // Past the bound it must report "no position", which the caller reads as keep-real.
        Harness h;
        init(h, 2, 3, /*truePhase=*/1, 5556);
        for (int i = 0; i < 200; i++) step(h, 2, i);
        CHECK(h.p.valid, "precondition: converged");
        const int64_t anchor = h.p.lastAnchorTs;
        CHECK(policy::RotationPositionAt(h.p, anchor + 2 * h.spacing, h.spacing) >= 0,
              "two flips out must still extrapolate");
        CHECK(policy::RotationPositionAt(h.p, anchor + 4 * h.spacing, h.spacing) >= 0,
              "four flips out must still extrapolate");
        CHECK(policy::RotationPositionAt(h.p, anchor + 54 * h.spacing, h.spacing) < 0,
              "a 300 ms unanchored gap (54 flips) must refuse to extrapolate, not guess");
        CHECK(policy::RotationPositionAt(h.p, anchor - 2 * h.spacing, h.spacing) < 0,
              "a time BEFORE the anchor is not a forward extrapolation");
    }

    {   // A GRID LONGER THAN THE CLASS ARRAY MUST STAY INERT. RotationPeriodBatches bounds
        // the PERIOD, but flipsPerSource is what indexes the classes, and a short period can
        // carry a long grid: -src 15 against an x3 flip rate gives flipsPerSource 12 with
        // period 6. Two thirds of the reachable classes would then be unreadable, and the
        // vote would decide on a subset - confidently naming a phase from partial evidence.
        policy::RotationPhase p;
        policy::RotationReset(p, /*stride=*/2, /*flipsPerSource=*/12, kVoteMinSeparationUs);
        CHECK(p.period == 1,
              "a 12-flip grid exceeds the class array and must report inert, got period %d",
              p.period);
        int64_t t = 100000;
        for (int i = 0; i < 400; i++) {
            t += 2 * 5556;
            policy::RotationAdvance(p, t, 2);
            policy::RotationObserve(p, ((i % 3) == 0 ? kRealLed : kGenLed));
        }
        CHECK(!p.valid && policy::RotationRealMember(p, 0) < 0,
              "an over-long grid must never decide");
    }

    std::printf("  rotation phase: period arithmetic, ensemble vote on overlapping classes, "
                "all-generated batches kept empty, refusal on a uniform population, "
                "bounded extrapolation\n");
}

static void test_dejit_removes_late_blends() {
    const int64_t kSrc = 16667, kFlip = 8333, kEps = 400, kDelivery = 1300;
    const int64_t kBase = 1000000, kFrames = 400;

    struct Outcome { int synthOff, synthOn, removed, added; long long corrected, late; };
    auto run = [&](int64_t lateUs, int everyN) {
        SimParams p;
        p.srcPeriod = kSrc; p.presentPeriod = kSrc; p.arrivalJitter = 0;
        p.combQpc = kSrc; p.presents = 0; p.phaseOffset = 0;
        p.passthroughQpc = kSrc / 4; p.lagOverride = 0;
        for (int64_t k = 0; k < kFrames; k++) {
            // Late deliveries are periodic but sparse, so the chain stays warm between them
            // exactly as it does in the field; a burst would test re-acquisition instead.
            const int64_t late = (everyN > 0 && k % everyN == 0 && k > 20) ? lateUs : 0;
            const int64_t arr = kBase + k * kSrc + late;
            p.explicitArrivals.push_back(arr);          // generated member (batch opens)
            p.explicitArrivals.push_back(arr + kEps);   // real member, one flip later
            // The flips do NOT move: the frames were rendered on time and scanned out on
            // the grid; only the handover to the capture API slipped.
            p.flipDisplay.push_back(kBase + k * kSrc);
            p.flipDisplay.push_back(kBase + k * kSrc + kFlip);
            p.flipKnown.push_back(kBase + k * kSrc + kDelivery);
            p.flipKnown.push_back(kBase + k * kSrc + kFlip + kDelivery);
        }
        // Presents are PHASE-OFFSET from arrivals by half a source period. Real capture and
        // present clocks are independent and the comb lock settles at some arbitrary phase;
        // putting both on the identical grid makes every batch get measured at the instant
        // it arrives, BEFORE its flip is delivered ~1.3 ms later, so the chain can only ever
        // acquire by accident. That artifact - not the code - is why an earlier version of
        // this sweep reported zero corrections at 3 ms and 5 ms while a standalone probe of
        // the same function fired correctly.
        for (int64_t i = 0; i < kFrames; i++)
            p.explicitPresents.push_back(kBase + i * kSrc + kSrc / 2);
        const SimResult off = Simulate(p);
        p.flipDejitter = true;
        const SimResult on = Simulate(p);
        Outcome o{0, 0, 0, 0, on.correctedBatches, on.lateBatches};
        for (size_t i = 60; i < off.ops.size() && i < on.ops.size(); i++) {
            const bool a = off.ops[i] == policy::CompositeOp::Synthesize;
            const bool b = on.ops[i] == policy::CompositeOp::Synthesize;
            if (a) o.synthOff++;
            if (b) o.synthOn++;
            if (a && !b) o.removed++;
            if (!a && b) o.added++;
        }
        return o;
    };

    // Control: a timeline with NO late deliveries must be untouched. If the correction fires
    // on a clean grid it is fabricating lateness, which is the failure mode that shipped
    // once already (a mis-locked chain reading a constant offset as delivery delay).
    const Outcome clean = run(0, 0);
    CHECK(clean.removed == 0 && clean.added == 0,
          "clean grid: dejit changed %d decisions (removed %d, added %d) with nothing late",
          clean.removed + clean.added, clean.removed, clean.added);

    std::printf("  dejit lateness sweep (every 40th batch late, 400 source frames):\n");
    int sawRemoval = 0;
    for (int64_t late : {1500, 3000, 5000, 7000, 9000}) {
        const Outcome o = run(late, 40);
        std::printf("      late %5lld us: synth %3d -> %3d  (removed %d, added %d, "
                    "corrections %lld of %lld late)\n",
                    (long long)late, o.synthOff, o.synthOn, o.removed, o.added,
                    o.corrected, o.late);
        CHECK(o.added == 0,
              "lateness %lld us: dejit ADDED %d blends", (long long)late, o.added);
        if (o.removed > 0) sawRemoval++;
    }
    CHECK(sawRemoval > 0,
          "dejit removed no blends at ANY lateness from 1.5 to 9 ms: the correction is a "
          "no-op and every do-no-harm fixture would still pass");
    std::printf("  dejit: clean grid untouched; removals appear as lateness grows\n");
}

static void test_flip_pairing() {
    const int64_t kGrid = 8333;            // 60x2: 120 flips/s
    // Cadence window, in this file's microseconds. The confidence bound is derived from the
    // measured step (a quarter of it), so tests exercise the same derivation the relay does
    // rather than pinning a number the relay does not use.
    const int64_t kWindow = 200000;
    const int64_t kBound = kGrid / 4;
    const int64_t kBase = 1000000;
    policy::FlipHistory h;
    for (int i = 0; i < 60; i++) {
        policy::Flip f;
        f.displayTs = kBase + (int64_t)i * kGrid;
        f.eventTs = f.displayTs - 200;
        f.head = 0;
        h.Add(f);
        policy::Flip g;                     // head 1 decoy on a different cadence
        g.displayTs = kBase + (int64_t)i * 16667;
        g.head = 1;
        h.Add(g);
    }
    const int64_t anchor = kBase + 30 * kGrid;

    // A batch landing on a flip: member 0 takes that flip, member 1 the NEXT one. The two
    // arrive an epsilon apart and scan out a full grid step apart, which is the entire
    // reason this function exists.
    policy::FlipPairing p0 = policy::PairBatchMember(h, 0, anchor + 240, 0, kWindow);
    CHECK(p0.paired && p0.displayTs == anchor,
          "member 0 should pair to its anchor flip, got paired=%d ts=%lld",
          (int)p0.paired, (long long)p0.displayTs);
    CHECK(p0.anchorOffset == 240, "anchor offset should be the wake's lateness, got %lld",
          (long long)p0.anchorOffset);
    policy::FlipPairing p1 = policy::PairBatchMember(h, 0, anchor + 240, 1, kWindow);
    CHECK(p1.paired && p1.displayTs == anchor + kGrid,
          "member 1 should pair one grid step later, got %lld",
          (long long)(p1.displayTs - anchor));
    CHECK(p1.displayTs - p0.displayTs == kGrid,
          "two members of one batch must scan out exactly one flip apart");

    // Head isolation: head 1 runs a different cadence, and leaking it in would produce a
    // plausible-looking wrong answer rather than a visible failure.
    policy::FlipPairing ph = policy::PairBatchMember(h, 1, anchor + 240, 0, kWindow);
    CHECK(!ph.paired || ph.displayTs % 16667 == kBase % 16667,
          "a head-1 pairing must come from head 1's own grid");

    // A wake landing just BEFORE a flip belongs to that flip, not the previous one: the
    // driver hands the frame over ahead of its proposed scanout time, and 17.7% of real
    // batches arrive on this side. Anchoring at-or-before would put them a grid step back.
    policy::FlipPairing early = policy::PairBatchMember(h, 0, anchor - 90, 0, kWindow);
    CHECK(early.paired && early.displayTs == anchor,
          "a wake 90us before its flip must anchor to THAT flip, got %lld",
          (long long)(early.displayTs - anchor));
    CHECK(early.anchorOffset == -90,
          "an early wake must report a negative offset, got %lld",
          (long long)early.anchorOffset);
    policy::FlipPairing early1 = policy::PairBatchMember(h, 0, anchor - 90, 1, kWindow);
    CHECK(early1.paired && early1.displayTs == anchor + kGrid,
          "member stepping must work from an early-arriving batch too, got %lld",
          (long long)(early1.displayTs - anchor));

    // Past the derived bound (a quarter step, 2083us here) no flip is close enough to claim
    // this wake, so it is unpairable rather than pairable-with-a-big-offset. The bound has to
    // be tighter than half a step for this to be reachable at all, which is why it is a
    // quarter and not a half.
    policy::FlipPairing far =
        policy::PairBatchMember(h, 0, anchor + kBound + 400, 0, kWindow);
    CHECK(!far.paired && !far.anchorFound,
          "a wake past a quarter step from any flip must be unpairable");
    policy::FlipPairing near =
        policy::PairBatchMember(h, 0, anchor + kBound - 400, 0, kWindow);
    CHECK(near.paired && near.displayTs == anchor,
          "a wake inside the bound must still pair, with the offset reported");
    CHECK(near.anchorOffset == kBound - 400,
          "the reported offset must be the real distance, not a clamped one, got %lld",
          (long long)near.anchorOffset);

    // The member's flip has not been announced yet. This is the ordinary early state, not
    // an anomaly, and the caller has to be able to tell the two apart.
    policy::FlipHistory partial;
    for (int i = 0; i < 40; i++) {
        policy::Flip f;
        f.displayTs = kBase + (int64_t)i * kGrid;
        f.head = 0;
        partial.Add(f);
    }
    const int64_t last = kBase + 39 * kGrid;
    policy::FlipPairing ahead = policy::PairBatchMember(partial, 0, last, 1, kWindow);
    CHECK(!ahead.paired && ahead.anchorFound && ahead.memberAhead,
          "member 1 past the end of history must report memberAhead, not a guess");

    // A hole in the grid must not be stepped over. Indexing alone would hand back the flip
    // AFTER the hole and call it member 1.
    policy::FlipHistory gap;
    for (int i = 0; i < 40; i++) {
        if (i == 21) continue;              // the flip member 1 would want
        policy::Flip f;
        f.displayTs = kBase + (int64_t)i * kGrid;
        f.head = 0;
        gap.Add(f);
    }
    policy::FlipPairing holed =
        policy::PairBatchMember(gap, 0, kBase + 20 * kGrid, 1, kWindow);
    CHECK(!holed.paired && holed.gridGap,
          "a missing flip must fail as a grid gap, got paired=%d ts=%lld",
          (int)holed.paired, (long long)holed.displayTs);

    // No cadence at all: no data is not an anomaly either.
    policy::FlipHistory empty;
    policy::FlipPairing none = policy::PairBatchMember(empty, 0, anchor, 0, kWindow);
    CHECK(!none.paired && !none.anchorFound && !none.memberAhead && !none.gridGap,
          "an empty history must report plain no-data");

    std::printf("  flip pairing: member steps the grid, head isolation, and unpairable "
                "wakes report WHY (out of range / not yet announced / grid gap)\n");
}

// Batch-collapse keep-real, asserted directly rather than inferred from a timeline.
// Wake order under frame generation is [GENERATED, REAL], so the SECOND member of a
// batch is the one worth keeping: it publishes, and the generated member ahead of it is
// retracted. Flipping this to keep-first ghosted throughout in the definitive A/B, and
// the flip would be invisible in any aggregate timing metric, so it is pinned here.
static void test_batch_collapse_keep_real() {
    const int64_t kThreshold = 3000;
    policy::BatchState s;

    // A lone wake opens a batch: nothing to retract, stamped at its own arrival.
    policy::BatchDecision d = policy::UpdateBatch(s, 100000, kThreshold);
    CHECK(!d.intraBatch, "first wake should open a batch");
    CHECK(!d.retractPrevious, "first wake has no predecessor to retract");
    CHECK(d.stampTs == 100000, "lone wake stamps at its own arrival, got %lld",
          (long long)d.stampTs);
    CHECK(d.member == 0, "a batch-opening wake is member 0, got %d", d.member);

    // Its twin arrives an epsilon later: same batch, keeps the BATCH-START stamp so the
    // ring timeline stays at base cadence, and retracts the generated member.
    d = policy::UpdateBatch(s, 100400, kThreshold);
    CHECK(d.intraBatch, "a wake 400us later is inside the batch");
    CHECK(d.retractPrevious, "keep-real: the intra-batch member retracts its predecessor");
    CHECK(d.stampTs == 100000, "intra-batch member stamps at batch start, got %lld",
          (long long)d.stampTs);
    CHECK(d.member == 1, "the second member is index 1, got %d", d.member);

    // A third member an epsilon on is still inside the batch (the chain rule).
    d = policy::UpdateBatch(s, 100700, kThreshold);
    CHECK(d.intraBatch, "chained third member is still intra-batch");
    CHECK(d.stampTs == 100000, "chained member still stamps at batch start, got %lld",
          (long long)d.stampTs);
    // The index has to keep counting, not saturate: it is what places the member on the
    // flip grid, and a third member scans out two flips after the anchor, not one.
    CHECK(d.member == 2, "the chained third member is index 2, got %d", d.member);

    // The next base frame opens a new batch and reports the base period, with the
    // submission epsilon excluded (batch start to batch start, not wake to wake).
    d = policy::UpdateBatch(s, 116667, kThreshold);
    CHECK(!d.intraBatch, "a wake a full source period later opens a new batch");
    CHECK(!d.retractPrevious, "a new batch must not retract the previous batch's keeper");
    CHECK(d.batchGap == 16667, "batch gap is base cadence, got %lld", (long long)d.batchGap);
    CHECK(d.member == 0, "a new batch restarts the member index, got %d", d.member);

    // A gap just under the threshold still collapses; just over it does not.
    policy::BatchState t;
    policy::UpdateBatch(t, 0, kThreshold);
    CHECK(policy::UpdateBatch(t, kThreshold - 1, kThreshold).intraBatch,
          "a gap just under the threshold collapses");
    policy::BatchState u;
    policy::UpdateBatch(u, 0, kThreshold);
    CHECK(!policy::UpdateBatch(u, kThreshold, kThreshold).intraBatch,
          "a gap at the threshold does not collapse");
    std::printf("  batch-collapse keep-real: stamping, retraction, chaining, thresholds\n");
}

// The same stall on the arrival cadence the relay actually sees with frame generation on.
// Smooth Motion x2 delivers the real frame and its generated twin about 0.4 ms apart, then
// the rest of the source period to the next pair, so the ring takes two arrivals per source
// period rather than one. Measured on a 60 fps source: gaps alternate ~16.3 ms / ~0.4 ms,
// P(short | previous long) = 0.91 and P(short | previous short) = 0.00, and each pair sums
// to one source period. A uniform grid therefore tests a cadence that never occurs whenever
// frame generation is enabled, which is the default configuration.
//
// Both the resume phase and the parity matter: a freeze can begin on the real frame or on
// its generated twin, and only the paired cadence has that second degree of freedom.
static void test_lock_reseed_stall_paired_cadence() {
    int worstRun = 0, badPhases = 0, totalPhases = 0;
    int64_t worstPhase = 0;
    int worstParity = 0;
    for (int parity = 0; parity < 2; parity++) {
        for (int k = 0; k < 21; k++) {
            SimParams p;
            p.srcPeriod = 16667;
            p.presentPeriod = 16667;
            p.arrivalJitter = 0;
            p.combQpc = 16667;
            p.presents = 700;
            p.phaseOffset = 0;
            p.passthroughQpc = 4166;
            p.lagOverride = 0;
            p.stallAtArrival = 300 + parity;
            p.stallArrivals = 2;         // 2 timeout re-grabs = ~200 ms frozen
            p.stallGap = 100000;         // NvFBC's grab timeout
            p.postStallPhase = (int64_t)k * 16667 / 21;
            p.periodPattern = { 16250, 400 };   // the real/generated pair
            const SimResult r = Simulate(p);

            size_t stallEnd = 0;
            int wide = 0;
            for (size_t i = 0; i < r.span.size(); i++) {
                if (r.span[i] < 0 || r.span[i] > p.srcPeriod * 2) stallEnd = i;
                if (r.span[i] > p.srcPeriod * 2) wide++;
            }
            CHECK(wide > 0, "paired cadence: the stall was not simulated (phase %d)", k);

            int run = 0, best = 0;
            for (size_t i = stallEnd + 1; i < r.ops.size() && i <= stallEnd + 250; i++) {
                if (r.ops[i] == policy::CompositeOp::Synthesize) {
                    run++;
                    if (run > best) best = run;
                } else {
                    run = 0;
                }
            }
            totalPhases++;
            if (best >= 50) badPhases++;
            if (best > worstRun) { worstRun = best; worstPhase = p.postStallPhase; worstParity = parity; }
        }
    }
    std::printf("  paired-cadence stall: worst synth run %d (resume phase %" PRId64
                "us, parity %d), %d of %d phases over 50\n",
                worstRun, worstPhase, worstParity, badPhases, totalPhases);
    CHECK(badPhases == 0,
          "slow recovery on a paired cadence: %d of %d resume phases blended 50+ presents",
          badPhases, totalPhases);
}

// Replay real captures through the real policy. Every .trace in testdata/ is a corpus
// member: drop a capture in (testdata/mktrace.py builds one from an NvFBCR.log)
// and it becomes a permanent gate.
//
// WHY A CORPUS AND NOT A SCENARIO. Synthetic stalls only contain the shapes someone
// thought to model, and all of them pass while hardware still blends for seconds after
// roughly one stall in seven. Worse, a single captured event is not a gate either: a
// candidate stall fix took one capture's worst event from 189 presents to nothing while
// quadrupling long recoveries across the whole session by suppressing a third of all
// re-seeds. Only whole-session numbers, over more than one capture, catch that.
//
// EACH FIXTURE CARRIES ITS OWN FIDELITY CHECK. A fixture records what the RELAY did on
// that capture, and the replay must land near it. The model is not exact (it drives the
// policy directly rather than reproducing every ring effect), so the per-fixture
// tolerance is explicit: a configuration where the model cannot track the hardware must
// widen it deliberately, in the fixture, where the next reader can see it.
struct TraceFixture {
    std::string path;
    std::string name;
    std::string description;
    std::vector<int64_t> arrivals;
    std::vector<int64_t> presents;
    // Head-0 scanout times in DELIVERY order, and the instant each first became knowable
    // to the relay. Both empty for a fixture built from a log with no ETW; flipKnown alone
    // is empty for one predating lag=, which can drive a pairing rule but not a timing one.
    // Delivery order is deliberate: sorting by scanout time would erase the very lateness
    // the coherence rule exists to handle.
    std::vector<int64_t> flipDisplay;
    std::vector<int64_t> flipKnown;
    // Bounds on the REPLAY. Inequalities, so an improvement passes and only a
    // regression fails. Absent (-1) means the fixture is unblessed: the test prints the
    // measured lines and fails, so a new capture cannot silently join as a no-op.
    int maxWorstRun = -1;
    int maxLongRuns = -1;
    double maxSynthPct = -1.0;
    int minReseeds = -1;
    // What the relay actually did, and how far the replay may sit from it.
    int fieldWorstRun = -1;
    int fieldLongRuns = -1;
    double fieldSynthPct = -1.0;
    // Pairing gates, overridable per fixture because the defaults encode the x2 walks:
    // 98% placed assumes the flip grid never pauses, but with frame generation OFF a
    // stalled game stops presenting and the grid stops WITH it, so its transition batches
    // have nothing to pair against - a regime property, not a join regression. A fixture
    // that overrides these owns the explanation in a comment beside the override.
    int minPlacedPct = 98;
    int maxAheadPct = 2;
    // Stage-6 POSITIVE gate: blends this capture's late deliveries cost, which arming the
    // correction must remove. Every other fixture proves dejit does no HARM; without at
    // least one that proves it does its JOB, the whole feature could regress to a no-op and
    // the suite would stay green. -1 = this fixture makes no such claim.
    int minBlendsRemoved = -1;
    // Blends the correction may ADD on this capture. 0 for the regimes where it is trusted;
    // higher where it is measured to churn (see the x3 and FG-off fixtures, which record
    // real numbers rather than pretending to a cleanliness they do not have). -1 disables.
    int maxBlendsAdded = -1;
};

static bool ParseFixture(const std::string& path, TraceFixture* out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out->path = path;
    char tag[64];
    bool ok = true;
    while (std::fscanf(f, "%63s", tag) == 1) {
        auto rest_of_line = [&](std::string* into) {
            int c;
            std::string acc;
            while ((c = std::fgetc(f)) != EOF && c != '\n') {
                if (c != '\r') acc += (char)c;   // fixture may arrive CRLF-checked-out
            }
            size_t i = acc.find_first_not_of(" \t");
            if (into) *into = (i == std::string::npos) ? std::string() : acc.substr(i);
        };
        auto num = [&](long long* v) { if (std::fscanf(f, "%lld", v) != 1) ok = false; };
        auto dbl = [&](double* v) { if (std::fscanf(f, "%lf", v) != 1) ok = false; };
        long long n = 0;
        if (tag[0] == '#')                              { rest_of_line(NULL); }
        else if (std::strcmp(tag, "description") == 0)  { rest_of_line(&out->description); }
        else if (std::strcmp(tag, "max_worst_run") == 0){ num(&n); out->maxWorstRun = (int)n; }
        else if (std::strcmp(tag, "max_long_runs") == 0){ num(&n); out->maxLongRuns = (int)n; }
        else if (std::strcmp(tag, "min_reseeds") == 0)  { num(&n); out->minReseeds = (int)n; }
        else if (std::strcmp(tag, "max_synth_pct") == 0){ dbl(&out->maxSynthPct); }
        else if (std::strcmp(tag, "field_worst_run") == 0) { num(&n); out->fieldWorstRun = (int)n; }
        else if (std::strcmp(tag, "field_long_runs") == 0) { num(&n); out->fieldLongRuns = (int)n; }
        else if (std::strcmp(tag, "field_synth_pct") == 0) { dbl(&out->fieldSynthPct); }
        else if (std::strcmp(tag, "min_placed_pct") == 0)  { num(&n); out->minPlacedPct = (int)n; }
        else if (std::strcmp(tag, "max_ahead_pct") == 0)   { num(&n); out->maxAheadPct = (int)n; }
        else if (std::strcmp(tag, "min_blends_removed") == 0) {
            num(&n); out->minBlendsRemoved = (int)n;
        }
        else if (std::strcmp(tag, "max_blends_added") == 0) {
            num(&n); out->maxBlendsAdded = (int)n;
        }
        else if (std::strcmp(tag, "arrivals") == 0 || std::strcmp(tag, "presents") == 0 ||
                 std::strcmp(tag, "flips_h0") == 0) {
            std::vector<int64_t>* into = &out->presents;
            if (std::strcmp(tag, "arrivals") == 0) into = &out->arrivals;
            else if (std::strcmp(tag, "flips_h0") == 0) into = &out->flipDisplay;
            num(&n);
            if (!ok) break;
            into->clear();
            into->reserve((size_t)n);
            int64_t acc = 0;
            for (long long i = 0; i < n; i++) {
                long long v = 0;
                if (std::fscanf(f, "%lld", &v) != 1) { ok = false; break; }
                acc = (i == 0) ? v : acc + v;
                into->push_back(acc);
            }
        }
        else if (std::strcmp(tag, "flips_h0_delay") == 0) {
            // Raw, not delta-encoded: knowability is not monotonic in delivery order, so
            // a delta chain would be both larger and a lie about the shape of the data.
            num(&n);
            if (!ok) break;
            out->flipKnown.clear();
            out->flipKnown.reserve((size_t)n);
            for (long long i = 0; i < n; i++) {
                long long v = 0;
                if (std::fscanf(f, "%lld", &v) != 1) { ok = false; break; }
                const size_t k = (size_t)i;
                if (k < out->flipDisplay.size()) out->flipKnown.push_back(out->flipDisplay[k] + v);
            }
            // A delay stream that does not line up with its flips is a malformed fixture,
            // not a partially usable one: silently keeping the prefix would give the
            // coherence rule a timeline that is wrong only in its tail.
            if (out->flipKnown.size() != out->flipDisplay.size()) ok = false;
        }
        if (!ok) break;
    }
    std::fclose(f);
    return ok && !out->arrivals.empty() && !out->presents.empty();
}

// Fixtures live beside this source, so they resolve from the source tree rather than
// from whatever directory the test binary was launched in.
static std::string TestDataDir() {
    std::string here(__FILE__);
    const size_t cut = here.find_last_of("/\\");
    return ((cut == std::string::npos) ? std::string() : here.substr(0, cut + 1)) + "testdata/";
}

static std::vector<std::string> FixturePaths() {
    std::vector<std::string> out;
    std::string dir = TestDataDir();
    // No <filesystem>: an index file also documents the corpus in one readable place.
    std::FILE* idx = std::fopen((dir + "index.txt").c_str(), "rb");
    if (!idx) {   // out-of-tree build: fall back to the path from the repo root
        dir = "samples/NvFBC/NvFBCR/testdata/";
        idx = std::fopen((dir + "index.txt").c_str(), "rb");
    }
    if (!idx) return out;
    char name[256];
    while (std::fscanf(idx, "%255s", name) == 1) {
        if (name[0] == '#') { int c; while ((c = std::fgetc(idx)) != EOF && c != '\n') {} continue; }
        out.push_back(dir + name);
    }
    std::fclose(idx);
    return out;
}

// Run the ROTATION VOTE over a real capture, through the shipped entry points, and check it
// converges exactly where a rotation exists and refuses everywhere else.
//
// THIS TEST EXISTS BECAUSE ITS ABSENCE COST A CAPTURE. The vote shipped with two faults that
// a synthetic test could not see, because the synthetic test generated its inputs from the
// same assumptions the code made: it asked for a batch's anchor flip AT BATCH OPEN, when the
// flip is still ~6 ms from being delivered (98% of batches unplaceable on a real log), and
// it keyed classes by batch INDEX, which drifts from grid position every time the stride
// breaks (~every 89 batches, measured). Both are invisible unless the data comes from a
// capture. So this drives the fixture's own arrivals and flips, with flips entering history
// when they became KNOWABLE, exactly as the relay sees them.
static void ReportRotation(const TraceFixture& fx, int64_t lagFloor, int64_t batchThreshold) {
    if (fx.flipDisplay.empty() || fx.flipKnown.empty()) return;

    // Batches, by the ring's own rule.
    std::vector<int64_t> starts;
    for (size_t i = 0; i < fx.arrivals.size(); i++) {
        if (i == 0 || fx.arrivals[i] - fx.arrivals[i - 1] >= batchThreshold)
            starts.push_back(fx.arrivals[i]);
    }
    if (starts.size() < 200) return;

    // Flips in delivery order, admitted once knowable, plus the bracketing lag - the point
    // in time at which the relay could actually answer for this batch.
    std::vector<std::pair<int64_t, int64_t> > byKnown;   // (known, display)
    for (size_t i = 0; i < fx.flipDisplay.size(); i++)
        byKnown.push_back(std::make_pair(fx.flipKnown[i], fx.flipDisplay[i]));
    std::sort(byKnown.begin(), byKnown.end());

    policy::FlipHistory h;
    policy::RotationPhase rp;
    size_t nf = 0;
    int64_t ema = 0;
    long long observed = 0, validB = 0, keepReal = 0, dropWhole = 0;
    // The regime is the DOMINANT flip rate, not any rate ever glimpsed: every capture opens
    // on the desktop and passes through loading, so a latch on the first x3-looking reading
    // labels an x2 fixture x3 and then demands x3 behaviour of it.
    long long fpsHist[16] = {0};
    for (size_t b = 0; b < starts.size(); b++) {
        const int64_t bs = starts[b];
        while (nf < byKnown.size() && byKnown[nf].first <= bs + lagFloor) {
            policy::Flip f;
            f.displayTs = byKnown[nf].second;
            f.eventTs = byKnown[nf].first;
            f.head = 0;
            h.Add(f);
            nf++;
        }
        const int64_t gap = (b > 0) ? bs - starts[b - 1] : 0;
        if (gap > 0 && gap < 1000000 / 8) ema = ema ? (ema * 7 + gap) / 8 : gap;
        const policy::Flip* newest = h.NewestAtOrBefore(bs + lagFloor, 0);
        if (!newest || ema <= 0) continue;
        const int64_t sp = h.MedianSpacing(newest->displayTs - 200000, newest->displayTs, 0);
        if (sp <= 0) continue;
        const int fps = (int)((16667 + sp / 2) / sp);
        const int stride = (int)((ema + sp / 2) / sp);
        if (fps >= 0 && fps < 16) fpsHist[fps]++;
        if (stride != rp.stride || fps != rp.flipsPerSource)
            policy::RotationReset(rp, stride, fps, kVoteMinSeparationUs);
        // MODEL THE SHIPPING PATH, not an easier one. The relay votes on the batch TWO
        // batches back - a batch's own anchor flip is not delivered when it opens - and then
        // decides for the CURRENT batch by extrapolating the grid position forward. An
        // earlier version of this check voted on the current batch and read rp.gridPos
        // directly, which exercised neither the lag nor the extrapolation: the two faults
        // that cost a capture both lived in exactly that gap.
        static const size_t kVoteLagBatches = 2;
        if (b >= kVoteLagBatches) {
            const int64_t vbs = starts[b - kVoteLagBatches];
            const policy::FlipPairing r = policy::PairBatchMember(h, 0, vbs, 0, 200000);
            if (r.anchorFound) {
                const int64_t anchorTs = vbs - r.anchorOffset;
                const policy::Flip* buf[24];
                int steps = 1;
                if (rp.haveAnchor) {
                    const int n = h.InRange(rp.lastAnchorTs + 1, anchorTs, 0, buf, 24);
                    steps = 0;
                    int64_t prevTs = 0;
                    bool havePrev = false;
                    for (int k = 0; k < n; k++) {          // distinct display times only
                        if (havePrev && buf[k]->displayTs == prevTs) continue;
                        prevTs = buf[k]->displayTs;
                        havePrev = true;
                        steps++;
                    }
                }
                if (policy::RotationAdvance(rp, anchorTs, steps)) {
                    policy::RotationObserve(rp, r.anchorOffset);
                }
            }
        }
        observed++;
        const int pos = policy::RotationPositionAt(rp, bs, sp);
        if (pos >= 0 && rp.valid) {
            validB++;
            const int m = policy::RotationRealMember(rp, pos);
            if (m >= 2) dropWhole++;
            else if (m >= 0) keepReal++;
        }
    }
    if (observed < 200) return;
    int dominantFps = 0;
    for (int i = 1; i < 16; i++) if (fpsHist[i] > fpsHist[dominantFps]) dominantFps = i;
    const double validPct = 100.0 * validB / observed;
    std::printf("    rotation: x%d grid, %.0f%% of %lld observed batches decided"
                " (keeps %lld, drops %lld)\n",
                dominantFps, validPct, observed, keepReal, dropWhole);

    if (dominantFps == 3) {
        // x3 rotates, so the vote must find it. Every x3 capture measured decides on
        // 45-80% of batches; well under a third means the signal was lost, which is what
        // the two shipped faults looked like (0%).
        CHECK(validPct > 30.0,
              "[%s] x3 rotation vote decided only %.0f%% of batches: the rotation is "
              "readable on every x3 capture measured, so this is the vote losing it",
              fx.description.c_str(), validPct);
        // Two keepers and one whole-batch drop per rotation is the tiling; a wildly
        // different ratio means the phase is being read but misapplied.
        const double ratio = dropWhole ? (double)keepReal / (double)dropWhole : 99.0;
        CHECK(ratio > 1.4 && ratio < 2.6,
              "[%s] keep:drop ratio %.2f is not the 2:1 the x3 tiling requires",
              fx.description.c_str(), ratio);
    } else {
        // NOTHING ELSE MAY DECIDE. x2 and frame-generation-off have no rotation, and a
        // vote that finds one would retract the wrong member on the path that ships.
        CHECK(validB == 0,
              "[%s] the rotation vote decided on %lld batches in a regime that does not "
              "rotate: this is the x2 keep-real path and it must stay inert",
              fx.description.c_str(), validB);
    }
}

// Replay the join over a real capture, on the relay's own terms: a flip enters history when
// the relay could FIRST HAVE KNOWN it, not when it happened, and a capture is placed on the
// grid one bracketing lag after it arrived, which is when the policy first needs its stamp.
// Feeding history from display times instead would answer a question nobody is asking - it
// would measure whether the grid is joinable in hindsight, which was never in doubt.
static void ReportPairing(const TraceFixture& fx, int64_t lagFloor, int64_t batchThreshold) {
    if (fx.flipDisplay.empty()) return;

    struct Attempt { int64_t at; int64_t batchStart; int member; };
    std::vector<Attempt> pend;
    pend.reserve(fx.arrivals.size());
    policy::BatchState bs;
    for (size_t i = 0; i < fx.arrivals.size(); i++) {
        const policy::BatchDecision d = policy::UpdateBatch(bs, fx.arrivals[i], batchThreshold);
        pend.push_back({fx.arrivals[i] + lagFloor, d.stampTs, d.member});
    }

    policy::FlipHistory h;
    size_t fi = 0, pi = 0;
    int paired = 0, ahead = 0, gap = 0, noAnchor = 0;
    std::vector<int64_t> offsets;
    offsets.reserve(pend.size());
    // Cadence window in the fixture's microseconds; the confidence bound derives from the
    // measured step inside PairBatchMember. A FIXED bound was tried and does not survive a
    // second capture: 1 ms placed 99.2% on one 60x2 walk and 91.5% on another whose render
    // times wobbled more, because with G-SYNC the scanout grid follows the render rate.
    const int64_t bound = 200000;

    while (pi < pend.size()) {
        const int64_t now = pend[pi].at;
        while (fi < fx.flipDisplay.size()) {
            const int64_t known = fx.flipKnown.empty() ? fx.flipDisplay[fi] : fx.flipKnown[fi];
            if (known > now) break;
            policy::Flip f;
            f.displayTs = fx.flipDisplay[fi];
            f.eventTs = known;
            f.head = 0;
            h.Add(f);
            fi++;
        }
        const policy::FlipPairing r =
            policy::PairBatchMember(h, 0, pend[pi].batchStart, pend[pi].member, bound);
        if (r.paired) { paired++; offsets.push_back(r.anchorOffset); }
        else if (r.memberAhead) ahead++;
        else if (r.gridGap) gap++;
        else noAnchor++;
        pi++;
    }

    std::sort(offsets.begin(), offsets.end());
    const int64_t p50 = offsets.empty() ? 0 : offsets[offsets.size() / 2];
    const int64_t p95 = offsets.empty() ? 0 : offsets[(size_t)(offsets.size() * 0.95)];
    const size_t total = pend.size();
    std::printf("    pairing: %.1f%% placed (p50 +%lld us, p95 +%lld us), "
                "unpaired %.1f%% not-yet-announced / %.1f%% grid gap / %.1f%% no anchor\n",
                100.0 * paired / (double)total, (long long)p50, (long long)p95,
                100.0 * ahead / (double)total, 100.0 * gap / (double)total,
                100.0 * noAnchor / (double)total);

    // Measured 99.5% on the walk fixture, so 98 is a regression gate rather than a hopeful
    // one (the default; a fixture whose REGIME cannot meet it declares its own bound with
    // the reason - see TraceFixture::minPlacedPct). The premise of the whole design is that
    // the flip data arrives in time at the CURRENT lag floor; if that stops holding the
    // number should be argued about, not quietly accepted.
    CHECK(paired * 100 / (int)total >= fx.minPlacedPct,
          "%s: only %d of %zu captures placed on the flip grid (bound %d%%); the join does "
          "not hold at a %lld us floor", fx.path.c_str(), paired, total, fx.minPlacedPct,
          (long long)lagFloor);
    // Not-yet-announced is the failure the lag floor governs. It measured 0.0%, which is the
    // evidence that the floor needs no raising; a nonzero reading here is the first sign that
    // conclusion has expired.
    CHECK(ahead * 100 / (int)total <= fx.maxAheadPct,
          "%s: %d of %zu captures had no flip announced in time (bound %d%%); the lag floor "
          "conclusion no longer holds", fx.path.c_str(), ahead, total, fx.maxAheadPct);
}

static void test_replay_capture_corpus() {
    const std::vector<std::string> paths = FixturePaths();
    // Loud, never a skip: a corpus gate that quietly vanishes leaves the suite green
    // while the thing it guards regresses. Both lookup paths are relative to the repo
    // root, which is where CI and the documented local command both run.
    CHECK(!paths.empty(),
          "no replay fixtures found (looked for %sindex.txt and "
          "samples/NvFBC/NvFBCR/testdata/index.txt). Run the suite from the repository "
          "root. The capture corpus is the only gate that has caught a real pacing "
          "regression, so it is a failure rather than a skip.",
          TestDataDir().c_str());

    for (const std::string& path : paths) {
        TraceFixture fx;
        CHECK(ParseFixture(path, &fx), "fixture unreadable or malformed: %s", path.c_str());

        // Flip data has no consumer yet, so nothing else would notice a decoding mistake.
        // These are the shape checks that hold for any ETW capture of a frame-generated
        // source, so they fail on a corrupt stream rather than only on an implausible one.
        if (!fx.flipDisplay.empty()) {
            std::vector<int64_t> gaps;
            for (size_t i = 1; i < fx.flipDisplay.size(); i++) {
                const int64_t d = fx.flipDisplay[i] - fx.flipDisplay[i - 1];
                if (d > 0 && d < 40000) gaps.push_back(d);
            }
            CHECK(gaps.size() > fx.flipDisplay.size() / 2,
                  "%s: only %zu of %zu flip gaps are plausible; the stream is not a scanout "
                  "grid", path.c_str(), gaps.size(), fx.flipDisplay.size());
            std::sort(gaps.begin(), gaps.end());
            const int64_t medGap = gaps[gaps.size() / 2];
            CHECK(medGap > 2000 && medGap < 20000,
                  "%s: median flip spacing %lld us is outside any sane refresh rate",
                  path.c_str(), (long long)medGap);
            if (!fx.flipKnown.empty()) {
                CHECK(fx.flipKnown.size() == fx.flipDisplay.size(),
                      "%s: %zu knowability stamps for %zu flips", path.c_str(),
                      fx.flipKnown.size(), fx.flipDisplay.size());
                // The driver stamps the event as it assigns the flip time and delivery only
                // adds delay, so learning of a flip BEFORE it happened means the two streams
                // were joined wrong.
                std::vector<int64_t> delay;
                for (size_t i = 0; i < fx.flipKnown.size(); i++)
                    delay.push_back(fx.flipKnown[i] - fx.flipDisplay[i]);
                std::sort(delay.begin(), delay.end());
                const int64_t medDelay = delay[delay.size() / 2];
                CHECK(medDelay > 0 && medDelay < 100000,
                      "%s: median knowability delay %lld us is not a delivery latency",
                      path.c_str(), (long long)medDelay);
                std::printf("  flips [%s]: %zu head-0, spacing %lld us (%.1f Hz), "
                            "known +%lld us median\n",
                            fx.description.c_str(), fx.flipDisplay.size(),
                            (long long)medGap, 1e6 / (double)medGap, (long long)medDelay);
            }
            // 20833 us is today's floor at 60 fps (srcPeriod * 1.25); 3000 us is the batch
            // threshold CaptureRing uses. Both are the shipping values, so this reports what
            // the relay would actually achieve rather than what a tuned one could.
            ReportPairing(fx, 20833, 3000);
            ReportRotation(fx, 20833, 3000);
        }

        SimParams p;
        p.srcPeriod = 16667;
        p.presentPeriod = 16667;
        p.arrivalJitter = 0;          // the capture carries the real jitter
        p.combQpc = 16667;
        p.presents = 0;               // explicitPresents drives the count
        p.phaseOffset = 0;
        p.passthroughQpc = 4166;
        p.lagOverride = 0;
        // Guard deliberately ARMED (production's rule) even though every fixture predates
        // it: at a present clock matching the source rate the target advances a full
        // period every present, so the guard must not change one decision here, and the
        // corpus pins verify that on real captures. An earlier output-stamp-based guard
        // failed exactly this gate - late-stamped deliveries read as sub-tooth advances
        // and a quarter of one fixture's genuine hole covers demoted - which is why the
        // guard measures target advance instead.
        p.explicitArrivals = fx.arrivals;
        p.explicitPresents = fx.presents;
        const SimResult r = Simulate(p);

        // Must match mktrace.py's WARMUP: the lock starts cold here where the relay had
        // been running for minutes, so the head of a replay is a window artifact.
        const size_t kWarmup = 200;
        int run = 0, worst = 0, longRuns = 0, synth = 0, reseeds = 0;
        for (size_t i = kWarmup; i < r.ops.size(); i++) {
            if (r.ops[i] == policy::CompositeOp::Synthesize) {
                synth++; run++;
                if (run > worst) worst = run;
            } else {
                if (run >= 50) longRuns++;
                run = 0;
            }
        }
        if (run >= 50) longRuns++;
        for (size_t i = kWarmup; i < r.snapped.size(); i++) if (r.snapped[i]) reseeds++;
        const double synthPct = 100.0 * synth / (double)(r.ops.size() - kWarmup);

        // The fixture's own time window, printed because every wrong verdict this project has
        // reached came from comparing two populations windowed differently. Twice it was a
        // corpus number set beside a whole-run log summary: a fixture trimmed to gameplay was
        // read against a log covering the desktop at both ends, and the difference was
        // reported as a model divergence that did not exist. The rule "window every capture
        // identically" was already written down both times. A number on the line is harder to
        // skip than a rule you have to remember, so before setting anything here beside a
        // figure from an NvFBCR.log, check the log covers THIS span and no more.
        const double spanStart = fx.presents.empty() ? 0.0 : fx.presents.front() / 1e6;
        const double spanEnd = fx.presents.empty() ? 0.0 : fx.presents.back() / 1e6;
        std::printf("  corpus [%s]\n"
                    "    window: log t %.1fs..%.1fs (%.1f min, %zu presents)\n"
                    "    replay: synth %.1f%%, runs>=50 %d, worst %d, re-seeds %d\n",
                    fx.description.empty() ? path.c_str() : fx.description.c_str(),
                    spanStart, spanEnd, (spanEnd - spanStart) / 60.0, fx.presents.size(),
                    synthPct, longRuns, worst, reseeds);
        if (fx.fieldWorstRun >= 0) {
            std::printf("    field : synth %.1f%%, runs>=50 %d, worst %d\n",
                        fx.fieldSynthPct, fx.fieldLongRuns, fx.fieldWorstRun);
        }

        if (fx.maxWorstRun < 0) {
            CHECK(false,
                  "fixture %s has no bounds. Compare the replay line above against the "
                  "field line: if they disagree materially the model is missing something "
                  "this capture exercises, and bounds would be theatre. If they agree, "
                  "paste:\n"
                  "        max_worst_run %d\n        max_long_runs %d\n"
                  "        max_synth_pct %.1f\n        min_reseeds %d",
                  path.c_str(), worst, longRuns, synthPct + 0.4, (int)(reseeds * 0.97));
            continue;
        }

        // The field numbers are printed, never asserted. They are fidelity evidence for
        // the policy the capture was taken WITH, so they are checked once by a human at
        // blessing time. Asserting them continuously would invert as soon as someone
        // lands a real improvement: the replay would rightly diverge downward from
        // recorded hardware behaviour and a fidelity check would call that a failure.
        // Regressions are caught by the bounds below, which do not have that problem.

        CHECK(worst <= fx.maxWorstRun,
              "[%s] worst recovery grew to %d presents (bound %d)",
              fx.description.c_str(), worst, fx.maxWorstRun);
        CHECK(longRuns <= fx.maxLongRuns,
              "[%s] long recoveries grew to %d (bound %d)",
              fx.description.c_str(), longRuns, fx.maxLongRuns);
        CHECK(synthPct <= fx.maxSynthPct,
              "[%s] synth share grew to %.1f%% (bound %.1f%%)",
              fx.description.c_str(), synthPct, fx.maxSynthPct);
        CHECK(reseeds >= fx.minReseeds,
              "[%s] re-seeds fell to %d (bound %d): a suppressed re-seed means a stall "
              "recovers by slew instead of snapping",
              fx.description.c_str(), reseeds, fx.minReseeds);

        // STAGE 6 GATE: replay the same capture with delivery-lateness correction armed.
        // The design constraint is DO NO HARM on a steady capture: on-time batches are
        // untouched by construction, so a steady walk must replay inside its own bounds.
        // (A naive flip-stamp-everything variant was tried first and measured 4.4-5.0%
        // synth against these same fixtures' 1.5-1.8% bounds: moving every stamp +8.33 ms
        // onto the flip base inherits real scanout jitter everywhere. The correction form
        // keeps the arrival base and touches only the late outliers.) The WIN - phantom
        // blends collapsing - is gated by fixtures carrying real late-delivery events;
        // a steady walk cannot show it and is not asked to.
        if (!fx.flipDisplay.empty() && !fx.flipKnown.empty()) {
            SimParams p6 = p;
            p6.flipDisplay = fx.flipDisplay;
            p6.flipKnown = fx.flipKnown;
            p6.flipDejitter = true;
            const SimResult r6 = Simulate(p6);
            int run6 = 0, worst6 = 0, longRuns6 = 0, synth6 = 0;
            for (size_t i = kWarmup; i < r6.ops.size(); i++) {
                if (r6.ops[i] == policy::CompositeOp::Synthesize) {
                    synth6++; run6++;
                    if (run6 > worst6) worst6 = run6;
                } else {
                    if (run6 >= 50) longRuns6++;
                    run6 = 0;
                }
            }
            if (run6 >= 50) longRuns6++;
            const double synthPct6 = 100.0 * synth6 / (double)(r6.ops.size() - kWarmup);
            std::printf("    stage6 dejitter: synth %.1f%%, runs>=50 %d, worst %d; "
                        "batches %lld anchored %lld late %lld corrected %lld "
                        "fence-blocked %lld lock-declined %lld\n",
                        synthPct6, longRuns6, worst6, r6.measuredBatches,
                        r6.anchoredBatches, r6.lateBatches, r6.correctedBatches,
                        r6.fenceBlockedBatches, r6.lockDeclinedBatches);
            CHECK(synthPct6 <= fx.maxSynthPct,
                  "[%s] DEJITTERED synth share %.1f%% exceeds the fixture bound %.1f%%: "
                  "the correction is doing harm on a steady capture",
                  fx.description.c_str(), synthPct6, fx.maxSynthPct);
            CHECK(worst6 <= fx.maxWorstRun,
                  "[%s] DEJITTERED worst recovery %d exceeds the fixture bound %d",
                  fx.description.c_str(), worst6, fx.maxWorstRun);
            CHECK(r6.anchoredBatches * 100 >= r6.measuredBatches * 95,
                  "[%s] only %lld of %lld batches anchored: the stride chain is not "
                  "holding on this capture",
                  fx.description.c_str(), r6.anchoredBatches, r6.measuredBatches);

            // PER-PRESENT DIFF, because aggregates cannot see this: one blend removed here
            // and one added there nets to zero, and the aggregate share on map-cycle content
            // is dominated by transition bursts that swamp the isolated gameplay blends
            // entirely. Reading the aggregate is what led this project to conclude the
            // feature did nothing, on a capture where it removed a visible artifact.
            int removed = 0, added = 0, sideSwitch = 0;
            for (size_t i = kWarmup; i < r.ops.size() && i < r6.ops.size(); i++) {
                if (r.ops[i] == r6.ops[i]) continue;
                const bool wasSynth = r.ops[i] == policy::CompositeOp::Synthesize;
                const bool isSynth = r6.ops[i] == policy::CompositeOp::Synthesize;
                if (wasSynth && !isSynth) removed++;
                else if (!wasSynth && isSynth) added++;
                else sideSwitch++;
            }
            std::printf("    stage6 effect: %d blends removed, %d added, %d passthrough "
                        "side switches\n", removed, added, sideSwitch);
            if (fx.maxBlendsAdded >= 0) {
                CHECK(added <= fx.maxBlendsAdded,
                      "[%s] dejit ADDED %d blends (fixture allows %d): the correction is "
                      "moving frames away from targets, not onto them",
                      fx.description.c_str(), added, fx.maxBlendsAdded);
            }
            if (fx.minBlendsRemoved >= 0) {
                CHECK(removed >= fx.minBlendsRemoved,
                      "[%s] dejit removed %d blends, fixture claims at least %d: this "
                      "capture's late deliveries are no longer being corrected",
                      fx.description.c_str(), removed, fx.minBlendsRemoved);
            }
        }
    }
}

// ---------------------------------------------------------------------------------
// Composite (blend-mode) suites. Order no longer matters: every simulation reseeds the
// LCG (see kRngSeed), so a suite's timeline depends only on its own parameters and the
// census pins below are constants of the POLICY alone. New suites may go anywhere.
//
// The threshold convention mirrors the production Setup rule
// T = max(assumed srcP, presentP) / 4. The presentP floor covers the oversampling
// regime: when the source outpaces the present, a real frame is always within
// srcP/2 of the target, so the threshold must exceed srcP/2 for passthrough to
// dominate; at-rate and sub-rate sources get srcP/4.
// ---------------------------------------------------------------------------------

static int64_t ThresholdUs(int64_t srcPeriod, int64_t presentPeriod) {
    const int64_t base = srcPeriod > presentPeriod ? srcPeriod : presentPeriod;
    return base / 4;
}

// Exact op census pin. The LCG is seeded and every suite timeline deterministic, so
// these counts are constants of the policy; any behavioral drift anywhere in a
// regime forces a conscious look, including drift that stays inside the contract
// tolerances asserted beside it. Update a pin only with its diff understood - the
// new number then documents the behavioral change in the commit.
static void PinCensus(const SimResult& r, size_t warmup, int wantPass, int wantSynth,
                      int wantHold, int wantTransitions, const char* suite) {
    int pass = 0, synth = 0, hold = 0, transitions = 0;
    for (size_t i = warmup; i < r.ops.size(); i++) {
        if (IsPass(r.ops[i])) pass++;
        else if (r.ops[i] == policy::CompositeOp::Synthesize) synth++;
        else hold++;
        if (i > warmup && IsPass(r.ops[i]) != IsPass(r.ops[i - 1])) transitions++;
    }
    if (pass != wantPass || synth != wantSynth || hold != wantHold ||
        transitions != wantTransitions) {
        g_failures++;
        std::printf("FAIL %s census: pass %d synth %d hold %d transitions %d"
                    " (pinned %d/%d/%d/%d)\n", suite, pass, synth, hold, transitions,
                    wantPass, wantSynth, wantHold, wantTransitions);
    }
}

// The production corner the lock exists for: 60-in-60-out with a slow beat. Locked,
// nearly every present has a real frame at the target and passes through sharp.
static void test_composite_passthrough_at_lock() {
    SimParams p{};
    p.srcPeriod = 16672;
    p.presentPeriod = 16667;
    p.arrivalJitter = 300;
    p.combQpc = 16672;
    p.presents = 12000;
    p.passthroughQpc = ThresholdUs(p.srcPeriod, p.presentPeriod);
    SimResult r = Simulate(p);
    const size_t warmup = 3000;   // covers worst-case lock acquisition (~40 s)
    CHECK(r.ops.size() == (size_t)p.presents,
          "composite decisions not recorded (%zu of %lld presents)",
          r.ops.size(), (long long)p.presents);
    int pass = 0, blend = 0, hold = 0, engagedN = 0, total = 0;
    for (size_t i = warmup; i < r.ops.size(); i++) {
        total++;
        if (r.engaged[i]) engagedN++;
        if (IsPass(r.ops[i])) pass++;
        else if (r.ops[i] == policy::CompositeOp::Synthesize) blend++;
        else hold++;
    }
    CHECK(engagedN >= total * 99 / 100, "lock engaged only %d/%d post-warmup", engagedN, total);
    CHECK(pass >= total * 98 / 100, "passthrough %d/%d post-warmup (< 98%%)", pass, total);
    CHECK(blend <= total / 100, "%d blends at lock (> 1%%)", blend);
    CHECK(hold <= 5, "%d holds at lock", hold);
    PinCensus(r, warmup, 9000, 0, 0, 0, "passthrough_at_lock");
}

// Gate placement: at locked operating points the nearest-real-frame distance sits
// far below the threshold, so the pass/blend boundary is unreachable and the
// composition cannot flip-flop. Placement covers the LOCKED regime only - an
// unlocked coherent-clock source can park ON the boundary, which is why the gate
// also carries its one-sided Schmitt band (see test_composite_gate_hysteresis).
static void test_composite_gate_placement() {
    SimParams p{};
    p.srcPeriod = 16672;
    p.presentPeriod = 16667;
    p.arrivalJitter = 300;
    p.combQpc = 16672;
    p.presents = 12000;
    p.passthroughQpc = ThresholdUs(p.srcPeriod, p.presentPeriod);
    SimResult r = Simulate(p);
    const size_t warmup = 3000;
    const int64_t T = p.passthroughQpc;
    int nearBoundary = 0, alternations = 0;
    bool prevPass = true;
    int flips = 0;
    for (size_t i = warmup; i < r.ops.size(); i++) {
        if (!r.engaged[i]) continue;
        if (r.minDiff[i] >= T / 2 && r.minDiff[i] < T + T / 2) nearBoundary++;
        const bool isPass = IsPass(r.ops[i]);
        if (i > warmup && isPass != prevPass) {
            flips++;
            if (flips >= 2) alternations++;
        } else if (isPass == prevPass) {
            flips = 0;
        }
        prevPass = isPass;
    }
    CHECK(nearBoundary == 0, "%d engaged presents inside the threshold boundary band", nearBoundary);
    CHECK(alternations == 0, "%d pass/blend alternation windows while locked", alternations);
    PinCensus(r, warmup, 9000, 0, 0, 0, "gate_placement");
}

// Dropped source frames need no detection. Each isolated drop widens one bracket;
// that present blends at w ~= 0.5, neighbors stay sharp, and the lock never notices
// (the wrapped error is invariant modulo the comb).
static void test_composite_hole_classification() {
    SimParams p{};
    p.srcPeriod = 16672;
    p.presentPeriod = 16667;
    p.arrivalJitter = 300;
    p.combQpc = 16672;
    p.presents = 12000;
    p.passthroughQpc = ThresholdUs(p.srcPeriod, p.presentPeriod);
    const int kHoles = 25;
    for (int i = 0; i < kHoles; i++) p.drops.push_back(3000 + 100 * (int64_t)i);
    SimResult r = Simulate(p);
    const size_t warmup = 2900;
    int blends = 0, holds = 0, isolated = 0, engagedAll = 1;
    double wLo = 1.0, wHi = 0.0;
    for (size_t i = warmup; i < r.ops.size(); i++) {
        if (!r.engaged[i]) engagedAll = 0;
        if (r.ops[i] == policy::CompositeOp::Hold) holds++;
        if (r.ops[i] != policy::CompositeOp::Synthesize) continue;
        blends++;
        if (r.weights[i] < wLo) wLo = r.weights[i];
        if (r.weights[i] > wHi) wHi = r.weights[i];
        if (IsPass(r.ops[i - 1]) && i + 1 < r.ops.size() && IsPass(r.ops[i + 1])) isolated++;
    }
    CHECK(blends == kHoles, "%d blend presents for %d injected holes", blends, kHoles);
    CHECK(isolated == blends, "%d/%d hole blends not isolated", blends - isolated, blends);
    CHECK(holds == 0, "%d holds for single-frame holes (1.25x lag must recover them)", holds);
    CHECK(engagedAll, "lock disturbed by hole injection");
    CHECK(blends == 0 || (wLo >= 0.40 && wHi <= 0.60),
          "hole blend weights [%.3f, %.3f] not centered", wLo, wHi);
    PinCensus(r, warmup, 9075, 25, 0, 50, "hole_classification");
}

// Recovery depth: at 1.25x lag a two-frame hole cannot fully recover. The in-gap
// presents are at most one hold (after endpoint not yet arrived; more lookahead
// only if the pull happens to sit high) followed by one deep blend near w = 2/3;
// neighbors clean, lock undisturbed, output never regresses.
static void test_composite_two_frame_hole() {
    SimParams p{};
    p.srcPeriod = 16672;
    p.presentPeriod = 16667;
    p.arrivalJitter = 300;
    p.combQpc = 16672;
    p.presents = 12000;
    p.passthroughQpc = ThresholdUs(p.srcPeriod, p.presentPeriod);
    p.drops.push_back(6000);
    p.drops.push_back(6001);
    SimResult r = Simulate(p);
    const size_t warmup = 3000;
    std::vector<size_t> nonPass;
    int engagedAll = 1;
    for (size_t i = warmup; i < r.ops.size(); i++) {
        if (!r.engaged[i]) engagedAll = 0;
        if (!IsPass(r.ops[i])) nonPass.push_back(i);
    }
    CHECK(nonPass.size() >= 1 && nonPass.size() <= 2,
          "%zu non-pass presents for a two-frame hole", nonPass.size());
    if (!nonPass.empty()) {
        const size_t last = nonPass.back();
        CHECK(r.ops[last] == policy::CompositeOp::Synthesize,
              "two-frame hole did not end in a recovery blend");
        CHECK(r.weights[last] >= 0.55 && r.weights[last] <= 0.78,
              "recovery blend w=%.3f not near 2/3", r.weights[last]);
        for (size_t i = 0; i + 1 < nonPass.size(); i++) {
            CHECK(nonPass[i] + 1 == nonPass[i + 1], "two-frame hole presents not contiguous");
        }
    }
    CHECK(engagedAll, "lock disturbed by the two-frame hole");
    PinCensus(r, warmup, 8998, 1, 1, 2, "two_frame_hole");
}

// Oversampled source, no lock: the presentP-floored threshold exceeds srcP/2, so a
// real frame is always eligible and passthrough is free. The side-choice Schmitt
// band must reproduce the nearest-mode stride discipline (>= 99% stride-4, no
// period-2 alternation window) with both frames permanently eligible.
static void test_composite_oversampling() {
    SimParams p{};
    p.srcPeriod = 4168;
    p.presentPeriod = 16667;
    p.arrivalJitter = 150;
    p.combQpc = 0;
    p.presents = 7200;
    p.passthroughQpc = ThresholdUs(p.srcPeriod, p.presentPeriod);
    SimResult r = Simulate(p);
    const size_t warmup = 100;
    int pass = 0, total = 0;
    for (size_t i = warmup; i < r.ops.size(); i++) {
        total++;
        if (IsPass(r.ops[i])) pass++;
    }
    CHECK(pass >= total * 99 / 100, "passthrough %d/%d at 240->60 (< 99%%)", pass, total);
    int off = 0, altWindow = 0, worstAltWindow = 0, strides = 0;
    int prevStride = 4;
    for (size_t i = warmup + 1; i < r.outTs.size(); i++) {
        const int64_t delta = r.outTs[i] - r.outTs[i - 1];
        if (delta == 0) continue;
        const int s = (int)((delta + p.srcPeriod / 2) / p.srcPeriod);
        strides++;
        if (s != 4) {
            off++;
            if (prevStride != 4 && s != prevStride) {
                altWindow++;
                if (altWindow > worstAltWindow) worstAltWindow = altWindow;
            }
        } else {
            altWindow = 0;
        }
        prevStride = s;
    }
    CHECK(off <= strides / 100, "%d/%d off-strides (>1%%)", off, strides);
    CHECK(worstAltWindow <= 1, "period-2 output stride window of %d alternations", worstAltWindow);
    PinCensus(r, warmup, 7100, 0, 0, 0, "oversampling");
}

// Refused ratio (144->60, M=5 comb under the jitter floor): the threshold keys off
// the declared source period, not the comb, so it stays well-defined and the
// oversampled geometry still passes real frames through sharp.
static void test_composite_refusal_regime() {
    SimParams p{};
    p.srcPeriod = 6944;
    p.presentPeriod = 16667;
    p.arrivalJitter = 600;
    p.combQpc = 6944 / 5;
    p.presents = 6000;
    p.passthroughQpc = ThresholdUs(p.srcPeriod, p.presentPeriod);
    SimResult r = Simulate(p);
    const size_t warmup = 100;
    int pass = 0, hold = 0, total = 0;
    for (size_t i = warmup; i < r.ops.size(); i++) {
        total++;
        if (IsPass(r.ops[i])) pass++;
        if (r.ops[i] == policy::CompositeOp::Hold) hold++;
    }
    CHECK(pass >= total * 95 / 100, "passthrough %d/%d at refused 144->60 (< 95%%)", pass, total);
    CHECK(hold <= 5, "%d holds at refused 144->60", hold);
    PinCensus(r, warmup, 5900, 0, 0, 0, "refusal_regime");
}

// Unlocked at-rate sweep: the target drifts through the bracket, so blend fires for
// the mid-bracket half of every beat - the soft-output regime, deliberately (this is
// the regime the lock exists to eliminate). The pass fraction tracks the threshold
// geometry (T / (srcP/2) ~= 50%) and transitions stay a small minority of presents
// (boundary chatter is confined to the two threshold crossings per beat).
static void test_composite_unlocked_sweep() {
    SimParams p{};
    p.srcPeriod = 16672;
    p.presentPeriod = 16667;
    p.arrivalJitter = 300;
    p.combQpc = 0;
    p.presents = 12000;
    p.passthroughQpc = ThresholdUs(p.srcPeriod, p.presentPeriod);
    SimResult r = Simulate(p);
    const size_t warmup = 100;
    int pass = 0, blend = 0, transitions = 0, total = 0;
    for (size_t i = warmup; i < r.ops.size(); i++) {
        total++;
        if (IsPass(r.ops[i])) pass++;
        if (r.ops[i] == policy::CompositeOp::Synthesize) blend++;
        if (i > warmup && IsPass(r.ops[i]) != IsPass(r.ops[i - 1])) transitions++;
    }
    CHECK(pass >= total * 35 / 100 && pass <= total * 65 / 100,
          "unlocked pass fraction %d/%d outside the geometric ~50%%", pass, total);
    CHECK(blend >= total * 35 / 100, "unlocked blend fraction %d/%d (< 35%%)", blend, total);
    CHECK(transitions <= total / 10, "%d pass/blend transitions (> 10%%)", transitions);
    PinCensus(r, warmup, 6020, 5880, 0, 8, "unlocked_sweep");
}

// Parked phase at the gate: an unlocked source whose clock is coherent with the
// present clock does not sweep - it parks at whatever phase startup handed it,
// indefinitely. Parked within jitter of the threshold, a memoryless gate flips on
// jitter tails (isolated one-present synths in a sharp stream, each a content-time
// hitch). The gate Schmitt band must pin every parking spot to one stable regime:
// zero pass/synth transitions wherever the phase lands.
static void test_composite_gate_hysteresis() {
    // srcPeriod == presentPeriod exactly: zero skew, the phase parks. With the
    // production lag (srcP + srcP/4) the target sits 3/4 into a source interval, so
    // phaseOffset places the after-frame against the 4166 us threshold: 0 lands
    // exactly on it, -1500 well inside (sharp regime), +1500 well outside (soft
    // regime). Jitter 300 us spans the bare threshold in the parked-on-it case.
    struct { int64_t offset; bool expectPass; const char* label; } cases[] = {
        { 0,     true,  "parked on the threshold" },
        { -1500, true,  "parked inside" },
        { 1500,  false, "parked outside" },
    };
    for (const auto& c : cases) {
        SimParams p{};
        p.srcPeriod = 16667;
        p.presentPeriod = 16667;
        p.arrivalJitter = 300;
        p.combQpc = 0;
        p.presents = 6000;
        p.phaseOffset = c.offset;
        p.passthroughQpc = ThresholdUs(p.srcPeriod, p.presentPeriod);
        SimResult r = Simulate(p);
        const size_t warmup = 100;
        int transitions = 0, pass = 0, total = 0;
        for (size_t i = warmup; i < r.ops.size(); i++) {
            total++;
            if (IsPass(r.ops[i])) pass++;
            if (i > warmup && IsPass(r.ops[i]) != IsPass(r.ops[i - 1])) transitions++;
        }
        CHECK(transitions == 0, "%s: %d pass/synth transitions (gate chatter)",
              c.label, transitions);
        if (c.expectPass) {
            CHECK(pass == total, "%s: %d/%d pass (expected all)", c.label, pass, total);
        } else {
            CHECK(pass == 0, "%s: %d/%d pass (expected none)", c.label, pass, total);
        }
    }
}

// Enabling the composite config must leave the nearest selection byte-identical:
// the shared PolicyConfig is the only coupling surface between the two decision
// paths, and this pins it.
static void test_composite_v16_differential() {
    SimParams p{};
    p.srcPeriod = 16672;
    p.presentPeriod = 16667;
    p.arrivalJitter = 300;
    p.combQpc = 16672;
    p.presents = 12000;
    // Both runs get the same jitter stream because Simulate reseeds; the differential is
    // then purely the composite config.
    p.passthroughQpc = 0;
    SimResult off = Simulate(p);
    p.passthroughQpc = ThresholdUs(p.srcPeriod, p.presentPeriod);
    SimResult on = Simulate(p);
    CHECK(off.picks.size() == on.picks.size(), "differential run sizes diverged");
    for (size_t i = 0; i < off.picks.size() && i < on.picks.size(); i++) {
        CHECK(off.picks[i] == on.picks[i] && off.shownTs[i] == on.shownTs[i] &&
              off.pull[i] == on.pull[i],
              "selection diverged at present %zu with composite config set", i);
        if (g_failures) return;
    }
}

// Permanent ring underrun: a declared source rate far below the actual one sizes the
// lag past the ring window (240 fps actual against a 50 ms lag = 12 frames of depth on
// an 8-slot ring), so every present's before-frame has been evicted. Selection must
// keep showing the ring's oldest frame with monotone output (the "display pinned at
// oldest frame" telemetry class), and the composite must hold - its nearest real frame
// sits beyond the passthrough threshold and synthesis has no bracket.
static void test_ring_underrun_graceful() {
    SimParams p{};
    p.srcPeriod = 4168;
    p.presentPeriod = 16667;
    p.arrivalJitter = 150;
    p.combQpc = 0;
    p.presents = 3000;
    p.lagOverride = 50000;       // the lag a -src 25 declaration would size
    p.passthroughQpc = 10000;    // max(assumed srcP, presentP)/4 for that declaration
    SimResult r = Simulate(p);
    const size_t warmup = 100;
    int noBefore = 0, holds = 0, afterAdv = 0, total = 0;
    for (size_t i = warmup; i < r.ops.size(); i++) {
        total++;
        if (r.beforeDiff[i] < 0) noBefore++;
        if (r.ops[i] == policy::CompositeOp::Hold) holds++;
        if (r.picks[i] == Pick::AfterAdv) afterAdv++;
    }
    CHECK(noBefore >= total * 95 / 100,
          "before-frame present on %d/%d presents (eviction not modeled?)", total - noBefore, total);
    CHECK(holds >= total * 95 / 100, "composite held only %d/%d at underrun", holds, total);
    CHECK(afterAdv >= total * 90 / 100,
          "selection advanced on only %d/%d underrun presents", afterAdv, total);
    for (size_t i = warmup + 1; i < r.shownTs.size(); i++) {
        CHECK(r.shownTs[i] >= r.shownTs[i - 1],
              "shown ts stepped back at underrun present %zu", i);
        if (g_failures) return;
    }
    PinCensus(r, warmup, 0, 0, 2900, 0, "ring_underrun");
}

// The lag-to-depth sizing rule, pinned at the configurations that ship. Until this was a
// function it lived inside TemporalCaptureMode::Setup, a Windows-only translation unit, so
// the rule that decides whether the relay runs 16 slots or 32 could not be tested at all.
static void test_ring_slots_for_lag() {
    const int kMin = 16, kMax = 32;              // CaptureRing's kDefaultRingSlots, RING_SIZE
    const int64_t src60 = 16667;
    const int64_t baseLag60 = src60 + src60 / 4; // LagForSourcePeriod: 1.25 source periods

    // The daily driver: -lag 75 at -src 60. The rule asks for 34 and the ceiling gives 32.
    CHECK(policy::RingSlotsForLag(baseLag60 + 75000, src60, kMin, kMax) == kMax,
          "-lag 75 at 60 fps must size the ring to the maximum, got %d",
          policy::RingSlotsForLag(baseLag60 + 75000, src60, kMin, kMax));

    // No extra lag: the rule asks for 7, and the floor holds it at the default. This is the
    // case that makes resizing unconditional safe, and the reason the default exists.
    CHECK(policy::RingSlotsForLag(baseLag60, src60, kMin, kMax) == kMin,
          "the unextended lag must leave the ring at its default, got %d",
          policy::RingSlotsForLag(baseLag60, src60, kMin, kMax));

    // Between the two the rule is live rather than clamped, so a regression that broke the
    // arithmetic while leaving both ends right would still be caught.
    const int mid = policy::RingSlotsForLag(baseLag60 + 40000, src60, kMin, kMax);
    CHECK(mid > kMin && mid < kMax,
          "-lag 40 at 60 fps must land strictly between the bounds, got %d", mid);

    // A faster declared source shortens the wake period, so the same lag needs more slots:
    // the depth is wake history, not wall-clock reach.
    CHECK(policy::RingSlotsForLag(baseLag60, 8333, kMin, kMax) >
              policy::RingSlotsForLag(baseLag60, src60, kMin, kMax) ||
          policy::RingSlotsForLag(baseLag60, src60, kMin, kMax) == kMin,
          "halving the source period must not ask for fewer slots");

    // Degenerate input must not divide by zero.
    CHECK(policy::RingSlotsForLag(baseLag60, 0, kMin, kMax) == kMax,
          "a zero source period must clamp rather than fault, got %d",
          policy::RingSlotsForLag(baseLag60, 0, kMin, kMax));
}

// WHY the daily driver needs 32 slots, on a timeline rather than by arithmetic. Frame
// generation puts two wakes in every source period, and a retracted member still occupies
// its ring position, so the window covers half as much time as slot count against source
// frames suggests. At the shipping lag a ring of 8 cannot reach its own target.
//
// The arrival pattern here is the [generated, real] pair as the ring receives it. Keep-real
// is not modelled - the point is slot CONSUMPTION, which is identical either way.
static void test_ring_depth_at_shipping_lag() {
    SimParams p{};
    p.srcPeriod = 16667;
    p.presentPeriod = 16667;
    p.arrivalJitter = 150;
    p.combQpc = 0;
    p.presents = 2000;
    p.periodPattern = {351, 16316};     // the measured submission epsilon, then the rest
    p.lagOverride = 16667 + 16667 / 4 + 75000;   // -lag 75 at -src 60
    p.passthroughQpc = 4166;

    const size_t warmup = 200;
    int starved[3] = {0, 0, 0};
    const int depths[3] = {8, 16, 32};
    for (int d = 0; d < 3; d++) {
        p.ringSlots = depths[d];
        const SimResult r = Simulate(p);
        for (size_t i = warmup; i < r.beforeDiff.size(); i++)
            if (r.beforeDiff[i] < 0) starved[d]++;
    }
    const int total = (int)p.presents - (int)warmup;

    CHECK(starved[0] > total / 2,
          "a ring of 8 must starve at the shipping lag (got %d of %d) - if it does not, the "
          "timeline is not reaching back as far as the field's",
          starved[0], total);
    CHECK(starved[2] == 0,
          "a ring of 32 must never starve at the shipping lag, got %d of %d",
          starved[2], total);
    CHECK(starved[1] <= starved[0],
          "depth must be monotonic: 16 starved %d where 8 starved %d",
          starved[1], starved[0]);

    // The sizing rule and the timeline must agree about which depth this configuration
    // needs. Pinning them together is what stops one drifting from the other.
    CHECK(policy::RingSlotsForLag(p.lagOverride, p.srcPeriod, 16, 32) == 32,
          "the sizing rule disagrees with the depth this timeline requires");

    std::printf("  ring depth at shipping lag: starved presents 8-slot %d, 16-slot %d, "
                "32-slot %d (of %d)\n", starved[0], starved[1], starved[2], total);
}

// WHY 16 slots is not enough, which the regular-cadence timeline above cannot show. NvFBC
// does not deliver smoothly: it pauses for around 100 ms and then flushes everything it
// held, consuming a dozen slots in a few milliseconds. Those slots all carry stamps from
// the flush instant, so the frames that bracket a target 95.8 ms in the past are exactly
// the ones a shallow ring evicts. This is the shape behind the field measurement that put
// the hold cliff between 24 and 28 slots.
static void test_ring_depth_under_burst_delivery() {
    const int64_t wake = 8333;        // two wakes per source period, as x2 delivers
    const int64_t pause = 100000;     // the measured NvFBC delivery pause
    const int64_t flushStep = 300;    // the flush itself is far faster than the cadence
    const int64_t burstEvery = 240;   // wakes between bursts: one every 2 s

    SimParams p{};
    p.srcPeriod = 16667;
    p.presentPeriod = 16667;
    p.combQpc = 0;
    p.presents = 2000;
    p.lagOverride = 16667 + 16667 / 4 + 75000;   // -lag 75 at -src 60
    p.passthroughQpc = 4166;

    const int64_t horizon = (p.presents + 4) * p.presentPeriod;
    int64_t t = 0;
    for (int64_t i = 0; t < horizon; i++) {
        if (i > 0 && i % burstEvery == 0) {
            t += pause;
            for (int64_t k = 0; k < pause / wake; k++)
                p.explicitArrivals.push_back(t + k * flushStep);
            t += (pause / wake) * flushStep;
            continue;
        }
        p.explicitArrivals.push_back(t);
        t += wake;
    }

    const size_t warmup = 200;
    int starved[2] = {0, 0};
    const int depths[2] = {16, 32};
    for (int d = 0; d < 2; d++) {
        p.ringSlots = depths[d];
        const SimResult r = Simulate(p);
        for (size_t i = warmup; i < r.beforeDiff.size(); i++)
            if (r.beforeDiff[i] < 0) starved[d]++;
    }

    CHECK(starved[0] > 0,
          "burst delivery must starve a 16-slot ring at the shipping lag - if it does not, "
          "the burst is not consuming slots the way NvFBC does");
    CHECK(starved[1] == 0,
          "a 32-slot ring must survive burst delivery at the shipping lag, got %d starved",
          starved[1]);

    std::printf("  ring depth under burst delivery: starved presents 16-slot %d, "
                "32-slot %d\n", starved[0], starved[1]);
}

// The generated-frame substitution, rule by rule, on hand-built brackets. Every case here
// is a present that WOULD blend: one real frame a full period behind the target, another a
// full period ahead, and a generated frame at the midpoint where the driver put it.
static void test_generated_substitution_rules() {
    PolicyConfig cfg;
    cfg.stickinessQpc = kStickinessUs;
    cfg.passthroughQpc = 4166;

    // A bracket that synthesizes: both endpoints one full period from the target, which is
    // four times the gate away.
    const int64_t target = 100000;
    BracketInfo base;
    base.hasBefore = base.hasAfter = true;
    base.beforeTs = target - 8333;
    base.afterTs = target + 8333;
    base.beforeDiff = 8333;
    base.afterDiff = 8333;

    {   // Disarmed (no candidate offered): the decision must be exactly what it was.
        policy::CompositeState s;
        const policy::CompositeDecision d = policy::DecideComposite(base, s, cfg);
        CHECK(d.op == policy::CompositeOp::Synthesize,
              "without a candidate the bracket must still synthesize, got %s",
              policy::CompositeLabel(d.op));
    }

    {   // Offered and on target: substituted.
        BracketInfo b = base;
        b.hasGen = true;
        b.genUsable = true;
        b.genTs = target;
        b.genDiff = 0;
        policy::CompositeState s;
        CHECK(policy::GeneratedCandidateOnTarget(b, s, cfg),
              "a generated frame ON the target must be a candidate");
        const policy::CompositeDecision d = policy::DecideComposite(b, s, cfg);
        CHECK(d.op == policy::CompositeOp::PassthroughGenerated,
              "a usable generated frame on target must be presented, got %s",
              policy::CompositeLabel(d.op));
        CHECK(s.lastOutputTs == target, "the output time must be the frame's content time");
        CHECK(s.lastGenTs == target, "the substitution must be remembered for the no-reuse rule");
    }

    {   // THE CONTENT CHECK. Same bracket, the caller says the pixels are a race copy of a
        // real frame. Placement still says yes; the decision must not.
        BracketInfo b = base;
        b.hasGen = true;
        b.genUsable = false;
        b.genTs = target;
        b.genDiff = 0;
        policy::CompositeState s;
        CHECK(policy::GeneratedCandidateOnTarget(b, s, cfg),
              "the content check must not change the PLACEMENT answer");
        const policy::CompositeDecision d = policy::DecideComposite(b, s, cfg);
        CHECK(d.op == policy::CompositeOp::Synthesize,
              "a content-rejected frame must fall back to the blend, got %s",
              policy::CompositeLabel(d.op));
    }

    {   // Outside the gate: the geometry that makes the threshold free also makes this the
        // case where no frame sits near the target at all.
        BracketInfo b = base;
        b.hasGen = true;
        b.genUsable = true;
        b.genTs = target - 5000;
        b.genDiff = 5000;
        policy::CompositeState s;
        CHECK(!policy::GeneratedCandidateOnTarget(b, s, cfg), "past the gate must not be a candidate");
        const policy::CompositeDecision d = policy::DecideComposite(b, s, cfg);
        CHECK(d.op == policy::CompositeOp::Synthesize, "past the gate must blend, got %s",
              policy::CompositeLabel(d.op));
    }

    {   // NO REUSE. The same generated frame stays reachable for many presents; showing it
        // twice is a duplicate the content check cannot see, because the pixels are good.
        BracketInfo b = base;
        b.hasGen = true;
        b.genUsable = true;
        b.genTs = target;
        b.genDiff = 0;
        policy::CompositeState s;
        CHECK(policy::DecideComposite(b, s, cfg).op == policy::CompositeOp::PassthroughGenerated,
              "first substitution must happen");
        const policy::CompositeDecision again = policy::DecideComposite(b, s, cfg);
        CHECK(again.op == policy::CompositeOp::Synthesize,
              "the same generated frame must not be presented twice, got %s",
              policy::CompositeLabel(again.op));
    }

    {   // MONOTONE. A generated frame no newer than what was already shown would step the
        // output backward, which the composite never does. The case is built so the BLEND
        // is still fine (it outputs at the target, which advances): only the substitution
        // regresses, so this isolates the substitution's own rule rather than re-testing
        // the general monotone guard.
        BracketInfo b = base;
        b.hasGen = true;
        b.genUsable = true;
        b.genTs = target - 3000;      // inside the gate, but behind what was shown
        b.genDiff = 3000;
        policy::CompositeState s;
        s.lastOutputTs = target - 1000;
        CHECK(!policy::GeneratedCandidateOnTarget(b, s, cfg),
              "a frame older than the last output must not be a candidate");
        const policy::CompositeDecision d = policy::DecideComposite(b, s, cfg);
        CHECK(d.op == policy::CompositeOp::Synthesize,
              "a backward substitution must fall back to the blend, got %s",
              policy::CompositeLabel(d.op));
        CHECK(s.lastGenTs == INT64_MIN,
              "a refused substitution must not be recorded as one");
    }

    {   // A present that is behind the last output entirely still HOLDS: the substitution
        // must not rescue a present the composite has already ruled out.
        BracketInfo b = base;
        b.hasGen = true;
        b.genUsable = true;
        b.genTs = target;
        b.genDiff = 0;
        policy::CompositeState s;
        s.lastOutputTs = target + 1;
        CHECK(policy::DecideComposite(b, s, cfg).op == policy::CompositeOp::Hold,
              "a present behind the last output must hold");
    }

    {   // A present that PASSES THROUGH must never be diverted, however good the candidate.
        BracketInfo b = base;
        b.beforeTs = target - 100;
        b.beforeDiff = 100;
        b.hasGen = true;
        b.genUsable = true;
        b.genTs = target;
        b.genDiff = 0;
        policy::CompositeState s;
        CHECK(!policy::GeneratedCandidateOnTarget(b, s, cfg),
              "a real frame on target leaves nothing to substitute for");
        CHECK(policy::DecideComposite(b, s, cfg).op == policy::CompositeOp::PassthroughBefore,
              "passthrough must win over the substitution");
    }

    {   // A one-sided bracket holds, and holds have no generated frame to promote: the
        // after endpoint has not arrived, so there is nothing to be between.
        BracketInfo b;
        b.hasBefore = true;
        b.beforeTs = target - 50000;
        b.beforeDiff = 50000;
        b.hasGen = true;
        b.genUsable = true;
        b.genTs = target;
        b.genDiff = 0;
        policy::CompositeState s;
        CHECK(!policy::GeneratedCandidateOnTarget(b, s, cfg),
              "an incomplete bracket must not substitute");
        CHECK(policy::DecideComposite(b, s, cfg).op == policy::CompositeOp::Hold,
              "an incomplete bracket must still hold");
    }
}

// WHERE a generated frame is placed, and what dejitter does to that placement. Both rules
// used to live inside CaptureRing, a Windows translation unit that compiles only in CI, so
// neither this suite nor the replay harness could see them - and both shipped with a bug
// that no aggregate would have shown, because a mis-placed substitution looks exactly like
// a correct one in every count the relay logs.
static void test_generated_frame_placement() {
    const int64_t before = 100000, after = 116667;   // one 60 fps source period apart
    int64_t ts = 0;

    // x2: one generated frame, at the midpoint. This is the only value that has been
    // MEASURED (content phase a constant 0.4952), so it is the anchor for the general rule.
    CHECK(policy::PlaceGeneratedFrame(before, after, 0, 2, &ts), "x2 placement must succeed");
    CHECK(ts == (before + after) / 2, "x2 must place at the midpoint, got %lld",
          (long long)(ts - before));

    // x3: thirds, from the same arithmetic and no multiplier detection anywhere.
    CHECK(policy::PlaceGeneratedFrame(before, after, 0, 3, &ts), "x3 member 0 must place");
    CHECK(ts == before + (after - before) / 3, "x3 member 0 must sit a third along");
    CHECK(policy::PlaceGeneratedFrame(before, after, 1, 3, &ts), "x3 member 1 must place");
    CHECK(ts == before + (after - before) * 2 / 3, "x3 member 1 must sit two thirds along");

    // Every placement lands strictly INSIDE its own neighbours, at any multiplier. This is
    // the invariant a substitution depends on: the frame stands in for a blend between
    // these two, so outside them it is not a substitute for anything.
    for (int n = 2; n <= 8; n++) {
        int64_t prev = before;
        for (int j = 0; j < n - 1; j++) {
            CHECK(policy::PlaceGeneratedFrame(before, after, j, n, &ts),
                  "placement %d of %d must succeed", j, n);
            CHECK(ts > before && ts < after,
                  "placement %d of %d escaped its neighbours", j, n);
            CHECK(ts > prev, "placements must increase with member index (%d of %d)", j, n);
            prev = ts;
        }
    }

    // Degenerate intervals are refused rather than placed somewhere arbitrary.
    CHECK(!policy::PlaceGeneratedFrame(after, before, 0, 2, &ts),
          "reversed neighbours must be refused");
    CHECK(!policy::PlaceGeneratedFrame(before, before, 0, 2, &ts),
          "a zero-width interval must be refused");
    CHECK(!policy::PlaceGeneratedFrame(before, after, 0, 1, &ts),
          "a single-member batch has no generated frame to place");
    CHECK(!policy::PlaceGeneratedFrame(before, after, 1, 2, &ts),
          "the last member of a batch is the REAL frame, never a placement");

    // DEJITTER. Corrections are per batch, and the placement spans two batches, so it takes
    // their mean. With dejitter off nothing moves.
    CHECK(policy::PlaceGeneratedFrame(before, after, 0, 2, &ts), "setup");
    CHECK(policy::CorrectGeneratedStamp(ts, 0, 0) == ts,
          "no correction must leave the placement alone");
    CHECK(policy::CorrectGeneratedStamp(ts, 1000, 1000) == ts - 1000,
          "two equal corrections must move the placement by that amount");
    CHECK(policy::CorrectGeneratedStamp(ts, 2000, 0) == ts - 1000,
          "one corrected neighbour must move the placement by half");

    // THE BUG THIS PINS: correcting the endpoints and not the frame between them slides it
    // relative to the interval the passthrough gate measures. Correct all three and the
    // placement must still be the midpoint of the corrected endpoints.
    {
        const int64_t corrNewer = 3000, corrOlder = 0;
        const int64_t correctedBefore = before - corrOlder;
        const int64_t correctedAfter = after - corrNewer;
        int64_t raw = 0;
        CHECK(policy::PlaceGeneratedFrame(before, after, 0, 2, &raw), "setup");
        const int64_t corrected = policy::CorrectGeneratedStamp(raw, corrNewer, corrOlder);
        CHECK(corrected == (correctedBefore + correctedAfter) / 2,
              "a corrected placement must equal the midpoint of the corrected endpoints "
              "(got %lld, want %lld)",
              (long long)corrected, (long long)((correctedBefore + correctedAfter) / 2));
        CHECK(corrected > correctedBefore && corrected < correctedAfter,
              "a corrected placement must stay between its corrected neighbours");
    }
}

// Composite output content time is non-decreasing across pull wraps (the monotone
// guard), on a run long enough to contain several beats.
static void test_composite_monotone_output() {
    SimParams p{};
    p.srcPeriod = 16672;
    p.presentPeriod = 16667;
    p.arrivalJitter = 300;
    p.combQpc = 16672;
    p.presents = 24000;
    p.passthroughQpc = ThresholdUs(p.srcPeriod, p.presentPeriod);
    SimResult r = Simulate(p);
    CHECK(r.wraps >= 1, "no pull wrap in %lld presents; guard unexercised", (long long)p.presents);
    for (size_t i = 1; i < r.outTs.size(); i++) {
        if (r.outTs[i - 1] == INT64_MIN) continue;   // before the first output
        CHECK(r.outTs[i] >= r.outTs[i - 1],
              "composite output regressed at present %zu (%" PRId64 " -> %" PRId64 ")",
              i, r.outTs[i - 1], r.outTs[i]);
        if (g_failures) return;
    }
    PinCensus(r, 100, 23900, 0, 0, 0, "monotone_output");
}

// Lock acquisition traverses the gate band. Parked outside the threshold with the
// lock armed, the pull walks the target onto the comb at the slew rate, so the
// nearest-frame distance migrates through [threshold + band, threshold, operating
// point] while the gate sits in synth state. The one-sided band makes that regime
// change monotone: passing resumes at the bare threshold and, with the mean
// distance still falling, jitter cannot reach the surrender edge a full band
// above - exactly one pass/synth transition for the whole traverse, then a locked
// passing steady state.
static void test_composite_lock_acquisition() {
    SimParams p{};
    p.srcPeriod = 16667;
    p.presentPeriod = 16667;
    p.arrivalJitter = 300;
    p.combQpc = 16667;
    p.presents = 6000;
    p.phaseOffset = 2000;   // unpulled target starts ~6.2 ms from the nearest frame
    p.passthroughQpc = ThresholdUs(p.srcPeriod, p.presentPeriod);
    SimResult r = Simulate(p);
    const size_t warmup = 10;   // ring fill only: the traverse itself is under test
    int transitions = 0;
    for (size_t i = warmup + 1; i < r.ops.size(); i++) {
        if (IsPass(r.ops[i]) != IsPass(r.ops[i - 1])) transitions++;
    }
    CHECK(transitions == 1, "%d pass/synth transitions across lock acquisition", transitions);
    int tailPass = 0;
    for (size_t i = r.ops.size() - 1000; i < r.ops.size(); i++) {
        if (IsPass(r.ops[i])) tailPass++;
    }
    CHECK(tailPass == 1000, "steady state after acquisition: %d/1000 pass", tailPass);
    CHECK(r.engaged.back(), "lock not engaged at the end of the acquisition run");
    // The hold is the tooth guard on the one present after the acquisition pull snap,
    // whose re-phased target sits sub-tooth from the last consumed one: a re-present in
    // place of a blend during re-lock (pre-guard pin: 62 synths, 0 holds).
    PinCensus(r, warmup, 5928, 61, 1, 1, "lock_acquisition");
}

// Display-quantized arrivals: a 90 fps source on a FIXED-REFRESH 240 Hz panel flips
// on the refresh grid, so arrival gaps cycle 2-3-3 refresh periods (8.3/12.5/12.5 ms)
// around the 11.1 ms mean. The quantization does NOT break the M=2 comb: the lock
// engages and holds, and the composite settles into the strict period-2 regime the
// 3:2 geometry forces - alternating pass/synth with weights at the comb midpoint.
// (A VRR panel presents the smooth cadence instead; this pins the harder half.)
static void test_composite_quantized_arrivals() {
    SimParams p{};
    p.srcPeriod = 11111;
    p.presentPeriod = 16667;
    p.arrivalJitter = 100;
    p.combQpc = 11111 / 2;   // the M=2 comb the production ratio scan derives for 90:60
    p.presents = 6000;
    p.periodPattern = {8333, 12500, 12500};
    p.passthroughQpc = ThresholdUs(p.srcPeriod, p.presentPeriod);
    SimResult r = Simulate(p);
    const size_t warmup = 100;
    int engagedN = 0, total = 0;
    double wLo = 1.0, wHi = 0.0;
    for (size_t i = warmup; i < r.ops.size(); i++) {
        total++;
        if (r.engaged[i]) engagedN++;
        if (r.ops[i] != policy::CompositeOp::Synthesize) continue;
        if (r.weights[i] < wLo) wLo = r.weights[i];
        if (r.weights[i] > wHi) wHi = r.weights[i];
    }
    CHECK(engagedN >= total * 95 / 100,
          "M=2 lock engaged only %d/%d on the quantized cadence", engagedN, total);
    CHECK(wLo >= 0.40 && wHi <= 0.65,
          "quantized synth weights [%.3f, %.3f] not near the comb midpoint", wLo, wHi);
    PinCensus(r, warmup, 2950, 2950, 0, 5899, "quantized_arrivals");
}

// THE TOOTH GUARD at a present clock TWICE the source rate. In-game frame generation
// presents through the game's own swapchain, so DWM's compose clock - which the vsync
// present blocks on - runs at the DISPLAYED rate, twice the base. Every other target
// then sits mid-tooth, where the bracket is two ADJACENT real frames and the mid-weight
// blend is a frame the source never produced; a 60 Hz scanout downstream samples one
// parity of the resulting alternation, all-sharp or all-blend by phase luck. Guarded,
// the mid-tooth presents re-present (hold-comb) and every real frame still passes
// through sharp exactly once.
static void test_composite_tooth_guard_double_rate() {
    SimParams p{};
    p.srcPeriod = 16672;
    p.presentPeriod = 8336;
    p.arrivalJitter = 300;
    p.combQpc = 16672;
    p.presents = 12000;
    p.passthroughQpc = ThresholdUs(p.srcPeriod, p.presentPeriod);
    // Off the gate's knife edge: the default lag parks the two parities at exactly a
    // quarter period from the teeth, where the Schmitt band latches BOTH into passing.
    // The field runs nowhere near that edge; the offset puts one parity well inside the
    // gate and the other mid-tooth. The comb lock itself DISENGAGES in this regime (the
    // mid-tooth parity wraps to a half-comb error and trips the stability gate; measured
    // lk=0 pull=0 through every field test window), so the guard must not depend on it.
    p.phaseOffset = 3500;
    SimResult r = Simulate(p);
    const size_t warmup = 3000;
    int pass = 0, synth = 0, holdComb = 0, hold = 0;
    int64_t stepLo = INT64_MAX, stepHi = INT64_MIN;
    int64_t prevPassOut = INT64_MIN;
    for (size_t i = warmup; i < r.ops.size(); i++) {
        if (IsPass(r.ops[i])) {
            pass++;
            if (prevPassOut != INT64_MIN) {
                const int64_t step = r.outTs[i] - prevPassOut;
                if (step < stepLo) stepLo = step;
                if (step > stepHi) stepHi = step;
            }
            prevPassOut = r.outTs[i];
        }
        else if (r.ops[i] == policy::CompositeOp::Synthesize) synth++;
        else if (r.ops[i] == policy::CompositeOp::HoldComb) holdComb++;
        else hold++;
    }
    const int total = pass + synth + holdComb + hold;
    CHECK(synth == 0, "%d mid-tooth blends survived the guard", synth);
    CHECK(hold == 0, "%d plain holds in a clean double-rate run", hold);
    CHECK(pass * 2 >= total * 98 / 100 && pass * 2 <= total * 102 / 100,
          "pass share %d of %d is not the on-tooth half", pass, total);
    // Content still advances one tooth per pass: nothing skipped, nothing repeated.
    CHECK(pass == 0 || (stepLo > 16672 - 2000 && stepHi < 16672 + 2000),
          "pass-to-pass content steps [%lld, %lld] not one source period",
          (long long)stepLo, (long long)stepHi);
}

// Holes are still covered at the doubled present clock: the missing tooth's target
// advances a FULL source period past the last output, which is exactly what the guard's
// cut admits. One blend per hole, mid-tooth neighbors guarded, no plain holds.
static void test_composite_tooth_guard_hole_cover() {
    SimParams p{};
    p.srcPeriod = 16672;
    p.presentPeriod = 8336;
    p.arrivalJitter = 300;
    p.combQpc = 16672;
    p.presents = 12000;
    p.passthroughQpc = ThresholdUs(p.srcPeriod, p.presentPeriod);
    p.phaseOffset = 3500;   // off the knife edge; see the double-rate test
    const int kHoles = 25;
    for (int i = 0; i < kHoles; i++) p.drops.push_back(3000 + 100 * (int64_t)i);
    SimResult r = Simulate(p);
    const size_t warmup = 3000;
    int blends = 0, holds = 0;
    for (size_t i = warmup; i < r.ops.size(); i++) {
        if (r.ops[i] == policy::CompositeOp::Synthesize) blends++;
        if (r.ops[i] == policy::CompositeOp::Hold) holds++;
    }
    CHECK(blends == kHoles, "%d blends for %d injected holes at the doubled clock",
          blends, kHoles);
    // One plain hold per hole: the mid-tooth present BEFORE the missing tooth reaches
    // for an after endpoint two teeth out, which is beyond the 1.25x lag, so the
    // one-sided rule holds it (same re-present the guard would produce one arm later).
    CHECK(holds == kHoles, "%d plain holds for %d holes at the doubled clock",
          holds, kHoles);
}

// Four presents per tooth (a composed 240 Hz desktop over a 60 fps source), zero
// jitter so the run is deterministic. The passthrough gate spans two present periods
// at this ratio, so two or three of the four parities legitimately PASS the same
// tooth (repeats, not regressions) and the guard demotes only the mid-tooth
// remainder; the largest sub-tooth advance (3/4 of a period) stays under the cut.
// What matters is that nothing BLENDS and content still advances one tooth at a time.
static void test_composite_tooth_guard_quad_rate() {
    SimParams p{};
    p.srcPeriod = 16672;
    p.presentPeriod = 4168;
    p.arrivalJitter = 0;
    p.combQpc = 16672;
    p.presents = 12000;
    p.passthroughQpc = ThresholdUs(p.srcPeriod, p.presentPeriod);
    p.phaseOffset = 3500;   // off the knife edge; see the double-rate test
    SimResult r = Simulate(p);
    const size_t warmup = 3000;
    int synth = 0, holdComb = 0, hold = 0, contentSteps = 0;
    int64_t stepLo = INT64_MAX, stepHi = INT64_MIN;
    int64_t prevOut = INT64_MIN;
    for (size_t i = warmup; i < r.ops.size(); i++) {
        if (r.ops[i] == policy::CompositeOp::Synthesize) synth++;
        else if (r.ops[i] == policy::CompositeOp::HoldComb) holdComb++;
        else if (r.ops[i] == policy::CompositeOp::Hold) hold++;
        if (r.outTs[i] != prevOut) {
            if (prevOut != INT64_MIN) {
                const int64_t step = r.outTs[i] - prevOut;
                if (step < stepLo) stepLo = step;
                if (step > stepHi) stepHi = step;
                contentSteps++;
            }
            prevOut = r.outTs[i];
        }
    }
    const int total = (int)(r.ops.size() - warmup);
    CHECK(synth == 0, "%d blends at the quad clock", synth);
    CHECK(hold == 0, "%d plain holds at the quad clock", hold);
    CHECK(holdComb > 0, "the guard never fired at the quad clock");
    CHECK(contentSteps * 4 >= total * 96 / 100 && contentSteps * 4 <= total * 104 / 100,
          "%d content advances over %d presents is not one tooth per four", contentSteps,
          total);
    CHECK(stepLo > 16672 - 200 && stepHi < 16672 + 200,
          "content steps [%lld, %lld] not one source period",
          (long long)stepLo, (long long)stepHi);
}

// The ARMING rule, which decides whether a whole regime blends or re-presents. Pinned
// against the SINK rate rather than the present rate: the first version compared the
// source against the present period, which is correct only while a vsync present happens
// to tick at the sink rate, and silently disarmed the guard under a timer present (b:120
// into the 60 Hz card would have blended every other target - the parity lottery back).
static void test_tooth_guard_arming() {
    const int64_t src60 = 16667, src30 = 33333, src240 = 4166;
    const int64_t sink60 = 16667;
    CHECK(policy::ToothGuardPeriod(src60, sink60, true) == src60,
          "60 fps source into a 60 Hz sink did not arm");
    // The regime this test exists for: present rate is irrelevant, only the sink counts.
    // b:120 and b:vsync-under-frame-generation both land here.
    CHECK(policy::ToothGuardPeriod(src60, sink60, true) == src60,
          "arming must not depend on the present rate");
    CHECK(policy::ToothGuardPeriod(src240, sink60, true) == src240,
          "a source faster than the sink did not arm");
    // Rate conversion stays unguarded: mid-tooth synthesis is that mode's whole output.
    CHECK(policy::ToothGuardPeriod(src30, sink60, true) == 0,
          "a sub-rate source armed the guard, which would kill upconversion");
    // A source declared a whisker off nominal still arms (the eighth of tolerance).
    CHECK(policy::ToothGuardPeriod(17500, sink60, true) == 17500,
          "a source just under nominal fell outside the tolerance");
    CHECK(policy::ToothGuardPeriod(19000, sink60, true) == 0,
          "a source well past the tolerance armed anyway");
    // No comb, no teeth to be between.
    CHECK(policy::ToothGuardPeriod(src60, sink60, false) == 0, "armed without the comb");
    // Unknown/absent inputs never arm, so a failed refresh query degrades to today's rule
    // rather than to a guard running on a garbage period.
    CHECK(policy::ToothGuardPeriod(src60, 0, true) == 0, "armed with an unknown sink");
    CHECK(policy::ToothGuardPeriod(0, sink60, true) == 0, "armed with no declared source");
}

// The guard's exact cut, and its safety rails, pinned against hand-built brackets.
static void test_composite_tooth_guard_boundary() {
    policy::PolicyConfig cfg;
    cfg.stickinessQpc = kStickinessUs;
    cfg.passthroughQpc = 4168;
    cfg.srcPeriodQpc = 16672;
    const int64_t cut = (16672 * 7) / 8;

    // A hole bracket: before shown at 0, after two teeth out. Sweeping the target
    // across the cut flips the decision exactly there.
    policy::BracketInfo hole;
    hole.hasBefore = hole.hasAfter = true;
    hole.beforeTs = 0;
    hole.afterTs = 33344;
    {
        policy::CompositeState s;
        s.lastOutputTs = 0;
        s.lastTargetTs = 0;
        policy::BracketInfo b = hole;
        b.beforeDiff = cut - 1;
        b.afterDiff = hole.afterTs - b.beforeDiff;
        CHECK(policy::DecideComposite(b, s, cfg).op == policy::CompositeOp::HoldComb,
              "one tick under the cut did not demote");
        CHECK(s.lastOutputTs == 0, "a guarded re-present advanced composite state");
    }
    {
        policy::CompositeState s;
        s.lastOutputTs = 0;
        s.lastTargetTs = 0;
        policy::BracketInfo b = hole;
        b.beforeDiff = cut;
        b.afterDiff = hole.afterTs - b.beforeDiff;
        CHECK(policy::DecideComposite(b, s, cfg).op == policy::CompositeOp::Synthesize,
              "the cut itself did not synthesize");
    }
    {
        // Fresh state: INT64_MIN means nothing output yet, which must synthesize
        // rather than demote (and must not overflow the advance arithmetic).
        policy::CompositeState s;
        policy::BracketInfo b = hole;
        b.beforeDiff = 8336;
        b.afterDiff = hole.afterTs - b.beforeDiff;
        CHECK(policy::DecideComposite(b, s, cfg).op == policy::CompositeOp::Synthesize,
              "first output of a run was demoted");
    }
    {
        // The substitution mirror: a generated frame sitting perfectly on a
        // manufactured tooth is refused, and the decision re-presents instead. With
        // the guard disarmed the identical bracket substitutes, pinning that the
        // mirror is the only thing refusing it.
        policy::CompositeState s;
        s.lastOutputTs = 0;
        s.lastTargetTs = 0;
        policy::BracketInfo b;
        b.hasBefore = b.hasAfter = true;
        b.beforeTs = 0;
        b.afterTs = 16672;
        b.beforeDiff = 8336;
        b.afterDiff = 8336;
        b.hasGen = true;
        b.genUsable = true;
        b.genTs = 8336;
        b.genDiff = 0;
        CHECK(!policy::GeneratedCandidateOnTarget(b, s, cfg),
              "generated candidate accepted on a manufactured tooth");
        CHECK(policy::DecideComposite(b, s, cfg).op == policy::CompositeOp::HoldComb,
              "manufactured tooth did not demote with a generated frame reachable");
        policy::PolicyConfig unguarded = cfg;
        unguarded.srcPeriodQpc = 0;
        policy::CompositeState s2;
        s2.lastOutputTs = 0;
        s2.lastTargetTs = 0;
        CHECK(policy::DecideComposite(b, s2, unguarded).op ==
                  policy::CompositeOp::PassthroughGenerated,
              "unguarded control did not substitute");
    }
}

// The pre-guard rule on the identical double-rate run, as a differential: half the
// presents blend at mid weight and nothing re-presents. This is the field shape the
// guard was built against; if the guard path ever leaks into the unguarded rule, or
// the simulator stops producing the pathology, this is what fails.
static void test_composite_tooth_guard_differential() {
    SimParams p{};
    p.srcPeriod = 16672;
    p.presentPeriod = 8336;
    p.arrivalJitter = 300;
    p.combQpc = 16672;
    p.presents = 12000;
    p.passthroughQpc = ThresholdUs(p.srcPeriod, p.presentPeriod);
    p.phaseOffset = 3500;   // off the knife edge; see the double-rate test
    p.noToothGuard = true;
    SimResult r = Simulate(p);
    const size_t warmup = 3000;
    int synth = 0, holdComb = 0, total = 0;
    double wLo = 1.0, wHi = 0.0;
    for (size_t i = warmup; i < r.ops.size(); i++) {
        total++;
        if (r.ops[i] == policy::CompositeOp::HoldComb) holdComb++;
        if (r.ops[i] != policy::CompositeOp::Synthesize) continue;
        synth++;
        if (r.weights[i] < wLo) wLo = r.weights[i];
        if (r.weights[i] > wHi) wHi = r.weights[i];
    }
    CHECK(holdComb == 0, "hold-comb emitted with the guard disabled");
    CHECK(synth * 2 >= total * 96 / 100 && synth * 2 <= total * 104 / 100,
          "unguarded synth share %d of %d is not the mid-tooth half", synth, total);
    CHECK(synth == 0 || (wLo >= 0.35 && wHi <= 0.65),
          "unguarded blend weights [%.3f, %.3f] not mid-tooth", wLo, wHi);
}

// ---------------------------------------------------------------------------------
// Real-log replay: regression pin against a captured NvFBCR.log
// ---------------------------------------------------------------------------------

struct LogLine {
    int64_t tgt, before, after, pull;
    int lk;
    char pick[16];
    bool hasOp;      // blend/interp-mode line: op=/bw= present after mark=
    char op[16];
    double bw;
};

static bool ParseTemporalLine(const char* line, LogLine* out) {
    const char* t = std::strstr(line, "temporal dl=");
    if (!t) return false;
    long long tgt, before, after, pull;
    int lk;
    char pick[16];
    if (std::sscanf(t, "temporal dl=%*dus tgt=%lldus before=%lldus(d%*d) after=%lldus"
                       " w=%*f pick=%15s jit=%*dus pdt=%*dus lag=%*dus pull=%lldus lk=%d",
                    &tgt, &before, &after, pick, &pull, &lk) != 6) {
        return false;
    }
    out->tgt = tgt; out->before = before; out->after = after; out->pull = pull;
    out->lk = lk;
    std::snprintf(out->pick, sizeof(out->pick), "%s", pick);
    out->hasOp = false;
    out->op[0] = '\0';
    out->bw = 0.0;
    const char* o = std::strstr(t, " op=");
    if (o && std::sscanf(o, " op=%15s bw=%lf", out->op, &out->bw) == 2) {
        out->hasOp = true;
    }
    return true;
}

static int Replay(const char* path, int64_t combUs, int64_t passUsArg) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) { std::printf("cannot open %s\n", path); return 1; }
    std::vector<LogLine> lines;
    // Presents whose target fell off the ring back emit only an error line: production
    // selection state still advanced on them, so replay can desync past such a gap.
    size_t unloggedGaps = 0;
    // Blend/interp logs announce their passthrough threshold at Setup; parsing it makes
    // the composite replay self-configuring (--passthrough overrides).
    long long loggedPassUs = 0;
    char buf[1024];
    while (std::fgets(buf, sizeof(buf), f)) {
        LogLine l;
        if (ParseTemporalLine(buf, &l)) lines.push_back(l);
        else if (std::strstr(buf, "target older than ring window")) unloggedGaps++;
        else {
            const char* pt = std::strstr(buf, "passthrough threshold ");
            if (pt) loggedPassUs = std::atoll(pt + 22);
        }
    }
    std::fclose(f);
    std::printf("replay %s: %zu temporal lines\n", path, lines.size());
    if (unloggedGaps) {
        std::printf("  WARNING: %zu ring-underrun present(s) with no temporal line;"
                    " selection state may desync at the gap\n", unloggedGaps);
    }
    if (lines.empty()) {
        std::printf("  no parseable temporal lines: not a temporal-mode log, or the"
                    " format predates the pull=/lk= fields\n");
        return 1;
    }

    PolicyConfig cfg;
    cfg.stickinessQpc = kStickinessUs;
    cfg.phasePullSlewQpc = kSlewUs;
    const size_t warmup = 60;
    size_t mismatches = 0, firstMismatch = 0;

    size_t opLines = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].hasOp) opLines++;
    }

    if (opLines == 0) {
        // Nearest-mode log: pin the selection sequence.
        SelectionState sel;
        for (size_t i = 0; i < lines.size(); i++) {
            const LogLine& l = lines[i];
            BracketInfo b;
            b.hasBefore = true;
            b.beforeTs = l.before;
            b.beforeDiff = l.tgt - l.before;
            b.hasAfter = l.after >= 0;
            b.afterTs = l.after;
            b.afterDiff = l.after - l.tgt;
            const bool prevAfter = sel.lastPickAfter;
            const Pick p = policy::SelectFrame(b, sel, cfg);
            if (i >= warmup && std::strcmp(policy::PickLabel(p), l.pick) != 0) {
                if (!mismatches) firstMismatch = i;
                if (mismatches < 5) {
                    // Production decided in QPC ticks; the log's fields are rounded to whole
                    // microseconds. A band margin within a few us of zero means the decision
                    // sat on the stickiness-band edge and the rounding flipped it: log
                    // quantization, not a policy divergence.
                    const int64_t bias = prevAfter ? -cfg.stickinessQpc : cfg.stickinessQpc;
                    const int64_t margin = b.beforeDiff - (b.afterDiff + bias);
                    std::printf("  MISMATCH line %zu: policy %s, log %s, band margin %" PRId64 " us\n",
                                i, policy::PickLabel(p), l.pick, margin);
                }
                mismatches++;
            }
        }
        std::printf("  selection: %zu/%zu picks match after %zu-line warmup",
                    lines.size() - warmup - mismatches, lines.size() - warmup, warmup);
        if (mismatches) std::printf("  FIRST MISMATCH at line %zu", firstMismatch);
        std::printf("\n");
    } else {
        // Blend/interp-mode log (pick=none throughout: selection never ran): pin the
        // composite op sequence and, on synth lines, the weight.
        cfg.passthroughQpc = (passUsArg > 0) ? passUsArg : loggedPassUs;
        std::printf("  composite-mode log (%zu/%zu lines carry op=), threshold %" PRId64
                    " us (%s)\n", opLines, lines.size(), cfg.passthroughQpc,
                    (passUsArg > 0) ? "--passthrough" : "from the Setup line");
        if (cfg.passthroughQpc <= 0) {
            std::printf("  no threshold found: pass --passthrough <us> (the Setup line was"
                        " not in this log)\n");
            return 1;
        }
        policy::CompositeState comp;
        size_t wMismatches = 0;
        double wWorst = 0.0;
        for (size_t i = 0; i < lines.size(); i++) {
            const LogLine& l = lines[i];
            BracketInfo b;
            b.hasBefore = true;
            b.beforeTs = l.before;
            b.beforeDiff = l.tgt - l.before;
            b.hasAfter = l.after >= 0;
            b.afterTs = l.after;
            b.afterDiff = l.after - l.tgt;
            const policy::CompositeDecision d = policy::DecideComposite(b, comp, cfg);
            if (i < warmup || !l.hasOp) continue;
            if (std::strcmp(policy::CompositeLabel(d.op), l.op) != 0) {
                if (!mismatches) firstMismatch = i;
                if (mismatches < 5) {
                    // The threshold comparison also ran in QPC ticks upstream; a margin
                    // within a few us of zero is log quantization at the gate edge.
                    int64_t md = b.beforeDiff;
                    if (b.hasAfter && b.afterDiff < md) md = b.afterDiff;
                    std::printf("  MISMATCH line %zu: policy %s, log %s, gate margin %" PRId64 " us\n",
                                i, policy::CompositeLabel(d.op), l.op,
                                md - cfg.passthroughQpc);
                }
                mismatches++;
            } else if (d.op == policy::CompositeOp::Synthesize) {
                double dw = d.weight - l.bw;
                if (dw < 0) dw = -dw;
                if (dw > wWorst) wWorst = dw;
                if (dw > 0.005) wMismatches++;
            }
        }
        std::printf("  composite: %zu/%zu ops match after %zu-line warmup",
                    opLines - warmup - mismatches, opLines - warmup, warmup);
        if (mismatches) std::printf("  FIRST MISMATCH at line %zu", firstMismatch);
        std::printf("\n  synth weights: %zu beyond 0.005 of bw=, worst |dw|=%.4f\n",
                    wMismatches, wWorst);
        mismatches += wMismatches;
    }

    if (combUs > 0) {
        PolicyConfig lcfg = cfg;
        lcfg.combQpc = combUs;
        PhaseLockState lock;
        // The pull is an integrator regulated through the PHYSICAL phase, so an
        // open-loop replay accumulates rounding bias without bound. Pin it
        // teacher-forced instead: seed each step with the LOGGED previous pull,
        // predict one step, compare. errEma stays replay-evolved (unobservable in
        // the log); at lock the delta saturates at the slew, where prediction must
        // be exact.
        size_t lkAgree = 0, lkTotal = 0;
        std::vector<int64_t> diffs;
        int64_t prevLoggedPull = 0;
        for (const LogLine& l : lines) {
            if (l.lk < 0) continue;
            lock.pullQpc = prevLoggedPull;
            BracketInfo b;
            b.hasBefore = true;
            b.hasAfter = l.after >= 0;
            if (b.hasBefore && b.hasAfter) {
                policy::UpdatePhaseLock(lock, lcfg, l.tgt - l.before);
            }
            lkTotal++;
            if ((lock.engaged ? 1 : 0) == l.lk) lkAgree++;
            int64_t d = lock.pullQpc - l.pull;
            if (d < 0) d = -d;
            diffs.push_back(d);
            prevLoggedPull = l.pull;
        }
        std::sort(diffs.begin(), diffs.end());
        const int64_t p50 = diffs.empty() ? 0 : diffs[diffs.size() / 2];
        const int64_t p99 = diffs.empty() ? 0 : diffs[(size_t)(diffs.size() * 0.99)];
        const int64_t mx = diffs.empty() ? 0 : diffs.back();
        std::printf("  lock (teacher-forced): lk agrees %zu/%zu; per-step |pull pred - logged|"
                    " p50=%" PRId64 " p99=%" PRId64 " max=%" PRId64 " us\n",
                    lkAgree, lkTotal, p50, p99, mx);
    }
    return mismatches ? 1 : 0;
}

int main(int argc, char** argv) {
    const char* replayPath = nullptr;
    int64_t combUs = 0;
    int64_t passUs = 0;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--replay") && i + 1 < argc) replayPath = argv[++i];
        else if (!std::strcmp(argv[i], "--comb") && i + 1 < argc) combUs = std::atoll(argv[++i]);
        else if (!std::strcmp(argv[i], "--passthrough") && i + 1 < argc) passUs = std::atoll(argv[++i]);
    }
    if (replayPath) return Replay(replayPath, combUs, passUs);

    test_lock_off_matches_v15();
    test_monotonic_and_pull_bounds_across_wrap();
    test_refusal_at_fine_ratio();
    test_hysteresis_no_flip_flop();
    test_advance_gate_no_excursion();
    test_no_excursion_while_locked();
    test_lock_reseed_recovery();

    test_composite_passthrough_at_lock();
    test_composite_gate_placement();
    test_composite_hole_classification();
    test_composite_two_frame_hole();
    test_composite_oversampling();
    test_composite_refusal_regime();
    test_composite_unlocked_sweep();
    test_composite_gate_hysteresis();
    test_composite_v16_differential();
    test_composite_monotone_output();
    test_ring_underrun_graceful();
    test_ring_slots_for_lag();
    test_ring_depth_at_shipping_lag();
    test_ring_depth_under_burst_delivery();
    test_generated_substitution_rules();
    test_generated_frame_placement();
    test_composite_lock_acquisition();
    test_composite_quantized_arrivals();
    test_tooth_guard_arming();
    test_composite_tooth_guard_double_rate();
    test_composite_tooth_guard_hole_cover();
    test_composite_tooth_guard_quad_rate();
    test_composite_tooth_guard_boundary();
    test_composite_tooth_guard_differential();

    test_flip_history();
    test_flip_pairing();
    test_anchor_chain();
    test_keep_decision();
    test_x3_phasekeep_field_failure();
    test_rotation_phase();
    test_dejit_removes_late_blends();
    test_batch_collapse_keep_real();
    test_lock_reseed_wide_bracket_stall();
    test_lock_reseed_stall_paired_cadence();
    test_replay_capture_corpus();

    if (g_failures) {
        std::printf("POLICY TESTS FAILED: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("POLICY TESTS PASSED (7 selection + 18 composite suites)\n");
    return 0;
}
