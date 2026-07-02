#pragma once

#include "IFrameCaptureMode.h"
#include "PresentScheduler.h"
#include "CaptureRing.h"

// Frame temporal capture mode — nearest-frame selection for smooth fixed-rate capture of a
// (possibly variable-rate) source.
//
// Composition of the shared pieces plus a trivial selection step:
//   CaptureRing      — capture thread fills a ring with source frames stamped at arrival.
//   Present timing   — two options (the <selection>:<present> framework's present axis):
//                      timer  (t:60)    — PresentScheduler's absolute-QPC deadline drives it.
//                      vsync  (t:vsync) — the INTERVAL_ONE present blocks on DWM's compose clock
//                                         (the PRIMARY/source display — NOT the capture card;
//                                         windowed flips always ride DWM, see spec Rounds 5-8).
//                                         Correct only when source rate == target rate. To be
//                                         replaced by the phase-locked timer (DWM-clock-steered
//                                         scheduled present with known target T).
//   This mode        — each present: select the ring frame nearest a content target lagged one
//                      period behind, with hysteresis (monotonic), copy to backbuffer, present.
//
// Blend mode will differ only in the last step (blend the bracketing pair instead of picking
// one); optical flow later replaces that step again.
class FrameTemporalCaptureMode : public IFrameCaptureMode {
private:
    PresentScheduler m_scheduler;
    CaptureRing m_ring;
    LONGLONG m_bracketingDelayQpc;  // present-target lag (≈ one present period)
    bool m_vsyncPresent;            // false: QPC-timer present (t:60); true: vblank present (t:vsync)
    LARGE_INTEGER m_baseQpc;        // logging time origin
    float m_targetFramerate;
    IDirect3DDevice9Ex* m_device;

public:
    FrameTemporalCaptureMode(float framerate, bool vsyncPresent = false);

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;
};
