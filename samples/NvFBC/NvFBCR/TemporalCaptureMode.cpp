#include "TemporalCaptureMode.h"
#include <SimpleLogger.h>

// External global variables
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;

// Selection-outcome labels for the per-present "pick=" log field. The temporal log line is a
// stable format consumed by offline analysis — change values only deliberately.
static const char* const kPickNone      = "none";
static const char* const kPickBefore    = "before";
static const char* const kPickAfter     = "after";
static const char* const kPickAfterAdv  = "after-adv";    // only the after-frame is newer than last shown
static const char* const kPickBeforeAdv = "before-adv";
static const char* const kPickRepeat    = "repeat";       // nothing newer than last shown — genuine stall
static const char* const kPickBlend     = "blend";        // composed lerp(before, after, w)
static const char* const kPickStall     = "stall";        // blend wanted a bracket, only one side exists
static const char* const kPickInterp    = "interp";       // NvOFFRUC motion-compensated frame

TemporalCaptureMode::TemporalCaptureMode(float framerate, bool vsyncPresent, TemporalCompositor comp)
    : m_bracketingDelayQpc(0)
    , m_lagSlewMaxQpc(0)
    , m_phasePullQpc(0)
    , m_phaseErrEmaQpc(0)
    , m_phaseDevEmaQpc(0)
    , m_phasePullSlewQpc(0)
    , m_stickinessQpc(0)
    , m_lastPickAfter(false)
    , m_vsyncPresent(vsyncPresent)
    , m_comp(comp)
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
    // Seed the target lag at one present period; per-present it adapts toward
    // max(present period, 1.25 x measured source period) so an after-frame exists even when
    // the source runs slower than the present rate (30-base frame gen, heavy scenes). The
    // slew limit turns regime changes into a bounded latency ramp instead of a step.
    m_bracketingDelayQpc = m_scheduler.PeriodQpc();
    m_lagSlewMaxQpc = m_scheduler.Freq() / 10000;   // 100 us per present
    m_phasePullSlewQpc = m_scheduler.Freq() / 40000; // 25 us per present (phase-pull, blend only)

    // Selection stickiness (Schmitt band): prefer the before-frame unless the after-frame is
    // closer by more than this margin. Without it, when the target dwells near the midpoint
    // between two source frames, capture jitter (~±300 µs) flips the nearest-pick every present
    // — measured at 240→60 (v0.0.10 validation) as ~6 s windows of stride-3/5 alternation
    // (period-2 judder) every ~33.5 s. With the band, the pick holds one side through the dwell
    // and slips exactly once per sweep (a single 1-source-frame step — imperceptible). 1 ms:
    // comfortably above jitter, well below any source period we target (4.17 ms at 240 Hz).
    m_stickinessQpc = m_scheduler.Freq() / 1000;

    // Blend renderer serves blend mode AND as interp's runtime fallback.
    if (m_comp != kCompositorNearest && !m_blendRenderer.Setup(m_device)) {
        LOGERR("Temporal blend/interp mode refused - BlendRenderer setup failed");
        return false;
    }

    LOG("Temporal mode initialized - %s present (%.2f fps nominal), %s",
        m_vsyncPresent ? "vsync/vblank" : "QPC-timer", m_targetFramerate,
        m_comp == kCompositorInterp ? "NvOFFRUC interp compositor (blend fallback)" :
        m_comp == kCompositorBlend ? "lerp-blend compositor" :
        "nearest-frame selection + hysteresis");
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

    // The sidecar opens ring slot shared handles, so it can only start after the ring did.
    // Setup failure is loud and non-fatal: the compose arm falls back to blend.
    if (m_comp == kCompositorInterp) {
        if (!m_sidecar.Setup(m_device, &m_ring, BUF_WIDTH, BUF_HEIGHT,
                             m_baseQpc, m_scheduler.Freq())) {
            LOGERR("Temporal interp: sidecar setup failed - running with blend fallback");
        }
    }

    MSG msg = {};
    RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };
    LONGLONG lastPresentQpc = 0;
    LONGLONG lastShownTs = 0;   // hysteresis: QPC of the last presented frame (strictly advances)
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
        // ADAPTIVE BRACKETING DELAY: follow the capture thread's source-period estimate.
        // 1.25x covers arrival jitter on top of one full source period; the estimate is 0
        // until the ring warms up, leaving the seed value (one present period) in effect.
        {
            const LONGLONG srcP = m_ring.EstimatedSourcePeriodQpc();
            LONGLONG desired = m_scheduler.PeriodQpc();
            if (srcP > 0) {
                const LONGLONG fromSrc = srcP + srcP / 4;
                if (fromSrc > desired) desired = fromSrc;
            }
            LONGLONG delta = desired - m_bracketingDelayQpc;
            if (delta > m_lagSlewMaxQpc) delta = m_lagSlewMaxQpc;
            else if (delta < -m_lagSlewMaxQpc) delta = -m_lagSlewMaxQpc;
            m_bracketingDelayQpc += delta;
        }
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

        // PHASE PULL (blend only): w is the phase offset between the present and capture
        // clocks — it drifts through [0,1] over each beat period and is essentially never
        // 0/1, so an interpolating compositor synthesizes EVERY frame even at matched rates
        // where passthrough is pixel-perfect. Fix: a slow control term pulls the target back
        // onto the before-frame timeline (increasing lag by the filtered beforeDiff — the
        // safe direction, it adds bracket margin). Engaged only while the phase is STABLE
        // (devEma small = near-integer rate ratio, lock possible); at sweeping ratios
        // (90->60) the pull returns to zero and interpolation proceeds — it is genuinely
        // needed there. See docs/phase-alignment-spec.md.
        if (m_comp != kCompositorNearest && bracket.hasBefore && bracket.hasAfter) {
            const LONGLONG err = bracket.beforeDiff;
            m_phaseErrEmaQpc = m_phaseErrEmaQpc ? (m_phaseErrEmaQpc * 15 + err) / 16 : err;
            LONGLONG dev = err - m_phaseErrEmaQpc;
            if (dev < 0) dev = -dev;
            m_phaseDevEmaQpc = (m_phaseDevEmaQpc * 15 + dev) / 16;

            const LONGLONG srcP = m_ring.EstimatedSourcePeriodQpc();
            LONGLONG want = 0;
            if (srcP > 0 && m_phaseDevEmaQpc < srcP / 8) {
                want = m_phasePullQpc + m_phaseErrEmaQpc;   // absorb the measured offset
                if (want > srcP) want = srcP;               // bounded: <= one source period
            }
            // Asymmetric slew: approach a lock at full rate; back away at quarter rate so a
            // transient wrap-resync (dev spike once per beat) dents the pull instead of
            // draining it. Persistent disengage (non-integer ratio) still decays to zero.
            const LONGLONG up = m_phasePullSlewQpc;
            const LONGLONG down = m_phasePullSlewQpc / 4;
            LONGLONG delta = want - m_phasePullQpc;
            if (delta > up) delta = up;
            else if (delta < -down) delta = -down;
            m_phasePullQpc += delta;
        }

        bool beforeNew = bracket.hasBefore && bracket.beforeTs > lastShownTs;
        bool afterNew  = bracket.hasAfter  && bracket.afterTs  > lastShownTs;

        IDirect3DSurface9* chosen = NULL;
        const char* pick = kPickNone;
        if (m_comp != kCompositorNearest) {
            // BLEND COMPOSE: render lerp(before, after, w) at the target instant. No
            // hysteresis or Schmitt band — there is no discrete pick to oscillate; the
            // schedule's monotonic targets guarantee monotonic content time. When the
            // bracket is one-sided (source stall, warmup) present the existing side
            // unblended; the adaptive lag makes that rare.
            if (bracket.hasBefore && bracket.hasAfter) {
                if (m_comp == kCompositorInterp && m_sidecar.Enabled()
                    && m_sidecar.Interpolate(bracket, target)) {
                    chosen = m_sidecar.OutputSurface9(); pick = kPickInterp;
                } else if (m_blendRenderer.Blend(bracket.beforeTexture, bracket.afterTexture, (float)bracket.weight)) {
                    pick = kPickBlend;
                } else {
                    chosen = bracket.beforeSurface; pick = kPickStall;
                }
            } else if (bracket.hasBefore) {
                chosen = bracket.beforeSurface; pick = kPickStall;
            } else if (bracket.hasAfter) {
                chosen = bracket.afterSurface; pick = kPickStall;
            } else {
                pick = kPickRepeat;
            }
        } else if (beforeNew && afterNew) {
            const LONGLONG bias = m_lastPickAfter ? -m_stickinessQpc : m_stickinessQpc;
            if (bracket.beforeDiff <= bracket.afterDiff + bias) { chosen = bracket.beforeSurface; pick = kPickBefore; lastShownTs = bracket.beforeTs; m_lastPickAfter = false; }
            else { chosen = bracket.afterSurface; pick = kPickAfter; lastShownTs = bracket.afterTs; m_lastPickAfter = true; }
        } else if (afterNew) {
            chosen = bracket.afterSurface; pick = kPickAfterAdv; lastShownTs = bracket.afterTs; m_lastPickAfter = true;    // advance past last
        } else if (beforeNew) {
            chosen = bracket.beforeSurface; pick = kPickBeforeAdv; lastShownTs = bracket.beforeTs; m_lastPickAfter = false;
        } else {
            // No frame newer than the last shown: genuine stall — repeat (the fundamental dupe).
            chosen = bracket.hasBefore ? bracket.beforeSurface : (bracket.hasAfter ? bracket.afterSurface : NULL);
            pick = kPickRepeat;
        }

        if (chosen) {
            m_device->StretchRect(chosen, &srcRect, g_backbuffer, &srcRect, D3DTEXF_NONE);
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
            LOG("temporal dl=%lldus tgt=%lldus before=%lldus(d%d) after=%lldus w=%.3f pick=%s jit=%lldus pdt=%lldus lag=%lldus pull=%lldus",
                (long long)((deadline - m_baseQpc.QuadPart) * usPerTick),
                (long long)((target - m_baseQpc.QuadPart) * usPerTick),
                (long long)((bracket.beforeTs - m_baseQpc.QuadPart) * usPerTick), bracket.beforeDepth,
                bracket.hasAfter ? (long long)((bracket.afterTs - m_baseQpc.QuadPart) * usPerTick) : -1LL,
                bracket.weight, pick,
                (long long)((beforePresent.QuadPart - deadline) * usPerTick),
                (long long)(presentDelta * usPerTick),
                (long long)(m_bracketingDelayQpc * usPerTick),
                (long long)(m_phasePullQpc * usPerTick));
            if (!bracket.hasAfter) {
                LOG("temporal: no after-frame (source slower than present?) - repeating newest");
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
    return m_comp == kCompositorInterp ? "TemporalInterp"
         : m_comp == kCompositorBlend ? "TemporalBlend" : "Temporal";
}
