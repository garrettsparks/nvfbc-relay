#pragma once

#include "IFrameCaptureMode.h"

// VSync-driven capture mode
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
};