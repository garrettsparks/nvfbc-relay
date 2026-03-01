#include "FrucCaptureMode.h"
#include <SimpleLogger.h>

// Forward declarations of CUDA kernels (implemented in InterpolateKernel.cu)
extern "C" void launchDownscaleKernel(
    const uint8_t* src,
    int srcWidth,
    int srcHeight,
    uint8_t* dst,
    int dstWidth,
    int dstHeight,
    int dstStrideBytes,
    CUstream stream);

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
    int srcStrideBytes,
    int gridSize,
    float weight,
    CUstream stream);

extern "C" void launchInterpolateKernelToSurface(
    const uint8_t* frame0,
    const uint8_t* frame1,
    const uint8_t* flowData,
    cudaSurfaceObject_t output,
    int width,
    int height,
    int flowWidth,
    int flowHeight,
    int flowStrideBytes,
    int srcStrideBytes,
    int gridSize,
    float weight,
    CUstream stream);

FrucCaptureMode::FrucCaptureMode(float framerate)
    : m_targetFramerate(framerate == 0.0f ? 60.0f : framerate)
    , m_isVsyncMode(framerate == 0.0f)
    , m_width(0)
    , m_height(0)
    , m_captureWidth(0)
    , m_captureHeight(0)
    , m_flowWidth(0)
    , m_flowHeight(0)
    , m_cuContext(nullptr)
    , m_cuDevice(0)
    , m_cudaStream(nullptr)
    , m_captureStream(nullptr)
    , m_flowStream(nullptr)
    , m_cudaInitialized(false)
    , m_interpStartEvent(nullptr)
    , m_interpEndEvent(nullptr)
    , m_cudaOutputResource(nullptr)
    , m_nvofInitialized(false)
    , m_gridSize(4)
    , m_ringStride(0)
    , m_ringWriteIndex(0)
    , m_capturedFrameCount(0)
    , m_fullResGrabBuffer(0)
    , m_outputSurface(nullptr)
    , m_interpSurfObj(0)
    , m_outputMapped(false)
    , m_device(nullptr)
    , m_captureThread(NULL)
    , m_captureRunning(false)
    , m_sessionInvalidated(false)
    , m_captureGrabCount(0)
    , m_flowThread(NULL)
    , m_flowRequestEvent(NULL)
    , m_flowDoneEvent(NULL)
    , m_flowShutdown(false)
    , m_flowPrevSlot(0)
    , m_flowNextSlot(0)
    , m_flowOutputIdx(0)
    , m_lastFlowTimeUs(0.0)
    , m_nvfbcCuda(nullptr)
    , m_cudaGrabParams{}
    , m_grabInfo{}
    , m_strideChecked(false)
    , m_needsStrideCopy(false)
    , m_grabStride(0)
    , m_grabTempBuffer(0)
{
    LOG("=== FrucCaptureMode GPU-Resident Pipeline V7 (direct-to-backbuffer) ===");
    LOG("Target framerate: %.2f fps", m_targetFramerate);
    LOG("VSync mode: %s", m_isVsyncMode ? "yes" : "no");
    LOG("Flow grid size: %d, Ring size: %d", m_gridSize, RING_SIZE);

    for (int i = 0; i < RING_SIZE; i++) {
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

    // Stop flow worker thread
    if (m_flowThread) {
        m_flowShutdown.store(true);
        if (m_flowRequestEvent) SetEvent(m_flowRequestEvent);  // wake it up
        WaitForSingleObject(m_flowThread, 5000);
        CloseHandle(m_flowThread);
        m_flowThread = NULL;
    }
    if (m_flowRequestEvent) { CloseHandle(m_flowRequestEvent); m_flowRequestEvent = NULL; }
    if (m_flowDoneEvent) { CloseHandle(m_flowDoneEvent); m_flowDoneEvent = NULL; }

    // Release NvFBCCuda (must be done before CUDA context destruction)
    if (m_nvfbcCuda) {
        m_nvfbcCuda->NvFBCCudaRelease();
        m_nvfbcCuda = nullptr;
    }

    // Release NvOF resources (NvOF input buffers ARE the ring — no separate ring to free)
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
    if (m_fullResGrabBuffer) { cuMemFree(m_fullResGrabBuffer); m_fullResGrabBuffer = 0; }
    if (m_grabTempBuffer) { cuMemFree(m_grabTempBuffer); m_grabTempBuffer = 0; }

    // Destroy CUDA events
    if (m_interpStartEvent) { cuEventDestroy(m_interpStartEvent); m_interpStartEvent = nullptr; }
    if (m_interpEndEvent) { cuEventDestroy(m_interpEndEvent); m_interpEndEvent = nullptr; }

    // Release D3D9 interop surface
    if (m_outputSurface) { m_outputSurface->Release(); m_outputSurface = nullptr; }

    // Destroy CUDA streams
    if (m_flowStream) { cuStreamDestroy(m_flowStream); m_flowStream = nullptr; }
    if (m_captureStream) { cuStreamDestroy(m_captureStream); m_captureStream = nullptr; }
    if (m_cudaStream) { cuStreamDestroy(m_cudaStream); m_cudaStream = nullptr; }

    // Destroy D3D9 interop context
    if (m_cuContext) { cuCtxDestroy(m_cuContext); m_cuContext = nullptr; }

    m_cudaInitialized = false;
}

UINT FrucCaptureMode::GetPresentationInterval() const {
    return D3DPRESENT_INTERVAL_IMMEDIATE;
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

    // Present thread stream (flow/interp/output)
    result = cuStreamCreate(&m_cudaStream, CU_STREAM_DEFAULT);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuStreamCreate(present)", result);
        return false;
    }

    // Capture thread stream (downscale kernel)
    result = cuStreamCreate(&m_captureStream, CU_STREAM_DEFAULT);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuStreamCreate(capture)", result);
        return false;
    }

    // Flow thread stream (kept for future use)
    result = cuStreamCreate(&m_flowStream, CU_STREAM_DEFAULT);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuStreamCreate(flow)", result);
        return false;
    }

    // CUDA events for GPU kernel timing (no extra sync — queried after existing cuStreamSynchronize)
    result = cuEventCreate(&m_interpStartEvent, CU_EVENT_DEFAULT);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuEventCreate(interpStart)", result);
        return false;
    }
    result = cuEventCreate(&m_interpEndEvent, CU_EVENT_DEFAULT);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuEventCreate(interpEnd)", result);
        return false;
    }

    LOG("CUDA initialized with D3D9 interop (3 streams + 2 timing events)");
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

        // Create RING_SIZE input buffers — these ARE the ring buffer (no separate ring alloc)
        m_inputBuffers = m_nvOF->CreateBuffers(NV_OF_BUFFER_USAGE_INPUT, RING_SIZE);
        LOG("Created %d NvOF input buffers (used as ring buffer)", (int)m_inputBuffers.size());

        // Query stride from first buffer
        NvOFBufferCudaDevicePtr* buf0 = dynamic_cast<NvOFBufferCudaDevicePtr*>(m_inputBuffers[0].get());
        if (!buf0) {
            LOGERR("Failed to get NvOF input buffer 0 as CudaDevicePtr");
            return false;
        }
        m_ringStride = (int)buf0->getStrideInfo().strideInfo[0].strideXInBytes;
        LOG("NvOF input buffer stride: %d bytes (tight=%d bytes, padding=%d bytes/row)",
            m_ringStride, m_width * 4, m_ringStride - m_width * 4);

        m_outputBuffers = m_nvOF->CreateBuffers(NV_OF_BUFFER_USAGE_OUTPUT, 2);
        LOG("Created %d NvOF output buffers (double-buffered for V7 pipeline)", (int)m_outputBuffers.size());

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
    LOG("Allocating GPU-resident buffers (V7: direct-to-backbuffer)...");

    // Full-res grab buffer (single, for NvFBCCuda grabs before downscale)
    size_t fullResSize = m_captureWidth * m_captureHeight * 4;
    CUresult fullResResult = cuMemAlloc(&m_fullResGrabBuffer, fullResSize);
    if (fullResResult != CUDA_SUCCESS) {
        LogCudaError("cuMemAlloc(fullResGrabBuffer)", fullResResult);
        return false;
    }
    LOG("Allocated full-res grab buffer (%zu bytes, %dx%d)", fullResSize, m_captureWidth, m_captureHeight);

    // Ring buffer = NvOF input buffers (already allocated in InitNvOF), no separate alloc needed

    // D3D9 output surface at working resolution (= backbuffer res, so StretchRect is 1:1 copy)
    // Interp kernel writes directly to this surface via surf2Dwrite (no intermediate buffer)
    HRESULT hr = m_device->CreateOffscreenPlainSurface(
        m_width, m_height, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_outputSurface, NULL);
    if (FAILED(hr)) {
        LOGERR("Failed to create output surface (HRESULT: 0x%08x)", hr);
        return false;
    }

    // Register output surface for CUDA interop
    CUresult result = cuGraphicsD3D9RegisterResource(&m_cudaOutputResource, m_outputSurface,
                                                      CU_GRAPHICS_REGISTER_FLAGS_NONE);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuGraphicsD3D9RegisterResource(outputSurface)", result);
        return false;
    }

    // Allocate temp buffer for padded grabs if stride mismatch detected
    if (m_needsStrideCopy) {
        size_t tempSize = (size_t)m_grabStride * m_captureHeight;
        CUresult tempRes = cuMemAlloc(&m_grabTempBuffer, tempSize);
        if (tempRes != CUDA_SUCCESS) {
            LogCudaError("cuMemAlloc(grabTempBuffer)", tempRes);
            return false;
        }
        LOG("Allocated grab temp buffer (%zu bytes, stride=%d)", tempSize, m_grabStride);
    }

    LOG("GPU-resident buffer allocation complete (ring=NvOF inputs, 1 output surface)");
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
    LOG("FrucCaptureMode::Setup() - GPU-resident pipeline V7 (direct-to-backbuffer)");

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
    LOG("Capture thread started (V7: NvFBCCuda + downscale %dx%d → %dx%d)",
        self->m_captureWidth, self->m_captureHeight, self->m_width, self->m_height);

    // Set CUDA context on this thread
    cuCtxSetCurrent(self->m_cuContext);

    // Local copy of grab params
    NVFBC_CUDA_GRAB_FRAME_PARAMS grabParams = self->m_cudaGrabParams;
    NvFBCFrameGrabInfo grabInfo = {};
    grabParams.pNvFBCFrameGrabInfo = &grabInfo;

    // Stride handling is determined by probe grab before thread launch.
    bool needsStrideCopy = self->m_needsStrideCopy;

    // Duplicate detection: time each grab to distinguish new vs stale frames.
    LARGE_INTEGER perfFreq;
    QueryPerformanceFrequency(&perfFreq);
    double usPerTick = 1000000.0 / (double)perfFreq.QuadPart;
    double dupThresholdUs = (double)self->m_cudaGrabParams.dwWaitTime * 500.0;
    int dupCount = 0;

    // Grab always targets the full-res buffer (or temp buffer for stride handling)
    CUdeviceptr grabTarget = needsStrideCopy ? self->m_grabTempBuffer : self->m_fullResGrabBuffer;

    // Cache NvOF ring buffer device pointers (avoid dynamic_cast in hot loop)
    CUdeviceptr ringPtrs[RING_SIZE];
    for (int i = 0; i < RING_SIZE; i++) {
        NvOFBufferCudaDevicePtr* buf = dynamic_cast<NvOFBufferCudaDevicePtr*>(self->m_inputBuffers[i].get());
        if (!buf) {
            LOGERR("Capture thread: Failed to get NvOF input buffer %d as CudaDevicePtr", i);
            return 1;
        }
        ringPtrs[i] = buf->getCudaDevicePtr();
    }
    int ringStride = self->m_ringStride;

    while (self->m_captureRunning.load()) {
        int writeSlot = self->m_ringWriteIndex.load();

        grabParams.pCUDADeviceBuffer = (void*)grabTarget;

        LARGE_INTEGER tBefore, tAfter;
        QueryPerformanceCounter(&tBefore);
        NVFBCRESULT res = self->m_nvfbcCuda->NvFBCCudaGrabFrame(&grabParams);
        QueryPerformanceCounter(&tAfter);

        if (res == NVFBC_SUCCESS) {
            double grabUs = (double)(tAfter.QuadPart - tBefore.QuadPart) * usPerTick;

            // Fast return = new frame available, slow return = timeout (stale frame)
            if (grabUs >= dupThresholdUs) {
                dupCount++;
                continue;
            }

            // If stride padding: copy padded grab → tight-stride full-res buffer
            if (needsStrideCopy) {
                CUDA_MEMCPY2D cp = {};
                cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
                cp.srcDevice = self->m_grabTempBuffer;
                cp.srcPitch = self->m_grabStride;
                cp.dstMemoryType = CU_MEMORYTYPE_DEVICE;
                cp.dstDevice = self->m_fullResGrabBuffer;
                cp.dstPitch = self->m_captureWidth * 4;
                cp.WidthInBytes = self->m_captureWidth * 4;
                cp.Height = self->m_captureHeight;
                cuMemcpy2DAsync(&cp, self->m_captureStream);
            }

            // Downscale full-res → NvOF ring slot at working resolution
            launchDownscaleKernel(
                (const uint8_t*)self->m_fullResGrabBuffer,
                self->m_captureWidth, self->m_captureHeight,
                (uint8_t*)ringPtrs[writeSlot],
                self->m_width, self->m_height,
                ringStride,
                self->m_captureStream);
            cuStreamSynchronize(self->m_captureStream);

            LARGE_INTEGER capTime;
            QueryPerformanceCounter(&capTime);

            // Record timestamp and advance ring buffer
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
    }

    LOG("Capture thread exiting (total duplicates skipped: %d)", dupCount);
    return 0;
}

