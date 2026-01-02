#include "VsyncWindowFollowCaptureMode.h"
#include <SimpleLogger.h>

// External globals from NvFBCR.cpp
extern int BUF_WIDTH;
extern int BUF_HEIGHT;
extern IDirect3DSurface9* g_backbuffer;

struct DisplayPosition {
    int dxAdapterIndex;
    RECT position;
    char deviceName[32];
    std::string friendlyName;
};

extern DisplayPosition source;

VsyncWindowFollowCaptureMode::VsyncWindowFollowCaptureMode(const std::string& windowTitle)
    : m_targetWindow(NULL)
    , m_windowTitle(windowTitle)
    , m_windowFound(false)
    , m_captureWidth(0)
    , m_captureHeight(0)
    , m_lastLoggedWidth(0)
    , m_lastLoggedHeight(0)
{
    m_lastWindowRect = {0, 0, 0, 0};
    m_perfFreq.QuadPart = 0;
}

VsyncWindowFollowCaptureMode::~VsyncWindowFollowCaptureMode() {}

UINT VsyncWindowFollowCaptureMode::GetPresentationInterval() const {
    return D3DPRESENT_INTERVAL_ONE;
}

bool VsyncWindowFollowCaptureMode::Setup() {
    QueryPerformanceFrequency(&m_perfFreq);

    // Try to find the window
    m_targetWindow = FindWindowByTitle(m_windowTitle);

    if (!m_targetWindow) {
        LOG("Window '%s' not found - will search each frame until found", m_windowTitle.c_str());
    } else {
        RECT rect;
        if (GetWindowRect(m_targetWindow, &rect)) {
            m_lastWindowRect = rect;
            m_captureWidth = rect.right - rect.left;
            m_captureHeight = rect.bottom - rect.top;
            m_windowFound = true;
            LOG("Found window '%s' at (%d, %d) size %dx%d",
                m_windowTitle.c_str(),
                rect.left, rect.top,
                m_captureWidth, m_captureHeight);
        }
    }

    LOG("VSync window follow mode initialized - tracking: '%s'", m_windowTitle.c_str());
    LOG("Output FPS will match target monitor's refresh rate");
    return true;
}

void VsyncWindowFollowCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    MSG msg;
    bool firstFrame = true;

    while (TRUE)
    {
        // Update window tracking
        if (!UpdateWindowTracking()) {
            // Window not found or lost - try to find it again
            m_targetWindow = FindWindowByTitle(m_windowTitle);
            if (m_targetWindow) {
                LOG("Window '%s' found/recovered", m_windowTitle.c_str());
                UpdateWindowTracking();
            }
        }

        // Update grab parameters with current window position
        if (m_windowFound) {
            // Get window dimensions
            int windowWidth = m_lastWindowRect.right - m_lastWindowRect.left;
            int windowHeight = m_lastWindowRect.bottom - m_lastWindowRect.top;

            // Calculate aspect ratios
            float windowAspect = (float)windowWidth / (float)windowHeight;
            float outputAspect = (float)BUF_WIDTH / (float)BUF_HEIGHT;

            // Calculate crop to match output aspect ratio
            int cropX = 0;
            int cropY = 0;
            int cropWidth = windowWidth;
            int cropHeight = windowHeight;

            // Check if window size changed - only log on size changes
            bool sizeChanged = (windowWidth != m_lastLoggedWidth || windowHeight != m_lastLoggedHeight);

            if (windowAspect > outputAspect) {
                // Window is wider than output - crop left/right sides
                cropWidth = (int)((float)windowHeight * outputAspect);
                cropX = (windowWidth - cropWidth) / 2;
                if (sizeChanged) {
                    LOG("Window resized: %dx%d - aspect (%.2f) wider than output (%.2f), cropping %d pixels from sides",
                        windowWidth, windowHeight, windowAspect, outputAspect, cropX);
                }
            } else if (windowAspect < outputAspect) {
                // Window is taller than output - crop top/bottom
                cropHeight = (int)((float)windowWidth / outputAspect);
                cropY = (windowHeight - cropHeight) / 2;
                if (sizeChanged) {
                    LOG("Window resized: %dx%d - aspect (%.2f) taller than output (%.2f), cropping %d pixels from top/bottom",
                        windowWidth, windowHeight, windowAspect, outputAspect, cropY);
                }
            } else {
                if (sizeChanged) {
                    LOG("Window resized: %dx%d - aspect (%.2f) matches output (%.2f), no cropping needed",
                        windowWidth, windowHeight, windowAspect, outputAspect);
                }
            }

            // Update last logged size
            if (sizeChanged) {
                m_lastLoggedWidth = windowWidth;
                m_lastLoggedHeight = windowHeight;
            }

            // Convert to absolute coordinates on source display
            int absoluteX = m_lastWindowRect.left + cropX;
            int absoluteY = m_lastWindowRect.top + cropY;

            // Convert to coordinates relative to the source display
            int relativeX = absoluteX - source.position.left;
            int relativeY = absoluteY - source.position.top;

            // Clamp to display bounds
            relativeX = max(0, min(relativeX, (int)BUF_WIDTH));
            relativeY = max(0, min(relativeY, (int)BUF_HEIGHT));

            // Clamp crop size to fit within display
            cropWidth = min(cropWidth, (int)BUF_WIDTH - relativeX);
            cropHeight = min(cropHeight, (int)BUF_HEIGHT - relativeY);

            // Align dimensions to multiples of 4 (common video API requirement)
            // This prevents potential NvFBC errors with odd dimensions
            relativeX = (relativeX / 4) * 4;
            relativeY = (relativeY / 4) * 4;
            cropWidth = (cropWidth / 4) * 4;
            cropHeight = (cropHeight / 4) * 4;

            // Ensure minimum size of 4x4
            if (cropWidth < 4) cropWidth = 4;
            if (cropHeight < 4) cropHeight = 4;

            // Debug logging when size changes
            if (sizeChanged) {
                LOG("NvFBC params: dwStartX=%d, dwStartY=%d, dwTargetWidth=%d, dwTargetHeight=%d",
                    relativeX, relativeY, cropWidth, cropHeight);
            }

            // Use CROP mode to capture the cropped region at native resolution
            grabParams->eGMode = NVFBC_TODX9VID_SOURCEMODE_CROP;
            grabParams->dwStartX = relativeX;
            grabParams->dwStartY = relativeY;
            grabParams->dwTargetWidth = cropWidth;
            grabParams->dwTargetHeight = cropHeight;

            // On first successful window track, log the region
            if (firstFrame) {
                LOG("Capturing region: offset (%d, %d) size %dx%d (cropped to match aspect)",
                    relativeX, relativeY, cropWidth, cropHeight);
                LOG("Window size: %dx%d, Cropped size: %dx%d, Output size: %dx%d",
                    windowWidth, windowHeight, cropWidth, cropHeight, BUF_WIDTH, BUF_HEIGHT);
                firstFrame = false;
            }
        } else {
            // Window not found - capture full display as fallback
            grabParams->eGMode = NVFBC_TODX9VID_SOURCEMODE_SCALE;
            grabParams->dwStartX = 0;
            grabParams->dwStartY = 0;
            grabParams->dwTargetWidth = BUF_WIDTH;
            grabParams->dwTargetHeight = BUF_HEIGHT;
        }

        // Clear backbuffer to black to prevent ghosting from previous frames
        // Use ColorFill since g_backbuffer might not be a render target
        device->ColorFill(g_backbuffer, NULL, D3DCOLOR_XRGB(0, 0, 0));

        // Poll for latest frame (never blocks - always gets most recent frame available)
        NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);

        if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION)
        {
            LOGERR("NvFBC session invalidated - session needs to be recreated");
            break;
        }
        // Ignore other errors (e.g., no new frame) - we'll just present what we have

        // Present and wait for VSync - this blocks until monitor refresh
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

