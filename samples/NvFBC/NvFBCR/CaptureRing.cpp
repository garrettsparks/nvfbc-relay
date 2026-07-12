#include "CaptureRing.h"
#include <SimpleLogger.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

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
    , m_dumpAtSeconds(0)
    , m_dumpStartQpc(0)
    , m_dumpCount(0)
    , m_dumpDrained(false)
    , m_probeRequested(false)
    , m_probeActive(false)
    , m_classMapActive(false)
    , m_diffMap(NULL)
    , m_classMap(NULL)
    , m_diffBlocks(0)
    , m_classStamps(0)
{
    for (int i = 0; i < RING_SIZE; i++) {
        m_ring[i].capTexture = NULL;
        m_ring[i].capSurface = NULL;
        m_ring[i].mainTexture = NULL;
        m_ring[i].mainSurface = NULL;
        m_ring[i].valid = false;
        m_ring[i].timestamp.QuadPart = 0;
    }
    for (int i = 0; i < DUMP_FRAMES; i++) m_dumpStaging[i] = NULL;
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
    // Safe to free after Stop(): the driver only writes the probe maps during grab calls,
    // and the grab thread is joined.
    if (m_diffMap)  { VirtualFree(m_diffMap, 0, MEM_RELEASE);  m_diffMap = NULL; }
    if (m_classMap) { VirtualFree(m_classMap, 0, MEM_RELEASE); m_classMap = NULL; }
    for (int i = 0; i < DUMP_FRAMES; i++) {
        if (m_dumpStaging[i]) { m_dumpStaging[i]->Release(); m_dumpStaging[i] = NULL; }
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

    // CONTENT PROBE (-probe): ask the driver for a per-grab diffmap (changed 32x32 blocks vs
    // the previous grab) and a high-frequency-content classification map. Both are written
    // into client memory alongside the grab; the capture loop reduces them to two numbers on
    // the capture log line. Driver support varies by feature, so degrade one rung at a time:
    // full probe -> diffmap only -> no probe (loud, so a silent-zero column is impossible).
    void* diffMaps[1] = { NULL };
    void* classMaps[1] = { NULL };
    if (m_probeRequested) {
        m_diffBlocks  = ((m_width + 31) / 32) * ((m_height + 31) / 32);
        m_classStamps = ((m_width + 15) / 16) * ((m_height + 15) / 16);
        m_diffMap  = VirtualAlloc(NULL, NVFBC_TODX9VID_MAX_DIFF_MAP_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        m_classMap = VirtualAlloc(NULL, NVFBC_TODX9VID_MAX_CLASSIFICATION_MAP_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (m_diffMap && m_classMap) {
            diffMaps[0] = m_diffMap;
            classMaps[0] = m_classMap;
            setupParams.bDiffMap = 1;
            setupParams.eDiffMapBlockSize = NVFBC_TODX9VID_DIFFMAP_BLOCKSIZE_32X32;
            setupParams.dwDiffMapBuffSize = NVFBC_TODX9VID_MAX_DIFF_MAP_SIZE;
            setupParams.ppDiffMap = diffMaps;
            setupParams.bClassificationMap = 1;
            setupParams.dwClassificationMapBuffSize = NVFBC_TODX9VID_MAX_CLASSIFICATION_MAP_SIZE;
            setupParams.dwClassificationMapStampWidth = 16;
            setupParams.dwClassificationMapStampHeight = 16;
            setupParams.ppClassificationMap = classMaps;
            m_probeActive = true;
            m_classMapActive = true;
        } else {
            LOGERR("CaptureRing: probe buffer allocation failed - content probe disabled");
        }
    }

    if (m_probeActive && NVFBC_SUCCESS != m_nvfbc->NvFBCToDx9VidSetUp(&setupParams)) {
        LOGERR("CaptureRing: probe setup refused with classification map - retrying diffmap-only");
        setupParams.bClassificationMap = 0;
        setupParams.dwClassificationMapBuffSize = 0;
        setupParams.dwClassificationMapStampWidth = 0;
        setupParams.dwClassificationMapStampHeight = 0;
        setupParams.ppClassificationMap = NULL;
        m_classMapActive = false;
        if (NVFBC_SUCCESS != m_nvfbc->NvFBCToDx9VidSetUp(&setupParams)) {
            LOGERR("CaptureRing: probe setup refused entirely - content probe disabled");
            setupParams.bDiffMap = 0;
            setupParams.dwDiffMapBuffSize = 0;
            setupParams.ppDiffMap = NULL;
            m_probeActive = false;
        }
    }
    if (m_probeActive) {
        LOG("CaptureRing: content probe ACTIVE - diffmap 32x32 (%d blocks)%s; capture lines gain blk=/hf=",
            m_diffBlocks, m_classMapActive ? ", classification 16x16 stamps" : " (classification unavailable)");
    }

    // Frame-dump staging: capture-device render targets, filled by GPU-to-GPU StretchRect
    // during the dump window (cheap enough not to distort the arrival timeline the dump
    // exists to photograph) and drained to disk afterward.
    if (m_dumpAtSeconds > 0) {
        bool ok = true;
        for (int i = 0; i < DUMP_FRAMES && ok; i++) {
            if (FAILED(m_capDevice->CreateRenderTarget(m_width, m_height, D3DFMT_A2R10G10B10,
                    D3DMULTISAMPLE_NONE, 0, FALSE, &m_dumpStaging[i], NULL))) {
                ok = false;
            }
        }
        if (ok) {
            m_dumpStartQpc = baseQpc.QuadPart + (LONGLONG)m_dumpAtSeconds * m_freqQuad;
            LOG("CaptureRing: frame dump ARMED - %d frames at t+%ds -> dump_NN_capXXXX.bmp",
                DUMP_FRAMES, m_dumpAtSeconds);
        } else {
            LOGERR("CaptureRing: frame-dump staging allocation failed - dump disabled");
            for (int i = 0; i < DUMP_FRAMES; i++) {
                if (m_dumpStaging[i]) { m_dumpStaging[i]->Release(); m_dumpStaging[i] = NULL; }
            }
            m_dumpAtSeconds = 0;
        }
    }

    if (!m_probeActive && NVFBC_SUCCESS != m_nvfbc->NvFBCToDx9VidSetUp(&setupParams)) {
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
            // never pollute it; gaps over 125 ms are stalls, not cadence). Grab-timeout
            // re-grabs of a static source return SUCCESS at the timeout period and DO
            // enter: that is the source's effective cadence while nothing new is drawn.
            // EMA alpha 1/8: stable within ~8 source frames of a regime change,
            // jitter-immune in steady state.
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
        // Probe reduction: blk = diffmap blocks changed vs the previous grab (0 = content
        // dupe); hf = classification-map byte sum (relative high-frequency-content measure,
        // format empirically calibrated). The scan runs after publish so it never delays a
        // frame, but it does widen the wake-to-regrab window - probe runs are instrument
        // runs, not reference runs.
        char probeSuffix[64];
        probeSuffix[0] = '\0';
        int probeChanged = -1;
        unsigned long long probeHf = 0;
        if (m_probeActive) {
            const unsigned char* diff = (const unsigned char*)m_diffMap;
            probeChanged = 0;
            for (int i = 0; i < m_diffBlocks; i++) probeChanged += (diff[i] != 0);
            if (m_classMapActive) {
                const unsigned char* cls = (const unsigned char*)m_classMap;
                for (int i = 0; i < m_classStamps; i++) probeHf += cls[i];
            }
            snprintf(probeSuffix, sizeof(probeSuffix), " blk=%d/%d hf=%llu", probeChanged, m_diffBlocks, probeHf);
        }

        LOG("capture #%lld arr=%lldus dt=%lldus flush=%lldus col=%lld%s",
            count,
            (long long)((now.QuadPart - m_baseQpc.QuadPart) * usPerTick),
            (long long)(dt * usPerTick),
            (long long)flushUs,
            collapsed,
            probeSuffix);

        // Frame-dump window: stage this capture GPU-to-GPU (does not disturb the timeline);
        // drain to disk only once the window is full, when timing no longer matters.
        if (m_dumpStartQpc != 0 && !m_dumpDrained && now.QuadPart >= m_dumpStartQpc
            && m_dumpCount < DUMP_FRAMES) {
            m_capDevice->StretchRect(m_ring[slot].capSurface, NULL, m_dumpStaging[m_dumpCount], NULL, D3DTEXF_NONE);
            DumpMeta& meta = m_dumpMeta[m_dumpCount];
            meta.captureIndex = count;
            meta.arrQpc = now.QuadPart;
            meta.dtQpc = dt;
            meta.blkChanged = probeChanged;
            meta.hfSum = probeHf;
            m_dumpCount++;
            if (m_dumpCount == DUMP_FRAMES) DrainFrameDump();
        }
    }

    // Partial window at shutdown (stop requested mid-dump): drain what was staged.
    if (m_dumpCount > 0 && !m_dumpDrained) DrainFrameDump();
}

// 24bpp bottom-up BMP. A2R10G10B10 layout: A[31:30] R[29:20] G[19:10] B[9:0]; 8-bit
// conversion drops the two low bits per channel. A wrong channel order would only tint the
// image - the dump's purpose (ghosting visibility) survives any color mistake.
static bool WriteBmp24(const char* path, const unsigned int* pixels, int width, int height, int pitchPx) {
    FILE* f = NULL;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    const int rowBytes = ((width * 3 + 3) / 4) * 4;
    const int dataSize = rowBytes * height;
    unsigned char fileHdr[14] = { 'B','M' };
    *(unsigned int*)(fileHdr + 2) = 14 + 40 + dataSize;
    *(unsigned int*)(fileHdr + 10) = 14 + 40;
    unsigned char infoHdr[40] = { 0 };
    *(unsigned int*)(infoHdr + 0) = 40;
    *(int*)(infoHdr + 4) = width;
    *(int*)(infoHdr + 8) = height;
    *(unsigned short*)(infoHdr + 12) = 1;
    *(unsigned short*)(infoHdr + 14) = 24;
    *(unsigned int*)(infoHdr + 20) = dataSize;
    fwrite(fileHdr, 1, 14, f);
    fwrite(infoHdr, 1, 40, f);
    unsigned char* row = (unsigned char*)malloc(rowBytes);
    if (!row) { fclose(f); return false; }
    memset(row, 0, rowBytes);
    for (int y = height - 1; y >= 0; y--) {
        const unsigned int* src = pixels + (size_t)y * pitchPx;
        for (int x = 0; x < width; x++) {
            const unsigned int px = src[x];
            row[x * 3 + 0] = (unsigned char)((px >> 2)  & 0xFF);   // B10 -> 8
            row[x * 3 + 1] = (unsigned char)((px >> 12) & 0xFF);   // G10 -> 8
            row[x * 3 + 2] = (unsigned char)((px >> 22) & 0xFF);   // R10 -> 8
        }
        fwrite(row, 1, rowBytes, f);
    }
    free(row);
    fclose(f);
    return true;
}

void CaptureRing::DrainFrameDump() {
    m_dumpDrained = true;
    LOG("CaptureRing: frame-dump drain start (%d frames) - capture paused", m_dumpCount);

    IDirect3DSurface9* sysmem = NULL;
    if (FAILED(m_capDevice->CreateOffscreenPlainSurface(m_width, m_height, D3DFMT_A2R10G10B10,
            D3DPOOL_SYSTEMMEM, &sysmem, NULL))) {
        LOGERR("CaptureRing: frame-dump sysmem surface failed - dump lost");
        return;
    }

    const double usPerTick = 1000000.0 / (double)m_freqQuad;
    for (int i = 0; i < m_dumpCount; i++) {
        char path[64];
        snprintf(path, sizeof(path), "dump_%02d_cap%lld.bmp", i, m_dumpMeta[i].captureIndex);
        bool ok = false;
        if (SUCCEEDED(m_capDevice->GetRenderTargetData(m_dumpStaging[i], sysmem))) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(sysmem->LockRect(&lr, NULL, D3DLOCK_READONLY))) {
                ok = WriteBmp24(path, (const unsigned int*)lr.pBits, m_width, m_height, lr.Pitch / 4);
                sysmem->UnlockRect();
            }
        }
        LOG("dump %02d cap=%lld arr=%lldus dt=%lldus blk=%d hf=%llu file=%s%s",
            i, m_dumpMeta[i].captureIndex,
            (long long)((m_dumpMeta[i].arrQpc - m_baseQpc.QuadPart) * usPerTick),
            (long long)(m_dumpMeta[i].dtQpc * usPerTick),
            m_dumpMeta[i].blkChanged, m_dumpMeta[i].hfSum,
            path, ok ? "" : " WRITE-FAILED");
    }
    sysmem->Release();
    LOG("CaptureRing: frame-dump drain complete");
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
                out->beforeSurface = m_ring[slot].mainSurface;
                out->beforeTexture = m_ring[slot].mainTexture;
                out->beforeTs = ts;
                out->beforeDiff = diff;
                out->beforeDepth = (int)(p - 1 - i);
            }
        } else {
            if (-diff < bestAfterDiff) {
                bestAfterDiff = -diff;
                out->hasAfter = true;
                out->afterSurface = m_ring[slot].mainSurface;
                out->afterTexture = m_ring[slot].mainTexture;
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
