#pragma once

#include "IFrameCaptureMode.h"

// Timer-driven capture mode
class TimerCaptureMode : public IFrameCaptureMode {
private:
    HANDLE m_timer;
    LARGE_INTEGER m_interval;
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