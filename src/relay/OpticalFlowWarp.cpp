#include "OpticalFlowWarp.h"
#include "FlowWarpShader.h"

#include <d3dcompiler.h>
#include <string.h>
#include <SimpleLogger.h>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "nvofapi64.lib")

namespace {

// Fullscreen triangle from the vertex id, position only: both pixel shaders work from
// SV_Position, the warp because its math is in pixels, the luma draw with the target's size.
const char* kVs =
    "void main(uint id : SV_VertexID, out float4 pos : SV_Position) {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n";

// The frame as 8-bit luma at the output's size, BT.709 as the warp's guard reads it. A frame of
// another size is sampled linearly at the same place, which scales it on the way.
const char* kLumaPs =
    "Texture2D src : register(t0);\n"
    "SamplerState samp : register(s0);\n"
    "cbuffer LumaParams : register(b0) { float2 invSize; float2 unused; };\n"
    "float main(float4 pos : SV_Position) : SV_Target {\n"
    "    float3 c = src.SampleLevel(samp, pos.xy * invSize, 0).rgb;\n"
    "    return dot(c, float3(0.2126, 0.7152, 0.0722));\n"
    "}\n";

bool Compile(const char* src, size_t len, const char* name, const char* profile, ID3DBlob** out) {
    ID3DBlob* err = NULL;
    const HRESULT hr =
        D3DCompile(src, len, name, NULL, NULL, "main", profile, 0, 0, out, &err);
    if (FAILED(hr)) {
        LOGERR("OpticalFlowWarp: %s failed to compile (0x%08x): %s", name, (unsigned)hr,
               err ? (const char*)err->GetBufferPointer() : "(no message)");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    return true;
}

double MsSince(const LARGE_INTEGER& start) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return 1000.0 * (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
}

}  // namespace

OpticalFlowWarp::~OpticalFlowWarp() {
    if (m_session) {
        for (Slot& s : m_slots) {
            for (Field* f : {&s.fwd, &s.bwd, &s.fwdCost, &s.bwdCost}) {
                if (f->handle) m_of.nvOFUnregisterResourceD3D11(f->handle);
            }
        }
        for (Luma& l : m_luma) {
            if (l.handle) m_of.nvOFUnregisterResourceD3D11(l.handle);
        }
        for (const auto& r : m_registered) m_of.nvOFUnregisterResourceD3D11(r.second);
        m_of.nvOFDestroy(m_session);
    }
    for (Slot& s : m_slots) {
        for (Field* f : {&s.fwd, &s.bwd, &s.fwdCost, &s.bwdCost}) {
            if (f->srv) f->srv->Release();
            if (f->texture) f->texture->Release();
        }
    }
    for (Luma& l : m_luma) {
        if (l.rtv) l.rtv->Release();
        if (l.texture) l.texture->Release();
    }
    if (m_warpCb) m_warpCb->Release();
    if (m_lumaCb) m_lumaCb->Release();
    if (m_linear) m_linear->Release();
    if (m_warpPs) m_warpPs->Release();
    if (m_lumaPs) m_lumaPs->Release();
    if (m_vs) m_vs->Release();
}

bool OpticalFlowWarp::Setup(ID3D11Device* dev, ID3D11DeviceContext* ctx, int width, int height,
                            bool direct) {
    m_dev = dev;
    m_ctx = ctx;
    m_width = width;
    m_height = height;
    m_direct = direct;
    m_cellsX = (width + kGrid - 1) / kGrid;
    m_cellsY = (height + kGrid - 1) / kGrid;

    // nvofapi64.dll is an implicit import resolved at launch, so reaching here means the
    // driver's library loaded; the session is what says whether this GPU has the engine.
    NV_OF_STATUS st = NvOFAPICreateInstanceD3D11(NV_OF_API_VERSION, &m_of);
    if (st != NV_OF_SUCCESS) {
        LOGERR("OpticalFlowWarp: the driver's optical flow interface refused version %d.%d "
               "(status %d)", NV_OF_API_MAJOR_VERSION, NV_OF_API_MINOR_VERSION, (int)st);
        return false;
    }
    st = m_of.nvCreateOpticalFlowD3D11(dev, ctx, &m_session);
    if (st != NV_OF_SUCCESS) {
        m_session = NULL;
        LOGERR("OpticalFlowWarp: no optical flow session on this GPU (status %d)", (int)st);
        return false;
    }
    NV_OF_INIT_PARAMS init = {};
    init.width = (uint32_t)width;
    init.height = (uint32_t)height;
    init.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
    init.mode = NV_OF_MODE_OPTICALFLOW;
    init.perfLevel = NV_OF_PERF_LEVEL_MEDIUM;
    init.enableOutputCost = NV_OF_TRUE;
    init.predDirection = NV_OF_PRED_DIRECTION_BOTH;
    init.inputBufferFormat = direct ? NV_OF_BUFFER_FORMAT_ABGR8 : NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    st = m_of.nvOFInit(m_session, &init);
    if (st != NV_OF_SUCCESS) {
        LOGERR("OpticalFlowWarp: the engine refused %dx%d grid 4 MEDIUM both ways (status %d)",
               width, height, (int)st);
        return false;
    }

    for (Slot& s : m_slots) {
        if (!MakeField(DXGI_FORMAT_R16G16_SINT, true, &s.fwd) ||
            !MakeField(DXGI_FORMAT_R16G16_SINT, true, &s.bwd) ||
            !MakeField(DXGI_FORMAT_R32_UINT, false, &s.fwdCost) ||
            !MakeField(DXGI_FORMAT_R32_UINT, false, &s.bwdCost)) {
            return false;
        }
    }
    if (!direct) {
        for (Luma& l : m_luma) {
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = (UINT)width;
            td.Height = (UINT)height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev->CreateTexture2D(&td, NULL, &l.texture)) ||
                FAILED(dev->CreateRenderTargetView(l.texture, NULL, &l.rtv))) {
                LOGERR("OpticalFlowWarp: a luma frame could not be created");
                return false;
            }
            st = m_of.nvOFRegisterResourceD3D11(m_session, l.texture, &l.handle);
            if (st != NV_OF_SUCCESS) {
                LOGERR("OpticalFlowWarp: a luma frame was not registered (status %d)", (int)st);
                return false;
            }
        }
    }
    if (!CompileShaders()) return false;
    m_ready = true;
    LOG("OpticalFlowWarp: optical flow engine on the present's device, %dx%d, grid 4 MEDIUM, "
        "both ways, cost on, %d flow slots, input %s", width, height, policy::kFlowSlots,
        direct ? "WGC frames as they are (ABGR8, -xofdirect)" : "luma drawn from each frame");
    return true;
}

