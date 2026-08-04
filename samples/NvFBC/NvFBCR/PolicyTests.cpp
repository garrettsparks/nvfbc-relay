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

// Mirrors CaptureRing::RING_SIZE: the bracket sees only the newest kRingSlots arrivals,
// so a target lagging more than the ring window behind loses its before-frame (the
// production ring-underrun class).
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
};

static SimResult Simulate(const SimParams& p) {
    SeedRng();   // each simulation owns its jitter stream; see kRngSeed
    PolicyConfig cfg;
    cfg.stickinessQpc = kStickinessUs;
    cfg.combQpc = p.combQpc;
    cfg.phasePullSlewQpc = kSlewUs;
    cfg.passthroughQpc = p.passthroughQpc;
    cfg.stallSpanQpc = p.srcPeriod * 2;   // production sizes this from the declared source rate

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
    {
        policy::BatchState bs;
        for (size_t i = 0; i < arrivals.size(); i++) {
            const policy::BatchDecision bd =
                policy::UpdateBatch(bs, arrivals[i], p.batchThresholdQpc);
            stamps[i] = bd.stampTs;
            if (bd.retractPrevious && i >= 1) valid[i - 1] = 0;
        }
    }

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
        // kRingSlots back). Both constraints are inert for well-configured timelines -
        // the lag sizing keeps the bracket well inside the window - but load-bearing
        // for hole recovery depth (a widened bracket's after endpoint may not have
        // arrived yet: those presents must hold, not synthesize) and for underruns
        // (a lag past the ring window loses the before-frame entirely).
        while (published < arrivals.size() && arrivals[published] <= deadline) published++;
        const size_t oldest = published > (size_t)kRingSlots ? published - kRingSlots : 0;

        // Nearest valid slot on each side of the target, which is what production's
        // ring scan returns. Retracted slots still occupy their ring position (the
        // write counter advanced), so the window is kRingSlots WAKES wide while only
        // the surviving members are eligible. When everything visible is newer than
        // the target there is no before-frame and the nearest-after is the oldest
        // visible slot, the same result the production scan gives while the display
        // is pinned at the ring's tail.
        BracketInfo b;
        for (size_t i = oldest; i < published; i++) {
            if (!valid[i]) continue;
            const int64_t ts = stamps[i];
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
    // a later binary search over this history would depend on the ordering holding.
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

    // Its twin arrives an epsilon later: same batch, keeps the BATCH-START stamp so the
    // ring timeline stays at base cadence, and retracts the generated member.
    d = policy::UpdateBatch(s, 100400, kThreshold);
    CHECK(d.intraBatch, "a wake 400us later is inside the batch");
    CHECK(d.retractPrevious, "keep-real: the intra-batch member retracts its predecessor");
    CHECK(d.stampTs == 100000, "intra-batch member stamps at batch start, got %lld",
          (long long)d.stampTs);

    // A third member an epsilon on is still inside the batch (the chain rule).
    d = policy::UpdateBatch(s, 100700, kThreshold);
    CHECK(d.intraBatch, "chained third member is still intra-batch");
    CHECK(d.stampTs == 100000, "chained member still stamps at batch start, got %lld",
          (long long)d.stampTs);

    // The next base frame opens a new batch and reports the base period, with the
    // submission epsilon excluded (batch start to batch start, not wake to wake).
    d = policy::UpdateBatch(s, 116667, kThreshold);
    CHECK(!d.intraBatch, "a wake a full source period later opens a new batch");
    CHECK(!d.retractPrevious, "a new batch must not retract the previous batch's keeper");
    CHECK(d.batchGap == 16667, "batch gap is base cadence, got %lld", (long long)d.batchGap);

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

        std::printf("  corpus [%s]\n"
                    "    replay: synth %.1f%%, runs>=50 %d, worst %d, re-seeds %d\n",
                    fx.description.empty() ? path.c_str() : fx.description.c_str(),
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
    PinCensus(r, warmup, 5928, 62, 0, 1, "lock_acquisition");
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
    test_composite_lock_acquisition();
    test_composite_quantized_arrivals();

    test_flip_history();
    test_batch_collapse_keep_real();
    test_lock_reseed_wide_bracket_stall();
    test_lock_reseed_stall_paired_cadence();
    test_replay_capture_corpus();

    if (g_failures) {
        std::printf("POLICY TESTS FAILED: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("POLICY TESTS PASSED (7 selection + 13 composite suites)\n");
    return 0;
}
