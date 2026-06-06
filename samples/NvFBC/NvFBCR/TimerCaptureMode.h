#pragma once

#include "IFrameCaptureMode.h"

// Timer-driven capture mode.
//
// Presents at a fixed framerate using a high-resolution waitable timer. Each present is
// scheduled against an absolute QueryPerformanceCounter deadline on a fixed timeline
// rather than incrementing a timer relative to the current time every iteration.
// The absolute schedule keeps the average rate pinned to the target period,
// so that jitter does not accumulate.
class TimerCaptureMode : public IFrameCaptureMode {
private:
    HANDLE m_timer;
    LARGE_INTEGER m_freq;           // QPC frequency (ticks/sec)
    LONGLONG m_periodQpc;           // target frame interval in QPC ticks
    LARGE_INTEGER m_nextPresent;    // absolute QPC deadline for the next present
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
