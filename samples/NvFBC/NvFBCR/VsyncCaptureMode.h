#pragma once

#include "IFrameCaptureMode.h"
#include "VBlankWaiter.h"

// VSync-driven capture mode. Paces on the TARGET capture-card vblank via VBlankWaiter (by
// HMONITOR) rather than D3D9 INTERVAL_ONE — the latter syncs to the present device's adapter,
// which is the SOURCE display, so it paced to the source refresh, not the target's.
class VsyncCaptureMode : public IFrameCaptureMode {
public:
    VsyncCaptureMode();
    virtual ~VsyncCaptureMode();

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;

private:
    VBlankWaiter m_vblank;   // blocks on the TARGET display's vblank
};