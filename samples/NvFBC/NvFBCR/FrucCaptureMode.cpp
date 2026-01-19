#include "FrucCaptureMode.h"
#include <SimpleLogger.h>

FrucCaptureMode::FrucCaptureMode(float framerate)
    : m_targetFramerate(framerate == 0.0f ? 60.0f : framerate)
    , m_isVsyncMode(framerate == 0.0f)
{
    LOG("=== FrucCaptureMode STUB created ===");
    LOG("Target framerate: %.2f fps", m_targetFramerate);
    LOG("VSync mode: %s", m_isVsyncMode ? "yes" : "no");
}

FrucCaptureMode::~FrucCaptureMode() {
    LOG("=== FrucCaptureMode STUB destroyed ===");
}

UINT FrucCaptureMode::GetPresentationInterval() const {
    return m_isVsyncMode ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
}

bool FrucCaptureMode::Setup() {
    LOG("FrucCaptureMode::Setup() - STUB (doing nothing, returning success)");
    return true;
}

void FrucCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    LOG("FrucCaptureMode::Run() - STUB (simple capture/present loop, no interpolation)");

    MSG msg;
    int frameCount = 0;

    while (TRUE) {
        // Simple capture
        NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);

        if (fbcRes == NVFBC_SUCCESS) {
            frameCount++;

            if (frameCount % 60 == 0) {
                LOG("STUB: Captured and presented %d frames", frameCount);
            }

            // Simple present (no interpolation)
            device->PresentEx(NULL, NULL, NULL, NULL, GetPresentationInterval());
        }
        else if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("NvFBC session invalidated");
            break;
        }

        // Process messages
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (msg.message == WM_QUIT)
            break;
    }

    LOG("FrucCaptureMode::Run() exiting - captured %d frames total", frameCount);
}

const char* FrucCaptureMode::GetModeName() const {
    static char modeName[64];
    sprintf_s(modeName, sizeof(modeName), "FRUC-STUB-%.2f", m_targetFramerate);
    return modeName;
}