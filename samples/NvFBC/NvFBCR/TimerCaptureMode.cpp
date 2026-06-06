#include "TimerCaptureMode.h"
#include <SimpleLogger.h>

TimerCaptureMode::TimerCaptureMode(float framerate)
    : m_timer(NULL)
    , m_periodQpc(0)
    , m_framerate(framerate)
{
    m_freq.QuadPart = 0;
    m_nextPresent.QuadPart = 0;
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
    m_timer = CreateWaitableTimerEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION | CREATE_WAITABLE_TIMER_MANUAL_RESET, TIMER_ALL_ACCESS);

    if (NULL == m_timer) {
        LOGERR("CreateWaitableTimerEx failed (error: %d)", GetLastError());
        return false;
    }

    QueryPerformanceFrequency(&m_freq);
    // Round to nearest tick to minimize accumulated rounding error over many frames.
    m_periodQpc = (LONGLONG)((double)m_freq.QuadPart / m_framerate + 0.5);

    LOG("Timer mode initialized - target framerate: %.2f fps (period: %lld ticks)", m_framerate, m_periodQpc);
    return true;
}

void TimerCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    MSG msg;

    // Initiate the absolute schedule one period out from now.
    QueryPerformanceCounter(&m_nextPresent);
    m_nextPresent.QuadPart += m_periodQpc;

    while (TRUE)
    {
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

        // Wait until the absolute scheduled deadline. Advancing the deadline by a fixed
        // period each frame keeps the average rate pinned to the target period,
        // so wake-latency jitter does not accumulate into drift.
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG ticksUntilPresent = m_nextPresent.QuadPart - now.QuadPart;
        if (ticksUntilPresent > 0)
        {
            LARGE_INTEGER due;
            due.QuadPart = -(ticksUntilPresent * 10000000 / m_freq.QuadPart);  // 100ns units, relative
            SetWaitableTimer(m_timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(m_timer, INFINITE);
        }
        m_nextPresent.QuadPart += m_periodQpc;
    }
}

const char* TimerCaptureMode::GetModeName() const {
    return "Timer";
}
