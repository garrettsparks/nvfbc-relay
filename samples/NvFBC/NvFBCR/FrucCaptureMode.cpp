#include "FrucCaptureMode.h"
#include <SimpleLogger.h>

// Forward declaration of CUDA kernel (implemented in InterpolateKernel.cu)
extern "C" void launchInterpolateKernel(
    const uint8_t* frame0,
    const uint8_t* frame1,
    const int16_t* flowVectors,
    uint8_t* output,
    int width,
    int height,
    int flowWidth,
    int flowHeight,
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
    , m_nvofInitialized(false)
    , m_gridSize(4)  // 4x4 pixel blocks per motion vector
    , m_currentHistoryIndex(0)
    , m_capturedFrameCount(0)
    , m_cudaFrame0(0)
    , m_cudaFrame1(0)
    , m_cudaOutputFrame(0)
    , m_hostOutputBuffer(nullptr)
    , m_outputSurface(nullptr)
    , m_device(nullptr)
    , m_captureTarget(nullptr)
{
    LOG("=== FrucCaptureMode Phase 4 (NvOFA) ===");
    LOG("Target framerate: %.2f fps", m_targetFramerate);
    LOG("VSync mode: %s", m_isVsyncMode ? "yes" : "no");
    LOG("Flow grid size: %d", m_gridSize);

    // Initialize frame history
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        m_frameHistory[i].d3dSurface = nullptr;
        m_frameHistory[i].hostBuffer = nullptr;
        m_frameHistory[i].hostBufferSize = 0;
        m_frameHistory[i].timestamp.QuadPart = 0;
        m_frameHistory[i].valid = false;
    }

    // Get performance counter frequency
    QueryPerformanceFrequency(&m_perfFreq);
    m_lastPresentTime.QuadPart = 0;
}

FrucCaptureMode::~FrucCaptureMode() {
    LOG("=== FrucCaptureMode cleanup ===");
    Cleanup();
}

void FrucCaptureMode::Cleanup() {
    // Release NvOF resources (must be done before CUDA context destruction)
    m_outputBuffers.clear();
    m_inputBuffers.clear();
    m_nvOF.reset();
    m_nvofInitialized = false;

    // Free CUDA device memory
    if (m_cudaFrame0) {
        cuMemFree(m_cudaFrame0);
        m_cudaFrame0 = 0;
    }
    if (m_cudaFrame1) {
        cuMemFree(m_cudaFrame1);
        m_cudaFrame1 = 0;
    }
    if (m_cudaOutputFrame) {
        cuMemFree(m_cudaOutputFrame);
        m_cudaOutputFrame = 0;
    }

    // Free host buffers and release D3D9 surfaces
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        if (m_frameHistory[i].hostBuffer) {
            delete[] m_frameHistory[i].hostBuffer;
            m_frameHistory[i].hostBuffer = nullptr;
        }
        if (m_frameHistory[i].d3dSurface) {
            m_frameHistory[i].d3dSurface->Release();
            m_frameHistory[i].d3dSurface = nullptr;
        }
    }

    if (m_hostOutputBuffer) {
        delete[] m_hostOutputBuffer;
        m_hostOutputBuffer = nullptr;
    }

    // Release D3D9 surfaces
    if (m_outputSurface) {
        m_outputSurface->Release();
        m_outputSurface = nullptr;
    }
    if (m_captureTarget) {
        m_captureTarget->Release();
        m_captureTarget = nullptr;
    }

    // Destroy CUDA stream
    if (m_cudaStream) {
        cuStreamDestroy(m_cudaStream);
        m_cudaStream = nullptr;
    }

    // Release primary context
    if (m_cuContext) {
        cuDevicePrimaryCtxRelease(m_cuDevice);
        m_cuContext = nullptr;
    }

    m_cudaInitialized = false;
}

UINT FrucCaptureMode::GetPresentationInterval() const {
    return m_isVsyncMode ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
}

