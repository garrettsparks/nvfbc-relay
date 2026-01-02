#include "VsyncTemporalCaptureMode.h"
#include <SimpleLogger.h>
#include <string>

// Forward declaration of DisplayPosition struct
struct DisplayPosition {
    int dxAdapterIndex;
    RECT position;
    char deviceName[32];
    std::string friendlyName;
};

// External global variables
extern IDirect3D9Ex* g_pD3DEx;
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;
extern DisplayPosition target;

VsyncTemporalCaptureMode::VsyncTemporalCaptureMode()
    : m_currentHistoryIndex(0)
    , m_captureTarget(NULL)
    , m_targetFramerate(60.0f)  // Default, will be detected
    , m_device(NULL)
{
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        m_frameHistory[i].surface = NULL;
        m_frameHistory[i].valid = false;
        m_frameHistory[i].timestamp.QuadPart = 0;
    }
    m_perfFreq.QuadPart = 0;
}

VsyncTemporalCaptureMode::~VsyncTemporalCaptureMode() {
    // Release frame history surfaces
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        if (m_frameHistory[i].surface) {
            m_frameHistory[i].surface->Release();
            m_frameHistory[i].surface = NULL;
        }
    }

    if (m_captureTarget) {
        m_captureTarget->Release();
        m_captureTarget = NULL;
    }
}

UINT VsyncTemporalCaptureMode::GetPresentationInterval() const {
    return D3DPRESENT_INTERVAL_ONE;
}

bool VsyncTemporalCaptureMode::Setup() {
    m_device = g_pD3D9Device;

    // Detect target display refresh rate
    D3DDISPLAYMODE displayMode;
    HRESULT hr = g_pD3DEx->GetAdapterDisplayMode(target.dxAdapterIndex, &displayMode);
    if (SUCCEEDED(hr)) {
        m_targetFramerate = (float)displayMode.RefreshRate;
        LOG("VSync temporal mode detected target refresh rate: %.2f Hz", m_targetFramerate);
    } else {
        LOG("Failed to detect refresh rate (error: 0x%08x), defaulting to 60.0 Hz", hr);
        m_targetFramerate = 60.0f;
    }

    // Create capture target surface (where NvFBC will write)
    hr = m_device->CreateOffscreenPlainSurface(
        BUF_WIDTH, BUF_HEIGHT,
        D3DFMT_A2B10G10R10,
        D3DPOOL_DEFAULT,
        &m_captureTarget,
        NULL);

    if (FAILED(hr)) {
        LOGERR("Failed to create capture target surface (error: 0x%08x)", hr);
        return false;
    }

    // Create frame history surfaces
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        hr = m_device->CreateOffscreenPlainSurface(
            BUF_WIDTH, BUF_HEIGHT,
            D3DFMT_A2B10G10R10,
            D3DPOOL_DEFAULT,
            &m_frameHistory[i].surface,
            NULL);

        if (FAILED(hr)) {
            LOGERR("Failed to create frame history surface %d (error: 0x%08x)", i, hr);
            return false;
        }
    }

    QueryPerformanceFrequency(&m_perfFreq);

    LOG("VSync temporal mode initialized - target framerate: %.2f fps (auto-detected)", m_targetFramerate);
    LOG("Frame history size: %d (temporal frame selection for smooth VRR capture)", FRAME_HISTORY_SIZE);
    LOG("Output FPS will match target monitor's refresh rate via VSync");
    return true;
}

void VsyncTemporalCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    MSG msg;
    LARGE_INTEGER nextPresentTime, currentTime;
    QueryPerformanceCounter(&nextPresentTime);
    LONGLONG ticksPerFrame = (LONGLONG)(m_perfFreq.QuadPart / m_targetFramerate);

    // Update NvFBC to write to our capture target instead of backbuffer
    NVFBC_TODX9VID_OUT_BUF outBuf[1];
    outBuf[0].pPrimary = m_captureTarget;

    NVFBC_TODX9VID_SETUP_PARAMS setupParams = {};
    setupParams.dwVersion = NVFBC_TODX9VID_SETUP_PARAMS_V3_VER;
    setupParams.bWithHWCursor = 1;
    setupParams.bStereoGrab = 0;
    setupParams.bDiffMap = 0;
    setupParams.ppBuffer = outBuf;
    setupParams.eMode = NVFBC_TODX9VID_ARGB10;
    setupParams.dwNumBuffers = 1;
    setupParams.bHDRRequest = TRUE;

    if (NVFBC_SUCCESS != nvfbcDx9->NvFBCToDx9VidSetUp(&setupParams)) {
        LOGERR("Failed to reconfigure NvFBC for vsync temporal mode");
        return;
    }

    while (TRUE)
    {
        QueryPerformanceCounter(&currentTime);
        LONGLONG timeUntilPresent = nextPresentTime.QuadPart - currentTime.QuadPart;

        // Poll for latest frame (NOWAIT - never blocks)
        NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);

        if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("NvFBC session invalidated - session needs to be recreated");
            break;
        }

        // Only store frame if we got a new one and we're close to present time (within 2 frame periods)
        if (fbcRes == NVFBC_SUCCESS && timeUntilPresent < (ticksPerFrame * 2)) {
            QueryPerformanceCounter(&currentTime);

            // Copy captured frame to current history slot
            RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };
            device->StretchRect(
                m_captureTarget,
                &srcRect,
                m_frameHistory[m_currentHistoryIndex].surface,
                &srcRect,
                D3DTEXF_NONE);

            // Update timestamp and mark valid
            m_frameHistory[m_currentHistoryIndex].timestamp = currentTime;
            m_frameHistory[m_currentHistoryIndex].valid = true;

            // Advance to next slot
            m_currentHistoryIndex = (m_currentHistoryIndex + 1) % FRAME_HISTORY_SIZE;
        }

        // Check if it's time to present
        QueryPerformanceCounter(&currentTime);
        if (currentTime.QuadPart >= nextPresentTime.QuadPart) {
            // Select best frame from history based on target present time
            SelectFrameToBackbuffer(nextPresentTime, g_backbuffer);

            // Present with VSync - this blocks until monitor refresh
            device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_ONE);

            // Update next present time based on actual present time
            QueryPerformanceCounter(&currentTime);
            nextPresentTime.QuadPart = currentTime.QuadPart + ticksPerFrame;
        }

        // Process Windows messages
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (msg.message == WM_QUIT)
            break;
    }
}

const char* VsyncTemporalCaptureMode::GetModeName() const {
    return "VsyncTemporal";
}

void VsyncTemporalCaptureMode::SelectFrameToBackbuffer(LARGE_INTEGER targetTime, IDirect3DSurface9* backbuffer) {
    // Find the two frames that bracket the target time
    int bestBefore = -1;
    int bestAfter = -1;
    LONGLONG smallestBeforeDiff = LLONG_MAX;
    LONGLONG smallestAfterDiff = LLONG_MAX;

    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        if (!m_frameHistory[i].valid) continue;

        LONGLONG diff = targetTime.QuadPart - m_frameHistory[i].timestamp.QuadPart;

        if (diff >= 0 && diff < smallestBeforeDiff) {
            smallestBeforeDiff = diff;
            bestBefore = i;
        }
        else if (diff < 0 && -diff < smallestAfterDiff) {
            smallestAfterDiff = -diff;
            bestAfter = i;
        }
    }

    RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };

    // Prefer frames from the past (before target time)
    if (bestBefore >= 0 && bestAfter >= 0) {
        // Both available - use the "before" frame
        m_device->StretchRect(
            m_frameHistory[bestBefore].surface,
            &srcRect,
            backbuffer,
            &srcRect,
            D3DTEXF_NONE);
    }
    else if (bestBefore >= 0) {
        // Only have a "before" frame, use it
        m_device->StretchRect(
            m_frameHistory[bestBefore].surface,
            &srcRect,
            backbuffer,
            &srcRect,
            D3DTEXF_NONE);
    }
    else if (bestAfter >= 0) {
        // Only have an "after" frame, use it
        m_device->StretchRect(
            m_frameHistory[bestAfter].surface,
            &srcRect,
            backbuffer,
            &srcRect,
            D3DTEXF_NONE);
    }
    // If no valid frames, backbuffer will just show whatever was there before
}