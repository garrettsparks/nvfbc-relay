#include "BlendShaders.h"

#include <d3dcompiler.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <string>

#include <SimpleLogger.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace {

// Fullscreen triangle from the vertex id; the pixel shaders work from SV_Position.
const char* kVs =
    "void main(uint id : SV_VertexID, out float4 pos : SV_Position) {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n";

// The blend pixel shaders share their inputs: the two frames at the output's size, the weight,
// and for the pan shaders the shift and a bilinear sampler.
const char* kBlendHead =
    "Texture2D texA : register(t0);\n"
    "Texture2D texB : register(t1);\n"
    "Texture2D<float4> shiftTex : register(t2);\n"
    "SamplerState lin : register(s0);\n"
    "cbuffer Blend : register(b0) { float w; float pad; float2 invSize; };\n"
    "static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);\n";

const char* kPlainPs =
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "    int3 p = int3(pos.xy, 0);\n"
    "    return float4(lerp(texA.Load(p).rgb, texB.Load(p).rgb, w), 1.0);\n"
    "}\n";

// sRGB's own curve each way, not a power of 2.2, so a frame mixed with itself comes back as it
// went in.
const char* kLinearPs =
    "float3 ToLinear(float3 c) {\n"
    "    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);\n"
    "}\n"
    "float3 ToSrgb(float3 c) {\n"
    "    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;\n"
    "}\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "    int3 p = int3(pos.xy, 0);\n"
    "    float3 a = ToLinear(saturate(texA.Load(p).rgb));\n"
    "    float3 b = ToLinear(saturate(texB.Load(p).rgb));\n"
    "    return float4(ToSrgb(lerp(a, b, w)), 1.0);\n"
    "}\n";

// The content moved by s from a to b (b(x) = a(x - s)) sits at x - w s in a and at x + (1 - w) s
// in b at time w. Each pixel takes that moved mix only where its two samples agree clearly
// better than the plain mix's two do, by 0.01 to 0.05 of luma, and keeps the plain mix
// otherwise: on a tie the plain one, since a static overlay agrees with itself unmoved, and a
// pattern in it can happen to agree with itself moved as well (the probe's striped overlay did,
// and a 50/50 mix there drew half of it shifted). A moved sample off either frame leaves the
// plain mix.
const char* kPanPs =
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "    int3 p = int3(pos.xy, 0);\n"
    "    float3 a = texA.Load(p).rgb;\n"
    "    float3 b = texB.Load(p).rgb;\n"
    "    float3 plain = lerp(a, b, w);\n"
    "    float4 s = shiftTex.Load(int3(0, 0, 0));\n"
    "    if (s.z < 0.5) return float4(plain, 1.0);\n"
    "    float2 size = 1.0 / invSize;\n"
    "    float2 pa = pos.xy - w * s.xy;\n"
    "    float2 pb = pos.xy + (1.0 - w) * s.xy;\n"
    "    if (any(pa < 0.0) || any(pa > size) || any(pb < 0.0) || any(pb > size)) {\n"
    "        return float4(plain, 1.0);\n"
    "    }\n"
    "    float3 ma = texA.SampleLevel(lin, pa * invSize, 0).rgb;\n"
    "    float3 mb = texB.SampleLevel(lin, pb * invSize, 0).rgb;\n"
    "    float dMoved = abs(dot(ma - mb, kLuma));\n"
    "    float dPlain = abs(dot(a - b, kLuma));\n"
    "    float t = smoothstep(0.01, 0.05, dPlain - dMoved);\n"
    "    return float4(lerp(plain, lerp(ma, mb, w), t), 1.0);\n"
    "}\n";

// Luma at a quarter of the source's size each way: each output texel the mean of the 4x4 block
// under it, four bilinear samples at the corners between its 2x2 quarters.
const char* kDownPs =
    "Texture2D src : register(t0);\n"
    "SamplerState lin : register(s0);\n"
    "cbuffer Down : register(b0) { float2 srcInvSize; float color; float pad; };\n"
    "float main(float4 pos : SV_Position) : SV_Target {\n"
    "    float2 c = floor(pos.xy) * 4.0 + 2.0;\n"
    "    float sum = 0.0;\n"
    "    [unroll] for (int j = 0; j < 4; j++) {\n"
    "        float2 o = float2((j & 1) ? 1.0 : -1.0, (j & 2) ? 1.0 : -1.0);\n"
    "        float4 v = src.SampleLevel(lin, (c + o) * srcInvSize, 0);\n"
    "        sum += color > 0.5 ? dot(v.rgb, float3(0.2126, 0.7152, 0.0722)) : v.r;\n"
    "    }\n"
    "    return sum * 0.25;\n"
    "}\n";

