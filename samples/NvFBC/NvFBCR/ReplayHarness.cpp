// Replay a captured NvFBCR.log through the REAL policy code (design:
// docs/phasekeep-replay-harness-spec.md).
//
// Why this exists: every x3 phasekeep failure so far was diagnosed offline from a log in
// minutes, AFTER a capture session was spent finding it. Five of them. This moves the
// diagnosis before the capture. It is only worth having if it can say NO, so its first job
// is not measuring anything new - it is reproducing a run whose answer is already known
// (validation gate 1 in the spec), and the harness is untrusted until it does.
//
// Fidelity contract: the DECISIONS come from TemporalPolicy.cpp, linked, never
// reimplemented. What this file reimplements is the WIRING the Windows TUs own - the
// capture loop's batch/EMA/vote sequence (CaptureRing.cpp, the wake loop), the two oracle
// shims (TemporalCaptureMode::Grid and ::AnchorAndSteps) and EtwFlipConsumer's history
// accessors. Those are copied structurally with their source cited at each step, and any
// drift between them and the relay is exactly what gate 1 is built to catch.
//
// Flips enter history when the relay could FIRST HAVE KNOWN them (evt + lag), never when
// they happened. Modelling display order instead is the mistake that cost the first
// capture: 98.2% of batches could not be placed because the vote asked for flip data
// before it was delivered, and a hindsight model shows none of it.
//
// Build (locally, no Windows headers involved):
//   g++ -std=c++17 -O2 -Wall -Werror -o /tmp/replay \
//       samples/NvFBC/NvFBCR/ReplayHarness.cpp samples/NvFBC/NvFBCR/TemporalPolicy.cpp
// Run:
//   /tmp/replay <NvFBCR.log>

#include "TemporalPolicy.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// UNITS: QPC TICKS at 10 MHz, the field's own units, reconstructed from the log's
// microseconds by multiplying by ten. Not microseconds, and the difference is not cosmetic.
//
// The policy layer is unit-agnostic by CONTRACT but not quite in fact: two literals inside it
// carry units. kReorderSlack is documented as such and is slack rather than a threshold, so
// it is harmless either way. kMinSeparation is a THRESHOLD - the class-mean margin the
// rotation vote must clear - written as 80 with a comment reading "microseconds in tests, QPC
// ticks live". Live, therefore, it gates at 80 ticks = 8 us; a harness running in
// microseconds gates at 80 us and demands a TEN TIMES larger margin than the relay did.
//
// That difference is invisible wherever the vote converges comfortably (x3 class separations
// run 120-225 us) and total wherever it barely converges: replaying the reset-storm capture
// in microseconds steers 0 batches against the field's 127. Gate 2 is what caught it.
//
// What is genuinely lost: the log rounds to 1 us, so the model's resolution is 10 ticks
// where the field had 1. Class means average over at least 24 samples so the vote is
// unaffected, and batch splitting (30000 ticks) and the grid (~55000 ticks) are orders away.
// It WOULD matter for the dejitter's 80-tick correction threshold, so dejitter replay must
// not be added here without revisiting it.
const int64_t kTicksPerUs = 10;

namespace {

// ---------------------------------------------------------------------------------
// The log
// ---------------------------------------------------------------------------------

struct FlipRec {
    int64_t display = 0;   // disp=: intended scanout
    int64_t known = 0;     // evt= + lag=: when this process learned of it
    uint32_t head = 0;
    uint32_t token = 0;
};

struct WakeRec {
    int64_t arrival = 0;
    // The vote does NOT run at the arrival instant. CaptureRing's wake loop stamps the
    // batch, then does the StretchRect and blocks on the GPU flush, and only then opens the
    // batch and consults the rotation - so the flip history the vote sees is current as of
    // arrival + flush, and flush is hundreds of microseconds against a 5.6 ms x3 flip step.
    // Modelling the vote at the arrival instant instead costs one reset on the validation
    // capture, which is the whole reason this field is parsed.
    int64_t flush = 0;
    // NvFBC's own change map for this grab: blocks that differ from the previous grab.
    // -1 when the capture ran without the instrument.
    int64_t changedBlocks = -1;
    long long index = 0;    // capture #N, so a divergence can be named
    long long collapsed = -1;   // col=, cumulative; -1 when the log predates the field
};

// One present, from the log's own present timeline.
//
// TWO KINDS OF LINE feed this, and the pair is mutually exclusive in the relay
// (TemporalCaptureMode, the `if (!bracket.info.hasBefore)` branch): a normal present logs
// `temporal dl=... tgt=... pick=...`, while a present whose target fell off the back of the
// ring logs only `temporal: target older than ring window`. So a STARVED PRESENT CARRIES NO
// TIMES AT ALL - which is exactly the population gate 3 measures, and the reason the naive
// "count temporal lines" denominator reads 4613 where the true present count is 9326.
//
// Deadlines for the starved ones are interpolated evenly between the neighbouring logged
// presents. Evenly, not on a 16.67 ms grid: the present clock follows DWM, so it is 60 Hz
// under a fullscreen game and 240 Hz on the composed desktop, and a fixed grid misses 2057
// of 4612 gaps. Even spacing between two known deadlines is self-calibrating instead.
struct PresentRec {
    int64_t deadline = 0;
    int64_t target = 0;
    bool logged = false;      // false = a starvation line, so no times of its own
    bool usable = false;      // an interpolated present with logged neighbours on both sides
    // Ground truth, when logged. NONE OF THIS IS AN INPUT: the target is computed here from
    // the deadline and the harness's OWN comb-lock state, and these are what that
    // computation is scored against. Feeding the log's tgt= in instead would import a value
    // the relay's ring produced (pull is lock state updated from the previous present's
    // bracket), which both leaves the lock unvalidated and makes a ring-size counterfactual
    // run on the target trajectory of the ring size it is being compared to.
    int64_t liveTarget = 0;
    int64_t livePull = 0;
    int64_t beforeTs = 0;
    int64_t afterTs = 0;
    bool hasAfter = false;
    int beforeDepth = -1;
    char pick[16] = {0};
};

// What the relay itself reported, for the harness to be scored against rather than
// trusted. Absent (-1) means this log cannot gate the harness.
struct LiveSummary {
    long long steered = -1;
    long long flipped = -1;
    long long empty = -1;
    long long reclaimed = -1;
    long long undecided = -1;
    long long resets = -1;
    bool present() const { return steered >= 0; }
};

struct Capture {
    std::vector<FlipRec> flips;
    std::vector<WakeRec> wakes;
    std::vector<PresentRec> presents;
    LiveSummary live;
    // Resolved options, read from the log rather than assumed: a harness configured by hand
    // can diverge from the run it is replaying and blame the model for it.
    double srcFps = 0.0;
    int64_t lagUs = 0;
    int64_t stickinessUs = 0;
    int64_t combUs = 0;
    bool phaseKeep = false;
    bool etw = false;
    bool blend = false;
    int64_t passthroughUs = 0;
};

// Payload of a log line: everything past the "[File.cpp:NNN] | " prefix.
const char* Payload(const char* line) {
    const char* bar = std::strstr(line, "| ");
    return bar ? bar + 2 : line;
}

// Read "<key>=<integer>" out of a line. Returns false when the key is absent OR its value
// is not a number, which is what skips the format-documentation lines that carry the same
// keys with "<us>" placeholders.
bool Field(const char* s, const char* key, int64_t* out) {
    const char* at = std::strstr(s, key);
    if (!at) return false;
    at += std::strlen(key);
    char* end = NULL;
    const long long v = std::strtoll(at, &end, 10);
    if (end == at) return false;
    *out = (int64_t)v;
    return true;
}

bool ParseLog(const char* path, Capture* out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return false;
    }
    char line[4096];
    while (std::fgets(line, sizeof(line), f)) {
        const char* p = Payload(line);

        if (std::strncmp(p, "flip ", 5) == 0) {
            FlipRec r;
            int64_t evt = 0, lag = 0, head = 0, token = 0;
            if (!Field(p, "disp=", &r.display)) continue;
            if (!Field(p, "evt=", &evt)) continue;
            // lag= is what makes delivery order modellable at all. A log without it can
            // only be replayed in hindsight, which answers a different question, so it is
            // refused rather than silently approximated.
            if (!Field(p, "lag=", &lag)) continue;
            if (!Field(p, "head=", &head)) continue;
            Field(p, "token=", &token);
            r.display *= kTicksPerUs;
            r.known = (evt + lag) * kTicksPerUs;
            r.head = (uint32_t)head;
            r.token = (uint32_t)token;
            out->flips.push_back(r);
            continue;
        }

        if (std::strncmp(p, "capture #", 9) == 0) {
            WakeRec w;
            int64_t idx = 0, col = 0;
            const char* hash = p + 8;
            char* end = NULL;
            idx = (int64_t)std::strtoll(hash + 1, &end, 10);
            if (end == hash + 1) continue;
            if (!Field(p, "arr=", &w.arrival)) continue;
            Field(p, "flush=", &w.flush);
            w.arrival *= kTicksPerUs;
            w.flush *= kTicksPerUs;
            w.index = idx;
            w.collapsed = Field(p, "col=", &col) ? col : -1;
            // The driver's change map for this grab, -1 when the instrument was off. Zero
            // means this grab returned the same content as the previous one, which is how
            // the ring refuses a capture-race duplicate without touching pixels.
            int64_t diff = -1;
            w.changedBlocks = Field(p, "diff=", &diff) ? diff : -1;
            out->wakes.push_back(w);
            continue;
        }

        if (std::strncmp(p, "temporal dl=", 12) == 0) {
            PresentRec pr;
            int64_t dl = 0, tgt = 0, bef = 0, aft = 0;
            if (!Field(p, "dl=", &dl) || !Field(p, "tgt=", &tgt)) continue;
            int64_t pull = 0;
            Field(p, "pull=", &pull);
            pr.deadline = dl * kTicksPerUs;
            pr.target = tgt * kTicksPerUs;      // interpolation scaffold only
            pr.liveTarget = tgt * kTicksPerUs;  // scored against, never consumed
            pr.livePull = pull * kTicksPerUs;
            if (Field(p, "before=", &bef)) pr.beforeTs = bef * kTicksPerUs;
            if (Field(p, "after=", &aft)) {
                pr.hasAfter = aft != -1;
                pr.afterTs = aft * kTicksPerUs;
            }
            int64_t depth = -1;
            const char* d = std::strstr(p, "us(d");
            if (d) {
                char* e = NULL;
                depth = std::strtoll(d + 4, &e, 10);
                pr.beforeDepth = (int)depth;
            }
            const char* pk = std::strstr(p, "pick=");
            if (pk) {
                pk += 5;
                size_t n = 0;
                while (pk[n] && pk[n] != ' ' && n + 1 < sizeof(pr.pick)) n++;
                std::memcpy(pr.pick, pk, n);
                pr.pick[n] = 0;
            }
            pr.logged = true;
            pr.usable = true;
            out->presents.push_back(pr);
            continue;
        }
        if (std::strstr(p, "target older than ring window")) {
            out->presents.push_back(PresentRec());   // logged=false: times filled in later
            continue;
        }

        // Resolved options and the run's own phasekeep tally.
        if (std::strstr(p, "src rate hint")) {
            const char* at = std::strstr(p, "src rate hint");
            out->srcFps = std::strtod(at + 13, NULL);
            out->phaseKeep = std::strstr(p, "phasekeep ON") != NULL;
            out->etw = std::strstr(p, "etw flip capture on") != NULL &&
                       std::strstr(p, "flip join on") != NULL;
            continue;
        }
        if (std::strstr(p, "Blend compositor ACTIVE") || std::strstr(p, "Interp compositor ACTIVE")) {
            const char* at = std::strstr(p, "threshold");
            if (at) out->passthroughUs = (int64_t)std::strtoll(at + 9, NULL, 10);
            out->blend = true;
            continue;
        }
        if (std::strstr(p, "Temporal lag fixed at")) {
            const char* at = std::strstr(p, "fixed at");
            out->lagUs = (int64_t)std::strtoll(at + 8, NULL, 10);
            continue;
        }
        if (std::strstr(p, "Selection stickiness band:")) {
            const char* at = std::strstr(p, "band:");
            out->stickinessUs = (int64_t)std::strtoll(at + 5, NULL, 10);
            continue;
        }
        if (std::strstr(p, "modulus") && std::strstr(p, "Phase comb lock")) {
            const char* at = std::strstr(p, "modulus");
            out->combUs = (int64_t)std::strtoll(at + 7, NULL, 10);
            continue;
        }
        if (std::strncmp(p, "phasekeep summary:", 18) == 0) {
            LiveSummary& s = out->live;
            long long a = 0, b = 0, c = 0, d = 0, e = 0, g = 0;
            if (std::sscanf(p,
                            "phasekeep summary: %lld batches steered, %lld kept an earlier "
                            "member, %lld all-generated batches dropped, %lld singles "
                            "reclaimed, %lld undecided dropped, %lld vote resets",
                            &a, &b, &c, &d, &e, &g) == 6) {
                s.steered = a;
                s.flipped = b;
                s.empty = c;
                s.reclaimed = d;
                s.undecided = e;
                s.resets = g;
            }
            continue;
        }
    }
    std::fclose(f);
    return true;
}

