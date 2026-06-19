#pragma once

#include <memory>
#include "IFrameCaptureMode.h"
#include "PresentScheduler.h"
#include "CaptureRing.h"
#include "PresentTiming.h"

// Frame temporal capture mode — nearest-frame selection for smooth fixed-rate capture of a
// (possibly variable-rate) source.
//
//   CaptureRing      — capture thread fills a ring with source frames stamped at arrival.
//   Selection        — each present: pick the ring frame nearest a target lagged one period behind,
//                      with hysteresis (monotonic). SHARED across all present modes (this class).
//   IPresentTiming   — the ONLY part that varies: when/how to flip (timer / vsync / rlock / dlock).
//                      Injected as a strategy so each mode is isolated in its own class.
//
// Blend mode will differ only in the selection step (blend the bracketing pair); the timing
// strategies are reusable as-is.
class FrameTemporalCaptureMode : public IFrameCaptureMode {
private:
    PresentScheduler m_scheduler;
    CaptureRing m_ring;
    std::unique_ptr<IPresentTiming> m_timing;   // owned present-timing strategy
    LONGLONG m_bracketingDelayQpc;              // present-target lag (≈ one present period)
    LARGE_INTEGER m_baseQpc;                     // logging time origin
    float m_targetFramerate;
    IDirect3DDevice9Ex* m_device;

public:
    // Takes ownership of `timing`.
    FrameTemporalCaptureMode(float framerate, IPresentTiming* timing);

    virtual UINT GetPresentationInterval() const override { return m_timing->PresentInterval(); }
    // Present on the TARGET (capture-card) adapter; NvFBC lives on the ring's own source-adapter
    // capture device, so the present device is free to live on the target.
    virtual bool PresentsOnTargetAdapter() const override { return true; }
    virtual bool WantsExclusiveFullscreen() const override { return m_timing->WantsExclusiveFullscreen(); }
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;
};
