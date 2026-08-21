#include "CaptureRing.h"
#include <SimpleLogger.h>
#include <limits.h>
#include <math.h>
#include <string.h>

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
        m_ring[i].batchStart.QuadPart = 0;
        m_ring[i].member = 0;
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
    if (m_fgSmallRT)  { m_fgSmallRT->Release();  m_fgSmallRT = NULL; }
    if (m_fgSmallSys) { m_fgSmallSys->Release(); m_fgSmallSys = NULL; }
    delete[] m_fgLumWake[0]; m_fgLumWake[0] = NULL;
    delete[] m_fgLumWake[1]; m_fgLumWake[1] = NULL;
    delete[] m_fgLumKept;    m_fgLumKept = NULL;
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
        m_ring[i].sharedHandle = shared;
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

    // ---- Content-phase instrument resources (-fgphase). ----
    // A failed setup disables the instrument, never the relay: it is a measurement rig.
    if (m_fgPhaseRequested) {
        hr = m_capDevice->CreateRenderTarget(kFgW, kFgH, D3DFMT_A2B10G10R10,
                                             D3DMULTISAMPLE_NONE, 0, FALSE,
                                             &m_fgSmallRT, NULL);
        if (SUCCEEDED(hr)) {
            hr = m_capDevice->CreateOffscreenPlainSurface(kFgW, kFgH, D3DFMT_A2B10G10R10,
                                                          D3DPOOL_SYSTEMMEM,
                                                          &m_fgSmallSys, NULL);
        }
        if (SUCCEEDED(hr)) {
            m_fgLumWake[0] = new float[kFgW * kFgH];
            m_fgLumWake[1] = new float[kFgW * kFgH];
            m_fgLumKept = new float[kFgW * kFgH];
            m_fgPhaseActive = true;
            LOG("fgphase instrument ACTIVE: per-batch content phase f of the generated "
                "member against its real neighbours, %dx%d luma; join f to the flip lines "
                "offline for g. Readback stalls the capture thread once per wake - "
                "instrument runs are not reference runs.", kFgW, kFgH);
        } else {
            LOGERR("fgphase instrument DISABLED: small-surface setup failed (0x%08x)", hr);
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

    NVFBC_TODX9VID_OUT_BUF outBuf[1] = {};
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
    policy::BatchState batchState;
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

        const LONGLONG prevArrival = batchState.lastArrivalTs;
        const policy::BatchDecision batch =
            policy::UpdateBatch(batchState, now.QuadPart, batchThresholdQpc);
        // Source-period estimate (gaps over 125 ms are stalls, not cadence). Grab-timeout
        // re-grabs of a static source return SUCCESS at the timeout period and DO enter:
        // that is the source's effective cadence while nothing new is drawn. EMA alpha
        // 1/8: stable within ~8 source frames of a regime change, jitter-immune in
        // steady state.
        if (batch.batchGap > 0 && batch.batchGap < m_freqQuad / 8) {
            long long ema = m_srcPeriodEmaQpc.load(std::memory_order_relaxed);
            ema = ema ? (ema * 7 + batch.batchGap) / 8 : batch.batchGap;
            m_srcPeriodEmaQpc.store(ema, std::memory_order_relaxed);
        }

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
        m_ring[slot].member = batch.member;
        // Publish the batch start for the stage-6 walk. At batch OPEN only: every member
        // shares the start stamp, so one entry names the whole batch, and the release
        // store is what lets the present thread read the entry without touching slots.
        if (batch.member == 0) {
            const long long opens = m_batchOpens.load(std::memory_order_relaxed);
            m_batchStarts[opens % kBatchHistory] = batch.stampTs;
            m_batchOpens.store(opens + 1, std::memory_order_release);

            // Feed the rotation vote once per batch, at open, where the batch's grid
            // position is defined. A batch that cannot be placed simply casts no vote; a
            // batch that arrived off-cadence means the batch counter and the grid may have
            // slipped apart, and residue is only meaningful while they march together, so
            // that drops the vote rather than letting it drift.
            m_rotRealMember = -1;
            m_rotSpacing = 0;
            if (m_rotationOracle) {
                const long long batchPeriod = m_srcPeriodEmaQpc.load(std::memory_order_relaxed);
                int stride = 0, flipsPerSource = 0;
                long long spacing = 0;
                if (m_rotationOracle->Grid(batchPeriod, &stride, &flipsPerSource, &spacing)) {
                    if (stride != m_rotation.stride ||
                        flipsPerSource != m_rotation.flipsPerSource) {
                        policy::RotationReset(m_rotation, stride, flipsPerSource);
                        m_phaseKeepResets++;
                    }
                    m_rotSpacing = spacing;

                    // FEED THE VOTE A BATCH THAT IS OLD ENOUGH TO ANSWER FOR. A batch's own
                    // anchor flip has NOT been delivered when it opens - measured on a real
                    // x3 capture, the freshest flip the relay knows at batch open is 5974 us
                    // (p50) in the past, more than one x3 flip step, and 98.2% of batches
                    // cannot be placed at all. Asking here is what made the first build vote
                    // on nothing and steer nothing. Two batches back is ~24 ms at x3, past
                    // the p95 delivery lag with room to spare.
                    static const long long kVoteLagBatches = 2;
                    const long long voteIdx = opens - kVoteLagBatches;
                    if (voteIdx >= 0) {
                        const LONGLONG vbs = m_batchStarts[voteIdx % kBatchHistory];
                        long long off = 0;
                        int steps = 0;
                        const long long prevAnchor =
                            m_rotation.haveAnchor ? m_rotation.lastAnchorTs : -1;
                        if (m_rotationOracle->AnchorAndSteps(vbs, prevAnchor, &off, &steps)) {
                            if (policy::RotationAdvance(m_rotation, vbs - off, steps)) {
                                policy::RotationObserve(m_rotation, off);
                            }
                        }
                    }

                    // Decide for THIS batch by carrying the vote's position forward over the
                    // few milliseconds since it was last established - exact while the flip
                    // step is, which over two batches it comfortably is.
                    const int pos = policy::RotationPositionAt(m_rotation, batch.stampTs,
                                                               spacing);
                    if (pos >= 0) m_rotRealMember = policy::RotationRealMember(m_rotation, pos);
                    // The rotation is PROVEN LIVE but this batch cannot be placed on it
                    // (an unplaceable vote batch left the position stale). Keeping member 1
                    // here risks keeping a GENERATED frame - the backward-step carrier the
                    // 2026-08-20 capture filmed - so keep NOTHING: a hole becomes a repeat
                    // (nearest) or a blend (blend mode), both honest. Strictly gated on the
                    // vote being currently valid: where it is not (x2, FG off, desktop, an
                    // unconverged start) this stays plain keep-real, never a drop.
                    if (m_rotation.valid && m_rotRealMember < 0) {
                        m_rotRealMember = m_rotation.flipsPerSource;   // no member is real
                        m_phaseKeepUndecided++;
                    }
                    if (m_rotRealMember >= 0) m_phaseKeepBatches++;

                    // Uniform stamp convention while the rotation is armed: pass the
                    // spacing to the keep decision for EVERY batch of a rotating regime,
                    // steered or not, so all stamps live on one content-aligned timeline.
                    // Where composition cannot rotate the spacing stays 0 and stamps stay
                    // at batch start - the x2 path, bit-for-bit.
                    if (m_rotation.period <= 1) m_rotSpacing = 0;
                }
            }

            // Reclaim the previous batch's coalesced single. A single-member batch whose
            // named keeper (member 1) never arrived kept nothing - but the lone wake's
            // pixels are the FRONTBUFFER AT GRAB, i.e. the newest flip, which in the
            // [gen,real] class is the REAL frame: the x2-proven coalesced-single mechanism,
            // and the reason singles concentrate 7x in exactly that class (18% vs 2.5%,
            // measured). Dropping them threw away ~5 real frames per second that were
            // actually captured. The slot is re-validated one batch late, which is the
            // earliest the fact is knowable and ~11 ms - well inside the bracketing lag.
            if (m_prevKeeper == 1 && m_prevLastMember == 0 && count >= 1) {
                const int prevSlot = (int)((count - 1) % RING_SIZE);
                m_ring[prevSlot].timestamp.QuadPart = m_prevBatchStart + m_prevSpacing;
                m_ring[prevSlot].valid = true;
                m_phaseKeepReclaimed++;
            }
            m_prevKeeper = m_rotRealMember;
            m_prevBatchStart = batch.stampTs;
            m_prevSpacing = m_rotSpacing;
        }
        if (batch.member > 0) m_prevLastMember = batch.member;
        else m_prevLastMember = 0;

        // What this wake does to the ring, decided in the policy layer so it is testable at
        // all: with no rotation guidance (m_rotRealMember < 0 - x2, frame generation off, an
        // unconverged vote) it reduces to exactly the keep-real rule this loop always had.
        const policy::KeepDecision keep =
            policy::DecideKeep(batch, m_rotRealMember, m_rotSpacing, count >= 1);
        if (m_rotRealMember >= 0 && batch.member == 0) {
            if (m_rotRealMember >= 2) m_phaseKeepEmpty++;
            else if (m_rotRealMember == 0) m_phaseKeepFlipped++;
        }

        m_ring[slot].timestamp.QuadPart = keep.stampTs;
        m_ring[slot].batchStart.QuadPart = batch.stampTs;
        m_ring[slot].valid = keep.keepThis;
        m_writeCount = count + 1;
        m_published.store(count + 1);  // publish only after the slot write is GPU-complete

        if (keep.retractPrev) {
            // Retract the previous member (the generated frame): hide it from future brackets.
            // Content is never overwritten, so a present read already in flight stays coherent.
            m_ring[(int)((count - 1) % RING_SIZE)].valid = false;
        }
        if (keep.collapsed) collapsed++;

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

        if (m_fgPhaseActive) {
            FgPhaseOnWake(batch.member, slot,
                          (LONGLONG)((batch.stampTs - m_baseQpc.QuadPart) * usPerTick),
                          keep.keepThis);
        }
    }
}

