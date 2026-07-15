#include "TemporalCaptureMode.h"
#include <SimpleLogger.h>

// External global variables
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;

// Presents between estimator-vs-assumption audits (about 10 s at 60 Hz): rare enough to keep
// the log quiet, frequent enough that a wrong -src is caught within the first minute.
static const int kTelemetryPeriodPresents = 600;

// Source rate assumed when -src is not given: the slowest source served without
// configuration, at any present rate. Slower sources need an explicit -src.
static const float kDefaultAssumedSrcFps = 60.0f;

// Selection-outcome labels for the per-present "pick=" log field. The temporal log line is a
// stable format consumed by offline analysis — change values only deliberately.
static const char* const kPickNone      = "none";
static const char* const kPickBefore    = "before";
static const char* const kPickAfter     = "after";
static const char* const kPickAfterAdv  = "after-adv";    // only the after-frame is newer than last shown
static const char* const kPickBeforeAdv = "before-adv";
static const char* const kPickRepeat    = "repeat";       // nothing newer than last shown — genuine stall

TemporalCaptureMode::TemporalCaptureMode(float framerate, bool vsyncPresent, float srcRateHint, bool noLock)
    : m_bracketingDelayQpc(0)
    , m_assumedSrcPeriodQpc(0)
    , m_stickinessQpc(0)
    , m_combQpc(0)
    , m_phasePullQpc(0)
    , m_phaseErrEmaQpc(0)
    , m_phaseDevEmaQpc(0)
    , m_phasePullSlewQpc(0)
    , m_telemetryCountdown(0)
    , m_phaseSeeded(false)
    , m_lockEngaged(false)
    , m_noLock(noLock)
    , m_lastPickAfter(false)
    , m_advGateOpen(true)
    , m_vsyncPresent(vsyncPresent)
    , m_targetFramerate(framerate)
    , m_srcRateHint(srcRateHint)
    , m_device(NULL)
{
    m_baseQpc.QuadPart = 0;
}

LONGLONG TemporalCaptureMode::LagForSourcePeriod(LONGLONG srcPeriodQpc) const {
    LONGLONG lag = srcPeriodQpc + srcPeriodQpc / 4;
    if (lag < m_scheduler.PeriodQpc()) lag = m_scheduler.PeriodQpc();
    return lag;
}

UINT TemporalCaptureMode::GetPresentationInterval() const {
    // vsync present needs the device created with INTERVAL_ONE so PresentEx blocks on vsync.
    // NOTE: windowed INTERVAL_ONE blocks on DWM's compose clock, whose identity is
    // regime-dependent: composed desktop → primary/source display; fullscreen game on the
    // source → DWM composes only the card's display and the present is card-locked 60 Hz
    // (the production case). See spec Rounds 5-10.
    return m_vsyncPresent ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
}

