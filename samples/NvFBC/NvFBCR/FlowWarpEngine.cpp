#include "FlowWarpEngine.h"
#include <SimpleLogger.h>
#include <d3dcompiler.h>

// The common header (real, vendored from the archive: NV OF API v5.0) carries the structs
// and enums; the D3D11 session-init declarations live in nvOpticalFlowD3D11.h, which the
// user drops in from their SDK download (dev-program license).
#include "../../../third_party/NvOFSDK/nvOpticalFlowCommon.h"
#if defined(__has_include)
#  if __has_include("../../../third_party/NvOFSDK/nvOpticalFlowD3D11.h")
#    include <d3d11.h>
#    include "../../../third_party/NvOFSDK/nvOpticalFlowD3D11.h"
#    define NVOF_D3D11_AVAILABLE 1
#  endif
#endif

// Warp v1: occlusion-blind bidirectional shift-and-lerp along one forward flow field.
// Flow texels are S10.5 fixed point (1/32 pixel units) at grid resolution; scale to UV.
// "Basically a lerp with a weight" - correct, after moving the pixels first.
static const char* kWarpVs =
    "void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {\n"
    "    uv = float2((id << 1) & 2, id & 2);\n"
    "    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n";
static const char* kWarpPs =
    "Texture2D beforeTex : register(t0);\n"
    "Texture2D afterTex : register(t1);\n"
    "Texture2D<int2> flowTex : register(t2);\n"
    "SamplerState samp : register(s0);\n"
    "cbuffer WarpParams : register(b0) {\n"
    "    float w;           // interpolation phase, 0=before 1=after\n"
    "    float invW;        // 1/width\n"
    "    float invH;        // 1/height\n"
    "    float gridSize;    // flow grid (4)\n"
    "};\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "    int2 cell = int2(pos.xy / gridSize);\n"
    "    int2 raw = flowTex.Load(int3(cell, 0));\n"
    "    // S10.5 fixed point: 1/32 pixel units -> pixels -> UV\n"
    "    float2 f = float2(raw) / 32.0 * float2(invW, invH);\n"
    "    float3 cb = beforeTex.SampleLevel(samp, uv - w * f, 0).rgb;\n"
    "    float3 ca = afterTex.SampleLevel(samp, uv + (1.0 - w) * f, 0).rgb;\n"
    "    return float4(lerp(cb, ca, w), 1.0);\n"
    "}\n";

FlowWarpEngine::FlowWarpEngine()
    : m_dev(NULL), m_ctx(NULL), m_width(0), m_height(0), m_gridSize(4)
    , m_flowTex(NULL), m_flowSrv(NULL)
    , m_warpVs(NULL), m_warpPs(NULL), m_warpCb(NULL), m_warpSampler(NULL)
    , m_ofLib(NULL), m_ofHandle(NULL), m_ofFuncs(NULL)
    , m_regBefore(NULL), m_regAfter(NULL), m_regFlow(NULL)
    , m_enabled(false)
{
}

FlowWarpEngine::~FlowWarpEngine() {
#ifdef NVOF_D3D11_AVAILABLE
    // VERIFY-ON-HEADER-DROP: release registered OF buffers + destroy the session per the
    // D3D11 header's function list (names below follow the SDK samples' conventions).
#endif
    if (m_ofLib) FreeLibrary((HMODULE)m_ofLib);
    if (m_warpSampler) m_warpSampler->Release();
    if (m_warpCb) m_warpCb->Release();
    if (m_warpPs) m_warpPs->Release();
    if (m_warpVs) m_warpVs->Release();
    if (m_flowSrv) m_flowSrv->Release();
    if (m_flowTex) m_flowTex->Release();
}

bool FlowWarpEngine::Setup(ID3D11Device* dev, ID3D11DeviceContext* ctx, int width, int height) {
    m_dev = dev; m_ctx = ctx; m_width = width; m_height = height;
    if (!CreateWarpPipeline()) return false;
    if (!CreateFlowSession()) return false;
    m_enabled = true;
    LOG("FlowWarpEngine initialized - NVOFA grid %d, occlusion-blind warp v1", m_gridSize);
    return true;
}

