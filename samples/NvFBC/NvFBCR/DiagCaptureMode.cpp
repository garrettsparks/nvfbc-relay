#include "DiagCaptureMode.h"
#include <SimpleLogger.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

// External globals (NvFBCR.cpp)
extern IDirect3D9Ex* g_pD3DEx;
extern int g_targetAdapterIndex;

DiagCaptureMode::DiagCaptureMode(bool vsyncPresent)
    : m_vsyncPresent(vsyncPresent)
    , m_rasterDevice(NULL)
{
}

DiagCaptureMode::~DiagCaptureMode() {
    if (m_rasterDevice) {
        m_rasterDevice->Release();
        m_rasterDevice = NULL;
    }
}

UINT DiagCaptureMode::GetPresentationInterval() const {
    // diag:vsync — INTERVAL_ONE so present block time measures DWM's delivery cadence.
    // diag       — IMMEDIATE; the QPC scheduler paces (steady probe).
    return m_vsyncPresent ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
}

bool DiagCaptureMode::Setup() {
    if (!m_scheduler.Setup(60.0f)) {
        return false;
    }
    LOG("Diag mode initialized - %s probe, 60Hz",
        m_vsyncPresent ? "INTERVAL_ONE (DWM delivery cadence)" : "QPC-timer/IMMEDIATE (steady)");
    return true;
}

void DiagCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    LARGE_INTEGER baseQpc;
    QueryPerformanceCounter(&baseQpc);
    const double usPerTick = 1000000.0 / (double)m_scheduler.Freq();

    // Private device on the TARGET adapter, used ONLY for GetRasterStatus reads of the capture
    // card's raster (the main present device sits on the source adapter). Same pattern as
    // CaptureRing's private device: separate device, same window.
    {
        D3DPRESENT_PARAMETERS d3dpp = {};
        d3dpp.Windowed = TRUE;
        d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
        d3dpp.BackBufferWidth = 4;
        d3dpp.BackBufferHeight = 4;
        d3dpp.BackBufferCount = 1;
        d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        d3dpp.hDeviceWindow = hwnd;
        HRESULT hr = g_pD3DEx->CreateDeviceEx(
            g_targetAdapterIndex, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING,
            &d3dpp, NULL, &m_rasterDevice);
        if (FAILED(hr)) {
            // Not fatal — DWM/present probes still run; raster columns log hr only.
            LOGERR("diag: raster device on target adapter %d failed (0x%08x)", g_targetAdapterIndex, hr);
            m_rasterDevice = NULL;
        } else {
            LOG("diag: raster device on target adapter %d (windowed GetRasterStatus probe)", g_targetAdapterIndex);
        }
    }

    MSG msg = {};
    LONGLONG lastPresentQpc = 0;
    m_scheduler.Seed();

    while (TRUE)
    {
        if (!m_vsyncPresent) {
            m_scheduler.WaitUntilDeadline();
        }

        // Keep an image flowing (NOWAIT: returns latest immediately, no timing impact).
        NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);
        if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("NvFBC session invalidated - stopping");
            break;
        }

        // Present. INTERVAL_ONE variant: the block time IS the measurement (DWM delivery).
        LARGE_INTEGER beforePresent, afterPresent;
        QueryPerformanceCounter(&beforePresent);
        HRESULT presentHr = device->PresentEx(NULL, NULL, NULL, NULL,
            m_vsyncPresent ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE);
        QueryPerformanceCounter(&afterPresent);

        // Candidate A probe: DWM compose clock (global; primary cadence per docs).
        DWM_TIMING_INFO ti = {};
        ti.cbSize = sizeof(ti);
        HRESULT dwmHr = DwmGetCompositionTimingInfo(NULL, &ti);

        // Candidate B probe: capture-card raster via the target-adapter device.
        D3DRASTER_STATUS rs = {};
        HRESULT rasterHr = m_rasterDevice ? m_rasterDevice->GetRasterStatus(0, &rs) : E_FAIL;

        LONGLONG pdt = (lastPresentQpc != 0) ? (beforePresent.QuadPart - lastPresentQpc) : 0;
        lastPresentQpc = beforePresent.QuadPart;

        // One line per tick (~60/s). dwm_vbl = last DWM vblank QPC (rel base, µs); dwm_per =
        // DWM's reported refresh period (µs); dwm_cref = refresh counter (delta shows compose
        // rate); rate = rateRefresh num/den. scan/inb from the CARD's raster.
        LOG("diagclk pdt=%lldus pblk=%lldus phr=0x%08x dwm_hr=0x%08x dwm_vbl=%lldus dwm_per=%.1fus dwm_cref=%llu dwm_rate=%u/%u raster_hr=0x%08x inb=%d scan=%u",
            (long long)(pdt * usPerTick),
            (long long)((afterPresent.QuadPart - beforePresent.QuadPart) * usPerTick),
            presentHr, dwmHr,
            (long long)(((LONGLONG)ti.qpcVBlank - baseQpc.QuadPart) * usPerTick),
            (double)ti.qpcRefreshPeriod * usPerTick,
            (unsigned long long)ti.cRefresh,
            ti.rateRefresh.uiNumerator, ti.rateRefresh.uiDenominator,
            rasterHr, (int)rs.InVBlank, rs.ScanLine);

        if (!m_vsyncPresent) {
            m_scheduler.Advance();
        }

        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (msg.message == WM_QUIT) break;
    }
}

const char* DiagCaptureMode::GetModeName() const {
    return "Diag";
}
