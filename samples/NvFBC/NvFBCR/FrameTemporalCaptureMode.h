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
//                      vsync  (t:vsync) — the INTERVAL_ONE present blocks on the capture-card
//                                         vblank; the present device is created on the TARGET
//                                         adapter (PresentsOnTargetAdapter) so the vblank is the
//                                         card's, not the source's. Selection anchors to "now".
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
    // Present on the TARGET (capture-card) adapter so INTERVAL_ONE vblank-syncs to the card, not
    // the source. Safe only because this mode's NvFBC lives on the ring's own capture device
    // (pinned to the source adapter) — the present device is free to live on the target.
    virtual bool PresentsOnTargetAdapter() const override { return true; }
    // Vsync present: try exclusive fullscreen on the target so INTERVAL_ONE can lock to the
    // capture card's vblank (windowed DWM otherwise paces at the primary's rate). Timer present
    // (t:60) doesn't need it. Fails fast if FS is unavailable (no windowed fallback).
    virtual bool WantsExclusiveFullscreen() const override { return m_vsyncPresent; }
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;
};
