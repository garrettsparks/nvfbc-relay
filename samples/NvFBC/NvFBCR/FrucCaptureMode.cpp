#include "FrucCaptureMode.h"
#include <SimpleLogger.h>

// Forward declaration of CUDA kernel (implemented in InterpolateKernel.cu)
extern "C" void launchInterpolateKernel(
    const uint8_t* frame0,
    const uint8_t* frame1,
    const uint8_t* flowData,
    uint8_t* output,
    int width,
    int height,
    int flowWidth,
    int flowHeight,
    int flowStrideBytes,
    int gridSize,
    float weight,
    CUstream stream);

FrucCaptureMode::FrucCaptureMode(float framerate)
    : m_targetFramerate(framerate == 0.0f ? 60.0f : framerate)
    , m_isVsyncMode(framerate == 0.0f)
    , m_width(0)
    , m_height(0)
    , m_flowWidth(0)
    , m_flowHeight(0)
    , m_cuContext(nullptr)
    , m_cuDevice(0)
    , m_cudaStream(nullptr)
    , m_cudaInitialized(false)
    , m_cudaOutputResource(nullptr)
    , m_nvofInitialized(false)
    , m_gridSize(4)
    , m_ringWriteIndex(0)
    , m_capturedFrameCount(0)
    , m_cudaOutputFrame(0)
    , m_outputSurface(nullptr)
    , m_device(nullptr)
    , m_captureThread(NULL)
    , m_captureRunning(false)
    , m_sessionInvalidated(false)
    , m_captureGrabCount(0)
    , m_nvfbcCuda(nullptr)
    , m_cudaGrabParams{}
    , m_grabInfo{}
    , m_strideChecked(false)
    , m_needsStrideCopy(false)
    , m_grabStride(0)
    , m_grabTempBuffer(0)
{
    LOG("=== FrucCaptureMode GPU-Resident Pipeline V4 (NvFBCCuda, no D3D9 on capture) ===");
    LOG("Target framerate: %.2f fps", m_targetFramerate);
    LOG("VSync mode: %s", m_isVsyncMode ? "yes" : "no");
    LOG("Flow grid size: %d, Ring size: %d", m_gridSize, RING_SIZE);

    for (int i = 0; i < RING_SIZE; i++) {
        m_cudaFrames[i] = 0;
        m_frameHistory[i].timestamp.QuadPart = 0;
        m_frameHistory[i].valid = false;
    }

    QueryPerformanceFrequency(&m_perfFreq);
}

FrucCaptureMode::~FrucCaptureMode() {
    LOG("=== FrucCaptureMode cleanup ===");
    Cleanup();
}

void FrucCaptureMode::Cleanup() {
    // Stop capture thread
    if (m_captureThread) {
        m_captureRunning.store(false);
        WaitForSingleObject(m_captureThread, 5000);
        CloseHandle(m_captureThread);
        m_captureThread = NULL;
    }

    // Release NvFBCCuda (must be done before CUDA context destruction)
    if (m_nvfbcCuda) {
        m_nvfbcCuda->NvFBCCudaRelease();
        m_nvfbcCuda = nullptr;
    }

    // Release NvOF resources (must be done before CUDA context destruction)
    m_outputBuffers.clear();
    m_inputBuffers.clear();
    m_nvOF.reset();
    m_nvofInitialized = false;

    // Unregister CUDA-D3D9 interop (output only)
    if (m_cudaOutputResource) {
        cuGraphicsUnregisterResource(m_cudaOutputResource);
        m_cudaOutputResource = nullptr;
    }

    // Free CUDA device memory
    for (int i = 0; i < RING_SIZE; i++) {
        if (m_cudaFrames[i]) {
            cuMemFree(m_cudaFrames[i]);
            m_cudaFrames[i] = 0;
        }
    }
    if (m_cudaOutputFrame) { cuMemFree(m_cudaOutputFrame); m_cudaOutputFrame = 0; }
    if (m_grabTempBuffer) { cuMemFree(m_grabTempBuffer); m_grabTempBuffer = 0; }

    // Release D3D9 surfaces (output only)
    if (m_outputSurface) { m_outputSurface->Release(); m_outputSurface = nullptr; }

    // Destroy CUDA stream
    if (m_cudaStream) { cuStreamDestroy(m_cudaStream); m_cudaStream = nullptr; }

    // Destroy D3D9 interop context
    if (m_cuContext) { cuCtxDestroy(m_cuContext); m_cuContext = nullptr; }

    m_cudaInitialized = false;
}

UINT FrucCaptureMode::GetPresentationInterval() const {
    return D3DPRESENT_INTERVAL_ONE;
}

bool FrucCaptureMode::InitCuda() {
    LOG("Initializing CUDA with D3D9 interop...");

    CUresult result = cuInit(0);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuInit", result);
        return false;
    }

    result = cuD3D9CtxCreate(&m_cuContext, &m_cuDevice, 0, m_device);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuD3D9CtxCreate", result);
        return false;
    }

    char deviceName[256];
    cuDeviceGetName(deviceName, sizeof(deviceName), m_cuDevice);
    int ccMajor = 0, ccMinor = 0;
    cuDeviceGetAttribute(&ccMajor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, m_cuDevice);
    cuDeviceGetAttribute(&ccMinor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, m_cuDevice);
    LOG("CUDA device: %s (compute capability %d.%d, sm_%d%d)", deviceName, ccMajor, ccMinor, ccMajor, ccMinor);

    // Present thread stream (only stream needed — capture thread uses NvFBCCuda directly)
    result = cuStreamCreate(&m_cudaStream, CU_STREAM_DEFAULT);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuStreamCreate(present)", result);
        return false;
    }

    LOG("CUDA initialized with D3D9 interop (1 stream: present only)");
    m_cudaInitialized = true;
    return true;
}

