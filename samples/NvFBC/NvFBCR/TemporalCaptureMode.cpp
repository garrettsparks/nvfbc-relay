#include "TemporalCaptureMode.h"
#include "FrameCompositors.h"
#include <SimpleLogger.h>
#include <cstdio>

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

TemporalCaptureMode::TemporalCaptureMode(float framerate, bool vsyncPresent, float srcRateHint, bool lock,
                                         CompositorKind compositor, bool mark, unsigned int markFrames,
                                         bool tint)
    : m_bracketingDelayQpc(0)
    , m_assumedSrcPeriodQpc(0)
    , m_compositor(NULL)
    , m_telemetryCountdown(0)
    , m_compositorKind(compositor)
    , m_lock(lock)
    , m_mark(mark)
    , m_markFrames(markFrames)
    , m_tint(tint)
    , m_vsyncPresent(vsyncPresent)
    , m_targetFramerate(framerate)
    , m_srcRateHint(srcRateHint)
    , m_device(NULL)
{
    m_baseQpc.QuadPart = 0;
}

TemporalCaptureMode::~TemporalCaptureMode() {
    delete m_compositor;
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
    // Marker resources live on the PRESENT device (the burn is a backbuffer overlay,
    // never a ring-surface write). A failed Init disables the marker, not the relay.
    if (m_mark) {
        m_marker.Init(m_device, BUF_WIDTH, BUF_HEIGHT, m_markFrames);
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
    m_policyCfg.stickinessQpc = m_scheduler.Freq() / 1000;

    LOG("Temporal mode initialized - %s present (%.2f fps nominal), nearest-frame selection + hysteresis",
        m_vsyncPresent ? "vsync/vblank" : "QPC-timer", m_targetFramerate);
    LOG("Temporal lag fixed at %lld us (source assumed %s%.1f fps)",
        m_bracketingDelayQpc * 1000000 / m_scheduler.Freq(),
        (m_srcRateHint > 0.0f) ? "-src " : ">= ", assumedFps);
    LOG("Selection stickiness band: %lld us (anti flip-flop at bracket midpoint)",
        m_policyCfg.stickinessQpc * 1000000 / m_scheduler.Freq());

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
    m_policyCfg.phasePullSlewQpc = m_scheduler.Freq() / 40000;   // 25 us per present
    // Twice the declared source period: wide enough that ordinary jitter and a single
    // dropped frame stay under it, narrow enough that a frozen source (whose grab-timeout
    // re-grabs land ~100 ms apart) reads as stalled on the first present.
    m_policyCfg.stallSpanQpc = m_assumedSrcPeriodQpc * 2;
    if (m_lock && m_srcRateHint > 0.0f) {
        int combM = 1;
        bool combMatched = false;
        const double ratio = (double)m_srcRateHint / (double)m_targetFramerate;
        for (int m = 1; m <= 8; m++) {
            const double nm = ratio * (double)m;
            const long long n = (long long)(nm + 0.5);
            const double frac = nm - (double)n;
            if (n >= 1 && frac > -0.02 && frac < 0.02) { combM = m; combMatched = true; break; }
        }
        m_policyCfg.combQpc = m_assumedSrcPeriodQpc / combM;
        LOG("Phase comb lock ACTIVE (-lock): modulus %lld us (ratio denominator M=%d%s); pull=/lk= on the temporal line",
            m_policyCfg.combQpc * 1000000 / m_scheduler.Freq(), combM,
            combMatched ? "" : ", no rational match: M=1 fallback, stability gate decides");
    } else {
        LOG("Phase comb lock off (%s); target rides the static lag alone",
            !m_lock ? "-lock not set" : "-lock set but no -src to derive the comb");
    }

    // COMPOSITOR: nearest keeps the validated selection path; the synthesizing
    // compositors (blend, interp) pass a real frame through sharp whenever one sits
    // within the passthrough threshold of the target and synthesize at the bracket
    // weight otherwise. The threshold floors at the present period so an oversampling
    // source (whose frames are always within half a source period of any target)
    // passes through free; at-rate and slower sources get a quarter source period,
    // far above the locked operating point and far below the mid-gap distance of a
    // hole, so the gate cannot chatter.
    if (m_compositorKind != kCompositorNearest) {
        const LONGLONG thresholdBase =
            (m_assumedSrcPeriodQpc > m_scheduler.PeriodQpc()) ? m_assumedSrcPeriodQpc
                                                              : m_scheduler.PeriodQpc();
        m_policyCfg.passthroughQpc = thresholdBase / 4;
        if (m_compositorKind == kCompositorInterp) {
            m_compositor = new InterpCompositor(&m_policyCfg);
            LOG("Interp compositor ACTIVE (o mode): passthrough threshold %lld us; op=/bw=/pt= on the temporal line",
                m_policyCfg.passthroughQpc * 1000000 / m_scheduler.Freq());
        } else {
            m_compositor = new BlendCompositor(&m_policyCfg, m_tint);
            LOG("Blend compositor ACTIVE (b mode): passthrough threshold %lld us; op=/bw= on the temporal line",
                m_policyCfg.passthroughQpc * 1000000 / m_scheduler.Freq());
        }
    } else {
        m_compositor = new NearestCompositor(&m_policyCfg);
    }
    if (!m_compositor->Setup(m_device, BUF_WIDTH, BUF_HEIGHT)) {
        LOGERR("Compositor setup failed - refusing the mode");
        return false;
    }
    return true;
}

void TemporalCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    QueryPerformanceCounter(&m_baseQpc);
    // Every time in this log (arr=, dl=, tgt=, before=, after=) is microseconds since this
    // origin, not absolute QPC. Anything correlating the log against another QPC-stamped
    // source - an ETW trace, a second process - needs the origin to convert, and recovering
    // it by cross-correlating event sequences is guesswork this one line removes.
    LOG("QPC origin %lld ticks, frequency %lld Hz (log times are us since the origin)",
        m_baseQpc.QuadPart, m_scheduler.Freq());
    const double usPerTick = 1000000.0 / (double)m_scheduler.Freq();
    const long long lagUs = (long long)(m_bracketingDelayQpc * usPerTick);

    // Note: Start releases nvfbcDx9 (the session bound to the present device) and rebinds
    // NvFBC to the ring's private capture device. nvfbcDx9 must not be used after this call.
    if (!m_ring.Start(nvfbcDx9, grabParams, m_baseQpc, hwnd)) {
        return;
    }

    // Capture-side compositor resources (the interp sidecar opens ring slot shared
    // handles, which exist only now). A compositor that cannot finish initializing
    // refuses the mode instead of silently running a different one.
    if (!m_compositor->OnCaptureStarted(&m_ring, m_baseQpc, m_scheduler.Freq())) {
        LOGERR("Compositor capture-side init failed - refusing the mode");
        m_ring.Stop();
        return;
    }

    MSG msg = {};
    LONGLONG lastPresentQpc = 0;
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
        const LONGLONG target = deadline - (m_bracketingDelayQpc + m_lockState.pullQpc);

        FrameBracket bracket;
        m_ring.FindBracket(target, &bracket);

        // Update the comb-lock pull for the next present. Skipped when the bracket is
        // incomplete (startup, stalls): the pull freezes rather than integrating on a
        // one-sided error, and the frozen value stays bounded by construction.
        if (m_policyCfg.combQpc > 0) {
            const bool resumedFromStall =
                policy::UpdateStallRun(m_lockState, m_policyCfg, bracket.info);
            if (!policy::BracketIsStalled(bracket.info, m_policyCfg)) {
                policy::UpdatePhaseLock(m_lockState, m_policyCfg, bracket.info.beforeDiff,
                                        resumedFromStall);
            }
        }

        // The DECISION is pure policy (selection or composite, in TemporalPolicy.cpp
        // with the mechanism rationale); the compositor executes it onto the
        // backbuffer. This loop owns the timing, the present, and the log.
        CompositeOutcome outcome;
        m_compositor->Compose(bracket, g_backbuffer, &outcome);

        // Burn the marker over the composed backbuffer, once per present (repeats
        // included: the counter identifies presented frames, not source frames).
        // Before the present stamp, so jit/pdt absorb its cost and a -mark on/off
        // A/B measures it.
        long long markN = -1;
        if (m_mark) {
            markN = (long long)m_marker.Burn(g_backbuffer, outcome.pickCode, outcome.weightQ,
                                             outcome.synthesized, m_compositor->Id(),
                                             outcome.pixelExec);
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
        if (!bracket.info.hasBefore) {
            // Benign while the ring is still filling at startup; once it has wrapped at least
            // once it means the target fell off the back of the ring.
            if (m_ring.Published() >= CaptureRing::RING_SIZE) {
                LOGERR("temporal: target older than ring window - ring too small / delay too large (p=%lld)",
                    m_ring.Published());
            }
        } else {
            // pull/lk/mark are append-only: effective latency = lag + pull (a bounded
            // sawtooth at lock); lk=-1 marks the lock feature disabled entirely; mark=-1
            // marks the frame marker disabled (video-to-log join key otherwise).
            // op=/bw= append only in blend mode (what the compositor did and at what
            // weight); nearest lines carry no new fields.
            int lkField = -1;
            if (m_policyCfg.combQpc > 0) {
                lkField = m_lockState.engaged ? 1 : 0;
            }
            char opFields[96] = "";
            if (outcome.opLabel) {
                int n = snprintf(opFields, sizeof(opFields), " op=%s bw=%.3f",
                                 outcome.opLabel, outcome.opWeight);
                if (outcome.synthExec && n > 0 && (size_t)n < sizeof(opFields)) {
                    n += snprintf(opFields + n, sizeof(opFields) - n, " sx=%s",
                                  outcome.synthExec);
                }
                if (outcome.synthUs >= 0 && n > 0 && (size_t)n < sizeof(opFields)) {
                    snprintf(opFields + n, sizeof(opFields) - n, " pt=%lld", outcome.synthUs);
                }
            }
            LOG("temporal dl=%lldus tgt=%lldus before=%lldus(d%d) after=%lldus w=%.3f pick=%s jit=%lldus pdt=%lldus lag=%lldus pull=%lldus lk=%d mark=%lld%s",
                (long long)((deadline - m_baseQpc.QuadPart) * usPerTick),
                (long long)((target - m_baseQpc.QuadPart) * usPerTick),
                (long long)((bracket.info.beforeTs - m_baseQpc.QuadPart) * usPerTick), bracket.beforeDepth,
                bracket.info.hasAfter ? (long long)((bracket.info.afterTs - m_baseQpc.QuadPart) * usPerTick) : -1LL,
                bracket.weight, outcome.pickLabel,
                (long long)((beforePresent.QuadPart - deadline) * usPerTick),
                (long long)(presentDelta * usPerTick),
                lagUs,
                (long long)(m_lockState.pullQpc * usPerTick),
                lkField,
                markN,
                opFields);
            if (!bracket.info.hasAfter) {
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
    if (m_compositorKind == kCompositorInterp) return "Temporal interp";
    if (m_compositorKind == kCompositorBlend) return "Temporal blend";
    return "Temporal";
}
