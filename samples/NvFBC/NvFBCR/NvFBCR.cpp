/*!
 * \defgroup NvFBCR
 * \brief
 * Nvidia Framebuffer Capture Relay
 *
 * This is a simple application that captures display to a DirectX 9 surface,
 * sets up a pseudo-fullscreen window on another display,
 * and presents that surface on the window through the same DX9 context.
 *
 * It is very efficient because everything remains in VRAM and as few copies as possible are done to achieve this.
 * Many features and frills typical of capture software are omitted for speed and downstream flexibility.
 *
 * The process is as follows:
 *
 * Discover all physical heads on the system and ask the user to choose a capture and target display with a capture interval.
 * Create a window (in windowed mode) on the target display in pseudo-fullscreen, i.e., no borders or bar.
 * Create a DX9 context for that window and get a pointer to its single backbuffer as a DX9 surface (using immediate presentation).
 * Set up NvFBC capture device with the window's backbuffer as the target capture surface.
 * In a loop,
 *      Call NvFBC capture, thereby writing the captured frame directly to the window's backbuffer with no copies involved.
 *      Call Present.
 *
 *
 * Potential improvement is to use FLIPEX presentation model. Because this is a DX9 window not using FLIPEX presentation,
 *      a blt copy is performed as DWM manages the render. I couldn't get FLIPEX to work, primarily because
 *
 * https://learn.microsoft.com/en-us/windows/win32/direct3darticles/direct3d-9ex-improvements#direct3d-9ex-flip-mode-presentation
 * "If Windowed is set to TRUE and SwapEffect is set to D3DSWAPEFFECT_FLIPEX, the runtime creates one extra back buffer and rotates
 * whichever handle belongs to the buffer that becomes the front buffer at presentation time."
 *
 * This appears to be true - when modified with a backbuffer count of 2 as suggested in the FLIPEX documentation, every third frame
 * appears to be blank, which seems evidential of this extra back buffer. But I can't get a surface pointer to this buffer
 * through the dx9 device like I can for the buffers I explicitly requested on device creation.
 *
 * Stranger still, when I hold the NvFBC output buffer constant to one back buffer and just copy it to the other back buffer
 * using StretchRect on every frame, this works and does not skip every third frame, without obviously touching this third implicit back buffer.
 * But doing this copy defeats the purpose of using FLIPEX, which is to eliminate the extra copy.
 *
 */


#include <windows.h>

#include <iostream>
#include <assert.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <vector>

#include <NvFBCLibrary.h>
#include <SimpleLogger.h>
#include <NvFBC/NvFBCToDx9vid.h>
#include <AdminCheck.h>

using namespace std;


#define _CRT_SECURE_NO_WARNINGS  1

// DisplayPosition struct - defined early for use in capture modes
struct DisplayPosition {
    int dxAdapterIndex;
    RECT position;
    char deviceName[32];
    string friendlyName;
};

// ===============================================
// Frame Capture Mode Interface and Implementations
// ===============================================

// Abstract interface for frame capture modes
class IFrameCaptureMode {
public:
    virtual ~IFrameCaptureMode() {}

    // Get the D3DPRESENT_INTERVAL value for device creation
    virtual UINT GetPresentationInterval() const = 0;

    // Setup mode-specific resources
    virtual bool Setup() = 0;

    // Run the entire capture loop (including message processing)
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) = 0;

    // Get descriptive name for logging
    virtual const char* GetModeName() const = 0;
};

// VSync-driven capture mode
class VsyncCaptureMode : public IFrameCaptureMode {
public:
    VsyncCaptureMode() {}
    virtual ~VsyncCaptureMode() {}

    virtual UINT GetPresentationInterval() const override {
        return D3DPRESENT_INTERVAL_ONE;
    }

    virtual bool Setup() override {
        LOG("VSync mode initialized - VSync will control frame timing");
        LOG("Output FPS will match target monitor's refresh rate");
        return true;
    }

    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override
    {
        MSG msg;

        while (TRUE)
        {
            // Poll for latest frame (never blocks - always gets most recent frame available)
            NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);

            if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION)
            {
                LOGERR("NvFBC session invalidated - session needs to be recreated");
                break;
            }
            // Ignore other errors (e.g., no new frame) - we'll just present what we have

            // Present and wait for VSync - this blocks until monitor refresh
            // This synchronizes our output with the actual display hardware
            device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_ONE);

            // Process Windows messages
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            if (msg.message == WM_QUIT)
                break;
        }
    }

    virtual const char* GetModeName() const override {
        return "VSync";
    }
};

// Timer-driven capture mode
class TimerCaptureMode : public IFrameCaptureMode {
private:
    HANDLE m_timer;
    LARGE_INTEGER m_interval;
    float m_framerate;

public:
    TimerCaptureMode(float framerate)
        : m_timer(NULL)
        , m_framerate(framerate)
    {
        m_interval.QuadPart = -(LONGLONG)(10000000.0f / framerate);
    }

    virtual ~TimerCaptureMode() {
        if (m_timer) {
            CloseHandle(m_timer);
            m_timer = NULL;
        }
    }

    virtual UINT GetPresentationInterval() const override {
        return D3DPRESENT_INTERVAL_IMMEDIATE;
    }

    virtual bool Setup() override {
        m_timer = CreateWaitableTimer(NULL, TRUE, NULL);

        if (NULL == m_timer) {
            LOGERR("CreateWaitableTimer failed (error: %d)", GetLastError());
            return false;
        }

        LOG("Timer mode initialized - target framerate: %.2f fps", m_framerate);
        return true;
    }

    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override
    {
        MSG msg;

        while (TRUE)
        {
            // Set timer for this frame
            SetWaitableTimer(m_timer, &m_interval, 0, NULL, NULL, FALSE);

            // Grab frame
            NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);

            if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION)
            {
                LOGERR("NvFBC session invalidated - session needs to be recreated");
                break;
            }

            // Present immediately (non-blocking)
            device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);

            // Process Windows messages
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            if (msg.message == WM_QUIT)
                break;

            // Wait for timer to maintain target framerate
            WaitForSingleObject(m_timer, INFINITE);
        }
    }

    virtual const char* GetModeName() const override {
        return "Timer";
    }
};

// Frame temporal capture mode - temporal frame selection for smooth VRR capture
class FrameTemporalCaptureMode : public IFrameCaptureMode {
private:
    static const int FRAME_HISTORY_SIZE = 2;

    struct FrameHistoryEntry {
        IDirect3DSurface9* surface;
        LARGE_INTEGER timestamp;
        bool valid;
    };

    FrameHistoryEntry m_frameHistory[FRAME_HISTORY_SIZE];
    int m_currentHistoryIndex;
    IDirect3DSurface9* m_captureTarget;
    LARGE_INTEGER m_perfFreq;
    float m_targetFramerate;
    IDirect3DDevice9Ex* m_device;  // Store device pointer for StretchRect

public:
    FrameTemporalCaptureMode(float framerate)
        : m_currentHistoryIndex(0)
        , m_captureTarget(NULL)
        , m_targetFramerate(framerate)
        , m_device(NULL)
    {
        for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
            m_frameHistory[i].surface = NULL;
            m_frameHistory[i].valid = false;
            m_frameHistory[i].timestamp.QuadPart = 0;
        }
        m_perfFreq.QuadPart = 0;
    }

    virtual ~FrameTemporalCaptureMode() {
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

    virtual UINT GetPresentationInterval() const override {
        return D3DPRESENT_INTERVAL_IMMEDIATE;
    }

    virtual bool Setup() override {
        // Need to access globals - these are extern'd below
        extern IDirect3DDevice9Ex* g_pD3D9Device;
        extern int BUF_WIDTH;
        extern int BUF_HEIGHT;

        m_device = g_pD3D9Device;

        // Create capture target surface (where NvFBC will write)
        HRESULT hr = m_device->CreateOffscreenPlainSurface(
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

        LOG("Frame selection mode initialized - target framerate: %.2f fps", m_targetFramerate);
        LOG("Frame history size: %d (temporal frame selection for smooth VRR capture)", FRAME_HISTORY_SIZE);
        return true;
    }

    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override
    {
        extern int BUF_WIDTH;
        extern int BUF_HEIGHT;
        extern IDirect3DSurface9* g_backbuffer;

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
            LOGERR("Failed to reconfigure NvFBC for frame selection mode");
            return;
        }

        while (TRUE)
        {
            QueryPerformanceCounter(&currentTime);

            // Calculate time until next present
            LONGLONG timeUntilPresent = nextPresentTime.QuadPart - currentTime.QuadPart;
            DWORD msUntilPresent = timeUntilPresent > 0 ?
                (DWORD)((timeUntilPresent * 1000) / m_perfFreq.QuadPart) : 0;

            // Smart sleep: if we're far from present time, sleep most of it
            if (msUntilPresent > 5) {
                Sleep(msUntilPresent - 4);  // Wake up 4ms before present time
                continue;
            }

            // Poll for latest frame (NOWAIT - never blocks)
            NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);

            if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION) {
                LOGERR("NvFBC session invalidated - session needs to be recreated");
                break;
            }

            // Only store frame if we're close to present time (within 2 frame periods)
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

                // Present the selected frame
                device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);

                // Schedule next present
                nextPresentTime.QuadPart += ticksPerFrame;

                // Prevent falling too far behind
                if (nextPresentTime.QuadPart < currentTime.QuadPart) {
                    nextPresentTime = currentTime;
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
    }

    virtual const char* GetModeName() const override {
        return "FrameTemporal";
    }

