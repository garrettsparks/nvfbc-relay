#include "TemporalPolicy.h"

namespace policy {

// Map a tick offset into [-p/2, p/2): the signed distance to the nearest point on a
// p-periodic timeline. C++ % truncates toward zero, so negative remainders need folding up.
int64_t WrapHalf(int64_t d, int64_t p) {
    int64_t m = (d + p / 2) % p;
    if (m < 0) m += p;
    return m - p / 2;
}

void UpdatePhaseLock(PhaseLockState& s, const PolicyConfig& cfg, int64_t beforeDiff) {
    // Closed loop: the pull is already inside the target this error was measured at, so
    // want = pull + errEma converges instead of integrating. Error and EMAs live on the
    // circular comb domain; a linear controller here saturates against clock skew and
    // drains through a disengaged sweep every beat (measured - see the comb-lock spec).
    const int64_t err = WrapHalf(beforeDiff, cfg.combQpc);
    if (!s.seeded) {
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
    if (delta > cfg.phasePullSlewQpc) delta = cfg.phasePullSlewQpc;
    else if (delta < -cfg.phasePullSlewQpc) delta = -cfg.phasePullSlewQpc;
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

}  // namespace policy
