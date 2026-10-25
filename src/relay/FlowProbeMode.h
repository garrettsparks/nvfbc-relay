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
//   ofprobe:<s>          capabilities, then at once <s> seconds of the compositor's configuration
//                        (luma, grid 4, MEDIUM, both ways, cost on) at 60 flows a second, handed
//                        over as the compositor hands them, with nothing waiting for the end:
//                        for a GPU trace taken a fixed time into a benchmark, which counts the
//                        flow's turns on the game's graphics engine. The time logged is the
//                        execute's return, which under load comes well before the flow is done
//   ofprobeorder:<s>     the ordering check of the self-test, repeated for <s> seconds, so it can
//                        run beside a benchmark: whether a warp drawn a present after its flow
//                        was handed over reads that flow, with nothing waiting between them
//
// The loop variants that measured the turns a flow takes (with and without a completion query,
// with a draw, forward only, on the engine's D3D12 and CUDA interfaces) are in the history before
// the probe was trimmed; their results are in the spec, 6.1.
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
    FlowProbeMode(int loopSeconds, bool orderLoop);

    UINT GetPresentationInterval() const override;
    MaybeFailure Setup(const RelayContext& ctx) override;
    MaybeFailure Run(RelayContext& ctx, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams) override;
    const char* GetModeName() const override;

private:
    int m_loopSeconds;
    bool m_orderLoop;
};