// Fill in deadlines and targets for the starved presents, which carry no times of their
// own. Interpolated evenly between the nearest logged presents on each side; a run with no
// logged neighbour on one side (the head or tail of the log) is left unusable rather than
// extrapolated, and counted so the omission is visible.
void InterpolatePresents(std::vector<PresentRec>* ps, long long* unusable) {
    std::vector<PresentRec>& v = *ps;
    *unusable = 0;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i].logged) continue;
        size_t lo = i, hi = i;
        while (lo > 0 && !v[lo - 1].logged) lo--;
        while (hi + 1 < v.size() && !v[hi + 1].logged) hi++;
        const bool haveLeft = lo > 0;
        const bool haveRight = hi + 1 < v.size();
        const size_t runLen = hi - lo + 1;
        if (!haveLeft || !haveRight) {
            for (size_t k = lo; k <= hi; k++) *unusable += v[k].usable ? 0 : 1;
            i = hi;
            continue;
        }
        const PresentRec& L = v[lo - 1];
        const PresentRec& R = v[hi + 1];
        for (size_t k = lo; k <= hi; k++) {
            const double f = (double)(k - lo + 1) / (double)(runLen + 1);
            v[k].deadline = L.deadline + (int64_t)((double)(R.deadline - L.deadline) * f);
            v[k].target = L.target + (int64_t)((double)(R.target - L.target) * f);
            v[k].usable = true;
        }
        i = hi;
    }
}

// ---------------------------------------------------------------------------------
// The oracle shims. Structural copies of TemporalCaptureMode::Grid and ::AnchorAndSteps
// plus EtwFlipConsumer::MedianFlipSpacing / ::CountFlipsBetween - the only parts of the
// chain that live in a Windows TU and so cannot be linked. Kept side by side here, and
// deliberately verbose about it, because a silent drift from the relay's versions is the
// one failure mode gate 1 exists to detect.
// ---------------------------------------------------------------------------------

// production's m_flipCadenceWindowQpc = freq / 5, i.e. 200 ms of 10 MHz ticks
const int64_t kCadenceWindow = 200000 * kTicksPerUs;

// EtwFlipConsumer::MedianFlipSpacing: the window ends at the NEWEST flip in history, not
// at the caller's clock, so a stall does not shorten the sample.
int64_t MedianFlipSpacing(const policy::FlipHistory& h) {
    const policy::Flip* newest = h.NewestAtOrBefore(INT64_MAX, 0);
    if (!newest) return 0;
    return h.MedianSpacing(newest->displayTs - kCadenceWindow, newest->displayTs, 0);
}

// EtwFlipConsumer::CountFlipsBetween. DISTINCT display times, not records: the stream
// carries duplicates and counting one would advance the grid position permanently.
int CountFlipsBetween(const policy::FlipHistory& h, int64_t lo, int64_t hi) {
    static const int kCap = 24;   // past this the caller treats the advance as unusable
    const policy::Flip* buf[kCap];
    const int n = h.InRange(lo + 1, hi, 0, buf, kCap);
    int distinct = 0;
    int64_t prev = 0;
    bool havePrev = false;
    for (int i = 0; i < n; i++) {
        if (havePrev && buf[i]->displayTs == prev) continue;
        prev = buf[i]->displayTs;
        havePrev = true;
        distinct++;
    }
    return distinct;
}

// TemporalCaptureMode::Grid. Returning false whenever anything is unknown is what makes
// "no flip data yet" indistinguishable from "cannot rotate" - both mean plain keep-real.
bool Grid(const policy::FlipHistory& h, int64_t assumedSrcPeriod, int64_t batchPeriod,
          int* outStride, int* outFlipsPerSource, int64_t* outSpacing) {
    if (assumedSrcPeriod <= 0 || batchPeriod <= 0) return false;
    const int64_t spacing = MedianFlipSpacing(h);
    if (spacing <= 0) return false;
    *outFlipsPerSource = (int)((assumedSrcPeriod + spacing / 2) / spacing);
    *outStride = (int)((batchPeriod + spacing / 2) / spacing);
    *outSpacing = spacing;
    return true;
}

// TemporalCaptureMode::AnchorAndSteps.
bool AnchorAndSteps(const policy::FlipHistory& h, int64_t batchStartTs, int64_t prevAnchorTs,
                    int64_t* outOffset, int* outSteps) {
    const policy::FlipPairing fp =
        policy::PairBatchMember(h, 0, batchStartTs, 0, kCadenceWindow);
    if (!fp.anchorFound) return false;
    *outOffset = fp.anchorOffset;
    *outSteps = 1;
    if (prevAnchorTs >= 0) {
        const int64_t anchorTs = batchStartTs - fp.anchorOffset;
        *outSteps = CountFlipsBetween(h, prevAnchorTs, anchorTs);
    }
    return true;
}

// ---------------------------------------------------------------------------------
// The capture-side replay: CaptureRing::CaptureLoop's wake sequence.
// ---------------------------------------------------------------------------------

// The ring, exactly as CaptureRing holds it: one array, written by the capture side and read
// by the present side. Invalid slots are SKIPPED BUT STILL OCCUPY A POSITION, which is the
// whole mechanism behind ring-8 starvation - the search window is the newest RING_SIZE-1
// WAKES, not the newest RING_SIZE-1 valid frames, so at x3 (where phasekeep drops a third of
// batches whole, on top of the retracted members) the reachable window holds very few usable
// frames. See CaptureRing.cpp FindBracket.
struct RingModel {
    struct Slot {
        int64_t stamp = 0;
        int64_t batchStart = 0;
        int member = 0;
        bool valid = false;
        // The third slot state: retracted by keep-real, so NOT a bracket endpoint, but its
        // pixels are still in the ring and still reachable. stamp is rewritten to the
        // midpoint of the two real neighbours when the slot enters this state.
        bool gen = false;
    };
    std::vector<Slot> slots;
    long long published = 0;
    int size = 16;

    void Init(int n) {
        size = n;
        slots.assign((size_t)n, Slot());
        published = 0;
    }
    void Write(long long count, int64_t stamp, int64_t batchStart, int member, bool valid) {
        Slot& s = slots[(size_t)(count % size)];
        s.stamp = stamp;
        s.batchStart = batchStart;
        s.member = member;
        s.valid = valid;
        s.gen = false;
        published = count + 1;
    }
    // Keep-real drops the batch's generated member. Production clears the valid bit and the
    // slot is gone; keeping it REACHABLE is what this models, so the count of
    // substitutions it would make can be read off an existing log.
    //
    // The stamp is the midpoint of the two real neighbours, never the generated frame's own
    // flip time: the f/g measurement puts content phase at a constant 0.4952 that does not
    // track the display, and the midpoint rule lands 259 us from it against 587 us sd for
    // the flip time.
    // substitutable mirrors CaptureRing::SubstitutableRegime: the caller measures the
    // delivery structure and says whether a retracted member is a generated frame whose
    // content is at the midpoint. Passing it in rather than recomputing it here keeps the
    // ring model a ring model.
    void Retract(long long count, bool substitutable, int64_t srcPeriod) {
        if (count < 1) return;
        slots[(size_t)((count - 1) % size)].valid = false;
        slots[(size_t)((count - 1) % size)].gen = false;
        if (!substitutable) return;

        const int newMember = slots[(size_t)(count % size)].member;
        const int members = newMember + 1;
        if (newMember < 1) return;
        const int64_t after = slots[(size_t)(count % size)].stamp;
        long long oldest = count - (size - 1);
        if (oldest < 0) oldest = 0;
        int64_t before = 0;
        bool haveBefore = false;
        for (long long i = count - 1 - newMember; i >= oldest; i--) {
            const Slot& s = slots[(size_t)(i % size)];
            if (!s.valid) continue;
            before = s.stamp;
            haveBefore = true;
            break;
        }
        if (!haveBefore || before >= after) return;
        (void)srcPeriod;
        for (int j = 0; j < newMember; j++) {
            Slot& g = slots[(size_t)((count - newMember + j) % size)];
            g.stamp = before + (after - before) * (j + 1) / members;
            g.gen = true;
        }
    }
    void Revalidate(long long count, int64_t stamp) {
        if (count >= 1) {
            Slot& s = slots[(size_t)((count - 1) % size)];
            s.stamp = stamp;
            s.valid = true;
            s.gen = false;
        }
    }
    // The reachable generated frame nearest a target. The window is FindBracket's window,
    // not the whole array: a slot that has aged out of the search span is unreachable
    // whether it is valid or not.
    bool FindGenerated(int64_t target, int64_t* diffOut, int64_t* stampOut,
                       int64_t* depthOut) const {
        long long oldest = published - (size - 1);
        if (oldest < 0) oldest = 0;
        int64_t best = INT64_MAX, bestStamp = 0, bestDepth = 0;
        for (long long i = published - 1; i >= oldest; i--) {
            const Slot& s = slots[(size_t)(i % size)];
            if (!s.gen) continue;
            int64_t d = target - s.stamp;
            if (d < 0) d = -d;
            if (d < best) { best = d; bestStamp = s.stamp; bestDepth = published - 1 - i; }
        }
        if (best == INT64_MAX) return false;
        *diffOut = best;
        *stampOut = bestStamp;
        *depthOut = bestDepth;
        return true;
    }
    // CaptureRing::FindBracket, with the overlay left out (dejitter is off on every capture
    // this harness is pointed at, and its threshold is below the log's resolution).
    void FindBracket(int64_t target, policy::BracketInfo* out, int* beforeDepth) const {
        *out = policy::BracketInfo();
        *beforeDepth = -1;
        long long oldest = published - (size - 1);
        if (oldest < 0) oldest = 0;
        int64_t bestBefore = INT64_MAX, bestAfter = INT64_MAX;
        for (long long i = published - 1; i >= oldest; i--) {
            const Slot& s = slots[(size_t)(i % size)];
            if (!s.valid) continue;
            const int64_t diff = target - s.stamp;
            if (diff >= 0) {
                if (diff < bestBefore) {
                    bestBefore = diff;
                    out->hasBefore = true;
                    out->beforeTs = s.stamp;
                    out->beforeDiff = diff;
                    *beforeDepth = (int)(published - 1 - i);
                }
            } else if (-diff < bestAfter) {
                bestAfter = -diff;
                out->hasAfter = true;
                out->afterTs = s.stamp;
                out->afterDiff = -diff;
            }
        }
    }
};

