#include "TimerCaptureMode.h"
#include <SimpleLogger.h>

TimerCaptureMode::TimerCaptureMode(float framerate)
    : m_framerate(framerate)
{
}

UINT TimerCaptureMode::GetPresentationInterval() const {
    return D3DPRESENT_INTERVAL_IMMEDIATE;
}

bool TimerCaptureMode::Setup() {
    if (!m_scheduler.Setup(m_framerate)) {
        return false;
    }
    LOG("Timer mode initialized - target framerate: %.2f fps", m_framerate);
    return true;
}

void TimerCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    MSG msg;

    m_scheduler.Seed();

    while (TRUE)
    {
        // Grab frame
        NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);

        if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION)
        {
            LOGERR("NvFBC session invalidated - session needs to be recreated");
            break;
        }

        // Present immediately (non-blocking), because the device was CREATED with
        // INTERVAL_IMMEDIATE. dwFlags is 0 and must stay 0: it is not an interval, and
        // D3DPRESENT_INTERVAL_IMMEDIATE (0x80000000) is not even a defined flag bit
        // (see TemporalCaptureMode::Run).
        device->PresentEx(NULL, NULL, NULL, NULL, 0);

        // Process Windows messages
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (msg.message == WM_QUIT)
            break;

        // Wait out the rest of this frame on the absolute schedule, then advance.
        m_scheduler.WaitUntilDeadline();
        m_scheduler.Advance();
    }
}

const char* TimerCaptureMode::GetModeName() const {
    return "Timer";
}