bool FrucCaptureMode::InitCuda() {
    LOG("Initializing CUDA...");

    // Initialize CUDA driver API
    CUresult result = cuInit(0);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuInit", result);
        return false;
    }

    // Get CUDA device count
    int deviceCount = 0;
    result = cuDeviceGetCount(&deviceCount);
    if (result != CUDA_SUCCESS || deviceCount == 0) {
        LogCudaError("cuDeviceGetCount", result);
        LOGERR("No CUDA devices found");
        return false;
    }

    // Get first CUDA device
    result = cuDeviceGet(&m_cuDevice, 0);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuDeviceGet", result);
        return false;
    }

    // Get device name and compute capability for logging
    char deviceName[256];
    cuDeviceGetName(deviceName, sizeof(deviceName), m_cuDevice);
    int ccMajor = 0, ccMinor = 0;
    cuDeviceGetAttribute(&ccMajor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, m_cuDevice);
    cuDeviceGetAttribute(&ccMinor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, m_cuDevice);
    LOG("CUDA device: %s (compute capability %d.%d, sm_%d%d)", deviceName, ccMajor, ccMinor, ccMajor, ccMinor);

    // Use the primary context so that <<<>>> kernel launches (runtime API) share the same context
    // as our driver API allocations and NvOF
    result = cuDevicePrimaryCtxRetain(&m_cuContext, m_cuDevice);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuDevicePrimaryCtxRetain", result);
        return false;
    }
    result = cuCtxSetCurrent(m_cuContext);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuCtxSetCurrent", result);
        return false;
    }

    // Create CUDA stream
    result = cuStreamCreate(&m_cudaStream, CU_STREAM_DEFAULT);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuStreamCreate", result);
        return false;
    }

    LOG("CUDA initialized successfully");
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
        // Create NvOF CUDA instance
        // Using ABGR8 format (8-bit per channel, what we'll convert to from ARGB10)
        // Using CUdeviceptr buffer type for direct GPU memory access
        m_nvOF = NvOFCuda::Create(
            m_cuContext,
            m_width,
            m_height,
            NV_OF_BUFFER_FORMAT_ABGR8,
            NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR,
            NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR,
            NV_OF_MODE_OPTICALFLOW,
            NV_OF_PERF_LEVEL_FAST,  // Use FAST for real-time
            m_cudaStream,
            m_cudaStream
        );

        // Check if requested grid size is supported
        uint32_t hwGridSize;
        if (!m_nvOF->CheckGridSize(m_gridSize)) {
            if (!m_nvOF->GetNextMinGridSize(m_gridSize, hwGridSize)) {
                LOGERR("No supported grid size found");
                return false;
            }
            LOG("Requested grid size %d not supported, using %d", m_gridSize, hwGridSize);
            m_gridSize = hwGridSize;
        }

        // Initialize NvOF
        m_nvOF->Init(m_gridSize);

        // Calculate flow dimensions
        m_flowWidth = (m_width + m_gridSize - 1) / m_gridSize;
        m_flowHeight = (m_height + m_gridSize - 1) / m_gridSize;
        LOG("Flow vector grid: %dx%d (grid size %d)", m_flowWidth, m_flowHeight, m_gridSize);

        // Create input buffers (2 for frame pairs)
        m_inputBuffers = m_nvOF->CreateBuffers(NV_OF_BUFFER_USAGE_INPUT, 2);
        LOG("Created %d NvOF input buffers", (int)m_inputBuffers.size());

        // Create output buffer (1 for flow vectors)
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
    LOG("Allocating buffers...");

    // Frame size in bytes (ARGB8 = 4 bytes per pixel)
    size_t frameSize = m_width * m_height * 4;

    // Create frame history surfaces and host buffers
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        // Create D3D9 surface for this history slot
        HRESULT hr = m_device->CreateOffscreenPlainSurface(
            m_width, m_height,
            D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT,
            &m_frameHistory[i].d3dSurface,
            NULL
        );
        if (FAILED(hr)) {
            LOGERR("Failed to create frame history surface %d (HRESULT: 0x%08x)", i, hr);
            return false;
        }

        // Allocate host staging buffer
        m_frameHistory[i].hostBuffer = new uint8_t[frameSize];
        m_frameHistory[i].hostBufferSize = frameSize;
        if (!m_frameHistory[i].hostBuffer) {
            LOGERR("Failed to allocate host buffer %d", i);
            return false;
        }
    }
    LOG("Allocated %d frame history surfaces with host staging buffers (%zu bytes each)", FRAME_HISTORY_SIZE, frameSize);

    // Allocate host output buffer
    m_hostOutputBuffer = new uint8_t[frameSize];
    if (!m_hostOutputBuffer) {
        LOGERR("Failed to allocate host output buffer");
        return false;
    }

    // Allocate CUDA device memory for frame copies (for warp kernel)
    CUresult result = cuMemAlloc(&m_cudaFrame0, frameSize);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuMemAlloc frame0", result);
        return false;
    }

    result = cuMemAlloc(&m_cudaFrame1, frameSize);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuMemAlloc frame1", result);
        return false;
    }

    result = cuMemAlloc(&m_cudaOutputFrame, frameSize);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuMemAlloc output", result);
        return false;
    }
    LOG("Allocated CUDA device memory for frames");

    // Create D3D9 output surface for final presentation
    HRESULT hr = m_device->CreateOffscreenPlainSurface(
        m_width, m_height,
        D3DFMT_A8R8G8B8,  // 8-bit ARGB for output
        D3DPOOL_DEFAULT,
        &m_outputSurface,
        NULL
    );
    if (FAILED(hr)) {
        LOGERR("Failed to create output surface (HRESULT: 0x%08x)", hr);
        return false;
    }
    LOG("Created D3D9 output surface");

    LOG("Buffer allocation complete");
    return true;
}

