#include "TimerCaptureMode.h"
#include <SimpleLogger.h>

TimerCaptureMode::TimerCaptureMode(float framerate)
    : m_timer(NULL)
    , m_framerate(framerate)
{
    m_interval.QuadPart = -(LONGLONG)(10000000.0f / framerate);
}

TimerCaptureMode::~TimerCaptureMode() {
    if (m_timer) {
        CloseHandle(m_timer);
        m_timer = NULL;
    }
}

UINT TimerCaptureMode::GetPresentationInterval() const {
    return D3DPRESENT_INTERVAL_IMMEDIATE;
}

bool TimerCaptureMode::Setup() {
    m_timer = CreateWaitableTimer(NULL, TRUE, NULL);

    if (NULL == m_timer) {
        LOGERR("CreateWaitableTimer failed (error: %d)", GetLastError());
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

const char* TimerCaptureMode::GetModeName() const {
    return "Timer";
}