#include "CaptureRing.h"
#include <SimpleLogger.h>
#include <limits.h>

// External globals (NvFBCR.cpp). Start() rebinds the NvFBC session to the capture device and
// must update the global so WinMain's Cleanup releases the right session.
extern IDirect3D9Ex* g_pD3DEx;
extern NvFBCLibrary* pNVFBCLib;
extern NvFBCToDx9Vid* NvFBCDX9;

// With a private capture device the blocking grab can wait as long as it likes — its lock
// holds affect nothing the present thread uses. The timeout only bounds how quickly the
// capture thread notices a stop request.
static const NvU32 kGrabWaitMs = 100;

CaptureRing::CaptureRing()
    : m_captureTarget(NULL)
    , m_presentDevice(NULL)
    , m_capDevice(NULL)
    , m_capSync(NULL)
    , m_nvfbc(NULL)
    , m_width(0)
    , m_height(0)
    , m_freqQuad(0)
    , m_published(0)
    , m_srcPeriodEmaQpc(0)
    , m_stop(true)   // not running until Start()
    , m_writeCount(0)
{
    for (int i = 0; i < RING_SIZE; i++) {
        m_ring[i].capTexture = NULL;
        m_ring[i].capSurface = NULL;
        m_ring[i].mainTexture = NULL;
        m_ring[i].mainSurface = NULL;
        m_ring[i].sharedHandle = NULL;
        m_ring[i].valid = false;
        m_ring[i].timestamp.QuadPart = 0;
    }
    m_baseQpc.QuadPart = 0;
}

CaptureRing::~CaptureRing() {
    Stop();

    for (int i = 0; i < RING_SIZE; i++) {
        if (m_ring[i].mainSurface) { m_ring[i].mainSurface->Release(); m_ring[i].mainSurface = NULL; }
        if (m_ring[i].mainTexture) { m_ring[i].mainTexture->Release(); m_ring[i].mainTexture = NULL; }
        if (m_ring[i].capSurface)  { m_ring[i].capSurface->Release();  m_ring[i].capSurface = NULL; }
        if (m_ring[i].capTexture)  { m_ring[i].capTexture->Release();  m_ring[i].capTexture = NULL; }
    }
    if (m_captureTarget) {
        m_captureTarget->Release();
        m_captureTarget = NULL;
    }
    if (m_capSync) {
        m_capSync->Release();
        m_capSync = NULL;
    }
    if (m_capDevice) {
        m_capDevice->Release();
        m_capDevice = NULL;
    }
}

bool CaptureRing::Setup(IDirect3DDevice9Ex* presentDevice, int width, int height) {
    m_presentDevice = presentDevice;
    m_width = width;
    m_height = height;

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    m_freqQuad = freq.QuadPart;
    return true;
}

