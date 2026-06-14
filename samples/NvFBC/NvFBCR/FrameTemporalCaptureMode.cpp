#include "FrameTemporalCaptureMode.h"
#include <SimpleLogger.h>

// External global variables
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;

FrameTemporalCaptureMode::FrameTemporalCaptureMode(float framerate)
    : m_bracketingDelayQpc(0)
    , m_targetFramerate(framerate)
    , m_device(NULL)
{
    m_baseQpc.QuadPart = 0;
}

UINT FrameTemporalCaptureMode::GetPresentationInterval() const {
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

    LOG("Temporal mode initialized - present %.2f fps, nearest-frame selection", m_targetFramerate);
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
    m_scheduler.Seed();

    while (TRUE)
    {
        m_scheduler.WaitUntilDeadline();
        const LONGLONG deadline = m_scheduler.Deadline();
        const LONGLONG target = deadline - m_bracketingDelayQpc;

        // Select: the ring frame nearest the target (before or after).
        FrameBracket bracket;
        m_ring.FindBracket(target, &bracket);

        // Always-future selection (experiment): pick the first frame at/after the target,
        // rather than the nearest of the bracket. A stable monotonic policy avoids the
        // boundary wobble that, with nearest-pick, holds the same frame for ~6 presents at
        // each drift slip (~81 dupes vs the ~12 fundamental rate-matching floor). Falls back
        // to the newest before-frame only when no future frame exists (source slower than
        // present). Costs up to one extra frame of latency vs nearest — acceptable.
        IDirect3DSurface9* chosen = NULL;
        const char* pick = "none";
        if (bracket.hasAfter) {
            chosen = bracket.afterSurface; pick = "after";
        } else if (bracket.hasBefore) {
            chosen = bracket.beforeSurface; pick = "before-only";
        }

        if (chosen) {
            m_device->StretchRect(chosen, &srcRect, g_backbuffer, &srcRect, D3DTEXF_NONE);
        }

        LARGE_INTEGER beforePresent;
        QueryPerformanceCounter(&beforePresent);
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

        m_scheduler.Advance();

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
