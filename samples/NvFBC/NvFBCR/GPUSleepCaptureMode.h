#pragma once

#include "IFrameCaptureMode.h"

// GPU-based CPU sleep capture mode - uses NvFBCToDx9VidGPUBasedCPUSleep
// for sub-quantum microsecond-precision frame pacing
class GPUSleepCaptureMode : public IFrameCaptureMode {
private:
    __int64 m_targetIntervalUs;       // target frame interval in microseconds
    float m_framerate;
    LARGE_INTEGER m_perfFreq;         // QPC frequency for elapsed time measurement

public:
    GPUSleepCaptureMode(float framerate);
    virtual ~GPUSleepCaptureMode();

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;
};