bool FrucCaptureMode::Setup() {
    LOG("FrucCaptureMode::Setup() - Phase 4 NvOFA implementation");

    // Store device pointer
    m_device = g_pD3D9Device;

    // Initialize CUDA (dimensions not needed yet)
    if (!InitCuda()) {
        LOGERR("Failed to initialize CUDA");
        return false;
    }

    LOG("FrucCaptureMode setup complete (NvOF and buffers will be initialized in Run)");
    return true;
}

bool FrucCaptureMode::CaptureFrame(NvFBCToDx9Vid* nvfbcDx9, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams) {
    // Capture frame from NvFBC (writes to m_captureTarget)
    NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);

    if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION) {
        LOGERR("NvFBC session invalidated during frame capture");
        return false;
    }

    if (fbcRes != NVFBC_SUCCESS) {
        // No new frame available - not an error, just skip
        return true;
    }

    // Get current frame history entry
    FrameHistoryEntry* entry = &m_frameHistory[m_currentHistoryIndex];

    // Copy from NvFBC capture target to current ring buffer slot
    RECT srcRect = { 0, 0, (LONG)m_width, (LONG)m_height };
    HRESULT hr = m_device->StretchRect(
        m_captureTarget,
        &srcRect,
        entry->d3dSurface,
        &srcRect,
        D3DTEXF_NONE
    );
    if (FAILED(hr)) {
        LOGERR("Failed to copy captured frame to ring buffer (HRESULT: 0x%08x)", hr);
        return false;
    }

    // Capture timestamp
    QueryPerformanceCounter(&entry->timestamp);

    // Mark entry as valid
    entry->valid = true;
    m_capturedFrameCount++;

    // Debug first few captures
    if (m_capturedFrameCount <= 5) {
        LOG("Captured frame %d to ring buffer slot %d", m_capturedFrameCount, m_currentHistoryIndex);
    }

    // Advance ring buffer index
    m_currentHistoryIndex = (m_currentHistoryIndex + 1) % FRAME_HISTORY_SIZE;

    return true;
}

