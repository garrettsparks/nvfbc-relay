#include "D3D9Present.h"
#include <SimpleLogger.h>

D3D9PresentPath::D3D9PresentPath(IFrameCompositor* compositor)
    : m_compositor(compositor)
    , m_device(NULL)
    , m_hwnd(NULL)
    , m_mark(false)
    , m_backbuffer(NULL)
    , m_fallbackBackbuffer(NULL)
    , m_presentTarget(NULL)
    , m_presentFailures(0)
    , m_backbufferFailures(0)
    , m_presentStatsSwapChain(NULL)
    , m_lastSyncRefresh(0)
    , m_missedRefreshes(0)
    , m_statsSamples(0)
    , m_usPerTick(0.0)
{
    m_baseQpc.QuadPart = 0;
}

D3D9PresentPath::~D3D9PresentPath() {
    if (m_backbuffer) m_backbuffer->Release();
    if (m_presentStatsSwapChain) m_presentStatsSwapChain->Release();
    delete m_compositor;
}

bool D3D9PresentPath::Setup(const RelayContext& ctx, CaptureRing* ring,
                            const policy::PolicyConfig* /*cfg*/, bool mark,
                            unsigned int markFrames, LARGE_INTEGER baseQpc, LONGLONG freqQpc) {
    IDirect3DDevice9Ex* device = ctx.presentDevice;
    const int width = ctx.width;
    const int height = ctx.height;
    m_device = device;
    m_hwnd = ctx.outputWindow;
    m_fallbackBackbuffer = ctx.backBuffer;
    m_baseQpc = baseQpc;
    m_usPerTick = 1000000.0 / (double)freqQpc;

    if (!m_compositor->Setup(device, width, height)) {
        LOGERR("Compositor setup failed - refusing the mode");
        return false;
    }
    // Marker resources live on the PRESENT device (the burn is a backbuffer overlay, never
    // a ring-surface write). A failed Init disables the marker, not the relay: Burn keeps
    // counting so mark= stays a continuous present count.
    m_mark = mark;
    if (m_mark) {
        m_marker.Init(device, width, height, markFrames);
    }
    // Capture-side resources that exist only now (ring slot shared handles): the interp
    // sidecar's aliases. A compositor that cannot finish initializing refuses the mode
    // instead of silently running a different one.
    if (!m_compositor->OnCaptureStarted(ring, baseQpc, freqQpc)) {
        LOGERR("Compositor capture-side init failed - refusing the mode");
        return false;
    }

    // Present statistics live on the swapchain, not the device, and are only meaningful
    // under flip mode. Acquired once: the swapchain object is stable even though its buffers
    // rotate.
    if (ctx.flipEx) {
        IDirect3DSwapChain9* sc = NULL;
        if (SUCCEEDED(device->GetSwapChain(0, &sc)) && sc) {
            if (FAILED(sc->QueryInterface(__uuidof(IDirect3DSwapChain9Ex),
                                          (void**)&m_presentStatsSwapChain))) {
                m_presentStatsSwapChain = NULL;
            }
            sc->Release();
        }
        LOG("Flip-mode presentation ACTIVE (-flipex): FLIPEX swap effect, back buffer acquired "
            "per present; present statistics %s",
            m_presentStatsSwapChain ? "available (presentstats: lines follow)" : "UNAVAILABLE");
    }
    return true;
}

void D3D9PresentPath::Compose(const FrameBracket& bracket, CompositeOutcome* out) {
    // The back buffer is acquired PER PRESENT, never cached. Under D3DSWAPEFFECT_DISCARD
    // back buffer 0 is the same surface every time and this is a no-op, but under FLIPEX
    // the runtime rotates which handle is the back buffer at presentation time, so a cached
    // pointer composites into a surface that is no longer the one being presented - the
    // "every third frame is blank" that made the earlier FLIPEX attempt fail. Falls back to
    // the shell's cached back buffer if the call fails, so a failure degrades rather than
    // presenting whatever the flip queue left behind.
    if (m_backbuffer) {
        m_backbuffer->Release();
        m_backbuffer = NULL;
    }
    if (FAILED(m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_backbuffer)) ||
        !m_backbuffer) {
        m_backbuffer = NULL;
        m_backbufferFailures++;
        if (m_backbufferFailures == 1 || (m_backbufferFailures % 600) == 0) {
            LOGERR("GetBackBuffer failed (%lld so far); falling back to the cached surface",
                   m_backbufferFailures);
        }
    }
    m_presentTarget = m_backbuffer ? m_backbuffer : m_fallbackBackbuffer;

    m_compositor->Compose(bracket, m_presentTarget, out);
}

long long D3D9PresentPath::BurnMarker(const CompositeOutcome& out) {
    // Burn the marker over the composed backbuffer, once per present (repeats included: the
    // counter identifies presented frames, not source frames). Before the present stamp, so
    // jit/pdt absorb its cost and a -mark on/off A/B measures it.
    if (!m_mark) return -1;
    return (long long)m_marker.Burn(m_presentTarget, out.pickCode, out.weightQ,
                                    out.synthesized, m_compositor->Id(), out.pixelExec);
}