private:
    void SelectFrameToBackbuffer(LARGE_INTEGER targetTime, IDirect3DSurface9* backbuffer) {
        extern int BUF_WIDTH;
        extern int BUF_HEIGHT;

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
};

// Frame blend capture mode - GPU pixel shader blending for smooth output
class FrameBlendCaptureMode : public IFrameCaptureMode {
private:
    static const int FRAME_HISTORY_SIZE = 2;
    static constexpr float BLEND_WEIGHT_THRESHOLD = 0.05f;  // Skip GPU blending if weight < (1.0 - this) or > this

    struct FrameHistoryEntry {
        IDirect3DSurface9* surface;
        LARGE_INTEGER timestamp;
        bool valid;
    };

    struct QuadVertex {
        float x, y, z;
        float u, v;
    };

    FrameHistoryEntry m_frameHistory[FRAME_HISTORY_SIZE];
    int m_currentHistoryIndex;
    IDirect3DSurface9* m_captureTarget;
    IDirect3DTexture9* m_frameTextures[FRAME_HISTORY_SIZE];
    IDirect3DTexture9* m_captureTexture;
    IDirect3DVertexShader9* m_vertexShader;
    IDirect3DPixelShader9* m_pixelShader;
    IDirect3DVertexDeclaration9* m_vertexDeclaration;
    IDirect3DVertexBuffer9* m_quadVertexBuffer;
    LARGE_INTEGER m_perfFreq;
    float m_targetFramerate;
    IDirect3DDevice9Ex* m_device;
    LARGE_INTEGER m_lastGrabTime;
    bool m_shaderAvailable;

public:
    FrameBlendCaptureMode(float framerate)
        : m_currentHistoryIndex(0)
        , m_captureTarget(NULL)
        , m_captureTexture(NULL)
        , m_vertexShader(NULL)
        , m_pixelShader(NULL)
        , m_vertexDeclaration(NULL)
        , m_quadVertexBuffer(NULL)
        , m_targetFramerate(framerate)
        , m_device(NULL)
        , m_shaderAvailable(false)
    {
        for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
            m_frameHistory[i].surface = NULL;
            m_frameHistory[i].valid = false;
            m_frameHistory[i].timestamp.QuadPart = 0;
            m_frameTextures[i] = NULL;
        }
        m_perfFreq.QuadPart = 0;
        m_lastGrabTime.QuadPart = 0;
    }

    virtual ~FrameBlendCaptureMode() {
        // Release shader resources
        if (m_pixelShader) {
            m_pixelShader->Release();
            m_pixelShader = NULL;
        }
        if (m_vertexShader) {
            m_vertexShader->Release();
            m_vertexShader = NULL;
        }
        if (m_vertexDeclaration) {
            m_vertexDeclaration->Release();
            m_vertexDeclaration = NULL;
        }
        if (m_quadVertexBuffer) {
            m_quadVertexBuffer->Release();
            m_quadVertexBuffer = NULL;
        }

        // Release textures and surfaces
        for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
            if (m_frameTextures[i]) {
                m_frameTextures[i]->Release();
                m_frameTextures[i] = NULL;
            }
            if (m_frameHistory[i].surface) {
                m_frameHistory[i].surface->Release();
                m_frameHistory[i].surface = NULL;
            }
        }
        if (m_captureTexture) {
            m_captureTexture->Release();
            m_captureTexture = NULL;
        }
        if (m_captureTarget) {
            m_captureTarget->Release();
            m_captureTarget = NULL;
        }
    }

    virtual UINT GetPresentationInterval() const override {
        return D3DPRESENT_INTERVAL_IMMEDIATE;
    }

    virtual bool Setup() override {
        extern IDirect3DDevice9Ex* g_pD3D9Device;
        extern int BUF_WIDTH;
        extern int BUF_HEIGHT;

        m_device = g_pD3D9Device;

        // Compile and create shaders
        if (!CompileAndCreateShaders()) {
            LOGERR("Failed to create shaders for blend mode");
            return false;
        }

        // Create capture texture (where NvFBC will write)
        HRESULT hr = m_device->CreateTexture(
            BUF_WIDTH, BUF_HEIGHT,
            1,
            D3DUSAGE_RENDERTARGET,
            D3DFMT_A2B10G10R10,
            D3DPOOL_DEFAULT,
            &m_captureTexture,
            NULL);

        if (FAILED(hr)) {
            LOGERR("Failed to create capture texture (error: 0x%08x)", hr);
            return false;
        }

        hr = m_captureTexture->GetSurfaceLevel(0, &m_captureTarget);
        if (FAILED(hr)) {
            LOGERR("Failed to get surface from capture texture (error: 0x%08x)", hr);
            return false;
        }

        // Create frame history textures
        for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
            hr = m_device->CreateTexture(
                BUF_WIDTH, BUF_HEIGHT,
                1,
                D3DUSAGE_RENDERTARGET,
                D3DFMT_A2B10G10R10,
                D3DPOOL_DEFAULT,
                &m_frameTextures[i],
                NULL);

            if (FAILED(hr)) {
                LOGERR("Failed to create frame texture %d (error: 0x%08x)", i, hr);
                return false;
            }

            hr = m_frameTextures[i]->GetSurfaceLevel(0, &m_frameHistory[i].surface);
            if (FAILED(hr)) {
                LOGERR("Failed to get surface from frame texture %d (error: 0x%08x)", i, hr);
                return false;
            }
        }

        QueryPerformanceFrequency(&m_perfFreq);

        // Initialize static render states
        if (!InitBlendingRenderStates()) {
            LOGERR("Failed to initialize blending render states");
            return false;
        }

        m_shaderAvailable = true;

        LOG("Frame blend mode initialized - target framerate: %.2f fps", m_targetFramerate);
        LOG("GPU pixel shader blending enabled (blend threshold: %.1f)", BLEND_WEIGHT_THRESHOLD);
        return true;
    }

    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override
    {
        extern int BUF_WIDTH;
        extern int BUF_HEIGHT;
        extern IDirect3DSurface9* g_backbuffer;

        MSG msg;
        LARGE_INTEGER nextPresentTime, currentTime;
        QueryPerformanceCounter(&nextPresentTime);
        LONGLONG ticksPerFrame = (LONGLONG)(m_perfFreq.QuadPart / m_targetFramerate);

        // Update NvFBC to write to our capture target
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
            LOGERR("Failed to reconfigure NvFBC for frame blend mode");
            return;
        }

        // Calculate minimum interval between grabs (3x output rate)
        LONGLONG minGrabInterval = (LONGLONG)(m_perfFreq.QuadPart / (m_targetFramerate * 3.0f));

        while (TRUE)
        {
            QueryPerformanceCounter(&currentTime);

            LONGLONG timeUntilPresent = nextPresentTime.QuadPart - currentTime.QuadPart;
            DWORD msUntilPresent = timeUntilPresent > 0 ?
                (DWORD)((timeUntilPresent * 1000) / m_perfFreq.QuadPart) : 0;

            if (msUntilPresent > 5) {
                Sleep(msUntilPresent - 4);
                continue;
            }

            // Rate limit grabs
            LONGLONG timeSinceLastGrab = currentTime.QuadPart - m_lastGrabTime.QuadPart;
            if (timeSinceLastGrab < minGrabInterval) {
                Sleep(1);
                continue;
            }

            NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);
            m_lastGrabTime = currentTime;

            if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION) {
                LOGERR("NvFBC session invalidated - session needs to be recreated");
                break;
            }

            if (fbcRes == NVFBC_SUCCESS && timeUntilPresent < (ticksPerFrame * 2)) {
                QueryPerformanceCounter(&currentTime);

                RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };
                device->StretchRect(
                    m_captureTarget,
                    &srcRect,
                    m_frameHistory[m_currentHistoryIndex].surface,
                    &srcRect,
                    D3DTEXF_NONE);

                m_frameHistory[m_currentHistoryIndex].timestamp = currentTime;
                m_frameHistory[m_currentHistoryIndex].valid = true;

                m_currentHistoryIndex = (m_currentHistoryIndex + 1) % FRAME_HISTORY_SIZE;
            }

            QueryPerformanceCounter(&currentTime);
            if (currentTime.QuadPart >= nextPresentTime.QuadPart) {
                BlendFramesToBackbuffer(nextPresentTime, g_backbuffer);

                device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);

                nextPresentTime.QuadPart += ticksPerFrame;

                if (nextPresentTime.QuadPart < currentTime.QuadPart) {
                    nextPresentTime = currentTime;
                }
            }

            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            if (msg.message == WM_QUIT)
                break;
        }
    }

    virtual const char* GetModeName() const override {
        return "FrameBlend";
    }

