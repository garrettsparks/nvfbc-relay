#pragma once

#include <windows.h>
#include <d3d11.h>

// Raw NVOFA path: hardware optical flow (NvOFAPI, D3D11 interface) + our own warp
// compositor. We own the blend math (no dimming possible), the flow comes from the same
// silicon FRUC uses, and the warp is upgradeable (occlusion handling, cost-buffer
// confidence, 10-bit end-to-end later). The only NVIDIA runtime dependency is
// nvofapi64.dll, which ships in the driver.
//
// The D3D11 session-init declarations come from third_party/NvOFSDK/nvOpticalFlowD3D11.h,
// picked up via __has_include; if that header is ever absent the engine constructs
// disabled (loud LOGERR) rather than running warp-without-flow, which would be plain
// blend wearing a costume.
//
// Warp v1: single forward flow field F (before->after, 4x4 grid, S10.5 fixed point),
// output(p) = lerp(before(p - w*F), after(p + (1-w)*F), w).
// Occlusion-blind: halos at disocclusions are the expected v1 artifact; v2 options are
// backward flow (NV_OF_PRED_DIRECTION_BACKWARD / bwdOutputBuffer) and cost-buffer-weighted
// blending.
class FlowWarpEngine {
public:
    FlowWarpEngine();
    ~FlowWarpEngine();

    FlowWarpEngine(const FlowWarpEngine&) = delete;
    FlowWarpEngine& operator=(const FlowWarpEngine&) = delete;

    // dev/ctx are the sidecar's D3D11 device. Fails loud (false) if the OF D3D11 header
    // is missing, if the driver lacks NVOFA, or on any session/buffer failure.
    bool Setup(ID3D11Device* dev, ID3D11DeviceContext* ctx, int width, int height);

    // Compute flow before->after and render the warped blend at phase w into outRtv.
    // beforeSrv/afterSrv are the sidecar's converted BGRA8 frames.
    bool Interpolate(ID3D11ShaderResourceView* beforeSrv, ID3D11ShaderResourceView* afterSrv,
                     ID3D11Texture2D* beforeTex, ID3D11Texture2D* afterTex,
                     float w, ID3D11RenderTargetView* outRtv);

    bool Enabled() const { return m_enabled; }

private:
    bool CreateWarpPipeline();
    bool CreateFlowSession();   // gated on nvOpticalFlowD3D11.h presence

    ID3D11Device* m_dev;
    ID3D11DeviceContext* m_ctx;
    int m_width;
    int m_height;
    int m_gridSize;             // 4 (universal); 1 on Ampere+ if caps allow (future knob)

    // Flow output: (w/grid, h/grid) R16G16_SINT, S10.5 fixed point per NV_OF_FLOW_VECTOR.
    ID3D11Texture2D* m_flowTex;
    ID3D11ShaderResourceView* m_flowSrv;

    // Warp pass
    ID3D11VertexShader* m_warpVs;
    ID3D11PixelShader* m_warpPs;
    ID3D11Buffer* m_warpCb;      // { w, 1/width, 1/height, gridSize }
    ID3D11SamplerState* m_warpSampler;

    // OF session state (opaque here; real types live behind the header gate in the .cpp)
    void* m_ofLib;
    void* m_ofHandle;
    void* m_ofFuncs;
    void* m_regBefore;
    void* m_regAfter;
    void* m_regFlow;

    bool m_enabled;
};