const char* kStageHead =
    "cbuffer Stage : register(b0) {\n"
    "    int2 size; int radius; int stride; int scale; int color; int zero; int last;\n"
    "};\n"
    "Texture2D<float4> prevShift : register(t2);\n"
    "int2 Centre() {\n"
    "    return scale > 0 ? int2(round(prevShift.Load(int3(0, 0, 0)).xy * scale)) : int2(0, 0);\n"
    "}\n";

// One group per candidate shift: the mean luma difference between b and a moved by it, over
// every stride-th texel where both are inside the frame, or a huge cost when less than a
// quarter of the texels overlap.
const char* kSearchCs =
    "Texture2D<float4> srcA : register(t0);\n"
    "Texture2D<float4> srcB : register(t1);\n"
    "RWTexture2D<float> costs : register(u0);\n"
    "groupshared float gsSum[256];\n"
    "groupshared float gsN[256];\n"
    "float Lum(Texture2D<float4> t, int2 p) {\n"
    "    float4 c = t.Load(int3(p, 0));\n"
    "    return color != 0 ? dot(c.rgb, float3(0.2126, 0.7152, 0.0722)) : c.r;\n"
    "}\n"
    "[numthreads(16, 16, 1)]\n"
    "void main(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID, uint gi : SV_GroupIndex) {\n"
    "    int side = 2 * radius + 1;\n"
    "    int k = (int)g.x;\n"
    "    int2 s = (zero != 0 && k == side * side) ? int2(0, 0)\n"
    "                                             : Centre() + int2(k % side, k / side) - radius;\n"
    "    float sum = 0.0, n = 0.0;\n"
    "    for (int y = (int)t.y * stride; y < size.y; y += 16 * stride) {\n"
    "        for (int x = (int)t.x * stride; x < size.x; x += 16 * stride) {\n"
    "            int2 pa = int2(x, y) - s;\n"
    "            if (any(pa < 0) || any(pa >= size)) continue;\n"
    "            sum += abs(Lum(srcB, int2(x, y)) - Lum(srcA, pa));\n"
    "            n += 1.0;\n"
    "        }\n"
    "    }\n"
    "    gsSum[gi] = sum;\n"
    "    gsN[gi] = n;\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    for (uint h = 128; h > 0; h >>= 1) {\n"
    "        if (gi < h) { gsSum[gi] += gsSum[gi + h]; gsN[gi] += gsN[gi + h]; }\n"
    "        GroupMemoryBarrierWithGroupSync();\n"
    "    }\n"
    "    if (gi == 0) {\n"
    "        float points = (float)((size.x + stride - 1) / stride) *\n"
    "                       (float)((size.y + stride - 1) / stride);\n"
    "        costs[int2(k, 0)] = gsN[0] >= 0.25 * points ? gsSum[0] / gsN[0] : 1e9;\n"
    "    }\n"
    "}\n";