// Downscale a ring surface on the GPU, read it back, convert to blurred luma. The blur
// (separable Gaussian, sigma 2) is part of the validated estimator configuration: it is
// irrelevant to lerp recovery but extends the first-order range for WARPED content, which
// is what driver-generated frames are suspected to be at x3.
bool CaptureRing::ReadFgLuma(IDirect3DSurface9* src, float* out) {
    if (FAILED(m_capDevice->StretchRect(src, NULL, m_fgSmallRT, NULL, D3DTEXF_LINEAR)))
        return false;
    if (FAILED(m_capDevice->GetRenderTargetData(m_fgSmallRT, m_fgSmallSys)))
        return false;
    D3DLOCKED_RECT lr;
    if (FAILED(m_fgSmallSys->LockRect(&lr, NULL, D3DLOCK_READONLY)))
        return false;
    for (int y = 0; y < kFgH; y++) {
        const DWORD* row = (const DWORD*)((const BYTE*)lr.pBits + y * lr.Pitch);
        float* dst = out + y * kFgW;
        for (int x = 0; x < kFgW; x++) {
            const DWORD v = row[x];   // A2B10G10R10: R low, then G, then B
            const float r = (float)(v & 0x3FF);
            const float g = (float)((v >> 10) & 0x3FF);
            const float b = (float)((v >> 20) & 0x3FF);
            dst[x] = (2.0f * r + 5.0f * g + b) * 0.125f;
        }
    }
    m_fgSmallSys->UnlockRect();

    // Separable Gaussian, sigma 2, radius 6; kernel constants fixed at compile time.
    static const float k[7] = { 0.19967f, 0.17603f, 0.12098f, 0.06476f,
                                0.02700f, 0.00874f, 0.00220f };
    float line[kFgW > kFgH ? kFgW : kFgH];
    for (int y = 0; y < kFgH; y++) {
        float* rowp = out + y * kFgW;
        for (int x = 0; x < kFgW; x++) line[x] = rowp[x];
        for (int x = 0; x < kFgW; x++) {
            float acc = k[0] * line[x];
            for (int t = 1; t <= 6; t++) {
                const int lo = x - t < 0 ? 0 : x - t;
                const int hi = x + t >= kFgW ? kFgW - 1 : x + t;
                acc += k[t] * (line[lo] + line[hi]);
            }
            rowp[x] = acc;
        }
    }
    for (int x = 0; x < kFgW; x++) {
        for (int y = 0; y < kFgH; y++) line[y] = out[y * kFgW + x];
        for (int y = 0; y < kFgH; y++) {
            float acc = k[0] * line[y];
            for (int t = 1; t <= 6; t++) {
                const int lo = y - t < 0 ? 0 : y - t;
                const int hi = y + t >= kFgH ? kFgH - 1 : y + t;
                acc += k[t] * (line[lo] + line[hi]);
            }
            out[y * kFgW + x] = acc;
        }
    }
    return true;
}