private:
    bool CompileAndCreateShaders() {
        ID3DBlob* vsBlob = NULL;
        ID3DBlob* psBlob = NULL;
        ID3DBlob* errorBlob = NULL;

        const char* vertexShaderCode =
            "struct VS_INPUT {\n"
            "    float3 pos : POSITION;\n"
            "    float2 uv : TEXCOORD0;\n"
            "};\n"
            "struct VS_OUTPUT {\n"
            "    float4 pos : POSITION;\n"
            "    float2 uv : TEXCOORD0;\n"
            "};\n"
            "VS_OUTPUT main(VS_INPUT input) {\n"
            "    VS_OUTPUT output;\n"
            "    output.pos = float4(input.pos, 1.0);\n"
            "    output.uv = input.uv;\n"
            "    return output;\n"
            "}\n";

        const char* pixelShaderCode =
            "sampler2D texBefore : register(s0);\n"
            "sampler2D texAfter : register(s1);\n"
            "float blendWeight : register(c0);\n"
            "struct PS_INPUT {\n"
            "    float2 uv : TEXCOORD0;\n"
            "};\n"
            "float4 main(PS_INPUT input) : COLOR0 {\n"
            "    float4 colorBefore = tex2D(texBefore, input.uv);\n"
            "    float4 colorAfter = tex2D(texAfter, input.uv);\n"
            "    return lerp(colorBefore, colorAfter, blendWeight);\n"
            "}\n";

        HRESULT hr = D3DCompile(
            vertexShaderCode,
            strlen(vertexShaderCode),
            "VertexShader",
            NULL, NULL, "main", "vs_3_0",
            0, 0,
            &vsBlob,
            &errorBlob);

        if (FAILED(hr)) {
            if (errorBlob) {
                LOGERR("Vertex shader compile error: %s", (char*)errorBlob->GetBufferPointer());
                errorBlob->Release();
            }
            return false;
        }

        hr = D3DCompile(
            pixelShaderCode,
            strlen(pixelShaderCode),
            "PixelShader",
            NULL, NULL, "main", "ps_3_0",
            0, 0,
            &psBlob,
            &errorBlob);

        if (FAILED(hr)) {
            if (errorBlob) {
                LOGERR("Pixel shader compile error: %s", (char*)errorBlob->GetBufferPointer());
                errorBlob->Release();
            }
            if (vsBlob) vsBlob->Release();
            return false;
        }

        hr = m_device->CreateVertexShader((DWORD*)vsBlob->GetBufferPointer(), &m_vertexShader);
        if (FAILED(hr)) {
            LOGERR("Failed to create vertex shader (error: 0x%08x)", hr);
            vsBlob->Release();
            psBlob->Release();
            return false;
        }

        hr = m_device->CreatePixelShader((DWORD*)psBlob->GetBufferPointer(), &m_pixelShader);
        if (FAILED(hr)) {
            LOGERR("Failed to create pixel shader (error: 0x%08x)", hr);
            vsBlob->Release();
            psBlob->Release();
            return false;
        }

        vsBlob->Release();
        psBlob->Release();

        D3DVERTEXELEMENT9 vertexElements[] = {
            { 0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
            { 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
            D3DDECL_END()
        };

        hr = m_device->CreateVertexDeclaration(vertexElements, &m_vertexDeclaration);
        if (FAILED(hr)) {
            LOGERR("Failed to create vertex declaration (error: 0x%08x)", hr);
            return false;
        }

        // Create vertex buffer for fullscreen quad
        hr = m_device->CreateVertexBuffer(
            6 * sizeof(QuadVertex),
            D3DUSAGE_WRITEONLY,
            0,
            D3DPOOL_DEFAULT,
            &m_quadVertexBuffer,
            NULL);

        if (FAILED(hr)) {
            LOGERR("Failed to create vertex buffer (error: 0x%08x)", hr);
            return false;
        }

        QuadVertex* pVertices = NULL;
        hr = m_quadVertexBuffer->Lock(0, 0, (void**)&pVertices, 0);
        if (SUCCEEDED(hr)) {
            pVertices[0] = { -1.0f,  1.0f, 0.5f,  0.0f, 0.0f };
            pVertices[1] = {  1.0f,  1.0f, 0.5f,  1.0f, 0.0f };
            pVertices[2] = { -1.0f, -1.0f, 0.5f,  0.0f, 1.0f };
            pVertices[3] = {  1.0f,  1.0f, 0.5f,  1.0f, 0.0f };
            pVertices[4] = {  1.0f, -1.0f, 0.5f,  1.0f, 1.0f };
            pVertices[5] = { -1.0f, -1.0f, 0.5f,  0.0f, 1.0f };
            m_quadVertexBuffer->Unlock();
        } else {
            LOGERR("Failed to lock vertex buffer (error: 0x%08x)", hr);
            return false;
        }

        LOG("Shaders compiled and created successfully");
        return true;
    }

    bool InitBlendingRenderStates() {
        // Configure sampler states with LINEAR filtering
        m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        m_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        m_device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        m_device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        // Set static render states
        m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
        m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        m_device->SetRenderState(D3DRS_LIGHTING, FALSE);

        // Set vertex declaration and stream source
        m_device->SetVertexDeclaration(m_vertexDeclaration);
        m_device->SetStreamSource(0, m_quadVertexBuffer, 0, sizeof(QuadVertex));

        // Set shaders
        m_device->SetVertexShader(m_vertexShader);
        m_device->SetPixelShader(m_pixelShader);

        LOG("Static render states initialized (LINEAR filtering)");
        return true;
    }

    void BlendFramesToBackbuffer(LARGE_INTEGER targetTime, IDirect3DSurface9* backbuffer) {
        extern int BUF_WIDTH;
        extern int BUF_HEIGHT;

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

        if (bestBefore >= 0 && bestAfter >= 0 && m_shaderAvailable) {
            LONGLONG totalDiff = m_frameHistory[bestAfter].timestamp.QuadPart -
                                 m_frameHistory[bestBefore].timestamp.QuadPart;
            float weight = totalDiff > 0 ? (float)smallestBeforeDiff / (float)totalDiff : 0.5f;

            // Skip GPU blending if weight is extreme (>90% one frame or the other)
            if (weight < (1.0f - BLEND_WEIGHT_THRESHOLD)) {
                m_device->StretchRect(
                    m_frameHistory[bestBefore].surface,
                    &srcRect,
                    backbuffer,
                    &srcRect,
                    D3DTEXF_NONE);
            }
            else if (weight > BLEND_WEIGHT_THRESHOLD) {
                m_device->StretchRect(
                    m_frameHistory[bestAfter].surface,
                    &srcRect,
                    backbuffer,
                    &srcRect,
                    D3DTEXF_NONE);
            }
            else {
                // Do GPU blending
                m_device->SetRenderTarget(0, backbuffer);
                m_device->SetTexture(0, m_frameTextures[bestBefore]);
                m_device->SetTexture(1, m_frameTextures[bestAfter]);

                float constants[4] = { weight, 0.0f, 0.0f, 0.0f };
                m_device->SetPixelShaderConstantF(0, constants, 1);

                HRESULT hr = m_device->BeginScene();
                if (SUCCEEDED(hr)) {
                    hr = m_device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                    m_device->EndScene();

                    if (FAILED(hr)) {
                        LOGERR("DrawPrimitive failed (error: 0x%08x), falling back to StretchRect", hr);
                        m_device->StretchRect(
                            m_frameHistory[bestBefore].surface,
                            &srcRect,
                            backbuffer,
                            &srcRect,
                            D3DTEXF_NONE);
                    }
                } else {
                    LOGERR("BeginScene failed (error: 0x%08x), falling back to StretchRect", hr);
                    m_device->StretchRect(
                        m_frameHistory[bestBefore].surface,
                        &srcRect,
                        backbuffer,
                        &srcRect,
                        D3DTEXF_NONE);
                }

                m_device->SetTexture(0, NULL);
                m_device->SetTexture(1, NULL);
            }
        }
        else if (bestBefore >= 0) {
            m_device->StretchRect(
                m_frameHistory[bestBefore].surface,
                &srcRect,
                backbuffer,
                &srcRect,
                D3DTEXF_NONE);
        }
        else if (bestAfter >= 0) {
            m_device->StretchRect(
                m_frameHistory[bestAfter].surface,
                &srcRect,
                backbuffer,
                &srcRect,
                D3DTEXF_NONE);
        }
    }
};

// VSync + Blend capture mode - combines vsync-driven timing with GPU frame blending
class VsyncBlendCaptureMode : public IFrameCaptureMode {
private:
    static const int FRAME_HISTORY_SIZE = 2;
    static constexpr float BLEND_WEIGHT_THRESHOLD = 0.05f;

    struct FrameHistoryEntry {
        IDirect3DSurface9* surface;
        LARGE_INTEGER timestamp;
        bool valid;
    };

    struct QuadVertex {
        float x, y, z;
        float u, v;
    };

    FrameHistoryEntry m_frameHistory[FRAME_HISTORY_SIZE];
    int m_currentHistoryIndex;
    IDirect3DSurface9* m_captureTarget;
    IDirect3DTexture9* m_frameTextures[FRAME_HISTORY_SIZE];
    IDirect3DTexture9* m_captureTexture;
    IDirect3DVertexShader9* m_vertexShader;
    IDirect3DPixelShader9* m_pixelShader;
    IDirect3DVertexDeclaration9* m_vertexDeclaration;
    IDirect3DVertexBuffer9* m_quadVertexBuffer;
    LARGE_INTEGER m_perfFreq;
    float m_targetFramerate;
    IDirect3DDevice9Ex* m_device;
    bool m_shaderAvailable;

public:
    VsyncBlendCaptureMode()
        : m_currentHistoryIndex(0)
        , m_captureTarget(NULL)
        , m_captureTexture(NULL)
        , m_vertexShader(NULL)
        , m_pixelShader(NULL)
        , m_vertexDeclaration(NULL)
        , m_quadVertexBuffer(NULL)
        , m_targetFramerate(60.0f)  // Default, will be detected
        , m_device(NULL)
        , m_shaderAvailable(false)
    {
        for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
            m_frameHistory[i].surface = NULL;
            m_frameHistory[i].valid = false;
            m_frameHistory[i].timestamp.QuadPart = 0;
            m_frameTextures[i] = NULL;
        }
        m_perfFreq.QuadPart = 0;
    }

    virtual ~VsyncBlendCaptureMode() {
        // Release shader resources
        if (m_pixelShader) {
            m_pixelShader->Release();
            m_pixelShader = NULL;
        }
        if (m_vertexShader) {
            m_vertexShader->Release();
            m_vertexShader = NULL;
        }
        if (m_vertexDeclaration) {
            m_vertexDeclaration->Release();
            m_vertexDeclaration = NULL;
        }
        if (m_quadVertexBuffer) {
            m_quadVertexBuffer->Release();
            m_quadVertexBuffer = NULL;
        }

        // Release textures and surfaces
        for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
            if (m_frameTextures[i]) {
                m_frameTextures[i]->Release();
                m_frameTextures[i] = NULL;
            }
            if (m_frameHistory[i].surface) {
                m_frameHistory[i].surface->Release();
                m_frameHistory[i].surface = NULL;
            }
        }
        if (m_captureTexture) {
            m_captureTexture->Release();
            m_captureTexture = NULL;
        }
        if (m_captureTarget) {
            m_captureTarget->Release();
            m_captureTarget = NULL;
        }
    }

    virtual UINT GetPresentationInterval() const override {
        return D3DPRESENT_INTERVAL_ONE;
    }

    virtual bool Setup() override {
        extern IDirect3DDevice9Ex* g_pD3D9Device;
        extern IDirect3D9Ex* g_pD3DEx;
        extern int BUF_WIDTH;
        extern int BUF_HEIGHT;
        extern DisplayPosition target;

        m_device = g_pD3D9Device;

        // Detect target display refresh rate
        D3DDISPLAYMODE displayMode;
        HRESULT hr = g_pD3DEx->GetAdapterDisplayMode(target.dxAdapterIndex, &displayMode);
        if (SUCCEEDED(hr)) {
            m_targetFramerate = (float)displayMode.RefreshRate;
            LOG("VSync blend mode detected target refresh rate: %.2f Hz", m_targetFramerate);
        } else {
            LOG("Failed to detect refresh rate (error: 0x%08x), defaulting to 60.0 Hz", hr);
            m_targetFramerate = 60.0f;
        }

        // Compile and create shaders
        if (!CompileAndCreateShaders()) {
            LOGERR("Failed to create shaders for vsync blend mode");
            return false;
        }

        // Create capture texture (where NvFBC will write)
        hr = m_device->CreateTexture(
            BUF_WIDTH, BUF_HEIGHT,
            1,
            D3DUSAGE_RENDERTARGET,
            D3DFMT_A2B10G10R10,
            D3DPOOL_DEFAULT,
            &m_captureTexture,
            NULL);

        if (FAILED(hr)) {
            LOGERR("Failed to create capture texture (error: 0x%08x)", hr);
            return false;
        }

        hr = m_captureTexture->GetSurfaceLevel(0, &m_captureTarget);
        if (FAILED(hr)) {
            LOGERR("Failed to get surface from capture texture (error: 0x%08x)", hr);
            return false;
        }

        // Create frame history textures
        for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
            hr = m_device->CreateTexture(
                BUF_WIDTH, BUF_HEIGHT,
                1,
                D3DUSAGE_RENDERTARGET,
                D3DFMT_A2B10G10R10,
                D3DPOOL_DEFAULT,
                &m_frameTextures[i],
                NULL);

            if (FAILED(hr)) {
                LOGERR("Failed to create frame texture %d (error: 0x%08x)", i, hr);
                return false;
            }

            hr = m_frameTextures[i]->GetSurfaceLevel(0, &m_frameHistory[i].surface);
            if (FAILED(hr)) {
                LOGERR("Failed to get surface from frame texture %d (error: 0x%08x)", i, hr);
                return false;
            }
        }

        QueryPerformanceFrequency(&m_perfFreq);

        // Initialize static render states
        if (!InitBlendingRenderStates()) {
            LOGERR("Failed to initialize blending render states");
            return false;
        }

        m_shaderAvailable = true;

        LOG("VSync blend mode initialized - target framerate: %.2f fps (auto-detected)", m_targetFramerate);
        LOG("GPU pixel shader blending enabled (blend threshold: %.1f)", BLEND_WEIGHT_THRESHOLD);
        LOG("Output FPS will match target monitor's refresh rate via VSync");
        return true;
    }

    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override
    {
        extern int BUF_WIDTH;
        extern int BUF_HEIGHT;
        extern IDirect3DSurface9* g_backbuffer;

        MSG msg;
        LARGE_INTEGER nextPresentTime, currentTime;
        QueryPerformanceCounter(&nextPresentTime);
        LONGLONG ticksPerFrame = (LONGLONG)(m_perfFreq.QuadPart / m_targetFramerate);

        // Update NvFBC to write to our capture target
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
            LOGERR("Failed to reconfigure NvFBC for vsync blend mode");
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

            // Store frame if we got a new one and we're close to present time
            if (fbcRes == NVFBC_SUCCESS && timeUntilPresent < (ticksPerFrame * 2)) {
                QueryPerformanceCounter(&currentTime);

                RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };
                device->StretchRect(
                    m_captureTarget,
                    &srcRect,
                    m_frameHistory[m_currentHistoryIndex].surface,
                    &srcRect,
                    D3DTEXF_NONE);

                m_frameHistory[m_currentHistoryIndex].timestamp = currentTime;
                m_frameHistory[m_currentHistoryIndex].valid = true;

                m_currentHistoryIndex = (m_currentHistoryIndex + 1) % FRAME_HISTORY_SIZE;
            }

            // Check if it's time to present
            QueryPerformanceCounter(&currentTime);
            if (currentTime.QuadPart >= nextPresentTime.QuadPart) {
                // Select/blend best frames from history
                BlendFramesToBackbuffer(nextPresentTime, g_backbuffer);

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

    virtual const char* GetModeName() const override {
        return "VsyncBlend";
    }

private:
    bool CompileAndCreateShaders() {
        ID3DBlob* vsBlob = NULL;
        ID3DBlob* psBlob = NULL;
        ID3DBlob* errorBlob = NULL;

        const char* vertexShaderCode =
            "struct VS_INPUT {\n"
            "    float3 pos : POSITION;\n"
            "    float2 uv : TEXCOORD0;\n"
            "};\n"
            "struct VS_OUTPUT {\n"
            "    float4 pos : POSITION;\n"
            "    float2 uv : TEXCOORD0;\n"
            "};\n"
            "VS_OUTPUT main(VS_INPUT input) {\n"
            "    VS_OUTPUT output;\n"
            "    output.pos = float4(input.pos, 1.0);\n"
            "    output.uv = input.uv;\n"
            "    return output;\n"
            "}\n";

        const char* pixelShaderCode =
            "sampler2D texBefore : register(s0);\n"
            "sampler2D texAfter : register(s1);\n"
            "float blendWeight : register(c0);\n"
            "struct PS_INPUT {\n"
            "    float2 uv : TEXCOORD0;\n"
            "};\n"
            "float4 main(PS_INPUT input) : COLOR0 {\n"
            "    float4 colorBefore = tex2D(texBefore, input.uv);\n"
            "    float4 colorAfter = tex2D(texAfter, input.uv);\n"
            "    return lerp(colorBefore, colorAfter, blendWeight);\n"
            "}\n";

        HRESULT hr = D3DCompile(
            vertexShaderCode,
            strlen(vertexShaderCode),
            "VertexShader",
            NULL, NULL, "main", "vs_3_0",
            0, 0,
            &vsBlob,
            &errorBlob);

        if (FAILED(hr)) {
            if (errorBlob) {
                LOGERR("Vertex shader compile error: %s", (char*)errorBlob->GetBufferPointer());
                errorBlob->Release();
            }
            return false;
        }

        hr = D3DCompile(
            pixelShaderCode,
            strlen(pixelShaderCode),
            "PixelShader",
            NULL, NULL, "main", "ps_3_0",
            0, 0,
            &psBlob,
            &errorBlob);

        if (FAILED(hr)) {
            if (errorBlob) {
                LOGERR("Pixel shader compile error: %s", (char*)errorBlob->GetBufferPointer());
                errorBlob->Release();
            }
            if (vsBlob) vsBlob->Release();
            return false;
        }

        hr = m_device->CreateVertexShader((DWORD*)vsBlob->GetBufferPointer(), &m_vertexShader);
        if (FAILED(hr)) {
            LOGERR("Failed to create vertex shader (error: 0x%08x)", hr);
            vsBlob->Release();
            psBlob->Release();
            return false;
        }

        hr = m_device->CreatePixelShader((DWORD*)psBlob->GetBufferPointer(), &m_pixelShader);
        if (FAILED(hr)) {
            LOGERR("Failed to create pixel shader (error: 0x%08x)", hr);
            vsBlob->Release();
            psBlob->Release();
            return false;
        }

        vsBlob->Release();
        psBlob->Release();

        D3DVERTEXELEMENT9 vertexElements[] = {
            { 0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
            { 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
            D3DDECL_END()
        };

        hr = m_device->CreateVertexDeclaration(vertexElements, &m_vertexDeclaration);
        if (FAILED(hr)) {
            LOGERR("Failed to create vertex declaration (error: 0x%08x)", hr);
            return false;
        }

        // Create vertex buffer for fullscreen quad
        hr = m_device->CreateVertexBuffer(
            6 * sizeof(QuadVertex),
            D3DUSAGE_WRITEONLY,
            0,
            D3DPOOL_DEFAULT,
            &m_quadVertexBuffer,
            NULL);

        if (FAILED(hr)) {
            LOGERR("Failed to create vertex buffer (error: 0x%08x)", hr);
            return false;
        }

        QuadVertex* pVertices = NULL;
        hr = m_quadVertexBuffer->Lock(0, 0, (void**)&pVertices, 0);
        if (SUCCEEDED(hr)) {
            pVertices[0] = { -1.0f,  1.0f, 0.5f,  0.0f, 0.0f };
            pVertices[1] = {  1.0f,  1.0f, 0.5f,  1.0f, 0.0f };
            pVertices[2] = { -1.0f, -1.0f, 0.5f,  0.0f, 1.0f };
            pVertices[3] = {  1.0f,  1.0f, 0.5f,  1.0f, 0.0f };
            pVertices[4] = {  1.0f, -1.0f, 0.5f,  1.0f, 1.0f };
            pVertices[5] = { -1.0f, -1.0f, 0.5f,  0.0f, 1.0f };
            m_quadVertexBuffer->Unlock();
        } else {
            LOGERR("Failed to lock vertex buffer (error: 0x%08x)", hr);
            return false;
        }

        LOG("Shaders compiled and created successfully");
        return true;
    }

    bool InitBlendingRenderStates() {
        // Configure sampler states with LINEAR filtering
        m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        m_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        m_device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        m_device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        // Set static render states
        m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
        m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        m_device->SetRenderState(D3DRS_LIGHTING, FALSE);

        // Set vertex declaration and stream source
        m_device->SetVertexDeclaration(m_vertexDeclaration);
        m_device->SetStreamSource(0, m_quadVertexBuffer, 0, sizeof(QuadVertex));

        // Set shaders
        m_device->SetVertexShader(m_vertexShader);
        m_device->SetPixelShader(m_pixelShader);

        LOG("Static render states initialized (LINEAR filtering)");
        return true;
    }

    void BlendFramesToBackbuffer(LARGE_INTEGER targetTime, IDirect3DSurface9* backbuffer) {
        extern int BUF_WIDTH;
        extern int BUF_HEIGHT;

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

        if (bestBefore >= 0 && bestAfter >= 0 && m_shaderAvailable) {
            LONGLONG totalDiff = m_frameHistory[bestAfter].timestamp.QuadPart -
                                 m_frameHistory[bestBefore].timestamp.QuadPart;
            float weight = totalDiff > 0 ? (float)smallestBeforeDiff / (float)totalDiff : 0.5f;

            // Skip GPU blending if weight is extreme (>90% one frame or the other)
            if (weight < (1.0f - BLEND_WEIGHT_THRESHOLD)) {
                m_device->StretchRect(
                    m_frameHistory[bestBefore].surface,
                    &srcRect,
                    backbuffer,
                    &srcRect,
                    D3DTEXF_NONE);
            }
            else if (weight > BLEND_WEIGHT_THRESHOLD) {
                m_device->StretchRect(
                    m_frameHistory[bestAfter].surface,
                    &srcRect,
                    backbuffer,
                    &srcRect,
                    D3DTEXF_NONE);
            }
            else {
                // Do GPU blending
                m_device->SetRenderTarget(0, backbuffer);
                m_device->SetTexture(0, m_frameTextures[bestBefore]);
                m_device->SetTexture(1, m_frameTextures[bestAfter]);

                float constants[4] = { weight, 0.0f, 0.0f, 0.0f };
                m_device->SetPixelShaderConstantF(0, constants, 1);

                HRESULT hr = m_device->BeginScene();
                if (SUCCEEDED(hr)) {
                    hr = m_device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                    m_device->EndScene();

                    if (FAILED(hr)) {
                        LOGERR("DrawPrimitive failed (error: 0x%08x), falling back to StretchRect", hr);
                        m_device->StretchRect(
                            m_frameHistory[bestBefore].surface,
                            &srcRect,
                            backbuffer,
                            &srcRect,
                            D3DTEXF_NONE);
                    }
                } else {
                    LOGERR("BeginScene failed (error: 0x%08x), falling back to StretchRect", hr);
                    m_device->StretchRect(
                        m_frameHistory[bestBefore].surface,
                        &srcRect,
                        backbuffer,
                        &srcRect,
                        D3DTEXF_NONE);
                }

                m_device->SetTexture(0, NULL);
                m_device->SetTexture(1, NULL);
            }
        }
        else if (bestBefore >= 0) {
            m_device->StretchRect(
                m_frameHistory[bestBefore].surface,
                &srcRect,
                backbuffer,
                &srcRect,
                D3DTEXF_NONE);
        }
        else if (bestAfter >= 0) {
            m_device->StretchRect(
                m_frameHistory[bestAfter].surface,
                &srcRect,
                backbuffer,
                &srcRect,
                D3DTEXF_NONE);
        }
    }
};

// VSync + Temporal capture mode - frame selection with vsync-driven timing
class VsyncTemporalCaptureMode : public IFrameCaptureMode {
private:
    static const int FRAME_HISTORY_SIZE = 2;

    struct FrameHistoryEntry {
        IDirect3DSurface9* surface;
        LARGE_INTEGER timestamp;
        bool valid;
    };

    FrameHistoryEntry m_frameHistory[FRAME_HISTORY_SIZE];
    int m_currentHistoryIndex;
    IDirect3DSurface9* m_captureTarget;
    LARGE_INTEGER m_perfFreq;
    float m_targetFramerate;
    IDirect3DDevice9Ex* m_device;

public:
    VsyncTemporalCaptureMode()
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

    virtual ~VsyncTemporalCaptureMode() {
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

    virtual UINT GetPresentationInterval() const override {
        return D3DPRESENT_INTERVAL_ONE;
    }

    virtual bool Setup() override {
        extern IDirect3DDevice9Ex* g_pD3D9Device;
        extern IDirect3D9Ex* g_pD3DEx;
        extern int BUF_WIDTH;
        extern int BUF_HEIGHT;
        extern DisplayPosition target;

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

    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override
    {
        extern int BUF_WIDTH;
        extern int BUF_HEIGHT;
        extern IDirect3DSurface9* g_backbuffer;

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

    virtual const char* GetModeName() const override {
        return "VsyncTemporal";
    }

private:
    void SelectFrameToBackbuffer(LARGE_INTEGER targetTime, IDirect3DSurface9* backbuffer) {
        extern int BUF_WIDTH;
        extern int BUF_HEIGHT;

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
};

// Helper function to parse capture mode string and create appropriate mode instance
IFrameCaptureMode* ParseCaptureMode(const string& modeStr) {
    if (modeStr.empty() || _stricmp(modeStr.c_str(), "vsync") == 0) {
        // Default to vsync mode
        return new VsyncCaptureMode();
    }

    // Check for vsync temporal mode (t:vsync or just t)
    if (_stricmp(modeStr.c_str(), "t") == 0 || _stricmp(modeStr.c_str(), "t:vsync") == 0) {
        return new VsyncTemporalCaptureMode();
    }

    // Check for temporal frame selection mode (t:60 format)
    if (modeStr.length() > 2 && modeStr[0] == 't' && modeStr[1] == ':') {
        try {
            float framerate = stof(modeStr.substr(2));
            if (framerate > 0.0f && framerate <= 1000.0f) {
                return new FrameTemporalCaptureMode(framerate);
            }
        }
        catch (...) {
            // Invalid number after t:
        }
    }

    // Check for vsync blend mode (b:vsync or just b)
    if (_stricmp(modeStr.c_str(), "b") == 0 || _stricmp(modeStr.c_str(), "b:vsync") == 0) {
        return new VsyncBlendCaptureMode();
    }

    // Check for frame blend mode (b:60 format)
    if (modeStr.length() > 2 && modeStr[0] == 'b' && modeStr[1] == ':') {
        try {
            float framerate = stof(modeStr.substr(2));
            if (framerate > 0.0f && framerate <= 1000.0f) {
                return new FrameBlendCaptureMode(framerate);
            }
        }
        catch (...) {
            // Invalid number after b:
        }
    }

    // Try to parse as numeric framerate
    try {
        float framerate = stof(modeStr);
        if (framerate > 0.0f && framerate <= 1000.0f) {
            return new TimerCaptureMode(framerate);
        }
    }
    catch (...) {
        // Not a valid number
    }

    LOGERR("Invalid capture mode: '%s' (use 'vsync', 't' or 't:vsync' for vsync temporal, 't:60' for timed temporal, 'b' or 'b:vsync' for vsync blend, 'b:60' for timed blend, or numeric fps like '60')", modeStr.c_str());
    return NULL;
}

// ===============================================


// DirectX resources
IDirect3D9Ex        *g_pD3DEx = NULL;
IDirect3DDevice9Ex  *g_pD3D9Device = NULL;
IDirect3DSurface9* g_backbuffer = NULL;

bool g_bNvFBCLibLoaded = false;

NvFBCToDx9Vid *NvFBCDX9 = NULL;
NvFBCLibrary *pNVFBCLib;

int BUF_WIDTH;
int BUF_HEIGHT;

DisplayPosition source, target;

vector <DisplayPosition> displays;

void Cleanup()
{
    LOG("Cleanup started");

    //! Release the NvFBCDX9 instance
    if (NvFBCDX9)
    {
        NvFBCDX9->NvFBCToDx9VidRelease();
        NvFBCDX9 = NULL;
    }

    if (g_backbuffer) {
        g_backbuffer->Release();
        g_backbuffer = NULL;
    }

    if (g_pD3D9Device)
    {
        g_pD3D9Device->Release();
        g_pD3D9Device = NULL;
    }

    if (g_pD3DEx)
    {
        g_pD3DEx->Release();
        g_pD3DEx = NULL;
    }

    if (g_bNvFBCLibLoaded)
    {
        pNVFBCLib->close();
    }

    if (pNVFBCLib)
    {
        delete pNVFBCLib;
        pNVFBCLib = NULL;
    }

    LOG("Cleanup completed");
    SimpleLogger::getInstance().flush();
}

int InitDisplays() {

    Direct3DCreate9Ex(D3D_SDK_VERSION, &g_pD3DEx);
    int adapterCount = static_cast<int>(g_pD3DEx->GetAdapterCount());

    for (int i = 0; i < adapterCount; i++) {
        MONITORINFOEX mi;
        ZeroMemory(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);
        GetMonitorInfo(g_pD3DEx->GetAdapterMonitor(i), &mi);

        DisplayPosition newMon = {
                    i,
                    mi.rcMonitor
        };
        strcpy(newMon.deviceName, mi.szDevice);
        displays.push_back(newMon);

    }


    vector<DISPLAYCONFIG_PATH_INFO> paths;
    vector<DISPLAYCONFIG_MODE_INFO> modes;
    UINT32 flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    LONG result = ERROR_SUCCESS;

    do
    {
        // Determine how many path and mode structures to allocate
        UINT32 pathCount, modeCount;
        result = GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount);

        if (result != ERROR_SUCCESS)
        {
            LOGERR("GetDisplayConfigBufferSizes failed (error: 0x%08x)", result);
            return HRESULT_FROM_WIN32(result);
        }

        // Allocate the path and mode arrays
        paths.resize(pathCount);
        modes.resize(modeCount);

        // Get all active paths and their modes
        result = QueryDisplayConfig(flags, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);

        // The function may have returned fewer paths/modes than estimated
        paths.resize(pathCount);
        modes.resize(modeCount);

        // It's possible that between the call to GetDisplayConfigBufferSizes and QueryDisplayConfig
        // that the display state changed, so loop on the case of ERROR_INSUFFICIENT_BUFFER.
    } while (result == ERROR_INSUFFICIENT_BUFFER);

    if (result != ERROR_SUCCESS)
    {
        LOGERR("QueryDisplayConfig failed (error: 0x%08x)", result);
        return HRESULT_FROM_WIN32(result);
    }

    int i = 0;
    // For each active path
    for (int i = 0; i < paths.size(); i++)
    {
        // Find the target (monitor) friendly name
        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName = {};
        targetName.header.adapterId = paths[i].targetInfo.adapterId;
        targetName.header.id = paths[i].targetInfo.id;
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        result = DisplayConfigGetDeviceInfo(&targetName.header);

        if (result != ERROR_SUCCESS)
        {
            return HRESULT_FROM_WIN32(result);
        }

        char devNameStr[64];
        char DefChar = ' ';
        WideCharToMultiByte(CP_ACP, 0, targetName.monitorFriendlyDeviceName, -1, devNameStr, 64, &DefChar, NULL);

        displays[i].friendlyName = string(devNameStr);
    }
    return 1;
}

HRESULT InitD3D9(unsigned int deviceID, HWND hwnd, UINT presentationInterval)
{
    HRESULT hr = S_OK;
    D3DPRESENT_PARAMETERS d3dpp;

    // Create the Direct3D9 device and the swap chain.
    ZeroMemory(&d3dpp, sizeof(d3dpp));

    d3dpp.Windowed = TRUE;
    d3dpp.BackBufferFormat = D3DFMT_A2R10G10B10;
    //d3dpp.BackBufferFormat = D3DFMT_X8R8G8B8;

    d3dpp.BackBufferWidth  = BUF_WIDTH;
    d3dpp.BackBufferHeight = BUF_HEIGHT;
    d3dpp.BackBufferCount = 1;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    //d3dpp.SwapEffect = D3DSWAPEFFECT_FLIPEX;
    d3dpp.PresentationInterval = presentationInterval;
    //d3dpp.Flags = D3DPRESENTFLAG_VIDEO;
    d3dpp.hDeviceWindow = hwnd;
    DWORD dwBehaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING;

    hr = g_pD3DEx->CreateDeviceEx(
        deviceID,
        D3DDEVTYPE_HAL,
        hwnd,
        dwBehaviorFlags,
        &d3dpp,
        NULL,
        &g_pD3D9Device);

    assert(SUCCEEDED(hr));

    return hr;
}

HRESULT InitD3D9Surfaces()
{
    HRESULT hr = E_FAIL;

    if (g_pD3D9Device)
    {

        hr = g_pD3D9Device->CreateOffscreenPlainSurface(BUF_WIDTH, BUF_HEIGHT, D3DFMT_A2B10G10R10, D3DPOOL_DEFAULT, &g_backbuffer, NULL); //D3DFMT_A8R8G8B8 D3DFMT_A2B10G10R10
        if (FAILED(hr))
        {
            LOGERR("Failed to create D3D9 surface D3DFMT_A2B10G10R10 (error: 0x%08x)", hr);
        }
        g_pD3D9Device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_backbuffer);
    }

    return hr;
}

// this is the main message handler for the program
LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_DESTROY:
    {
        PostQuitMessage(0);
        return 0;
    } break;
    }

    return DefWindowProc(hWnd, message, wParam, lParam);
}

int ReadIntFromCmd(string prompt) {
    cout << prompt;
    string cinString;
    getline(cin, cinString);
    return cinString.empty() ? -1 : stoi(cinString);
}

bool ParseCommandLineArgs(LPSTR lpCmdLine, int* sourceIndex, int* targetIndex, string* framerateStr) {
    *sourceIndex = -1;
    *targetIndex = -1;
    *framerateStr = "";

    if (!lpCmdLine || strlen(lpCmdLine) == 0) {
        return false;
    }

    string cmdLine(lpCmdLine);
    vector<string> args;

    // Split command line into tokens
    size_t pos = 0;
    while (pos < cmdLine.length()) {
        // Skip whitespace
        while (pos < cmdLine.length() && cmdLine[pos] == ' ') {
            pos++;
        }
        if (pos >= cmdLine.length()) break;

        // Extract token
        size_t tokenStart = pos;
        while (pos < cmdLine.length() && cmdLine[pos] != ' ') {
            pos++;
        }
        args.push_back(cmdLine.substr(tokenStart, pos - tokenStart));
    }

    bool foundAny = false;

    // Parse arguments
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "-source" && i + 1 < args.size()) {
            *sourceIndex = stoi(args[i + 1]);
            foundAny = true;
            i++; // Skip the value
        }
        else if (args[i] == "-target" && i + 1 < args.size()) {
            *targetIndex = stoi(args[i + 1]);
            foundAny = true;
            i++; // Skip the value
        }
        else if (args[i] == "-framerate" && i + 1 < args.size()) {
            *framerateStr = args[i + 1];  // Store as string instead of converting to int
            foundAny = true;
            i++; // Skip the value
        }
    }

    return foundAny;
}

