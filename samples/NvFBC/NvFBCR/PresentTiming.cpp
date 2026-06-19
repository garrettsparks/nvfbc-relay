#include "PresentTiming.h"
#include "PresentScheduler.h"
#include <SimpleLogger.h>

// Busy-poll GetRasterStatus to land on the target's next vblank EDGE: first wait until we're out
// of any current vblank (InVBlank false), then until it goes true again. Bounded by timeoutQpc so a
// broken/absent raster can't hang the loop — on timeout returns false and the caller presents
// anyway. Busy-waits (yielding) up to ~one frame; this is rlock's CPU cost. No logging in here —
// it runs many times per frame.
static bool WaitRasterVBlank(IDirect3DDevice9Ex* device, LONGLONG timeoutQpc) {
    LARGE_INTEGER start; QueryPerformanceCounter(&start);
    D3DRASTER_STATUS rs;
    bool sawActive = false;
    while (true) {
        if (FAILED(device->GetRasterStatus(0, &rs))) return false;   // no real raster
        if (!sawActive) {
            if (!rs.InVBlank) sawActive = true;                      // left the current blank
        } else if (rs.InVBlank) {
            return true;                                             // rising edge into next blank
        }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (now.QuadPart - start.QuadPart > timeoutQpc) return false;
        SwitchToThread();                                            // yield without a full sleep
    }
}

static LONGLONG NowQpc() {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return now.QuadPart;
}

// ---- TimerPresentTiming ----
FrameStart TimerPresentTiming::BeginFrame(IDirect3DDevice9Ex*) {
    m_sched->WaitUntilDeadline();
    return { true, true, m_sched->Deadline() };
}
void TimerPresentTiming::EndFrame() {
    m_sched->Advance();
}

// ---- VsyncPresentTiming ----
FrameStart VsyncPresentTiming::BeginFrame(IDirect3DDevice9Ex*) {
    m_sched->WaitUntilDeadline();          // 60Hz floor; INTERVAL_ONE present blocks on top
    return { true, true, NowQpc() };       // anchor selection target to "now"
}
void VsyncPresentTiming::EndFrame() {
    m_sched->Advance();
}

// ---- RasterLockPresentTiming ----
bool RasterLockPresentTiming::Setup(IDirect3DDevice9Ex*, HWND, PresentScheduler* s) {
    m_sched = s;
    // Backstop: if the vblank never arrives, present anyway after ~1.3 periods (no hang/spin).
    m_gateTimeoutQpc = (s->PeriodQpc() * 13) / 10;
    return true;
}
FrameStart RasterLockPresentTiming::BeginFrame(IDirect3DDevice9Ex* device) {
    // The vblank gate IS the pace. Returning now() AFTER the wait anchors the selection target to
    // the actual present moment (the flip lands ~one StretchRect into the blanking interval).
    bool hit = WaitRasterVBlank(device, m_gateTimeoutQpc);
    return { true, hit, NowQpc() };        // timeout is not fatal — we present anyway
}

// ---- DxgiLockPresentTiming ----
bool DxgiLockPresentTiming::Setup(IDirect3DDevice9Ex*, HWND hwnd, PresentScheduler*) {
    // The window is pseudo-fullscreen on the target, so MonitorFromWindow gives the target HMONITOR.
    if (!m_vblank.Setup(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST))) {
        LOGERR("dlock: could not bind WaitForVBlank to target display");
        return false;
    }
    return true;
}
FrameStart DxgiLockPresentTiming::BeginFrame(IDirect3DDevice9Ex*) {
    bool ok = m_vblank.Wait();             // false => output lost (fatal)
    return { ok, ok, NowQpc() };
}