// =============================================================================
// Flow worker thread (V7): runs NvOF asynchronously, overlapped with present.
// Waits for signal from present thread, computes flow, signals completion.
// Uses double-buffered NvOF outputs to avoid read/write conflicts with interp.
// NvOF input buffers ARE the ring — no copy needed, Execute reads directly.
// =============================================================================
DWORD WINAPI FrucCaptureMode::FlowWorkerThreadProc(LPVOID param) {
    FrucCaptureMode* self = (FrucCaptureMode*)param;
    LOG("Flow worker thread started (V7: pipelined, zero-copy ring→NvOF)");

    cuCtxSetCurrent(self->m_cuContext);

    LARGE_INTEGER perfFreq;
    QueryPerformanceFrequency(&perfFreq);
    double usPerTick = 1000000.0 / (double)perfFreq.QuadPart;

    while (true) {
        WaitForSingleObject(self->m_flowRequestEvent, INFINITE);
        if (self->m_flowShutdown.load()) break;

        LARGE_INTEGER tStart, tEnd;
        QueryPerformanceCounter(&tStart);

        // Read shared state (written by present thread before signaling)
        int prevSlot = self->m_flowPrevSlot;
        int nextSlot = self->m_flowNextSlot;
        int outputIdx = self->m_flowOutputIdx;

        try {
            // NvOF Execute directly from ring slots (NvOF input buffers ARE the ring)
            self->m_nvOF->Execute(
                self->m_inputBuffers[prevSlot].get(),
                self->m_inputBuffers[nextSlot].get(),
                self->m_outputBuffers[outputIdx].get()
            );

            QueryPerformanceCounter(&tEnd);
            double flowUs = (double)(tEnd.QuadPart - tStart.QuadPart) * usPerTick;
            self->m_lastFlowTimeUs.store(flowUs);
        }
        catch (const std::exception& e) {
            LOGERR("Flow thread: NvOF error: %s", e.what());
        }

        SetEvent(self->m_flowDoneEvent);
    }

    LOG("Flow worker thread exiting");
    return 0;
}