bool FrucCaptureMode::InitNvOF() {
    LOG("Initializing NvOF (Optical Flow)...");

    if (!m_cudaInitialized) {
        LOGERR("CUDA not initialized before NvOF");
        return false;
    }

    if (m_width == 0 || m_height == 0) {
        LOGERR("Frame dimensions not set before NvOF init");
        return false;
    }

    try {
        m_nvOF = NvOFCuda::Create(
            m_cuContext, m_width, m_height,
            NV_OF_BUFFER_FORMAT_ABGR8,
            NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR,
            NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR,
            NV_OF_MODE_OPTICALFLOW,
            NV_OF_PERF_LEVEL_FAST,
            m_cudaStream, m_cudaStream
        );

        uint32_t hwGridSize;
        if (!m_nvOF->CheckGridSize(m_gridSize)) {
            if (!m_nvOF->GetNextMinGridSize(m_gridSize, hwGridSize)) {
                LOGERR("No supported grid size found");
                return false;
            }
            LOG("Requested grid size %d not supported, using %d", m_gridSize, hwGridSize);
            m_gridSize = hwGridSize;
        }

        m_nvOF->Init(m_gridSize);

        m_flowWidth = (m_width + m_gridSize - 1) / m_gridSize;
        m_flowHeight = (m_height + m_gridSize - 1) / m_gridSize;
        LOG("Flow vector grid: %dx%d (grid size %d)", m_flowWidth, m_flowHeight, m_gridSize);

        m_inputBuffers = m_nvOF->CreateBuffers(NV_OF_BUFFER_USAGE_INPUT, 2);
        LOG("Created %d NvOF input buffers", (int)m_inputBuffers.size());

        m_outputBuffers = m_nvOF->CreateBuffers(NV_OF_BUFFER_USAGE_OUTPUT, 1);
        LOG("Created %d NvOF output buffers", (int)m_outputBuffers.size());

        m_nvofInitialized = true;
        LOG("NvOF initialized successfully");
        return true;
    }
    catch (const std::exception& e) {
        LogNvOFError("NvOF initialization", e);
        return false;
    }
}

bool FrucCaptureMode::AllocateBuffers() {
    LOG("Allocating GPU-resident buffers (V4: no interop surfaces)...");

    size_t frameSize = m_width * m_height * 4;

    // Ring buffer slots
    for (int i = 0; i < RING_SIZE; i++) {
        CUresult result = cuMemAlloc(&m_cudaFrames[i], frameSize);
        if (result != CUDA_SUCCESS) {
            LogCudaError("cuMemAlloc ring slot", result);
            return false;
        }
    }
    LOG("Allocated %d ring buffer slots (%zu bytes each)", RING_SIZE, frameSize);

    // Output frame
    CUresult result = cuMemAlloc(&m_cudaOutputFrame, frameSize);
    if (result != CUDA_SUCCESS) { LogCudaError("cuMemAlloc output", result); return false; }

    // D3D9 output surface (for present path only)
    HRESULT hr = m_device->CreateOffscreenPlainSurface(
        m_width, m_height, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_outputSurface, NULL);
    if (FAILED(hr)) {
        LOGERR("Failed to create output surface (HRESULT: 0x%08x)", hr);
        return false;
    }

    // Register output surface for CUDA interop (present thread only)
    result = cuGraphicsD3D9RegisterResource(&m_cudaOutputResource, m_outputSurface,
                                             CU_GRAPHICS_REGISTER_FLAGS_NONE);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuGraphicsD3D9RegisterResource(outputSurface)", result);
        return false;
    }

    // Allocate temp buffer for padded grabs if stride mismatch detected
    if (m_needsStrideCopy) {
        size_t tempSize = (size_t)m_grabStride * m_height;
        CUresult tempRes = cuMemAlloc(&m_grabTempBuffer, tempSize);
        if (tempRes != CUDA_SUCCESS) {
            LogCudaError("cuMemAlloc(grabTempBuffer)", tempRes);
            return false;
        }
        LOG("Allocated grab temp buffer (%zu bytes, stride=%d)", tempSize, m_grabStride);
    }

    LOG("GPU-resident buffer allocation complete (%d ring + 1 output, no capture interop)", RING_SIZE);
    return true;
}