bool FrucCaptureMode::CopyFrameToHost(int historyIndex) {
    FrameHistoryEntry* entry = &m_frameHistory[historyIndex];

    if (!entry->valid || !entry->d3dSurface || !entry->hostBuffer) {
        return false;
    }

    // Lock the D3D9 surface to get CPU access
    D3DLOCKED_RECT lockedRect;
    HRESULT hr = entry->d3dSurface->LockRect(&lockedRect, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        LOGERR("Failed to lock D3D9 surface (HRESULT: 0x%08x)", hr);
        return false;
    }

    // Copy data to host buffer
    // NvFBC is set to ARGB8 mode, NvOF expects ABGR8
    // D3D9 A8R8G8B8 is stored as BGRA in memory (little endian)
    // NvOF ABGR8 expects RGBA in memory
    // So we need to swap R and B channels
    uint8_t* dst = entry->hostBuffer;
    const uint8_t* src = (const uint8_t*)lockedRect.pBits;

    for (int y = 0; y < m_height; y++) {
        const uint8_t* srcRow = src + y * lockedRect.Pitch;
        uint8_t* dstRow = dst + y * m_width * 4;

        for (int x = 0; x < m_width; x++) {
            // D3D9 A8R8G8B8 in memory (little endian): B, G, R, A
            uint8_t b = srcRow[x * 4 + 0];
            uint8_t g = srcRow[x * 4 + 1];
            uint8_t r = srcRow[x * 4 + 2];
            uint8_t a = srcRow[x * 4 + 3];

            // NvOF ABGR8 in memory: R, G, B, A
            dstRow[x * 4 + 0] = r;
            dstRow[x * 4 + 1] = g;
            dstRow[x * 4 + 2] = b;
            dstRow[x * 4 + 3] = a;
        }
    }

    entry->d3dSurface->UnlockRect();

    // Diagnostic: check if captured frame has actual pixel data (first time only)
    static bool checkedInput = false;
    if (!checkedInput) {
        checkedInput = true;
        uint32_t nonZeroCount = 0;
        for (int i = 0; i < m_width * 4 && i < 256; i++) {
            if (dst[i] != 0) nonZeroCount++;
        }
        LOG("DIAG CopyFrameToHost: first 256 bytes, %d non-zero", nonZeroCount);
        LOG("DIAG CopyFrameToHost: first 16 bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
            dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], dst[6], dst[7],
            dst[8], dst[9], dst[10], dst[11], dst[12], dst[13], dst[14], dst[15]);
    }

    return true;
}

FrucCaptureMode::FrameHistoryEntry* FrucCaptureMode::GetFrame(int offset) {
    // offset 0 = most recent, 1 = previous
    int index = (m_currentHistoryIndex + FRAME_HISTORY_SIZE - 1 - offset) % FRAME_HISTORY_SIZE;
    FrameHistoryEntry* entry = &m_frameHistory[index];
    return entry->valid ? entry : nullptr;
}