bool OpticalFlowWarp::MakeField(DXGI_FORMAT format, bool view, Field* out) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)m_cellsX;
    td.Height = (UINT)m_cellsY;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_dev->CreateTexture2D(&td, NULL, &out->texture)) ||
        (view && FAILED(m_dev->CreateShaderResourceView(out->texture, NULL, &out->srv)))) {
        LOGERR("OpticalFlowWarp: a flow field could not be created");
        return false;
    }
    const NV_OF_STATUS st = m_of.nvOFRegisterResourceD3D11(m_session, out->texture, &out->handle);
    if (st != NV_OF_SUCCESS) {
        LOGERR("OpticalFlowWarp: a flow field was not registered (status %d)", (int)st);
        return false;
    }
    return true;
}

bool OpticalFlowWarp::CompileShaders() {
    ID3DBlob* blob = NULL;
    if (!Compile(kVs, strlen(kVs), "OpticalFlowWarpVS", "vs_5_0", &blob)) return false;
    HRESULT hr =
        m_dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &m_vs);
    blob->Release();
    if (FAILED(hr)) return false;
    if (!Compile(kLumaPs, strlen(kLumaPs), "OpticalFlowLumaPS", "ps_5_0", &blob)) return false;
    hr = m_dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &m_lumaPs);
    blob->Release();
    if (FAILED(hr)) return false;
    const std::string warp = FlowWarpShaderSource();
    if (!Compile(warp.c_str(), warp.size(), "FlowWarpPS", "ps_5_0", &blob)) return false;
    hr = m_dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &m_warpPs);
    blob->Release();
    if (FAILED(hr)) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_dev->CreateSamplerState(&sd, &m_linear))) return false;

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = 16;
    const float luma[4] = {1.0f / (float)m_width, 1.0f / (float)m_height, 0.0f, 0.0f};
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = luma;
    if (FAILED(m_dev->CreateBuffer(&bd, &init, &m_lumaCb))) return false;
    bd.ByteWidth = sizeof(FlowWarpParams);
    if (FAILED(m_dev->CreateBuffer(&bd, NULL, &m_warpCb))) return false;
    return true;
}

