#pragma once

#include <windows.h>
#include <d3d9.h>
#include "VBlankWaiter.h"

class PresentScheduler;

// Result of BeginFrame: ok=false => fatal, caller breaks. gateHit indicates a lock mode actually
// caught the vblank (vs timed out) — diagnostic only. deadline is the QPC reference time the
// selection target is computed from (now, post-pace), so the target is anchored to the ACTUAL
// present moment — including the vblank wait for lock modes.
struct FrameStart { bool ok; bool gateHit; LONGLONG deadline; };

// Present-timing strategy: the ONLY thing that varies between the temporal present modes. The
// shared loop (selection / ring / hysteresis / instrumentation) lives in FrameTemporalCaptureMode
// and calls these hooks. Each strategy is a separate class, so changing one cannot affect another.
//
// Per-frame call order from the shared loop:
//   FrameStart fs = BeginFrame(dev);      // ALL pacing incl. the lock modes' vblank gate
//   ... select (target = fs.deadline - bracketingDelay) + StretchRect ...
//   ... PresentEx(..., PresentInterval()) // flips right after the pace point (in the blank)
//   EndFrame();                           // bookkeeping (e.g. advance the scheduler)
//
// BeginFrame is BOTH the pace and the gate: timer waits its deadline, vsync waits the floor, and
// the lock modes wait for the card vblank here — then return now. Doing the gate here (not after
// selection) keeps the selection target anchored to the present moment instead of ~one frame stale.
class IPresentTiming {
public:
    virtual ~IPresentTiming() {}
    virtual bool Setup(IDirect3DDevice9Ex* device, HWND hwnd, PresentScheduler* scheduler) = 0;
    virtual UINT PresentInterval() const = 0;
    virtual bool WantsExclusiveFullscreen() const = 0;
    virtual FrameStart BeginFrame(IDirect3DDevice9Ex* device) = 0;
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
    FrameStart BeginFrame(IDirect3DDevice9Ex*) override;
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
    FrameStart BeginFrame(IDirect3DDevice9Ex*) override;
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
    FrameStart BeginFrame(IDirect3DDevice9Ex* device) override;
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
    FrameStart BeginFrame(IDirect3DDevice9Ex*) override;
    void EndFrame() override {}
    const char* Name() const override { return "dlock (FS self-gate via WaitForVBlank)"; }
};