bool FrucCaptureMode::ComputeOpticalFlow() {
    if (!m_nvofInitialized) {
        return false;
    }

    // Get the two most recent frames
    FrameHistoryEntry* frame0 = GetFrame(1);  // Previous frame
    FrameHistoryEntry* frame1 = GetFrame(0);  // Current frame

    if (!frame0 || !frame1) {
        return false;
    }

    try {
        // Upload frame data to NvOF input buffers
        m_inputBuffers[0]->UploadData(frame0->hostBuffer);
        m_inputBuffers[1]->UploadData(frame1->hostBuffer);

        // Restore our context before CUDA operations (NvOF UploadData may have changed it)
        CUresult ctxRes = cuCtxSetCurrent(m_cuContext);

        // Also copy to our CUDA buffers for the warp kernel
        size_t frameSize = m_width * m_height * 4;
        CUresult cpy0 = cuMemcpyHtoDAsync(m_cudaFrame0, frame0->hostBuffer, frameSize, m_cudaStream);
        CUresult cpy1 = cuMemcpyHtoDAsync(m_cudaFrame1, frame1->hostBuffer, frameSize, m_cudaStream);
        cuStreamSynchronize(m_cudaStream);

        // Diagnostic: check host buffers and GPU round-trip for first 5 calls
        static int flowDiagCount = 0;
        if (flowDiagCount < 5) {
            // Sample from middle of frame (not corner)
            int midOffset = (m_height / 2) * m_width * 4 + (m_width / 2) * 4;
            LOG("DIAG flow[%d]: cuCtxSet=%d cpy0=%d cpy1=%d | "
                "host frame0 mid: %02x %02x %02x %02x | "
                "host frame1 mid: %02x %02x %02x %02x | "
                "frame0==frame1 mid: %s",
                flowDiagCount, ctxRes, cpy0, cpy1,
                frame0->hostBuffer[midOffset], frame0->hostBuffer[midOffset+1],
                frame0->hostBuffer[midOffset+2], frame0->hostBuffer[midOffset+3],
                frame1->hostBuffer[midOffset], frame1->hostBuffer[midOffset+1],
                frame1->hostBuffer[midOffset+2], frame1->hostBuffer[midOffset+3],
                (memcmp(frame0->hostBuffer, frame1->hostBuffer, frameSize) == 0) ? "YES" : "NO");

            // GPU round-trip: read back mid-pixel from both GPU buffers
            uint8_t gpu0[4] = {}, gpu1[4] = {};
            cuMemcpyDtoH(gpu0, m_cudaFrame0 + midOffset, 4);
            cuMemcpyDtoH(gpu1, m_cudaFrame1 + midOffset, 4);
            LOG("DIAG flow[%d]: GPU frame0 mid: %02x %02x %02x %02x | GPU frame1 mid: %02x %02x %02x %02x",
                flowDiagCount,
                gpu0[0], gpu0[1], gpu0[2], gpu0[3],
                gpu1[0], gpu1[1], gpu1[2], gpu1[3]);

            flowDiagCount++;
        }

        // Execute optical flow
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

bool FrucCaptureMode::InterpolateFrame(float weight) {
    if (!m_nvofInitialized) {
        return false;
    }

    try {
        // Get flow vector buffer's device pointer
        NvOFBufferCudaDevicePtr* flowBuffer = dynamic_cast<NvOFBufferCudaDevicePtr*>(m_outputBuffers[0].get());
        if (!flowBuffer) {
            LOGERR("Failed to get flow buffer as CudaDevicePtr");
            return false;
        }

        CUdeviceptr flowPtr = flowBuffer->getCudaDevicePtr();

        // Restore our context before kernel launch (NvOF Execute may have changed it)
        cuCtxSetCurrent(m_cuContext);

        // Launch the interpolation kernel
        launchInterpolateKernel(
            (const uint8_t*)m_cudaFrame0,
            (const uint8_t*)m_cudaFrame1,
            (const int16_t*)flowPtr,
            (uint8_t*)m_cudaOutputFrame,
            m_width,
            m_height,
            m_flowWidth,
            m_flowHeight,
            m_gridSize,
            weight,
            m_cudaStream
        );

        // Diagnostic: check kernel launch error (first time only)
        static bool checkedLaunch = false;
        if (!checkedLaunch) {
            checkedLaunch = true;
            cudaError_t launchErr = cudaGetLastError();
            LOG("DIAG kernel launch: cudaGetLastError=%d (%s)", launchErr, cudaGetErrorString(launchErr));
        }

        // Download result to host
        size_t frameSize = m_width * m_height * 4;
        cuMemcpyDtoHAsync(m_hostOutputBuffer, m_cudaOutputFrame, frameSize, m_cudaStream);

        // Synchronize to ensure download is complete
        cuStreamSynchronize(m_cudaStream);

        return true;
    }
    catch (const std::exception& e) {
        LogNvOFError("InterpolateFrame", e);
        return false;
    }
}

void FrucCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    LOG("FrucCaptureMode::Run() - Phase 4: NvOFA optical flow interpolation");
    LOG("Presentation: %s", m_isVsyncMode ? "VSync" : "Timed");

    // Get dimensions from globals (now initialized)
    m_width = BUF_WIDTH;
    m_height = BUF_HEIGHT;
    LOG("Frame buffer dimensions: %dx%d", m_width, m_height);

    // Create single NvFBC capture target surface (where NvFBC always writes)
    HRESULT hr = device->CreateOffscreenPlainSurface(
        m_width, m_height,
        D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT,
        &m_captureTarget,
        NULL
    );
    if (FAILED(hr)) {
        LOGERR("Failed to create NvFBC capture target surface (HRESULT: 0x%08x)", hr);
        return;
    }
    LOG("Created NvFBC capture target surface");

    // Configure NvFBC with single buffer (following FrameTemporalCaptureMode pattern)
    NVFBC_TODX9VID_OUT_BUF outBuf[1];
    outBuf[0].pPrimary = m_captureTarget;
    outBuf[0].pSecondary = nullptr;

    NVFBC_TODX9VID_SETUP_PARAMS setupParams = {};
    setupParams.dwVersion = NVFBC_TODX9VID_SETUP_PARAMS_V3_VER;
    setupParams.bWithHWCursor = 1;
    setupParams.bStereoGrab = 0;
    setupParams.bDiffMap = 0;
    setupParams.bClassificationMap = 0;
    setupParams.ppBuffer = outBuf;
    setupParams.ppDiffMap = nullptr;
    setupParams.ppClassificationMap = nullptr;
    setupParams.eMode = NVFBC_TODX9VID_ARGB;
    setupParams.dwNumBuffers = 1;  // Single buffer - we manage ring buffer manually
    setupParams.bHDRRequest = FALSE;

    NVFBCRESULT setupResult = nvfbcDx9->NvFBCToDx9VidSetUp(&setupParams);
    if (NVFBC_SUCCESS != setupResult) {
        LOGERR("Failed to configure NvFBC (error code: 0x%08x)", setupResult);
        return;
    }
    LOG("NvFBC configured with single capture buffer (manual ring buffer management)");

    // Initialize NvOF now that we have dimensions
    if (!InitNvOF()) {
        LOGERR("Failed to initialize NvOF");
        return;
    }

    // Allocate buffers
    if (!AllocateBuffers()) {
        LOGERR("Failed to allocate buffers");
        return;
    }

    // ===== Main capture/interpolation loop =====
    MSG msg;
    LARGE_INTEGER targetPresentTime;
    LONGLONG targetFrameTime = (LONGLONG)(m_perfFreq.QuadPart / m_targetFramerate);

    bool targetInitialized = false;
    bool flowComputedForCurrentPair = false;

    int framesSinceLog = 0;
    const int LOG_INTERVAL = 300;  // Log every 300 frames (5 seconds at 60fps)

    int presentCount = 0;

    while (TRUE) {
        // Capture frame to ring buffer
        int capturedBefore = m_capturedFrameCount;
        if (!CaptureFrame(nvfbcDx9, grabParams)) {
            LOGERR("Fatal error during frame capture");
            break;
        }
        bool newFrame = (m_capturedFrameCount > capturedBefore);

        if (newFrame) {
            // Copy newly captured frame to host buffer (for NvOF upload)
            int prevIndex = (m_currentHistoryIndex + FRAME_HISTORY_SIZE - 1) % FRAME_HISTORY_SIZE;
            CopyFrameToHost(prevIndex);

            // Get frame pair: frame0 = previous, frame1 = just captured
            FrameHistoryEntry* frame0 = GetFrame(1);
            FrameHistoryEntry* frame1 = GetFrame(0);

            if (frame0 && frame1 && frame0->valid && frame1->valid) {
                LONGLONG t0 = frame0->timestamp.QuadPart;
                LONGLONG t1 = frame1->timestamp.QuadPart;

                // Initialize target time from first frame pair
                if (!targetInitialized) {
                    // First target is one output frame period after frame0
                    targetPresentTime.QuadPart = t0 + targetFrameTime;
                    targetInitialized = true;
                    LOG("Target initialized: first target at %.2fms after first capture",
                        (double)targetFrameTime * 1000.0 / m_perfFreq.QuadPart);
                }

                // New frame pair means flow needs recomputation
                flowComputedForCurrentPair = false;

                // Present all target times that fall between frame0 and frame1
                // We always stay behind the latest capture so we have both past and future frames
                while (targetPresentTime.QuadPart <= t1) {
                    // Skip any targets that fell before frame0 (catch-up after hiccup)
                    if (targetPresentTime.QuadPart < t0) {
                        targetPresentTime.QuadPart += targetFrameTime;
                        continue;
                    }

                    // Compute optical flow once per frame pair
                    if (!flowComputedForCurrentPair) {
                        if (!ComputeOpticalFlow()) {
                            break;
                        }
                        flowComputedForCurrentPair = true;
                    }

                    // Calculate interpolation weight
                    // weight = (target - t0) / (t1 - t0)
                    float weight = 0.5f;
                    if (t1 > t0) {
                        double rawWeight = (double)(targetPresentTime.QuadPart - t0) / (double)(t1 - t0);
                        weight = (float)(rawWeight < 0.0 ? 0.0 : (rawWeight > 1.0 ? 1.0 : rawWeight));
                    }

                    // Debug logging for first 120 presents (2 seconds at 60fps)
                    if (presentCount < 120) {
                        double t0Ms = (double)t0 * 1000.0 / m_perfFreq.QuadPart;
                        double t1Ms = (double)t1 * 1000.0 / m_perfFreq.QuadPart;
                        double tTargetMs = (double)targetPresentTime.QuadPart * 1000.0 / m_perfFreq.QuadPart;
                        LONGLONG actualPresentTime;
                        QueryPerformanceCounter((LARGE_INTEGER*)&actualPresentTime);
                        double actualMs = (double)actualPresentTime * 1000.0 / m_perfFreq.QuadPart;
                        double latencyMs = actualMs - tTargetMs;
                        LOG("Present[%d]: target=%.2fms, actual=%.2fms, latency=%.2fms, weight=%.3f, bracket=[%.2fms, %.2fms]",
                            presentCount, tTargetMs, actualMs, latencyMs, weight, t0Ms, t1Ms);
                    }

                    // Interpolate and present
                    if (InterpolateFrame(weight)) {
                        D3DLOCKED_RECT lockedRect;
                        HRESULT hr = m_outputSurface->LockRect(&lockedRect, NULL, 0);
                        if (SUCCEEDED(hr)) {
                            for (int y = 0; y < m_height; y++) {
                                memcpy(
                                    (uint8_t*)lockedRect.pBits + y * lockedRect.Pitch,
                                    m_hostOutputBuffer + y * m_width * 4,
                                    m_width * 4
                                );
                            }
                            m_outputSurface->UnlockRect();

                            HRESULT stretchHr = device->StretchRect(m_outputSurface, NULL, g_backbuffer, NULL, D3DTEXF_NONE);
                            HRESULT presentHr = device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);

                            // Diagnostics for first 5 presents: kernel output sample, StretchRect, PresentEx
                            if (presentCount < 5) {
                                // Sample pixel from middle of frame (not corner which might be static)
                                int midOffset = (m_height / 2) * m_width * 4 + (m_width / 2) * 4;
                                LOG("DIAG present[%d]: weight=%.3f, LockRect=0x%08x, StretchRect=0x%08x, PresentEx=0x%08x, "
                                    "mid-pixel: %02x %02x %02x %02x, corner-pixel: %02x %02x %02x %02x",
                                    presentCount, weight, hr, stretchHr, presentHr,
                                    m_hostOutputBuffer[midOffset], m_hostOutputBuffer[midOffset+1],
                                    m_hostOutputBuffer[midOffset+2], m_hostOutputBuffer[midOffset+3],
                                    m_hostOutputBuffer[0], m_hostOutputBuffer[1],
                                    m_hostOutputBuffer[2], m_hostOutputBuffer[3]);
                            }
                        } else {
                            if (presentCount < 5) {
                                LOG("DIAG present[%d]: LockRect FAILED 0x%08x", presentCount, hr);
                            }
                            device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);
                        }

                        presentCount++;
                        framesSinceLog++;
                        if (framesSinceLog >= LOG_INTERVAL) {
                            LOG("Phase 4 running - captured %d frames, presented %d interpolated frames",
                                m_capturedFrameCount, presentCount);
                            framesSinceLog = 0;
                        }
                    }

                    // Advance to next output target
                    targetPresentTime.QuadPart += targetFrameTime;
                }
            }
        }

        // Process Windows messages
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (msg.message == WM_QUIT)
            break;
    }

    LOG("FrucCaptureMode::Run() exiting - captured %d frames, presented %d interpolated",
        m_capturedFrameCount, presentCount);

    // Surfaces will be released in Cleanup()
}

const char* FrucCaptureMode::GetModeName() const {
    static char modeName[64];
    sprintf_s(modeName, sizeof(modeName), "OptFlow-Phase4-%.2f", m_targetFramerate);
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
