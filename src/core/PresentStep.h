#pragma once

#include "TemporalPolicy.h"

// One present's policy, in the order the relay runs it (design: docs/present-step-spec.md):
// deadline -> target (lag, pull, ease) -> lookahead move lands -> stage-6 walk -> bracket read
// -> stall run, lock update, resume guard -> recent frames, lookahead update -> guard exemption
// -> decision. The relay's loop, the flow planner and the replay all call StepPresent, so the
// corpus runs the code the relay runs and the planner predicts with the rule that will decide.
// The pacing wait, the present, the compositor's drawing, the marker and logging stay with the
// caller; the step returns what happened and the caller logs it.

namespace policy {

// Every piece of policy state that crosses presents. Plain values, so the planner can copy it
// and step the copy without touching the real one.
struct PresentStepState {
    PhaseLockState lock;
    PhaseLookahead lookahead;
    SelectionState sel;       // the nearest decision's memory
    CompositeState comp;      // the synthesizing decision's memory
    StampOverlay overlay;     // stage-6 corrections, applied at every ring read
    AnchorChain chain;        // stage-6 stride anchor
    long long nextBatch = 0;  // the next batch the stage-6 walk measures
    int64_t maxTarget = INT64_MIN;   // newest target consumed: the stage-6 fence
    PresentHistory presentHist;
};

// The capture ring as the step reads it. The relay's wraps CaptureRing; the replay's wraps its
// arrays and its own model of when each frame is in the ring.
class IRingView {
public:
    virtual ~IRingView() {}
    // The nearest usable frame on each side of the target, stamps read through the overlay.
    virtual void FindBracket(int64_t target, const StampOverlay* overlay,
                             BracketInfo* out) const = 0;
    // The frames around the target for the lookahead (see RecentFrames), read as above.
    virtual void ReadRecentFrames(int64_t target, const StampOverlay* overlay,
                                  RecentFrames* out) const = 0;
    // Batches opened so far, and the batch-start stamp of batch i. Only the last
    // BatchHistory() of them are kept; older entries have been overwritten.
    virtual long long BatchOpens() const = 0;
    virtual int64_t BatchStartAt(long long i) const = 0;
    virtual long long BatchHistory() const = 0;
};

// Where the stage-6 walk measures a batch's delivery lateness (policy::MeasureLateness against
// the source display's flips). The relay's reads its ETW consumer, the replay's the flips the
// fixture recorded. The source owns the display head and the cadence window.
class ILatenessSource {
public:
    virtual ~ILatenessSource() {}
    virtual LateCorrection MeasureLateness(int64_t batchStart, AnchorChain& chain) = 0;
};

// A stage-6 verdict on a batch that measured late.
enum class DejitVerdict { Corrected, FenceBlocked, LockDeclined };

struct LateBatch {
    int64_t batchStart = 0;
    int64_t correction = 0;
    DejitVerdict verdict = DejitVerdict::Corrected;
};

struct PresentStepResult {
    int64_t target = 0;               // the target the bracket was read at
    int64_t meanPresent = 0;          // RecordPresent's mean interval
    int64_t rephase = 0;              // the lookahead move that landed on this present (0: none)
    BracketInfo bracket;              // as the ring returned it, toothGuardExempt filled in
    bool resumedFromStall = false;
    bool lockUpdated = false;         // the lock took a step on this present's bracket
    // The selection pick when cfg.passthroughQpc is 0, else the composite decision.
    Pick pick = Pick::None;
    CompositeDecision decision;
    // The lookahead's update (None when it did not run) and the move it describes.
    LookaheadEvent lookaheadEvent = LookaheadEvent::None;
    int64_t lookaheadSize = 0;
    int64_t lookaheadFirstTs = 0;
    // Stage 6 on this present: batches measured, anchored on the flip grid and found late, the
    // late ones' verdicts (in order, the first kMaxLateBatches listed), and batches skipped
    // because the ring's batch history lapped the walk.
    static const int kMaxLateBatches = 64;
    int dejitMeasured = 0;
    int dejitAnchored = 0;
    int dejitLate = 0;
    int dejitCorrected = 0;
    int dejitFenceBlocked = 0;
    int dejitLockDeclined = 0;
    long long dejitSkipped = 0;
    int nLateBatches = 0;
    LateBatch lateBatches[kMaxLateBatches];
};

// For the replay only. In: a target to take in place of the step's own once any lookahead move
// has landed, with the pull set to match, so every decision after comes from a live capture's
// timeline. Out: what UpdateLookahead was handed, taken before the call, so a tool can make the
// call again at another target.
struct PresentStepHooks {
    bool forceTarget = false;
    int64_t forcedTarget = 0;
    bool lookaheadRan = false;
    RecentFrames frames;
    PhaseLockState lock;
    PhaseLookahead lookahead;
};

// One present. deadline is the time this present is for, from the caller's pacing (vblank,
// compose tick or timer); bracketingLag the static lag behind it. lateness null leaves the
// stage-6 walk out (stage 6 off, or the planner stepping a copy); the overlay still applies.
PresentStepResult StepPresent(PresentStepState& s, const PolicyConfig& cfg, const IRingView& ring,
                              ILatenessSource* lateness, int64_t deadline, int64_t bracketingLag,
                              PresentStepHooks* hooks = nullptr);

// The next count presents as the step will decide them, from the ring as it is now: a copy of
// the state stepped once per present, at deadlines presentPeriod apart after deadline (this
// present's). No stage-6 walk: its chain is sequential state that must not advance on a copy,
// and a correction it would find lands one present later at most. out[j] is present j + 1's
// result: its target, bracket and decision. The flow planner flows the pairs of the ones that
// synthesize. What it cannot know is a frame that has not arrived yet, a correction made in
// between, and the deadline's jitter (docs/present-step-spec.md, 3.4).
void PlanNextPresents(const PresentStepState& s, const PolicyConfig& cfg, const IRingView& ring,
                      int64_t deadline, int64_t presentPeriod, int64_t bracketingLag, int count,
                      PresentStepResult* out);

}  // namespace policy