// The least cost among the candidates, its shift written as the step's result. The last step
// also refines the fraction with a parabola through the best cost and its neighbours each way,
// and follows the shift only when it beats no shift (the extra candidate) by a fifth and moves
// a quarter pixel or more; its fourth value is then the best cost over no shift's (the rule
// follows below 0.8), the other steps' their best cost.
const char* kPickCs =
    "Texture2D<float> costs : register(t0);\n"
    "RWTexture2D<float4> outShift : register(u0);\n"
    "groupshared float gsC[256];\n"
    "groupshared uint gsI[256];\n"
    "float Cost(int i) { return costs.Load(int3(i, 0, 0)); }\n"
    "[numthreads(256, 1, 1)]\n"
    "void main(uint gi : SV_GroupIndex) {\n"
    "    int side = 2 * radius + 1;\n"
    "    int n = side * side;\n"
    "    float best = 3.4e38;\n"
    "    uint bi = 0;\n"
    "    for (int i = (int)gi; i < n; i += 256) {\n"
    "        float c = Cost(i);\n"
    "        if (c < best) { best = c; bi = (uint)i; }\n"
    "    }\n"
    "    gsC[gi] = best;\n"
    "    gsI[gi] = bi;\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    for (uint h = 128; h > 0; h >>= 1) {\n"
    "        if (gi < h && (gsC[gi + h] < gsC[gi] ||\n"
    "                       (gsC[gi + h] == gsC[gi] && gsI[gi + h] < gsI[gi]))) {\n"
    "            gsC[gi] = gsC[gi + h];\n"
    "            gsI[gi] = gsI[gi + h];\n"
    "        }\n"
    "        GroupMemoryBarrierWithGroupSync();\n"
    "    }\n"
    "    if (gi != 0) return;\n"
    "    int b = (int)gsI[0];\n"
    "    int bx = b % side, by = b / side;\n"
    "    float2 s = float2(Centre() + int2(bx, by) - radius);\n"
    "    float follow = 1.0;\n"
    "    float kept = gsC[0];\n"
    "    if (last != 0) {\n"
    "        if (bx > 0 && bx < side - 1) {\n"
    "            float l = Cost(b - 1), r = Cost(b + 1), d = l - 2.0 * gsC[0] + r;\n"
    "            if (d > 0.0) s.x += clamp(0.5 * (l - r) / d, -0.5, 0.5);\n"
    "        }\n"
    "        if (by > 0 && by < side - 1) {\n"
    "            float u = Cost(b - side), v = Cost(b + side), d = u - 2.0 * gsC[0] + v;\n"
    "            if (d > 0.0) s.y += clamp(0.5 * (u - v) / d, -0.5, 0.5);\n"
    "        }\n"
    "        float none = Cost(n);\n"
    "        // A best match near the coarse search's reach (16 texels of 1/16, 256 pixels) may\n"
    "        // be cut short by it, and is what a scene cut finds: never followed.\n"
    "        bool inReach = all(abs(s) < 240.0);\n"
    "        follow = (gsC[0] < 0.8 * none && length(s) >= 0.25 && inReach) ? 1.0 : 0.0;\n"
    "        // How well the shift matched against no shift at all, the rule's ratio.\n"
    "        kept = none > 0.0 ? gsC[0] / none : 1.0;\n"
    "    }\n"
    "    outShift[int2(0, 0)] = float4(s, follow, kept);\n"
    "}\n";

// The first step's search on the 1/16 copy reaches this many texels each way (256 pixels). The
// pick's 240-pixel limit on following (kPickCs) sits inside it.
const int kCoarseRadius = 16;
// The steps after it search around the step before, whose whole texel is off by half of one at
// most: 2 texels on the 1/4 copy, 2 pixels at full size; each searches one more for slack.
const int kMidRadius = 3;
const int kFineRadius = 3;
// Every this many texels (pixels at full size) each way are compared, at each step. The costs
// are the probe's (ofprobe, per step): with every 2nd, 2nd and 8th the steps took 0.016, 0.054
// and 0.094 ms idle at 2560x1440, in proportion to the texels compared.
const int kCoarseStride = 4;
const int kMidStride = 4;
const int kFineStride = 16;
// Candidates the cost row holds: the first step's, the largest.
const int kMaxCandidates = (2 * kCoarseRadius + 1) * (2 * kCoarseRadius + 1) + 1;

double MsFor(UINT64 t0, UINT64 t1, UINT64 freq) {
    return 1000.0 * (double)(t1 - t0) / (double)freq;
}

}  // namespace

const char* BlendShaders::Name(int shader) {
    static const char* const kNames[] = { "linear", "pan", "plain" };
    return shader >= Linear && shader <= Plain ? kNames[shader] : "?";
}

BlendShaders::~BlendShaders() {
    for (Tex* t : {&m_a1, &m_b1, &m_a2, &m_b2, &m_costs, &m_coarse, &m_mid, &m_shift}) {
        FreeTex(t);
    }
    for (Timing& t : m_timings) {
        if (t.shift) t.shift->Release();
        if (t.end) t.end->Release();
        if (t.start) t.start->Release();
        if (t.disjoint) t.disjoint->Release();
    }
    if (m_pickCs) m_pickCs->Release();
    if (m_searchCs) m_searchCs->Release();
    if (m_stageCb) m_stageCb->Release();
    if (m_downCb) m_downCb->Release();
    if (m_blendCb) m_blendCb->Release();
    if (m_linear) m_linear->Release();
    if (m_downPs) m_downPs->Release();
    if (m_blendPs) m_blendPs->Release();
    if (m_vs) m_vs->Release();
}

