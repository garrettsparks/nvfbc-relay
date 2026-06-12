#include "CaptureRing.h"
#include <SimpleLogger.h>
#include <limits.h>

// How long one blocking grab call may wait before returning empty-handed. This must be SHORT:
// the grab holds the D3D9 device lock (D3DCREATE_MULTITHREADED) for its entire wait, stalling
// the present thread's StretchRect/Present until it returns — measured as present jitter of
// exactly half the capture period (2.3ms @ 240Hz, 5.2ms @ 100Hz, 8.4ms @ 60Hz) with presents
// quantized to capture arrivals. A short timeout bounds each lock hold; the grab still returns
// immediately when a frame arrives, so arrival timestamps are unaffected.
static const NvU32 kGrabWaitMs = 2;

CaptureRing::CaptureRing()
    : m_captureTarget(NULL)
    , m_device(NULL)
    , m_width(0)
    , m_height(0)
    , m_freqQuad(0)
    , m_published(0)
    , m_stop(true)   // not running until Start()
    , m_writeCount(0)
{
    for (int i = 0; i < RING_SIZE; i++) {
        m_ring[i].texture = NULL;
        m_ring[i].surface = NULL;
        m_ring[i].valid = false;
        m_ring[i].timestamp.QuadPart = 0;
    }
    m_baseQpc.QuadPart = 0;
}

CaptureRing::~CaptureRing() {
    Stop();

    for (int i = 0; i < RING_SIZE; i++) {
        if (m_ring[i].surface) {
            m_ring[i].surface->Release();
            m_ring[i].surface = NULL;
        }
        if (m_ring[i].texture) {
            m_ring[i].texture->Release();
            m_ring[i].texture = NULL;
        }
    }
    if (m_captureTarget) {
        m_captureTarget->Release();
        m_captureTarget = NULL;
    }
}

bool CaptureRing::Setup(IDirect3DDevice9Ex* device, int width, int height) {
    m_device = device;
    m_width = width;
    m_height = height;

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    m_freqQuad = freq.QuadPart;

    // Capture target surface (NvFBC writes each grab here).
    HRESULT hr = m_device->CreateOffscreenPlainSurface(
        width, height, D3DFMT_A2B10G10R10, D3DPOOL_DEFAULT, &m_captureTarget, NULL);
    if (FAILED(hr)) {
        LOGERR("CaptureRing: failed to create capture target surface (error: 0x%08x)", hr);
        return false;
    }

    // Ring slots: render-target textures so consumers can StretchRect the surface (temporal)
    // or sample the texture in a shader (blend).
    for (int i = 0; i < RING_SIZE; i++) {
        hr = m_device->CreateTexture(
            width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A2B10G10R10, D3DPOOL_DEFAULT,
            &m_ring[i].texture, NULL);
        if (FAILED(hr)) {
            LOGERR("CaptureRing: failed to create ring texture %d (error: 0x%08x)", i, hr);
            return false;
        }
        hr = m_ring[i].texture->GetSurfaceLevel(0, &m_ring[i].surface);
        if (FAILED(hr)) {
            LOGERR("CaptureRing: failed to get ring surface %d (error: 0x%08x)", i, hr);
            return false;
        }
    }

    LOG("CaptureRing initialized - %dx%d, %d slots", width, height, RING_SIZE);
    return true;
}

bool CaptureRing::Start(NvFBCToDx9Vid* nvfbc, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams, LARGE_INTEGER baseQpc) {
    m_baseQpc = baseQpc;

    // Redirect NvFBC output into our capture target (instead of the window backbuffer).
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

    if (NVFBC_SUCCESS != nvfbc->NvFBCToDx9VidSetUp(&setupParams)) {
        LOGERR("CaptureRing: failed to reconfigure NvFBC output");
        return false;
    }

    // Blocking grab: wake once per real source frame (or after kGrabWaitMs to re-check stop).
    grabParams->dwFlags = NVFBC_TODX9VID_WAIT_WITH_TIMEOUT;
    grabParams->dwWaitTime = kGrabWaitMs;

    m_published.store(0);
    m_writeCount = 0;
    m_stop.store(false);
    m_captureThread = std::thread(&CaptureRing::CaptureLoop, this, nvfbc, grabParams);
    return true;
}

void CaptureRing::Stop() {
    m_stop.store(true);
    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }
}

void CaptureRing::CaptureLoop(NvFBCToDx9Vid* nvfbc, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams) {
    RECT srcRect = { 0, 0, (LONG)m_width, (LONG)m_height };
    const double usPerTick = 1000000.0 / (double)m_freqQuad;
    LONGLONG lastArrival = 0;

    while (!m_stop.load()) {
        NVFBCRESULT res = nvfbc->NvFBCToDx9VidGrabFrame(grabParams);

        if (res == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("CaptureRing: NvFBC session invalidated - stopping capture");
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

void CaptureRing::FindBracket(LONGLONG targetQpc, FrameBracket* out) const {
    *out = FrameBracket{};

    const long long p = m_published.load();
    long long oldest = p - (RING_SIZE - 1);
    if (oldest < 0) oldest = 0;

    LONGLONG bestBeforeDiff = LLONG_MAX, bestAfterDiff = LLONG_MAX;
    for (long long i = p - 1; i >= oldest; i--) {
        int slot = (int)(i % RING_SIZE);
        if (!m_ring[slot].valid) continue;
        LONGLONG ts = m_ring[slot].timestamp.QuadPart;
        LONGLONG diff = targetQpc - ts;
        if (diff >= 0) {
            if (diff < bestBeforeDiff) {
                bestBeforeDiff = diff;
                out->hasBefore = true;
                out->beforeSurface = m_ring[slot].surface;
                out->beforeTexture = m_ring[slot].texture;
                out->beforeTs = ts;
                out->beforeDiff = diff;
                out->beforeDepth = (int)(p - 1 - i);
            }
        } else {
            if (-diff < bestAfterDiff) {
                bestAfterDiff = -diff;
                out->hasAfter = true;
                out->afterSurface = m_ring[slot].surface;
                out->afterTexture = m_ring[slot].texture;
                out->afterTs = ts;
                out->afterDiff = -diff;
            }
        }
    }

    if (out->hasBefore && out->hasAfter) {
        out->weight = (double)out->beforeDiff / (double)(out->beforeDiff + out->afterDiff);
    } else if (out->hasBefore) {
        out->weight = 1.0;  // target is past the newest frame; nothing to interpolate toward
    } else {
        out->weight = 0.0;
    }
}