bool TemporalCaptureMode::Setup() {
    m_device = g_pD3D9Device;

    if (!m_ring.Setup(m_device, BUF_WIDTH, BUF_HEIGHT)) {
        return false;
    }
    if (!m_scheduler.Setup(m_targetFramerate)) {
        return false;
    }
    // STATIC BRACKETING LAG: max(present period, 1.25 x assumed source period). The lag
    // exists so that a frame newer than the target has already arrived at pick time; the
    // worst-case wait is one source period, and 1.25x covers arrival jitter on top of it.
    // The assumption defaults to 60 fps (the slowest source served without configuration);
    // -src overrides it in either direction: slower sources need more lag, faster ones can
    // ride the present-period floor. Computed once and never moved: the lag is output
    // latency, and only a constant can be compensated for downstream (T10). The measured
    // source period is not fed back into the lag; it only audits the assumption (telemetry).
    const float assumedFps = (m_srcRateHint > 0.0f) ? m_srcRateHint : kDefaultAssumedSrcFps;
    m_assumedSrcPeriodQpc = (LONGLONG)((double)m_scheduler.Freq() / assumedFps);
    m_bracketingDelayQpc = LagForSourcePeriod(m_assumedSrcPeriodQpc);
    m_telemetryCountdown = kTelemetryPeriodPresents;

    // Selection stickiness (Schmitt band): prefer the before-frame unless the after-frame is
    // closer by more than this margin. Without it, when the target dwells near the midpoint
    // between two source frames, capture jitter (~±300 µs) flips the nearest-pick every present
    // — measured at 240→60 (v0.0.10 validation) as ~6 s windows of stride-3/5 alternation
    // (period-2 judder) every ~33.5 s. With the band, the pick holds one side through the dwell
    // and slips exactly once per sweep (a single 1-source-frame step — imperceptible). 1 ms:
    // comfortably above jitter, well below any source period we target (4.17 ms at 240 Hz).
    m_stickinessQpc = m_scheduler.Freq() / 1000;

    LOG("Temporal mode initialized - %s present (%.2f fps nominal), nearest-frame selection + hysteresis",
        m_vsyncPresent ? "vsync/vblank" : "QPC-timer", m_targetFramerate);
    LOG("Temporal lag fixed at %lld us (source assumed %s%.1f fps)",
        m_bracketingDelayQpc * 1000000 / m_scheduler.Freq(),
        (m_srcRateHint > 0.0f) ? "-src " : ">= ", assumedFps);
    LOG("Selection stickiness band: %lld us (anti flip-flop at bracket midpoint)",
        m_stickinessQpc * 1000000 / m_scheduler.Freq());

    // PHASE COMB LOCK (see docs/phase-comb-lock-spec.md). Each present the target's phase
    // within a source interval advances by (presentP mod srcP); at a rational rate ratio
    // N:M (reduced) it visits exactly M values spaced srcP/M apart - the comb. A slow
    // control term (the pull, applied as extra lag) locks the target onto the comb, so
    // selection operates at a stable phase just behind each real frame instead of sweeping
    // through the bracket every beat: the boundary-dwell excursions (gate-decline repeat,
    // then a multi-frame catch-up) become unreachable, and one comb-spacing slip per beat
    // remains - the same slip an unlocked beat already pays. Anchored ONLY to an explicit
    // -src: anchoring the default-60 fallback would manufacture false locks on undeclared
    // sources. The denominator scan is capped: past M=8 the comb spacing approaches arrival
    // jitter, the stability gate cannot close, and the lock refuses - correct for
    // effectively-irrational ratios. Latency: the pull adds a bounded slow sawtooth
    // (<= one comb spacing peak-to-peak, drift-rate ramp, one discrete step per beat),
    // accepted as a documented trade alongside the static lag (spec clause 4).
    m_phasePullSlewQpc = m_scheduler.Freq() / 40000;   // 25 us per present
    if (m_srcRateHint > 0.0f && !m_noLock) {
        int combM = 1;
        const double ratio = (double)m_srcRateHint / (double)m_targetFramerate;
        for (int m = 1; m <= 8; m++) {
            const double nm = ratio * (double)m;
            const long long n = (long long)(nm + 0.5);
            const double frac = nm - (double)n;
            if (n >= 1 && frac > -0.02 && frac < 0.02) { combM = m; break; }
        }
        m_combQpc = m_assumedSrcPeriodQpc / combM;
        LOG("Phase comb lock ACTIVE: modulus %lld us (ratio denominator M=%d); pull=/lk= on the temporal line",
            m_combQpc * 1000000 / m_scheduler.Freq(), combM);
    } else {
        LOG("Phase comb lock inactive (%s); target rides the static lag alone",
            m_noLock ? "-nolock" : "no explicit -src");
    }
    return true;
}

// Map a tick offset into (-p/2, p/2]: the signed distance to the nearest point on a
// p-periodic timeline. C++ % truncates toward zero, so negative remainders need folding up.
static LONGLONG WrapHalf(LONGLONG d, LONGLONG p) {
    LONGLONG m = (d + p / 2) % p;
    if (m < 0) m += p;
    return m - p / 2;
}