bool BlendShaders::MakeTex(Tex* t, int w, int h, DXGI_FORMAT format, bool rt, bool uav) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rt ? D3D11_BIND_RENDER_TARGET : 0) |
                   (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
    if (FAILED(m_dev->CreateTexture2D(&td, NULL, &t->tex)) ||
        FAILED(m_dev->CreateShaderResourceView(t->tex, NULL, &t->srv)) ||
        (rt && FAILED(m_dev->CreateRenderTargetView(t->tex, NULL, &t->rtv))) ||
        (uav && FAILED(m_dev->CreateUnorderedAccessView(t->tex, NULL, &t->uav)))) {
        LOGERR("BlendShaders: a %dx%d working texture could not be created", w, h);
        return false;
    }
    return true;
}

void BlendShaders::FreeTex(Tex* t) {
    if (t->uav) t->uav->Release();
    if (t->rtv) t->rtv->Release();
    if (t->srv) t->srv->Release();
    if (t->tex) t->tex->Release();
    *t = Tex();
}

bool BlendShaders::CompilePs(const char* src, const char* name, ID3D11PixelShader** out) {
    ID3DBlob* blob = NULL;
    ID3DBlob* err = NULL;
    const HRESULT hr = D3DCompile(src, strlen(src), name, NULL, NULL, "main", "ps_5_0", 0, 0,
                                  &blob, &err);
    if (FAILED(hr)) {
        LOGERR("BlendShaders: %s failed to compile (0x%08x): %s", name, (unsigned)hr,
               err ? (const char*)err->GetBufferPointer() : "(no message)");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    const HRESULT created =
        m_dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, out);
    blob->Release();
    return SUCCEEDED(created);
}

bool BlendShaders::CompileCs(const char* src, const char* name, ID3D11ComputeShader** out) {
    ID3DBlob* blob = NULL;
    ID3DBlob* err = NULL;
    const HRESULT hr = D3DCompile(src, strlen(src), name, NULL, NULL, "main", "cs_5_0", 0, 0,
                                  &blob, &err);
    if (FAILED(hr)) {
        LOGERR("BlendShaders: %s failed to compile (0x%08x): %s", name, (unsigned)hr,
               err ? (const char*)err->GetBufferPointer() : "(no message)");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    const HRESULT created =
        m_dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, out);
    blob->Release();
    return SUCCEEDED(created);
}

