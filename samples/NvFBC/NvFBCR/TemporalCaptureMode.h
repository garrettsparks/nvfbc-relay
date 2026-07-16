#pragma once

#include "IFrameCaptureMode.h"
#include "PresentScheduler.h"
#include "CaptureRing.h"
#include "FrameMarker.h"

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
//   This mode        — each present: select the ring frame nearest a content target lagged a
//                      fixed bracketing delay behind, with hysteresis (monotonic), copy to
//                      backbuffer, present.
//
// Blend mode will differ only in the last step (blend the bracketing pair instead of picking
// one); optical flow later replaces that step again.
class TemporalCaptureMode : public IFrameCaptureMode {
private:
    PresentScheduler m_scheduler;
    CaptureRing m_ring;
    LONGLONG m_bracketingDelayQpc;  // present-target lag; static: max(present period, 1.25 x assumed source period)
    LONGLONG m_assumedSrcPeriodQpc; // declared/default source period the lag was sized for
    LONGLONG m_stickinessQpc;       // selection Schmitt band (anti flip-flop at bracket midpoint)
    LONGLONG m_combQpc;             // phase-comb spacing (assumed srcP / M); 0 = lock disabled
    LONGLONG m_phasePullQpc;        // live comb-lock pull: extra lag holding the target on the comb
    LONGLONG m_phaseErrEmaQpc;      // EMA of the wrapped phase error (alpha 1/16)
    LONGLONG m_phaseDevEmaQpc;      // EMA of |err - errEma|: phase stability (lock gate input)
    LONGLONG m_phasePullSlewQpc;    // max pull change per present
    int m_telemetryCountdown;       // presents until the next estimator-vs-assumption audit
    bool m_phaseSeeded;             // errEma holds a sample (0 is a legal EMA value)
    bool m_lockEngaged;             // stability-gate state at the last pull update
    bool m_lock;                    // -lock: opt in to the comb lock (needs -src); default off
    bool m_mark;                    // -mark: burn the frame-counter marker (debug); default off
    bool m_lastPickAfter;           // Schmitt state: which bracket side the last pick took
    bool m_advGateOpen;             // Schmitt state: last advance-gate decision (see ADVANCE GATE)
    bool m_vsyncPresent;            // false: QPC-timer present (t:60); true: vblank present (t:vsync)
    LARGE_INTEGER m_baseQpc;        // logging time origin
    float m_targetFramerate;
    float m_srcRateHint;            // declared source fps (-src); 0 = unset, assume >= 60
    IDirect3DDevice9Ex* m_device;
    FrameMarker m_marker;           // per-present provenance burn-in (inert unless -mark)

    // The one lag-sizing rule: 1.25x the source period for bracketing headroom, floored at
    // the present period. Setup sizes the operative lag with it; telemetry sizes suggestions.
    LONGLONG LagForSourcePeriod(LONGLONG srcPeriodQpc) const;

    // One comb-lock step, closed-loop on the live bracket: the pull is already inside the
    // target the bracket was found at, so bracket.beforeDiff is the loop error. Updates the
    // pull for the NEXT present (EMA filter, stability gate, symmetric bounded slew, wrap
    // modulo the comb behind a hysteresis band).
    void UpdatePhaseLock(LONGLONG beforeDiffQpc);

public:
    TemporalCaptureMode(float framerate, bool vsyncPresent = false, float srcRateHint = 0.0f,
                        bool lock = false, bool mark = false);

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;
};
