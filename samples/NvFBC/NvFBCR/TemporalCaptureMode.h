#pragma once

#include "IFrameCaptureMode.h"
#include "PresentScheduler.h"
#include "CaptureRing.h"
#include "BlendRenderer.h"

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
//   This mode        — each present: select the ring frame nearest a content target lagged one
//                      period behind, with hysteresis (monotonic), copy to backbuffer, present.
//   Blend variant    — (b:60 / b:vsync) same loop, but the compose step renders
//                      lerp(before, after, w) via BlendRenderer instead of picking one frame:
//                      no stride quantization at non-integer ratios, at the cost of motion
//                      blur proportional to the bracket gap. Selection hysteresis and the
//                      Schmitt band do not apply (there is no discrete pick to oscillate);
//                      monotonic targets guarantee monotonic content time.
//
// Blend mode will differ only in the last step (blend the bracketing pair instead of picking
// one); optical flow later replaces that step again.
class TemporalCaptureMode : public IFrameCaptureMode {
private:
    PresentScheduler m_scheduler;
    CaptureRing m_ring;
    BlendRenderer m_blendRenderer;  // used only when m_blend
    LONGLONG m_bracketingDelayQpc;  // present-target lag (adaptive: >= one present period)
    LONGLONG m_lagSlewMaxQpc;       // max lag change per present (bounded latency ramp)
    LONGLONG m_stickinessQpc;       // selection Schmitt band (anti flip-flop at bracket midpoint)
    LONGLONG m_phasePullQpc;        // blend only: extra lag pulling the target onto real frames
    LONGLONG m_phaseErrEmaQpc;      // EMA of beforeDiff (the phase offset to the before frame)
    LONGLONG m_phaseDevEmaQpc;      // EMA of |err - errEma|: phase stability (lock gate)
    LONGLONG m_phasePullSlewQpc;    // max pull change per present
    bool m_lastPickAfter;           // Schmitt state: which bracket side the last pick took
    bool m_vsyncPresent;            // false: QPC-timer present (t:60); true: vblank present (t:vsync)
    bool m_blend;                   // false: nearest-pick (t:*); true: lerp compositor (b:*)
    LARGE_INTEGER m_baseQpc;        // logging time origin
    float m_targetFramerate;
    IDirect3DDevice9Ex* m_device;

public:
    TemporalCaptureMode(float framerate, bool vsyncPresent = false, bool blend = false);

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;
};
