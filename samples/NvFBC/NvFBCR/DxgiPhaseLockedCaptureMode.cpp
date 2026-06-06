#include "DxgiPhaseLockedCaptureMode.h"
#include <SimpleLogger.h>

DxgiPhaseLockedCaptureMode::DxgiPhaseLockedCaptureMode(float framerate)
    : m_timer(NULL)
    , m_perfFreqQuad(0)
    , m_targetPeriodQpc(0)
    , m_marginQpc(0)
    , m_nextPresentQpc(0)
    , m_framerate(framerate)
    , m_framesSinceResync(0)
    , m_targetOutput(NULL)
    , m_lastVblankQpc(0)
    , m_observedPeriodQpc(0)
    , m_vblankThreadStop(false)
{
}

DxgiPhaseLockedCaptureMode::~DxgiPhaseLockedCaptureMode() {
    // Backstop teardown in case Run() exited early. Normal teardown happens at
    // the end of Run().
    if (m_vblankThread.joinable()) {
        m_vblankThreadStop.store(true);
        m_vblankThread.join();
    }
    if (m_targetOutput) {
        m_targetOutput->Release();
        m_targetOutput = NULL;
    }
    if (m_timer) {
        CloseHandle(m_timer);
        m_timer = NULL;
    }
}

UINT DxgiPhaseLockedCaptureMode::GetPresentationInterval() const {
    // The mode owns its own scheduling; Present must not block on D3D9-side vsync.
    return D3DPRESENT_INTERVAL_IMMEDIATE;
}