bool FrucCaptureMode::InterpolateFrame(float weight, CUdeviceptr framePrev, CUdeviceptr frameCurr, int flowOutputIdx) {
    if (!m_nvofInitialized) return false;

    try {
        NvOFBufferCudaDevicePtr* flowBuffer = dynamic_cast<NvOFBufferCudaDevicePtr*>(m_outputBuffers[flowOutputIdx].get());
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

        // Map D3D9 output surface for direct kernel write
        CUresult mapResult = cuGraphicsMapResources(1, &m_cudaOutputResource, m_cudaStream);
        if (mapResult != CUDA_SUCCESS) {
            LogCudaError("cuGraphicsMapResources(interp)", mapResult);
            return false;
        }

        CUarray outputArray;
        mapResult = cuGraphicsSubResourceGetMappedArray(&outputArray, m_cudaOutputResource, 0, 0);
        if (mapResult != CUDA_SUCCESS) {
            LogCudaError("cuGraphicsSubResourceGetMappedArray(interp)", mapResult);
            cuGraphicsUnmapResources(1, &m_cudaOutputResource, m_cudaStream);
            return false;
        }

        // Create surface object for kernel to write via surf2Dwrite
        CUDA_RESOURCE_DESC resDesc = {};
        resDesc.resType = CU_RESOURCE_TYPE_ARRAY;
        resDesc.res.array.hArray = outputArray;
        mapResult = cuSurfObjectCreate(&m_interpSurfObj, &resDesc);
        if (mapResult != CUDA_SUCCESS) {
            LogCudaError("cuSurfObjectCreate", mapResult);
            cuGraphicsUnmapResources(1, &m_cudaOutputResource, m_cudaStream);
            return false;
        }
        m_outputMapped = true;

        // Record GPU start event, launch kernel, record GPU end event
        cuEventRecord(m_interpStartEvent, m_cudaStream);

        launchInterpolateKernelToSurface(
            (const uint8_t*)framePrev,
            (const uint8_t*)frameCurr,
            (const uint8_t*)flowPtr,
            (cudaSurfaceObject_t)m_interpSurfObj,
            m_width, m_height,
            m_flowWidth, m_flowHeight,
            flowStrideBytes, m_ringStride, m_gridSize,
            weight, m_cudaStream
        );

        cuEventRecord(m_interpEndEvent, m_cudaStream);

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

bool FrucCaptureMode::PresentFromGPU(IDirect3DDevice9Ex* device, LARGE_INTEGER* pPresentExStart, float* pInterpGpuMs) {
    // Sync kernel, query GPU elapsed, unmap D3D9 surface, StretchRect to backbuffer, PresentEx
    // The interp kernel was already launched async by InterpolateFrame()
    *pInterpGpuMs = 0;
    if (m_outputMapped) {
        cuStreamSynchronize(m_cudaStream);

        // Query actual GPU kernel execution time from CUDA events
        cuEventElapsedTime(pInterpGpuMs, m_interpStartEvent, m_interpEndEvent);

        cuSurfObjectDestroy(m_interpSurfObj);
        m_interpSurfObj = 0;
        cuGraphicsUnmapResources(1, &m_cudaOutputResource, m_cudaStream);
        m_outputMapped = false;
    }

    // 1:1 copy from interop surface to backbuffer (same resolution)
    device->StretchRect(m_outputSurface, NULL, g_backbuffer, NULL, D3DTEXF_NONE);

    // Record split point: everything above is "present" (sync + unmap + StretchRect), pacing+PresentEx below is "pace"
    QueryPerformanceCounter(pPresentExStart);

    // Frame pacing: sleep until next frame boundary using NvFBC high-precision sleep
    static LARGE_INTEGER lastPresentTime = {};
    static bool hasLastPresentTime = false;
    if (hasLastPresentTime) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsedUs = (double)(now.QuadPart - lastPresentTime.QuadPart) * 1e6 / (double)m_perfFreq.QuadPart;
        double targetIntervalUs = 1e6 / (double)m_targetFramerate;
        double remainingUs = targetIntervalUs - elapsedUs;
        if (remainingUs > 200.0) {  // Only sleep if > 200us remaining
            m_nvfbcCuda->NvFBCCudaGPUBasedCPUSleep((__int64)remainingUs);
        }
    }

    device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);
    QueryPerformanceCounter(&lastPresentTime);
    hasLastPresentTime = true;
    return true;
}

