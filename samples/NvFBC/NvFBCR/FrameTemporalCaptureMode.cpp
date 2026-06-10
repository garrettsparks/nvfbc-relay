#include "FrameTemporalCaptureMode.h"
#include <SimpleLogger.h>
#include <limits.h>

// External global variables
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;

// How long the blocking grab waits before returning empty-handed. Only governs how quickly the
// capture thread notices a stop request / stall — not pacing (pacing is the source itself).
static const NvU32 kGrabWaitMs = 100;

FrameTemporalCaptureMode::FrameTemporalCaptureMode(float framerate)
    : m_captureTarget(NULL)
    , m_device(NULL)
    , m_targetFramerate(framerate)
    , m_bracketingDelayQpc(0)
    , m_published(0)
    , m_stop(false)
    , m_writeCount(0)
{
    for (int i = 0; i < RING_SIZE; i++) {
        m_ring[i].surface = NULL;
        m_ring[i].valid = false;
        m_ring[i].timestamp.QuadPart = 0;
    }
    m_baseQpc.QuadPart = 0;
}

FrameTemporalCaptureMode::~FrameTemporalCaptureMode() {
    // Backstop: ensure the capture thread is stopped before releasing its resources.
    m_stop.store(true);
    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }

    for (int i = 0; i < RING_SIZE; i++) {
        if (m_ring[i].surface) {
            m_ring[i].surface->Release();
            m_ring[i].surface = NULL;
        }
    }
    if (m_captureTarget) {
        m_captureTarget->Release();
        m_captureTarget = NULL;
    }
}

UINT FrameTemporalCaptureMode::GetPresentationInterval() const {
    return D3DPRESENT_INTERVAL_IMMEDIATE;
}

bool FrameTemporalCaptureMode::Setup() {
    m_device = g_pD3D9Device;

    // Capture target surface (where NvFBC writes each grab).
    HRESULT hr = m_device->CreateOffscreenPlainSurface(
        BUF_WIDTH, BUF_HEIGHT, D3DFMT_A2B10G10R10, D3DPOOL_DEFAULT, &m_captureTarget, NULL);
    if (FAILED(hr)) {
        LOGERR("Failed to create capture target surface (error: 0x%08x)", hr);
        return false;
    }

    // Ring slots.
    for (int i = 0; i < RING_SIZE; i++) {
        hr = m_device->CreateOffscreenPlainSurface(
            BUF_WIDTH, BUF_HEIGHT, D3DFMT_A2B10G10R10, D3DPOOL_DEFAULT, &m_ring[i].surface, NULL);
        if (FAILED(hr)) {
            LOGERR("Failed to create ring surface %d (error: 0x%08x)", i, hr);
            return false;
        }
    }

    if (!m_scheduler.Setup(m_targetFramerate)) {
        return false;
    }
    // Lag the present target by one present period so the ring reliably holds a frame on each
    // side of it (with source rate >= present rate, a frame newer than the target has arrived).
    m_bracketingDelayQpc = m_scheduler.PeriodQpc();

    LOG("Temporal mode initialized - present %.2f fps, ring %d, blocking-grab capture",
        m_targetFramerate, RING_SIZE);
    return true;
}

void FrameTemporalCaptureMode::CaptureLoop(
    NvFBCToDx9Vid* nvfbcDx9, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams)
{
    RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };
    const double usPerTick = 1000000.0 / (double)m_scheduler.Freq();
    LONGLONG lastArrival = 0;

    while (!m_stop.load()) {
        // Blocking grab: returns when a new source frame is available (or after kGrabWaitMs).
        NVFBCRESULT res = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);

        if (res == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("NvFBC session invalidated (capture thread) - stopping");
            m_stop.store(true);
            break;
        }
        if (res != NVFBC_SUCCESS) {
            // Timed out with no new frame (e.g. static source) — loop and re-check stop.
            continue;
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        long long count = m_writeCount;
        int slot = (int)(count % RING_SIZE);
        m_device->StretchRect(m_captureTarget, &srcRect, m_ring[slot].surface, &srcRect, D3DTEXF_NONE);
        m_ring[slot].timestamp = now;
        m_ring[slot].valid = true;

        m_writeCount = count + 1;
        m_published.store(count + 1);  // publish only after the slot is fully written

        // Verbose: the source arrival timeline (dt = inter-arrival gap ≈ source frame period).
        LONGLONG dt = (lastArrival != 0) ? (now.QuadPart - lastArrival) : 0;
        LOG("capture #%lld arr=%lldus dt=%lldus",
            count,
            (long long)((now.QuadPart - m_baseQpc.QuadPart) * usPerTick),
            (long long)(dt * usPerTick));
        lastArrival = now.QuadPart;
    }
}

void FrameTemporalCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    // Redirect NvFBC capture into our intermediate surface (instead of the window backbuffer).
    NVFBC_TODX9VID_OUT_BUF outBuf[1];
    outBuf[0].pPrimary = m_captureTarget;

    NVFBC_TODX9VID_SETUP_PARAMS setupParams = {};
    setupParams.dwVersion = NVFBC_TODX9VID_SETUP_PARAMS_V3_VER;
    setupParams.bWithHWCursor = 1;
    setupParams.bStereoGrab = 0;
    setupParams.bDiffMap = 0;
    setupParams.ppBuffer = outBuf;
    setupParams.eMode = NVFBC_TODX9VID_ARGB10;
    setupParams.dwNumBuffers = 1;
    setupParams.bHDRRequest = TRUE;

    if (NVFBC_SUCCESS != nvfbcDx9->NvFBCToDx9VidSetUp(&setupParams)) {
        LOGERR("Failed to reconfigure NvFBC for temporal mode");
        return;
    }

    // Switch the grab to blocking-with-timeout so the capture thread wakes per source frame.
    grabParams->dwFlags = NVFBC_TODX9VID_WAIT_WITH_TIMEOUT;
    grabParams->dwWaitTime = kGrabWaitMs;

    QueryPerformanceCounter(&m_baseQpc);
    const double usPerTick = 1000000.0 / (double)m_scheduler.Freq();

    // Start capture, then run the present loop on this (main) thread.
    m_stop.store(false);
    m_published.store(0);
    m_writeCount = 0;
    m_captureThread = std::thread(&FrameTemporalCaptureMode::CaptureLoop, this, nvfbcDx9, grabParams);

    MSG msg = {};
    RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };
    LONGLONG lastPresentQpc = 0;
    m_scheduler.Seed();

    while (TRUE)
    {
        m_scheduler.WaitUntilDeadline();
        const LONGLONG deadline = m_scheduler.Deadline();
        const LONGLONG target = deadline - m_bracketingDelayQpc;

        // Find the ring frames bracketing `target`. Read only the published frames excluding the
        // slot capture is currently writing: indices [p - (RING_SIZE-1), p-1].
        const long long p = m_published.load();
        long long oldest = p - (RING_SIZE - 1);
        if (oldest < 0) oldest = 0;

        int beforeSlot = -1, afterSlot = -1, beforeDepth = -1;
        LONGLONG beforeTs = 0, afterTs = 0;
        LONGLONG bestBeforeDiff = LLONG_MAX, bestAfterDiff = LLONG_MAX;
        for (long long i = p - 1; i >= oldest; i--) {
            int slot = (int)(i % RING_SIZE);
            if (!m_ring[slot].valid) continue;
            LONGLONG ts = m_ring[slot].timestamp.QuadPart;
            LONGLONG diff = target - ts;
            if (diff >= 0) {
                if (diff < bestBeforeDiff) { bestBeforeDiff = diff; beforeSlot = slot; beforeTs = ts; beforeDepth = (int)(p - 1 - i); }
            } else {
                if (-diff < bestAfterDiff) { bestAfterDiff = -diff; afterSlot = slot; afterTs = ts; }
            }
        }

        // Pick the nearer of the bracketing pair (genuine nearest, not always-before).
        int chosenSlot = -1;
        const char* pick = "none";
        if (beforeSlot >= 0 && afterSlot >= 0) {
            if (bestBeforeDiff <= bestAfterDiff) { chosenSlot = beforeSlot; pick = "before"; }
            else { chosenSlot = afterSlot; pick = "after"; }
        } else if (beforeSlot >= 0) {
            chosenSlot = beforeSlot; pick = "before-only";
        } else if (afterSlot >= 0) {
            chosenSlot = afterSlot; pick = "after-only";
        }

        if (chosenSlot >= 0) {
            m_device->StretchRect(m_ring[chosenSlot].surface, &srcRect, g_backbuffer, &srcRect, D3DTEXF_NONE);
        }

        LARGE_INTEGER beforePresent;
        QueryPerformanceCounter(&beforePresent);
        device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);

        // Inter-present interval (should hold steady at the present period if the scheduler works).
        LONGLONG presentDelta = (lastPresentQpc != 0) ? (beforePresent.QuadPart - lastPresentQpc) : 0;
        lastPresentQpc = beforePresent.QuadPart;

        // Logging: bracket timestamps double as the source timeline; weight is what blend would
        // use; jit is actual-present vs scheduled deadline; pdt is the actual inter-present gap.
        if (beforeSlot < 0) {
            // No frame at/older than the target. Benign while the ring is still filling at
            // startup; once it has wrapped at least once it means the target fell off the back
            // — the ring is too small or the bracketing delay too large.
            if (p >= RING_SIZE) {
                LOGERR("temporal: target older than ring window - ring too small / delay too large (p=%lld)", p);
            }
        } else {
            double weight = (afterSlot >= 0)
                ? (double)bestBeforeDiff / (double)(bestBeforeDiff + bestAfterDiff)
                : 1.0;  // no after-frame => source slower than present; presenting newest-before
            LOG("temporal dl=%lldus tgt=%lldus before=%lldus(d%d) after=%lldus w=%.3f pick=%s jit=%lldus pdt=%lldus",
                (long long)((deadline - m_baseQpc.QuadPart) * usPerTick),
                (long long)((target - m_baseQpc.QuadPart) * usPerTick),
                (long long)((beforeTs - m_baseQpc.QuadPart) * usPerTick), beforeDepth,
                (afterSlot >= 0) ? (long long)((afterTs - m_baseQpc.QuadPart) * usPerTick) : -1LL,
                weight, pick,
                (long long)((beforePresent.QuadPart - deadline) * usPerTick),
                (long long)(presentDelta * usPerTick));
            if (afterSlot < 0) {
                LOG("temporal: no after-frame (source slower than present?) - repeating newest");
            }
        }

        m_scheduler.Advance();

        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (msg.message == WM_QUIT) break;
        if (m_stop.load()) break;  // capture thread reported a fatal error
    }

    // Teardown: stop and join the capture thread.
    m_stop.store(true);
    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }
}

const char* FrameTemporalCaptureMode::GetModeName() const {
    return "Temporal";
}