void CaptureRing::FgPhaseOnWake(int member, int slot, LONGLONG batchStartUs,
                                bool keptThisMember) {
    float* L = m_fgLumWake[m_fgWakeParity];
    if (!ReadFgLuma(m_ring[slot].capSurface, L)) {
        LOGERR("fgphase: readback failed, instrument disabled");
        m_fgPhaseActive = false;
        return;
    }
    const float* prevWake = m_fgLumWake[1 - m_fgWakeParity];

    if (member >= 1) {
        // This wake retracted the previous one: prev wake = the generated member, this
        // wake = the real member. Measure the generated frame's content phase between the
        // last KEPT real frame and this one. Only the first retraction of a batch measures
        // (a third member's semantics are not a bracket).
        if (member == 1 && m_fgPrevWakeValid && m_fgKeptValid) {
            static const int kMx = kFgW / 20, kMy = kFgH / 20;   // 5% margins
            double sgd = 0.0, sdd = 0.0, sgr = 0.0;
            for (int y = kMy; y < kFgH - kMy; y++) {
                const int base = y * kFgW;
                for (int x = kMx; x < kFgW - kMx; x++) {
                    const double d = L[base + x] - m_fgLumKept[base + x];
                    const double gd = prevWake[base + x] - m_fgLumKept[base + x];
                    const double gr = prevWake[base + x] - L[base + x];
                    sgd += gd * d;
                    sdd += d * d;
                    sgr += gr * gr;
                }
            }
            const int n = (kFgW - 2 * kMx) * (kFgH - 2 * kMy);
            const double motion = sqrt(sdd / n);
            if (sdd > 0.0) {
                const double f = sgd / sdd;
                double srr = 0.0;
                for (int y = kMy; y < kFgH - kMy; y++) {
                    const int base = y * kFgW;
                    for (int x = kMx; x < kFgW - kMx; x++) {
                        const double d = L[base + x] - m_fgLumKept[base + x];
                        const double r = (prevWake[base + x] - m_fgLumKept[base + x]) - f * d;
                        srr += r * r;
                    }
                }
                // Raw per-batch readings; the offline pass owns the gates (motion floor,
                // residual ceiling) so thresholds can be tuned without a rebuild. gdiff is
                // the gen-vs-real pixel distance, the discriminator the offline pass needs
                // because the NvFBC race and dup-pairs both put REAL pixels in the gen
                // slot 33-50% of the time: those batches read f~1 with gdiff~0 and are a
                // different population, not evidence about generated content.
                LOG("fgphase: arr=%lldus f=%.4f motion=%.2f resid=%.3f gdiff=%.2f",
                    (long long)batchStartUs, f, motion,
                    motion > 0.0 ? sqrt(srr / n) / motion : 0.0,
                    sqrt(sgr / n));
            }
        }
        // Track whatever the RING actually kept, never whatever this instrument assumed.
        // Default keep-real retains the arriving member, but phase-aware keep-real can
        // retain member 0 instead, and an instrument holding the other frame as its
        // reference would measure the next batch against a frame that was thrown away.
        if (keptThisMember) {
            memcpy(m_fgLumKept, L, sizeof(float) * kFgW * kFgH);
            m_fgKeptValid = true;
        } else {
            memcpy(m_fgLumKept, prevWake, sizeof(float) * kFgW * kFgH);
            m_fgKeptValid = true;
        }
    } else if (m_fgPrevMember == 0 && m_fgPrevWakeValid) {
        // The previous wake opened a batch and was never retracted: a kept single
        // (coalesced real, or FG off). Promote it - one wake late, which is the earliest
        // the fact is knowable.
        memcpy(m_fgLumKept, prevWake, sizeof(float) * kFgW * kFgH);
        m_fgKeptValid = true;
    }

    m_fgPrevMember = member;
    m_fgPrevWakeValid = true;
    m_fgWakeParity ^= 1;
}