bool BlendShaders::Setup(ID3D11Device* dev, ID3D11DeviceContext* ctx, int shader, int width,
                         int height) {
    m_dev = dev;
    m_ctx = ctx;
    m_shader = shader;
    m_width = width;
    m_height = height;
    m_w1 = width / 4;
    m_h1 = height / 4;
    m_w2 = m_w1 / 4;
    m_h2 = m_h1 / 4;

    ID3DBlob* blob = NULL;
    if (FAILED(D3DCompile(kVs, strlen(kVs), "BlendShadersVS", NULL, NULL, "main", "vs_5_0", 0, 0,
                          &blob, NULL)) ||
        FAILED(m_dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL,
                                         &m_vs))) {
        if (blob) blob->Release();
        LOGERR("BlendShaders: the vertex shader could not be readied");
        return false;
    }
    blob->Release();

    const char* body = kPlainPs;
    if (shader == Linear) body = kLinearPs;
    else if (shader == Pan) body = kPanPs;
    const std::string blendSrc = std::string(kBlendHead) + body;
    if (!CompilePs(blendSrc.c_str(), "BlendShadersPS", &m_blendPs)) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_dev->CreateSamplerState(&sd, &m_linear))) return false;

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_dev->CreateBuffer(&bd, NULL, &m_blendCb)) ||
        FAILED(m_dev->CreateBuffer(&bd, NULL, &m_downCb))) {
        return false;
    }
    bd.ByteWidth = sizeof(Stage);
    if (FAILED(m_dev->CreateBuffer(&bd, NULL, &m_stageCb))) return false;

    // The shift texture exists for every shader, so the pan shader's blend always has one to
    // read and the probe one to look at; the others never write it.
    if (!MakeTex(&m_shift, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, false, true)) return false;
    ForceShift(0.0f, 0.0f);

    if (shader == Pan) {
        const std::string head = kStageHead;
        if (!CompilePs(kDownPs, "BlendShadersDownPS", &m_downPs) ||
            !CompileCs((head + kSearchCs).c_str(), "BlendShadersSearchCS", &m_searchCs) ||
            !CompileCs((head + kPickCs).c_str(), "BlendShadersPickCS", &m_pickCs) ||
            !MakeTex(&m_a1, m_w1, m_h1, DXGI_FORMAT_R16_FLOAT, true, false) ||
            !MakeTex(&m_b1, m_w1, m_h1, DXGI_FORMAT_R16_FLOAT, true, false) ||
            !MakeTex(&m_a2, m_w2, m_h2, DXGI_FORMAT_R16_FLOAT, true, false) ||
            !MakeTex(&m_b2, m_w2, m_h2, DXGI_FORMAT_R16_FLOAT, true, false) ||
            !MakeTex(&m_costs, kMaxCandidates, 1, DXGI_FORMAT_R32_FLOAT, false, true) ||
            !MakeTex(&m_coarse, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, false, true) ||
            !MakeTex(&m_mid, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, false, true)) {
            return false;
        }
    }
    D3D11_QUERY_DESC qd = {};
    D3D11_TEXTURE2D_DESC sd2 = {};
    m_shift.tex->GetDesc(&sd2);
    sd2.Usage = D3D11_USAGE_STAGING;
    sd2.BindFlags = 0;
    sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (Timing& t : m_timings) {
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(m_dev->CreateQuery(&qd, &t.disjoint))) t.disjoint = NULL;
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(m_dev->CreateQuery(&qd, &t.start))) t.start = NULL;
        if (FAILED(m_dev->CreateQuery(&qd, &t.end))) t.end = NULL;
        // Without it the pan shader's shift goes unread (ps=), and nothing else.
        if (shader == Pan && FAILED(m_dev->CreateTexture2D(&sd2, NULL, &t.shift))) t.shift = NULL;
    }
    m_ready = true;
    LOG("BlendShaders: -blend %s, %dx%d%s", Name(shader), width, height,
        shader == Pan ? ", the shift found on luma at 1/16, 1/4 and full size" : "");
    return true;
}

void BlendShaders::ForceShift(float x, float y) {
    if (!m_shift.tex) return;
    const float v[4] = {x, y, (x != 0.0f || y != 0.0f) ? 1.0f : 0.0f, 0.0f};
    m_ctx->UpdateSubresource(m_shift.tex, 0, NULL, v, sizeof(v), sizeof(v));
}

void BlendShaders::SetStage(const Stage& s) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(m_ctx->Map(m_stageCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &s, sizeof(s));
        m_ctx->Unmap(m_stageCb, 0);
    }
}

void BlendShaders::RunCs(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* t0,
                         ID3D11ShaderResourceView* t1, ID3D11ShaderResourceView* t2,
                         ID3D11UnorderedAccessView* u0, UINT x, UINT y, UINT z) {
    ID3D11ShaderResourceView* srvs[3] = {t0, t1, t2};
    m_ctx->CSSetShader(cs, NULL, 0);
    m_ctx->CSSetConstantBuffers(0, 1, &m_stageCb);
    m_ctx->CSSetShaderResources(0, 3, srvs);
    m_ctx->CSSetUnorderedAccessViews(0, 1, &u0, NULL);
    m_ctx->Dispatch(x, y, z);
    // What was written is read next, as a shader resource: nothing may still hold it.
    ID3D11ShaderResourceView* noSrvs[3] = {NULL, NULL, NULL};
    ID3D11UnorderedAccessView* noUav = NULL;
    m_ctx->CSSetShaderResources(0, 3, noSrvs);
    m_ctx->CSSetUnorderedAccessViews(0, 1, &noUav, NULL);
}