void ConsoleUserInput(string* framerateStr) {
    AllocConsole();
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONIN$", "r", stdin);
    cout.clear();
    clog.clear();
    cin.clear();
    cout << endl;
    for (vector<DisplayPosition>::iterator iter = displays.begin(); iter < displays.end(); iter++) {

        cout << "Adapter index [" << iter->dxAdapterIndex << "]"
            << endl << "\t"
            << "Scaled Position Top Left [" << iter->position.left << "," << iter->position.top << "]"
            << " | Scaled Position Bottom Right [" << iter->position.right << "," << iter->position.bottom << "]"
            << endl << "\t"
            << "Identifier [" << iter->deviceName << "]"
            << endl << "\t"
            << "Name [" << iter->friendlyName << "]"
            << endl;
    }

    int sourceIndex;
    int outputIndex;
    for (sourceIndex = ReadIntFromCmd("Capture Display Index ? "); sourceIndex < 0 || sourceIndex > displays.size() - 1;) {
        sourceIndex = ReadIntFromCmd("Capture Display Index ? ");
    }
    for (outputIndex = ReadIntFromCmd("Output Display Index ? "); outputIndex < 0 || outputIndex > displays.size() - 1;) {
        outputIndex = ReadIntFromCmd("Output Display Index ? ");
    }
    cout << "Capture/Present framerate ('vsync', 't' for vsync temporal, 't:60' for timed temporal, 'b' for vsync blend, 'b:60' for timed blend, or fps number, blank for vsync) ? ";
    string cinString;
    getline(cin, cinString);
    if (!cinString.empty())
        *framerateStr = cinString;

    for (vector<DisplayPosition>::iterator iter = displays.begin(); iter < displays.end(); iter++) {

        if (iter->dxAdapterIndex == outputIndex) {
            target = *iter;
        }
        if (iter->dxAdapterIndex == sourceIndex) {
            source = *iter;
        }
    }

    fclose(stdin);
    fclose(stdout);
    FreeConsole();
}