bool CaptureRing::Start(NvFBCToDx9Vid* nvfbc, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
                        LARGE_INTEGER baseQpc, HWND hwnd) {
    m_baseQpc = baseQpc;

    // ---- Create the private capture device on the same adapter as the present device. ----
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    m_presentDevice->GetCreationParameters(&cp);

    D3DPRESENT_PARAMETERS d3dpp = {};
    d3dpp.Windowed = TRUE;
    d3dpp.BackBufferFormat = D3DFMT_A2R10G10B10;
    d3dpp.BackBufferWidth = m_width;
    d3dpp.BackBufferHeight = m_height;
    d3dpp.BackBufferCount = 1;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    d3dpp.hDeviceWindow = hwnd;

    HRESULT hr = g_pD3DEx->CreateDeviceEx(
        cp.AdapterOrdinal, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
        &d3dpp, NULL, &m_capDevice);
    if (FAILED(hr)) {
        LOGERR("CaptureRing: failed to create capture device (error: 0x%08x)", hr);
        return false;
    }

    // Event query on the capture device. After each StretchRect into a shared slot we issue
    // and drain this so the capture device's GPU write is COMPLETE before we publish the slot.
    // Without it the present device can read stale pixels from a freshly-written shared slot
    // (D3D9Ex shared surfaces have no implicit cross-device coherency) — observed as periodic
    // "old frame" episodes. The wait lands on the capture thread (off the present path), so it
    // only adds a near-constant pipeline offset, not present-side jitter.
    hr = m_capDevice->CreateQuery(D3DQUERYTYPE_EVENT, &m_capSync);
    if (FAILED(hr)) {
        LOGERR("CaptureRing: failed to create capture sync query (error: 0x%08x)", hr);
        return false;
    }

    // ---- Capture-side resources. ----
    hr = m_capDevice->CreateOffscreenPlainSurface(
        m_width, m_height, D3DFMT_A2B10G10R10, D3DPOOL_DEFAULT, &m_captureTarget, NULL);
    if (FAILED(hr)) {
        LOGERR("CaptureRing: failed to create capture target surface (error: 0x%08x)", hr);
        return false;
    }

    // Shared ring slots: create on the capture device with a shared handle, open the same
    // resource on the present device. Slots are render-target textures so consumers can
    // StretchRect the surface (temporal) or sample the texture in a shader (blend).
    for (int i = 0; i < RING_SIZE; i++) {
        HANDLE shared = NULL;
        hr = m_capDevice->CreateTexture(
            m_width, m_height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A2B10G10R10, D3DPOOL_DEFAULT,
            &m_ring[i].capTexture, &shared);
        if (FAILED(hr)) {
            LOGERR("CaptureRing: failed to create shared ring texture %d (error: 0x%08x)", i, hr);
            return false;
        }
        m_ring[i].sharedHandle = shared;   // retained; cross-API consumers open this
        hr = m_ring[i].capTexture->GetSurfaceLevel(0, &m_ring[i].capSurface);
        if (FAILED(hr)) {
            LOGERR("CaptureRing: failed to get capture-side ring surface %d (error: 0x%08x)", i, hr);
            return false;
        }
        hr = m_presentDevice->CreateTexture(
            m_width, m_height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A2B10G10R10, D3DPOOL_DEFAULT,
            &m_ring[i].mainTexture, &shared);  // open existing shared resource
        if (FAILED(hr)) {
            LOGERR("CaptureRing: failed to open shared ring texture %d on present device (error: 0x%08x)", i, hr);
            return false;
        }
        hr = m_ring[i].mainTexture->GetSurfaceLevel(0, &m_ring[i].mainSurface);
        if (FAILED(hr)) {
            LOGERR("CaptureRing: failed to get present-side ring surface %d (error: 0x%08x)", i, hr);
            return false;
        }
    }

    // ---- Rebind NvFBC to the capture device. ----
    // Release the session WinMain created against the present device, create a new one bound
    // to the capture device, and update the global so Cleanup releases the right session.
    nvfbc->NvFBCToDx9VidRelease();
    DWORD maxW = 0, maxH = 0;
    m_nvfbc = (NvFBCToDx9Vid*)pNVFBCLib->create(NVFBC_TO_DX9_VID, &maxW, &maxH, 0, (void*)m_capDevice);
    if (!m_nvfbc) {
        LOGERR("CaptureRing: failed to create NvFBC session on the capture device");
        return false;
    }
    NvFBCDX9 = m_nvfbc;

    NVFBC_TODX9VID_OUT_BUF outBuf[1];
    outBuf[0].pPrimary = m_captureTarget;

    NVFBC_TODX9VID_SETUP_PARAMS setupParams = {};
    setupParams.dwVersion = NVFBC_TODX9VID_SETUP_PARAMS_V3_VER;
    // bWithHWCursor = 0: do NOT composite the HW cursor. With it on, NvFBC also wakes the grab on
    // every cursor MOVE (mouse polling rate, 125-1000+Hz, decoupled from the monitor) — those
    // sub-2ms wakes seed content-duplicate frames into the ring and put false timestamps on the
    // content timeline (bad for bracketing/blend/NVOFA). Off => the grab wakes only on content
    // frames => clean ~source-rate capture with accurate timestamps. Cost: the OS cursor won't
    // appear in the captured/streamed image (fine for game capture where the game owns the cursor).
    setupParams.bWithHWCursor = 0;
    setupParams.bStereoGrab = 0;
    setupParams.bDiffMap = 0;
    setupParams.ppBuffer = outBuf;
    setupParams.eMode = NVFBC_TODX9VID_ARGB10;
    setupParams.dwNumBuffers = 1;
    setupParams.bHDRRequest = TRUE;

    if (NVFBC_SUCCESS != m_nvfbc->NvFBCToDx9VidSetUp(&setupParams)) {
        LOGERR("CaptureRing: NvFBCToDx9VidSetUp on capture device failed");
        return false;
    }

    // Fully event-driven blocking grab — safe now that the lock it holds is private.
    grabParams->dwFlags = NVFBC_TODX9VID_WAIT_WITH_TIMEOUT;
    grabParams->dwWaitTime = kGrabWaitMs;

    LOG("CaptureRing initialized - %dx%d, %d shared slots, private capture device", m_width, m_height, RING_SIZE);

    m_published.store(0);
    m_writeCount = 0;
    m_stop.store(false);
    m_captureThread = std::thread(&CaptureRing::CaptureLoop, this, grabParams);
    return true;
}

void CaptureRing::Stop() {
    m_stop.store(true);
    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }
}

