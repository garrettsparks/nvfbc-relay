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
    // vsync present needs the device created with INTERVAL_ONE so PresentEx blocks on vblank.
    // The present device is created on the TARGET adapter (see PresentsOnTargetAdapter /
    // NvFBCR.cpp), so INTERVAL_ONE blocks on the capture-card vblank, not the source's.
    return m_vsyncPresent ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
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

    // Diagnostic (vsync only): does the present device's output (the capture card) actually have a
    // real vblank that INTERVAL_ONE can lock to? Log the advertised present intervals once, and for
    // the first kDiagN presents log the real PresentEx block time + GetRasterStatus. This settles
    // whether INTERVAL_ONE blocks here (~16.6ms = real vblank) or free-runs (~µs = no vblank).
    int diagCount = 0;
    const int kDiagN = 30;
    if (m_vsyncPresent) {
        D3DCAPS9 caps;
        if (SUCCEEDED(device->GetDeviceCaps(&caps))) {
            // Interval dividers (TWO/THREE/FOUR) are fullscreen-only; if FS gives a real vblank
            // they'd allow a hardware-locked divided present rate. Log all of them.
            LOG("diag caps: PresentationIntervals=0x%08x ONE=%d TWO=%d THREE=%d FOUR=%d IMMEDIATE=%d",
                caps.PresentationIntervals,
                (caps.PresentationIntervals & D3DPRESENT_INTERVAL_ONE) != 0,
                (caps.PresentationIntervals & D3DPRESENT_INTERVAL_TWO) != 0,
                (caps.PresentationIntervals & D3DPRESENT_INTERVAL_THREE) != 0,
                (caps.PresentationIntervals & D3DPRESENT_INTERVAL_FOUR) != 0,
                (caps.PresentationIntervals & D3DPRESENT_INTERVAL_IMMEDIATE) != 0);
        } else {
            LOG("diag caps: GetDeviceCaps failed");
        }
    }

    while (TRUE)
    {
        // 60Hz floor for BOTH modes (absolute-QPC schedule). Timer mode uses the scheduled
        // deadline. Vsync mode anchors the target to "now" (just after the floor wait) so selection
        // runs on the display clock — but the floor is also the runaway guard: if the INTERVAL_ONE
        // present does NOT block (e.g. a capture-card display with no real vblank), the floor still
        // caps the loop at the present rate instead of spinning and starving the source.
        m_scheduler.WaitUntilDeadline();
        LONGLONG deadline;
        if (m_vsyncPresent) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            deadline = now.QuadPart;
        } else {
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
        // Timer: immediate (non-blocking). Vsync: INTERVAL_ONE is meant to block until the
        // capture-card vblank. Time the call directly (afterPresent - beforePresent) so we can see
        // whether it actually blocks, independent of the loop floor above.
        HRESULT presentHr = device->PresentEx(NULL, NULL, NULL, NULL,
            m_vsyncPresent ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE);
        LARGE_INTEGER afterPresent;
        QueryPerformanceCounter(&afterPresent);

        if (m_vsyncPresent && diagCount < kDiagN) {
            D3DRASTER_STATUS rs = {};
            HRESULT rasterHr = device->GetRasterStatus(0, &rs);
            LOG("diag #%d present_block=%lldus presentHr=0x%08x raster_hr=0x%08x inVBlank=%d scanline=%u",
                diagCount,
                (long long)((afterPresent.QuadPart - beforePresent.QuadPart) * usPerTick),
                presentHr, rasterHr, (int)rs.InVBlank, rs.ScanLine);
            diagCount++;
        }

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

        m_scheduler.Advance();   // both modes advance the floor schedule (vsync now has the floor too)

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