LONGLONG D3D9PresentPath::Present(bool /*vsync*/) {
    // The presentation interval is fixed at device creation (GetPresentationInterval), so
    // the vsync flag is decided there: under INTERVAL_ONE this present blocks until DWM's
    // next compose (source clock on a composed desktop; card clock under a fullscreen game)
    // and IS the frame-pacing wait; under IMMEDIATE it returns at once.
    //
    // dwFlags is 0 and must stay 0. It is NOT a presentation interval: the only legal
    // values are D3DPRESENT_DONOTWAIT and D3DPRESENT_LINEAR_CONTENT, and the interval is
    // fixed at device creation. This argument used to receive the interval constants, which
    // meant vsync mode silently requested DONOTWAIT (numerically identical to INTERVAL_ONE,
    // both 1) and timer mode passed an undefined bit. Under DONOTWAIT a present that would
    // wait returns D3DERR_WASSTILLDRAWING WITHOUT PRESENTING, which is invisible in the log
    // (the present was counted) and shows downstream as the previous frame repeating - the
    // exact judder signature this relay is measured against.
    LARGE_INTEGER beforePresent, afterPresent;
    QueryPerformanceCounter(&beforePresent);
    const HRESULT presentHr = m_device->PresentEx(NULL, NULL, NULL, NULL, 0);
    QueryPerformanceCounter(&afterPresent);

    // GetBackBuffer AddRefs; release after the present so the runtime can rotate it.
    if (m_backbuffer) {
        m_backbuffer->Release();
        m_backbuffer = NULL;
    }
    if (FAILED(presentHr) || presentHr == S_PRESENT_MODE_CHANGED ||
        presentHr == S_PRESENT_OCCLUDED) {
        // Never silent: a present that did not reach the screen must be attributable, or a
        // video-vs-log disagreement has no explanation in the log.
        m_presentFailures++;
        if (m_presentFailures == 1 || (m_presentFailures % 600) == 0) {
            // CheckDeviceState separates the two readings of a non-OK present status that
            // this relay cannot otherwise tell apart: an ADVISORY one (the device is fine,
            // the runtime is just noting a conversion - which is the likely reading on a
            // multi-monitor desktop whose displays run different modes), versus a device
            // that genuinely wants recreating. S_OK here means the status is advisory and
            // no Reset is owed; anything else means the swapchain is in a state that a
            // Reset is supposed to clear, and ignoring it is a real bug rather than noise.
            const HRESULT devState = m_device->CheckDeviceState(m_hwnd);
            LOGERR("present returned 0x%08lx (%lld so far); CheckDeviceState 0x%08lx "
                   "(S_OK means advisory, no Reset owed)",
                   (unsigned long)presentHr, m_presentFailures, (unsigned long)devState);
        }
    }
    SamplePresentStats();
    return afterPresent.QuadPart - beforePresent.QuadPart;
}

void D3D9PresentPath::SamplePresentStats() {
    // PRESENT STATISTICS: what the SINK actually did with our frames, from the runtime
    // rather than inferred from ETW. Only a flip-mode swapchain reports these in windowed
    // mode - a bitblt one returns zeroes - so this is the half of -flipex that pays off
    // whether or not DWM ever promotes the window to independent flip.
    //
    // PresentRefreshCount equals SyncRefreshCount when every present landed on its own
    // vsync; when the former runs ahead, a refresh went by showing the previous frame,
    // which is a DOWNSTREAM DUPE measured in-process. That is the same quantity a marked
    // video plus a marker decode plus content-step analysis produces offline.
    if (!m_presentStatsSwapChain) return;
    D3DPRESENTSTATS ps;
    ZeroMemory(&ps, sizeof(ps));
    if (FAILED(m_presentStatsSwapChain->GetPresentStats(&ps))) return;
    if (m_lastSyncRefresh != 0 && ps.SyncRefreshCount > m_lastSyncRefresh) {
        const UINT elapsed = ps.SyncRefreshCount - m_lastSyncRefresh;
        // More than one sink refresh since the last present means refreshes that showed
        // no new frame of ours.
        if (elapsed > 1) m_missedRefreshes += (elapsed - 1);
    }
    m_lastSyncRefresh = ps.SyncRefreshCount;
    m_statsSamples++;
    if ((m_statsSamples % 1800) == 0) {
        LOG("presentstats: present=%u presentRefresh=%u syncRefresh=%u "
            "syncQpc=%lldus missedRefreshes=%lld over %lld presents",
            ps.PresentCount, ps.PresentRefreshCount, ps.SyncRefreshCount,
            (long long)((ps.SyncQPCTime.QuadPart - m_baseQpc.QuadPart) * m_usPerTick),
            m_missedRefreshes, m_statsSamples);
    }
}

void D3D9PresentPath::LogSummary() const {
    if (m_presentStatsSwapChain) {
        // The whole-run figure, so a capture carries its downstream dupe count without a
        // video: refreshes that showed no new frame of ours, over presents that reported.
        LOG("presentstats summary: %lld refreshes showed no new frame over %lld presents "
            "(%.3f/s at 60 Hz sink)", m_missedRefreshes, m_statsSamples,
            m_statsSamples > 0 ? (double)m_missedRefreshes * 60.0 / (double)m_statsSamples : 0.0);
    }
    if (m_backbufferFailures > 0) {
        LOGERR("GetBackBuffer failed %lld times over the run", m_backbufferFailures);
    }
    if (m_presentFailures > 0) {
        LOGERR("present reported a non-OK status %lld times over the run", m_presentFailures);
    }
}
