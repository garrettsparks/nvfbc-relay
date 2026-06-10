#pragma once

#include "IFrameCaptureMode.h"
#include "PresentScheduler.h"
#include <atomic>
#include <thread>

// Frame temporal capture mode — nearest-frame selection for smooth fixed-rate capture of a
// (possibly variable-rate) source.
//
// Two threads, strictly one-way (capture -> ring -> present):
//   Capture thread: a *blocking* NvFBC grab returns once per real source frame, so we
//     StretchRect it into a ring slot QPC-stamped at arrival (≈ the frame's true source time),
//     then publish via an atomic index. No backward signaling to this thread, ever.
//   Present thread (main, owns HWND + message pump): PresentScheduler's absolute-QPC schedule.
//     Each deadline, it targets a content time lagged by one period, finds the ring frames
//     bracketing it, and presents the nearer one.
//
// The only shared state is the atomic published index. Frames are never purged — the fixed
// ring overwrites the oldest slot naturally as capture advances. Requires
// D3DCREATE_MULTITHREADED on the device (set in InitD3D9), since both threads call D3D9.
class FrameTemporalCaptureMode : public IFrameCaptureMode {
private:
    static const int RING_SIZE = 8;  // start large to validate; shrink later from the logs

    struct FrameHistoryEntry {
        IDirect3DSurface9* surface;
        LARGE_INTEGER timestamp;   // QPC at capture ≈ source frame arrival time
        bool valid;
    };

    FrameHistoryEntry m_ring[RING_SIZE];
    IDirect3DSurface9* m_captureTarget;   // NvFBC writes here; capture thread copies into ring
    IDirect3DDevice9Ex* m_device;
    float m_targetFramerate;

    PresentScheduler m_scheduler;
    LONGLONG m_bracketingDelayQpc;        // present-target lag (≈ one present period)
    LARGE_INTEGER m_baseQpc;              // logging time origin

    // One-way capture -> present shared state.
    std::thread m_captureThread;
    std::atomic<long long> m_published;   // count of fully-written frames (monotonic)
    std::atomic<bool> m_stop;
    long long m_writeCount;               // capture-thread-local write counter

    void CaptureLoop(NvFBCToDx9Vid* nvfbcDx9, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams);

public:
    FrameTemporalCaptureMode(float framerate);
    virtual ~FrameTemporalCaptureMode();

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;
};