void BlendShaders::Downscale(ID3D11ShaderResourceView* src, int srcW, int srcH, bool color,
                             const Tex& dst, int dstW, int dstH) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(m_ctx->Map(m_downCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float* p = (float*)m.pData;
        p[0] = 1.0f / (float)srcW;
        p[1] = 1.0f / (float)srcH;
        p[2] = color ? 1.0f : 0.0f;
        p[3] = 0.0f;
        m_ctx->Unmap(m_downCb, 0);
    }
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)dstW;
    vp.Height = (float)dstH;
    vp.MaxDepth = 1.0f;
    m_ctx->OMSetRenderTargets(1, &dst.rtv, NULL);
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetInputLayout(NULL);
    m_ctx->VSSetShader(m_vs, NULL, 0);
    m_ctx->PSSetShader(m_downPs, NULL, 0);
    m_ctx->PSSetShaderResources(0, 1, &src);
    m_ctx->PSSetSamplers(0, 1, &m_linear);
    m_ctx->PSSetConstantBuffers(0, 1, &m_downCb);
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* noSrv = NULL;
    ID3D11RenderTargetView* noRtv = NULL;
    m_ctx->PSSetShaderResources(0, 1, &noSrv);
    m_ctx->OMSetRenderTargets(1, &noRtv, NULL);
}

void BlendShaders::Search(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b,
                          ID3D11ShaderResourceView* prev, const Stage& s, const Tex& out) {
    SetStage(s);
    const int side = 2 * s.radius + 1;
    RunCs(m_searchCs, a, b, prev, m_costs.uav, (UINT)(side * side + (s.zero ? 1 : 0)), 1, 1);
    // The pick reads its centre from the same previous shift, in t2.
    RunCs(m_pickCs, m_costs.srv, NULL, prev, out.uav, 1, 1, 1);
}

bool BlendShaders::Estimate(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b) {
    if (!m_ready || !a || !b) return false;
    if (m_shader != Pan) return true;
    Downscale(a, m_width, m_height, true, m_a1, m_w1, m_h1);
    Downscale(b, m_width, m_height, true, m_b1, m_w1, m_h1);
    Downscale(m_a1.srv, m_w1, m_h1, false, m_a2, m_w2, m_h2);
    Downscale(m_b1.srv, m_w1, m_h1, false, m_b2, m_w2, m_h2);
    if (m_stepStamps) m_ctx->End(m_stepStamps[0]);

    Stage s = {};
    s.sizeX = m_w2;
    s.sizeY = m_h2;
    s.radius = kCoarseRadius;
    s.stride = kCoarseStride;
    Search(m_a2.srv, m_b2.srv, NULL, s, m_coarse);
    if (m_stepStamps) m_ctx->End(m_stepStamps[1]);

    s = Stage();
    s.sizeX = m_w1;
    s.sizeY = m_h1;
    s.radius = kMidRadius;
    s.stride = kMidStride;
    s.scale = 4;
    Search(m_a1.srv, m_b1.srv, m_coarse.srv, s, m_mid);
    if (m_stepStamps) m_ctx->End(m_stepStamps[2]);

    s = Stage();
    s.sizeX = m_width;
    s.sizeY = m_height;
    s.radius = kFineRadius;
    s.stride = kFineStride;
    s.scale = 4;
    s.color = 1;
    s.zero = 1;
    s.last = 1;
    Search(a, b, m_mid.srv, s, m_shift);
    if (m_stepStamps) m_ctx->End(m_stepStamps[3]);
    return true;
}

