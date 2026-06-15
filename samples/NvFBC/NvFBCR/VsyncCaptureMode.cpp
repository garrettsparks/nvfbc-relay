#include "VsyncCaptureMode.h"
#include <SimpleLogger.h>

VsyncCaptureMode::VsyncCaptureMode() {}

VsyncCaptureMode::~VsyncCaptureMode() {}

UINT VsyncCaptureMode::GetPresentationInterval() const {
    // IMMEDIATE: pacing comes from an explicit WaitForVBlank on the TARGET output, not from
    // INTERVAL_ONE (which would sync to the present device's SOURCE adapter — the wrong display).
    return D3DPRESENT_INTERVAL_IMMEDIATE;
}

bool VsyncCaptureMode::Setup() {
    LOG("VSync mode initialized - pacing on the TARGET display's vblank (WaitForVBlank)");
    LOG("Output FPS will match the capture-card/target monitor's refresh rate");
    return true;
}

void VsyncCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    MSG msg;

    // The window is pseudo-fullscreen on the target display, so MonitorFromWindow gives the
    // target HMONITOR. Bail loudly if the vblank source can't bind rather than free-run.
    if (!m_vblank.Setup(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)))
    {
        LOGERR("VSync: could not bind vblank waiter to target display - aborting");
        return;
    }

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

        // Block until the TARGET display's vblank, then present non-blocking. This is the
        // frame-pacing wait; locking it to the named output (not the device adapter) is the fix
        // for vsync syncing to the source display.
        if (!m_vblank.Wait())
            break;   // output lost
        device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);

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
