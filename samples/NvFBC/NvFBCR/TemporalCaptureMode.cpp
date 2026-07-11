#include "TemporalCaptureMode.h"
#include <SimpleLogger.h>

// External global variables
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;
extern float g_srcRateHint;   // -src <fps>: declared source rate; 0 = unset (assume >= 60)

// Presents between estimator-vs-assumption audits (about 10 s at 60 Hz): rare enough to keep
// the log quiet, frequent enough that a wrong -src is caught within the first minute.
static const int kTelemetryPeriodPresents = 600;

// Selection-outcome labels for the per-present "pick=" log field. The temporal log line is a
// stable format consumed by offline analysis — change values only deliberately.
static const char* const kPickNone      = "none";
static const char* const kPickBefore    = "before";
static const char* const kPickAfter     = "after";
static const char* const kPickAfterAdv  = "after-adv";    // only the after-frame is newer than last shown
static const char* const kPickBeforeAdv = "before-adv";
static const char* const kPickRepeat    = "repeat";       // nothing newer than last shown — genuine stall

TemporalCaptureMode::TemporalCaptureMode(float framerate, bool vsyncPresent)
    : m_bracketingDelayQpc(0)
    , m_assumedSrcPeriodQpc(0)
    , m_stickinessQpc(0)
    , m_telemetryCountdown(0)
    , m_lastPickAfter(false)
    , m_advGateOpen(true)
    , m_vsyncPresent(vsyncPresent)
    , m_targetFramerate(framerate)
    , m_device(NULL)
{
    m_baseQpc.QuadPart = 0;
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
    const float assumedFps = (g_srcRateHint > 0.0f) ? g_srcRateHint : 60.0f;
    m_assumedSrcPeriodQpc = (LONGLONG)((double)m_scheduler.Freq() / assumedFps);
    m_bracketingDelayQpc = m_assumedSrcPeriodQpc + m_assumedSrcPeriodQpc / 4;
    if (m_bracketingDelayQpc < m_scheduler.PeriodQpc()) m_bracketingDelayQpc = m_scheduler.PeriodQpc();
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
        (g_srcRateHint > 0.0f) ? "-src " : ">= ", assumedFps);
    LOG("Selection stickiness band: %lld us (anti flip-flop at bracket midpoint)",
        m_stickinessQpc * 1000000 / m_scheduler.Freq());
    return true;
}

void TemporalCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    QueryPerformanceCounter(&m_baseQpc);
    const double usPerTick = 1000000.0 / (double)m_scheduler.Freq();

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
        const LONGLONG target = deadline - m_bracketingDelayQpc;

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
            LOG("temporal dl=%lldus tgt=%lldus before=%lldus(d%d) after=%lldus w=%.3f pick=%s jit=%lldus pdt=%lldus lag=%lldus",
                (long long)((deadline - m_baseQpc.QuadPart) * usPerTick),
                (long long)((target - m_baseQpc.QuadPart) * usPerTick),
                (long long)((bracket.beforeTs - m_baseQpc.QuadPart) * usPerTick), bracket.beforeDepth,
                bracket.hasAfter ? (long long)((bracket.afterTs - m_baseQpc.QuadPart) * usPerTick) : -1LL,
                bracket.weight, pick,
                (long long)((beforePresent.QuadPart - deadline) * usPerTick),
                (long long)(presentDelta * usPerTick),
                (long long)(m_bracketingDelayQpc * usPerTick));
            if (!bracket.hasAfter) {
                LOG("temporal: no after-frame (source slower than present?) - repeating newest");
            }
        }

        // ESTIMATOR TELEMETRY: the measured source period no longer drives the lag; it audits
        // the declared assumption. Slower than assumed means the bracketing headroom is gone
        // and repeats follow (wrong -src, or the source is struggling); much faster than
        // assumed means latency is being left on the table.
        if (--m_telemetryCountdown <= 0) {
            m_telemetryCountdown = kTelemetryPeriodPresents;
            const LONGLONG est = m_ring.EstimatedSourcePeriodQpc();
            if (est > 0) {
                const double estFps = (double)m_scheduler.Freq() / (double)est;
                LONGLONG lagForEst = est + est / 4;
                if (lagForEst < m_scheduler.PeriodQpc()) lagForEst = m_scheduler.PeriodQpc();
                if (est > m_assumedSrcPeriodQpc + m_assumedSrcPeriodQpc / 8) {
                    LOGERR("temporal telemetry: source measuring ~%.1f fps, slower than assumed - expect repeats; pass -src %.0f (lag %lld us)",
                        estFps, estFps, lagForEst * 1000000 / m_scheduler.Freq());
                } else if (est * 2 < m_assumedSrcPeriodQpc && lagForEst + m_scheduler.Freq() / 500 < m_bracketingDelayQpc) {
                    LOG("temporal telemetry: source measuring ~%.1f fps; -src %.0f would lower lag to %lld us",
                        estFps, estFps, lagForEst * 1000000 / m_scheduler.Freq());
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