struct PresentCensus {
    long long presents = 0;
    long long skipped = 0;         // unusable (no logged neighbour to interpolate from)
    long long noBefore = 0;        // gate 3's metric
    long long noAfter = 0;
    long long noBeforeLive = 0;    // what the log itself recorded, for comparison
    long long pickAgree = 0;
    long long pickCompared = 0;
    long long beforeTsAgree = 0;
    long long depthAgree = 0;
    // A boolean match on the before-stamp is too strict to interpret on its own: the log
    // rounds to 1 us, so a member stamped at its own flip (batchStart + member*spacing)
    // inherits any rounding in the measured spacing. Carry the MAGNITUDE so quantisation
    // noise cannot be mistaken for a selection error.
    long long tsDiffSum = 0;
    long long tsDiffMax = 0;
    long long tsDiffOver1ms = 0;
    // The comb lock, PREDICTED here and scored against the log's tgt=/pull=. This is the
    // half that would be untested if the target were taken from the log.
    long long opHold = 0, opPassBefore = 0, opPassAfter = 0, opSynth = 0;
    // GENERATED-FRAME SUBSTITUTION: of the synths, how many had a retracted generated frame
    // reachable and close enough to present sharp instead. The outcomes are kept apart
    // because they mean different things: no generated frame reachable at all is a source
    // that never paired (or a bracket so wide the frame aged out), while
    // reachable-but-outside-the-gate is the substitution the passthrough threshold rejects.
    long long genSub = 0, genSubNoGen = 0, genSubOutOfGate = 0;
    std::vector<int64_t> genSubDiff;      // |target - generated stamp| on the accepted ones
    // The same distance on the REFUSED ones. A refusal a few hundred us past the gate is a
    // threshold argument; one clustered near half a source period is a different statement
    // entirely, namely that no generated frame exists near this target at all and the
    // nearest one belongs to another batch.
    std::vector<int64_t> outDiff;
    // Blends the policy's no-reuse rule saved: the same generated frame stays reachable
    // for many presents, and showing it twice is a duplicate no content check can catch,
    // because the pixels are perfectly good both times.
    long long genSubRepeat = 0;
    // Blends its monotone rule saved. A synth outputs at the target, which advances by
    // construction; a substituted frame carries its own
    // content time and can sit behind what was already shown. Counted separately from the
    // repeat case because a repeat is one slot shown twice while this is a content
    // REGRESSION, and the composite's monotonic rule does not cover a frame it never saw.
    long long genSubBackward = 0;
    // How far back in the ring the substituted frame sat, in wakes. This is the answer to
    // whether retaining generated frames needs a deeper ring: they already occupy a slot
    // today (retraction clears the valid bit, it does not free the position), so the depth
    // reached is the whole cost.
    std::vector<int64_t> genSubDepth;
    // Bracket span at the synth, bucketed in QUARTER source periods (bucket 4 = exactly one
    // period), last bucket open-ended. A single "one period" column hides the thing worth
    // knowing: the spec's claim is GEOMETRIC - at a span of exactly one period the
    // generated frame is inside the gate by construction - and that holds only as long as
    // the span really is one period. A 1.4-period bracket is one late frame, not a dropped
    // one, and its midpoint is nowhere near the target.
    static const int kSpanBuckets = 12;
    long long synthBySpan[kSpanBuckets] = {0}, genSubBySpan[kSpanBuckets] = {0};
    long long noGenBySpan[kSpanBuckets] = {0}, outOfGateBySpan[kSpanBuckets] = {0};
    long long tgtDiffSum = 0, tgtDiffMax = 0;
    long long pullDiffSum = 0, pullDiffMax = 0;
    // Shown-stamp step census, the same six classes pacing.py prints so replayed and live
    // runs are directly comparable.
    long long stepZero = 0, stepFlip = 0, stepPeriod = 0, stepOneFive = 0, stepBig = 0,
              stepBack = 0;
    double spanS = 0.0;
};

struct CaptureCensus {
    long long wakes = 0;
    long long opens = 0;          // batches
    long long steered = 0;        // m_phaseKeepBatches
    long long flipped = 0;        // m_phaseKeepFlipped
    long long empty = 0;          // m_phaseKeepEmpty
    long long reclaimed = 0;      // m_phaseKeepReclaimed
    long long undecided = 0;      // m_phaseKeepUndecided
    long long resets = 0;         // m_phaseKeepResets
    long long collapsed = 0;      // the log's col=
    // Where the replay's cumulative col= first parts company with the log's. This is a
    // per-wake check, far sharper than any aggregate: aggregates can agree while the
    // per-wake decisions differ in compensating ways.
    long long colDivergeAtWake = -1;
    long long colDiverged = 0;
    // Why batches went unsteered, which is the diagnosis question 2 needs.
    long long notPaired = 0;      // the pairing gate refused to arm the vote
    long long noGrid = 0;         // Grid() had no cadence yet
    long long votePending = 0;    // vote not yet valid (re-earning after a reset)
    long long positionLost = 0;   // vote valid, RotationPositionAt could not place it
    long long anchorMissing = 0;  // the vote batch could not be anchored at all
    long long advanceRefused = 0; // RotationAdvance rejected the measured step count
    // WHY an unsteered batch was unsteered, read from the vote's public state after the
    // observation. "Still collecting" and "collected but unconvincing" are different
    // problems with different fixes, and the aggregate steering number conflates them:
    // the first is cured by fewer resets, the second is not cured by anything cheap.
    long long samplesShort = 0;   // some reachable residue below kMinSamplesPerResidue
    long long shapeFail = 0;      // every residue full, yet the vote refused the shape
    long long observations = 0;   // RotationObserve calls that landed
    // Batches whose retracted member was kept reachable.
    long long substitutableBatches = 0;
    // Retractions the driver's change map refused as capture-race duplicates.
    long long genDupRefused = 0;
    // Grid readings actually seen, to name the regime rather than assume it.
    long long fpsHist[16] = {0};
    long long strideHist[16] = {0};
    // STEERING WITHIN THE ROTATING REGIME. The whole-run steering percentage answers a
    // question nobody asked: every capture spends minutes on the desktop, in menus and
    // loading, where the grid does not rotate, the vote correctly refuses, and plain
    // keep-real is the right answer. Counting those batches as "downtime" understates the
    // vote by whatever fraction of the session was not gameplay.
    long long inRotating = 0;
    long long steeredInRotating = 0;
    long long steeredByFps[16] = {0};
};

struct Config {
    int64_t batchThreshold = 3000 * kTicksPerUs;    // (m_freqQuad * 3) / 1000
    int64_t stallGap = 125000 * kTicksPerUs;        // m_freqQuad / 8
    int64_t assumedSrcPeriod = 0;                   // freq / assumedFps
    bool phaseKeep = true;
    // Gate 2 needs the SUPERSEDED constants, to prove the harness reproduces a collapse it
    // was not built against. Both are expressed here rather than by editing the policy:
    //
    // clampedEma false = the pre-2dae12d rule, which folded every gap under 125 ms into the
    // batch-period estimate, so a single grab-timeout stall flipped the derived stride 2->3.
    //
    // advanceBound is the old RotationAdvance ceiling. The bound itself is a literal inside
    // the linked policy and stays that way; what this does is hand RotationAdvance a count
    // past its OWN ceiling whenever the real count exceeds the emulated one, which drives
    // the identical code path (re-origin, return false). Emulating the effect through the
    // real function beats forking it - a copy could drift and then the collapse it
    // reproduced would be the copy's, not the relay's.
    bool clampedEma = true;
    int advanceBound = 24;
    int ringSlots = 16;            // CaptureRing::RING_SIZE; gate 3 replays this at 8
    // Present-side config, all as TemporalCaptureMode::Setup derives it.
    int64_t stickiness = 0;        // freq / 1000
    int64_t comb = 0;              // assumedSrcPeriod / M
    int64_t phasePullSlew = 0;     // freq / 40000, i.e. 25 us per present
    int64_t stallSpan = 0;         // assumedSrcPeriod * 2, or * 5/2 under phasekeep
    int64_t lag = 0;               // the static bracketing delay
    int64_t passthrough = 0;       // blend-mode passthrough gate
    bool blend = false;            // the capture ran b: mode, so DecideComposite governs
    // Offer the retracted generated frame to the policy. Off by default so a replay
    // reproduces the build that recorded the log; --sub-gen answers what the change would
    // have done to that same capture.
    bool subGen = false;
    // The vote's class-mean margin, in this harness's ticks. Defaults to what production now
    // passes (80 us of QPC). Every log recorded BEFORE the units fix ran with an effective
    // 8 us, so reproducing such a run - gates 1 and 2 - needs `--sep-us 8`, and the header
    // prints the value so a comparison can never silently be against the wrong build.
    int64_t minSeparation = 80 * kTicksPerUs;
    // The pairing gate (CaptureRing's m_batchMembersEmaQ8), in Q8 members per batch. 0
    // disables it, which is how the pre-guard build is reproduced for validation.
    int64_t minPairingQ8 = 410;
    // Present-census window, in the log's OWN dl= clock, so a bound reads the same here as
    // in a report that cites "log 2958-3043 s". Presents outside it are decided normally
    // and counted into a discard: the comb lock and the composite Schmitt states carry
    // across presents, so a window that skipped the policy would be scoring a different
    // decision sequence than the one it sits inside.
    int64_t fromTs = INT64_MIN;
    int64_t toTs = INT64_MAX;
};

// What the live path decided for one batch, recorded so the ORACLE can mark it. Steering
// uptime says nothing about steering CORRECTNESS: a vote that is up 96% of the time and
// wrong a third of the time is worse than no vote at all, and no field instrument can tell
// the difference because both members of a batch carry the same stamp.
struct BatchOutcome {
    int64_t start = 0;
    int members = 0;
    int steeredMember = -1;   // as DecideKeep was told; -1 = fell back to plain keep-real
};

// One reset, with enough context to classify it later. Question 2 is "WHICH resets are
// these", and an aggregate count cannot answer it.
struct ResetRec {
    int64_t at = 0;
    long long batch = 0;
    int fromStride = 0, fromFps = 0;
    int toStride = 0, toFps = 0;
    int64_t spacing = 0;
    int64_t batchPeriodEma = 0;
    long long batchesSinceLast = 0;
};

