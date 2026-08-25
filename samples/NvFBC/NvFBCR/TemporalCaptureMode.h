#pragma once

#include "IFrameCaptureMode.h"
#include "IFrameCompositor.h"
#include "PresentScheduler.h"
#include "CaptureRing.h"
#include "FrameMarker.h"
#include "TemporalPolicy.h"
#include "EtwConsumer.h"

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

class TemporalCaptureMode : public IFrameCaptureMode,
                            private CaptureRing::IRotationOracle {
private:
    // CaptureRing::IRotationOracle, answered on the CAPTURE thread. Both take the flip
    // history's lock for one bounded lookup each; the ring calls them once per batch.
    virtual bool Grid(long long batchPeriodTicks, int* outStride, int* outFlipsPerSource,
                      long long* outSpacingTicks) override;
    virtual bool AnchorAndSteps(long long batchStartTs, long long prevAnchorTs,
                                long long* outOffset, int* outSteps) override;

    PresentScheduler m_scheduler;
    CaptureRing m_ring;
    LONGLONG m_bracketingDelayQpc;  // present-target lag; static: max(present period, 1.25 x assumed source period)
    LONGLONG m_assumedSrcPeriodQpc; // declared/default source period the lag was sized for
    policy::PolicyConfig m_policyCfg;    // stickiness band, comb spacing (0 = lock off), pull slew, passthrough gate
    policy::PhaseLockState m_lockState;  // comb-lock pull/EMAs/gate (pure policy state)
    IFrameCompositor* m_compositor; // owned; picked at Setup from m_compositorKind
    int m_telemetryCountdown;       // presents until the next estimator-vs-assumption audit
    CompositorKind m_compositorKind;
    bool m_lock;                    // -lock: opt in to the comb lock (needs -src); default off
    bool m_mark;                    // -mark: burn the frame-counter marker (debug); default off
    unsigned int m_markFrames;      // -mark N: burn only the first N presents; 0 = all (unset)
    bool m_tint;                    // -tint: border-tint synthesized frames (blend mode, debug)
    bool m_vsyncPresent;            // false: QPC-timer present (t:60); true: vblank present (t:vsync)
    LARGE_INTEGER m_baseQpc;        // logging time origin
    float m_targetFramerate;
    float m_srcRateHint;            // declared source fps (-src); 0 = unset, assume >= 60
    IDirect3DDevice9Ex* m_device;
    FrameMarker m_marker;           // per-present provenance burn-in (inert unless -mark)
    bool m_etw;                     // -etw: read the driver's scanout times while capturing
    // -nojoin: keep the ETW session and its flip lines, skip the per-present grid lookup.
    // The A/B control for the join itself: -etw off logs no flips, so it cannot answer
    // whether the join affects the flip grid, and an older build differs by more than the
    // join. This is the only arrangement where one binary in one session isolates it.
    bool m_noJoin;
    // -dejit: subtract each batch's measured delivery lateness from its stamps (stage 6,
    // the phantom-blend fix). Requires -etw with the join on AND the comb lock configured
    // (the calm gate reads lock state; without a lock it would be vacuously open, running
    // corrections straight through the stall recoveries it exists to protect). Corrections
    // live in the overlay, never in the slots; off, FindBracket takes the exact pre-stage-6
    // read path.
    bool m_dejitter;
    bool m_fgPhase;                 // -fgphase: per-batch content-phase instrument (ring-side)
    bool m_phaseKeep;               // -phasekeep: phase-aware keep-real (needs -etw with the join)
    // -lag N: extra bracketing delay in ms, on top of the derived 1.25x source period. A HOLD
    // is a bracket with no after-frame, and it re-presents the last output - a visible
    // duplicate. Moving the target further into the past makes it likelier that a newer frame
    // has already arrived, so this trades output latency for holds. Latency here is not felt
    // by the player (the source display is direct) and only shifts the stream, which is
    // already seconds behind. Replayed on a 55-minute capture: holds 1.23/s at +0,
    // 0.29/s at +50 ms, 0.01/s at +75 ms; the frames those holds wanted DID arrive, just
    // later than the target. The cost is that they become blends, not passthroughs.
    unsigned int m_extraLagMs;
    bool m_phaseKeepRequested;      // asked for, so an unmet prerequisite can say so once
    policy::AnchorChain m_anchorChain;   // stride continuity for the correction's anchoring
    policy::StampOverlay m_overlay;      // present-thread-owned; FindBracket reads through it
    long long m_nextBatch = 0;           // cursor into the ring's batch-start history
    LONGLONG m_maxTargetQpc = 0;         // newest target consumed; the coherence-rule fence
    // Session telemetry, all logged at exit: without the blocked counts, a live A/B cannot
    // distinguish "no late deliveries" from "corrections measured and discarded".
    long long m_dejitMeasured = 0;
    long long m_dejitLate = 0;
    long long m_dejitCorrected = 0;
    long long m_dejitFenceBlocked = 0;
    long long m_dejitLockDeclined = 0;
    long long m_dejitSkipped = 0;        // batches lapped past while the walk was pinned
    EtwFlipConsumer m_etwConsumer;  // inert unless m_etw; nothing in the policy reads it
    // How far back PairBatchMember measures the flip grid's step, in QPC ticks. The
    // confidence bound it accepts is derived from that measurement, so nothing here needs to
    // know the frame-generation multiplier. 200 ms holds ~24 flips at 60x2 and ~36 at 60x3,
    // which is a stable median without spanning a rate change.
    LONGLONG m_flipCadenceWindowQpc = 0;

    // The one lag-sizing rule: 1.25x the source period for bracketing headroom, floored at
    // the present period. Setup sizes the operative lag with it; telemetry sizes suggestions.
    LONGLONG LagForSourcePeriod(LONGLONG srcPeriodQpc) const;

public:
    TemporalCaptureMode(float framerate, bool vsyncPresent = false, float srcRateHint = 0.0f,
                        bool lock = false, CompositorKind compositor = kCompositorNearest,
                        bool mark = false, unsigned int markFrames = 0, bool tint = false,
                        bool etw = false, bool noJoin = false, bool dejitter = false,
                        bool fgPhase = false, bool phaseKeep = false,
                        unsigned int extraLagMs = 0);
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
