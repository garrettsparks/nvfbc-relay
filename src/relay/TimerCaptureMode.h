#pragma once

#include "IFrameCaptureMode.h"
#include "PresentScheduler.h"

// Timer-driven capture mode.
//
// Presents at a fixed framerate: one capture + one immediate present per scheduled deadline,
// paced by PresentScheduler (an absolute-QPC schedule on a high-resolution waitable timer, so
// per-frame wake latency cannot accumulate into drift).
class TimerCaptureMode : public IFrameCaptureMode {
private:
    PresentScheduler m_scheduler;
    float m_framerate;

public:
    TimerCaptureMode(float framerate);

    virtual UINT GetPresentationInterval() const override;
    virtual MaybeFailure Setup(const RelayContext& ctx) override;
    virtual MaybeFailure Run(RelayContext& ctx,
                             NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams) override;
    virtual const char* GetModeName() const override;
};