void CaptureRing::FindBracket(LONGLONG targetQpc, const policy::StampOverlay* overlay,
                              FrameBracket* out) const {
    *out = FrameBracket{};

    const long long p = m_published.load();
    long long oldest = p - (RING_SIZE - 1);
    if (oldest < 0) oldest = 0;

    LONGLONG bestBeforeDiff = LLONG_MAX, bestAfterDiff = LLONG_MAX;
    for (long long i = p - 1; i >= oldest; i--) {
        int slot = (int)(i % RING_SIZE);
        if (!m_ring[slot].valid) continue;
        LONGLONG ts = m_ring[slot].timestamp.QuadPart;
        // Corrections are measured PER BATCH, so they are looked up by the slot's batch
        // start rather than by its own stamp - the two differ only where member stamps are
        // offset onto their own flips, and there a member would otherwise miss the
        // correction its batch earned.
        if (overlay) ts -= overlay->CorrectionFor(m_ring[slot].batchStart.QuadPart);
        LONGLONG diff = targetQpc - ts;
        if (diff >= 0) {
            if (diff < bestBeforeDiff) {
                bestBeforeDiff = diff;
                out->info.hasBefore = true;
                out->beforeSurface = m_ring[slot].mainSurface;
                out->beforeTexture = m_ring[slot].mainTexture;
                out->info.beforeTs = ts;
                out->info.beforeDiff = diff;
                out->beforeDepth = (int)(p - 1 - i);
                out->beforeSlot = slot;
                out->beforeMember = m_ring[slot].member;
            }
        } else {
            if (-diff < bestAfterDiff) {
                bestAfterDiff = -diff;
                out->info.hasAfter = true;
                out->afterSurface = m_ring[slot].mainSurface;
                out->afterTexture = m_ring[slot].mainTexture;
                out->info.afterTs = ts;
                out->info.afterDiff = -diff;
                out->afterSlot = slot;
                out->afterMember = m_ring[slot].member;
            }
        }
    }

    if (out->info.hasBefore && out->info.hasAfter) {
        out->weight = (double)out->info.beforeDiff /
                      (double)(out->info.beforeDiff + out->info.afterDiff);
    } else if (out->info.hasBefore) {
        out->weight = 1.0;  // target is past the newest frame; nothing to interpolate toward
    } else {
        out->weight = 0.0;
    }
}

