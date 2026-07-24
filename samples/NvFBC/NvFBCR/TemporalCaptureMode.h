#pragma once

#include "IFrameCaptureMode.h"
#include "IFrameCompositor.h"
#include "PresentScheduler.h"
#include "CaptureRing.h"
#include "FrameMarker.h"
#include "TemporalPolicy.h"

// Temporal capture mode — nearest-frame selection for smooth fixed-rate capture of a
// (possibly variable-rate) source.
//
// Composition of the shared pieces plus a trivial selection step:
//   CaptureRing      — capture thread fills a ring with source frames stamped at arrival.
//   Present timing   — two options (the <selection>:<present> framework's present axis):
//                      timer  (t:60)    — PresentScheduler's absolute-QPC deadline drives it.
//                      vsync  (t:vsync) — the INTERVAL_ONE present blocks on DWM's compose clock
//                                         (windowed flips always ride DWM). That clock is
//                                         regime-dependent: on a composed desktop it is the
//                                         PRIMARY/source display ("wrong display", known and
//                                         accepted); under a fullscreen game on the source, DWM
//                                         composes only the card's display and the present
//                                         becomes card-locked 60 Hz — the production use case
//                                         (spec Rounds 5-10).
//   This mode        — each present: aim a content target lagged a fixed bracketing delay
//                      behind, bracket it in the ring, hand the bracket to the compositor
//                      (nearest copies one real frame; blend lerps the pair; interp
//                      motion-compensates the pair), present.

// Which compositor the mode letter selected (t nearest, b blend, o interp).
enum CompositorKind {
    kCompositorNearest = 0,
    kCompositorBlend = 1,
    kCompositorInterp = 2,
};

class TemporalCaptureMode : public IFrameCaptureMode {
private:
    PresentScheduler m_scheduler;
    CaptureRing m_ring;
    LONGLONG m_bracketingDelayQpc;  // present-target lag; static: max(present period, 1.25 x assumed source period)
    LONGLONG m_assumedSrcPeriodQpc; // declared/default source period the lag was sized for
    policy::PolicyConfig m_policyCfg;    // stickiness band, comb spacing (0 = lock off), pull slew, passthrough gate
    policy::PhaseLockState m_lockState;  // comb-lock pull/EMAs/gate (pure policy state)
    int m_lockStallRun = 0;              // consecutive incomplete-bracket presents (source stall); a run triggers a pull re-seed on resume
    IFrameCompositor* m_compositor; // owned; picked at Setup from m_compositorKind
    int m_telemetryCountdown;       // presents until the next estimator-vs-assumption audit
    CompositorKind m_compositorKind;
    bool m_lock;                    // -lock: opt in to the comb lock (needs -src); default off
    bool m_mark;                    // -mark: burn the frame-counter marker (debug); default off
    unsigned int m_markFrames;      // -mark N: burn only the first N presents; 0 = all (unset)
    bool m_vsyncPresent;            // false: QPC-timer present (t:60); true: vblank present (t:vsync)
    LARGE_INTEGER m_baseQpc;        // logging time origin
    float m_targetFramerate;
    float m_srcRateHint;            // declared source fps (-src); 0 = unset, assume >= 60
    IDirect3DDevice9Ex* m_device;
    FrameMarker m_marker;           // per-present provenance burn-in (inert unless -mark)

    // The one lag-sizing rule: 1.25x the source period for bracketing headroom, floored at
    // the present period. Setup sizes the operative lag with it; telemetry sizes suggestions.
    LONGLONG LagForSourcePeriod(LONGLONG srcPeriodQpc) const;

public:
    TemporalCaptureMode(float framerate, bool vsyncPresent = false, float srcRateHint = 0.0f,
                        bool lock = false, CompositorKind compositor = kCompositorNearest,
                        bool mark = false, unsigned int markFrames = 0);
    virtual ~TemporalCaptureMode();

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;
};
