#pragma once

#include "IFrameCaptureMode.h"

// VSync + Temporal capture mode - frame selection with vsync-driven timing
class VsyncTemporalCaptureMode : public IFrameCaptureMode {
private:
    static const int FRAME_HISTORY_SIZE = 2;

    struct FrameHistoryEntry {
        IDirect3DSurface9* surface;
        LARGE_INTEGER timestamp;
        bool valid;
    };

    FrameHistoryEntry m_frameHistory[FRAME_HISTORY_SIZE];
    int m_currentHistoryIndex;
    IDirect3DSurface9* m_captureTarget;
    LARGE_INTEGER m_perfFreq;
    float m_targetFramerate;
    IDirect3DDevice9Ex* m_device;

public:
    VsyncTemporalCaptureMode();
    virtual ~VsyncTemporalCaptureMode();

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;

private:
    void SelectFrameToBackbuffer(LARGE_INTEGER targetTime, IDirect3DSurface9* backbuffer);
};