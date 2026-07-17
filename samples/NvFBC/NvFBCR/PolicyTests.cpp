// Tests-as-spec for TemporalPolicy (docs/policy-extraction-spec.md). Console target,
// deliberately NOT in the Windows solution: pure C++17, no GPU, no timing dependence.
//
//   g++ -std=c++17 -O2 -o policytests PolicyTests.cpp TemporalPolicy.cpp
//   ./policytests                       run the synthetic invariant suite
//   ./policytests --replay NvFBCR.log [--comb <us>]
//                                       pin the policy against a real log's pick=
//                                       sequence (and pull=/lk= when --comb is given)
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
};

struct SimParams {
    int64_t srcPeriod;
    int64_t presentPeriod;
    int64_t arrivalJitter;
    int64_t combQpc;         // 0 = lock off
    int64_t presents;
    int64_t phaseOffset;     // shifts arrival phase vs deadlines
};

static SimResult Simulate(const SimParams& p) {
    PolicyConfig cfg;
    cfg.stickinessQpc = kStickinessUs;
    cfg.combQpc = p.combQpc;
    cfg.phasePullSlewQpc = kSlewUs;

    int64_t lag = p.srcPeriod + p.srcPeriod / 4;
    if (lag < p.presentPeriod) lag = p.presentPeriod;

    // Pre-generate arrivals covering the whole run.
    std::vector<int64_t> arrivals;
    const int64_t horizon = (p.presents + 4) * p.presentPeriod;
    for (int64_t t = p.phaseOffset, i = 0; t < horizon; i++) {
        arrivals.push_back(t + JitterUs(p.arrivalJitter));
        t = p.phaseOffset + (i + 1) * p.srcPeriod;
    }

    SelectionState sel;
    PhaseLockState lock;
    SimResult r;
    size_t cursor = 0;
    int64_t prevPull = 0;
    for (int64_t k = 1; k <= p.presents; k++) {
        const int64_t deadline = k * p.presentPeriod;
        const int64_t target = deadline - (lag + lock.pullQpc);

        while (cursor + 1 < arrivals.size() && arrivals[cursor + 1] <= target) cursor++;
        BracketInfo b;
        b.hasBefore = arrivals[cursor] <= target;
        if (b.hasBefore) {
            b.beforeTs = arrivals[cursor];
            b.beforeDiff = target - b.beforeTs;
        }
        b.hasAfter = cursor + 1 < arrivals.size();
        if (b.hasAfter) {
            b.afterTs = arrivals[cursor + 1];
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
    }
    return r;
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
// Real-log replay: regression pin against a captured NvFBCR.log
// ---------------------------------------------------------------------------------

struct LogLine {
    int64_t tgt, before, after, pull;
    int lk;
    char pick[16];
};

static bool ParseTemporalLine(const char* line, LogLine* out) {
    const char* t = std::strstr(line, "temporal dl=");
    if (!t) return false;
    long long tgt, before, after, pull;
    int lk;
    char pick[16];
    if (std::sscanf(t, "temporal dl=%*lldus tgt=%lldus before=%lldus(d%*d) after=%lldus"
                       " w=%*f pick=%15s jit=%*lldus pdt=%*lldus lag=%*lldus pull=%lldus lk=%d",
                    &tgt, &before, &after, pick, &pull, &lk) != 6) {
        return false;
    }
    out->tgt = tgt; out->before = before; out->after = after; out->pull = pull;
    out->lk = lk;
    std::snprintf(out->pick, sizeof(out->pick), "%s", pick);
    return true;
}

static int Replay(const char* path, int64_t combUs) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) { std::printf("cannot open %s\n", path); return 1; }
    std::vector<LogLine> lines;
    // Presents whose target fell off the ring back emit only an error line: production
    // selection state still advanced on them, so replay can desync past such a gap.
    size_t unloggedGaps = 0;
    char buf[1024];
    while (std::fgets(buf, sizeof(buf), f)) {
        LogLine l;
        if (ParseTemporalLine(buf, &l)) lines.push_back(l);
        else if (std::strstr(buf, "target older than ring window")) unloggedGaps++;
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
    SelectionState sel;
    const size_t warmup = 60;
    size_t mismatches = 0, firstMismatch = 0;
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
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--replay") && i + 1 < argc) replayPath = argv[++i];
        else if (!std::strcmp(argv[i], "--comb") && i + 1 < argc) combUs = std::atoll(argv[++i]);
    }
    if (replayPath) return Replay(replayPath, combUs);

    test_lock_off_matches_v15();
    test_monotonic_and_pull_bounds_across_wrap();
    test_refusal_at_fine_ratio();
    test_hysteresis_no_flip_flop();
    test_advance_gate_no_excursion();
    test_no_excursion_while_locked();

    if (g_failures) {
        std::printf("POLICY TESTS FAILED: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("POLICY TESTS PASSED (6 invariant suites)\n");
    return 0;
}
