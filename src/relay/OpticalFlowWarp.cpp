#include "OpticalFlowWarp.h"
#include "FlowWarpShader.h"

#include <d3dcompiler.h>
#include <string.h>

#include <algorithm>
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
        m_of.nvOFDestroy(m_session);
    }
    for (Slot& s : m_slots) {
        for (Field* f : {&s.fwd, &s.bwd, &s.fwdCost, &s.bwdCost}) {
            if (f->srv) f->srv->Release();
            if (f->texture) f->texture->Release();
        }
        if (s.meansSrv) s.meansSrv->Release();
        if (s.means) s.means->Release();
    }
    for (Luma& l : m_luma) {
        if (l.meanSrv) l.meanSrv->Release();
        if (l.meanRtv) l.meanRtv->Release();
        if (l.mean) l.mean->Release();
        if (l.rtv) l.rtv->Release();
        if (l.texture) l.texture->Release();
    }
    for (MeanRead& r : m_meanReads) {
        if (r.staging) r.staging->Release();
    }
    for (Timing* ring : {m_timings, m_meanTimings}) {
        for (int i = 0; i < kTimings; i++) {
            Timing& t = ring[i];
            if (t.end) t.end->Release();
            if (t.start) t.start->Release();
            if (t.disjoint) t.disjoint->Release();
        }
    }
    if (m_warpCb) m_warpCb->Release();
    if (m_meanCb) m_meanCb->Release();
    if (m_lumaCb) m_lumaCb->Release();
    if (m_linear) m_linear->Release();
    if (m_warpPs) m_warpPs->Release();
    if (m_lumaPs) m_lumaPs->Release();
    if (m_vs) m_vs->Release();
}

bool OpticalFlowWarp::Setup(ID3D11Device* dev, ID3D11DeviceContext* ctx, int width, int height,
                            UINT fieldBinds) {
    m_dev = dev;
    m_ctx = ctx;
    m_width = width;
    m_height = height;
    m_fieldBinds = fieldBinds;
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
    init.enableOutputCost = kCost ? NV_OF_TRUE : NV_OF_FALSE;
    init.predDirection = NV_OF_PRED_DIRECTION_BOTH;
    init.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    st = m_of.nvOFInit(m_session, &init);
    if (st != NV_OF_SUCCESS) {
        LOGERR("OpticalFlowWarp: the engine refused %dx%d grid 4 MEDIUM both ways (status %d)",
               width, height, (int)st);
        return false;
    }

    for (Slot& s : m_slots) {
        if (!MakeField(DXGI_FORMAT_R16G16_SINT, true, &s.fwd) ||
            !MakeField(DXGI_FORMAT_R16G16_SINT, true, &s.bwd)) {
            return false;
        }
        if (kCost && (!MakeField(DXGI_FORMAT_R32_UINT, false, &s.fwdCost) ||
                      !MakeField(DXGI_FORMAT_R32_UINT, false, &s.bwdCost))) {
            return false;
        }
    }
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
    if (!CompileShaders()) return false;
    m_gate = MakeCutGate();
    m_ready = true;
    LOG("OpticalFlowWarp: optical flow engine on the present's device, %dx%d, grid 4 MEDIUM, "
        "both ways, cost %s, %d flow slots, input luma drawn from each frame", width, height,
        kCost ? "on" : "off", policy::kFlowSlots);
    if (m_gate) {
        LOG("OpticalFlowWarp: cut gate on: each frame's mean luma from %dx%d, a pair whose darker "
            "mean (+%.2f) is under %.2f of the brighter's (+%.2f) is drawn as the crossfade",
            kMeanW, kMeanH, (double)flowwarp::kCutFloor, (double)flowwarp::kCutRatio,
            (double)flowwarp::kCutFloor);
    }
    return true;
}