bool FlowWarpEngine::CreateWarpPipeline() {
    ID3DBlob* blob = NULL; ID3DBlob* err = NULL;
    HRESULT hr = D3DCompile(kWarpVs, strlen(kWarpVs), "WarpVS", NULL, NULL, "main", "vs_5_0", 0, 0, &blob, &err);
    if (FAILED(hr)) {
        LOGERR("FlowWarpEngine: warp VS compile failed: %s", err ? (char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return false;
    }
    hr = m_dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &m_warpVs);
    blob->Release();
    if (FAILED(hr)) { LOGERR("FlowWarpEngine: warp VS create failed (0x%08x)", hr); return false; }

    hr = D3DCompile(kWarpPs, strlen(kWarpPs), "WarpPS", NULL, NULL, "main", "ps_5_0", 0, 0, &blob, &err);
    if (FAILED(hr)) {
        LOGERR("FlowWarpEngine: warp PS compile failed: %s", err ? (char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return false;
    }
    hr = m_dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &m_warpPs);
    blob->Release();
    if (FAILED(hr)) { LOGERR("FlowWarpEngine: warp PS create failed (0x%08x)", hr); return false; }

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;   // 4 floats
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = m_dev->CreateBuffer(&bd, NULL, &m_warpCb);
    if (FAILED(hr)) { LOGERR("FlowWarpEngine: warp CB failed (0x%08x)", hr); return false; }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;   // sub-pixel flow displacements
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = m_dev->CreateSamplerState(&sd, &m_warpSampler);
    if (FAILED(hr)) { LOGERR("FlowWarpEngine: warp sampler failed (0x%08x)", hr); return false; }

    // Flow output texture: (w/grid, h/grid), R16G16_SINT = NV_OF_FLOW_VECTOR (S10.5 x, y).
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (m_width + m_gridSize - 1) / m_gridSize;
    td.Height = (m_height + m_gridSize - 1) / m_gridSize;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16_SINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = m_dev->CreateTexture2D(&td, NULL, &m_flowTex);
    if (FAILED(hr)) { LOGERR("FlowWarpEngine: flow texture failed (0x%08x)", hr); return false; }
    hr = m_dev->CreateShaderResourceView(m_flowTex, NULL, &m_flowSrv);
    if (FAILED(hr)) { LOGERR("FlowWarpEngine: flow SRV failed (0x%08x)", hr); return false; }
    return true;
}

bool FlowWarpEngine::CreateFlowSession() {
#ifndef NVOF_D3D11_AVAILABLE
    LOGERR("FlowWarpEngine: nvOpticalFlowD3D11.h not present in third_party/NvOFSDK/ - "
           "drop it from your Optical Flow SDK and rebuild to enable the raw-flow path");
    return false;
#else
    // VERIFY-ON-HEADER-DROP: everything in this block follows the SDK samples' documented
    // sequence; diff names/signatures against the real header on first build.
    //   1. LoadLibrary("nvofapi64.dll")                        (driver-shipped)
    //   2. NvOFAPICreateInstanceD3D11(NV_OF_API_VERSION, &funcList)
    //   3. funcList.nvCreateOpticalFlowD3D11(m_dev, m_ctx, &m_ofHandle)
    //   4. nvOFInit: NV_OF_INIT_PARAMS { width, height, outGridSize = GRID_SIZE_4,
    //        mode = NV_OF_MODE_OPTICALFLOW, perfLevel = NV_OF_PERF_LEVEL_MEDIUM,
    //        predDirection = NV_OF_PRED_DIRECTION_FORWARD, inputBufferFormat = ABGR8 }
    //   5. nvOFRegisterResourceD3D11 for the two input textures (sidecar's converted BGRA8;
    //      byte-order verify vs ABGR8) and m_flowTex as the OUTPUT buffer.
    m_ofLib = (void*)LoadLibraryA("nvofapi64.dll");
    if (!m_ofLib) {
        LOGERR("FlowWarpEngine: nvofapi64.dll not found (driver R455+ required)");
        return false;
    }
    LOGERR("FlowWarpEngine: session init not yet verified against the dropped header - "
           "complete CreateFlowSession per the VERIFY block and remove this line");
    return false;
#endif
}

bool FlowWarpEngine::Interpolate(ID3D11ShaderResourceView* beforeSrv, ID3D11ShaderResourceView* afterSrv,
                                 ID3D11Texture2D* beforeTex, ID3D11Texture2D* afterTex,
                                 float w, ID3D11RenderTargetView* outRtv) {
    if (!m_enabled) return false;

#ifdef NVOF_D3D11_AVAILABLE
    // VERIFY-ON-HEADER-DROP: nvOFExecute with inputFrame=beforeTex, referenceFrame=afterTex,
    // outputBuffer=m_regFlow (writes m_flowTex). Synchronous on the immediate context.
    (void)beforeTex; (void)afterTex;
    return false;   // unreachable until CreateFlowSession completes
#else
    (void)beforeTex; (void)afterTex;
#endif

    // Warp pass (live code, runs once the session above is completed):
    float cb[4] = { w, 1.0f / (float)m_width, 1.0f / (float)m_height, (float)m_gridSize };
    m_ctx->UpdateSubresource(m_warpCb, 0, NULL, cb, 0, 0);
    D3D11_VIEWPORT vp = {};
    vp.Width = (FLOAT)m_width; vp.Height = (FLOAT)m_height; vp.MaxDepth = 1.0f;
    m_ctx->OMSetRenderTargets(1, &outRtv, NULL);
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetInputLayout(NULL);
    m_ctx->VSSetShader(m_warpVs, NULL, 0);
    m_ctx->PSSetShader(m_warpPs, NULL, 0);
    ID3D11ShaderResourceView* srvs[3] = { beforeSrv, afterSrv, m_flowSrv };
    m_ctx->PSSetShaderResources(0, 3, srvs);
    m_ctx->PSSetSamplers(0, 1, &m_warpSampler);
    m_ctx->VSSetConstantBuffers(0, 1, &m_warpCb);
    m_ctx->PSSetConstantBuffers(0, 1, &m_warpCb);
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[3] = { NULL, NULL, NULL };
    m_ctx->PSSetShaderResources(0, 3, nulls);
    return true;
}
