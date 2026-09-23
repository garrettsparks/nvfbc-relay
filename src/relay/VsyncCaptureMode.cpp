#include "VsyncCaptureMode.h"
#include <SimpleLogger.h>

VsyncCaptureMode::VsyncCaptureMode() {}

VsyncCaptureMode::~VsyncCaptureMode() {}

UINT VsyncCaptureMode::GetPresentationInterval() const {
    return D3DPRESENT_INTERVAL_ONE;
}

bool VsyncCaptureMode::Setup() {
    LOG("VSync mode initialized - VSync will control frame timing");
    LOG("Output FPS will match target monitor's refresh rate");
    return true;
}

void VsyncCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
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

        // Present and wait for VSync - this blocks until monitor refresh, because the device
        // was CREATED with INTERVAL_ONE. dwFlags is 0 and must stay 0: it is not an interval,
        // and D3DPRESENT_INTERVAL_ONE is numerically D3DPRESENT_DONOTWAIT, which asks the
        // runtime to skip the present rather than wait (see TemporalCaptureMode::Run).
        device->PresentEx(NULL, NULL, NULL, NULL, 0);

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

const char* VsyncCaptureMode::GetModeName() const {
    return "VSync";
}
