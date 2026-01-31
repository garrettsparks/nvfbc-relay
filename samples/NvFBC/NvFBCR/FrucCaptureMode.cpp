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
    , m_currentInputIndex(0)
    , m_capturedFrameCount(0)
    , m_frucModule(nullptr)
    , m_frucHandle(nullptr)
    , m_frucCreate(nullptr)
    , m_frucRegisterResource(nullptr)
    , m_frucUnregisterResource(nullptr)
    , m_frucProcess(nullptr)
    , m_frucDestroy(nullptr)
    , m_frucInitialized(false)
    , m_device(nullptr)
    , m_interpolatedFrameCount(0)
    , m_fallbackFrameCount(0)
{
    LOG("=== FrucCaptureMode Phase 4 (FRUC Interpolation) ===");
    LOG("Target framerate: %.2f fps", m_targetFramerate);
    LOG("VSync mode: %s", m_isVsyncMode ? "yes" : "no");

    // Initialize NvFBC buffer
    m_nvfbcBuffer.d3dSurface = nullptr;
    m_nvfbcBuffer.cudaResource = nullptr;
    m_nvfbcBuffer.cudaPtr = 0;
    m_nvfbcBuffer.pitch = 0;
    m_nvfbcBuffer.isMapped = false;

    // Initialize input buffers (2 for FRUC - alternating)
    for (int i = 0; i < NUM_INPUT_BUFFERS; i++) {
        m_inputBuffers[i].d3dSurface = nullptr;
        m_inputBuffers[i].cudaResource = nullptr;
        m_inputBuffers[i].cudaPtr = 0;
        m_inputBuffers[i].pitch = 0;
        m_inputBuffers[i].isMapped = false;
    }

    // Initialize output buffer (from FRUC)
    m_outputBuffer.d3dSurface = nullptr;
    m_outputBuffer.cudaResource = nullptr;
    m_outputBuffer.cudaPtr = 0;
    m_outputBuffer.pitch = 0;
    m_outputBuffer.isMapped = false;

    // Get performance counter frequency
    QueryPerformanceFrequency(&m_perfFreq);
    m_lastPresentTime.QuadPart = 0;
    m_captureStartTime.QuadPart = 0;
}

FrucCaptureMode::~FrucCaptureMode() {
    LOG("=== FrucCaptureMode cleanup ===");
    Cleanup();
}

