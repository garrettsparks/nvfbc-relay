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
    , m_interopCaptureSurface(nullptr)
    , m_cudaCaptureResource(nullptr)
    , m_cudaOutputResource(nullptr)
    , m_nvofInitialized(false)
    , m_gridSize(4)  // 4x4 pixel blocks per motion vector (max supported by NvOF)
    , m_currentHistoryIndex(0)
    , m_capturedFrameCount(0)
    , m_cudaFrame0(0)
    , m_cudaFrame1(0)
    , m_cudaOutputFrame(0)
    , m_outputSurface(nullptr)
    , m_device(nullptr)
    , m_captureTarget(nullptr)
{
    LOG("=== FrucCaptureMode GPU-Resident Pipeline ===");
    LOG("Target framerate: %.2f fps", m_targetFramerate);
    LOG("VSync mode: %s", m_isVsyncMode ? "yes" : "no");
    LOG("Flow grid size: %d", m_gridSize);

    // Initialize frame history
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
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

    // Unregister CUDA-D3D9 interop resources (before freeing CUDA memory)
    if (m_cudaCaptureResource) {
        cuGraphicsUnregisterResource(m_cudaCaptureResource);
        m_cudaCaptureResource = nullptr;
    }
    if (m_cudaOutputResource) {
        cuGraphicsUnregisterResource(m_cudaOutputResource);
        m_cudaOutputResource = nullptr;
    }

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

    // Release D3D9 surfaces
    if (m_outputSurface) {
        m_outputSurface->Release();
        m_outputSurface = nullptr;
    }
    if (m_interopCaptureSurface) {
        m_interopCaptureSurface->Release();
        m_interopCaptureSurface = nullptr;
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

    // Destroy D3D9 interop context
    if (m_cuContext) {
        cuCtxDestroy(m_cuContext);
        m_cuContext = nullptr;
    }

    m_cudaInitialized = false;
}

UINT FrucCaptureMode::GetPresentationInterval() const {
    // FRUC always uses VSync-driven presentation for hardware-timed cadence
    return D3DPRESENT_INTERVAL_ONE;
}

bool FrucCaptureMode::InitCuda() {
    LOG("Initializing CUDA with D3D9 interop...");

    // Initialize CUDA driver API
    CUresult result = cuInit(0);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuInit", result);
        return false;
    }

    // Create CUDA context with D3D9 interop enabled.
    // This auto-selects the CUDA device matching the D3D9 adapter and enables
    // cuGraphicsD3D9RegisterResource on this context.
    result = cuD3D9CtxCreate(&m_cuContext, &m_cuDevice, 0, m_device);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuD3D9CtxCreate", result);
        return false;
    }

    // Get device name and compute capability for logging
    char deviceName[256];
    cuDeviceGetName(deviceName, sizeof(deviceName), m_cuDevice);
    int ccMajor = 0, ccMinor = 0;
    cuDeviceGetAttribute(&ccMajor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, m_cuDevice);
    cuDeviceGetAttribute(&ccMinor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, m_cuDevice);
    LOG("CUDA device: %s (compute capability %d.%d, sm_%d%d)", deviceName, ccMajor, ccMinor, ccMajor, ccMinor);

    // Create CUDA stream
    result = cuStreamCreate(&m_cudaStream, CU_STREAM_DEFAULT);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuStreamCreate", result);
        return false;
    }

    LOG("CUDA initialized with D3D9 interop");
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
        // Declared as ABGR8 but fed BGRA data — NvOF only cares about consistent
        // byte order between frames for motion vector computation
        m_nvOF = NvOFCuda::Create(
            m_cuContext,
            m_width,
            m_height,
            NV_OF_BUFFER_FORMAT_ABGR8,
            NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR,
            NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR,
            NV_OF_MODE_OPTICALFLOW,
            NV_OF_PERF_LEVEL_FAST,
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
    LOG("Allocating GPU-resident buffers...");

    size_t frameSize = m_width * m_height * 4;

    // Allocate CUDA device memory ring buffer (2 slots for frame pairs)
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
    LOG("Allocated CUDA device memory: 2 ring slots + 1 output (%zu bytes each)", frameSize);

    // Create D3D9 output surface for final presentation
    HRESULT hr = m_device->CreateOffscreenPlainSurface(
        m_width, m_height,
        D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT,
        &m_outputSurface,
        NULL
    );
    if (FAILED(hr)) {
        LOGERR("Failed to create output surface (HRESULT: 0x%08x)", hr);
        return false;
    }

    // Create a separate interop surface for capture (NvFBC target can't be directly
    // mapped — NvFBC writes may not synchronize through D3D9's pipeline, so we
    // StretchRect to this surface first, which is a proper D3D9 call that syncs)
    hr = m_device->CreateOffscreenPlainSurface(
        m_width, m_height,
        D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT,
        &m_interopCaptureSurface,
        NULL
    );
    if (FAILED(hr)) {
        LOGERR("Failed to create interop capture surface (HRESULT: 0x%08x)", hr);
        return false;
    }

    // Register D3D9 surfaces for CUDA interop
    // D3D9 interop only supports NONE, SURFACE_LDST, TEXTURE_GATHER flags
    result = cuGraphicsD3D9RegisterResource(&m_cudaCaptureResource, m_interopCaptureSurface,
                                             CU_GRAPHICS_REGISTER_FLAGS_NONE);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuGraphicsD3D9RegisterResource(interopCaptureSurface)", result);
        return false;
    }
    LOG("Registered interop capture surface for CUDA");

    result = cuGraphicsD3D9RegisterResource(&m_cudaOutputResource, m_outputSurface,
                                             CU_GRAPHICS_REGISTER_FLAGS_NONE);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuGraphicsD3D9RegisterResource(outputSurface)", result);
        return false;
    }
    LOG("Registered output surface for CUDA interop");

    LOG("GPU-resident buffer allocation complete (zero host staging buffers)");
    return true;
}

