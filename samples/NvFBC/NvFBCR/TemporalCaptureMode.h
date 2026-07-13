#pragma once

#include "IFrameCaptureMode.h"
#include "PresentScheduler.h"
#include "CaptureRing.h"

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
    // Shadow phase-pull instrument: the phase-pull control math run as a DEAD computation,
    // one instance per source-period variant. Each instance closes its own loop with a
    // private bracket query at its pulled target and drives nothing; the temporal log line
    // reports what a live pull would be doing.
    struct PhaseShadow {
        LONGLONG pullQpc;     // extra lag a live pull would apply right now
        LONGLONG errEmaQpc;   // EMA of the phase error at the pulled target
        LONGLONG devEmaQpc;   // EMA of |err - errEma|: phase stability (lock gate input)
        bool engaged;         // lock-gate state at the last update
        bool seeded;          // wrap variant only: errEma holds a sample (0 is a legal value)
        double weight;        // bracket weight at the pulled target (extreme at lock)
    };

    PresentScheduler m_scheduler;
    CaptureRing m_ring;
    LONGLONG m_bracketingDelayQpc;  // present-target lag; static: max(present period, 1.25 x assumed source period)
    LONGLONG m_assumedSrcPeriodQpc; // declared/default source period the lag was sized for
    LONGLONG m_stickinessQpc;       // selection Schmitt band (anti flip-flop at bracket midpoint)
    LONGLONG m_phasePullSlewQpc;    // max shadow pull change per present (approach rate)
    PhaseShadow m_shadowEst;        // variant fed by the ring's source-period estimator
    PhaseShadow m_shadowSrc;        // variant anchored to the declared -src period
    PhaseShadow m_shadowWrap;       // circular-phase variant, anchored to the declared -src comb
    LONGLONG m_combQpc;             // phase-comb spacing: assumed srcP / M (M=1 at integer ratios)
    int m_telemetryCountdown;       // presents until the next estimator-vs-assumption audit
    bool m_lastPickAfter;           // Schmitt state: which bracket side the last pick took
    bool m_advGateOpen;             // Schmitt state: last advance-gate decision (see ADVANCE GATE)
    bool m_vsyncPresent;            // false: QPC-timer present (t:60); true: vblank present (t:vsync)
    LARGE_INTEGER m_baseQpc;        // logging time origin
    float m_targetFramerate;
    float m_srcRateHint;            // declared source fps (-src); 0 = unset, assume >= 60
    IDirect3DDevice9Ex* m_device;

    // The one lag-sizing rule: 1.25x the source period for bracketing headroom, floored at
    // the present period. Setup sizes the operative lag with it; telemetry sizes suggestions.
    LONGLONG LagForSourcePeriod(LONGLONG srcPeriodQpc) const;

    // One shadow update step: private bracket query at the pulled target, then the pull
    // control math (EMA filter, stability gate, bounded asymmetric slew). srcPeriodQpc feeds
    // the gate and clamp; 0 keeps the variant disengaged (estimator not warmed up).
    void UpdatePhaseShadow(PhaseShadow* s, LONGLONG deadline, LONGLONG srcPeriodQpc);

    // Circular-phase counterpart: the error and its EMAs live in (-mod/2, mod/2], the pull
    // wraps modulo modulusQpc behind a hysteresis band, and the slew is symmetric. The
    // modulus is the phase-comb spacing: the full source period at integer ratios, srcP/M at
    // a rational ratio N:M (see the comb derivation in Setup).
    void UpdatePhaseShadowWrap(PhaseShadow* s, LONGLONG deadline, LONGLONG modulusQpc);

public:
    TemporalCaptureMode(float framerate, bool vsyncPresent = false, float srcRateHint = 0.0f);

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;
};
