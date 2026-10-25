#pragma once

#include "IFrameCaptureMode.h"
#include "LaunchOptions.h"

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
//   ofprobenowait:<s>    as ofprobe:<s> without the loop's own completion query after each
//                        execute: whether that query is one of the turns each flow takes on
//                        the graphics engine. The time logged is the execute's return alone,
//                        which under load comes well before the flow is done
//   ofprobenowaitfwd:<s> as ofprobenowait:<s> with the forward flow alone
//   ofprobedrawnowait:<s> as ofprobenowait:<s> with one full-screen draw on the same context
//                        before each flow: whether the flow's turns and a draw's merge
//   ofprobedrawonly:<s>  the draw alone at the same rate, flushed, no flow: the turns a draw
//                        takes by itself
//   ofprobe12:<s>        the same loop on the engine's D3D12 interface, frames and flow in D3D12
//                        resources, completion read from a fence by the CPU
//   ofprobe12fwd:<s>     as ofprobe12:<s> with the forward flow alone: whether each direction
//                        is a turn of its own on the graphics engine
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
    FlowProbeMode(int loopSeconds, launch::ProbeLoop loop);

    UINT GetPresentationInterval() const override;
    MaybeFailure Setup(const RelayContext& ctx) override;
    MaybeFailure Run(RelayContext& ctx, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams) override;
    const char* GetModeName() const override;

private:
    int m_loopSeconds;
    launch::ProbeLoop m_loop;
};