const char* VsyncWindowFollowCaptureMode::GetModeName() const {
    return "VsyncWindowFollow";
}

// Find window by partial title match
BOOL CALLBACK VsyncWindowFollowCaptureMode::EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    struct SearchParams {
        std::string searchTitle;
        HWND result;
    };

    SearchParams* params = reinterpret_cast<SearchParams*>(lParam);

    char windowTitle[256];
    GetWindowTextA(hwnd, windowTitle, sizeof(windowTitle));

    // Check if window is visible and title matches
    if (IsWindowVisible(hwnd) && strstr(windowTitle, params->searchTitle.c_str()) != NULL) {
        params->result = hwnd;
        return FALSE; // Stop enumeration
    }

    return TRUE; // Continue enumeration
}

HWND VsyncWindowFollowCaptureMode::FindWindowByTitle(const std::string& title) {
    struct SearchParams {
        std::string searchTitle;
        HWND result;
    } params;

    params.searchTitle = title;
    params.result = NULL;

    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&params));

    return params.result;
}

bool VsyncWindowFollowCaptureMode::UpdateWindowTracking() {
    if (!m_targetWindow) {
        m_windowFound = false;
        return false;
    }

    // Get current window position - this will fail if window is invalid
    RECT rect;
    if (!GetWindowRect(m_targetWindow, &rect)) {
        LOG("Window '%s' closed or invalid", m_windowTitle.c_str());
        m_targetWindow = NULL;
        m_windowFound = false;
        return false;
    }

    // Check if window moved or resized
    if (rect.left != m_lastWindowRect.left ||
        rect.top != m_lastWindowRect.top ||
        rect.right != m_lastWindowRect.right ||
        rect.bottom != m_lastWindowRect.bottom) {

        // Window moved or resized
        m_lastWindowRect = rect;
        m_captureWidth = rect.right - rect.left;
        m_captureHeight = rect.bottom - rect.top;

        // Only log significant moves (not every pixel)
        static LARGE_INTEGER lastLogTime = {0};
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);

        if (lastLogTime.QuadPart == 0 ||
            (currentTime.QuadPart - lastLogTime.QuadPart) > m_perfFreq.QuadPart) {
            LOG("Window '%s' moved to (%d, %d) size %dx%d",
                m_windowTitle.c_str(),
                rect.left, rect.top,
                m_captureWidth, m_captureHeight);
            lastLogTime = currentTime;
        }
    }

    // Check if window is minimized
    if (IsIconic(m_targetWindow)) {
        LOG("Window '%s' is minimized - cannot capture", m_windowTitle.c_str());
        m_windowFound = false;
        return false;
    }

    m_windowFound = true;
    return true;
}