bool FrucCaptureMode::Setup() {
    LOG("FrucCaptureMode::Setup() - GPU-resident pipeline");

    // Store device pointer (needed by InitCuda for cuD3D9CtxCreate)
    m_device = g_pD3D9Device;

    // Initialize CUDA with D3D9 interop
    if (!InitCuda()) {
        LOGERR("Failed to initialize CUDA with D3D9 interop");
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

    // StretchRect from NvFBC capture target to interop surface.
    // This is a D3D9 call that properly synchronizes with NvFBC's writes,
    // ensuring the data is visible when we map the interop surface for CUDA.
    RECT srcRect = { 0, 0, (LONG)m_width, (LONG)m_height };
    HRESULT hr = m_device->StretchRect(m_captureTarget, &srcRect,
                                        m_interopCaptureSurface, &srcRect, D3DTEXF_NONE);
    if (FAILED(hr)) {
        LOGERR("Failed to StretchRect capture→interop (HRESULT: 0x%08x)", hr);
        return false;
    }

    // Map interop surface for CUDA access
    CUresult result = cuGraphicsMapResources(1, &m_cudaCaptureResource, m_cudaStream);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuGraphicsMapResources(capture)", result);
        return false;
    }

    CUarray mappedArray;
    result = cuGraphicsSubResourceGetMappedArray(&mappedArray, m_cudaCaptureResource, 0, 0);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuGraphicsSubResourceGetMappedArray(capture)", result);
        cuGraphicsUnmapResources(1, &m_cudaCaptureResource, m_cudaStream);
        return false;
    }

    // Copy from mapped D3D9 array to CUDA ring buffer slot (GPU→GPU, no PCIe)
    CUdeviceptr targetFrame = (m_currentHistoryIndex == 0) ? m_cudaFrame0 : m_cudaFrame1;
    CUDA_MEMCPY2D cp = {};
    cp.srcMemoryType = CU_MEMORYTYPE_ARRAY;
    cp.srcArray = mappedArray;
    cp.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    cp.dstDevice = targetFrame;
    cp.dstPitch = m_width * 4;
    cp.WidthInBytes = m_width * 4;
    cp.Height = m_height;
    result = cuMemcpy2DAsync(&cp, m_cudaStream);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuMemcpy2DAsync(capture→ring)", result);
        cuGraphicsUnmapResources(1, &m_cudaCaptureResource, m_cudaStream);
        return false;
    }

    // Unmap: waits for async copy on m_cudaStream, then releases D3D9 access
    cuGraphicsUnmapResources(1, &m_cudaCaptureResource, m_cudaStream);

    // Get current frame history entry and record timestamp
    FrameHistoryEntry* entry = &m_frameHistory[m_currentHistoryIndex];
    QueryPerformanceCounter(&entry->timestamp);
    entry->valid = true;
    m_capturedFrameCount++;

    if (m_capturedFrameCount <= 5) {
        LOG("Captured frame %d to CUDA ring slot %d (GPU-resident)", m_capturedFrameCount, m_currentHistoryIndex);
    }

    // Advance ring buffer index
    m_currentHistoryIndex = (m_currentHistoryIndex + 1) % FRAME_HISTORY_SIZE;

    return true;
}

