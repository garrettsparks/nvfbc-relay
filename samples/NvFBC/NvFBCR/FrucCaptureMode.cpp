#include "FrucCaptureMode.h"
#include <SimpleLogger.h>

FrucCaptureMode::FrucCaptureMode(float framerate)
    : m_targetFramerate(framerate == 0.0f ? 60.0f : framerate)
    , m_isVsyncMode(framerate == 0.0f)
    , m_width(0)
    , m_height(0)
    , m_cuContext(nullptr)
    , m_cuDevice(0)
    , m_cudaInitialized(false)
    , m_currentHistoryIndex(0)
    , m_capturedFrameCount(0)
    , m_device(nullptr)
{
    LOG("=== FrucCaptureMode Phase 3 ===");
    LOG("Target framerate: %.2f fps", m_targetFramerate);
    LOG("VSync mode: %s", m_isVsyncMode ? "yes" : "no");

    // Initialize frame history
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        m_frameHistory[i].d3dSurface = nullptr;
        m_frameHistory[i].cudaResource = nullptr;
        m_frameHistory[i].cudaPtr = 0;
        m_frameHistory[i].pitch = 0;
        m_frameHistory[i].timestamp.QuadPart = 0;
        m_frameHistory[i].valid = false;
        m_frameHistory[i].isMapped = false;
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
    // Unmap and unregister all CUDA resources
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        if (m_frameHistory[i].isMapped && m_frameHistory[i].cudaResource) {
            cudaGraphicsUnmapResources(1, &m_frameHistory[i].cudaResource, 0);
            m_frameHistory[i].isMapped = false;
        }
        if (m_frameHistory[i].cudaResource) {
            cudaGraphicsUnregisterResource(m_frameHistory[i].cudaResource);
            m_frameHistory[i].cudaResource = nullptr;
        }
        // Note: We don't release d3dSurface - it's owned by NvFBC
        m_frameHistory[i].d3dSurface = nullptr;
    }

    // Destroy CUDA context
    if (m_cuContext) {
        cuCtxDestroy(m_cuContext);
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

    // Get device name for logging
    char deviceName[256];
    cuDeviceGetName(deviceName, sizeof(deviceName), m_cuDevice);
    LOG("CUDA device: %s", deviceName);

    // Create CUDA context
    // Use CU_CTX_SCHED_BLOCKING_SYNC for better CPU-GPU sync
    // D3D9 interop works automatically when we register resources
    result = cuCtxCreate(&m_cuContext, CU_CTX_SCHED_BLOCKING_SYNC, m_cuDevice);
    if (result != CUDA_SUCCESS) {
        LogCudaError("cuCtxCreate", result);
        return false;
    }

    LOG("CUDA context created successfully");
    LOG("CUDA initialized successfully");
    m_cudaInitialized = true;
    return true;
}

bool FrucCaptureMode::Setup() {
    LOG("FrucCaptureMode::Setup() - Phase 3 implementation");

    // Store device pointer
    m_device = g_pD3D9Device;

    // Note: Dimensions (BUF_WIDTH/BUF_HEIGHT) are not available yet
    // They will be set in Run() after they're initialized in main

    // Initialize CUDA
    if (!InitCuda()) {
        LOGERR("Failed to initialize CUDA");
        return false;
    }

    LOG("FrucCaptureMode setup complete (dimensions and NvFBC buffers will be configured in Run)");
    return true;
}

bool FrucCaptureMode::CaptureFrame(NvFBCToDx9Vid* nvfbcDx9, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams) {
    // Get current frame history entry
    FrameHistoryEntry* entry = &m_frameHistory[m_currentHistoryIndex];

    // Set NvFBC to capture into the current ring buffer index
    grabParams->dwBufferIdx = m_currentHistoryIndex;

    // Capture frame from NvFBC
    NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);

    if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION) {
        LOGERR("NvFBC session invalidated during frame capture");
        return false;
    }

    if (fbcRes != NVFBC_SUCCESS) {
        // No new frame available - not an error, just skip
        if (m_capturedFrameCount < 10) {
            LOG("Frame %d: No new frame (bufferIdx=%d, result=0x%x)", m_capturedFrameCount, m_currentHistoryIndex, fbcRes);
        }
        return true;
    }

    // Capture timestamp
    QueryPerformanceCounter(&entry->timestamp);

    // Mark entry as valid
    entry->valid = true;
    m_capturedFrameCount++;

    // Debug first few captures
    if (m_capturedFrameCount <= 10) {
        LOG("Captured frame %d to buffer %d", m_capturedFrameCount, m_currentHistoryIndex);
    }

    // Advance ring buffer index
    m_currentHistoryIndex = (m_currentHistoryIndex + 1) % FRAME_HISTORY_SIZE;

    return true;
}

