#include "CaptureRing.h"
#include <SimpleLogger.h>
#include <limits.h>

CaptureRing::CaptureRing()
    : m_captureTarget(NULL)
    , m_device(NULL)
    , m_nvfbc(NULL)
    , m_grabParams(NULL)
    , m_diffMap(NULL)
    , m_diffMapScanBytes(0)
    , m_width(0)
    , m_height(0)
    , m_freqQuad(0)
    , m_published(0)
    , m_staleSkips(0)
    , m_fatal(false)
    , m_lastArrival(0)
{
    for (int i = 0; i < RING_SIZE; i++) {
        m_ring[i].texture = NULL;
        m_ring[i].surface = NULL;
        m_ring[i].valid = false;
        m_ring[i].timestamp.QuadPart = 0;
    }
    m_diffMapArray[0] = NULL;
    m_baseQpc.QuadPart = 0;
}

CaptureRing::~CaptureRing() {
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
    if (m_diffMap) {
        VirtualFree(m_diffMap, 0, MEM_RELEASE);
        m_diffMap = NULL;
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

    // Diffmap buffer (must be VirtualAlloc'd per the NvFBC header). Only the first
    // ceil(w/128)*ceil(h/128) bytes are meaningful at the 128x128 block size.
    m_diffMap = VirtualAlloc(NULL, NVFBC_TODX9VID_MAX_DIFF_MAP_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!m_diffMap) {
        LOGERR("CaptureRing: VirtualAlloc for diffmap failed (error: %d)", GetLastError());
        return false;
    }
    m_diffMapArray[0] = m_diffMap;
    m_diffMapScanBytes = ((width + 127) / 128) * ((height + 127) / 128);

    LOG("CaptureRing initialized - %dx%d, %d slots, diffmap %d scan bytes (single-thread pump)",
        width, height, RING_SIZE, m_diffMapScanBytes);
    return true;
}

bool CaptureRing::Attach(NvFBCToDx9Vid* nvfbc, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams, LARGE_INTEGER baseQpc) {
    m_nvfbc = nvfbc;
    m_grabParams = grabParams;
    m_baseQpc = baseQpc;

    // Redirect NvFBC output into our capture target, with the diffmap enabled so stale
    // timeout re-grabs (no content change) can be detected and skipped.
    NVFBC_TODX9VID_OUT_BUF outBuf[1];
    outBuf[0].pPrimary = m_captureTarget;

    NVFBC_TODX9VID_SETUP_PARAMS setupParams = {};
    setupParams.dwVersion = NVFBC_TODX9VID_SETUP_PARAMS_V3_VER;
    setupParams.bWithHWCursor = 1;
    setupParams.bStereoGrab = 0;
    setupParams.bDiffMap = 1;
    setupParams.eDiffMapBlockSize = NVFBC_TODX9VID_DIFFMAP_BLOCKSIZE_128X128;
    setupParams.dwDiffMapBuffSize = NVFBC_TODX9VID_MAX_DIFF_MAP_SIZE;
    setupParams.ppDiffMap = m_diffMapArray;
    setupParams.ppBuffer = outBuf;
    setupParams.eMode = NVFBC_TODX9VID_ARGB10;
    setupParams.dwNumBuffers = 1;
    setupParams.bHDRRequest = TRUE;

    if (NVFBC_SUCCESS != nvfbc->NvFBCToDx9VidSetUp(&setupParams)) {
        LOGERR("CaptureRing: NvFBCToDx9VidSetUp with diffmap failed");
        return false;
    }

    grabParams->dwFlags = NVFBC_TODX9VID_WAIT_WITH_TIMEOUT;
    return true;
}

bool CaptureRing::DiffMapChanged() const {
    const unsigned char* p = (const unsigned char*)m_diffMap;
    for (int i = 0; i < m_diffMapScanBytes; i++) {
        if (p[i]) return true;
    }
    return false;
}

void CaptureRing::PumpUntil(LONGLONG stopQpc) {
    RECT srcRect = { 0, 0, (LONG)m_width, (LONG)m_height };
    const double usPerTick = 1000000.0 / (double)m_freqQuad;

    while (!m_fatal) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG remainTicks = stopQpc - now.QuadPart;
        long remainMs = (long)((remainTicks * 1000) / m_freqQuad);
        if (remainMs < 2) {
            break;  // too close to the stop time for a meaningful wait
        }

        // One blocking grab, capped at the remaining window. Early return = a frame event;
        // expiry = stale re-grab. Either way the diffmap decides whether content changed.
        m_grabParams->dwWaitTime = (NvU32)remainMs;
        NVFBCRESULT res = m_nvfbc->NvFBCToDx9VidGrabFrame(m_grabParams);

        if (res == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("CaptureRing: NvFBC session invalidated - stopping capture");
            m_fatal = true;
            break;
        }
        if (res != NVFBC_SUCCESS) {
            continue;  // unexpected non-fatal result; re-check the window and retry
        }

        if (!DiffMapChanged()) {
            // Stale re-grab (timeout expiry) or a no-content wake (e.g. HW cursor move).
            // Don't publish; loop — if the window expired, the time check exits.
            m_staleSkips++;
            continue;
        }

        LARGE_INTEGER arrival;
        QueryPerformanceCounter(&arrival);

        int slot = (int)(m_published % RING_SIZE);
        m_device->StretchRect(m_captureTarget, &srcRect, m_ring[slot].surface, &srcRect, D3DTEXF_NONE);
        m_ring[slot].timestamp = arrival;
        m_ring[slot].valid = true;
        m_published++;

        LONGLONG dt = (m_lastArrival != 0) ? (arrival.QuadPart - m_lastArrival) : 0;
        LOG("capture #%lld arr=%lldus dt=%lldus skip=%lld",
            m_published - 1,
            (long long)((arrival.QuadPart - m_baseQpc.QuadPart) * usPerTick),
            (long long)(dt * usPerTick),
            m_staleSkips);
        m_lastArrival = arrival.QuadPart;
    }
}

void CaptureRing::FindBracket(LONGLONG targetQpc, FrameBracket* out) const {
    *out = FrameBracket{};

    const long long p = m_published;
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