bool OpticalFlowWarp::DrawLuma(const Frame& frame, Luma* into) {
    if (!frame.srv) return false;
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MaxDepth = 1.0f;
    m_ctx->OMSetRenderTargets(1, &into->rtv, NULL);
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetInputLayout(NULL);
    m_ctx->VSSetShader(m_vs, NULL, 0);
    m_ctx->PSSetShader(m_lumaPs, NULL, 0);
    m_ctx->PSSetShaderResources(0, 1, &frame.srv);
    m_ctx->PSSetSamplers(0, 1, &m_linear);
    m_ctx->PSSetConstantBuffers(0, 1, &m_lumaCb);
    m_ctx->Draw(3, 0);
    // The luma frame is the engine's input next; nothing may still hold it as a target.
    ID3D11ShaderResourceView* noSrv = NULL;
    ID3D11RenderTargetView* noRtv = NULL;
    m_ctx->PSSetShaderResources(0, 1, &noSrv);
    m_ctx->OMSetRenderTargets(1, &noRtv, NULL);
    m_lumaDraws++;
    return true;
}

NvOFGPUBufferHandle OpticalFlowWarp::Registered(ID3D11Texture2D* texture) {
    for (const auto& r : m_registered) {
        if (r.first == texture) return r.second;
    }
    NvOFGPUBufferHandle handle = NULL;
    const NV_OF_STATUS st = m_of.nvOFRegisterResourceD3D11(m_session, texture, &handle);
    if (st != NV_OF_SUCCESS) {
        LOGERR("OpticalFlowWarp: a WGC texture was not registered as engine input (status %d)",
               (int)st);
        return NULL;
    }
    m_registered.push_back(std::make_pair(texture, handle));
    LOG("OpticalFlowWarp: registered WGC texture %zu as engine input", m_registered.size());
    return handle;
}

NvOFGPUBufferHandle OpticalFlowWarp::InputFor(const Frame& frame, int64_t keep) {
    if (m_direct) {
        D3D11_TEXTURE2D_DESC d = {};
        if (frame.texture) frame.texture->GetDesc(&d);
        if (!frame.texture || (int)d.Width != m_width || (int)d.Height != m_height) {
            m_noDirectInput++;
            return NULL;
        }
        return Registered(frame.texture);
    }
    Luma* use = NULL;
    for (Luma& l : m_luma) {
        if (l.valid && l.ts == frame.ts) use = &l;
    }
    if (!use) {
        // An empty luma frame, else the least recently used one that is not the pair's other.
        for (Luma& l : m_luma) {
            if (l.valid && l.ts == keep) continue;
            if (!l.valid) {
                use = &l;
                break;
            }
            if (!use || l.lastUse < use->lastUse) use = &l;
        }
        if (!use || !DrawLuma(frame, use)) return NULL;
        use->ts = frame.ts;
        use->valid = true;
    }
    use->lastUse = ++m_uses;
    return use->handle;
}