// The cut gate's textures. False, said loudly, leaves every pair a warp: the slots' means stay
// unbound and read 0, never a cut.
bool OpticalFlowWarp::MakeCutGate() {
    UINT support = 0;
    if (FAILED(m_dev->CheckFormatSupport(DXGI_FORMAT_R32_FLOAT, &support)) ||
        !(support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) ||
        !(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET)) {
        LOGERR("OpticalFlowWarp: cut gate OFF: this device cannot generate mips of R32_FLOAT, so "
               "a cut to black is warped like any other pair");
        return false;
    }
    for (Luma& l : m_luma) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)kMeanW;
        td.Height = (UINT)kMeanH;
        td.MipLevels = (UINT)kMeanMips;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = DXGI_FORMAT_R32_FLOAT;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rd.Texture2D.MipSlice = 0;
        if (FAILED(m_dev->CreateTexture2D(&td, NULL, &l.mean)) ||
            FAILED(m_dev->CreateRenderTargetView(l.mean, &rd, &l.meanRtv)) ||
            FAILED(m_dev->CreateShaderResourceView(l.mean, NULL, &l.meanSrv))) {
            LOGERR("OpticalFlowWarp: cut gate OFF: a frame's mean texture could not be created");
            return false;
        }
    }
    for (Slot& s : m_slots) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 2;
        td.Height = 1;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(m_dev->CreateTexture2D(&td, NULL, &s.means)) ||
            FAILED(m_dev->CreateShaderResourceView(s.means, NULL, &s.meansSrv))) {
            LOGERR("OpticalFlowWarp: cut gate OFF: a slot's means could not be created");
            return false;
        }
    }
    for (MeanRead& r : m_meanReads) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 2;
        td.Height = 1;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        // A read-back that cannot be made only goes unlogged.
        if (FAILED(m_dev->CreateTexture2D(&td, NULL, &r.staging))) r.staging = NULL;
    }
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
    // The render target flag is what makes D3D11 order a draw reading the field after the
    // engine's work on it; the field is never drawn into.
    td.BindFlags = m_fieldBinds;
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
    // The same luma shader draws the mean texture, sampling the frame at kMeanW x kMeanH points.
    const float mean[4] = {1.0f / (float)kMeanW, 1.0f / (float)kMeanH, 0.0f, 0.0f};
    init.pSysMem = mean;
    if (FAILED(m_dev->CreateBuffer(&bd, &init, &m_meanCb))) return false;
    bd.ByteWidth = sizeof(FlowWarpParams);
    if (FAILED(m_dev->CreateBuffer(&bd, NULL, &m_warpCb))) return false;

    // Timestamp queries for the warp's and the mean pass's GPU times. Without them those are
    // drawn untimed, so a failure here costs pt= and the summary's times and nothing else.
    D3D11_QUERY_DESC qd = {};
    for (Timing* ring : {m_timings, m_meanTimings}) {
        for (int i = 0; i < kTimings; i++) {
            Timing& t = ring[i];
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            if (FAILED(m_dev->CreateQuery(&qd, &t.disjoint))) t.disjoint = NULL;
            qd.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(m_dev->CreateQuery(&qd, &t.start))) t.start = NULL;
            if (FAILED(m_dev->CreateQuery(&qd, &t.end))) t.end = NULL;
        }
    }
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
    if (m_gate) DrawMean(frame, into);
    return true;
}

void OpticalFlowWarp::DrawMean(const Frame& frame, Luma* into) {
    Timing* timing = NULL;
    for (int i = 0; i < kTimings; i++) {
        Timing& t = m_meanTimings[i];
        if (!t.pending && t.disjoint && t.start && t.end) {
            timing = &t;
            break;
        }
    }
    if (timing) {
        m_ctx->Begin(timing->disjoint);
        m_ctx->End(timing->start);
    } else {
        m_meansUntimed++;
    }
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)kMeanW;
    vp.Height = (float)kMeanH;
    vp.MaxDepth = 1.0f;
    m_ctx->OMSetRenderTargets(1, &into->meanRtv, NULL);
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetInputLayout(NULL);
    m_ctx->VSSetShader(m_vs, NULL, 0);
    m_ctx->PSSetShader(m_lumaPs, NULL, 0);
    m_ctx->PSSetShaderResources(0, 1, &frame.srv);
    m_ctx->PSSetSamplers(0, 1, &m_linear);
    m_ctx->PSSetConstantBuffers(0, 1, &m_meanCb);
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* noSrv = NULL;
    ID3D11RenderTargetView* noRtv = NULL;
    m_ctx->PSSetShaderResources(0, 1, &noSrv);
    m_ctx->OMSetRenderTargets(1, &noRtv, NULL);
    // Box-filters down to 1x1: the last mip is the mean of the kMeanW x kMeanH samples.
    m_ctx->GenerateMips(into->meanSrv);
    if (timing) {
        m_ctx->End(timing->end);
        m_ctx->End(timing->disjoint);
        timing->present = m_lumaDraws;
        timing->pending = true;
    }
}

OpticalFlowWarp::Luma* OpticalFlowWarp::InputFor(const Frame& frame, int64_t keep) {
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
    return use;
}

