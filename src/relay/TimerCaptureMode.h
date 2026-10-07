#pragma once

#include "IFrameCaptureMode.h"
#include "PresentScheduler.h"

// Timer-driven capture mode.
//
// Presents at a fixed framerate: one capture + one immediate present per scheduled deadline,
// paced by PresentScheduler (an absolute-QPC schedule on a high-resolution waitable timer, so
// per-frame wake latency cannot accumulate into drift).
//
// ogTimer and ogFlags put back two of the original relay's behaviours for cost comparisons:
// its timer (a default-resolution waitable timer, re-armed relative to now before each grab, so
// every loop runs the period plus the timer's lateness) and its PresentEx flags (0x80000000).
class TimerCaptureMode : public IFrameCaptureMode {
private:
    PresentScheduler m_scheduler;
    float m_framerate;
    bool m_ogTimer;
    bool m_ogFlags;
    HANDLE m_ogTimerHandle = NULL;

public:
    TimerCaptureMode(float framerate, bool ogTimer, bool ogFlags);
    ~TimerCaptureMode();

    TimerCaptureMode(const TimerCaptureMode&) = delete;
    TimerCaptureMode& operator=(const TimerCaptureMode&) = delete;

    virtual UINT GetPresentationInterval() const override;
    virtual MaybeFailure Setup(const RelayContext& ctx) override;
    virtual MaybeFailure Run(RelayContext& ctx,
                             NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams) override;
    virtual const char* GetModeName() const override;
};
