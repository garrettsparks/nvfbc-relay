#pragma once

#include "IFrameCaptureMode.h"
#include "PresentScheduler.h"
#include "CaptureRing.h"

// Frame temporal capture mode — nearest-frame selection for smooth fixed-rate capture of a
// (possibly variable-rate) source.
//
// Single-thread composition (branch A): one loop alternates between pumping the CaptureRing
// (blocking grabs with a dynamic timeout, diffmap-deduplicated) and presenting on the
// PresentScheduler's absolute-QPC deadlines. Capture and present never contend for the D3D9
// device because they share the thread.
//
//   PresentScheduler — when to present (absolute-QPC deadlines).
//   CaptureRing      — pump-driven ring of content-distinct source frames stamped at arrival.
//   This mode        — each deadline: select the ring frame nearest a content target lagged
//                      one period behind the deadline, copy to backbuffer, present, log.
//
// Blend mode will differ only in the last step (blend the bracketing pair instead of picking
// one); optical flow later replaces that step again.
class FrameTemporalCaptureMode : public IFrameCaptureMode {
private:
    PresentScheduler m_scheduler;
    CaptureRing m_ring;
    LONGLONG m_bracketingDelayQpc;  // present-target lag (≈ one present period)
    LONGLONG m_marginQpc;           // stop pumping this long before each deadline
    LARGE_INTEGER m_baseQpc;        // logging time origin
    float m_targetFramerate;
    IDirect3DDevice9Ex* m_device;

public:
    FrameTemporalCaptureMode(float framerate);

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;
};
