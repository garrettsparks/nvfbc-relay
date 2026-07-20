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

// Deterministic LCG so every run exercises identical timelines.
static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
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
};

static SimResult Simulate(const SimParams& p) {
    PolicyConfig cfg;
    cfg.stickinessQpc = kStickinessUs;
    cfg.combQpc = p.combQpc;
    cfg.phasePullSlewQpc = kSlewUs;
    cfg.passthroughQpc = p.passthroughQpc;

    int64_t lag = p.srcPeriod + p.srcPeriod / 4;
    if (lag < p.presentPeriod) lag = p.presentPeriod;
    if (p.lagOverride > 0) lag = p.lagOverride;   // a mis-declared source rate, in effect

    // Pre-generate arrivals covering the whole run. Drops are filtered after
    // generation so the jitter stream (and every surviving arrival) is identical
    // with and without hole injection.
    std::vector<int64_t> arrivals;
    const int64_t horizon = (p.presents + 4) * p.presentPeriod;
    for (int64_t t = p.phaseOffset, i = 0; t < horizon; i++) {
        arrivals.push_back(t + JitterUs(p.arrivalJitter));
        t = p.phaseOffset + (i + 1) * p.srcPeriod;
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

    SelectionState sel;
    PhaseLockState lock;
    policy::CompositeState comp;
    SimResult r;
    size_t cursor = 0;
    size_t published = 0;
    int64_t prevPull = 0;
    for (int64_t k = 1; k <= p.presents; k++) {
        const int64_t deadline = k * p.presentPeriod;
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

        while (cursor + 1 < arrivals.size() && arrivals[cursor + 1] <= target) cursor++;
        BracketInfo b;
        b.hasBefore = cursor >= oldest && arrivals[cursor] <= target;
        if (b.hasBefore) {
            b.beforeTs = arrivals[cursor];
            b.beforeDiff = target - b.beforeTs;
        }
        // The after-frame is normally the arrival following the before; when the
        // before was evicted, everything in the window is newer than the target and
        // the nearest-after is the OLDEST visible frame (what production's bracket
        // scan returns while the display is pinned at the ring's tail).
        size_t afterIdx = cursor + 1;
        if (cursor < oldest) afterIdx = oldest;
        b.hasAfter = afterIdx < published;
        if (b.hasAfter) {
            b.afterTs = arrivals[afterIdx];
            b.afterDiff = b.afterTs - target;
        }

        if (cfg.combQpc > 0 && b.hasBefore && b.hasAfter) {
            policy::UpdatePhaseLock(lock, cfg, b.beforeDiff);
            if (lock.pullQpc - prevPull > cfg.combQpc / 2 ||
                prevPull - lock.pullQpc > cfg.combQpc / 2) {
                r.wraps++;
                r.wrapAt.push_back((size_t)(k - 1));
            }
            prevPull = lock.pullQpc;
        }

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

// ---------------------------------------------------------------------------------
// Composite (blend-mode) suites. All run AFTER the selection suites: the shared LCG
// stream means the selection suites' timelines stay byte-identical only if nothing
// before them draws jitter.
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
}

// Gate placement: at locked operating points the nearest-real-frame distance sits
// far below the threshold, so the pass/blend boundary is unreachable and the
// composition cannot flip-flop; the gate needs no hysteresis because placement
// keeps every operating point away from it.
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
    const uint64_t seed = g_rng;
    p.passthroughQpc = 0;
    SimResult off = Simulate(p);
    g_rng = seed;
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

    test_composite_passthrough_at_lock();
    test_composite_gate_placement();
    test_composite_hole_classification();
    test_composite_two_frame_hole();
    test_composite_oversampling();
    test_composite_refusal_regime();
    test_composite_unlocked_sweep();
    test_composite_v16_differential();
    test_composite_monotone_output();
    test_ring_underrun_graceful();

    if (g_failures) {
        std::printf("POLICY TESTS FAILED: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("POLICY TESTS PASSED (6 selection + 10 composite suites)\n");
    return 0;
}
