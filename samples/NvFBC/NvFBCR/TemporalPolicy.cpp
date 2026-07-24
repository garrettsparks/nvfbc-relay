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
    } else {
        s.errEmaQpc += WrapHalf(err - s.errEmaQpc, cfg.combQpc) / 16;
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
    if (!(resumedFromStall && s.engaged)) {
        if (delta > cfg.phasePullSlewQpc) delta = cfg.phasePullSlewQpc;
        else if (delta < -cfg.phasePullSlewQpc) delta = -cfg.phasePullSlewQpc;
    }
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