bool DxgiPhaseLockedCaptureMode::Setup() {
    m_timer = CreateWaitableTimerEx(NULL, NULL,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION | CREATE_WAITABLE_TIMER_MANUAL_RESET,
        TIMER_ALL_ACCESS);
    if (NULL == m_timer) {
        LOGERR("CreateWaitableTimerEx failed (error: %d)", GetLastError());
        return false;
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    m_perfFreqQuad = freq.QuadPart;

    // Round to nearest tick to minimize accumulated rounding error over many frames.
    m_targetPeriodQpc = (LONGLONG)((double)m_perfFreqQuad / m_framerate + 0.5);
    m_marginQpc = (m_perfFreqQuad * kPreVblankMarginUs) / 1000000;
    m_observedPeriodQpc.store(m_targetPeriodQpc);

    LOG("DXGI phase-locked mode initialized - target framerate: %.2f fps (period: %lld ticks)",
        m_framerate, m_targetPeriodQpc);
    return true;
}

bool DxgiPhaseLockedCaptureMode::FindTargetOutput(HMONITOR targetMonitor) {
    IDXGIFactory1* factory = NULL;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    if (FAILED(hr) || !factory) {
        LOGERR("CreateDXGIFactory1 failed (0x%08x)", hr);
        return false;
    }

    bool found = false;
    for (UINT adapterIdx = 0; !found; adapterIdx++) {
        IDXGIAdapter1* adapter = NULL;
        if (factory->EnumAdapters1(adapterIdx, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        for (UINT outputIdx = 0; ; outputIdx++) {
            IDXGIOutput* output = NULL;
            if (adapter->EnumOutputs(outputIdx, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == targetMonitor) {
                m_targetOutput = output;  // retain (do not Release)
                found = true;
                break;
            }
            output->Release();
        }
        adapter->Release();
    }

    factory->Release();
    return found;
}

void DxgiPhaseLockedCaptureMode::VblankWatcherLoop() {
    LONGLONG prev = 0;
    LONGLONG period = m_targetPeriodQpc;
    while (!m_vblankThreadStop.load()) {
        HRESULT hr = m_targetOutput->WaitForVBlank();
        if (FAILED(hr)) {
            // Adapter/output lost or torn down; stop tracking. The main loop will
            // detect the stale timestamp and free-run.
            LOGERR("WaitForVBlank failed (0x%08x); vblank watcher exiting", hr);
            break;
        }
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        m_lastVblankQpc.store(qpc.QuadPart);

        if (prev != 0) {
            LONGLONG delta = qpc.QuadPart - prev;
            // Reject outliers (a delayed wake) so one bad sample can't poison the
            // smoothed estimate; only fold in plausible single-frame intervals.
            if (delta > m_targetPeriodQpc / 2 && delta < m_targetPeriodQpc * 2) {
                period = period + (delta - period) / 16;  // EMA, alpha = 1/16
                m_observedPeriodQpc.store(period);
            }
        }
        prev = qpc.QuadPart;
    }
}

void DxgiPhaseLockedCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    // The window was created on the target display, so its monitor IS the target.
    HMONITOR targetMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!FindTargetOutput(targetMonitor)) {
        LOGERR("Could not locate IDXGIOutput for target monitor; aborting mode");
        return;
    }

    m_vblankThreadStop.store(false);
    m_vblankThread = std::thread(&DxgiPhaseLockedCaptureMode::VblankWatcherLoop, this);

    // Prime: wait briefly for the first observed vblank so we have a phase reference.
    LONGLONG primedVblank = 0;
    for (int i = 0; i < 200; i++) {  // up to ~200 ms
        primedVblank = m_lastVblankQpc.load();
        if (primedVblank != 0) break;
        Sleep(1);
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (primedVblank != 0) {
        // Schedule the first present ~one period out, aligned just before a vblank.
        m_nextPresentQpc = primedVblank + m_targetPeriodQpc - m_marginQpc;
        while (m_nextPresentQpc <= now.QuadPart) {
            m_nextPresentQpc += m_targetPeriodQpc;
        }
        LOG("Phase reference primed from target vblank");
    } else {
        // No vblank observed (output may not signal); free-run like a plain timer.
        // Resync will re-anchor automatically if vblanks start arriving later.
        m_nextPresentQpc = now.QuadPart + m_targetPeriodQpc;
        LOGERR("No target vblank observed during prime; free-running without phase lock");
    }
    m_framesSinceResync = 0;

    MSG msg = {};
    while (TRUE)
    {
        // 1. Wait until the scheduled present time.
        QueryPerformanceCounter(&now);
        LONGLONG ticksUntilPresent = m_nextPresentQpc - now.QuadPart;
        if (ticksUntilPresent > 0) {
            LARGE_INTEGER due;
            due.QuadPart = -(ticksUntilPresent * 10000000 / m_perfFreqQuad);  // 100ns, relative
            SetWaitableTimer(m_timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(m_timer, INFINITE);
        }
        else if (ticksUntilPresent < -m_targetPeriodQpc) {
            // Fell more than a full frame behind (stall). Re-anchor to the latest
            // observed vblank instead of trying to catch up frame-by-frame, which
            // would free-run at max speed until the schedule caught up.
            LONGLONG ref = m_lastVblankQpc.load();
            if (ref > 0) {
                m_nextPresentQpc = ref + m_targetPeriodQpc - m_marginQpc;
            } else {
                m_nextPresentQpc = now.QuadPart + m_targetPeriodQpc;
            }
            m_framesSinceResync = 0;
        }

        // 2. Grab the freshest frame (NOWAIT flag set by caller).
        NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);
        if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("NvFBC session invalidated - session needs to be recreated");
            break;
        }

        // 3. Present immediately — lands on the upcoming target scan-out.
        device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);

        // 4. Advance the free-running schedule.
        m_nextPresentQpc += m_targetPeriodQpc;
        m_framesSinceResync++;

        // 5. Periodic resync: snap the schedule to the *nearest* target vblank so
        //    accumulated drift is corrected gradually (no visible step) and bounded.
        if (m_framesSinceResync >= kResyncIntervalFrames) {
            LONGLONG ref = m_lastVblankQpc.load();
            LONGLONG period = m_observedPeriodQpc.load();
            if (ref > 0 && period > 0) {
                LONGLONG delta = m_nextPresentQpc + m_marginQpc - ref;
                LONGLONG nVblanks = (delta + period / 2) / period;  // round to nearest
                if (nVblanks < 0) nVblanks = 0;
                m_nextPresentQpc = ref + nVblanks * period - m_marginQpc;
            }
            m_framesSinceResync = 0;
        }

        // 6. Pump messages.
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (msg.message == WM_QUIT) {
            break;
        }
    }

    // Teardown: stop the watcher thread and release the output.
    m_vblankThreadStop.store(true);
    if (m_vblankThread.joinable()) {
        m_vblankThread.join();  // wakes within ~1 vblank (~16 ms) and exits
    }
    if (m_targetOutput) {
        m_targetOutput->Release();
        m_targetOutput = NULL;
    }
}

const char* DxgiPhaseLockedCaptureMode::GetModeName() const {
    return "DXGI Phase-Locked";
}