// =============================================================================
// Main loop: V7 direct-to-backbuffer with pipelined flow + present.
// Flow(N+1) runs on flow worker thread concurrently with Present(N).
// Budget = max(flow, present) instead of flow + present.
// Working resolution = backbuffer resolution (no StretchRect).
// =============================================================================
void FrucCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    LOG("FrucCaptureMode::Run() - V7 direct-to-backbuffer, pipelined flow + present");
    LOG("Presentation: %s (PresentationInterval=%d, INTERVAL_IMMEDIATE + DwmFlush)",
        m_isVsyncMode ? "VSync" : "Timed",
        GetPresentationInterval());

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

        m_captureWidth = probeInfo.dwWidth;
        m_captureHeight = probeInfo.dwHeight;

        // V7: Working resolution = backbuffer resolution (direct-to-backbuffer, no StretchRect)
        m_width = BUF_WIDTH;
        m_height = BUF_HEIGHT;

        // If capture res equals or is smaller than backbuffer, no downscale needed
        if (m_captureWidth <= m_width && m_captureHeight <= m_height) {
            m_width = m_captureWidth;
            m_height = m_captureHeight;
            LOG("Capture res <= backbuffer res, no downscale (using %dx%d)", m_width, m_height);
        }

        LOG("NvFBCCuda capture resolution: %dx%d (bufWidth=%d, HDR=%d)",
            m_captureWidth, m_captureHeight, probeInfo.dwBufferWidth, probeInfo.bIsHDR);
        LOG("Working resolution: %dx%d (backbuffer-native, no StretchRect)", m_width, m_height);
        LOG("Downscale ratio: %.2fx", (float)(m_captureWidth * m_captureHeight) / (float)(m_width * m_height));

        // Check stride on probe result (applies to full-res grab)
        if (probeInfo.dwBufferWidth != (DWORD)m_captureWidth) {
            m_strideChecked = true;
            m_needsStrideCopy = true;
            m_grabStride = probeInfo.dwBufferWidth * 4;
            LOG("NvFBCCuda stride padding detected: dwWidth=%d, dwBufferWidth=%d", m_captureWidth, probeInfo.dwBufferWidth);
        } else {
            m_strideChecked = true;
            m_needsStrideCopy = false;
            LOG("NvFBCCuda stride OK: dwWidth=%d == dwBufferWidth=%d — zero-copy", m_captureWidth, probeInfo.dwBufferWidth);
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
    LOG("Capture thread launched (V7: NvFBCCuda + downscale to NvOF ring)");

    // ===== Launch flow worker thread =====
    m_flowShutdown.store(false);
    m_flowRequestEvent = CreateEvent(NULL, FALSE, FALSE, NULL);  // auto-reset
    m_flowDoneEvent = CreateEvent(NULL, FALSE, FALSE, NULL);     // auto-reset
    if (!m_flowRequestEvent || !m_flowDoneEvent) {
        LOGERR("Failed to create flow events");
        return;
    }

    m_flowThread = CreateThread(NULL, 0, FlowWorkerThreadProc, this, 0, NULL);
    if (!m_flowThread) {
        LOGERR("Failed to create flow worker thread");
        return;
    }
    LOG("Flow worker thread launched (V7: pipelined, zero-copy ring→NvOF)");

    // ===== Main pipelined present/interpolation loop =====
    // V7 pipeline: flow(N+1) runs concurrently with present(N).
    // Budget = max(flow, present) instead of flow + present.
    MSG msg = {};
    bool hasOutputFrame = false;
    bool flowPending = false;  // true if flow thread is computing and we haven't waited yet
    int presentCount = 0;
    int framesSinceLog = 0;
    const int LOG_INTERVAL = 300;

    // Timing
    double accumSelectUs = 0;
    double accumFlowUs = 0, accumInterpSetupUs = 0;
    double accumFlowWaitUs = 0;
    double accumInterpGpuUs = 0;
    double accumPresentUs = 0, accumPacingUs = 0;
    int timingSelectCount = 0, timingPresentCount = 0, flowComputeCount = 0;
    double usPerTick = 1000000.0 / (double)m_perfFreq.QuadPart;

    // Per-iteration phase times (for stall breakdown)
    double iterSelectMs = 0, iterFlowMs = 0, iterFlowWaitMs = 0, iterInterpSetupMs = 0;
    float iterInterpGpuMs = 0;
    double iterPresentMs = 0, iterPacingMs = 0;

    // Weight statistics
    double accumWeight = 0;
    float minWeight = 1.0f, maxWeight = 0.0f;

    // Frame pair delta
    double accumPairDeltaMs = 0;
    double minPairDeltaMs = 1e9, maxPairDeltaMs = 0;

    // Present time tracking (timer-paced, not VBlank-aligned)
    LARGE_INTEGER lastPresentTime = {};
    bool hasLastPresentTime = false;
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

    // V7 pipeline state: which flow output buffer holds the CURRENT ready result
    int currentFlowOutputIdx = 0;  // toggles 0/1 each flow dispatch
    int readyFlowOutputIdx = 0;    // the output index whose flow result we'll read for interp

    // Bracket pair saved for the CURRENT ready flow (for weight calculation)
    int readyPrevSlot = -1, readyNextSlot = -1;
    LONGLONG readyT0 = 0, readyT1 = 0;

    // Cache NvOF ring buffer device pointers for InterpolateFrame calls
    CUdeviceptr ringPtrs[RING_SIZE];
    for (int i = 0; i < RING_SIZE; i++) {
        NvOFBufferCudaDevicePtr* buf = dynamic_cast<NvOFBufferCudaDevicePtr*>(m_inputBuffers[i].get());
        ringPtrs[i] = buf ? buf->getCudaDevicePtr() : 0;
    }

    LOG("Entering V7 main loop (direct-to-backbuffer, pipelined flow+present, ring=%d)", RING_SIZE);

    while (TRUE) {
        LARGE_INTEGER tStart, tEnd;

        // === STEP 1: WAIT FOR FLOW (if flow thread is computing) ===
        if (flowPending) {
            QueryPerformanceCounter(&tStart);
            WaitForSingleObject(m_flowDoneEvent, INFINITE);
            QueryPerformanceCounter(&tEnd);
            flowPending = false;

            iterFlowWaitMs = (double)(tEnd.QuadPart - tStart.QuadPart) * usPerTick / 1000.0;
            accumFlowWaitUs += iterFlowWaitMs * 1000.0;

            // Read flow timing from flow thread
            double flowUs = m_lastFlowTimeUs.load();
            iterFlowMs = flowUs / 1000.0;
            accumFlowUs += flowUs;
            flowComputeCount++;

            // The flow result is now in outputBuffers[currentFlowOutputIdx]
            readyFlowOutputIdx = currentFlowOutputIdx;
            readyPrevSlot = bracketPrevSlot;
            readyNextSlot = bracketNextSlot;
            readyT0 = cachedT0;
            readyT1 = cachedT1;
            pairValid = true;
        }

        // === STEP 2: INTERPOLATE using ready flow result ===
        if (pairValid && readyPrevSlot >= 0) {
            // Weight: where does lastPresentTime fall between the bracket frames?
            float weight = 0.5f;
            if (hasLastPresentTime && readyT1 > readyT0) {
                double rawWeight = (double)(lastPresentTime.QuadPart - readyT0) / (double)(readyT1 - readyT0);
                weight = (float)(rawWeight < 0.0 ? 0.0 : (rawWeight > 1.0 ? 1.0 : rawWeight));
            }

            LARGE_INTEGER tInterp;
            QueryPerformanceCounter(&tInterp);
            if (InterpolateFrame(weight, ringPtrs[readyPrevSlot], ringPtrs[readyNextSlot], readyFlowOutputIdx)) {
                QueryPerformanceCounter(&tEnd);
                iterInterpSetupMs = (double)(tEnd.QuadPart - tInterp.QuadPart) * usPerTick / 1000.0;
                accumInterpSetupUs += iterInterpSetupMs * 1000.0;
                hasOutputFrame = true;

                // Pair delta
                double pairDeltaMs = (double)(readyT1 - readyT0) * usPerTick / 1000.0;
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
                        hasLastPresentTime ? "predicted" : "fallback");
                }
            }
        }

        // === STEP 3: SELECT bracket pair for NEXT flow ===
        // Simple: always use the two newest captures. Dispatch flow when they change.
        int currentFrameCount = m_capturedFrameCount.load();

        if (currentFrameCount >= 2 && currentFrameCount > lastCheckedFrameCount) {
            QueryPerformanceCounter(&tStart);

            int wi = m_ringWriteIndex.load();
            int newNextSlot = (wi + RING_SIZE - 1) % RING_SIZE;  // newest
            int newPrevSlot = (wi + RING_SIZE - 2) % RING_SIZE;  // second newest

            cachedT0 = m_frameHistory[newPrevSlot].timestamp.QuadPart;
            cachedT1 = m_frameHistory[newNextSlot].timestamp.QuadPart;

            QueryPerformanceCounter(&tEnd);
            iterSelectMs = (double)(tEnd.QuadPart - tStart.QuadPart) * usPerTick / 1000.0;
            accumSelectUs += iterSelectMs * 1000.0;
            timingSelectCount++;

            // Dispatch flow if pair changed
            if (cachedT0 != lastCachedT0 || cachedT1 != lastCachedT1) {
                bracketPrevSlot = newPrevSlot;
                bracketNextSlot = newNextSlot;
                lastCachedT0 = cachedT0;
                lastCachedT1 = cachedT1;

                // Toggle flow output buffer
                currentFlowOutputIdx = 1 - readyFlowOutputIdx;

                // Set shared state for flow thread
                m_flowPrevSlot = newPrevSlot;
                m_flowNextSlot = newNextSlot;
                m_flowOutputIdx = currentFlowOutputIdx;

                // Signal flow thread to start (runs concurrently with present below)
                SetEvent(m_flowRequestEvent);
                flowPending = true;
            }

            lastCheckedFrameCount = currentFrameCount;
        }

        // === STEP 4: PRESENT (timer-paced via NvFBCCudaGPUBasedCPUSleep) ===
        // Flow thread is running concurrently during this section.
        if (hasOutputFrame) {
            QueryPerformanceCounter(&tStart);
            LARGE_INTEGER tPresentExStart;
            PresentFromGPU(device, &tPresentExStart, &iterInterpGpuMs);

            QueryPerformanceCounter(&lastPresentTime);
            hasLastPresentTime = true;

            iterPresentMs = (double)(tPresentExStart.QuadPart - tStart.QuadPart) * usPerTick / 1000.0;
            iterPacingMs = (double)(lastPresentTime.QuadPart - tPresentExStart.QuadPart) * usPerTick / 1000.0;
            accumPresentUs += iterPresentMs * 1000.0;
            accumPacingUs += iterPacingMs * 1000.0;
            accumInterpGpuUs += iterInterpGpuMs * 1000.0;

            // Stall detection with phase breakdown
            if (hasLastFrameTime) {
                double intervalMs = (double)(lastPresentTime.QuadPart - lastFrameTime.QuadPart) * usPerTick / 1000.0;
                if (intervalMs > STALL_THRESHOLD_MS) {
                    LOG("STALL[%d] #%d: %.1fms (select=%.1f flow=%.1f fwait=%.1f interp_setup=%.1f interp_gpu=%.2f present=%.1f pace=%.1f)",
                        presentCount, stallCount, intervalMs,
                        iterSelectMs, iterFlowMs, iterFlowWaitMs, iterInterpSetupMs, iterInterpGpuMs,
                        iterPresentMs, iterPacingMs);
                    stallCount++;
                }
            }
            lastFrameTime = lastPresentTime;
            hasLastFrameTime = true;

            // Reset per-iteration times for next cycle
            iterSelectMs = 0;
            iterFlowMs = 0;
            iterFlowWaitMs = 0;
            iterInterpSetupMs = 0;
            iterInterpGpuMs = 0;

            presentCount++;
            timingPresentCount++;
            framesSinceLog++;

            // Periodic timing log
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

                LOG("TIMING(ms): flow=%.2f(%d) fwait=%.2f interp_gpu=%.2f interp_setup=%.2f present=%.2f pace=%.2f "
                    "| select=%.3f (%d selects) "
                    "| weight avg=%.3f min=%.3f max=%.3f "
                    "| pair_delta avg=%.1f min=%.1f max=%.1f "
                    "| %d presents, %d stalls | capture_rate=%.1f/s",
                    flowPerCompute, flowComputeCount,
                    accumFlowWaitUs / np / 1000.0,
                    accumInterpGpuUs / np / 1000.0,
                    accumInterpSetupUs / np / 1000.0,
                    accumPresentUs / np / 1000.0,
                    accumPacingUs / np / 1000.0,
                    ns > 0 ? accumSelectUs / ns / 1000.0 : 0,
                    timingSelectCount,
                    accumWeight / np, minWeight, maxWeight,
                    accumPairDeltaMs / np, minPairDeltaMs, maxPairDeltaMs,
                    timingPresentCount, stallCount,
                    captureRate);

                accumSelectUs = 0;
                accumFlowUs = accumInterpSetupUs = 0;
                accumFlowWaitUs = 0;
                accumInterpGpuUs = 0;
                accumPresentUs = accumPacingUs = 0;
                timingSelectCount = timingPresentCount = 0;
                flowComputeCount = 0;
                accumWeight = 0;
                minWeight = 1.0f; maxWeight = 0.0f;
                accumPairDeltaMs = 0;
                minPairDeltaMs = 1e9; maxPairDeltaMs = 0;
                stallCount = 0;
                framesSinceLog = 0;
            }
        }

        // Check for fatal error from capture thread
        if (m_sessionInvalidated.load()) {
            LOGERR("NvFBC session invalidated");
            break;
        }

        // Process Windows messages
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (msg.message == WM_QUIT) break;
    }

    // === SHUTDOWN ===
    // Wait for any pending flow before shutting down threads
    if (flowPending) {
        WaitForSingleObject(m_flowDoneEvent, 5000);
        flowPending = false;
    }

    LOG("Shutting down threads...");
    m_flowShutdown.store(true);
    if (m_flowRequestEvent) SetEvent(m_flowRequestEvent);
    if (m_flowThread) {
        DWORD waitResult = WaitForSingleObject(m_flowThread, 5000);
        if (waitResult == WAIT_TIMEOUT) {
            LOGERR("Flow thread did not exit within 5s");
        }
        CloseHandle(m_flowThread);
        m_flowThread = NULL;
    }

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
    sprintf_s(modeName, sizeof(modeName), "OptFlow-NvFBCCuda-BB-%.2f", m_targetFramerate);
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
