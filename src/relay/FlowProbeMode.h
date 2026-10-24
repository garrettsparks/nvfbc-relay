#pragma once

#include "IFrameCaptureMode.h"

// THE OPTICAL FLOW ENGINE PROBE (docs/optical-flow-interpolation-spec.md, 6.1). A hidden mode
// that answers, on the machine it runs on, what the rebuilt optical-flow compositor assumes:
// whether the driver's optical flow engine is there through D3D11 and what it supports (P1), what
// it costs at the output size (P3), whether a BGRA frame registered as ABGR gives the same flow
// as luma (P4), and, run beside a game under a GPU trace, whether its work takes turns on the
// game's graphics engine (P2).
//
//   ofprobe              capabilities, the warp self-test, then each configuration timed and its
//                        flow checked
//   ofprobe:<s>          capabilities, then at once <s> seconds of the compositor's likely
//                        configuration (luma, grid 4, MEDIUM, both ways, cost on) at 60 flows a
//                        second, for a trace taken a fixed time into a benchmark
//   ofprobedraw:<s>      as ofprobe:<s> with one full-screen draw before each flow: the trace's
//                        positive control, a known turn on the graphics engine per flow
//
// Everything it finds goes to the log on lines starting "ofprobe:". It never shows a popup: an
// unattended run would wait on one forever, and every outcome, an engine that is missing
// included, is a result to read, not a failure of the run.
//
// The frames are generated: a textured pattern and the same pattern moved by a known amount, so
// each configuration's flow is checked against the motion it must find, and a configuration that
// is fast because it is wrong cannot pass for a fast one.
class FlowProbeMode : public IFrameCaptureMode {
public:
    FlowProbeMode(int loopSeconds, bool drawControl);

    UINT GetPresentationInterval() const override;
    MaybeFailure Setup(const RelayContext& ctx) override;
    MaybeFailure Run(RelayContext& ctx, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams) override;
    const char* GetModeName() const override;

private:
    int m_loopSeconds;
    bool m_drawControl;
};