CaptureCensus ReplayCaptureSide(const Capture& cap, const Config& cfg,
                               std::vector<ResetRec>* resets,
                               std::vector<BatchOutcome>* outcomes,
                               PresentCensus* pcOut) {
    CaptureCensus c;

    // Present-side state. Presents and wakes are interleaved BY TIME through one loop, so a
    // present sees exactly the ring the capture thread had built by that instant.
    RingModel ring;
    ring.Init(cfg.ringSlots);
    policy::SelectionState selState;
    policy::CompositeState compState;
    policy::PhaseLockState lockState;
    policy::PolicyConfig pcfg;
    pcfg.stickinessQpc = cfg.stickiness;
    pcfg.combQpc = cfg.comb;
    pcfg.phasePullSlewQpc = cfg.phasePullSlew;
    pcfg.stallSpanQpc = cfg.stallSpan;
    pcfg.passthroughQpc = cfg.passthrough;
    size_t nextPresent = 0;
    int64_t lastShownStamp = 0;
    bool haveShown = false;
    int64_t firstPresentTs = 0, lastPresentTs = 0;
    bool havePresentSpan = false;

    // One present against the ring as it stands. Kept as a lambda so the tail flush after
    // the last wake runs the identical path rather than a copy of it.
    PresentCensus discard;
    auto DoPresent = [&](const PresentRec& p) {
        if (!pcOut) return;
        const bool inWindow = p.deadline >= cfg.fromTs && p.deadline <= cfg.toTs;
        PresentCensus* const pc = inWindow ? pcOut : &discard;
        if (!p.usable) { pc->skipped++; return; }
        pc->presents++;
        if (!p.logged) pc->noBeforeLive++;
        if (inWindow) {
            if (!havePresentSpan) { firstPresentTs = p.deadline; havePresentSpan = true; }
            lastPresentTs = p.deadline;
        }

        // TemporalCaptureMode's own sequence: target from the deadline and the CURRENT pull,
        // bracket, then advance the lock for the NEXT present, then select.
        const int64_t target = p.deadline - (cfg.lag + lockState.pullQpc);

        policy::BracketInfo b;
        int depth = -1;
        ring.FindBracket(target, &b, &depth);
        if (!b.hasBefore) pc->noBefore++;
        if (!b.hasAfter) pc->noAfter++;

        if (pcfg.combQpc > 0) {
            const bool resumed = policy::UpdateStallRun(lockState, pcfg, b);
            if (!policy::BracketIsStalled(b, pcfg))
                policy::UpdatePhaseLock(lockState, pcfg, b.beforeDiff, resumed);
        }

        // Blend mode decides with DecideComposite, not SelectFrame - and the two differ in
        // exactly the case that matters here: a one-sided bracket HOLDS (a duplicate) unless
        // the single frame it has sits inside the passthrough gate, in which case it still
        // passes through sharp. Counting "incomplete bracket" as "hold" overstates holds by
        // that population, so the real decision is run.
        if (cfg.blend) {
            const int64_t prevOut = compState.lastOutputTs;
            const int64_t prevGen = compState.lastGenTs;
            int64_t genDepth = -1;
            // The generated candidate is offered to the REAL policy rather than scored by
            // a copy of its rule here. genUsable is forced true because a log carries no
            // pixels: the content check cannot be replayed, so this counts substitutions
            // the guard would still be free to refuse, and the census below reports the
            // refusals the policy itself makes.
            if (cfg.subGen) {
                int64_t gd = 0, gstamp = 0, gdepth = 0;
                if (ring.FindGenerated(target, &gd, &gstamp, &gdepth)) {
                    b.hasGen = true;
                    b.genUsable = true;
                    b.genTs = gstamp;
                    b.genDiff = gd;
                    genDepth = gdepth;
                }
            }
            const policy::CompositeDecision cd = policy::DecideComposite(b, compState, pcfg);

            // Bracket span in quarter source periods, for both outcomes: the substitution
            // and the blend it replaced have to be counted in the same bins or the rate
            // per bin means nothing.
            const int64_t span = b.beforeDiff + b.afterDiff;
            const int64_t sp = cfg.assumedSrcPeriod;
            int w = PresentCensus::kSpanBuckets - 1;
            if (sp > 0) {
                const int64_t q = span * 4 / sp;
                if (q < w && q >= 0) w = (int)q;
            }

            switch (cd.op) {
                case policy::CompositeOp::Hold: pc->opHold++; break;
                case policy::CompositeOp::PassthroughBefore: pc->opPassBefore++; break;
                case policy::CompositeOp::PassthroughAfter: pc->opPassAfter++; break;
                case policy::CompositeOp::PassthroughGenerated:
                    pc->genSub++;
                    pc->genSubBySpan[w]++;
                    pc->genSubDiff.push_back(b.genDiff);
                    pc->genSubDepth.push_back(genDepth);
                    break;
                case policy::CompositeOp::Synthesize:
                    pc->opSynth++;
                    pc->synthBySpan[w]++;
                    // WHY the policy declined, which is the whole diagnostic value: an
                    // out-of-gate refusal argues about a threshold, a reuse or a backward
                    // refusal is a rule doing its job, and no frame at all says the source
                    // never paired here.
                    if (!b.hasGen) {
                        pc->genSubNoGen++;
                        pc->noGenBySpan[w]++;
                    } else if (b.genDiff > pcfg.passthroughQpc) {
                        pc->genSubOutOfGate++;
                        pc->outOfGateBySpan[w]++;
                        pc->outDiff.push_back(b.genDiff);
                    } else if (b.genTs <= prevOut) {
                        pc->genSubBackward++;
                    } else if (b.genTs == prevGen) {
                        pc->genSubRepeat++;
                    }
                    break;
            }
        }
        const policy::Pick pick = policy::SelectFrame(b, selState, pcfg);

        if (p.logged) {
            int64_t td = target - p.liveTarget;
            if (td < 0) td = -td;
            pc->tgtDiffSum += td;
            if (td > pc->tgtDiffMax) pc->tgtDiffMax = td;
            int64_t pd = lockState.pullQpc - p.livePull;
            if (pd < 0) pd = -pd;
            pc->pullDiffSum += pd;
            if (pd > pc->pullDiffMax) pc->pullDiffMax = pd;
            pc->pickCompared++;
            if (std::strcmp(policy::PickLabel(pick), p.pick) == 0) pc->pickAgree++;
            if (b.hasBefore) {
                int64_t d = b.beforeTs - p.beforeTs;
                if (d < 0) d = -d;
                if (d == 0) pc->beforeTsAgree++;
                pc->tsDiffSum += d;
                if (d > pc->tsDiffMax) pc->tsDiffMax = d;
                if (d > 1000 * kTicksPerUs) pc->tsDiffOver1ms++;
            }
            if (depth == p.beforeDepth) pc->depthAgree++;
        }

        // What this present SHOWED, by the same rule pacing.py applies to a live log.
        int64_t shown = 0;
        bool has = false;
        if (pick == policy::Pick::Before || pick == policy::Pick::BeforeAdv) {
            shown = b.beforeTs;
            has = b.hasBefore;
        } else if (pick == policy::Pick::After || pick == policy::Pick::AfterAdv) {
            shown = b.afterTs;
            has = b.hasAfter;
        } else if (haveShown) {
            shown = lastShownStamp;
            has = true;
        }
        if (!has) return;
        if (haveShown) {
            const int64_t d = (shown - lastShownStamp) / kTicksPerUs;   // microseconds
            if (d < -4000) pc->stepBack++;
            else if (d < 4000) pc->stepZero++;
            else if (d < 13000) pc->stepFlip++;
            else if (d < 21000) pc->stepPeriod++;
            else if (d < 42000) pc->stepOneFive++;
            else pc->stepBig++;
        }
        lastShownStamp = shown;
        haveShown = true;
    };

    // Flips in DELIVERY order. The log is already written in that order (one ETW callback
    // thread reads the clock before adding), but sorting makes the model independent of
    // log interleaving between two threads rather than trusting it.
    std::vector<FlipRec> byKnown = cap.flips;
    std::stable_sort(byKnown.begin(), byKnown.end(),
                     [](const FlipRec& a, const FlipRec& b) { return a.known < b.known; });

    policy::FlipHistory hist;
    policy::BatchState batchState;
    policy::RotationPhase rot;

    int64_t rotPeriodEma = 0;         // m_rotPeriodEma: CLAMPED, stride-derivation only
    int64_t membersEmaQ8 = 0;         // m_batchMembersEmaQ8: the pairing gate
    // Whether retracted members are kept reachable, mirroring CaptureRing's arming.
    bool substitutable = false;
    int rotRealMember = -1;
    int64_t rotSpacing = 0;
    int prevKeeper = -1;
    int prevLastMember = 0;
    int64_t prevBatchStart = 0;
    int64_t prevSpacing = 0;
    long long opens = 0;
    long long collapsed = 0;
    long long lastResetBatch = 0;

    std::vector<int64_t> batchStarts(64, 0);   // CaptureRing::kBatchHistory
    const int kBatchHistory = 64;

    size_t nf = 0;
    for (size_t i = 0; i < cap.wakes.size(); i++) {
        const int64_t arr = cap.wakes[i].arrival;

        // Admit every flip the relay could already have known by the time this wake reads
        // history - the post-flush instant, not the arrival (see WakeRec::flush). ALL heads,
        // exactly as the ETW callback does; the queries then ask for head 0. Nothing before
        // the vote block touches history, so one admission point here is faithful.
        // A wake's slot does not become visible to the present thread at its ARRIVAL: the
        // capture loop stretches, blocks on the GPU flush, and only then release-stores the
        // published counter. So every present due before arr+flush must run against the
        // ring WITHOUT this wake's slot in it, or a present is handed a frame the relay had
        // not published yet.
        const int64_t reads = arr + cap.wakes[i].flush;
        while (nextPresent < cap.presents.size() &&
               cap.presents[nextPresent].usable &&
               cap.presents[nextPresent].deadline <= reads) {
            DoPresent(cap.presents[nextPresent]);
            nextPresent++;
        }
        while (nextPresent < cap.presents.size() && !cap.presents[nextPresent].usable) {
            DoPresent(cap.presents[nextPresent]);
            nextPresent++;
        }

        while (nf < byKnown.size() && byKnown[nf].known <= reads) {
            policy::Flip f;
            f.displayTs = byKnown[nf].display;
            f.eventTs = byKnown[nf].known;
            f.head = byKnown[nf].head;
            f.token = byKnown[nf].token;
            hist.Add(f);
            nf++;
        }

        const long long count = (long long)i;
        const policy::BatchDecision batch =
            policy::UpdateBatch(batchState, arr, cfg.batchThreshold);
        c.wakes++;

        if (batch.member == 0) {
            batchStarts[opens % kBatchHistory] = batch.stampTs;
            opens++;
            c.opens++;

            rotRealMember = -1;
            rotSpacing = 0;
            // The pairing gate, exactly as CaptureRing folds it: the PREVIOUS batch's member
            // count, running, never per-batch (x3's legitimate single-member batches are the
            // ones the reclaim depends on).
            if (batch.batchGap > 0) {
                const int64_t members = (int64_t)(prevLastMember + 1) << 8;
                membersEmaQ8 = membersEmaQ8 ? (membersEmaQ8 * 7 + members) / 8 : members;
            }
            substitutable = cfg.subGen;
            if (substitutable) c.substitutableBatches++;
            const bool paired = membersEmaQ8 >= cfg.minPairingQ8;
            if (!paired && cfg.phaseKeep) c.notPaired++;
            if (cfg.phaseKeep && paired) {
                // The CLAMPED batch-period EMA. Folding only gaps within half-to-double
                // keeps stall gaps out of the cadence: unclamped, 154 such gaps produced
                // ~183 of one run's 224 resets by flipping the derived stride 2 -> 3.
                if (cfg.clampedEma) {
                    if (batch.batchGap > 0) {
                        if (rotPeriodEma == 0) {
                            if (batch.batchGap < cfg.stallGap) rotPeriodEma = batch.batchGap;
                        } else if (batch.batchGap > rotPeriodEma / 2 &&
                                   batch.batchGap < rotPeriodEma * 2) {
                            rotPeriodEma = (rotPeriodEma * 7 + batch.batchGap) / 8;
                        }
                    }
                } else if (batch.batchGap > 0 && batch.batchGap < cfg.stallGap) {
                    // The superseded rule: any gap under 125 ms is cadence.
                    rotPeriodEma = rotPeriodEma ? (rotPeriodEma * 7 + batch.batchGap) / 8
                                                : batch.batchGap;
                }
                const int64_t batchPeriod = rotPeriodEma;
                int stride = 0, flipsPerSource = 0;
                int64_t spacing = 0;
                if (!Grid(hist, cfg.assumedSrcPeriod, batchPeriod, &stride,
                          &flipsPerSource, &spacing)) {
                    c.noGrid++;
                } else {
                    if (flipsPerSource >= 0 && flipsPerSource < 16) c.fpsHist[flipsPerSource]++;
                    if (stride >= 0 && stride < 16) c.strideHist[stride]++;
                    if (stride != rot.stride || flipsPerSource != rot.flipsPerSource) {
                        if (resets) {
                            ResetRec r;
                            r.at = batch.stampTs;
                            r.batch = opens - 1;
                            r.fromStride = rot.stride;
                            r.fromFps = rot.flipsPerSource;
                            r.toStride = stride;
                            r.toFps = flipsPerSource;
                            r.spacing = spacing;
                            r.batchPeriodEma = batchPeriod;
                            r.batchesSinceLast = (opens - 1) - lastResetBatch;
                            resets->push_back(r);
                        }
                        lastResetBatch = opens - 1;
                        policy::RotationReset(rot, stride, flipsPerSource,
                                              cfg.minSeparation);
                        c.resets++;
                    }
                    rotSpacing = spacing;

                    // Vote on a batch old enough to answer for: a batch's own anchor flip
                    // is ~6 ms from delivery when it opens, so asking here places 1.8% of
                    // them. Two batches back is ~24 ms at x3.
                    static const long long kVoteLagBatches = 2;
                    const long long voteIdx = opens - 1 - kVoteLagBatches;
                    if (voteIdx >= 0) {
                        const int64_t vbs = batchStarts[voteIdx % kBatchHistory];
                        int64_t off = 0;
                        int steps = 0;
                        const int64_t prevAnchor = rot.haveAnchor ? rot.lastAnchorTs : -1;
                        if (!AnchorAndSteps(hist, vbs, prevAnchor, &off, &steps)) {
                            c.anchorMissing++;
                        } else if (policy::RotationAdvance(
                                       rot, vbs - off,
                                       steps > cfg.advanceBound ? 25 : steps)) {
                            policy::RotationObserve(rot, off);
                            c.observations++;
                        } else {
                            c.advanceRefused++;
                        }
                    }

                    // Split the not-yet-valid population, from the vote's own public
                    // counters: only the residues the stride can reach are required to
                    // fill, which is the same reachability the vote itself computes.
                    if (!rot.valid && rot.flipsPerSource > 1) {
                        int reach = rot.stride > 0 ? rot.stride : 1;
                        int fq = rot.flipsPerSource;
                        while (fq != 0) { const int t = reach % fq; reach = fq; fq = t; }
                        if (reach < 1) reach = 1;
                        bool anyShort = false;
                        for (int k = 0; k < rot.flipsPerSource &&
                                        k < policy::RotationPhase::kMaxPeriod; k += reach) {
                            if (rot.offsetCount[k] < 24) { anyShort = true; break; }
                        }
                        if (anyShort) c.samplesShort++;
                        else c.shapeFail++;
                    }

                    const int pos = policy::RotationPositionAt(rot, batch.stampTs, spacing);
                    if (pos >= 0) rotRealMember = policy::RotationRealMember(rot, pos);
                    if (rot.valid && rotRealMember < 0) {
                        rotRealMember = rot.flipsPerSource;   // no member is real
                        c.undecided++;
                        if (pos < 0) c.positionLost++;
                    }
                    if (rotRealMember >= 0) c.steered++;
                    else if (!rot.valid) c.votePending++;
                    // "Rotating" = the grid this batch measured can rotate at all, which is
                    // exactly the condition under which the vote is supposed to steer.
                    if (policy::RotationPeriodBatches(flipsPerSource, stride) > 1) {
                        c.inRotating++;
                        if (rotRealMember >= 0) c.steeredInRotating++;
                    }
                    if (rotRealMember >= 0 && flipsPerSource >= 0 && flipsPerSource < 16)
                        c.steeredByFps[flipsPerSource]++;

                    if (rot.period <= 1) rotSpacing = 0;
                }
            }

            // Reclaim the previous batch's coalesced single: a batch told to keep member 1
            // whose member 1 never arrived kept nothing, but the lone wake's pixels are the
            // frontbuffer at grab - the newest flip, which in the [gen,real] class IS the
            // real frame.
            if (prevKeeper == 1 && prevLastMember == 0 && count >= 1) {
                c.reclaimed++;
                ring.Revalidate(count, prevBatchStart + prevSpacing);
            }
            prevKeeper = rotRealMember;
            prevBatchStart = batch.stampTs;
            prevSpacing = rotSpacing;
        }
        prevLastMember = (batch.member > 0) ? batch.member : 0;
        if (outcomes) {
            if (batch.member == 0) {
                BatchOutcome o;
                o.start = batch.stampTs;
                o.members = 1;
                outcomes->push_back(o);
            } else if (!outcomes->empty()) {
                outcomes->back().members = batch.member + 1;
            }
            // rotRealMember is read once at batch open and held for every member, so
            // recording it on each wake keeps the value the LAST member saw - the same one
            // DecideKeep was given throughout the batch.
            if (!outcomes->empty()) outcomes->back().steeredMember = rotRealMember;
        }

        const policy::KeepDecision keep =
            policy::DecideKeep(batch, rotRealMember, rotSpacing, count >= 1);
        if (rotRealMember >= 0 && batch.member == 0) {
            if (rotRealMember >= 2) c.empty++;
            else if (rotRealMember == 0) c.flipped++;
        }
        if (keep.collapsed) collapsed++;

        ring.Write(count, keep.stampTs, batch.stampTs, batch.member, keep.keepThis);
        // A change map reading zero means the two members are the same frame, so the
        // retracted one is a duplicate and never becomes reachable. Mirrors
        // CaptureRing::RetractGenerated; -1 (no instrument) leaves the slot reachable and
        // the compositor's own content check owns the question.
        if (keep.retractPrev && cap.wakes[i].changedBlocks != 0) {
            ring.Retract(count, substitutable, cfg.assumedSrcPeriod);
        } else if (keep.retractPrev) {
            ring.Retract(count, false, cfg.assumedSrcPeriod);
            c.genDupRefused++;
        }

        // Per-wake fidelity against the log's own cumulative col=.
        if (cap.wakes[i].collapsed >= 0 && cap.wakes[i].collapsed != collapsed) {
            if (c.colDivergeAtWake < 0) c.colDivergeAtWake = cap.wakes[i].index;
            c.colDiverged++;
        }
    }
    // Presents left after the final wake.
    while (nextPresent < cap.presents.size()) {
        DoPresent(cap.presents[nextPresent]);
        nextPresent++;
    }
    if (pcOut && havePresentSpan) {
        pcOut->spanS = (double)(lastPresentTs - firstPresentTs) / (1e6 * (double)kTicksPerUs);
    }
    c.collapsed = collapsed;
    return c;
}