FrucCaptureMode::FrameHistoryEntry* FrucCaptureMode::GetFrame(int offset) {
    // offset 0 = most recent, 1 = previous
    int index = GetFrameRingIndex(offset);
    FrameHistoryEntry* entry = &m_frameHistory[index];
    return entry->valid ? entry : nullptr;
}

int FrucCaptureMode::GetFrameRingIndex(int offset) {
    return (m_currentHistoryIndex + FRAME_HISTORY_SIZE - 1 - offset) % FRAME_HISTORY_SIZE;
}

CUdeviceptr FrucCaptureMode::GetFrameCudaPtr(int offset) {
    int idx = GetFrameRingIndex(offset);
    return (idx == 0) ? m_cudaFrame0 : m_cudaFrame1;
}

bool FrucCaptureMode::ComputeOpticalFlow() {
    if (!m_nvofInitialized) {
        return false;
    }

    // Verify frame pair is valid
    FrameHistoryEntry* frame0 = GetFrame(1);
    FrameHistoryEntry* frame1 = GetFrame(0);
    if (!frame0 || !frame1) {
        return false;
    }

    try {
        // Ensure our context is current
        cuCtxSetCurrent(m_cuContext);

        // Get CUDA pointers for the current frame pair
        CUdeviceptr prevFrame = GetFrameCudaPtr(1);  // previous
        CUdeviceptr currFrame = GetFrameCudaPtr(0);  // current

        // Device-to-device copy to NvOF input buffers (respecting NvOF stride)
        NvOFBufferCudaDevicePtr* nvofInput0 = dynamic_cast<NvOFBufferCudaDevicePtr*>(m_inputBuffers[0].get());
        NvOFBufferCudaDevicePtr* nvofInput1 = dynamic_cast<NvOFBufferCudaDevicePtr*>(m_inputBuffers[1].get());
        if (!nvofInput0 || !nvofInput1) {
            LOGERR("Failed to get NvOF input buffers as CudaDevicePtr");
            return false;
        }

        CUDA_MEMCPY2D cp = {};
        cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        cp.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cp.WidthInBytes = m_width * 4;
        cp.Height = m_height;
        cp.srcPitch = m_width * 4;

        // Copy previous frame → NvOF input 0
        cp.srcDevice = prevFrame;
        cp.dstDevice = nvofInput0->getCudaDevicePtr();
        cp.dstPitch = nvofInput0->getStrideInfo().strideInfo[0].strideXInBytes;
        CUresult r0 = cuMemcpy2DAsync(&cp, m_cudaStream);

        // Copy current frame → NvOF input 1
        cp.srcDevice = currFrame;
        cp.dstDevice = nvofInput1->getCudaDevicePtr();
        cp.dstPitch = nvofInput1->getStrideInfo().strideInfo[0].strideXInBytes;
        CUresult r1 = cuMemcpy2DAsync(&cp, m_cudaStream);

        cuStreamSynchronize(m_cudaStream);

        static int flowDiagCount = 0;
        if (flowDiagCount < 5) {
            LOG("DIAG flow[%d]: D2D copy prev(slot %d)→nvof0=%d, curr(slot %d)→nvof1=%d, "
                "nvof0 stride=%d, nvof1 stride=%d",
                flowDiagCount,
                GetFrameRingIndex(1), r0,
                GetFrameRingIndex(0), r1,
                (int)nvofInput0->getStrideInfo().strideInfo[0].strideXInBytes,
                (int)nvofInput1->getStrideInfo().strideInfo[0].strideXInBytes);
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

bool FrucCaptureMode::InterpolateFrame(float weight, CUdeviceptr framePrev, CUdeviceptr frameCurr) {
    if (!m_nvofInitialized) {
        return false;
    }

    try {
        // Get flow vector buffer's device pointer and stride
        NvOFBufferCudaDevicePtr* flowBuffer = dynamic_cast<NvOFBufferCudaDevicePtr*>(m_outputBuffers[0].get());
        if (!flowBuffer) {
            LOGERR("Failed to get flow buffer as CudaDevicePtr");
            return false;
        }

        CUdeviceptr flowPtr = flowBuffer->getCudaDevicePtr();
        NV_OF_CUDA_BUFFER_STRIDE_INFO strideInfo = flowBuffer->getStrideInfo();
        int flowStrideBytes = (int)strideInfo.strideInfo[0].strideXInBytes;

        // Log stride info once
        static bool loggedStride = false;
        if (!loggedStride) {
            loggedStride = true;
            int assumedStride = m_flowWidth * sizeof(int16_t) * 2;
            LOG("DIAG flow buffer stride: actual=%d bytes, assumed(tight)=%d bytes, mismatch=%d bytes/row",
                flowStrideBytes, assumedStride, flowStrideBytes - assumedStride);
        }

        // Restore our context before kernel launch (NvOF Execute may have changed it)
        cuCtxSetCurrent(m_cuContext);

        // Launch the interpolation kernel (all data GPU-resident)
        launchInterpolateKernel(
            (const uint8_t*)framePrev,
            (const uint8_t*)frameCurr,
            (const uint8_t*)flowPtr,
            (uint8_t*)m_cudaOutputFrame,
            m_width,
            m_height,
            m_flowWidth,
            m_flowHeight,
            flowStrideBytes,
            m_gridSize,
            weight,
            m_cudaStream
        );

        // Check kernel launch error (first time only)
        static bool checkedLaunch = false;
        if (!checkedLaunch) {
            checkedLaunch = true;
            cuStreamSynchronize(m_cudaStream);
            cudaError_t launchErr = cudaGetLastError();
            LOG("DIAG kernel launch: cudaGetLastError=%d (%s)", launchErr, cudaGetErrorString(launchErr));
        }

        // No host download — output stays on GPU for PresentFromGPU
        return true;
    }
    catch (const std::exception& e) {
        LogNvOFError("InterpolateFrame", e);
        return false;
    }
}

bool FrucCaptureMode::PresentFromGPU(IDirect3DDevice9Ex* device) {
    // Map D3D9 output surface for CUDA access
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

    // Copy interpolated output from CUDA linear memory to D3D9 array (GPU→GPU)
    CUDA_MEMCPY2D cp = {};
    cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cp.srcDevice = m_cudaOutputFrame;
    cp.srcPitch = m_width * 4;
    cp.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    cp.dstArray = outputArray;
    cp.WidthInBytes = m_width * 4;
    cp.Height = m_height;
    cuMemcpy2DAsync(&cp, m_cudaStream);

    // Sync stream and unmap so D3D9 can access the surface
    cuStreamSynchronize(m_cudaStream);
    cuGraphicsUnmapResources(1, &m_cudaOutputResource, m_cudaStream);

    // Present via D3D9 (VSync: blocks until VBlank for hardware-timed cadence)
    HRESULT stretchHr = device->StretchRect(m_outputSurface, NULL, g_backbuffer, NULL, D3DTEXF_NONE);
    HRESULT presentHr = device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_ONE);

    static int presentDiagCount = 0;
    if (presentDiagCount < 5) {
        LOG("DIAG PresentFromGPU[%d]: StretchRect=0x%08x, PresentEx=0x%08x",
            presentDiagCount, stretchHr, presentHr);
        presentDiagCount++;
    }

    return true;
}

void FrucCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    LOG("FrucCaptureMode::Run() - GPU-resident optical flow interpolation");
    LOG("Presentation: %s (PresentationInterval=%d, INTERVAL_ONE=%d)",
        m_isVsyncMode ? "VSync" : "Timed",
        GetPresentationInterval(), D3DPRESENT_INTERVAL_ONE);

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

    // Configure NvFBC with single buffer
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
    setupParams.dwNumBuffers = 1;
    setupParams.bHDRRequest = FALSE;

    NVFBCRESULT setupResult = nvfbcDx9->NvFBCToDx9VidSetUp(&setupParams);
    if (NVFBC_SUCCESS != setupResult) {
        LOGERR("Failed to configure NvFBC (error code: 0x%08x)", setupResult);
        return;
    }
    LOG("NvFBC configured with single capture buffer");

    // Initialize NvOF now that we have dimensions
    if (!InitNvOF()) {
        LOGERR("Failed to initialize NvOF");
        return;
    }

    // Allocate buffers and register D3D9 surfaces for CUDA interop
    if (!AllocateBuffers()) {
        LOGERR("Failed to allocate buffers");
        return;
    }

    // ===== Main capture/interpolation loop (VSync-driven) =====
    // PresentEx(D3DPRESENT_INTERVAL_ONE) blocks until VBlank — this is the timing
    // mechanism. No Sleep(), no manual target time tracking, no drift.
    MSG msg = {};
    bool flowComputedForCurrentPair = false;
    int presentCount = 0;
    int framesSinceLog = 0;
    const int LOG_INTERVAL = 300;

    // Timing instrumentation
    double accumCaptureUs = 0, accumFlowUs = 0, accumInterpUs = 0, accumPresentUs = 0;
    int timingCaptureCount = 0, timingPresentCount = 0;
    double usPerTick = 1000000.0 / (double)m_perfFreq.QuadPart;

    // Weight statistics
    double accumWeight = 0;
    float minWeight = 1.0f, maxWeight = 0.0f;

    // Frame pair delta statistics (how far apart the two ring buffer frames are)
    double accumPairDeltaMs = 0;
    double minPairDeltaMs = 1e9, maxPairDeltaMs = 0;

    // VBlank time tracking for interpolation weight.
    // After PresentEx returns (VBlank), we record the time. Next iteration uses
    // that time as the interpolation target — it falls between frame0 (captured
    // after previous VBlank) and frame1 (captured after this VBlank).
    LARGE_INTEGER lastVBlankTime = {};
    bool hasVBlankTime = false;

    // Per-frame outlier detection: log any frame-to-frame interval that deviates
    // significantly from the running average (catches hitches regardless of actual fps)
    LARGE_INTEGER lastFrameTime = {};
    bool hasLastFrameTime = false;
    int outlierCount = 0;
    double avgIntervalMs = 0.0;
    int intervalSampleCount = 0;

    LOG("Entering VSync-driven main loop (present blocks until VBlank)");

    while (TRUE) {
        LARGE_INTEGER tStart, tEnd;

        // === CAPTURE ===
        // WAIT_WITH_TIMEOUT grab: blocks briefly until new source frame or timeout.
        // After VSync present blocks ~14ms, there's usually a new source frame ready.
        int capturedBefore = m_capturedFrameCount;
        QueryPerformanceCounter(&tStart);
        if (!CaptureFrame(nvfbcDx9, grabParams)) {
            LOGERR("Fatal error during frame capture");
            break;
        }
        QueryPerformanceCounter(&tEnd);
        accumCaptureUs += (double)(tEnd.QuadPart - tStart.QuadPart) * usPerTick;
        timingCaptureCount++;

        bool newFrame = (m_capturedFrameCount > capturedBefore);
        if (newFrame) {
            flowComputedForCurrentPair = false;
        }

        // === FLOW + INTERPOLATION + VSYNC PRESENT ===
        FrameHistoryEntry* frame0 = GetFrame(1);
        FrameHistoryEntry* frame1 = GetFrame(0);

        if (frame0 && frame1 && frame0->valid && frame1->valid) {
            LONGLONG t0 = frame0->timestamp.QuadPart;
            LONGLONG t1 = frame1->timestamp.QuadPart;

            // Compute flow once per frame pair
            if (!flowComputedForCurrentPair) {
                QueryPerformanceCounter(&tStart);
                if (!ComputeOpticalFlow()) {
                    LOGERR("Optical flow computation failed");
                    break;
                }
                QueryPerformanceCounter(&tEnd);
                accumFlowUs += (double)(tEnd.QuadPart - tStart.QuadPart) * usPerTick;
                flowComputedForCurrentPair = true;
            }

            // Weight: where does the last VBlank fall between the two source frames?
            // frame0 was captured after the previous VBlank, frame1 after this one.
            // The VBlank moment (recorded after PresentEx returned last iteration)
            // sits between t0 and t1, giving a meaningful interpolation position.
            // First frame uses 0.5 as default until we have a VBlank timestamp.
            float weight = 0.5f;
            if (hasVBlankTime && t1 > t0) {
                double rawWeight = (double)(lastVBlankTime.QuadPart - t0) / (double)(t1 - t0);
                weight = (float)(rawWeight < 0.0 ? 0.0 : (rawWeight > 1.0 ? 1.0 : rawWeight));
            }

            CUdeviceptr prevFrame = GetFrameCudaPtr(1);
            CUdeviceptr currFrame = GetFrameCudaPtr(0);

            LARGE_INTEGER tInterp, tPresent;
            QueryPerformanceCounter(&tInterp);
            if (InterpolateFrame(weight, prevFrame, currFrame)) {
                QueryPerformanceCounter(&tPresent);
                // PresentFromGPU calls PresentEx(INTERVAL_ONE) which blocks until VBlank.
                // This is the frame pacer — no Sleep needed.
                PresentFromGPU(device);
                // Record VBlank time immediately after PresentEx returns.
                // Next iteration's weight calculation uses this as the interpolation target.
                QueryPerformanceCounter(&lastVBlankTime);
                hasVBlankTime = true;

                accumInterpUs += (double)(tPresent.QuadPart - tInterp.QuadPart) * usPerTick;
                accumPresentUs += (double)(lastVBlankTime.QuadPart - tPresent.QuadPart) * usPerTick;

                // Frame pair delta: how far apart the two source frames are
                double pairDeltaMs = (double)(t1 - t0) * usPerTick / 1000.0;
                accumPairDeltaMs += pairDeltaMs;
                if (pairDeltaMs < minPairDeltaMs) minPairDeltaMs = pairDeltaMs;
                if (pairDeltaMs > maxPairDeltaMs) maxPairDeltaMs = pairDeltaMs;

                // Per-frame outlier detection (relative to running average)
                if (hasLastFrameTime) {
                    double intervalMs = (double)(lastVBlankTime.QuadPart - lastFrameTime.QuadPart) * usPerTick / 1000.0;

                    // Update running average (exponential moving average after warmup)
                    if (intervalSampleCount < 30) {
                        // Warmup: simple cumulative average
                        avgIntervalMs = (avgIntervalMs * intervalSampleCount + intervalMs) / (intervalSampleCount + 1);
                        intervalSampleCount++;
                    } else {
                        // Check for outlier: >50% deviation from running average
                        double ratio = intervalMs / avgIntervalMs;
                        if (ratio > 1.5 || ratio < 0.5) {
                            double captureMs = (double)(tEnd.QuadPart - tStart.QuadPart) * usPerTick / 1000.0;
                            double presentMs = (double)(lastVBlankTime.QuadPart - tPresent.QuadPart) * usPerTick / 1000.0;
                            LOG("OUTLIER[%d] #%d: interval=%.2fms (avg=%.2f, ratio=%.2fx) "
                                "weight=%.3f capture=%.2fms present=%.2fms newFrame=%d",
                                presentCount, outlierCount,
                                intervalMs, avgIntervalMs, ratio,
                                weight, captureMs, presentMs, newFrame ? 1 : 0);
                            outlierCount++;
                        }
                        // Update average (slow EMA, alpha=0.02 so outliers don't skew it)
                        avgIntervalMs = avgIntervalMs * 0.98 + intervalMs * 0.02;
                    }
                }
                lastFrameTime = lastVBlankTime;
                hasLastFrameTime = true;

                // Accumulate weight statistics
                accumWeight += weight;
                if (weight < minWeight) minWeight = weight;
                if (weight > maxWeight) maxWeight = weight;

                if (presentCount < 10) {
                    LOG("Present[%d]: weight=%.3f", presentCount, weight);
                }

                presentCount++;
                timingPresentCount++;
                framesSinceLog++;
                if (framesSinceLog >= LOG_INTERVAL) {
                    double np = (double)timingPresentCount;
                    double nc = (double)timingCaptureCount;
                    double workMs = (accumFlowUs + accumInterpUs) / np / 1000.0;
                    LOG("TIMING(ms): flow=%.2f interp=%.2f present=%.2f work=%.2f "
                        "| capture=%.2f/grab | weight avg=%.3f min=%.3f max=%.3f "
                        "| pair_delta avg=%.1f min=%.1f max=%.1f "
                        "| %d presents, %d captures, %d outliers",
                        accumFlowUs / np / 1000.0,
                        accumInterpUs / np / 1000.0,
                        accumPresentUs / np / 1000.0,
                        workMs,
                        accumCaptureUs / nc / 1000.0,
                        accumWeight / np, minWeight, maxWeight,
                        accumPairDeltaMs / np, minPairDeltaMs, maxPairDeltaMs,
                        timingPresentCount, timingCaptureCount, outlierCount);
                    accumCaptureUs = accumFlowUs = accumInterpUs = accumPresentUs = 0;
                    timingPresentCount = timingCaptureCount = 0;
                    accumWeight = 0;
                    minWeight = 1.0f;
                    maxWeight = 0.0f;
                    accumPairDeltaMs = 0;
                    minPairDeltaMs = 1e9;
                    maxPairDeltaMs = 0;
                    outlierCount = 0;
                    framesSinceLog = 0;
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

    LOG("FrucCaptureMode::Run() exiting - captured %d frames, presented %d",
        m_capturedFrameCount, presentCount);
}

const char* FrucCaptureMode::GetModeName() const {
    static char modeName[64];
    sprintf_s(modeName, sizeof(modeName), "OptFlow-GPUResident-%.2f", m_targetFramerate);
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
