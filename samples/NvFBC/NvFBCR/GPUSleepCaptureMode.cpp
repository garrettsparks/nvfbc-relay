#include "GPUSleepCaptureMode.h"
#include <SimpleLogger.h>

GPUSleepCaptureMode::GPUSleepCaptureMode(float framerate)
    : m_targetIntervalUs((__int64)(1000000.0f / framerate))
    , m_framerate(framerate)
{
    m_perfFreq.QuadPart = 0;
}

GPUSleepCaptureMode::~GPUSleepCaptureMode() {
}

UINT GPUSleepCaptureMode::GetPresentationInterval() const {
    return D3DPRESENT_INTERVAL_IMMEDIATE;
}

bool GPUSleepCaptureMode::Setup() {
    QueryPerformanceFrequency(&m_perfFreq);

    LOG("GPU sleep mode initialized - target framerate: %.2f fps (interval: %lld us)", m_framerate, m_targetIntervalUs);
    return true;
}

void GPUSleepCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    MSG msg;

    while (TRUE)
    {
        // Record frame start time
        LARGE_INTEGER frameStart;
        QueryPerformanceCounter(&frameStart);

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

        // Measure elapsed work time
        LARGE_INTEGER frameEnd;
        QueryPerformanceCounter(&frameEnd);
        __int64 elapsedUs = (frameEnd.QuadPart - frameStart.QuadPart) * 1000000 / m_perfFreq.QuadPart;

        // Sleep for remaining time
        __int64 sleepUs = m_targetIntervalUs - elapsedUs;
        LOG("Frame work: %lld us, sleep: %lld us", elapsedUs, sleepUs > 0 ? sleepUs : 0);
        if (sleepUs > 0)
        {
            NVFBCRESULT sleepRes = nvfbcDx9->NvFBCToDx9VidGPUBasedCPUSleep(sleepUs);
            if (sleepRes != NVFBC_SUCCESS)
            {
                LOGERR("NvFBCToDx9VidGPUBasedCPUSleep failed (result: %d)", sleepRes);
            }
        }
    }
}

const char* GPUSleepCaptureMode::GetModeName() const {
    return "GPU Sleep";
}