// ---------------------------------------------------------------------------------
// THE FLOOR: what x3 phasekeep can achieve with PERFECT steering.
//
// This is the measurement that can kill x3 cheaply, so it deliberately gives the relay
// every advantage. The vote is replaced by an ORACLE with full hindsight: the whole flip
// stream, the whole batch stream, and the rotation phase read from the entire capture at
// once. Any hole it still leaves is a real frame that NEVER ARRIVED, and no amount of
// relay code recovers those - the ceiling is NvFBC delivery.
//
// A hole is a SOURCE PERIOD with no real frame. Real frames scan out on flips congruent to
// one phase modulo flipsPerSource, so the real flips ARE the source periods, one each, and
// the question per real flip is simply whether the batch that carried it delivered the
// member that held it. That framing needs no present clock and no ring: it is a property of
// what the capture API handed over, which is exactly the ceiling being measured.
// ---------------------------------------------------------------------------------

struct FloorResult {
    bool valid = false;
    const char* why = "";
    int flipsPerSource = 0;
    int realPhase = -1;
    int64_t classMean[policy::RotationPhase::kMaxPeriod] = {};
    long long classCount[policy::RotationPhase::kMaxPeriod] = {};
    long long sourcePeriods = 0;    // real flips inside the x3 regime
    long long covered = 0;
    long long coveredByReclaim = 0; // covered ONLY via the unverified coalesced-single path
    long long holeMemberMissing = 0; // the batch woke but the real member never arrived
    long long holeNoBatch = 0;       // nothing woke near that real flip at all
    long long holesIsolated = 0;     // neighbours within the stall span
    long long holesInStall = 0;      // gap to the neighbouring real content over 42 ms
    long long holesIsolatedNoReclaim = 0;
    double seconds = 0.0;
    // The live path graded against the oracle, over in-regime batches only.
    long long liveSteered = 0;
    long long liveSteeredAgrees = 0;   // the vote named the member the oracle names
    long long liveUnsteered = 0;
    long long shownGenSteered = 0;     // steered, but the kept member is not the real one
    long long shownGenFallback = 0;    // unsteered: plain keep-real kept a generated frame
    long long droppedWrong = 0;        // steered to drop a batch that DID hold a real frame
};

// Batches as the oracle sees them: start, member count, and the flip they anchor to.
struct OracleBatch {
    int64_t start = 0;
    int members = 0;
    int anchor = -1;      // index into the deduplicated head-0 display-ordered flip list
    int64_t aoff = 0;
    int64_t spacing = 0;
    bool inRegime = false;
};