void FrucCaptureMode::Cleanup() {
    LOG("Cleanup starting...");

    // Log final statistics
    if (m_interpolatedFrameCount > 0 || m_fallbackFrameCount > 0) {
        int total = m_interpolatedFrameCount + m_fallbackFrameCount;
        float successRate = (float)m_interpolatedFrameCount / (float)total * 100.0f;
        LOG("Final statistics:");
        LOG("  Total frames processed: %d", total);
        LOG("  Interpolated: %d", m_interpolatedFrameCount);
        LOG("  Fallbacks: %d", m_fallbackFrameCount);
        LOG("  Success rate: %.1f%%", successRate);
    }

    // Destroy FRUC handle first (before unregistering resources)
    if (m_frucHandle && m_frucDestroy) {
        LOG("Destroying FRUC handle...");
        m_frucDestroy(m_frucHandle);
        m_frucHandle = nullptr;
    }

    // Free CUDA output buffer
    if (m_outputBuffer.cudaPtr) {
        cuMemFree(m_outputBuffer.cudaPtr);
        m_outputBuffer.cudaPtr = 0;
    }

    // Free CUDA input buffers (both)
    for (int i = 0; i < NUM_INPUT_BUFFERS; i++) {
        if (m_inputBuffers[i].cudaPtr) {
            cuMemFree(m_inputBuffers[i].cudaPtr);
            m_inputBuffers[i].cudaPtr = 0;
        }
    }

    // Unload FRUC DLL
    if (m_frucModule) {
        FreeLibrary(m_frucModule);
        m_frucModule = nullptr;
    }
    m_frucInitialized = false;

    // NvFBC buffer - just clear pointer (owned by NvFBC)
    m_nvfbcBuffer.d3dSurface = nullptr;

    // Destroy CUDA context
    if (m_cuContext) {
        cuCtxDestroy(m_cuContext);
        m_cuContext = nullptr;
    }

    m_cudaInitialized = false;
    LOG("Cleanup complete");
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

bool FrucCaptureMode::InitFruc() {
    LOG("Initializing FRUC...");

    // Load NvOFFRUC.dll (from NVIDIA Optical Flow SDK)
    m_frucModule = LoadLibraryA("NvOFFRUC.dll");
    if (!m_frucModule) {
        DWORD error = GetLastError();
        LOGERR("Failed to load NvOFFRUC.dll (error: %d)", error);
        LOGERR("Download Optical Flow SDK from https://developer.nvidia.com/optical-flow-sdk");
        LOGERR("Copy NvOFFRUC.dll from NvOFFRUC/NvOFFRUCSample/bin/win64/ to application directory");
        return false;
    }

    LOG("Loaded NvOFFRUC.dll");

    // Get function pointers
    m_frucCreate = (PtrToFuncNvOFFRUCCreate)GetProcAddress(m_frucModule, CreateProcName);
    m_frucRegisterResource = (PtrToFuncNvOFFRUCRegisterResource)GetProcAddress(m_frucModule, RegisterResourceProcName);
    m_frucUnregisterResource = (PtrToFuncNvOFFRUCUnregisterResource)GetProcAddress(m_frucModule, UnregisterResourceProcName);
    m_frucProcess = (PtrToFuncNvOFFRUCProcess)GetProcAddress(m_frucModule, ProcessProcName);
    m_frucDestroy = (PtrToFuncNvOFFRUCDestroy)GetProcAddress(m_frucModule, DestroyProcName);

    if (!m_frucCreate || !m_frucRegisterResource || !m_frucUnregisterResource ||
        !m_frucProcess || !m_frucDestroy) {
        LOGERR("Failed to get FRUC function pointers");
        LOGERR("  Create: %p, Register: %p, Unregister: %p, Process: %p, Destroy: %p",
               m_frucCreate, m_frucRegisterResource, m_frucUnregisterResource,
               m_frucProcess, m_frucDestroy);
        return false;
    }

    LOG("Got all FRUC function pointers");
    return true;
}

bool FrucCaptureMode::CreateFrameBuffers() {
    LOG("Creating frame buffers for FRUC...");

    // Ensure CUDA context is current
    cuCtxPushCurrent(m_cuContext);

    // Calculate pitch (width * 4 bytes per pixel for ARGB) - use linear allocation like the sample
    size_t pitch = (size_t)m_width * 4;
    size_t bufferSize = pitch * m_height;

    // Allocate 2 CUDA input buffers (for alternating frames - FRUC needs both to compute optical flow)
    for (int i = 0; i < NUM_INPUT_BUFFERS; i++) {
        CUresult result = cuMemAlloc(&m_inputBuffers[i].cudaPtr, bufferSize);

        if (result != CUDA_SUCCESS) {
            LogCudaError("cuMemAlloc (input)", result);
            CUcontext poppedCtx;
            cuCtxPopCurrent(&poppedCtx);
            return false;
        }

        m_inputBuffers[i].pitch = pitch;
        LOG("Input buffer %d allocated: ptr=0x%llx, pitch=%zu, size=%zu", i, m_inputBuffers[i].cudaPtr, pitch, bufferSize);
    }

    // Allocate CUDA output buffer
    CUresult result = cuMemAlloc(&m_outputBuffer.cudaPtr, bufferSize);

    if (result != CUDA_SUCCESS) {
        LogCudaError("cuMemAlloc (output)", result);
        CUcontext poppedCtx;
        cuCtxPopCurrent(&poppedCtx);
        return false;
    }

    m_outputBuffer.pitch = pitch;
    LOG("Output buffer allocated: ptr=0x%llx, pitch=%zu, size=%zu", m_outputBuffer.cudaPtr, pitch, bufferSize);

    // Create FRUC instance
    NvOFFRUC_CREATE_PARAM createParams = {};
    createParams.uiWidth = m_width;
    createParams.uiHeight = m_height;
    createParams.pDevice = NULL;  // NULL for CUDA resources (only D3D11 uses a device pointer)
    createParams.eResourceType = CudaResource;
    createParams.eSurfaceFormat = ARGBSurface;
    createParams.eCUDAResourceType = CudaResourceCuDevicePtr;

    NvOFFRUC_STATUS frucStatus = m_frucCreate(&createParams, &m_frucHandle);
    if (frucStatus != NvOFFRUC_SUCCESS) {
        LogFrucError("NvOFFRUCCreate", frucStatus);
        CUcontext poppedCtx;
        cuCtxPopCurrent(&poppedCtx);
        return false;
    }

    LOG("FRUC instance created");

    // Register resources with FRUC (3 total: 2 input buffers + 1 output buffer)
    // FRUC needs 2 separate input buffers to access previous and current frame for optical flow
    NvOFFRUC_REGISTER_RESOURCE_PARAM registerParams = {};
    registerParams.pArrResource[0] = (void*)m_inputBuffers[0].cudaPtr;
    registerParams.pArrResource[1] = (void*)m_inputBuffers[1].cudaPtr;
    registerParams.pArrResource[2] = (void*)m_outputBuffer.cudaPtr;
    registerParams.uiCount = 3;

    frucStatus = m_frucRegisterResource(m_frucHandle, &registerParams);
    if (frucStatus != NvOFFRUC_SUCCESS) {
        LogFrucError("NvOFFRUCRegisterResource", frucStatus);
        CUcontext poppedCtx;
        cuCtxPopCurrent(&poppedCtx);
        return false;
    }

    LOG("Registered %d resources with FRUC (2 input + 1 output)", registerParams.uiCount);
    m_frucInitialized = true;

    CUcontext poppedCtx;
    cuCtxPopCurrent(&poppedCtx);

    LOG("Frame buffers created and FRUC initialized successfully");
    return true;
}

void FrucCaptureMode::LogFrucError(const char* operation, NvOFFRUC_STATUS status) {
    const char* errorStr = "Unknown error";
    if (status >= 0 && status < NvOFFRUC_ERR_MAX_ERROR) {
        errorStr = NvOFFRUCErrorString[status];
    }
    LOGERR("FRUC error in %s: %s (code: %d)", operation, errorStr, status);
}

bool FrucCaptureMode::Setup() {
    LOG("FrucCaptureMode::Setup() - Phase 4 implementation");

    // Store device pointer
    m_device = g_pD3D9Device;

    // Note: Dimensions (BUF_WIDTH/BUF_HEIGHT) are not available yet
    // They will be set in Run() after they're initialized in main

    // Initialize CUDA
    if (!InitCuda()) {
        LOGERR("Failed to initialize CUDA");
        return false;
    }

    // Load FRUC DLL (actual FRUC initialization happens in Run when dimensions are known)
    if (!InitFruc()) {
        LOGERR("Failed to load FRUC");
        return false;
    }

    LOG("FrucCaptureMode setup complete (dimensions and NvFBC buffers will be configured in Run)");
    return true;
}

// Returns: -1 = fatal error, 0 = no new frame (same as before), 1 = new frame captured
int FrucCaptureMode::CaptureFrame(NvFBCToDx9Vid* nvfbcDx9, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams) {
    // Always capture to buffer index 0 (single NvFBC buffer)
    grabParams->dwBufferIdx = 0;

    // Capture frame from NvFBC
    NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);

    if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION) {
        LOGERR("NvFBC session invalidated during frame capture");
        return -1;  // Fatal error
    }

    if (fbcRes != NVFBC_SUCCESS) {
        // No new frame available - same frame as before
        return 0;
    }

    m_capturedFrameCount++;

    // Debug first few captures
    if (m_capturedFrameCount <= 10) {
        LOG("Captured frame %d", m_capturedFrameCount);
    }

    return 1;  // New frame captured
}


void FrucCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    LOG("FrucCaptureMode::Run() - Phase 4: FRUC Optical Flow Interpolation");
    LOG("Presentation: %s", m_isVsyncMode ? "VSync" : "Timed");

    // Get dimensions from globals (now initialized)
    m_width = BUF_WIDTH;
    m_height = BUF_HEIGHT;
    LOG("Frame buffer dimensions: %dx%d", m_width, m_height);

    // ===== Create single NvFBC output buffer =====
    NVFBC_TODX9VID_OUT_BUF outBuf[1];

    HRESULT hr = device->CreateOffscreenPlainSurface(
        m_width, m_height,
        D3DFMT_A8R8G8B8,  // Standard 8-bit ARGB (matches FRUC's ARGBSurface format)
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
    setupParams.eMode = NVFBC_TODX9VID_ARGB;  // Standard 8-bit ARGB for FRUC compatibility
    setupParams.dwNumBuffers = 1;
    setupParams.bHDRRequest = FALSE;

    NVFBCRESULT setupResult = nvfbcDx9->NvFBCToDx9VidSetUp(&setupParams);
    if (NVFBC_SUCCESS != setupResult) {
        LOGERR("Failed to configure NvFBC for FRUC mode (error code: 0x%08x)", setupResult);
        return;
    }

    LOG("NvFBC configured with single output buffer");

    // ===== Create CUDA frame buffers and initialize FRUC =====
    if (!CreateFrameBuffers()) {
        LOGERR("Failed to create frame buffers for FRUC");
        return;
    }

    // ===== Create D3D surface for output presentation =====
    IDirect3DSurface9* outputSurface = nullptr;
    hr = device->CreateOffscreenPlainSurface(
        m_width, m_height,
        D3DFMT_A8R8G8B8,  // Match FRUC output format
        D3DPOOL_DEFAULT,
        &outputSurface,
        NULL
    );

    if (FAILED(hr)) {
        LOGERR("Failed to create output surface (HRESULT: 0x%08x)", hr);
        return;
    }

    LOG("Created output presentation surface");

    // ===== Timing setup =====
    MSG msg;
    LARGE_INTEGER now, captureTime, nextPresentTime;
    LONGLONG frameIntervalTicks = (LONGLONG)(m_perfFreq.QuadPart / m_targetFramerate);

    // Buffer delay: 2 output frames worth of time
    // This ensures we always have a "future" frame for interpolation
    LONGLONG bufferDelayTicks = frameIntervalTicks * 2;

    QueryPerformanceCounter(&m_captureStartTime);

    // First presentation happens after buffer delay
    nextPresentTime.QuadPart = m_captureStartTime.QuadPart + bufferDelayTicks;

    int framesSinceLog = 0;
    const int LOG_INTERVAL = 300;  // Log every 300 frames (~5 seconds at 60fps)

    double bufferDelayMs = (double)(bufferDelayTicks * 1000) / (double)m_perfFreq.QuadPart;
    LOG("Starting FRUC interpolation loop at %.2f fps", m_targetFramerate);
    LOG("Buffer delay: %.2f ms (presentation starts after buffer fills)", bufferDelayMs);

    // Track whether we have a valid interpolated frame ready for presentation
    bool outputFrameReady = false;

    while (TRUE) {
        // ===== CAPTURE PHASE: Feed EVERY NEW frame to FRUC =====
        // CRITICAL: We must capture and feed every frame the source produces
        // (e.g., all 90 frames if game runs at 90fps). FRUC uses consecutive
        // frame pairs for optical flow and interpolation.
        int captureResult = CaptureFrame(nvfbcDx9, grabParams);
        if (captureResult == -1) {
            LOGERR("Fatal error during frame capture");
            break;
        }

        // Only process if we got a NEW frame (captureResult == 1)
        // Skip if same frame as before (captureResult == 0)
        if (captureResult == 1 && m_nvfbcBuffer.d3dSurface && m_inputBuffers[m_currentInputIndex].cudaPtr) {
            QueryPerformanceCounter(&captureTime);
            D3DLOCKED_RECT lockedRect;
            hr = m_nvfbcBuffer.d3dSurface->LockRect(&lockedRect, NULL, D3DLOCK_READONLY);

            if (SUCCEEDED(hr)) {
                if (m_capturedFrameCount <= 3) {
                    LOG("Frame %d: LockRect succeeded, pitch=%d", m_capturedFrameCount, lockedRect.Pitch);
                }

                // Copy to current input buffer (alternating between 0 and 1)
                FrameBuffer& currentInput = m_inputBuffers[m_currentInputIndex];

                CUDA_MEMCPY2D copyParams = {};
                copyParams.srcMemoryType = CU_MEMORYTYPE_HOST;
                copyParams.srcHost = lockedRect.pBits;
                copyParams.srcPitch = lockedRect.Pitch;
                copyParams.dstMemoryType = CU_MEMORYTYPE_DEVICE;
                copyParams.dstDevice = currentInput.cudaPtr;
                copyParams.dstPitch = currentInput.pitch;
                copyParams.WidthInBytes = m_width * 4;
                copyParams.Height = m_height;

                CUresult copyResult = cuMemcpy2D(&copyParams);
                m_nvfbcBuffer.d3dSurface->UnlockRect();

                if (m_capturedFrameCount <= 3) {
                    LOG("Frame %d: CUDA memcpy result=%d, buffer=%d", m_capturedFrameCount, copyResult, m_currentInputIndex);
                }

                if (copyResult != CUDA_SUCCESS && m_capturedFrameCount < 10) {
                    LogCudaError("cuMemcpy2D (D3D->CUDA)", copyResult);
                }

                // Feed this frame to FRUC with its capture timestamp
                // FRUC internally uses the previous frame (in the other buffer) for optical flow
                double inputTimestamp = (double)(captureTime.QuadPart - m_captureStartTime.QuadPart) / (double)m_perfFreq.QuadPart;
                double outputTimestamp = (double)(nextPresentTime.QuadPart - m_captureStartTime.QuadPart) / (double)m_perfFreq.QuadPart;

                if (m_capturedFrameCount <= 3) {
                    LOG("Frame %d: Calling NvOFFRUCProcess (inputTs=%.3f, outputTs=%.3f)",
                        m_capturedFrameCount, inputTimestamp, outputTimestamp);
                }

                // Ensure CUDA context is current before calling FRUC
                cuCtxPushCurrent(m_cuContext);

                NvOFFRUC_PROCESS_IN_PARAMS inParams = {};
                inParams.stFrameDataInput.pFrame = (void*)currentInput.cudaPtr;
                inParams.stFrameDataInput.nTimeStamp = inputTimestamp;
                inParams.stFrameDataInput.nCuSurfacePitch = currentInput.pitch;
                // First frame: just cache it (no previous frame to interpolate from)
                // Subsequent frames: generate interpolated output
                inParams.bSkipWarp = (m_capturedFrameCount == 1) ? 1 : 0;

                NvOFFRUC_PROCESS_OUT_PARAMS outParams = {};
                outParams.stFrameDataOutput.pFrame = (void*)m_outputBuffer.cudaPtr;
                outParams.stFrameDataOutput.nTimeStamp = outputTimestamp;  // Target presentation time
                outParams.stFrameDataOutput.nCuSurfacePitch = m_outputBuffer.pitch;

                NvOFFRUC_STATUS frucStatus = NvOFFRUC_ERR_GENERIC;
                __try {
                    frucStatus = m_frucProcess(m_frucHandle, &inParams, &outParams);
                }
                __except(EXCEPTION_EXECUTE_HANDLER) {
                    LOGERR("EXCEPTION in NvOFFRUCProcess! Code: 0x%08x", GetExceptionCode());
                }

                CUcontext poppedCtx;
                cuCtxPopCurrent(&poppedCtx);

                if (m_capturedFrameCount <= 3) {
                    LOG("Frame %d: NvOFFRUCProcess returned %d", m_capturedFrameCount, frucStatus);
                }

                if (frucStatus == NvOFFRUC_SUCCESS) {
                    outputFrameReady = true;
                }

                // Alternate to the other input buffer for the next frame
                m_currentInputIndex = (m_currentInputIndex + 1) % NUM_INPUT_BUFFERS;
            }
        }

        // ===== PRESENTATION PHASE: Present at fixed rate (e.g., 60fps) =====
        QueryPerformanceCounter(&now);

        if (now.QuadPart >= nextPresentTime.QuadPart) {
            bool frucSuccess = outputFrameReady;

            if (frucSuccess) {
                m_interpolatedFrameCount++;

                // Copy FRUC output (CUDA) to D3D surface for presentation
                D3DLOCKED_RECT lockedRect;
                hr = outputSurface->LockRect(&lockedRect, NULL, 0);
                if (SUCCEEDED(hr)) {
                    CUDA_MEMCPY2D copyParams = {};
                    copyParams.srcMemoryType = CU_MEMORYTYPE_DEVICE;
                    copyParams.srcDevice = m_outputBuffer.cudaPtr;
                    copyParams.srcPitch = m_outputBuffer.pitch;
                    copyParams.dstMemoryType = CU_MEMORYTYPE_HOST;
                    copyParams.dstHost = lockedRect.pBits;
                    copyParams.dstPitch = lockedRect.Pitch;
                    copyParams.WidthInBytes = m_width * 4;
                    copyParams.Height = m_height;

                    cuMemcpy2D(&copyParams);
                    outputSurface->UnlockRect();
                }

                // Present interpolated frame
                IDirect3DSurface9* backbuffer = nullptr;
                hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
                if (backbuffer) {
                    device->StretchRect(outputSurface, NULL, backbuffer, NULL, D3DTEXF_NONE);
                    backbuffer->Release();
                }
            } else {
                m_fallbackFrameCount++;

                // Fallback: present the most recent captured frame
                IDirect3DSurface9* backbuffer = nullptr;
                hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
                if (backbuffer) {
                    device->StretchRect(m_nvfbcBuffer.d3dSurface, NULL, backbuffer, NULL, D3DTEXF_NONE);
                    backbuffer->Release();
                }
            }

            // Present the frame
            if (m_isVsyncMode) {
                device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_ONE);
            } else {
                device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);
            }

            // Schedule next presentation
            nextPresentTime.QuadPart += frameIntervalTicks;

            // If we've fallen behind, catch up (skip frames rather than queue them)
            if (nextPresentTime.QuadPart < now.QuadPart) {
                LONGLONG behind = now.QuadPart - nextPresentTime.QuadPart;
                LONGLONG framesToSkip = behind / frameIntervalTicks + 1;
                nextPresentTime.QuadPart += framesToSkip * frameIntervalTicks;
                LOG("Presentation fell behind, skipping %lld frames", framesToSkip);
            }

            framesSinceLog++;
            if (framesSinceLog >= LOG_INTERVAL) {
                float successRate = 0.0f;
                int total = m_interpolatedFrameCount + m_fallbackFrameCount;
                if (total > 0) {
                    successRate = (float)m_interpolatedFrameCount / (float)total * 100.0f;
                }
                LOG("Phase 4 running - captured %d, interpolated %d, fallback %d (%.1f%% success)",
                    m_capturedFrameCount, m_interpolatedFrameCount, m_fallbackFrameCount, successRate);
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

    // Cleanup output surface
    if (outputSurface) {
        outputSurface->Release();
    }

    LOG("FrucCaptureMode::Run() exiting - captured %d frames total", m_capturedFrameCount);
}

const char* FrucCaptureMode::GetModeName() const {
    static char modeName[64];
    sprintf_s(modeName, sizeof(modeName), "FRUC-%.0ffps", m_targetFramerate);
    return modeName;
}

void FrucCaptureMode::LogCudaError(const char* operation, CUresult result) {
    const char* errorName = nullptr;
    const char* errorString = nullptr;
    cuGetErrorName(result, &errorName);
    cuGetErrorString(result, &errorString);
    LOGERR("CUDA error in %s: %s (%s)", operation, errorName ? errorName : "unknown", errorString ? errorString : "no description");
}
