#pragma once

#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <vector>

#include "FlowPlan.h"
#include "nvof/nvOpticalFlowCommon.h"
#include "nvof/nvOpticalFlowD3D11.h"

// THE OPTICAL FLOW INTERPOLATION ON THE D3D11 PRESENT (docs/optical-flow-interpolation-spec.md,
// 4.3 and 4.4): the driver's optical flow engine, the flow fields it fills, and the warp that
// draws a synthesized frame from them, all on the present's own device and immediate context.
//
// The engine runs on its own hardware; what it costs the game is the turns its hand-off takes
// on the game's graphics engine, two per flow, and a draw issued on the same context just before
// a flow rides in those two (sittings oftrace2 to oftrace5). So the present hands each flow over
// right after a draw of its own, and nothing waits for a flow on the CPU. D3D11 gives no way to
// know when the engine has finished a flow (see Warp), so a warp is drawn only from a flow handed
// over at an earlier present, a flow taking about 4 ms under load against a present's 16.7.
//
// Each frame reaches the engine as 8-bit luma at the output's size, drawn from the frame the
// present samples (an NvFBC ring slot or a WGC texture, of the output's size or not), once per
// frame and kept for the pair after. One path for every capture: handing WGC's frames over as
// they are measured no cheaper for the game (sittings ofcap2, ofpts2) and had no input for the
// frames NvFBC captured before WGC took over.
//
// The flow fields live in policy::kFlowSlots cache slots, one forward and one backward field and
// their costs each, numbered as the FlowPlanner numbers them.
class OpticalFlowWarp {
public:
    // A captured frame as the present sees it.
    struct Frame {
        int64_t ts = 0;                          // capture stamp, the frame's identity
        ID3D11ShaderResourceView* srv = NULL;    // the colour the luma draw and the warp sample
    };

    OpticalFlowWarp() = default;
    ~OpticalFlowWarp();
    OpticalFlowWarp(const OpticalFlowWarp&) = delete;
    OpticalFlowWarp& operator=(const OpticalFlowWarp&) = delete;

    // Opens the engine on dev/ctx at the output's size and readies the shaders. Loud on failure.
    bool Setup(ID3D11Device* dev, ID3D11DeviceContext* ctx, int width, int height);

    // Hands the flow of before to after into cache slot `slot` to the engine. hints: the slot's
    // previous flow was the pair just before this one. False when it could not be handed over
    // (the caller draws the blend).
    bool Flow(int slot, const Frame& before, const Frame& after, bool hints);

    // Draws the warp at w from slot's fields over rtv, sampling colour from before and after.
    // Nothing in D3D11 orders this draw after the engine's work on the slot: the probe's ordering
    // check found a warp drawn straight after its flow reading the slot's previous flow in 99 of
    // 100 checks, and neither an event query nor a copy from the fields waits for the engine. The
    // caller warps only from a flow handed over at an earlier present.
    // present numbers the present it is drawn for, for its GPU time (TakeWarpTime).
    bool Warp(int slot, ID3D11ShaderResourceView* before, ID3D11ShaderResourceView* after,
              float w, ID3D11RenderTargetView* rtv, long long present);

    // The GPU time of warps drawn at earlier presents, read without waiting: each warp is timed
    // with timestamp queries around its draw, and the result is read once the GPU has it,
    // normally a present or two later. Returns false when no new result has arrived; otherwise
    // the newest one's microseconds and how many presents before present its warp was drawn.
    // Every result goes into the summary; the newest is returned.
    bool TakeWarpTime(long long present, long long* us, int* presentsAgo);

    void LogSummary() const;

private:
    static const int kGrid = 4;
    // Luma frames kept: the four cached pairs share frames, so six covers them with room.
    static const int kLumaSlots = 6;

    struct Luma {
        ID3D11Texture2D* texture = NULL;
        ID3D11RenderTargetView* rtv = NULL;
        NvOFGPUBufferHandle handle = NULL;
        int64_t ts = 0;
        bool valid = false;
        long long lastUse = 0;
    };
    struct Field {
        ID3D11Texture2D* texture = NULL;
        ID3D11ShaderResourceView* srv = NULL;
        NvOFGPUBufferHandle handle = NULL;
    };
    struct Slot {
        Field fwd, bwd, fwdCost, bwdCost;
    };
    // One warp's timestamp queries, in flight until read.
    struct Timing {
        ID3D11Query* disjoint = NULL;
        ID3D11Query* start = NULL;
        ID3D11Query* end = NULL;
        long long present = 0;
        bool pending = false;
    };
    // Warps whose times can be in flight at once; one more is drawn untimed.
    static const int kTimings = 8;

    bool MakeField(DXGI_FORMAT format, bool view, Field* out);
    bool CompileShaders();
    // The engine's input for a frame: its luma, drawn now if not kept. NULL when it has none.
    NvOFGPUBufferHandle InputFor(const Frame& frame, int64_t keep);
    bool DrawLuma(const Frame& frame, Luma* into);

    bool m_ready = false;
    int m_width = 0, m_height = 0;
    int m_cellsX = 0, m_cellsY = 0;
    ID3D11Device* m_dev = NULL;            // borrowed from the present
    ID3D11DeviceContext* m_ctx = NULL;     // borrowed from the present

    NV_OF_D3D11_API_FUNCTION_LIST m_of = {};
    NvOFHandle m_session = NULL;
    Slot m_slots[policy::kFlowSlots];
    Luma m_luma[kLumaSlots];

    ID3D11VertexShader* m_vs = NULL;
    ID3D11PixelShader* m_lumaPs = NULL;
    ID3D11PixelShader* m_warpPs = NULL;
    ID3D11SamplerState* m_linear = NULL;
    ID3D11Buffer* m_lumaCb = NULL;
    ID3D11Buffer* m_warpCb = NULL;
    Timing m_timings[kTimings];
    std::vector<float> m_warpUs;           // every warp GPU time read, for the summary
    long long m_warpsUntimed = 0;

    long long m_uses = 0;
    long long m_flows = 0;
    long long m_flowFailures = 0;
    long long m_lumaDraws = 0;
    long long m_warps = 0;
    double m_submitMsTotal = 0.0;
    double m_submitMsMax = 0.0;
};