FloorResult MeasureFloor(const Capture& cap, const Config& cfg,
                         const std::vector<BatchOutcome>& live) {
    FloorResult r;

    // Head-0 flips in DISPLAY order, distinct display times only. Hindsight is legitimate
    // here and nowhere else in this file: the oracle is the ceiling, not a proposal.
    std::vector<int64_t> disp;
    disp.reserve(cap.flips.size());
    for (const FlipRec& f : cap.flips)
        if (f.head == 0) disp.push_back(f.display);
    std::sort(disp.begin(), disp.end());
    disp.erase(std::unique(disp.begin(), disp.end()), disp.end());
    if (disp.size() < 1000) {
        r.why = "too few head-0 flips";
        return r;
    }

    // Batch split, by the ring's own rule, with the member count each batch actually got.
    std::vector<OracleBatch> batches;
    {
        policy::BatchState bs;
        for (const WakeRec& w : cap.wakes) {
            const policy::BatchDecision d =
                policy::UpdateBatch(const_cast<policy::BatchState&>(bs), w.arrival,
                                    cfg.batchThreshold);
            if (d.member == 0) {
                OracleBatch b;
                b.start = d.stampTs;
                b.members = 1;
                batches.push_back(b);
            } else if (!batches.empty()) {
                batches.back().members = d.member + 1;
            }
        }
    }
    if (batches.size() < 500) {
        r.why = "too few batches";
        return r;
    }

    // Anchor each batch, and decide per batch whether the x3 regime held there. A local
    // spacing, never a whole-capture one: every capture opens on the desktop, where this
    // panel presents at 240 Hz, and one global median would label those stretches x3.
    policy::FlipHistory hist;
    size_t nf = 0;
    std::vector<FlipRec> byKnown = cap.flips;
    std::stable_sort(byKnown.begin(), byKnown.end(),
                     [](const FlipRec& a, const FlipRec& b) { return a.known < b.known; });
    long long fpsHist[16] = {0};
    for (OracleBatch& b : batches) {
        while (nf < byKnown.size() && byKnown[nf].known <= b.start) {
            policy::Flip f;
            f.displayTs = byKnown[nf].display;
            f.eventTs = byKnown[nf].known;
            f.head = byKnown[nf].head;
            hist.Add(f);
            nf++;
        }
        const int64_t sp = MedianFlipSpacing(hist);
        if (sp <= 0) continue;
        b.spacing = sp;
        const int fps = (int)((cfg.assumedSrcPeriod + sp / 2) / sp);
        if (fps >= 0 && fps < 16) fpsHist[fps]++;
        // Nearest flip either side, same quarter-step confidence bound the relay uses.
        const size_t at = (size_t)(std::lower_bound(disp.begin(), disp.end(), b.start) -
                                   disp.begin());
        int best = -1;
        int64_t bestAbs = -1;
        for (size_t k = (at > 0 ? at - 1 : 0); k <= at && k < disp.size(); k++) {
            int64_t d = b.start - disp[k];
            if (d < 0) d = -d;
            if (bestAbs < 0 || d < bestAbs) { bestAbs = d; best = (int)k; }
        }
        if (best < 0 || bestAbs > sp / 4) continue;
        b.anchor = best;
        b.aoff = b.start - disp[best];
        b.inRegime = (fps == 3);
    }
    int dom = 0;
    for (int i = 1; i < 16; i++) if (fpsHist[i] > fpsHist[dom]) dom = i;
    if (dom != 3) {
        r.why = "not an x3 capture";
        return r;
    }
    r.flipsPerSource = 3;

    // The rotation phase, from the WHOLE capture at once: the class whose batches wake
    // BEFORE their flip is the real-led one. Requires the mechanism's signature (exactly
    // one class negative), because without it there is no phase to be right about and an
    // "oracle" built on a coin flip would report a floor that means nothing.
    int64_t sum[3] = {0, 0, 0};
    long long cnt[3] = {0, 0, 0};
    for (const OracleBatch& b : batches) {
        if (b.anchor < 0 || !b.inRegime) continue;
        const int g = b.anchor % 3;
        sum[g] += b.aoff;
        cnt[g]++;
    }
    int negatives = 0, neg = -1;
    for (int i = 0; i < 3; i++) {
        if (!cnt[i]) { r.why = "a rotation class was never observed"; return r; }
        r.classMean[i] = sum[i] / cnt[i];
        r.classCount[i] = cnt[i];
        if (r.classMean[i] < 0) { negatives++; neg = i; }
    }
    if (negatives != 1) {
        r.why = "no shaped rotation in this capture: the phase is unreadable, so there is "
                "no oracle to measure a floor against";
        return r;
    }
    r.realPhase = neg;

    // Which real flips got a real frame. A batch anchored at A carries the real frame at
    // A + m, m = (realPhase - A) mod 3; m == 2 is the all-generated batch, which holds no
    // real frame and is not a hole - it is the tiling.
    std::vector<char> covered(disp.size(), 0);
    std::vector<char> reclaimOnly(disp.size(), 0);
    std::vector<char> hadBatch(disp.size(), 0);
    for (const OracleBatch& b : batches) {
        if (b.anchor < 0 || !b.inRegime) continue;
        int m = (r.realPhase - (b.anchor % 3)) % 3;
        if (m < 0) m += 3;
        if (m >= 2) continue;                       // [gen,gen]: no real frame exists
        const size_t at = (size_t)b.anchor + (size_t)m;
        if (at >= disp.size()) continue;
        hadBatch[at] = 1;
        if (b.members > m) {
            covered[at] = 1;              // the real member actually arrived
        } else {
            // The batch woke but the member holding the real frame never arrived. Only
            // reachable for m == 1 with a single-member batch, which is the coalesced
            // single: the lone wake's pixels are the frontbuffer at grab, so the relay
            // reclaims them as the real frame. PROVEN at x2, ASSUMED at x3.
            //
            // This is a SEPARATE state, not a covered one. Folding the reclaim credit
            // straight into `covered` here made holeMemberMissing structurally unreachable
            // and it printed 0 on every capture - an identity dressed as a measurement.
            reclaimOnly[at] = 1;
        }
    }

    // WHICH FLIPS BELONG TO THE MEASUREMENT, decided from the flip grid and NOT from the
    // batches. Enumerating source periods from the batches that claimed them is circular:
    // a period where nothing woke would be excluded by construction, so the one hole class
    // that matters most - the real frame that never arrived at all - could never be
    // counted. (It reported a flawless 100% before this was fixed.)
    //
    // A flip is in the measurement if an in-regime batch anchored on or near it, and gaps
    // between consecutive in-regime batches are BRIDGED up to one second so that a source
    // stall stays inside the measurement and its missing frames are counted (as the stall
    // class). A longer gap is a regime change - this panel presents the desktop at 240 Hz -
    // and is left out rather than charged to x3.
    const int kRegimeBridge = 180;   // flips, ~1 s at x3's 180 Hz grid
    std::vector<char> inRegimeFlip(disp.size(), 0);
    int prevAnchor = -1;
    for (const OracleBatch& b : batches) {
        if (b.anchor < 0 || !b.inRegime) continue;
        for (int i = b.anchor; i <= b.anchor + 2 && i < (int)disp.size(); i++)
            inRegimeFlip[i] = 1;
        if (prevAnchor >= 0 && b.anchor > prevAnchor &&
            (b.anchor - prevAnchor) <= kRegimeBridge) {
            for (int i = prevAnchor; i <= b.anchor; i++) inRegimeFlip[i] = 1;
        }
        prevAnchor = b.anchor;
    }

    std::vector<size_t> realFlips;
    for (size_t i = 0; i < disp.size(); i++) {
        if ((int)(i % 3) != r.realPhase) continue;
        if (!inRegimeFlip[i]) continue;
        realFlips.push_back(i);
    }
    if (realFlips.size() < 500) {
        r.why = "too few in-regime source periods";
        return r;
    }

    // TWO floors, each classified against ITS OWN covered set. The reclaim credit is the
    // whole difference between them, so it cannot be applied by adding a count afterwards:
    // doing that charged reclaims sitting INSIDE stalls to the isolated class and never
    // re-ran the split, overstating the no-reclaim isolated rate.
    const int64_t stallSpan = 42000 * kTicksPerUs;
    std::vector<char> okWith(disp.size(), 0), okWithout(disp.size(), 0);
    for (size_t i = 0; i < disp.size(); i++) {
        okWith[i] = (covered[i] || reclaimOnly[i]) ? 1 : 0;
        okWithout[i] = covered[i];
    }
    // Nearest covered neighbour either side, by time: a frozen source leaves a wide gap and
    // is not an x3 regression (the baseline stalls too), so it is separated rather than
    // counted against the floor.
    auto Stalled = [&](const std::vector<char>& ok, size_t i) {
        int64_t prevTs = -1, nextTs = -1;
        for (size_t j = i; j-- > 0;)
            if (ok[realFlips[j]]) { prevTs = disp[realFlips[j]]; break; }
        for (size_t j = i + 1; j < realFlips.size(); j++)
            if (ok[realFlips[j]]) { nextTs = disp[realFlips[j]]; break; }
        if (prevTs < 0 || nextTs < 0) return true;
        return (nextTs - prevTs) > stallSpan;
    };
    for (size_t i = 0; i < realFlips.size(); i++) {
        const size_t at = realFlips[i];
        r.sourcePeriods++;
        if (reclaimOnly[at]) r.coveredByReclaim++;
        if (okWith[at]) r.covered++;
        else if (hadBatch[at]) r.holeMemberMissing++;   // reachable now: see the note above
        else r.holeNoBatch++;

        if (!okWith[at]) {
            if (Stalled(okWith, i)) r.holesInStall++;
            else r.holesIsolated++;
        }
        if (!okWithout[at] && !Stalled(okWithout, i)) r.holesIsolatedNoReclaim++;
    }

    // GRADE THE LIVE PATH. Both enumerations split batches with the same UpdateBatch over
    // the same wakes, so index i is the same batch in both; refuse to grade rather than
    // guess if that ever stops being true.
    if (live.size() != batches.size()) {
        // The comment used to promise "refuse to grade rather than guess" and then returned
        // silently, which reads as "nothing to report" instead of "not measured".
        std::fprintf(stderr, "NOT GRADED: %zu live batches vs %zu oracle batches - the two"
                             " enumerations disagree, so index alignment is unsafe\n",
                     live.size(), batches.size());
    } else {
        for (size_t i = 0; i < batches.size(); i++) {
            const OracleBatch& b = batches[i];
            if (b.anchor < 0 || !b.inRegime) continue;
            int truth = (r.realPhase - (b.anchor % 3)) % 3;
            if (truth < 0) truth += 3;
            const int told = live[i].steeredMember;
            if (told >= 0) {
                r.liveSteered++;
                if (told == truth) {
                    r.liveSteeredAgrees++;
                } else if (told >= 2 && truth < 2 && b.members > truth) {
                    // Told "no member is real" when one was, and it had arrived.
                    r.droppedWrong++;
                } else if (told < 2 && told != truth) {
                    r.shownGenSteered++;
                }
            } else {
                r.liveUnsteered++;
                // Plain keep-real retains the LAST member the batch delivered.
                const int kept = live[i].members - 1;
                if (kept != truth) r.shownGenFallback++;
            }
        }
    }
    r.seconds = (double)r.sourcePeriods * (double)cfg.assumedSrcPeriod /
                (1e6 * (double)kTicksPerUs);
    r.valid = true;
    return r;
}

void ReportPresent(const PresentCensus& p, const Config& cfg) {
    if (!p.presents) return;
    std::printf("\nPRESENT SIDE - the same ring, read from the other end\n");
    std::printf("  %lld presents over %.1f s (%.1f/s)%s\n", p.presents, p.spanS,
                p.spanS > 0 ? p.presents / p.spanS : 0.0,
                (cfg.fromTs != INT64_MIN || cfg.toTs != INT64_MAX) ? "  [WINDOWED]" : "");
    if (p.skipped)
        std::printf("  %lld starved presents skipped: no logged neighbour to interpolate"
                    " a deadline from\n", p.skipped);
    std::printf("  no before-frame: replay %lld (%.1f%%), log recorded %lld (%.1f%%)\n",
                p.noBefore, 100.0 * (double)p.noBefore / (double)p.presents,
                p.noBeforeLive, 100.0 * (double)p.noBeforeLive / (double)p.presents);
    std::printf("  no after-frame:  replay %lld (%.1f%%)\n", p.noAfter,
                100.0 * (double)p.noAfter / (double)p.presents);
    if (p.opHold + p.opPassBefore + p.opPassAfter + p.opSynth) {
        const double s2 = p.spanS > 0 ? p.spanS : 1.0;
        std::printf("  BLEND MODE (DecideComposite, the real decision this capture used):\n");
        std::printf("    hold %lld (%.2f/s)  pass-before %lld  pass-after %lld  synth %lld (%.2f/s)\n",
                    p.opHold, p.opHold / s2, p.opPassBefore, p.opPassAfter,
                    p.opSynth, p.opSynth / s2);
        std::printf("  GENERATED-FRAME SUBSTITUTION (present the retracted frame instead"
                    " of blending), gate %lld us:\n", cfg.passthrough / kTicksPerUs);
        const long long blendClass = p.opSynth + p.genSub;
        std::printf("    substituted %lld of %lld presents that would have blended"
                    " (%.1f%%, %.3f/s)\n", p.genSub, blendClass,
                    blendClass ? 100.0 * (double)p.genSub / (double)blendClass : 0.0,
                    p.genSub / s2);
        // The denominator is every present that WOULD have blended, which is the synths
        // left plus the ones the substitution took: once a substitution happens that
        // present is no longer a synth, so dividing by the survivors alone reads over 100%.
        std::printf("    by bracket span  %9s %8s %8s %8s %8s\n",
                    "blend-class", "sub", "rate", "no-gen", "out-gate");
        for (int w = 0; w < PresentCensus::kSpanBuckets; w++) {
            const long long pop = p.synthBySpan[w] + p.genSubBySpan[w];
            if (!pop) continue;
            char label[24];
            if (w == PresentCensus::kSpanBuckets - 1)
                std::snprintf(label, sizeof(label), "%.2f+ periods", w / 4.0);
            else
                std::snprintf(label, sizeof(label), "%.2f-%.2f periods", w / 4.0,
                              (w + 1) / 4.0);
            std::printf("      %-17s %8lld %8lld %7.1f%% %8lld %8lld\n", label,
                        pop, p.genSubBySpan[w],
                        100.0 * (double)p.genSubBySpan[w] / (double)pop,
                        p.noGenBySpan[w], p.outOfGateBySpan[w]);
        }
        const double u = (double)kTicksPerUs;
        auto pct = [&](std::vector<int64_t> d, const char* what) {
            if (d.empty()) return;
            std::sort(d.begin(), d.end());
            std::printf("    |target - generated stamp| %s: p50 %.0f us  p90 %.0f us"
                        "  max %.0f us\n", what,
                        (double)d[d.size() / 2] / u,
                        (double)d[(size_t)((double)d.size() * 0.9)] / u,
                        (double)d.back() / u);
        };
        pct(p.genSubDiff, "substituted");
        pct(p.outDiff, "refused   ");
        std::printf("    refused by the policy: %lld no generated frame reachable,"
                    " %lld outside the gate, %lld would not advance the output,"
                    " %lld already shown\n",
                    p.genSubNoGen, p.genSubOutOfGate, p.genSubBackward, p.genSubRepeat);
        if (!p.genSubDepth.empty()) {
            std::vector<int64_t> d = p.genSubDepth;
            std::sort(d.begin(), d.end());
            std::printf("    ring depth reached for the substituted frame: p50 %lld"
                        "  p90 %lld  max %lld wakes (of %d slots)\n",
                        d[d.size() / 2], d[(size_t)((double)d.size() * 0.9)], d.back(),
                        cfg.ringSlots);
        }
        std::printf("    NOT modelled: the NvFBC race puts real pixels in the generated slot"
                    " about 12%% of the time, which the guard rejects on gdiff/motion\n");
    }
    if (p.pickCompared) {
        std::printf("  against the log, on the %lld presents it logged: pick %.2f%%,"
                    " before-stamp exact %.2f%%, depth %.2f%%\n",
                    p.pickCompared, 100.0 * (double)p.pickAgree / (double)p.pickCompared,
                    100.0 * (double)p.beforeTsAgree / (double)p.pickCompared,
                    100.0 * (double)p.depthAgree / (double)p.pickCompared);
        std::printf("    before-stamp error: mean %.2f us, max %.1f us, over 1 ms on %lld"
                    " presents\n",
                    (double)p.tsDiffSum / (double)p.pickCompared / (double)kTicksPerUs,
                    (double)p.tsDiffMax / (double)kTicksPerUs, p.tsDiffOver1ms);
        std::printf("    comb lock PREDICTED (not taken from the log): target error mean"
                    " %.2f us / max %.1f us, pull error mean %.2f us / max %.1f us\n",
                    (double)p.tgtDiffSum / (double)p.pickCompared / (double)kTicksPerUs,
                    (double)p.tgtDiffMax / (double)kTicksPerUs,
                    (double)p.pullDiffSum / (double)p.pickCompared / (double)kTicksPerUs,
                    (double)p.pullDiffMax / (double)kTicksPerUs);
    }
    if (p.spanS > 0) {
        std::printf("  pacing (shown-stamp steps/s, pacing.py's classes): zero %.1f"
                    "  flip-size %.1f  period %.1f  1.5-2.5x %.2f  BIG %.2f"
                    "  backward %.2f\n",
                    p.stepZero / p.spanS, p.stepFlip / p.spanS, p.stepPeriod / p.spanS,
                    p.stepOneFive / p.spanS, p.stepBig / p.spanS, p.stepBack / p.spanS);
    }
}