bool OpticalFlowWarp::Flow(int slot, const Frame& before, const Frame& after, bool hints) {
    if (!m_ready || slot < 0 || slot >= policy::kFlowSlots) return false;
    Luma* l0 = InputFor(before, after.ts);
    Luma* l1 = l0 ? InputFor(after, before.ts) : NULL;
    if (!l0 || !l1) {
        m_flowFailures++;
        return false;
    }
    if (m_gate) {
        // The pair's means beside its fields, for the warp; then a copy toward the CPU, for the
        // log. Both are queued on this context ahead of any warp drawn from the slot.
        Slot& s = m_slots[slot];
        m_ctx->CopySubresourceRegion(s.means, 0, 0, 0, 0, l0->mean, kMeanMips - 1, NULL);
        m_ctx->CopySubresourceRegion(s.means, 0, 1, 0, 0, l1->mean, kMeanMips - 1, NULL);
        s.beforeTs = before.ts;
        s.afterTs = after.ts;
        s.ratio = -1.0f;
        MeanRead* read = NULL;
        for (MeanRead& r : m_meanReads) {
            if (!r.pending && r.staging) {
                read = &r;
                break;
            }
        }
        if (read) {
            m_ctx->CopyResource(read->staging, s.means);
            read->slot = slot;
            read->beforeTs = before.ts;
            read->afterTs = after.ts;
            read->pending = true;
        } else {
            m_meanReadsSkipped++;
        }
    }
    NV_OF_EXECUTE_INPUT_PARAMS in = {};
    in.inputFrame = l0->handle;
    in.referenceFrame = l1->handle;
    in.disableTemporalHints = hints ? NV_OF_FALSE : NV_OF_TRUE;
    NV_OF_EXECUTE_OUTPUT_PARAMS out = {};
    out.outputBuffer = m_slots[slot].fwd.handle;
    // NULL with kCost off: the cost fields were never created.
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
                           ID3D11ShaderResourceView* after, float w, ID3D11RenderTargetView* rtv,
                           long long present) {
    if (!m_ready || slot < 0 || slot >= policy::kFlowSlots || !before || !after || !rtv) {
        return false;
    }
    Timing* timing = NULL;
    for (Timing& t : m_timings) {
        if (!t.pending && t.disjoint && t.start && t.end) {
            timing = &t;
            break;
        }
    }
    if (timing) {
        m_ctx->Begin(timing->disjoint);
        m_ctx->End(timing->start);
    } else {
        m_warpsUntimed++;
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
    const Slot& s = m_slots[slot];
    ID3D11ShaderResourceView* srvs[5] = {before, after, s.fwd.srv, s.bwd.srv,
                                         m_gate ? s.meansSrv : NULL};
    m_ctx->PSSetShaderResources(0, 5, srvs);
    m_ctx->PSSetSamplers(0, 1, &m_linear);
    m_ctx->PSSetConstantBuffers(0, 1, &m_warpCb);
    m_ctx->Draw(3, 0);
    if (timing) {
        m_ctx->End(timing->end);
        m_ctx->End(timing->disjoint);
        timing->present = present;
        timing->pending = true;
    }
    // The fields are the engine's output next; nothing may still hold them as inputs.
    ID3D11ShaderResourceView* none[5] = {NULL, NULL, NULL, NULL, NULL};
    m_ctx->PSSetShaderResources(0, 5, none);
    m_warps++;
    if (m_gate) {
        if (s.ratio < 0.0f) m_warpsRatioUnread++;
        else if (s.ratio < flowwarp::kCutRatio) m_warpsCut++;
    }
    return true;
}

float OpticalFlowWarp::PairRatio(int slot) {
    if (!m_gate || slot < 0 || slot >= policy::kFlowSlots) return -1.0f;
    PollMeans();
    return m_slots[slot].ratio;
}

void OpticalFlowWarp::PollMeans() {
    if (!m_gate) return;
    for (MeanRead& r : m_meanReads) {
        if (!r.pending) continue;
        D3D11_MAPPED_SUBRESOURCE m = {};
        const HRESULT hr = m_ctx->Map(r.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        r.pending = false;
        if (FAILED(hr)) {
            m_meanReadsFailed++;
            continue;
        }
        const float m0 = ((const float*)m.pData)[0];
        const float m1 = ((const float*)m.pData)[1];
        m_ctx->Unmap(r.staging, 0);
        const float ratio = flowwarp::MeanRatio(m0, m1);
        m_ratios.push_back(ratio);
        if (ratio < flowwarp::kCutRatio) {
            m_cuts++;
            if (m_cuts <= 500 || (m_cuts % 100) == 0) {
                LOG("optical flow cut: a flowed pair's mean luma %.3f and %.3f, ratio %.3f under "
                    "%.2f: its warps are drawn as the crossfade (%lld so far)", (double)m0,
                    (double)m1, (double)ratio, (double)flowwarp::kCutRatio, m_cuts);
            }
        }
        // The slot may hold a later pair by now; the ratio belongs to the pair it was read for.
        Slot& s = m_slots[r.slot];
        if (s.beforeTs == r.beforeTs && s.afterTs == r.afterTs) s.ratio = ratio;
    }
    long long unused = 0;
    int ago = 0;
    ReadTimings(m_meanTimings, &m_meanUs, 0, &unused, &ago);
}

bool OpticalFlowWarp::TakeWarpTime(long long present, long long* us, int* presentsAgo) {
    PollMeans();
    return ReadTimings(m_timings, &m_warpUs, present, us, presentsAgo);
}

bool OpticalFlowWarp::ReadTimings(Timing* timings, std::vector<float>* out, long long present,
                                  long long* us, int* presentsAgo) {
    bool any = false;
    long long newest = -1;
    for (int i = 0; i < kTimings; i++) {
        Timing& t = timings[i];
        if (!t.pending) continue;
        // DONOTFLUSH: asking must never push work to the GPU or wait for it.
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
        if (m_ctx->GetData(t.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            continue;
        }
        UINT64 t0 = 0, t1 = 0;
        if (m_ctx->GetData(t.start, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            m_ctx->GetData(t.end, &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            continue;
        }
        t.pending = false;
        // A disjoint interval (a clock change mid-way) leaves this draw untimed.
        if (dj.Disjoint || dj.Frequency == 0 || t1 < t0) continue;
        // Whole microseconds, as pt= has always been, except for the mean pass, which can take
        // only a few: its summary keeps the fraction.
        const long long value = (long long)((t1 - t0) * 1000000ULL / dj.Frequency);
        if (timings == m_meanTimings) {
            out->push_back((float)((double)(t1 - t0) * 1e6 / (double)dj.Frequency));
        } else {
            out->push_back((float)value);
        }
        if (t.present > newest) {
            newest = t.present;
            *us = value;
            *presentsAgo = (int)(present - t.present);
            any = true;
        }
    }
    return any;
}

void OpticalFlowWarp::LogSummary() const {
    if (!m_ready) return;
    LOG("optical flow summary: %lld flows handed to the engine (call returned in %.3f ms mean, "
        "%.3f max), %lld refused or without input, %lld luma draws, %lld warps drawn",
        m_flows, m_flows ? m_submitMsTotal / (double)m_flows : 0.0, m_submitMsMax,
        m_flowFailures, m_lumaDraws, m_warps);
    std::vector<float> us = m_warpUs;
    std::sort(us.begin(), us.end());
    auto at = [&](double q) {
        return us.empty() ? 0.0 : (double)us[(size_t)(q * (double)(us.size() - 1) + 0.5)] / 1000.0;
    };
    LOG("optical flow warp GPU time (pt=): p50 %.3f p99 %.3f max %.3f ms over %zu warps timed, "
        "%lld drawn untimed", at(0.5), at(0.99), at(1.0), us.size(), m_warpsUntimed);
    if (!m_gate) {
        LOG("optical flow cut gate: OFF (see the setup line); every pair was warped");
        return;
    }
    std::vector<float> mean = m_meanUs;
    std::sort(mean.begin(), mean.end());
    auto meanAt = [&](double q) {
        return mean.empty() ? 0.0 : (double)mean[(size_t)(q * (double)(mean.size() - 1) + 0.5)];
    };
    LOG("optical flow cut gate, mean pass GPU time: p50 %.1f p99 %.1f max %.1f us over %zu "
        "frames timed, %lld drawn untimed", meanAt(0.5), meanAt(0.99), meanAt(1.0), mean.size(),
        m_meansUntimed);
    std::vector<float> r = m_ratios;
    std::sort(r.begin(), r.end());
    auto ratioAt = [&](double q) {
        return r.empty() ? 0.0 : (double)r[(size_t)(q * (double)(r.size() - 1) + 0.5)];
    };
    long long nearCut = 0;
    for (float v : r) {
        if (v >= flowwarp::kCutRatio && v < flowwarp::kCutRatio + 0.2f) nearCut++;
    }
    LOG("optical flow cut gate: %zu pairs read back (%lld not read: no read-back free; %lld "
        "failed), %lld cuts under %.2f; darker-over-brighter mean ratio min %.3f p0.1 %.3f p1 "
        "%.3f p50 %.3f, %lld pairs within 0.2 above the cut; warps drawn over a known cut %lld, "
        "drawn before their pair was read %lld", r.size(), m_meanReadsSkipped, m_meanReadsFailed,
        m_cuts, (double)flowwarp::kCutRatio, ratioAt(0.0), ratioAt(0.001), ratioAt(0.01),
        ratioAt(0.5), nearCut, m_warpsCut, m_warpsRatioUnread);
}