FrucCaptureMode::FrameHistoryEntry* FrucCaptureMode::GetMostRecentFrame() {
    // Find most recent valid frame
    int checkIndex = (m_currentHistoryIndex + FRAME_HISTORY_SIZE - 1) % FRAME_HISTORY_SIZE;

    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        FrameHistoryEntry* entry = &m_frameHistory[checkIndex];
        if (entry->valid) {
            return entry;
        }
        checkIndex = (checkIndex + FRAME_HISTORY_SIZE - 1) % FRAME_HISTORY_SIZE;
    }

    return nullptr;  // No valid frames yet
}

void FrucCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    LOG("FrucCaptureMode::Run() - Phase 3: Zero-copy frame history via NvFBC multi-buffer");
    LOG("Presentation: %s", m_isVsyncMode ? "VSync" : "Timed");

    // Get dimensions from globals (now initialized)
    m_width = BUF_WIDTH;
    m_height = BUF_HEIGHT;
    LOG("Frame buffer dimensions: %dx%d", m_width, m_height);

    // ===== Configure NvFBC with 4 output buffers =====
    NVFBC_TODX9VID_OUT_BUF outBuf[FRAME_HISTORY_SIZE];

    // Create D3D9 surfaces for each NvFBC output buffer
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        HRESULT hr = device->CreateOffscreenPlainSurface(
            m_width, m_height,
            D3DFMT_A2B10G10R10,  // Match NvFBC format
            D3DPOOL_DEFAULT,
            &outBuf[i].pPrimary,
            NULL
        );

        if (FAILED(hr)) {
            LOGERR("Failed to create NvFBC output surface %d (HRESULT: 0x%08x)", i, hr);
            return;
        }

        outBuf[i].pSecondary = nullptr;  // Not used
        LOG("Created NvFBC output buffer %d surface", i);
    }

    // Configure NvFBC with 4 buffers
    NVFBC_TODX9VID_SETUP_PARAMS setupParams = {};
    setupParams.dwVersion = NVFBC_TODX9VID_SETUP_PARAMS_V3_VER;
    setupParams.bWithHWCursor = 1;
    setupParams.bStereoGrab = 0;
    setupParams.bDiffMap = 0;
    setupParams.bClassificationMap = 0;
    setupParams.ppBuffer = outBuf;
    setupParams.ppDiffMap = nullptr;  // Not using diffmap
    setupParams.ppClassificationMap = nullptr;  // Not using classification
    setupParams.eMode = NVFBC_TODX9VID_ARGB10;
    setupParams.dwNumBuffers = FRAME_HISTORY_SIZE;
    setupParams.bHDRRequest = TRUE;

    NVFBCRESULT setupResult = nvfbcDx9->NvFBCToDx9VidSetUp(&setupParams);
    if (NVFBC_SUCCESS != setupResult) {
        LOGERR("Failed to reconfigure NvFBC for FRUC mode (error code: 0x%08x)", setupResult);
        LOGERR("Setup params: width=%d, height=%d, numBuffers=%d", m_width, m_height, FRAME_HISTORY_SIZE);
        return;
    }

    LOG("NvFBC configured with %d output buffers", FRAME_HISTORY_SIZE);

    // ===== Store NvFBC buffer surface pointers =====
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        // Store pointer to NvFBC buffer surface (we don't own it)
        m_frameHistory[i].d3dSurface = outBuf[i].pPrimary;
        LOG("Frame history %d -> surface %p", i, outBuf[i].pPrimary);
    }

    // TEMPORARY DIAGNOSTIC: Skip CUDA registration to see if it's preventing NvFBC writes
    LOG("WARNING: CUDA registration temporarily disabled for black screen diagnostic");

    /*
    // ===== Register NvFBC buffer surfaces with CUDA =====
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        // Register with CUDA for zero-copy interop
        cudaError_t cudaErr = cudaGraphicsD3D9RegisterResource(
            &m_frameHistory[i].cudaResource,
            m_frameHistory[i].d3dSurface,
            cudaGraphicsRegisterFlagsNone
        );

        if (cudaErr != cudaSuccess) {
            LOGERR("cudaGraphicsD3D9RegisterResource failed for buffer %d: %s", i, cudaGetErrorString(cudaErr));
            return;
        }

        LOG("Registered NvFBC buffer %d with CUDA", i);
    }

    LOG("All NvFBC buffers registered with CUDA successfully");
    */

    // ===== Main capture/presentation loop =====
    MSG msg;
    LARGE_INTEGER now;
    LONGLONG targetFrameTime = (LONGLONG)(m_perfFreq.QuadPart / m_targetFramerate);

    QueryPerformanceCounter(&m_lastPresentTime);

    int framesSinceLog = 0;
    const int LOG_INTERVAL = 300;  // Log every 300 frames (5 seconds at 60fps)

    int presentCount = 0;
    while (TRUE) {
        // Capture frame to ring buffer
        if (!CaptureFrame(nvfbcDx9, grabParams)) {
            LOGERR("Fatal error during frame capture");
            break;
        }

        // Get most recent captured frame
        FrameHistoryEntry* recentFrame = GetMostRecentFrame();

        if (presentCount < 10) {
            LOG("Present iteration %d: recentFrame=%p, surface=%p, valid=%d",
                presentCount, recentFrame, recentFrame ? recentFrame->d3dSurface : nullptr,
                recentFrame ? recentFrame->valid : 0);
        }

        if (recentFrame && recentFrame->d3dSurface) {
            // Copy most recent frame to backbuffer for presentation
            IDirect3DSurface9* backbuffer = nullptr;
            HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);

            if (presentCount < 10) {
                LOG("GetBackBuffer result: 0x%08x, backbuffer=%p", hr, backbuffer);
            }

            if (backbuffer) {
                HRESULT stretchResult = device->StretchRect(
                    recentFrame->d3dSurface, NULL,
                    backbuffer, NULL,
                    D3DTEXF_NONE
                );

                if (presentCount < 10) {
                    LOG("StretchRect result: 0x%08x", stretchResult);
                }

                backbuffer->Release();
            }
            presentCount++;

            // Present with appropriate timing
            if (m_isVsyncMode) {
                // VSync mode: present and wait for vsync
                device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_ONE);
            } else {
                // Timed mode: present when target frame time elapsed
                QueryPerformanceCounter(&now);
                LONGLONG elapsed = now.QuadPart - m_lastPresentTime.QuadPart;

                if (elapsed >= targetFrameTime) {
                    device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);
                    m_lastPresentTime = now;
                }
            }

            framesSinceLog++;
            if (framesSinceLog >= LOG_INTERVAL) {
                LOG("Phase 3 running - captured %d frames, presenting most recent", m_capturedFrameCount);
                framesSinceLog = 0;
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

    LOG("FrucCaptureMode::Run() exiting - captured %d frames total", m_capturedFrameCount);
}

const char* FrucCaptureMode::GetModeName() const {
    static char modeName[64];
    sprintf_s(modeName, sizeof(modeName), "FRUC-Phase3-%.2f", m_targetFramerate);
    return modeName;
}

void FrucCaptureMode::LogCudaError(const char* operation, CUresult result) {
    const char* errorName = nullptr;
    const char* errorString = nullptr;
    cuGetErrorName(result, &errorName);
    cuGetErrorString(result, &errorString);
    LOGERR("CUDA error in %s: %s (%s)", operation, errorName ? errorName : "unknown", errorString ? errorString : "no description");
}
