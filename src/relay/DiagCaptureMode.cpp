#include "DiagCaptureMode.h"
#include "D3D9Setup.h"
#include "OutputWindow.h"
#include <SimpleLogger.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

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

MaybeFailure DiagCaptureMode::Setup(const RelayContext& /*ctx*/) {
    if (!m_scheduler.Setup(60.0f)) {
        return ModeCouldNotStart(GetModeName());
    }
    LOG("Diag mode initialized - %s probe, 60Hz",
        m_vsyncPresent ? "INTERVAL_ONE (DWM delivery cadence)" : "QPC-timer/IMMEDIATE (steady)");
    return std::nullopt;
}

MaybeFailure DiagCaptureMode::Run(RelayContext& ctx, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams) {
    LARGE_INTEGER baseQpc;
    QueryPerformanceCounter(&baseQpc);
    const double usPerTick = 1000000.0 / (double)m_scheduler.Freq();
    IDirect3DDevice9Ex* device = ctx.presentDevice;

    // Private device on the TARGET adapter, used ONLY for GetRasterStatus reads of the capture
    // card's raster (the main present device sits on the source adapter). Same pattern as
    // CaptureRing's private device: separate device, same window.
    {
        D3DPRESENT_PARAMETERS params = WindowedPresentParams(
            ctx.deviceWindow, 4, 4, D3DFMT_UNKNOWN, 1, D3DSWAPEFFECT_DISCARD,
            D3DPRESENT_INTERVAL_IMMEDIATE);
        HRESULT hr = ctx.d3d->CreateDeviceEx(
            ctx.targetAdapter, D3DDEVTYPE_HAL, ctx.deviceWindow,
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
            &params, NULL, &m_rasterDevice);
        if (FAILED(hr)) {
            // Not fatal — DWM/present probes still run; raster columns log hr only.
            LOGERR("diag: raster device on target adapter %u failed (0x%08x)", ctx.targetAdapter, hr);
            m_rasterDevice = NULL;
        } else {
            LOG("diag: raster device on target adapter %u (windowed GetRasterStatus probe)", ctx.targetAdapter);
        }
    }

    LONGLONG lastPresentQpc = 0;
    m_scheduler.Seed();

    for (;;) {
        if (!m_vsyncPresent) {
            m_scheduler.WaitUntilDeadline();
        }

        // Keep an image flowing (NOWAIT: returns latest immediately, no timing impact).
        NVFBCRESULT fbcRes = ctx.session->NvFBCToDx9VidGrabFrame(grabParams);
        if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("NvFBC session invalidated - stopping");
            return CaptureLost("diag mode");
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
        if (!PumpMessages()) return std::nullopt;
    }
}

const char* DiagCaptureMode::GetModeName() const {
    return "Diag";
}