void ReportFloor(const FloorResult& r) {
    std::printf("\nDELIVERY FLOOR - hindsight phase, NOT an independent oracle\n");
    if (!r.valid) {
        std::printf("  not measurable: %s\n", r.why);
        return;
    }
    std::printf("  CAVEAT, measured: shifting the phase to a DELIBERATELY WRONG residue\n"
                "  moves the isolated rate by about as much as the spread across captures,\n"
                "  and on some captures a wrong phase scores BETTER (the coalesced singles\n"
                "  concentrate in the class the correct phase asks member 1 of). So this\n"
                "  measures capture-wake gaps and the reclaim's contribution; it does NOT\n"
                "  validate the rotation phase and cannot rank steering quality.\n");
    std::printf("  rotation phase %d, class mean arrival offsets (ticks):", r.realPhase);
    for (int i = 0; i < r.flipsPerSource; i++)
        std::printf(" [%d]=%+lld/%lld", i, (long long)r.classMean[i], r.classCount[i]);
    std::printf("\n");
    std::printf("  %lld source periods in x3 regime (%.1f s of gameplay)\n",
                r.sourcePeriods, r.seconds);
    // A three-way split, because "covered" and "member missing" are not independent: with
    // the reclaim credit granted, a missing real member IS counted covered, so reporting it
    // as a separate hole class could only ever print zero.
    std::printf("  of %lld source periods: %lld the real member arrived, %lld the member"
                " never arrived and the UNVERIFIED reclaim credited it, %lld nothing woke"
                " there\n",
                r.sourcePeriods, r.covered - r.coveredByReclaim, r.coveredByReclaim,
                r.holeNoBatch);
    const double iso = r.seconds > 0 ? (double)r.holesIsolated / r.seconds : 0.0;
    const double stall = r.seconds > 0 ? (double)r.holesInStall / r.seconds : 0.0;
    const double isoNR = r.seconds > 0 ? (double)r.holesIsolatedNoReclaim / r.seconds : 0.0;
    std::printf("  ISOLATED holes %.3f/s   (stall-class %.3f/s, which the baseline has too"
                " and does not count)\n",
                iso, stall);
    std::printf("  if the x3 reclaim turns out to carry generated pixels: %.3f/s isolated\n",
                isoNR);
    std::printf("  spec bar: above ~0.5/s isolated -> table x3. %s\n",
                iso > 0.5 ? "*** ABOVE THE BAR ***" : "within the bar");

    if (!r.liveSteered && !r.liveUnsteered) return;
    // NOT an accuracy figure. The phase here is the SAME statistic the vote uses - one
    // class-mean anchor offset negative, the rest positive - recomputed over the whole
    // capture with hindsight. So this is agreement between two computations of one
    // estimator, and it cannot separate "the vote is right" from "both share a bias".
    // There is no independent real-vs-generated label at x3 (etw-frame-timing-spec.md,
    // "Settled": nothing from ETW at x3, content analysis is the remaining route).
    std::printf("\n  VOTE vs A HINDSIGHT RECOMPUTATION OF THE SAME STATISTIC"
                " (in-regime batches)\n");
    std::printf("    NOT an independent label - agreement, not accuracy\n");
    std::printf("    vote steered %lld, agreed with the hindsight phase %lld times"
                " (%.2f%%)\n",
                r.liveSteered, r.liveSteeredAgrees,
                r.liveSteered ? 100.0 * (double)r.liveSteeredAgrees / (double)r.liveSteered
                              : 0.0);
    std::printf("    steered but wrong: %lld showed a generated frame, %lld dropped a batch"
                " that held a real one\n",
                r.shownGenSteered, r.droppedWrong);
    std::printf("    fell back to keep-real on %lld batches, of which %lld then showed a"
                " generated frame\n",
                r.liveUnsteered, r.shownGenFallback);
    // The user's acceptance bar is expressed in visible events per second, so end there.
    const double genPerSec =
        r.seconds > 0
            ? (double)(r.shownGenSteered + r.shownGenFallback) / r.seconds
            : 0.0;
    const double reclaimPerSec =
        r.seconds > 0 ? (double)r.coveredByReclaim / r.seconds : 0.0;
    std::printf("    GENERATED FRAMES SHOWN: %.2f/s  (plus %.2f/s of reclaimed singles,"
                " whose pixels are unverified at x3)\n",
                genPerSec, reclaimPerSec);
    std::printf("    user's bar: one event every couple of seconds is fine (~0.5/s),"
                " multiple per second is not\n");
}

// ---------------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------------

bool WithinPct(long long replay, long long live, double pct) {
    if (live == 0) return replay == 0;
    const double d = 100.0 * (double)(replay - live) / (double)live;
    return d <= pct && d >= -pct;
}

void GateLine(const char* name, long long replay, long long live, bool pass,
              const char* tolerance) {
    std::printf("  %-24s live %8lld   replay %8lld   %-14s %s\n", name, live, replay,
                tolerance, pass ? "PASS" : "*** FAIL ***");
}

int ReportGate1(const Capture& cap, const CaptureCensus& c) {
    const LiveSummary& L = cap.live;
    if (!L.present()) {
        std::printf("\nno 'phasekeep summary' line in this log: it cannot gate the harness\n");
        return 0;
    }
    std::printf("\nVALIDATION GATE 1 - reproduce the live run's own numbers\n");
    int fails = 0;
    struct Row { const char* name; long long replay; long long live; bool pass; const char* tol; };
    const bool pSteer = WithinPct(c.steered, L.steered, 5.0);
    const bool pReset = (c.resets - L.resets) <= 10 && (L.resets - c.resets) <= 10;
    const bool pRecl = WithinPct(c.reclaimed, L.reclaimed, 10.0);
    const bool pUnd = (c.undecided - L.undecided) <= 5 && (L.undecided - c.undecided) <= 5;
    const Row rows[] = {
        {"batches steered", c.steered, L.steered, pSteer, "+-5%"},
        {"vote resets", c.resets, L.resets, pReset, "+-10"},
        {"singles reclaimed", c.reclaimed, L.reclaimed, pRecl, "+-10%"},
        {"undecided dropped", c.undecided, L.undecided, pUnd, "+-5"},
    };
    for (const Row& r : rows) {
        GateLine(r.name, r.replay, r.live, r.pass, r.tol);
        if (!r.pass) fails++;
    }
    // Not gate-1 criteria, but they come free and a mismatch here localises a failure
    // above to the tiling rather than the vote.
    std::printf("  (not gated) kept member 0 : live %8lld   replay %8lld\n", L.flipped, c.flipped);
    std::printf("  (not gated) dropped whole: live %8lld   replay %8lld\n", L.empty, c.empty);
    return fails;
}

void ReportCensus(const Capture& cap, const CaptureCensus& c) {
    const double spanS =
        cap.wakes.empty() ? 0.0
                          : (double)(cap.wakes.back().arrival - cap.wakes.front().arrival) /
                                (1e6 * (double)kTicksPerUs);
    std::printf("replay: %lld wakes, %lld batches over %.1f s (%.1f batches/s)\n",
                c.wakes, c.opens, spanS, spanS > 0 ? c.opens / spanS : 0.0);
    int domFps = 0, domStride = 0;
    for (int i = 1; i < 16; i++) {
        if (c.fpsHist[i] > c.fpsHist[domFps]) domFps = i;
        if (c.strideHist[i] > c.strideHist[domStride]) domStride = i;
    }
    std::printf("grid: dominant x%d (stride %d) - batches by flips/source, and steering"
                " within each\n", domFps, domStride);
    for (int i = 0; i < 16; i++) {
        if (!c.fpsHist[i]) continue;
        std::printf("      x%-2d %8lld batches   steered %8lld (%5.1f%%)%s\n", i,
                    c.fpsHist[i], c.steeredByFps[i],
                    100.0 * (double)c.steeredByFps[i] / (double)c.fpsHist[i],
                    i == domFps ? "   <- the gameplay regime" : "");
    }

    std::printf("steering: %lld of %lld batches (%.1f%%) whole-run;"
                " %lld of %lld (%.1f%%) IN A ROTATING GRID\n",
                c.steered, c.opens, c.opens ? 100.0 * c.steered / c.opens : 0.0,
                c.steeredInRotating, c.inRotating,
                c.inRotating ? 100.0 * c.steeredInRotating / c.inRotating : 0.0);
    std::printf("  unsteered causes: pairing gate %lld, no grid %lld, vote invalid %lld\n",
                c.notPaired, c.noGrid, c.votePending);
    std::printf("  vote health: %lld observations landed; %lld batches still collecting,"
                " %lld with every residue full but the shape refused\n",
                c.observations, c.samplesShort, c.shapeFail);
    std::printf("  vote losses: anchor missing %lld, advance refused (re-origin) %lld\n",
                c.anchorMissing, c.advanceRefused);
    std::printf("  undecided (position lost with a valid vote): %lld\n", c.undecided);
    std::printf("  resets %lld, reclaims %lld, kept-member-0 %lld, dropped-whole %lld\n",
                c.resets, c.reclaimed, c.flipped, c.empty);

    if (c.colDivergeAtWake >= 0) {
        std::printf("PER-WAKE col= divergence: first at capture #%lld, %lld wakes differ"
                    " (replay total %lld)\n",
                    c.colDivergeAtWake, c.colDiverged, c.collapsed);
    } else {
        std::printf("per-wake col= matches the log on every wake (%lld collapsed)\n",
                    c.collapsed);
    }
    if (c.substitutableBatches) {
        std::printf("subgen: %lld of %lld batches offered a retracted frame to the ring;"
                    " %lld refused as duplicates by the driver's change map\n",
                    c.substitutableBatches, c.opens, c.genDupRefused);
    }
}

