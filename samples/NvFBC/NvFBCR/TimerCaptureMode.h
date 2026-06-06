#pragma once

#include "IFrameCaptureMode.h"

// Timer-driven capture mode
class TimerCaptureMode : public IFrameCaptureMode {
private:
    HANDLE m_timer;
    // EXPERIMENT: absolute QPC scheduling instead of a per-iteration relative timer.
    // Tests whether the drift in this mode came from relative-timer wake-latency
    // accumulation (not display-clock drift). Revert this commit to restore the
    // original relative-interval behavior.
    LARGE_INTEGER m_freq;          // QPC frequency (ticks/sec)
    LONGLONG m_periodQpc;          // target frame interval in QPC ticks
    LARGE_INTEGER m_nextPresent;   // absolute QPC deadline for the next present
    float m_framerate;

public:
    TimerCaptureMode(float framerate);
    virtual ~TimerCaptureMode();

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;
};