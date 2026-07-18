#include "FlowWarpEngine.h"
#include <SimpleLogger.h>
#include <d3dcompiler.h>

// The common header (NV OF API v5.0) carries the structs and enums; the D3D11
// session-init declarations live in nvOpticalFlowD3D11.h. Both are vendored in
// third_party/NvOFSDK; the __has_include gate keeps the engine buildable (disabled,
// loud) if the D3D11 header is ever absent.
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
    NV_OF_D3D11_API_FUNCTION_LIST* funcs = (NV_OF_D3D11_API_FUNCTION_LIST*)m_ofFuncs;
    if (funcs) {
        if (m_regBefore) funcs->nvOFUnregisterResourceD3D11((NvOFGPUBufferHandle)m_regBefore);
        if (m_regAfter) funcs->nvOFUnregisterResourceD3D11((NvOFGPUBufferHandle)m_regAfter);
        if (m_regFlow) funcs->nvOFUnregisterResourceD3D11((NvOFGPUBufferHandle)m_regFlow);
        if (m_ofHandle) funcs->nvOFDestroy((NvOFHandle)m_ofHandle);
        delete funcs;
    }
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
    LOGERR("FlowWarpEngine: nvOpticalFlowD3D11.h was not present in third_party/NvOFSDK/ "
           "at build time - restore it and rebuild to enable the raw-flow path");
    return false;
#else
    m_ofLib = (void*)LoadLibraryA("nvofapi64.dll");
    if (!m_ofLib) {
        LOGERR("FlowWarpEngine: nvofapi64.dll not found (driver R455+ required)");
        return false;
    }
    typedef NV_OF_STATUS(NVOFAPI* PFNCreateInstance)(uint32_t, NV_OF_D3D11_API_FUNCTION_LIST*);
    PFNCreateInstance createInstance =
        (PFNCreateInstance)GetProcAddress((HMODULE)m_ofLib, "NvOFAPICreateInstanceD3D11");
    if (!createInstance) {
        LOGERR("FlowWarpEngine: NvOFAPICreateInstanceD3D11 export missing (driver too old for D3D11 OF?)");
        return false;
    }
    NV_OF_D3D11_API_FUNCTION_LIST* funcs = new NV_OF_D3D11_API_FUNCTION_LIST{};
    NV_OF_STATUS st = createInstance(NV_OF_API_VERSION, funcs);
    if (st != NV_OF_SUCCESS) {
        LOGERR("FlowWarpEngine: NvOFAPICreateInstanceD3D11 failed (status %d)", (int)st);
        delete funcs;
        return false;
    }
    m_ofFuncs = funcs;

    NvOFHandle hOf = NULL;
    st = funcs->nvCreateOpticalFlowD3D11(m_dev, m_ctx, &hOf);
    if (st != NV_OF_SUCCESS) {
        LOGERR("FlowWarpEngine: nvCreateOpticalFlowD3D11 failed (status %d)", (int)st);
        return false;
    }
    m_ofHandle = hOf;

    // Log the driver's supported input formats once - if BGRA8 turns out wrong for ABGR8,
    // this line plus a channel-swap symptom pins it immediately.
    uint32_t fmtCount = 0;
    if (funcs->nvOFGetSurfaceFormatCountD3D11(hOf, NV_OF_BUFFER_USAGE_INPUT,
            NV_OF_MODE_OPTICALFLOW, &fmtCount) == NV_OF_SUCCESS && fmtCount > 0 && fmtCount <= 16) {
        DXGI_FORMAT fmts[16] = {};
        if (funcs->nvOFGetSurfaceFormatD3D11(hOf, NV_OF_BUFFER_USAGE_INPUT,
                NV_OF_MODE_OPTICALFLOW, fmts) == NV_OF_SUCCESS) {
            for (uint32_t i = 0; i < fmtCount; i++) {
                LOG("FlowWarpEngine: supported input DXGI format %d", (int)fmts[i]);
            }
        }
    }

    NV_OF_INIT_PARAMS init = {};
    init.width = (uint32_t)m_width;
    init.height = (uint32_t)m_height;
    init.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
    init.mode = NV_OF_MODE_OPTICALFLOW;
    init.perfLevel = NV_OF_PERF_LEVEL_MEDIUM;   // SLOW = best quality; knob for later A/B
    init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;   // BOTH + bwdOutputBuffer = v2 occlusion
    init.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
    st = funcs->nvOFInit(hOf, &init);
    if (st != NV_OF_SUCCESS) {
        LOGERR("FlowWarpEngine: nvOFInit failed (status %d)", (int)st);
        return false;
    }

    // Flow output registers now; the two input textures register lazily on first Interpolate
    // (the sidecar owns them and hands stable pointers per call).
    NvOFGPUBufferHandle hFlow = NULL;
    st = funcs->nvOFRegisterResourceD3D11(hOf, m_flowTex, &hFlow);
    if (st != NV_OF_SUCCESS) {
        LOGERR("FlowWarpEngine: flow buffer registration failed (status %d)", (int)st);
        return false;
    }
    m_regFlow = hFlow;
    return true;
#endif
}

bool FlowWarpEngine::Interpolate(ID3D11ShaderResourceView* beforeSrv, ID3D11ShaderResourceView* afterSrv,
                                 ID3D11Texture2D* beforeTex, ID3D11Texture2D* afterTex,
                                 float w, ID3D11RenderTargetView* outRtv) {
    if (!m_enabled) return false;

#ifdef NVOF_D3D11_AVAILABLE
    NV_OF_D3D11_API_FUNCTION_LIST* funcs = (NV_OF_D3D11_API_FUNCTION_LIST*)m_ofFuncs;
    // Lazy input registration: the sidecar's ping/pong pointers are stable for the session.
    if (!m_regBefore) {
        NvOFGPUBufferHandle h = NULL;
        if (funcs->nvOFRegisterResourceD3D11((NvOFHandle)m_ofHandle, beforeTex, &h) != NV_OF_SUCCESS) {
            LOGERR("FlowWarpEngine: before-frame registration failed");
            return false;
        }
        m_regBefore = h;
    }
    if (!m_regAfter) {
        NvOFGPUBufferHandle h = NULL;
        if (funcs->nvOFRegisterResourceD3D11((NvOFHandle)m_ofHandle, afterTex, &h) != NV_OF_SUCCESS) {
            LOGERR("FlowWarpEngine: after-frame registration failed");
            return false;
        }
        m_regAfter = h;
    }
    NV_OF_EXECUTE_INPUT_PARAMS in = {};
    in.inputFrame = (NvOFGPUBufferHandle)m_regBefore;
    in.referenceFrame = (NvOFGPUBufferHandle)m_regAfter;
    in.disableTemporalHints = NV_OF_FALSE;   // successive video frames: prior flow seeds this one
    NV_OF_EXECUTE_OUTPUT_PARAMS out = {};
    out.outputBuffer = (NvOFGPUBufferHandle)m_regFlow;
    NV_OF_STATUS st = funcs->nvOFExecute((NvOFHandle)m_ofHandle, &in, &out);
    if (st != NV_OF_SUCCESS) {
        LOGERR("FlowWarpEngine: nvOFExecute failed (status %d)", (int)st);
        return false;
    }
#else
    (void)beforeTex; (void)afterTex;
    return false;
#endif

    // Warp pass:
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