bool FrucCaptureMode::CreateNvFBCCuda() {
    LOG("Creating NvFBCCuda instance...");

    NvFBCCreateParams createParams;
    memset(&createParams, 0, sizeof(createParams));
    createParams.dwVersion = NVFBC_CREATE_PARAMS_VER;
    createParams.dwInterfaceType = NVFBC_SHARED_CUDA;
    createParams.pDevice = (void*)m_device;
    createParams.cudaCtx = (void*)m_cuContext;
    createParams.dwAdapterIdx = 0;  // ignored when pDevice is set

    NVFBCRESULT res = pNVFBCLib->createEx(&createParams);
    if (res != NVFBC_SUCCESS) {
        LOGERR("Failed to create NvFBCCuda instance (result: 0x%X)", res);
        return false;
    }

    m_nvfbcCuda = (NvFBCCuda*)createParams.pNvFBC;
    if (!m_nvfbcCuda) {
        LOGERR("NvFBCCuda creation returned success but pNvFBC is null");
        return false;
    }

    LOG("NvFBCCuda instance created (maxRes: %dx%d)", createParams.dwMaxDisplayWidth, createParams.dwMaxDisplayHeight);

    // Setup: ARGB format (matches NvOF ABGR8 — same byte order, just naming convention)
    NVFBC_CUDA_SETUP_PARAMS setupParams = {};
    setupParams.dwVersion = NVFBC_CUDA_SETUP_PARAMS_VER;
    setupParams.eFormat = NVFBC_TOCUDA_ARGB;
    setupParams.bHDRRequest = 0;

    res = m_nvfbcCuda->NvFBCCudaSetup(&setupParams);
    if (res != NVFBC_SUCCESS) {
        LOGERR("NvFBCCudaSetup failed (result: 0x%X)", res);
        m_nvfbcCuda->NvFBCCudaRelease();
        m_nvfbcCuda = nullptr;
        return false;
    }

    LOG("NvFBCCuda setup complete (format: ARGB)");
    return true;
}

bool FrucCaptureMode::Setup() {
    LOG("FrucCaptureMode::Setup() - GPU-resident pipeline V4 (NvFBCCuda)");

    m_device = g_pD3D9Device;

    if (!InitCuda()) {
        LOGERR("Failed to initialize CUDA with D3D9 interop");
        return false;
    }

    if (!CreateNvFBCCuda()) {
        LOGERR("Failed to create NvFBCCuda instance");
        return false;
    }

    LOG("FrucCaptureMode setup complete (NvOF and buffers will be initialized in Run)");
    return true;
}

// =============================================================================
// Capture thread: NvFBCCuda grab directly to ring buffer — no D3D9, no interop
// Duplicate detection: WAIT_WITH_TIMEOUT returns instantly for new frames but
// blocks ~dwWaitTime ms when no new frame. We time each grab — fast return means
// new frame (advance ring), slow return means stale (skip). This naturally
// throttles ring advances to the source display's refresh rate regardless of
// what that rate is, keeping the ring populated with temporally-spaced unique
// frames for proper bracket selection.
// =============================================================================
DWORD WINAPI FrucCaptureMode::CaptureThreadProc(LPVOID param) {
    FrucCaptureMode* self = (FrucCaptureMode*)param;
    LOG("Capture thread started (V4: NvFBCCuda, pure CUDA, no D3D9)");

    // Set CUDA context on this thread
    cuCtxSetCurrent(self->m_cuContext);

    // Local copy of grab params — each grab gets its own pCUDADeviceBuffer
    NVFBC_CUDA_GRAB_FRAME_PARAMS grabParams = self->m_cudaGrabParams;
    NvFBCFrameGrabInfo grabInfo = {};
    grabParams.pNvFBCFrameGrabInfo = &grabInfo;

    // Stride handling is determined by probe grab before thread launch.
    bool needsStrideCopy = self->m_needsStrideCopy;

    // Duplicate detection: time each grab to distinguish new vs stale frames.
    // WAIT_WITH_TIMEOUT returns instantly when a new frame is ready, but blocks
    // for ~dwWaitTime when no new frame is available.
    LARGE_INTEGER perfFreq;
    QueryPerformanceFrequency(&perfFreq);
    double usPerTick = 1000000.0 / (double)perfFreq.QuadPart;
    double dupThresholdUs = (double)self->m_cudaGrabParams.dwWaitTime * 500.0; // half the timeout in µs
    int dupCount = 0;

    while (self->m_captureRunning.load()) {
        int writeSlot = self->m_ringWriteIndex.load();

        if (needsStrideCopy) {
            grabParams.pCUDADeviceBuffer = (void*)self->m_grabTempBuffer;
        } else {
            grabParams.pCUDADeviceBuffer = (void*)self->m_cudaFrames[writeSlot];
        }

        LARGE_INTEGER tBefore, tAfter;
        QueryPerformanceCounter(&tBefore);
        NVFBCRESULT res = self->m_nvfbcCuda->NvFBCCudaGrabFrame(&grabParams);
        QueryPerformanceCounter(&tAfter);

        if (res == NVFBC_SUCCESS) {
            double grabUs = (double)(tAfter.QuadPart - tBefore.QuadPart) * usPerTick;

            // Fast return = new frame available, slow return = timeout (stale frame)
            if (grabUs >= dupThresholdUs) {
                dupCount++;
                continue;  // Don't advance ring — frame data is same as previous
            }

            LARGE_INTEGER capTime;
            QueryPerformanceCounter(&capTime);

            if (needsStrideCopy) {
                CUDA_MEMCPY2D cp = {};
                cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
                cp.srcDevice = self->m_grabTempBuffer;
                cp.srcPitch = self->m_grabStride;
                cp.dstMemoryType = CU_MEMORYTYPE_DEVICE;
                cp.dstDevice = self->m_cudaFrames[writeSlot];
                cp.dstPitch = self->m_width * 4;
                cp.WidthInBytes = self->m_width * 4;
                cp.Height = self->m_height;
                cuMemcpy2D(&cp);
            }

            // Record timestamp and advance ring buffer
            // Write order: data → timestamp → valid → writeIndex (seq_cst)
            self->m_frameHistory[writeSlot].timestamp.QuadPart = capTime.QuadPart;
            self->m_frameHistory[writeSlot].valid = true;

            int count = self->m_capturedFrameCount.fetch_add(1) + 1;
            if (count <= 5) {
                LOG("Captured frame %d to ring slot %d (dup_skipped=%d)", count, writeSlot, dupCount);
                dupCount = 0;
            }

            self->m_ringWriteIndex.store((writeSlot + 1) % RING_SIZE);
            self->m_captureGrabCount.fetch_add(1);
        }
        else if (res == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("NvFBC session invalidated on capture thread");
            self->m_sessionInvalidated.store(true);
            break;
        }
        // Non-SUCCESS, non-error: timeout expired, loop again
    }

    LOG("Capture thread exiting (total duplicates skipped: %d)", dupCount);
    return 0;
}