void TemporalCaptureMode::UpdatePhaseLock(LONGLONG beforeDiffQpc) {
    // Closed loop: the pull is already inside the target this error was measured at, so
    // want = pull + errEma converges instead of integrating. Error and EMAs live on the
    // circular comb domain; a linear controller here saturates against clock skew and
    // drains through a disengaged sweep every beat (measured - see the spec).
    const LONGLONG err = WrapHalf(beforeDiffQpc, m_combQpc);
    if (!m_phaseSeeded) {
        m_phaseErrEmaQpc = err;
        m_phaseSeeded = true;
    } else {
        m_phaseErrEmaQpc += WrapHalf(err - m_phaseErrEmaQpc, m_combQpc) / 16;
        m_phaseErrEmaQpc = WrapHalf(m_phaseErrEmaQpc, m_combQpc);
    }
    LONGLONG dev = WrapHalf(err - m_phaseErrEmaQpc, m_combQpc);
    if (dev < 0) dev = -dev;
    m_phaseDevEmaQpc = (m_phaseDevEmaQpc * 15 + dev) / 16;

    // Stability gate: stable phase = near-rational ratio, lock possible; sweeping phase =
    // genuine rate conversion, the pull decays to zero and selection proceeds unlocked.
    m_lockEngaged = m_phaseDevEmaQpc < m_combQpc / 8;
    const LONGLONG want = m_lockEngaged ? m_phasePullQpc + m_phaseErrEmaQpc : 0;
    LONGLONG delta = want - m_phasePullQpc;
    if (delta > m_phasePullSlewQpc) delta = m_phasePullSlewQpc;
    else if (delta < -m_phasePullSlewQpc) delta = -m_phasePullSlewQpc;
    m_phasePullQpc += delta;

    // Wrap hysteresis: the pull may overshoot the [0, comb) domain by a band before
    // wrapping, so jitter-scale wander at the boundary cannot chatter one-frame slips.
    const LONGLONG band = m_combQpc / 16;
    if (m_phasePullQpc < -band) m_phasePullQpc += m_combQpc;
    else if (m_phasePullQpc >= m_combQpc + band) m_phasePullQpc -= m_combQpc;
}

void TemporalCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    QueryPerformanceCounter(&m_baseQpc);
    const double usPerTick = 1000000.0 / (double)m_scheduler.Freq();
    const long long lagUs = (long long)(m_bracketingDelayQpc * usPerTick);

    // Note: Start releases nvfbcDx9 (the session bound to the present device) and rebinds
    // NvFBC to the ring's private capture device. nvfbcDx9 must not be used after this call.
    if (!m_ring.Start(nvfbcDx9, grabParams, m_baseQpc, hwnd)) {
        return;
    }

    MSG msg = {};
    RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };
    LONGLONG lastPresentQpc = 0;
    LONGLONG lastShownTs = 0;   // hysteresis: QPC of the last presented frame (strictly advances)
    IDirect3DSurface9* lastShownSurface = NULL;   // what pick=repeat must re-present
    m_scheduler.Seed();

    while (TRUE)
    {
        // Present timing. Timer mode waits on the absolute-QPC deadline. Vsync mode lets the
        // INTERVAL_ONE present (below) be the wait, and anchors the target to "now" (just after
        // the previous compose tick) so selection runs on DWM's compose clock rather than QPC.
        // That clock is regime-dependent: composed desktop → primary/source display; fullscreen
        // game on the source → card-locked 60 Hz (spec Rounds 5-10).
        LONGLONG deadline;
        if (m_vsyncPresent) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            deadline = now.QuadPart;
        } else {
            m_scheduler.WaitUntilDeadline();
            deadline = m_scheduler.Deadline();
        }
        // Comb lock applies the pull as extra lag; zero when disabled or disengaged. The
        // pull was computed from LAST present's bracket (closed loop, one-present latency
        // in the control path - negligible at 25 us/present slew).
        const LONGLONG target = deadline - (m_bracketingDelayQpc + m_phasePullQpc);

        // Select: nearest-to-target frame, with HYSTERESIS — present frames in strictly
        // increasing timestamp order. Among the bracket frames NEWER than the last presented
        // one, pick the nearest to target. If neither is newer (capture produced no new frame
        // this period — the genuine rate-matching stall at the drift boundary), repeat the
        // last frame: that is the one unavoidable dupe per drift sweep (~6/5min at 60->60).
        // This removes the boundary wobble where plain nearest-pick re-shows a frame ~14 times
        // over the ~1s the phase lingers at the alignment point.
        //
        // STICKINESS BAND (Schmitt trigger): when both frames are candidates, the threshold
        // depends on which side the LAST pick took — stay on that side unless the other frame
        // is closer by more than m_stickinessQpc. Plain nearest-pick lets ±300 µs capture
        // jitter flip the choice every present while the target dwells near the bracket
        // midpoint — at 240→60 that alternates the stride (3/5 instead of 4) for the several
        // seconds the dwell lasts, a visible period-2 judder window every drift sweep. The
        // state bit is what makes this hysteresis: jitter would have to cross the full 2·band
        // gap to flip the pick (a memoryless bias just relocates the flip-flop boundary —
        // measured, it did not shrink the windows), while slow drift still crosses the gap
        // exactly once per sweep: one clean single-frame slip instead of seconds of judder.
        FrameBracket bracket;
        m_ring.FindBracket(target, &bracket);

        // Update the comb-lock pull for the next present. Skipped when the bracket is
        // incomplete (startup, stalls): the pull freezes rather than integrating on a
        // one-sided error, and the frozen value stays bounded by construction.
        if (m_combQpc > 0 && bracket.hasBefore && bracket.hasAfter) {
            UpdatePhaseLock(bracket.beforeDiff);
        }

        bool beforeNew = bracket.hasBefore && bracket.beforeTs > lastShownTs;
        bool afterNew  = bracket.hasAfter  && bracket.afterTs  > lastShownTs;

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
        if (advance && bracket.hasBefore && !beforeNew) {
            const LONGLONG reopen = m_advGateOpen ? m_stickinessQpc : 2 * m_stickinessQpc;
            m_advGateOpen = bracket.beforeDiff >= reopen;
            advance = m_advGateOpen;
        }

        IDirect3DSurface9* chosen = NULL;
        const char* pick = kPickNone;
        if (beforeNew && afterNew) {
            const LONGLONG bias = m_lastPickAfter ? -m_stickinessQpc : m_stickinessQpc;
            if (bracket.beforeDiff <= bracket.afterDiff + bias) { chosen = bracket.beforeSurface; pick = kPickBefore; lastShownTs = bracket.beforeTs; m_lastPickAfter = false; }
            else { chosen = bracket.afterSurface; pick = kPickAfter; lastShownTs = bracket.afterTs; m_lastPickAfter = true; }
        } else if (advance) {
            chosen = bracket.afterSurface; pick = kPickAfterAdv; lastShownTs = bracket.afterTs; m_lastPickAfter = true;
        } else if (beforeNew) {
            chosen = bracket.beforeSurface; pick = kPickBeforeAdv; lastShownTs = bracket.beforeTs; m_lastPickAfter = false;
        } else {
            // Nothing eligible to advance to: either a genuine stall (no frame newer than the
            // last shown) or a newer after-frame the target has not reached yet. Repeat.
            // Repeat must re-present the last SHOWN frame, and bracket.before is not always
            // it. After an after-pick the target can still trail the shown frame (lag > one
            // source period at sub-rate sources), leaving before one frame BEHIND the
            // screen, so fetching it steps the display backward.
            chosen = lastShownSurface;
            if (!chosen && bracket.hasBefore) chosen = bracket.beforeSurface;
            if (!chosen && bracket.hasAfter)  chosen = bracket.afterSurface;
            pick = kPickRepeat;
        }

        if (chosen) {
            m_device->StretchRect(chosen, &srcRect, g_backbuffer, &srcRect, D3DTEXF_NONE);
            lastShownSurface = chosen;
        }

        LARGE_INTEGER beforePresent;
        QueryPerformanceCounter(&beforePresent);
        // Timer: immediate (non-blocking). Vsync: INTERVAL_ONE blocks until DWM's next compose
        // (source clock on a composed desktop; card clock under a fullscreen game) — this
        // present IS the frame-pacing wait in vsync mode.
        device->PresentEx(NULL, NULL, NULL, NULL,
            m_vsyncPresent ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE);

        // Inter-present interval (should hold steady at the present period if the scheduler works).
        LONGLONG presentDelta = (lastPresentQpc != 0) ? (beforePresent.QuadPart - lastPresentQpc) : 0;
        lastPresentQpc = beforePresent.QuadPart;

        // Logging: bracket timestamps double as the source timeline; w is what blend would use;
        // jit is actual-present vs scheduled deadline; pdt is the actual inter-present gap.
        if (!bracket.hasBefore) {
            // Benign while the ring is still filling at startup; once it has wrapped at least
            // once it means the target fell off the back of the ring.
            if (m_ring.Published() >= CaptureRing::RING_SIZE) {
                LOGERR("temporal: target older than ring window - ring too small / delay too large (p=%lld)",
                    m_ring.Published());
            }
        } else {
            // pull/lk are append-only: effective latency = lag + pull (a bounded sawtooth
            // at lock); lk=-1 marks the lock feature disabled entirely.
            LOG("temporal dl=%lldus tgt=%lldus before=%lldus(d%d) after=%lldus w=%.3f pick=%s jit=%lldus pdt=%lldus lag=%lldus pull=%lldus lk=%d",
                (long long)((deadline - m_baseQpc.QuadPart) * usPerTick),
                (long long)((target - m_baseQpc.QuadPart) * usPerTick),
                (long long)((bracket.beforeTs - m_baseQpc.QuadPart) * usPerTick), bracket.beforeDepth,
                bracket.hasAfter ? (long long)((bracket.afterTs - m_baseQpc.QuadPart) * usPerTick) : -1LL,
                bracket.weight, pick,
                (long long)((beforePresent.QuadPart - deadline) * usPerTick),
                (long long)(presentDelta * usPerTick),
                lagUs,
                (long long)(m_phasePullQpc * usPerTick),
                (m_combQpc > 0) ? (m_lockEngaged ? 1 : 0) : -1);
            if (!bracket.hasAfter) {
                LOG("temporal: no after-frame (source slower than present?) - repeating newest");
            }
        }

        // ESTIMATOR TELEMETRY: the measured source period never drives the lag; it audits
        // the declared assumption. Slower than assumed means the bracketing headroom is gone
        // and repeats follow (wrong -src, or the source is struggling); much faster than
        // assumed means latency is left on the table, and a lag exceeding the ring's history
        // at the measured rate means selection is pinned at the ring's oldest frame.
        if (--m_telemetryCountdown <= 0) {
            m_telemetryCountdown = kTelemetryPeriodPresents;
            const LONGLONG est = m_ring.EstimatedSourcePeriodQpc();
            if (est > 0) {
                const double estFps = (double)m_scheduler.Freq() / (double)est;
                const LONGLONG lagForEst = LagForSourcePeriod(est);
                const long long lagForEstUs = (long long)(lagForEst * usPerTick);
                if (m_bracketingDelayQpc > est * CaptureRing::RING_SIZE) {
                    LOGERR("temporal telemetry: lag %lld us exceeds ring history at the measured ~%.1f fps - display pinned at oldest frame; pass -src %.0f (lag %lld us)",
                        lagUs, estFps, estFps, lagForEstUs);
                } else if (est > m_assumedSrcPeriodQpc + m_assumedSrcPeriodQpc / 8) {
                    LOGERR("temporal telemetry: source measuring ~%.1f fps, slower than assumed - expect repeats; pass -src %.0f (lag %lld us)",
                        estFps, estFps, lagForEstUs);
                } else if (est * 2 < m_assumedSrcPeriodQpc && lagForEst + m_scheduler.Freq() / 500 < m_bracketingDelayQpc) {
                    LOG("temporal telemetry: source measuring ~%.1f fps; -src %.0f would lower lag to %lld us",
                        estFps, estFps, lagForEstUs);
                }
            }
        }

        if (!m_vsyncPresent) m_scheduler.Advance();   // vsync paces via the blocking present

        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (msg.message == WM_QUIT) break;
        if (m_ring.HasStopped()) break;  // capture thread hit a fatal error
    }

    m_ring.Stop();
}

const char* TemporalCaptureMode::GetModeName() const {
    return "Temporal";
}
