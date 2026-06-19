#include "FrameTemporalCaptureMode.h"
#include <SimpleLogger.h>

// External global variables
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;

FrameTemporalCaptureMode::FrameTemporalCaptureMode(float framerate, IPresentTiming* timing)
    : m_timing(timing)
    , m_bracketingDelayQpc(0)
    , m_targetFramerate(framerate)
    , m_device(NULL)
{
    m_baseQpc.QuadPart = 0;
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

    LOG("Temporal mode initialized - %s present (%.2f fps nominal), nearest-frame + hysteresis",
        m_timing->Name(), m_targetFramerate);
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

    // Bind the present-timing strategy's resources (e.g. dlock's WaitForVBlank). Bail loudly on a
    // fatal setup failure rather than silently degrade.
    if (!m_timing->Setup(device, hwnd, &m_scheduler)) {
        LOGERR("Temporal: present-timing setup failed (%s) - aborting", m_timing->Name());
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

    // Startup capability probe: which present intervals the device advertises (one line).
    {
        D3DCAPS9 caps;
        if (SUCCEEDED(device->GetDeviceCaps(&caps))) {
            LOG("diag caps: PresentationIntervals=0x%08x ONE=%d TWO=%d THREE=%d FOUR=%d IMMEDIATE=%d",
                caps.PresentationIntervals,
                (caps.PresentationIntervals & D3DPRESENT_INTERVAL_ONE) != 0,
                (caps.PresentationIntervals & D3DPRESENT_INTERVAL_TWO) != 0,
                (caps.PresentationIntervals & D3DPRESENT_INTERVAL_THREE) != 0,
                (caps.PresentationIntervals & D3DPRESENT_INTERVAL_FOUR) != 0,
                (caps.PresentationIntervals & D3DPRESENT_INTERVAL_IMMEDIATE) != 0);
        }
    }

    // Instrumentation CHECKED every frame but LOGGED only on change/anomaly, so steady state adds
    // no log volume (keeps the logger off the critical path).
    int diagCount = 0;
    const int kDiagN = 30;
    HRESULT lastPresentHr = S_OK;
    HRESULT lastDeviceState = S_OK;

    while (TRUE)
    {
        // --- pace + target anchor (strategy) ---
        const LONGLONG deadline = m_timing->BeginFrame();
        const LONGLONG target = deadline - m_bracketingDelayQpc;

        // --- selection: nearest-to-target with HYSTERESIS (monotonic) ---
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
            chosen = bracket.afterSurface; pick = "after-adv"; lastShownTs = bracket.afterTs;
        } else if (beforeNew) {
            chosen = bracket.beforeSurface; pick = "before-adv"; lastShownTs = bracket.beforeTs;
        } else {
            chosen = bracket.hasBefore ? bracket.beforeSurface : (bracket.hasAfter ? bracket.afterSurface : NULL);
            pick = "repeat";
        }

        if (chosen) {
            m_device->StretchRect(chosen, &srcRect, g_backbuffer, &srcRect, D3DTEXF_NONE);
        }

        // --- gate immediately before the flip (strategy; lock modes wait for the card vblank) ---
        GateResult gate = m_timing->GateBeforePresent(device);
        if (!gate.ok) { LOGERR("present gate failed (%s) - stopping", m_timing->Name()); break; }

        // --- present ---
        LARGE_INTEGER beforePresent;
        QueryPerformanceCounter(&beforePresent);
        HRESULT presentHr = device->PresentEx(NULL, NULL, NULL, NULL, m_timing->PresentInterval());
        LARGE_INTEGER afterPresent;
        QueryPerformanceCounter(&afterPresent);

        // --- robust present-failure instrumentation (logs only on transition / fatal) ---
        if (presentHr != lastPresentHr) {
            if (presentHr != S_OK) LOGERR("PresentEx returned 0x%08x (was 0x%08x)", presentHr, lastPresentHr);
            else                   LOG("PresentEx recovered to S_OK");
            lastPresentHr = presentHr;
        }
        HRESULT devState = device->CheckDeviceState(hwnd);
        if (devState != lastDeviceState) {
            LOG("device state -> 0x%08x", devState);
            lastDeviceState = devState;
        }
        if (devState == D3DERR_DEVICEHUNG || devState == D3DERR_DEVICEREMOVED) {
            LOGERR("fatal device state 0x%08x - stopping", devState);
            break;
        }

        LONGLONG presentDelta = (lastPresentQpc != 0) ? (beforePresent.QuadPart - lastPresentQpc) : 0;
        lastPresentQpc = beforePresent.QuadPart;

        // --- per-present pacing line (~present-rate volume) ---
        if (bracket.hasBefore) {
            LOG("temporal dl=%lldus tgt=%lldus before=%lldus(d%d) after=%lldus w=%.3f pick=%s jit=%lldus pdt=%lldus",
                (long long)((deadline - m_baseQpc.QuadPart) * usPerTick),
                (long long)((target - m_baseQpc.QuadPart) * usPerTick),
                (long long)((bracket.beforeTs - m_baseQpc.QuadPart) * usPerTick), bracket.beforeDepth,
                bracket.hasAfter ? (long long)((bracket.afterTs - m_baseQpc.QuadPart) * usPerTick) : -1LL,
                bracket.weight, pick,
                (long long)((beforePresent.QuadPart - deadline) * usPerTick),
                (long long)(presentDelta * usPerTick));
        } else if (m_ring.Published() >= CaptureRing::RING_SIZE) {
            LOGERR("temporal: target older than ring window - ring too small / delay too large (p=%lld)",
                m_ring.Published());
        }

        // --- gate diagnostics: first kDiagN presents only ---
        if (diagCount < kDiagN) {
            D3DRASTER_STATUS rs = {};
            HRESULT rasterHr = device->GetRasterStatus(0, &rs);
            LOG("diag #%d gate_hit=%d present_block=%lldus presentHr=0x%08x raster_hr=0x%08x inVBlank=%d scanline=%u",
                diagCount, (int)gate.gateHit,
                (long long)((afterPresent.QuadPart - beforePresent.QuadPart) * usPerTick),
                presentHr, rasterHr, (int)rs.InVBlank, rs.ScanLine);
            diagCount++;
        }

        m_timing->EndFrame();

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