// =============================================================================
// Optical flow: copies ring frames → NvOF inputs, executes flow
// =============================================================================
bool FrucCaptureMode::ComputeOpticalFlow(CUdeviceptr framePrev, CUdeviceptr frameCurr) {
    if (!m_nvofInitialized) return false;

    try {
        cuCtxSetCurrent(m_cuContext);

        NvOFBufferCudaDevicePtr* nvofInput0 = dynamic_cast<NvOFBufferCudaDevicePtr*>(m_inputBuffers[0].get());
        NvOFBufferCudaDevicePtr* nvofInput1 = dynamic_cast<NvOFBufferCudaDevicePtr*>(m_inputBuffers[1].get());
        if (!nvofInput0 || !nvofInput1) {
            LOGERR("Failed to get NvOF input buffers as CudaDevicePtr");
            return false;
        }

        // Copy ring frames → NvOF inputs (stride conversion)
        CUDA_MEMCPY2D cp = {};
        cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        cp.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cp.WidthInBytes = m_width * 4;
        cp.Height = m_height;
        cp.srcPitch = m_width * 4;

        cp.srcDevice = framePrev;
        cp.dstDevice = nvofInput0->getCudaDevicePtr();
        cp.dstPitch = nvofInput0->getStrideInfo().strideInfo[0].strideXInBytes;
        CUresult r0 = cuMemcpy2DAsync(&cp, m_cudaStream);

        cp.srcDevice = frameCurr;
        cp.dstDevice = nvofInput1->getCudaDevicePtr();
        cp.dstPitch = nvofInput1->getStrideInfo().strideInfo[0].strideXInBytes;
        CUresult r1 = cuMemcpy2DAsync(&cp, m_cudaStream);

        cuStreamSynchronize(m_cudaStream);

        static int flowDiagCount = 0;
        if (flowDiagCount < 5) {
            LOG("DIAG flow[%d]: ring→nvof0=%d, ring→nvof1=%d, "
                "nvof0 stride=%d, nvof1 stride=%d",
                flowDiagCount, r0, r1,
                (int)nvofInput0->getStrideInfo().strideInfo[0].strideXInBytes,
                (int)nvofInput1->getStrideInfo().strideInfo[0].strideXInBytes);
            flowDiagCount++;
        }

        m_nvOF->Execute(
            m_inputBuffers[0].get(),
            m_inputBuffers[1].get(),
            m_outputBuffers[0].get()
        );

        return true;
    }
    catch (const std::exception& e) {
        LogNvOFError("ComputeOpticalFlow", e);
        return false;
    }
}

bool FrucCaptureMode::InterpolateFrame(float weight, CUdeviceptr framePrev, CUdeviceptr frameCurr) {
    if (!m_nvofInitialized) return false;

    try {
        NvOFBufferCudaDevicePtr* flowBuffer = dynamic_cast<NvOFBufferCudaDevicePtr*>(m_outputBuffers[0].get());
        if (!flowBuffer) {
            LOGERR("Failed to get flow buffer as CudaDevicePtr");
            return false;
        }

        CUdeviceptr flowPtr = flowBuffer->getCudaDevicePtr();
        int flowStrideBytes = (int)flowBuffer->getStrideInfo().strideInfo[0].strideXInBytes;

        static bool loggedStride = false;
        if (!loggedStride) {
            loggedStride = true;
            int assumedStride = m_flowWidth * sizeof(int16_t) * 2;
            LOG("DIAG flow buffer stride: actual=%d bytes, assumed(tight)=%d bytes, mismatch=%d bytes/row",
                flowStrideBytes, assumedStride, flowStrideBytes - assumedStride);
        }

        cuCtxSetCurrent(m_cuContext);

        launchInterpolateKernel(
            (const uint8_t*)framePrev,
            (const uint8_t*)frameCurr,
            (const uint8_t*)flowPtr,
            (uint8_t*)m_cudaOutputFrame,
            m_width, m_height,
            m_flowWidth, m_flowHeight,
            flowStrideBytes, m_gridSize,
            weight, m_cudaStream
        );

        static bool checkedLaunch = false;
        if (!checkedLaunch) {
            checkedLaunch = true;
            cuStreamSynchronize(m_cudaStream);
            cudaError_t launchErr = cudaGetLastError();
            LOG("DIAG kernel launch: cudaGetLastError=%d (%s)", launchErr, cudaGetErrorString(launchErr));
        }

        return true;
    }
    catch (const std::exception& e) {
        LogNvOFError("InterpolateFrame", e);
        return false;
    }
}

