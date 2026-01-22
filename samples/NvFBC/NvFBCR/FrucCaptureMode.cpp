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
    , m_capturedFrameCount(0)
    , m_device(nullptr)
{
    LOG("=== FrucCaptureMode Phase 3 (Single-Buffer) ===");
    LOG("Target framerate: %.2f fps", m_targetFramerate);
    LOG("VSync mode: %s", m_isVsyncMode ? "yes" : "no");

    // Initialize NvFBC buffer
    m_nvfbcBuffer.d3dSurface = nullptr;
    m_nvfbcBuffer.cudaResource = nullptr;
    m_nvfbcBuffer.cudaPtr = nullptr;
    m_nvfbcBuffer.pitch = 0;
    m_nvfbcBuffer.isMapped = false;

    // Initialize CUDA buffer
    m_cudaBuffer.d3dSurface = nullptr;
    m_cudaBuffer.cudaResource = nullptr;
    m_cudaBuffer.cudaPtr = nullptr;
    m_cudaBuffer.pitch = 0;
    m_cudaBuffer.isMapped = false;

    // Get performance counter frequency
    QueryPerformanceFrequency(&m_perfFreq);
    m_lastPresentTime.QuadPart = 0;
}

FrucCaptureMode::~FrucCaptureMode() {
    LOG("=== FrucCaptureMode cleanup ===");
    Cleanup();
}

void FrucCaptureMode::Cleanup() {
    // Cleanup CUDA buffer (registered with CUDA)
    if (m_cudaBuffer.isMapped && m_cudaBuffer.cudaResource) {
        cudaGraphicsUnmapResources(1, &m_cudaBuffer.cudaResource, 0);
        m_cudaBuffer.isMapped = false;
    }
    if (m_cudaBuffer.cudaResource) {
        cudaGraphicsUnregisterResource(m_cudaBuffer.cudaResource);
        m_cudaBuffer.cudaResource = nullptr;
    }
    if (m_cudaBuffer.d3dSurface) {
        m_cudaBuffer.d3dSurface->Release();
        m_cudaBuffer.d3dSurface = nullptr;
    }

    // NvFBC buffer is NOT registered with CUDA - just clear pointer
    // Note: We don't release d3dSurface - it's owned by NvFBC
    m_nvfbcBuffer.d3dSurface = nullptr;
    m_nvfbcBuffer.cudaResource = nullptr;

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
    // Always capture to buffer index 0 (single NvFBC buffer)
    grabParams->dwBufferIdx = 0;

    // Capture frame from NvFBC
    NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);

    if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION) {
        LOGERR("NvFBC session invalidated during frame capture");
        return false;
    }

    if (fbcRes != NVFBC_SUCCESS) {
        // No new frame available - not an error, just skip
        if (m_capturedFrameCount < 10) {
            LOG("Frame %d: No new frame (result=0x%x)", m_capturedFrameCount, fbcRes);
        }
        return true;
    }

    m_capturedFrameCount++;

    // Debug first few captures
    if (m_capturedFrameCount <= 10) {
        LOG("Captured frame %d", m_capturedFrameCount);
    }

    return true;
}


void FrucCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    LOG("FrucCaptureMode::Run() - Phase 3: Single buffer capture (FRUC handles frame history)");
    LOG("Presentation: %s", m_isVsyncMode ? "VSync" : "Timed");

    // Get dimensions from globals (now initialized)
    m_width = BUF_WIDTH;
    m_height = BUF_HEIGHT;
    LOG("Frame buffer dimensions: %dx%d", m_width, m_height);

    // ===== Create single NvFBC output buffer =====
    NVFBC_TODX9VID_OUT_BUF outBuf[1];

    HRESULT hr = device->CreateOffscreenPlainSurface(
        m_width, m_height,
        D3DFMT_A2B10G10R10,  // Match NvFBC format for HDR
        D3DPOOL_DEFAULT,
        &outBuf[0].pPrimary,
        NULL
    );

    if (FAILED(hr)) {
        LOGERR("Failed to create NvFBC output surface (HRESULT: 0x%08x)", hr);
        return;
    }

    outBuf[0].pSecondary = nullptr;
    m_nvfbcBuffer.d3dSurface = outBuf[0].pPrimary;
    LOG("Created NvFBC output surface: %p", m_nvfbcBuffer.d3dSurface);

    // ===== Configure NvFBC with single buffer =====
    NVFBC_TODX9VID_SETUP_PARAMS setupParams = {};
    setupParams.dwVersion = NVFBC_TODX9VID_SETUP_PARAMS_V3_VER;
    setupParams.bWithHWCursor = 1;
    setupParams.bStereoGrab = 0;
    setupParams.bDiffMap = 0;
    setupParams.bClassificationMap = 0;
    setupParams.ppBuffer = outBuf;
    setupParams.ppDiffMap = nullptr;
    setupParams.ppClassificationMap = nullptr;
    setupParams.eMode = NVFBC_TODX9VID_ARGB10;
    setupParams.dwNumBuffers = 1;
    setupParams.bHDRRequest = TRUE;

    NVFBCRESULT setupResult = nvfbcDx9->NvFBCToDx9VidSetUp(&setupParams);
    if (NVFBC_SUCCESS != setupResult) {
        LOGERR("Failed to configure NvFBC for FRUC mode (error code: 0x%08x)", setupResult);
        return;
    }

    LOG("NvFBC configured with single output buffer");

    // ===== Create CUDA buffer for FRUC input =====
    hr = device->CreateOffscreenPlainSurface(
        m_width, m_height,
        D3DFMT_A2B10G10R10,
        D3DPOOL_DEFAULT,
        &m_cudaBuffer.d3dSurface,
        NULL
    );

    if (FAILED(hr)) {
        LOGERR("Failed to create CUDA buffer surface (HRESULT: 0x%08x)", hr);
        return;
    }

    LOG("Created CUDA buffer surface: %p", m_cudaBuffer.d3dSurface);

    // Register CUDA buffer with CUDA for future FRUC use
    cudaError_t cudaErr = cudaGraphicsD3D9RegisterResource(
        &m_cudaBuffer.cudaResource,
        m_cudaBuffer.d3dSurface,
        cudaGraphicsRegisterFlagsNone
    );

    if (cudaErr != cudaSuccess) {
        LOGERR("CUDA registration failed: %s", cudaGetErrorString(cudaErr));
        return;
    }

    LOG("CUDA buffer registered successfully");

    // ===== Main capture/presentation loop =====
    MSG msg;
    LARGE_INTEGER now;
    LONGLONG targetFrameTime = (LONGLONG)(m_perfFreq.QuadPart / m_targetFramerate);

    QueryPerformanceCounter(&m_lastPresentTime);

    int framesSinceLog = 0;
    const int LOG_INTERVAL = 300;  // Log every 300 frames (~5 seconds at 60fps)

    while (TRUE) {
        // Capture frame from NvFBC
        if (!CaptureFrame(nvfbcDx9, grabParams)) {
            LOGERR("Fatal error during frame capture");
            break;
        }

        // Copy captured frame to CUDA buffer (for Phase 4 FRUC input)
        if (m_nvfbcBuffer.d3dSurface && m_cudaBuffer.d3dSurface) {
            hr = device->StretchRect(
                m_nvfbcBuffer.d3dSurface, NULL,
                m_cudaBuffer.d3dSurface, NULL,
                D3DTEXF_NONE
            );

            // TODO Phase 4: Pass m_cudaBuffer to FRUC here
            // FRUC maintains its own internal frame history
        }

        // Present the captured frame
        if (m_nvfbcBuffer.d3dSurface) {
            IDirect3DSurface9* backbuffer = nullptr;
            hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);

            if (backbuffer) {
                device->StretchRect(
                    m_nvfbcBuffer.d3dSurface, NULL,
                    backbuffer, NULL,
                    D3DTEXF_NONE
                );
                backbuffer->Release();
            }

            // Present with appropriate timing
            if (m_isVsyncMode) {
                device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_ONE);
            } else {
                QueryPerformanceCounter(&now);
                LONGLONG elapsed = now.QuadPart - m_lastPresentTime.QuadPart;

                if (elapsed >= targetFrameTime) {
                    device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);
                    m_lastPresentTime = now;
                }
            }

            framesSinceLog++;
            if (framesSinceLog >= LOG_INTERVAL) {
                LOG("Phase 3 running - captured %d frames", m_capturedFrameCount);
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