bool BlendShaders::Blend(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, float w,
                         ID3D11RenderTargetView* rtv) {
    if (!m_ready || !a || !b || !rtv) return false;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(m_ctx->Map(m_blendCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float* p = (float*)m.pData;
        p[0] = w;
        p[1] = 0.0f;
        p[2] = 1.0f / (float)m_width;
        p[3] = 1.0f / (float)m_height;
        m_ctx->Unmap(m_blendCb, 0);
    }
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MaxDepth = 1.0f;
    ID3D11ShaderResourceView* srvs[3] = {a, b, m_shift.srv};
    m_ctx->OMSetRenderTargets(1, &rtv, NULL);
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetInputLayout(NULL);
    m_ctx->VSSetShader(m_vs, NULL, 0);
    m_ctx->PSSetShader(m_blendPs, NULL, 0);
    m_ctx->PSSetShaderResources(0, 3, srvs);
    m_ctx->PSSetSamplers(0, 1, &m_linear);
    m_ctx->PSSetConstantBuffers(0, 1, &m_blendCb);
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none[3] = {NULL, NULL, NULL};
    m_ctx->PSSetShaderResources(0, 3, none);
    return true;
}

bool BlendShaders::Draw(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, float w,
                        ID3D11RenderTargetView* rtv, long long present) {
    if (!m_ready) return false;
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
        m_untimed++;
    }
    const bool drawn = Estimate(a, b) && Blend(a, b, w, rtv);
    if (timing) {
        // Before the end query, so the shift is copied by the time the time can be read.
        if (timing->shift) m_ctx->CopyResource(timing->shift, m_shift.tex);
        m_ctx->End(timing->end);
        m_ctx->End(timing->disjoint);
        timing->present = present;
        timing->pending = true;
    }
    m_draws++;
    return drawn;
}

bool BlendShaders::TakeTime(long long present, long long* us, int* presentsAgo, int* follow,
                            float* shiftX, float* shiftY, float* ratio) {
    bool any = false;
    long long newest = -1;
    *follow = -1;
    for (Timing& t : m_timings) {
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
        // The pan shader's shift, copied before the end query, so there by now; never waited for.
        int followed = -1;
        float sx = 0.0f, sy = 0.0f, r = 0.0f;
        D3D11_MAPPED_SUBRESOURCE m;
        if (t.shift && SUCCEEDED(m_ctx->Map(t.shift, 0, D3D11_MAP_READ,
                                            D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
            const float* v = (const float*)m.pData;
            followed = v[2] > 0.5f ? 1 : 0;
            sx = v[0];
            sy = v[1];
            r = v[3];
            m_ctx->Unmap(t.shift, 0);
            m_shiftsRead++;
            m_ratios.push_back(r);
            if (followed) {
                m_shiftsFollowed++;
                m_shiftLengths.push_back(std::sqrt(sx * sx + sy * sy));
            }
        }
        // A disjoint interval (a clock change mid-way) leaves this draw untimed.
        if (dj.Disjoint || dj.Frequency == 0 || t1 < t0) continue;
        const long long value = (long long)(MsFor(t0, t1, dj.Frequency) * 1000.0);
        m_us.push_back((float)value);
        if (t.present > newest) {
            newest = t.present;
            *us = value;
            *presentsAgo = (int)(present - t.present);
            *follow = followed;
            *shiftX = sx;
            *shiftY = sy;
            *ratio = r;
            any = true;
        }
    }
    return any;
}

void BlendShaders::LogSummary() const {
    if (!m_ready) return;
    std::vector<float> us = m_us;
    std::sort(us.begin(), us.end());
    auto at = [&](double q) {
        return us.empty() ? 0.0 : (double)us[(size_t)(q * (double)(us.size() - 1) + 0.5)] / 1000.0;
    };
    LOG("blend shader %s GPU time (pt=): p50 %.3f p99 %.3f max %.3f ms over %zu draws timed, "
        "%lld drawn untimed, %lld drawn", Name(m_shader), at(0.5), at(0.99), at(1.0), us.size(),
        m_untimed, m_draws);
    if (m_shader != Pan) return;
    std::vector<float> len = m_shiftLengths, ratios = m_ratios;
    std::sort(len.begin(), len.end());
    std::sort(ratios.begin(), ratios.end());
    auto pct = [](const std::vector<float>& v, double q) {
        return v.empty() ? 0.0 : (double)v[(size_t)(q * (double)(v.size() - 1) + 0.5)];
    };
    LOG("blend shader pan shifts (ps=): followed in %lld of %lld read (%.1f%%); followed shift "
        "p50 %.1f p90 %.1f max %.1f px; best match over no shift (followed below 0.8) p10 %.2f "
        "p50 %.2f p90 %.2f", m_shiftsFollowed, m_shiftsRead,
        m_shiftsRead ? 100.0 * (double)m_shiftsFollowed / (double)m_shiftsRead : 0.0,
        pct(len, 0.5), pct(len, 0.9), pct(len, 1.0), pct(ratios, 0.1), pct(ratios, 0.5),
        pct(ratios, 0.9));
}