void CaptureRing::CaptureLoop(NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams) {
    RECT srcRect = { 0, 0, (LONG)m_width, (LONG)m_height };
    const double usPerTick = 1000000.0 / (double)m_freqQuad;
    LONGLONG lastArrival = 0;
    long long collapsed = 0;

    // Batch-collapse threshold. Under NVIDIA Smooth Motion (driver-level frame gen; in-game
    // DLSS-FG untested) the grab wakes ~2x per BASE frame, members arriving <2 ms apart
    // (submission-batched; display flips are ~half a base period apart) — see spec Round 10.
    // Real content cadences never produce sub-3ms gaps (333+ fps base would be required; non-FG
    // 240 Hz gaps are 4.17 ms). Wake order measured [GENERATED, REAL] (discriminator, Round 10),
    // so the intra-batch member is the REAL frame: publish it stamped with the BATCH-START time
    // (base cadence), then RETRACT the previous slot (valid=false — the generated member).
    // Retraction never overwrites a published slot, so a concurrent present read is safe
    // (content untouched; the flag only hides it from future brackets — worst case one present
    // inside the ~ε window still shows the gen frame). Keep-real won the definitive A/B:
    // keep-first ghosted throughout, keep-real crisp throughout (Round 10).
    const LONGLONG batchThresholdQpc = (m_freqQuad * 3) / 1000;
    LONGLONG batchStartQpc = 0;
    LOG("CaptureRing: batch-collapse keep-real (intra-batch wake <3ms = real member; previous slot retracted)");

    while (!m_stop.load()) {
        NVFBCRESULT res = m_nvfbc->NvFBCToDx9VidGrabFrame(grabParams);

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

        const LONGLONG prevArrival = lastArrival;
        const bool intraBatch =
            (prevArrival != 0 && (now.QuadPart - prevArrival) < batchThresholdQpc);
        if (!intraBatch) {
            // Source-period estimate (batch-start to batch-start, so frame-gen epsilon gaps
            // never pollute it; >125 ms gaps are stalls/timeouts, not cadence). EMA alpha=1/8:
            // stable within ~8 source frames of a regime change, jitter-immune in steady state.
            if (batchStartQpc != 0) {
                const LONGLONG gap = now.QuadPart - batchStartQpc;
                if (gap < m_freqQuad / 8) {
                    long long ema = m_srcPeriodEmaQpc.load(std::memory_order_relaxed);
                    ema = ema ? (ema * 7 + gap) / 8 : gap;
                    m_srcPeriodEmaQpc.store(ema, std::memory_order_relaxed);
                }
            }
            batchStartQpc = now.QuadPart;
        }
        lastArrival = now.QuadPart;   // chain: a 3rd member ε after the 2nd is still intra-batch

        long long count = m_writeCount;
        int slot = (int)(count % RING_SIZE);
        m_capDevice->StretchRect(m_captureTarget, &srcRect, m_ring[slot].capSurface, &srcRect, D3DTEXF_NONE);

        // Force the StretchRect to complete on the capture GPU before publishing, so the
        // present device never reads a not-yet-coherent shared slot. D3DGETDATA_FLUSH kicks
        // the command buffer; GetData returns S_FALSE until the GPU signals the event.
        m_capSync->Issue(D3DISSUE_END);
        while (m_capSync->GetData(NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) {
            if (m_stop.load()) break;
        }

        // Measure how long the flush blocked: this is the fix's cost. It should be small and
        // CONSISTENT (a near-constant pipeline offset, off the present path). Spikes here would
        // mean the flush is stalling capture — watch flush p95 and whether dt grows.
        LARGE_INTEGER afterFlush;
        QueryPerformanceCounter(&afterFlush);
        LONGLONG flushUs = (afterFlush.QuadPart - now.QuadPart) * 1000000 / m_freqQuad;

        // The intra-batch (real) member is stamped with the BATCH-START time so the ring
        // timeline stays at base cadence; everything else is stamped at its own arrival.
        m_ring[slot].timestamp.QuadPart = intraBatch ? batchStartQpc : now.QuadPart;
        m_ring[slot].valid = true;

        m_writeCount = count + 1;
        m_published.store(count + 1);  // publish only after the slot write is GPU-complete

        if (intraBatch && count >= 1) {
            // Retract the previous member (the generated frame): hide it from future brackets.
            // Content is never overwritten, so a present read already in flight stays coherent.
            m_ring[(int)((count - 1) % RING_SIZE)].valid = false;
            collapsed++;
        }

        // Verbose: source arrival timeline (dt = inter-arrival gap ≈ source frame period);
        // flush = GPU-completion wait added by the cross-device coherency fix; col = cumulative
        // batch-collapsed wakes (skipped or retracted frame-gen members).
        LONGLONG dt = (prevArrival != 0) ? (now.QuadPart - prevArrival) : 0;
        LOG("capture #%lld arr=%lldus dt=%lldus flush=%lldus col=%lld",
            count,
            (long long)((now.QuadPart - m_baseQpc.QuadPart) * usPerTick),
            (long long)(dt * usPerTick),
            (long long)flushUs,
            collapsed);
    }
}

void CaptureRing::FindBracket(LONGLONG targetQpc, FrameBracket* out) const {
    *out = FrameBracket{};
    out->beforeSlot = -1;
    out->afterSlot = -1;

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
                out->beforeSurface = m_ring[slot].mainSurface;
                out->beforeTexture = m_ring[slot].mainTexture;
                out->beforeTs = ts;
                out->beforeDiff = diff;
                out->beforeDepth = (int)(p - 1 - i);
                out->beforeSlot = slot;
            }
        } else {
            if (-diff < bestAfterDiff) {
                bestAfterDiff = -diff;
                out->hasAfter = true;
                out->afterSurface = m_ring[slot].mainSurface;
                out->afterTexture = m_ring[slot].mainTexture;
                out->afterTs = ts;
                out->afterDiff = -diff;
                out->afterSlot = slot;
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
