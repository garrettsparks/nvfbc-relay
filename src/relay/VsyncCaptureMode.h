#pragma once

#include "IFrameCaptureMode.h"

// VSync-driven capture mode
class VsyncCaptureMode : public IFrameCaptureMode {
public:
    VsyncCaptureMode();
    virtual ~VsyncCaptureMode();

    virtual UINT GetPresentationInterval() const override;
    virtual MaybeFailure Setup(const RelayContext& ctx) override;
    virtual MaybeFailure Run(RelayContext& ctx,
                             NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams) override;
    virtual const char* GetModeName() const override;
};