bool FrucCaptureMode::PresentFromGPU(IDirect3DDevice9Ex* device, LARGE_INTEGER* pPresentExStart) {
    // GPU work: CUDA output → D3D9 surface → backbuffer
    CUresult result = cuGraphicsMapResources(1, &m_cudaOutputResource, m_cudaStream);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuGraphicsMapResources(output)", result);
        return false;
    }

    CUarray outputArray;
    result = cuGraphicsSubResourceGetMappedArray(&outputArray, m_cudaOutputResource, 0, 0);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuGraphicsSubResourceGetMappedArray(output)", result);
        cuGraphicsUnmapResources(1, &m_cudaOutputResource, m_cudaStream);
        return false;
    }

    CUDA_MEMCPY2D cp = {};
    cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cp.srcDevice = m_cudaOutputFrame;
    cp.srcPitch = m_width * 4;
    cp.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    cp.dstArray = outputArray;
    cp.WidthInBytes = m_width * 4;
    cp.Height = m_height;
    cuMemcpy2DAsync(&cp, m_cudaStream);
    cuStreamSynchronize(m_cudaStream);
    cuGraphicsUnmapResources(1, &m_cudaOutputResource, m_cudaStream);

    device->StretchRect(m_outputSurface, NULL, g_backbuffer, NULL, D3DTEXF_NONE);

    // Record split point: everything above is "work", PresentEx below is "vsync"
    QueryPerformanceCounter(pPresentExStart);

    device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_ONE);
    return true;
}