// the entry point for any Windows program
_Use_decl_annotations_ int WINAPI WinMain(HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPSTR lpCmdLine,
    int nCmdShow)
{

    if (!InitDisplays()) {
        LOGERR("Unable to enumerate display adapters");
        return -1;
    }

    // Try to parse command line arguments
    LOG("Command line received: '%s'", lpCmdLine ? lpCmdLine : "(null)");
    int argSourceIndex = -1;
    int argTargetIndex = -1;
    string framerateStr = "";
    bool hasArgs = ParseCommandLineArgs(lpCmdLine, &argSourceIndex, &argTargetIndex, &framerateStr);
    LOG("Parsed args - hasArgs: %d, source: %d, target: %d, framerate: %s", hasArgs, argSourceIndex, argTargetIndex, framerateStr.c_str());

    // If all required args are provided via command line, use them
    if (hasArgs && argSourceIndex >= 0 && argTargetIndex >= 0 &&
        argSourceIndex < static_cast<int>(displays.size()) && argTargetIndex < static_cast<int>(displays.size())) {
        // Set source and target from command line args
        for (vector<DisplayPosition>::iterator iter = displays.begin(); iter < displays.end(); iter++) {
            if (iter->dxAdapterIndex == argTargetIndex) {
                target = *iter;
            }
            if (iter->dxAdapterIndex == argSourceIndex) {
                source = *iter;
            }
        }
    } else {
        // Fall back to interactive console input
        ConsoleUserInput(&framerateStr);
    }

    // Create capture mode instance
    IFrameCaptureMode* captureMode = ParseCaptureMode(framerateStr);
    if (!captureMode) {
        LOGERR("Failed to create capture mode");
        Cleanup();
        return -1;
    }

    LOG("=== NvFBCR Starting ===");
    LOG("Source display: [%d] %s (%s)", source.dxAdapterIndex, source.friendlyName.c_str(), source.deviceName);
    LOG("Target display: [%d] %s (%s)", target.dxAdapterIndex, target.friendlyName.c_str(), target.deviceName);
    LOG("Capture mode: %s", captureMode->GetModeName());
    LOG("Buffer size: %dx%d", BUF_WIDTH, BUF_HEIGHT);

    BUF_WIDTH = target.position.right - target.position.left;
    BUF_HEIGHT = target.position.bottom - target.position.top;

    HWND hWnd;
    WNDCLASSEX wc;

    DWORD maxDisplayWidth = -1, maxDisplayHeight = -1;

    ZeroMemory(&wc, sizeof(WNDCLASSEX));

    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "WindowClass";

    // register the window class
    RegisterClassEx(&wc);

    hWnd = CreateWindowEx(NULL,
        "WindowClass",
        "NvFBCR",
        WS_EX_TOPMOST | WS_POPUP, // pseudo fullscreen //WS_OVERLAPPEDWINDOW for windowed
        target.position.left, target.position.top,    // the starting x and y positions
        BUF_WIDTH, BUF_HEIGHT,
        NULL,
        NULL,
        hInstance,
        NULL);

    ShowWindow(hWnd, nCmdShow);

    NvFBCFrameGrabInfo frameGrabInfo = { 0 };

    //! DX9 resources
    NVFBC_TODX9VID_OUT_BUF NvFBC_OutBuf[1];

    //! Load the nvfbc Library
    pNVFBCLib = new NvFBCLibrary();
    if (!pNVFBCLib->load())
    {
        LOGERR("Unable to load the NvFBC library");
        return -1;
    }

    g_bNvFBCLibLoaded = true;
    if (!SUCCEEDED(InitD3D9(source.dxAdapterIndex, hWnd, captureMode->GetPresentationInterval())))
    {
        LOGERR("Unable to create D3D9Ex Device");
        delete captureMode;
        Cleanup();
        return -1;
    }

    if (!SUCCEEDED(InitD3D9Surfaces()))
    {
        LOGERR("Unable to create D3D9Ex surfaces");
        Cleanup();
        return -1;
    }
    //! Create an instance of the NvFBCDX9 class, the first argument specifies the frame buffer
    NvFBCDX9 = (NvFBCToDx9Vid*)pNVFBCLib->create(NVFBC_TO_DX9_VID, &maxDisplayWidth, &maxDisplayHeight, 0, (void*)g_pD3D9Device);
    if (!NvFBCDX9)
    {
        // Check if NvFBC is not enabled
        NvFBCStatusEx status = {0};
        status.dwVersion = NVFBC_STATUS_VER;
        status.dwAdapterIdx = 0;
        NVFBCRESULT statusRes = pNVFBCLib->getStatus(&status);

        if (statusRes == NVFBC_SUCCESS && !status.bIsCapturePossible)
        {
            LOG("NvFBC not enabled (bIsCapturePossible=false), attempting to enable...");

            // Check if running as admin before attempting to enable
            if (!IsRunningAsAdmin())
            {
                LOGERR("Failed to enable NvFBC: Administrator privileges required");
                LOGERR("Please run NvFBCR.exe as Administrator to enable NvFBC");
                LOGERR("Alternatively, run NvFBCEnable.exe as Administrator first, then run NvFBCR.exe");

                // Show error to user in console
                AllocConsole();
                FILE* fDummy;
                freopen_s(&fDummy, "CONOUT$", "w", stdout);
                freopen_s(&fDummy, "CONIN$", "r", stdin);
                cout.clear();
                cin.clear();

                cout << "\nERROR: Failed to enable NvFBC\n";
                cout << "---------------------------------------\n";
                cout << "Administrator privileges are required to enable NvFBC.\n\n";
                cout << "Please either:\n";
                cout << "  1. Run NvFBCR.exe as Administrator, OR\n";
                cout << "  2. Run NvFBCEnable.exe as Administrator first, then run NvFBCR.exe normally\n\n";
                cout << "Press Enter to exit...";
                cin.get();

                fclose(stdin);
                fclose(stdout);
                FreeConsole();

                Cleanup();
                return -1;
            }

            NVFBCRESULT enableRes = pNVFBCLib->enable(NVFBC_STATE_ENABLE);

            if (enableRes != NVFBC_SUCCESS)
            {
                LOGERR("Failed to enable NvFBC (result: 0x%X) - cannot proceed", enableRes);
                Cleanup();
                return -1;
            }

            LOG("NvFBC enabled successfully - restarting application...");
            Cleanup();

            // Restart the process with current configuration
            STARTUPINFO si = { 0 };
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi = { 0 };
            char exePath[MAX_PATH];
            GetModuleFileNameA(NULL, exePath, MAX_PATH);

            // Build command line with current values
            // Note: lpCommandLine must include the exe name as first token when lpApplicationName is non-NULL
            // Windows will strip the first token and pass the rest to WinMain's lpCmdLine
            char newCmdLine[512];
            sprintf_s(newCmdLine, sizeof(newCmdLine), "\"%s\" -source %d -target %d -framerate %s",
                exePath, source.dxAdapterIndex, target.dxAdapterIndex,
                framerateStr.empty() ? "vsync" : framerateStr.c_str());

            LOG("Relaunching with command line: '%s'", newCmdLine);
            LOG("Executable path: '%s'", exePath);

            if (CreateProcessA(NULL, newCmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
            {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                return 0;
            }
            else
            {
                LOGERR("Failed to restart process (error: %d)", GetLastError());
                return -1;
            }
        }

        LOGERR("Failed to create NvFBCToDx9Vid instance");
        LOGERR("    Requirement 1) Driver R355+ with Tesla/Quadro/GRID");
        LOGERR("    Requirement 2) Run 'NvFBCEnable -enable' after driver installation");
        Cleanup();
        return -1;
    }

    LOG("NvFBCToDX9Vid instance created successfully");

    NvFBC_OutBuf[0].pPrimary = g_backbuffer;


    NVFBC_TODX9VID_SETUP_PARAMS DX9SetupParams = {};
    DX9SetupParams.dwVersion = NVFBC_TODX9VID_SETUP_PARAMS_V3_VER;
    DX9SetupParams.bWithHWCursor = 1;
    DX9SetupParams.bStereoGrab = 0;
    DX9SetupParams.bDiffMap = 0;
    DX9SetupParams.ppBuffer = NvFBC_OutBuf;
    DX9SetupParams.eMode = NVFBC_TODX9VID_ARGB10; //NVFBC_TODX9VID_ARGB10; //NVFBC_TODX9VID_ARGB;
    DX9SetupParams.dwNumBuffers = 1;
    DX9SetupParams.bHDRRequest = TRUE;


    if (NVFBC_SUCCESS != NvFBCDX9->NvFBCToDx9VidSetUp(&DX9SetupParams))
    {
        LOGERR("Failed calling NvFBCToDx9VidSetUp()");
        Cleanup();
        return -1;
    }

    //! Setup NvFBC Grab Parameters
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS fbcDX9GrabParams = { 0 };
    {
        fbcDX9GrabParams.dwVersion = NVFBC_TODX9VID_GRAB_FRAME_PARAMS_V1_VER;
        fbcDX9GrabParams.dwFlags = NVFBC_TODX9VID_NOWAIT;
        fbcDX9GrabParams.eGMode = NVFBC_TODX9VID_SOURCEMODE_SCALE;
        fbcDX9GrabParams.dwTargetWidth = BUF_WIDTH;
        fbcDX9GrabParams.dwTargetHeight = BUF_HEIGHT;
        fbcDX9GrabParams.pNvFBCFrameGrabInfo = &frameGrabInfo;
    }

    // Setup and run capture mode
    if (!captureMode->Setup()) {
        LOGERR("Failed to setup capture mode");
        delete captureMode;
        Cleanup();
        return -1;
    }

    LOG("Entering capture loop - mode: %s", captureMode->GetModeName());

    // Run the mode's capture loop (contains entire loop logic including message processing)
    captureMode->Run(NvFBCDX9, &fbcDX9GrabParams, g_pD3D9Device, hWnd);

    // Clean up capture mode
    delete captureMode;

    Cleanup();

    return 0;
}