void ReportResets(const std::vector<ResetRec>& resets) {
    if (resets.empty()) return;
    std::printf("\nresets, by the grid transition that caused them\n");
    // Group identical transitions: question 2 asks which resets these ARE, and a
    // transition census answers it in one screen where a per-reset dump does not.
    struct Group { int fs, ff, ts, tf; long long n; long long sinceSum; long long sinceMin; };
    std::vector<Group> g;
    for (const ResetRec& r : resets) {
        bool found = false;
        for (Group& q : g) {
            if (q.fs == r.fromStride && q.ff == r.fromFps && q.ts == r.toStride &&
                q.tf == r.toFps) {
                q.n++;
                q.sinceSum += r.batchesSinceLast;
                if (r.batchesSinceLast < q.sinceMin) q.sinceMin = r.batchesSinceLast;
                found = true;
                break;
            }
        }
        if (!found)
            g.push_back({r.fromStride, r.fromFps, r.toStride, r.toFps, 1,
                         r.batchesSinceLast, r.batchesSinceLast});
    }
    std::sort(g.begin(), g.end(), [](const Group& a, const Group& b) { return a.n > b.n; });
    std::printf("  %-22s %8s %14s %10s\n", "(stride,flips/src)", "count", "mean gap", "min gap");
    for (const Group& q : g) {
        char from[32], to[32];
        std::snprintf(from, sizeof(from), "(%d,%d)", q.fs, q.ff);
        std::snprintf(to, sizeof(to), "(%d,%d)", q.ts, q.tf);
        char trans[48];
        std::snprintf(trans, sizeof(trans), "%s -> %s", from, to);
        std::printf("  %-22s %8lld %14.1f %10lld\n", trans, q.n,
                    (double)q.sinceSum / (double)q.n, q.sinceMin);
    }
    std::printf("  (gap = batches since the previous reset; a small mean is the re-earn"
                " window never closing)\n");
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = NULL;
    bool arm = false, oldConstants = false, quiet = false, forceBlend = false;
    bool subGen = false;
    long long sepUs = 80;
    int ringSlots = 16;
    long long pairingQ8 = 410;
    long long lagAddMs = 0;
    double fromS = -1.0, toS = -1.0;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--arm") == 0) arm = true;
        else if (std::strcmp(argv[i], "--old") == 0) oldConstants = true;
        else if (std::strcmp(argv[i], "--quiet") == 0) quiet = true;
        else if (std::strcmp(argv[i], "--sep-us") == 0 && i + 1 < argc) sepUs = std::atoll(argv[++i]);
        else if (std::strcmp(argv[i], "--ring") == 0 && i + 1 < argc) ringSlots = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--no-pairing-gate") == 0) pairingQ8 = 0;
        else if (std::strcmp(argv[i], "--force-blend") == 0) forceBlend = true;
        else if (std::strcmp(argv[i], "--sub-gen") == 0) subGen = true;
        else if (std::strcmp(argv[i], "--from") == 0 && i + 1 < argc) fromS = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--to") == 0 && i + 1 < argc) toS = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--lag-add") == 0 && i + 1 < argc) lagAddMs = std::atoll(argv[++i]);
        else if (argv[i][0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        } else path = argv[i];
    }
    if (!path) {
        std::fprintf(stderr,
                     "usage: %s [--arm] [--old] [--quiet] <NvFBCR.log>\n"
                     "  Replays the capture-side chain through the real policy and scores\n"
                     "  itself against the log's own phasekeep summary (spec gate 1).\n"
                     "  --arm  run the vote even where the field run had -phasekeep off.\n"
                     "         This is how the x2/FG-off NULL CONTROLS are exercised: the\n"
                     "         vote must steer 0%% of batches in a regime that cannot\n"
                     "         rotate, and a log recorded without the flag proves nothing\n"
                     "         about that unless the replay arms it.\n"
                     "  --old  superseded constants (unclamped EMA, advance bound 8), for\n"
                     "         reproducing the reset-storm collapse (spec gate 2).\n"
                     "  --quiet  one summary line, for sweeping a directory of logs.\n"
                     "  --force-blend  run DecideComposite on a capture recorded in t:\n"
                     "         mode. This is what exercises the substitution NULL CONTROL: an\n"
                     "         FG-off log has no generated frames, and a t: capture never\n"
                     "         calls DecideComposite, so without this the zero it prints\n"
                     "         is the mode's zero rather than the absence of the frames.\n"
                     "  --from <s> --to <s>  restrict the PRESENT census to a window of the\n"
                     "         log's own dl= clock, to trim desktop off the ends. Every\n"
                     "         present is still decided; only the counting is windowed.\n",
                     argv[0]);
        return 2;
    }

    // Validate the knobs rather than letting a typo produce a clean-looking wrong answer.
    // --sep-us 0 (or a non-numeric value, which atoll reads as 0) would drive the policy's
    // own "separation unset" refusal and print a confident `steered 0 (0.0%)`, defeating the
    // very fail-closed behaviour that refusal exists to make loud.
    if (ringSlots < 2 || ringSlots > 4096) {
        std::fprintf(stderr, "--ring must be between 2 and 4096 (got %d)\n", ringSlots);
        return 2;
    }
    if (sepUs <= 0) {
        std::fprintf(stderr, "--sep-us must be a positive integer (got '%lld'); 0 would make"
                             " the vote refuse every batch and report 0%% steering as though"
                             " it were a finding\n", sepUs);
        return 2;
    }
    if (pairingQ8 < 0) {
        std::fprintf(stderr, "--no-pairing-gate takes no argument\n");
        return 2;
    }

    Capture cap;
    if (!ParseLog(path, &cap)) return 2;

    if (cap.wakes.empty() || cap.flips.empty()) {
        if (quiet) std::printf("%-40s no ETW flip stream: not replayable\n", path);
        else std::fprintf(stderr, "nothing to replay (need both capture and flip lines)\n");
        return 2;
    }

    Config cfg;
    // production: m_assumedSrcPeriodQpc = freq / assumedFps, freq being 10 MHz
    const double freq = 1000000.0 * (double)kTicksPerUs;
    cfg.assumedSrcPeriod = (int64_t)(freq / (cap.srcFps > 0.0 ? cap.srcFps : 60.0));
    cfg.phaseKeep = cap.phaseKeep || arm;
    cfg.minSeparation = (int64_t)sepUs * kTicksPerUs;
    cfg.ringSlots = ringSlots;
    cfg.minPairingQ8 = pairingQ8;
    cfg.stickiness = cap.stickinessUs * kTicksPerUs;
    cfg.comb = cap.combUs * kTicksPerUs;
    // Extra bracketing lag to evaluate offline. A hold is a bracket with no AFTER frame;
    // moving the target earlier makes it likelier that a newer frame has already arrived, so
    // lag trades latency for holds. The capture stream is unchanged by this, which is what
    // makes the question answerable from a recorded log instead of a new capture.
    cfg.lag = (cap.lagUs + lagAddMs * 1000) * kTicksPerUs;
    cfg.blend = cap.blend || forceBlend;
    cfg.subGen = subGen;
    cfg.passthrough = cap.passthroughUs * kTicksPerUs;
    // A capture recorded in t: mode carries no passthrough threshold, so a forced blend
    // replay needs production's: a quarter of the source period, which is the 4166 us the
    // 60 fps runs print.
    if (cfg.blend && cfg.passthrough <= 0) cfg.passthrough = cfg.assumedSrcPeriod / 4;
    if (fromS >= 0.0) cfg.fromTs = (int64_t)(fromS * 1e6) * kTicksPerUs;
    if (toS >= 0.0) cfg.toTs = (int64_t)(toS * 1e6) * kTicksPerUs;
    cfg.phasePullSlew = (int64_t)(freq / 40000.0);        // 25 us per present
    // stallSpan is widened under phasekeep (TemporalCaptureMode::Setup), because a dropped
    // all-generated batch legitimately widens the bracket.
    cfg.stallSpan = cap.phaseKeep ? cfg.assumedSrcPeriod * 5 / 2 : cfg.assumedSrcPeriod * 2;
    if (oldConstants) {
        cfg.clampedEma = false;
        cfg.advanceBound = 8;
    }

    long long unusable = 0;
    InterpolatePresents(&cap.presents, &unusable);

    std::vector<ResetRec> resets;
    std::vector<BatchOutcome> outcomes;
    PresentCensus pres;
    const CaptureCensus c = ReplayCaptureSide(cap, cfg, &resets, &outcomes, &pres);

    if (quiet) {
        int domFps = 0;
        for (int i = 1; i < 16; i++) if (c.fpsHist[i] > c.fpsHist[domFps]) domFps = i;
        std::printf("%-40s x%d %7lld batches  steered %7lld (%5.1f%%)  resets %5lld"
                    "  reclaim %5lld%s\n",
                    path, domFps, c.opens, c.steered,
                    c.opens ? 100.0 * c.steered / c.opens : 0.0, c.resets, c.reclaimed,
                    c.colDivergeAtWake >= 0 ? "  COL-DIVERGED" : "");
        return 0;
    }

    std::printf("%s%s\n", path, oldConstants ? "   [SUPERSEDED CONSTANTS]" : "");
    // The ring size is printed because the log does not record it: replaying a capture at a
    // ring the build did not use is silent and looks like a finding. A 32-slot capture
    // replayed at the default 16 reports thousands of holds the run never had.
    std::printf("ring: %d slots (not recorded in the log - must match the build that"
                " captured it)\n", ringSlots);
    std::printf("parsed: %zu wakes, %zu flips (head 0: %zu), src %.1f fps, lag %lld us,"
                " phasekeep %s%s\n",
                cap.wakes.size(), cap.flips.size(),
                (size_t)std::count_if(cap.flips.begin(), cap.flips.end(),
                                      [](const FlipRec& f) { return f.head == 0; }),
                cap.srcFps, (long long)cap.lagUs, cap.phaseKeep ? "ON" : "off",
                (!cap.phaseKeep && arm) ? " (armed by --arm)" : "");
    if (lagAddMs) std::printf("BRACKETING LAG: %lld us + %lld ms = %lld us\n",
                              (long long)cap.lagUs, lagAddMs, cap.lagUs + lagAddMs * 1000);
    std::printf("vote class-mean margin: %lld us%s\n", sepUs,
                sepUs == 8 ? "  (the PRE-FIX effective gate: use this to reproduce any log"
                             " recorded before the units fix)"
                           : "  (what production passes now)");
    ReportCensus(cap, c);
    ReportResets(resets);
    ReportPresent(pres, cfg);
    ReportFloor(MeasureFloor(cap, cfg, outcomes));
    const int fails = ReportGate1(cap, c);
    if (fails) {
        std::printf("\nGATE 1 FAILED on %d of 4 criteria. Per the spec: do NOT loosen the\n"
                    "tolerance. Whatever the model is missing is the next finding.\n", fails);
        return 1;
    }
    if (cap.live.present()) std::printf("\nGATE 1 PASSED (4 of 4)\n");
    return 0;
}
