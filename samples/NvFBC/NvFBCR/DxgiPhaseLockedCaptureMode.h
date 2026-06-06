#pragma once

#include "IFrameCaptureMode.h"
#include <dxgi1_2.h>
#include <atomic>
#include <thread>

// Phase-locked timer capture mode (DXGI vblank resync).
//
// Drives presents from a free-running high-resolution CPU timer, but periodically
// re-snaps the schedule to the *target display's* actual vblank so cumulative drift
// between the CPU clock and the display PLL can never accumulate to a full frame.
//
// The phase reference is IDXGIOutput::WaitForVBlank on the target monitor's output,
// observed on a dedicated watcher thread. Unlike DwmGetCompositionTimingInfo (which is
// primary-display-only on Win8.1+), this references the specific target display, so it
// is correct whether the capture card is primary or secondary and regardless of refresh.
//
// See docs/phase-locked-timer-dxgi-spec.md and docs/frame-pacing-drift-analysis.md.
class DxgiPhaseLockedCaptureMode : public IFrameCaptureMode {
private:
    HANDLE m_timer;
    LONGLONG m_perfFreqQuad;          // QPC ticks per second
    LONGLONG m_targetPeriodQpc;       // nominal frame interval in QPC ticks
    LONGLONG m_marginQpc;             // present this many ticks before the vblank target
    LONGLONG m_nextPresentQpc;        // absolute QPC of next scheduled present
    float m_framerate;
    int m_framesSinceResync;

    static constexpr int kResyncIntervalFrames = 60;   // resync ~once per second at 60 fps
    static constexpr LONGLONG kPreVblankMarginUs = 1000;

    // DXGI vblank-watcher thread
    IDXGIOutput* m_targetOutput;
    std::thread m_vblankThread;
    std::atomic<LONGLONG> m_lastVblankQpc;     // QPC of most recently observed vblank
    std::atomic<LONGLONG> m_observedPeriodQpc; // exponentially-smoothed vblank-to-vblank period
    std::atomic<bool> m_vblankThreadStop;

    void VblankWatcherLoop();
    bool FindTargetOutput(HMONITOR targetMonitor);

public:
    DxgiPhaseLockedCaptureMode(float framerate);
    virtual ~DxgiPhaseLockedCaptureMode();

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;
};
