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
// right after a draw of its own, and nothing waits for a flow on the CPU. D3D11 orders a draw
// that reads a flow field after the engine's work on it only when the field can be a render
// target, as NVIDIA's samples make the engine's buffers (see Warp); with shader resource alone a
// warp read the slot's previous flow. Nothing in D3D11 says when a flow is done without waiting
// for it, so a warp is drawn from a flow handed over at an earlier present, a flow taking about
// 4 ms under load against a present's 16.7, and one not done by then holds that present on the
// GPU rather than warp from the old flow.
//
// Each frame reaches the engine as 8-bit luma at the output's size, drawn from the frame the
// present samples (an NvFBC ring slot or a WGC texture, of the output's size or not), once per
// frame and kept for the pair after. One path for every capture: handing WGC's frames over as
// they are measured no cheaper for the game (sittings ofcap2, ofpts2) and had no input for the
// frames NvFBC captured before WGC took over.
//
// The flow fields live in policy::kFlowSlots cache slots, one forward and one backward field and
// their costs each, numbered as the FlowPlanner numbers them.
//
// THE CUT GATE (FlowWarpMath.h, step 4): when a frame's luma is drawn for the engine, it is also
// drawn at 64x32 and reduced by mips to its mean, on the GPU. Handing a flow over copies the
// pair's two means beside the slot's fields, and the warp shader reads them: a pair whose darker
// mean is under half the brighter (a cut to black) is drawn as the crossfade. Nothing waits for
// the means on the CPU; a copy is read back a few presents later for the log only (the summary,
// a line per cut, and lr= on the present's line through PairRatio).
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
    // fieldBinds: the flow fields' bind flags; the probe's ordering control passes shader
    // resource alone.
    bool Setup(ID3D11Device* dev, ID3D11DeviceContext* ctx, int width, int height,
               UINT fieldBinds = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);

    // Hands the flow of before to after into cache slot `slot` to the engine. hints: the slot's
    // previous flow was the pair just before this one. False when it could not be handed over
    // (the caller draws the blend).
    bool Flow(int slot, const Frame& before, const Frame& after, bool hints);

    // Draws the warp at w from slot's fields over rtv, sampling colour from before and after.
    // D3D11 orders this draw after the engine's work on the slot because the fields can be render
    // targets: the probe's ordering check, with fields of shader resource alone, found a warp
    // drawn straight after its flow reading the slot's previous flow in 99 of 100 checks, and
    // with the render target flag in none. The order holds on the GPU: a draw issued before the
    // flow is done waits there for the engine, and the CPU call returns at once. An event query
    // ended after the flow does not wait for the engine, so it cannot say when a flow is done.
    // present numbers the present it is drawn for, for its GPU time (TakeWarpTime).
    bool Warp(int slot, ID3D11ShaderResourceView* before, ID3D11ShaderResourceView* after,
              float w, ID3D11RenderTargetView* rtv, long long present);

    // The GPU time of warps drawn at earlier presents, read without waiting: each warp is timed
    // with timestamp queries around its draw, and the result is read once the GPU has it,
    // normally a present or two later. Returns false when no new result has arrived; otherwise
    // the newest one's microseconds and how many presents before present its warp was drawn.
    // Every result goes into the summary; the newest is returned.
    bool TakeWarpTime(long long present, long long* us, int* presentsAgo);

    // The darker-over-brighter ratio of the means of the pair flowed into slot
    // (flowwarp::MeanRatio), once its read-back has arrived; -1 until then. Under
    // flowwarp::kCutRatio the slot's warps draw the crossfade. Reads any read-back that has
    // arrived first, without waiting.
    float PairRatio(int slot);

    // The slot's forward field, for the probe to copy and read back.
    ID3D11Texture2D* ForwardField(int slot) const {
        return slot >= 0 && slot < policy::kFlowSlots ? m_slots[slot].fwd.texture : NULL;
    }

    void LogSummary() const;

private:
    static const int kGrid = 4;
    // Luma frames kept: the four cached pairs share frames, so six covers them with room.
    static const int kLumaSlots = 6;

    // The cut gate's mean: each frame drawn at kMeanW x kMeanH, R32_FLOAT, whose last of
    // kMeanMips mips is its mean.
    static const int kMeanW = 64;
    static const int kMeanH = 32;
    static const int kMeanMips = 7;
    // Means read-backs that can be in flight at once; a flow handed over with none free is
    // gated all the same, only not logged.
    static const int kMeanReads = 8;

    struct Luma {
        ID3D11Texture2D* texture = NULL;
        ID3D11RenderTargetView* rtv = NULL;
        NvOFGPUBufferHandle handle = NULL;
        ID3D11Texture2D* mean = NULL;             // kMeanW x kMeanH with mips
        ID3D11RenderTargetView* meanRtv = NULL;   // its top mip
        ID3D11ShaderResourceView* meanSrv = NULL; // every mip, for GenerateMips
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
        ID3D11Texture2D* means = NULL;            // 2x1 R32_FLOAT: before, after
        ID3D11ShaderResourceView* meansSrv = NULL;
        int64_t beforeTs = 0, afterTs = 0;        // the pair last handed over
        float ratio = -1.0f;                      // its MeanRatio once read back, else -1
    };
    // One copy of a slot's means on its way back to the CPU.
    struct MeanRead {
        ID3D11Texture2D* staging = NULL;
        int slot = -1;
        int64_t beforeTs = 0, afterTs = 0;
        bool pending = false;
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
    bool MakeCutGate();
    bool CompileShaders();
    // The engine's input for a frame: its luma, drawn now if not kept. NULL when it has none.
    Luma* InputFor(const Frame& frame, int64_t keep);
    bool DrawLuma(const Frame& frame, Luma* into);
    // The frame's mean, drawn after its luma, timed.
    void DrawMean(const Frame& frame, Luma* into);
    // Reads every means read-back and pass time that has arrived, without waiting.
    void PollMeans();
    // Reads finished timestamp queries into us; the newest present's value into *newestUs.
    bool ReadTimings(Timing* timings, std::vector<float>* us, long long present,
                     long long* newestUs, int* presentsAgo);

    bool m_ready = false;
    int m_width = 0, m_height = 0;
    int m_cellsX = 0, m_cellsY = 0;
    UINT m_fieldBinds = 0;
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
    ID3D11Buffer* m_meanCb = NULL;
    ID3D11Buffer* m_warpCb = NULL;
    Timing m_timings[kTimings];
    std::vector<float> m_warpUs;           // every warp GPU time read, for the summary
    long long m_warpsUntimed = 0;

    // The cut gate.
    bool m_gate = false;                   // the means are drawn; false leaves every pair a warp
    MeanRead m_meanReads[kMeanReads];
    Timing m_meanTimings[kTimings];
    std::vector<float> m_meanUs;           // every mean pass's GPU time read
    long long m_meansUntimed = 0;
    std::vector<float> m_ratios;           // every pair's MeanRatio read back
    long long m_meanReadsSkipped = 0;      // flows handed over with no read-back free
    long long m_meanReadsFailed = 0;
    long long m_cuts = 0;                  // pairs read back as a cut
    long long m_warpsCut = 0;              // warps drawn over a pair known to be a cut
    long long m_warpsRatioUnread = 0;      // warps drawn before their pair's ratio was read

    long long m_uses = 0;
    long long m_flows = 0;
    long long m_flowFailures = 0;
    long long m_lumaDraws = 0;
    long long m_warps = 0;
    double m_submitMsTotal = 0.0;
    double m_submitMsMax = 0.0;
};