// =============================================================================
// Main loop: VSync-driven present + atomics-only bracket selection + flow + interp
// V4: No pause mechanism — capture thread is pure CUDA, no D3D9 contention.
// Present thread reads ring metadata via atomics (writeIndex is seq_cst,
// timestamps written before writeIndex advance).
// =============================================================================
void FrucCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    LOG("FrucCaptureMode::Run() - V4 NvFBCCuda, no pause, atomics-only bracket selection");
    LOG("Presentation: %s (PresentationInterval=%d, INTERVAL_ONE=%d)",
        m_isVsyncMode ? "VSync" : "Timed",
        GetPresentationInterval(), D3DPRESENT_INTERVAL_ONE);

    // NvFBCCuda captures at source display native resolution (no SOURCEMODE_SCALE).
    // Do a probe grab to determine the actual capture resolution before allocating.
    {
        NvU32 maxBufSize = 0;
        m_nvfbcCuda->NvFBCCudaGetMaxBufferSize(&maxBufSize);
        LOG("NvFBCCuda max buffer size: %u bytes", maxBufSize);

        CUdeviceptr probeBuf = 0;
        CUresult allocRes = cuMemAlloc(&probeBuf, maxBufSize);
        if (allocRes != CUDA_SUCCESS) {
            LogCudaError("cuMemAlloc(probeBuf)", allocRes);
            return;
        }

        NVFBC_CUDA_GRAB_FRAME_PARAMS probeParams = {};
        probeParams.dwVersion = NVFBC_CUDA_GRAB_FRAME_PARAMS_VER;
        probeParams.dwFlags = NVFBC_TOCUDA_NOFLAGS;  // blocking wait for first frame
        probeParams.pCUDADeviceBuffer = (void*)probeBuf;
        NvFBCFrameGrabInfo probeInfo = {};
        probeParams.pNvFBCFrameGrabInfo = &probeInfo;

        NVFBCRESULT res = m_nvfbcCuda->NvFBCCudaGrabFrame(&probeParams);
        cuMemFree(probeBuf);

        if (res != NVFBC_SUCCESS) {
            LOGERR("NvFBCCuda probe grab failed (result: 0x%X)", res);
            return;
        }

        m_width = probeInfo.dwWidth;
        m_height = probeInfo.dwHeight;
        LOG("NvFBCCuda capture resolution: %dx%d (bufWidth=%d, HDR=%d)",
            m_width, m_height, probeInfo.dwBufferWidth, probeInfo.bIsHDR);
        LOG("Output backbuffer resolution: %dx%d (StretchRect will scale)", BUF_WIDTH, BUF_HEIGHT);

        // Check stride on probe result
        if (probeInfo.dwBufferWidth != (DWORD)m_width) {
            m_strideChecked = true;
            m_needsStrideCopy = true;
            m_grabStride = probeInfo.dwBufferWidth * 4;
            LOG("NvFBCCuda stride padding detected: dwWidth=%d, dwBufferWidth=%d", m_width, probeInfo.dwBufferWidth);
        } else {
            m_strideChecked = true;
            m_needsStrideCopy = false;
            LOG("NvFBCCuda stride OK: dwWidth=%d == dwBufferWidth=%d — zero-copy", m_width, probeInfo.dwBufferWidth);
        }
    }

    if (!InitNvOF()) { LOGERR("Failed to initialize NvOF"); return; }
    if (!AllocateBuffers()) { LOGERR("Failed to allocate buffers"); return; }

    // ===== Configure grab params =====
    memset(&m_cudaGrabParams, 0, sizeof(m_cudaGrabParams));
    m_cudaGrabParams.dwVersion = NVFBC_CUDA_GRAB_FRAME_PARAMS_VER;
    m_cudaGrabParams.dwFlags = NVFBC_TOCUDA_WAIT_WITH_TIMEOUT;
    m_cudaGrabParams.dwWaitTime = 1;  // Short timeout to minimize D3D9 lock hold time
    m_cudaGrabParams.pNvFBCFrameGrabInfo = &m_grabInfo;
    // pCUDADeviceBuffer set per-grab in CaptureThreadProc

    // ===== Launch capture thread =====
    m_captureRunning.store(true);
    m_sessionInvalidated.store(false);
    m_captureGrabCount.store(0);
    m_ringWriteIndex.store(0);
    m_capturedFrameCount.store(0);

    m_captureThread = CreateThread(NULL, 0, CaptureThreadProc, this, 0, NULL);
    if (!m_captureThread) {
        LOGERR("Failed to create capture thread");
        return;
    }
    LOG("Capture thread launched (V4: NvFBCCuda, no D3D9, no pause)");

    // ===== Main present/interpolation loop =====
    MSG msg = {};
    bool hasOutputFrame = false;
    bool flowComputedForCurrentPair = false;
    int presentCount = 0;
    int framesSinceLog = 0;
    const int LOG_INTERVAL = 300;

    // Timing
    double accumSelectUs = 0;
    double accumFlowUs = 0, accumInterpUs = 0;
    double accumPresentWorkUs = 0, accumPresentVsyncUs = 0;
    int timingSelectCount = 0, timingPresentCount = 0, flowComputeCount = 0;
    double usPerTick = 1000000.0 / (double)m_perfFreq.QuadPart;

    // Per-iteration phase times (for stall breakdown)
    double iterSelectMs = 0, iterFlowMs = 0, iterInterpMs = 0;
    double iterPresentWorkMs = 0, iterPresentVsyncMs = 0;

    // Weight statistics
    double accumWeight = 0;
    float minWeight = 1.0f, maxWeight = 0.0f;

    // Frame pair delta
    double accumPairDeltaMs = 0;
    double minPairDeltaMs = 1e9, maxPairDeltaMs = 0;

    // VBlank time tracking
    LARGE_INTEGER lastVBlankTime = {};
    LARGE_INTEGER prevVBlankTime = {};
    bool hasVBlankTime = false;
    bool hasPrevVBlankTime = false;

    // Capture rate tracking
    LARGE_INTEGER lastCaptureRateTime = {};
    QueryPerformanceCounter(&lastCaptureRateTime);

    // Stall detection
    LARGE_INTEGER lastFrameTime = {};
    bool hasLastFrameTime = false;
    int stallCount = 0;
    const double STALL_THRESHOLD_MS = 20.0;

    // Bracket pair selection state
    int bracketPrevSlot = -1, bracketNextSlot = -1;
    LONGLONG cachedT0 = 0, cachedT1 = 0;
    LONGLONG lastCachedT0 = 0, lastCachedT1 = 0;
    bool pairValid = false;
    int lastCheckedFrameCount = 0;

    // Bracket selection fallback tracking
    int bracketHitCount = 0, bracketFallbackCount = 0;

    LOG("Entering V4 main loop (target=prevVBlankTime, ring=%d, no pause)", RING_SIZE);

    while (TRUE) {
        LARGE_INTEGER tStart, tEnd;

        // === PRESENT (VSync-driven, blocks until VBlank) ===
        if (hasOutputFrame) {
            QueryPerformanceCounter(&tStart);
            LARGE_INTEGER tPresentExStart;
            PresentFromGPU(device, &tPresentExStart);

            // Track prevVBlankTime BEFORE overwriting lastVBlankTime
            prevVBlankTime = lastVBlankTime;
            QueryPerformanceCounter(&lastVBlankTime);
            if (hasVBlankTime) hasPrevVBlankTime = true;
            hasVBlankTime = true;

            iterPresentWorkMs = (double)(tPresentExStart.QuadPart - tStart.QuadPart) * usPerTick / 1000.0;
            iterPresentVsyncMs = (double)(lastVBlankTime.QuadPart - tPresentExStart.QuadPart) * usPerTick / 1000.0;
            accumPresentWorkUs += iterPresentWorkMs * 1000.0;
            accumPresentVsyncUs += iterPresentVsyncMs * 1000.0;

            // Stall detection with phase breakdown
            if (hasLastFrameTime) {
                double intervalMs = (double)(lastVBlankTime.QuadPart - lastFrameTime.QuadPart) * usPerTick / 1000.0;
                if (intervalMs > STALL_THRESHOLD_MS) {
                    LOG("STALL[%d] #%d: %.1fms (select=%.1f flow=%.1f interp=%.1f work=%.1f vsync=%.1f)",
                        presentCount, stallCount, intervalMs,
                        iterSelectMs, iterFlowMs, iterInterpMs,
                        iterPresentWorkMs, iterPresentVsyncMs);
                    stallCount++;
                }
            }
            lastFrameTime = lastVBlankTime;
            hasLastFrameTime = true;

            // Reset per-iteration times for next cycle
            iterSelectMs = 0;
            iterFlowMs = 0;
            iterInterpMs = 0;

            presentCount++;
            timingPresentCount++;
            framesSinceLog++;
        }

        // Check for fatal error from capture thread
        if (m_sessionInvalidated.load()) {
            LOGERR("NvFBC session invalidated");
            break;
        }

        // === BRACKET PAIR SELECTION: read ring metadata via atomics (no pause) ===
        // Safety: capture writes timestamp+valid BEFORE advancing writeIndex (seq_cst).
        // Present reads writeIndex first, then timestamps of completed slots.
        // Ring size 6 provides 4+ slot overwrite margin (16.7ms at 240fps >> 1ms flow+interp).
        int currentFrameCount = m_capturedFrameCount.load();
        bool newFrameAvailable = (currentFrameCount > lastCheckedFrameCount);

        if (currentFrameCount >= 2 && newFrameAvailable) {
            QueryPerformanceCounter(&tStart);

            // Read ring writeIndex (atomic, seq_cst)
            int wi = m_ringWriteIndex.load();

            // Bracket pair selection: find the pair that straddles prevVBlankTime
            int newPrevSlot = -1, newNextSlot = -1;

            if (hasPrevVBlankTime) {
                LONGLONG target = prevVBlankTime.QuadPart;

                // Walk completed slots from newest to oldest
                int completedCount = currentFrameCount < (RING_SIZE - 1) ? currentFrameCount : (RING_SIZE - 1);
                for (int i = 1; i <= completedCount; i++) {
                    int slot = (wi + RING_SIZE - i) % RING_SIZE;
                    if (!m_frameHistory[slot].valid) break;

                    if (m_frameHistory[slot].timestamp.QuadPart <= target) {
                        newPrevSlot = slot;
                        if (i > 1) {
                            newNextSlot = (wi + RING_SIZE - (i - 1)) % RING_SIZE;
                        }
                        break;
                    }
                }
            }

            // Fallback: use two newest (startup, or source <= output Hz)
            if (newPrevSlot < 0 || newNextSlot < 0) {
                newPrevSlot = (wi + RING_SIZE - 2) % RING_SIZE;
                newNextSlot = (wi + RING_SIZE - 1) % RING_SIZE;
                bracketFallbackCount++;
            } else {
                bracketHitCount++;
            }

            // Cache timestamps from selected pair
            cachedT0 = m_frameHistory[newPrevSlot].timestamp.QuadPart;
            cachedT1 = m_frameHistory[newNextSlot].timestamp.QuadPart;

            QueryPerformanceCounter(&tEnd);
            iterSelectMs = (double)(tEnd.QuadPart - tStart.QuadPart) * usPerTick / 1000.0;
            accumSelectUs += iterSelectMs * 1000.0;
            timingSelectCount++;

            static int selectDiagCount = 0;
            if (selectDiagCount < 10) {
                double selectMs = (double)(tEnd.QuadPart - tStart.QuadPart) * usPerTick / 1000.0;
                double t0AgoMs = hasVBlankTime ? (double)(lastVBlankTime.QuadPart - cachedT0) * usPerTick / 1000.0 : 0;
                double t1AgoMs = hasVBlankTime ? (double)(lastVBlankTime.QuadPart - cachedT1) * usPerTick / 1000.0 : 0;
                LOG("DIAG select[%d]: wi=%d, prev=slot%d(%.1fms ago), next=slot%d(%.1fms ago), "
                    "total=%.3fms, bracket=%s",
                    selectDiagCount, wi, newPrevSlot, t0AgoMs, newNextSlot, t1AgoMs,
                    selectMs,
                    (newPrevSlot >= 0 && newNextSlot >= 0 && hasPrevVBlankTime &&
                     cachedT0 <= prevVBlankTime.QuadPart && cachedT1 > prevVBlankTime.QuadPart) ? "YES" : "fallback");
                selectDiagCount++;
            }

            // Check if pair actually changed (avoid redundant flow computation)
            if (cachedT0 != lastCachedT0 || cachedT1 != lastCachedT1) {
                bracketPrevSlot = newPrevSlot;
                bracketNextSlot = newNextSlot;
                lastCachedT0 = cachedT0;
                lastCachedT1 = cachedT1;
                flowComputedForCurrentPair = false;
                pairValid = true;
            }

            lastCheckedFrameCount = currentFrameCount;
        }

        // === FLOW + INTERPOLATION ===
        if (pairValid) {
            // Compute flow once per new frame pair
            if (!flowComputedForCurrentPair) {
                QueryPerformanceCounter(&tStart);
                if (!ComputeOpticalFlow(m_cudaFrames[bracketPrevSlot], m_cudaFrames[bracketNextSlot])) {
                    LOGERR("Optical flow computation failed");
                    break;
                }
                QueryPerformanceCounter(&tEnd);
                iterFlowMs = (double)(tEnd.QuadPart - tStart.QuadPart) * usPerTick / 1000.0;
                accumFlowUs += iterFlowMs * 1000.0;
                flowComputeCount++;
                flowComputedForCurrentPair = true;
            }

            // Weight from prevVBlankTime relative to bracket pair timestamps
            float weight = 0.5f;
            if (hasPrevVBlankTime && cachedT1 > cachedT0) {
                double rawWeight = (double)(prevVBlankTime.QuadPart - cachedT0) / (double)(cachedT1 - cachedT0);
                weight = (float)(rawWeight < 0.0 ? 0.0 : (rawWeight > 1.0 ? 1.0 : rawWeight));
            }

            LARGE_INTEGER tInterp;
            QueryPerformanceCounter(&tInterp);
            if (InterpolateFrame(weight, m_cudaFrames[bracketPrevSlot], m_cudaFrames[bracketNextSlot])) {
                QueryPerformanceCounter(&tEnd);
                iterInterpMs = (double)(tEnd.QuadPart - tInterp.QuadPart) * usPerTick / 1000.0;
                accumInterpUs += iterInterpMs * 1000.0;
                hasOutputFrame = true;

                // Pair delta
                double pairDeltaMs = (double)(cachedT1 - cachedT0) * usPerTick / 1000.0;
                accumPairDeltaMs += pairDeltaMs;
                if (pairDeltaMs < minPairDeltaMs) minPairDeltaMs = pairDeltaMs;
                if (pairDeltaMs > maxPairDeltaMs) maxPairDeltaMs = pairDeltaMs;

                // Weight stats
                accumWeight += weight;
                if (weight < minWeight) minWeight = weight;
                if (weight > maxWeight) maxWeight = weight;

                if (presentCount < 10) {
                    LOG("Present[%d]: weight=%.3f, pair_delta=%.1fms, bracket=%s",
                        presentCount, weight, pairDeltaMs,
                        hasPrevVBlankTime ? "prevVBlank" : "fallback");
                }

                if (framesSinceLog >= LOG_INTERVAL) {
                    double np = (double)timingPresentCount;
                    double ns = (double)timingSelectCount;

                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    int grabs = m_captureGrabCount.exchange(0);
                    double elapsedSec = (double)(now.QuadPart - lastCaptureRateTime.QuadPart) / (double)m_perfFreq.QuadPart;
                    double captureRate = (elapsedSec > 0) ? grabs / elapsedSec : 0;
                    lastCaptureRateTime = now;

                    double flowPerCompute = flowComputeCount > 0 ? accumFlowUs / flowComputeCount / 1000.0 : 0;

                    LOG("TIMING(ms): flow=%.2f(%d) interp=%.2f work=%.2f vsync=%.2f "
                        "| select=%.3f (%d selects) "
                        "| weight avg=%.3f min=%.3f max=%.3f "
                        "| pair_delta avg=%.1f min=%.1f max=%.1f "
                        "| %d presents, %d stalls | capture_rate=%.1f/s "
                        "| bracket_hit=%d fallback=%d",
                        flowPerCompute, flowComputeCount,
                        accumInterpUs / np / 1000.0,
                        accumPresentWorkUs / np / 1000.0,
                        accumPresentVsyncUs / np / 1000.0,
                        ns > 0 ? accumSelectUs / ns / 1000.0 : 0,
                        timingSelectCount,
                        accumWeight / np, minWeight, maxWeight,
                        accumPairDeltaMs / np, minPairDeltaMs, maxPairDeltaMs,
                        timingPresentCount, stallCount,
                        captureRate,
                        bracketHitCount, bracketFallbackCount);

                    accumSelectUs = 0;
                    accumFlowUs = accumInterpUs = 0;
                    accumPresentWorkUs = accumPresentVsyncUs = 0;
                    timingSelectCount = timingPresentCount = 0;
                    flowComputeCount = 0;
                    accumWeight = 0;
                    minWeight = 1.0f; maxWeight = 0.0f;
                    accumPairDeltaMs = 0;
                    minPairDeltaMs = 1e9; maxPairDeltaMs = 0;
                    stallCount = 0;
                    bracketHitCount = 0;
                    bracketFallbackCount = 0;
                    framesSinceLog = 0;
                }
            }
        }

        // Process Windows messages
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (msg.message == WM_QUIT) break;
    }

    // === SHUTDOWN ===
    LOG("Shutting down capture thread...");
    m_captureRunning.store(false);
    if (m_captureThread) {
        DWORD waitResult = WaitForSingleObject(m_captureThread, 5000);
        if (waitResult == WAIT_TIMEOUT) {
            LOGERR("Capture thread did not exit within 5s");
        }
        CloseHandle(m_captureThread);
        m_captureThread = NULL;
    }

    LOG("FrucCaptureMode::Run() exiting - captured %d frames, presented %d",
        m_capturedFrameCount.load(), presentCount);
}

const char* FrucCaptureMode::GetModeName() const {
    static char modeName[64];
    sprintf_s(modeName, sizeof(modeName), "OptFlow-NvFBCCuda-%.2f", m_targetFramerate);
    return modeName;
}

void FrucCaptureMode::LogCudaError(const char* operation, CUresult result) {
    const char* errorName = nullptr;
    const char* errorString = nullptr;
    cuGetErrorName(result, &errorName);
    cuGetErrorString(result, &errorString);
    LOGERR("CUDA error in %s: %s (%s)", operation,
           errorName ? errorName : "unknown",
           errorString ? errorString : "no description");
}

void FrucCaptureMode::LogNvOFError(const char* operation, const std::exception& e) {
    LOGERR("NvOF error in %s: %s", operation, e.what());
}
