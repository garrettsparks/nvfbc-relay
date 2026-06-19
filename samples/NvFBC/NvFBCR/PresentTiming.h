#pragma once

#include <windows.h>
#include <d3d9.h>
#include "VBlankWaiter.h"

class PresentScheduler;

// Outcome of the pre-present gate. ok=false => fatal, caller breaks the loop. gateHit indicates a
// lock mode actually caught the vblank (vs timed out) — diagnostic only.
struct GateResult { bool ok; bool gateHit; };

// Present-timing strategy: the ONLY thing that varies between the temporal present modes. The
// shared loop (selection / ring / hysteresis / instrumentation) lives in FrameTemporalCaptureMode
// and calls these hooks. Each strategy is a separate class, so changing one cannot affect another.
//
// Per-frame call order from the shared loop:
//   deadline = BeginFrame();              // pace the frame start; returns the selection target
//   ... select + StretchRect to backbuffer ...
//   GateResult g = GateBeforePresent(dev);// lock modes wait for the card vblank here
//   ... PresentEx(..., PresentInterval()) ...
//   EndFrame();                           // bookkeeping (e.g. advance the scheduler)
class IPresentTiming {
public:
    virtual ~IPresentTiming() {}
    virtual bool Setup(IDirect3DDevice9Ex* device, HWND hwnd, PresentScheduler* scheduler) = 0;
    virtual UINT PresentInterval() const = 0;
    virtual bool WantsExclusiveFullscreen() const = 0;
    virtual LONGLONG BeginFrame() = 0;
    virtual GateResult GateBeforePresent(IDirect3DDevice9Ex* device) = 0;
    virtual void EndFrame() = 0;
    virtual const char* Name() const = 0;
};

// t:60 — absolute-QPC timer paces the frame; non-blocking present. Windowed.
class TimerPresentTiming : public IPresentTiming {
    PresentScheduler* m_sched = nullptr;
public:
    bool Setup(IDirect3DDevice9Ex*, HWND, PresentScheduler* s) override { m_sched = s; return true; }
    UINT PresentInterval() const override { return D3DPRESENT_INTERVAL_IMMEDIATE; }
    bool WantsExclusiveFullscreen() const override { return false; }
    LONGLONG BeginFrame() override;
    GateResult GateBeforePresent(IDirect3DDevice9Ex*) override { return { true, true }; }
    void EndFrame() override;
    const char* Name() const override { return "timer (QPC, IMMEDIATE)"; }
};

// t:vsync — INTERVAL_ONE present (rides DWM); the 60Hz floor also paces. Windowed.
class VsyncPresentTiming : public IPresentTiming {
    PresentScheduler* m_sched = nullptr;
public:
    bool Setup(IDirect3DDevice9Ex*, HWND, PresentScheduler* s) override { m_sched = s; return true; }
    UINT PresentInterval() const override { return D3DPRESENT_INTERVAL_ONE; }
    bool WantsExclusiveFullscreen() const override { return false; }
    LONGLONG BeginFrame() override;
    GateResult GateBeforePresent(IDirect3DDevice9Ex*) override { return { true, true }; }
    void EndFrame() override;
    const char* Name() const override { return "vsync (INTERVAL_ONE, rides DWM)"; }
};

// t:rlock — exclusive FS; self-gate the flip to the card vblank by busy-polling GetRasterStatus.
class RasterLockPresentTiming : public IPresentTiming {
    PresentScheduler* m_sched = nullptr;
    LONGLONG m_gateTimeoutQpc = 0;
public:
    bool Setup(IDirect3DDevice9Ex*, HWND, PresentScheduler* s) override;
    UINT PresentInterval() const override { return D3DPRESENT_INTERVAL_IMMEDIATE; }
    bool WantsExclusiveFullscreen() const override { return true; }
    LONGLONG BeginFrame() override;
    GateResult GateBeforePresent(IDirect3DDevice9Ex* device) override;
    void EndFrame() override {}
    const char* Name() const override { return "rlock (FS self-gate via GetRasterStatus)"; }
};

// t:dlock — exclusive FS; self-gate the flip to the card vblank via IDXGIOutput::WaitForVBlank.
class DxgiLockPresentTiming : public IPresentTiming {
    VBlankWaiter m_vblank;
public:
    bool Setup(IDirect3DDevice9Ex*, HWND hwnd, PresentScheduler*) override;
    UINT PresentInterval() const override { return D3DPRESENT_INTERVAL_IMMEDIATE; }
    bool WantsExclusiveFullscreen() const override { return true; }
    LONGLONG BeginFrame() override;
    GateResult GateBeforePresent(IDirect3DDevice9Ex*) override;
    void EndFrame() override {}
    const char* Name() const override { return "dlock (FS self-gate via WaitForVBlank)"; }
};
