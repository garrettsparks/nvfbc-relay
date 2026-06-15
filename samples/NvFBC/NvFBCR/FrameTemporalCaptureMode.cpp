#include "FrameTemporalCaptureMode.h"
#include <SimpleLogger.h>

// External global variables
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;

FrameTemporalCaptureMode::FrameTemporalCaptureMode(float framerate, bool vsyncPresent)
    : m_bracketingDelayQpc(0)
    , m_vsyncPresent(vsyncPresent)
    , m_targetFramerate(framerate)
    , m_device(NULL)
{
    m_baseQpc.QuadPart = 0;
}

UINT FrameTemporalCaptureMode::GetPresentationInterval() const {
    // Always IMMEDIATE. Timer present is paced by PresentScheduler; vsync present is paced by an
    // explicit WaitForVBlank on the TARGET display (VBlankWaiter). We deliberately do NOT use
    // INTERVAL_ONE: that would block on the present *device's adapter* vblank, which is the SOURCE
    // display (the device is created on source.dxAdapterIndex) — so it paced to the source refresh
    // (240Hz), not the capture-card target (60Hz). See the spec's "vsync targets the wrong display".
    return D3DPRESENT_INTERVAL_IMMEDIATE;
}

bool FrameTemporalCaptureMode::Setup() {
    m_device = g_pD3D9Device;

    if (!m_ring.Setup(m_device, BUF_WIDTH, BUF_HEIGHT)) {
        return false;
    }
    if (!m_scheduler.Setup(m_targetFramerate)) {
        return false;
    }
    // Lag the present target by one present period so the ring reliably holds a frame on each
    // side of it (with source rate >= present rate, a frame newer than the target has arrived).
    m_bracketingDelayQpc = m_scheduler.PeriodQpc();

    LOG("Temporal mode initialized - %s present (%.2f fps nominal), nearest-frame selection + hysteresis",
        m_vsyncPresent ? "vsync/vblank" : "QPC-timer", m_targetFramerate);
    return true;
}

void FrameTemporalCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    QueryPerformanceCounter(&m_baseQpc);
    const double usPerTick = 1000000.0 / (double)m_scheduler.Freq();

    // Vsync present paces on the TARGET display's vblank. The window is pseudo-fullscreen on the
    // target, so MonitorFromWindow gives the target HMONITOR. Bail loudly if it can't bind rather
    // than silently free-run (this mode's whole point is target-locked pacing).
    if (m_vsyncPresent && !m_vblank.Setup(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST))) {
        LOGERR("Temporal: could not bind vblank waiter to target display - aborting vsync mode");
        return;
    }

    // Note: Start releases nvfbcDx9 (the session bound to the present device) and rebinds
    // NvFBC to the ring's private capture device. nvfbcDx9 must not be used after this call.
    if (!m_ring.Start(nvfbcDx9, grabParams, m_baseQpc, hwnd)) {
        return;
    }

    MSG msg = {};
    RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };
    LONGLONG lastPresentQpc = 0;
    LONGLONG lastShownTs = 0;   // hysteresis: QPC of the last presented frame (strictly advances)
    m_scheduler.Seed();

    while (TRUE)
    {
        // Present timing. Timer mode waits on the absolute-QPC deadline. Vsync mode blocks on the
        // TARGET display's vblank (VBlankWaiter), then anchors the target to "now" (just after that
        // vblank) so selection runs on the target's display clock rather than QPC — the variable
        // this t:vsync experiment isolates against the t:60 present. (The wait is on the named
        // output, so unlike INTERVAL_ONE it is immune to which adapter the present device is on.)
        LONGLONG deadline;
        if (m_vsyncPresent) {
            if (!m_vblank.Wait()) break;   // output lost (adapter reset/unplug)
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
        FrameBracket bracket;
        m_ring.FindBracket(target, &bracket);

        bool beforeNew = bracket.hasBefore && bracket.beforeTs > lastShownTs;
        bool afterNew  = bracket.hasAfter  && bracket.afterTs  > lastShownTs;

        IDirect3DSurface9* chosen = NULL;
        const char* pick = "none";
        if (beforeNew && afterNew) {
            if (bracket.beforeDiff <= bracket.afterDiff) { chosen = bracket.beforeSurface; pick = "before"; lastShownTs = bracket.beforeTs; }
            else { chosen = bracket.afterSurface; pick = "after"; lastShownTs = bracket.afterTs; }
        } else if (afterNew) {
            chosen = bracket.afterSurface; pick = "after-adv"; lastShownTs = bracket.afterTs;   // advance past last
        } else if (beforeNew) {
            chosen = bracket.beforeSurface; pick = "before-adv"; lastShownTs = bracket.beforeTs;
        } else {
            // No frame newer than the last shown: genuine stall — repeat (the fundamental dupe).
            chosen = bracket.hasBefore ? bracket.beforeSurface : (bracket.hasAfter ? bracket.afterSurface : NULL);
            pick = "repeat";
        }

        if (chosen) {
            m_device->StretchRect(chosen, &srcRect, g_backbuffer, &srcRect, D3DTEXF_NONE);
        }

        LARGE_INTEGER beforePresent;
        QueryPerformanceCounter(&beforePresent);
        // Always a non-blocking present. Pacing already happened above (timer deadline, or the
        // WaitForVBlank on the target). In windowed/DWM mode the immediate present is composited
        // tear-free; what matters is that we produce exactly one new frame per pacing tick.
        device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);

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
            LOG("temporal dl=%lldus tgt=%lldus before=%lldus(d%d) after=%lldus w=%.3f pick=%s jit=%lldus pdt=%lldus",
                (long long)((deadline - m_baseQpc.QuadPart) * usPerTick),
                (long long)((target - m_baseQpc.QuadPart) * usPerTick),
                (long long)((bracket.beforeTs - m_baseQpc.QuadPart) * usPerTick), bracket.beforeDepth,
                bracket.hasAfter ? (long long)((bracket.afterTs - m_baseQpc.QuadPart) * usPerTick) : -1LL,
                bracket.weight, pick,
                (long long)((beforePresent.QuadPart - deadline) * usPerTick),
                (long long)(presentDelta * usPerTick));
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

const char* FrameTemporalCaptureMode::GetModeName() const {
    return "Temporal";
}