bool OpticalFlowWarp::Flow(int slot, const Frame& before, const Frame& after, bool hints) {
    if (!m_ready || slot < 0 || slot >= policy::kFlowSlots) return false;
    const NvOFGPUBufferHandle in0 = InputFor(before, after.ts);
    const NvOFGPUBufferHandle in1 = in0 ? InputFor(after, before.ts) : NULL;
    if (!in0 || !in1) {
        m_flowFailures++;
        return false;
    }
    NV_OF_EXECUTE_INPUT_PARAMS in = {};
    in.inputFrame = in0;
    in.referenceFrame = in1;
    in.disableTemporalHints = hints ? NV_OF_FALSE : NV_OF_TRUE;
    NV_OF_EXECUTE_OUTPUT_PARAMS out = {};
    out.outputBuffer = m_slots[slot].fwd.handle;
    out.outputCostBuffer = m_slots[slot].fwdCost.handle;
    out.bwdOutputBuffer = m_slots[slot].bwd.handle;
    out.bwdOutputCostBuffer = m_slots[slot].bwdCost.handle;
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    const NV_OF_STATUS st = m_of.nvOFExecute(m_session, &in, &out);
    const double ms = MsSince(start);
    if (st != NV_OF_SUCCESS) {
        m_flowFailures++;
        if (m_flowFailures == 1 || (m_flowFailures % 600) == 0) {
            LOGERR("OpticalFlowWarp: a flow was refused (status %d, %lld so far)", (int)st,
                   m_flowFailures);
        }
        return false;
    }
    m_flows++;
    m_submitMsTotal += ms;
    if (ms > m_submitMsMax) m_submitMsMax = ms;
    return true;
}

bool OpticalFlowWarp::Warp(int slot, ID3D11ShaderResourceView* before,
                           ID3D11ShaderResourceView* after, float w, ID3D11RenderTargetView* rtv) {
    if (!m_ready || slot < 0 || slot >= policy::kFlowSlots || !before || !after || !rtv) {
        return false;
    }
    FlowWarpParams params = {};
    params.w = w;
    params.grid = (float)kGrid;
    params.width = (float)m_width;
    params.height = (float)m_height;
    params.cellsX = m_cellsX;
    params.cellsY = m_cellsY;
    m_ctx->UpdateSubresource(m_warpCb, 0, NULL, &params, 0, 0);
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MaxDepth = 1.0f;
    m_ctx->OMSetRenderTargets(1, &rtv, NULL);
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetInputLayout(NULL);
    m_ctx->VSSetShader(m_vs, NULL, 0);
    m_ctx->PSSetShader(m_warpPs, NULL, 0);
    ID3D11ShaderResourceView* srvs[4] = {before, after, m_slots[slot].fwd.srv,
                                         m_slots[slot].bwd.srv};
    m_ctx->PSSetShaderResources(0, 4, srvs);
    m_ctx->PSSetSamplers(0, 1, &m_linear);
    m_ctx->PSSetConstantBuffers(0, 1, &m_warpCb);
    m_ctx->Draw(3, 0);
    // The fields are the engine's output next; nothing may still hold them as inputs.
    ID3D11ShaderResourceView* none[4] = {NULL, NULL, NULL, NULL};
    m_ctx->PSSetShaderResources(0, 4, none);
    m_warps++;
    return true;
}

void OpticalFlowWarp::LogSummary() const {
    if (!m_ready) return;
    LOG("optical flow summary: %lld flows handed to the engine (call returned in %.3f ms mean, "
        "%.3f max), %lld refused or without input, %lld luma draws, %lld warps drawn",
        m_flows, m_flows ? m_submitMsTotal / (double)m_flows : 0.0, m_submitMsMax,
        m_flowFailures, m_lumaDraws, m_warps);
    if (m_direct) {
        LOG("optical flow summary (-xofdirect): %zu WGC textures registered, %lld frames had no "
            "WGC texture at the output's size", m_registered.size(), m_noDirectInput);
    }
}
