#include "FlowProbeMode.h"
#include "FlowWarpShader.h"
#include "OutputWindow.h"
#include <SimpleLogger.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "nvof/nvOpticalFlowCommon.h"
#include "nvof/nvOpticalFlowD3D11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "nvofapi64.lib")

namespace {

const int kWidth = 2560;
const int kHeight = 1440;
// The motion between the two generated frames: the after frame is the before frame moved by
// this many pixels, so the forward flow must read (kMoveX, kMoveY) and the backward the reverse.
const int kMoveX = 7;
const int kMoveY = 3;
// Executes timed per configuration, after the warm-up ones.
const int kWarmup = 5;
const int kTimed = 40;
// A flow vector counts as right within this many pixels of the true motion.
const float kRightWithinPx = 0.5f;
// The sustained loop's rate: one flow per frame of a 60 fps source.
const int kLoopRate = 60;

// A texture and the views the probe needs of it. Releases what it holds.
struct Tex {
    ID3D11Texture2D* tex = NULL;
    ID3D11RenderTargetView* rtv = NULL;
    ~Tex() {
        if (rtv) rtv->Release();
        if (tex) tex->Release();
    }
};

struct Device {
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    ~Device() {
        if (ctx) ctx->Release();
        if (dev) dev->Release();
    }
};

const char* FormatName(NV_OF_BUFFER_FORMAT f) {
    switch (f) {
    case NV_OF_BUFFER_FORMAT_GRAYSCALE8: return "GRAYSCALE8";
    case NV_OF_BUFFER_FORMAT_ABGR8: return "ABGR8";
    case NV_OF_BUFFER_FORMAT_NV12: return "NV12";
    default: return "other";
    }
}

const char* TextureName(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8_UNORM: return "R8";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    default: return "other";
    }
}

const char* PerfName(NV_OF_PERF_LEVEL p) {
    switch (p) {
    case NV_OF_PERF_LEVEL_SLOW: return "SLOW";
    case NV_OF_PERF_LEVEL_MEDIUM: return "MEDIUM";
    case NV_OF_PERF_LEVEL_FAST: return "FAST";
    default: return "?";
    }
}

// The adapter the game draws on: the NVIDIA one with the most video memory, else the default.
IDXGIAdapter* PickAdapter() {
    IDXGIFactory1* factory = NULL;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return NULL;
    IDXGIAdapter* best = NULL;
    SIZE_T bestMemory = 0;
    IDXGIAdapter* adapter = NULL;
    for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC desc = {};
        adapter->GetDesc(&desc);
        if (desc.VendorId == 0x10DE && desc.DedicatedVideoMemory > bestMemory) {
            if (best) best->Release();
            best = adapter;
            bestMemory = desc.DedicatedVideoMemory;
        } else {
            adapter->Release();
        }
    }
    factory->Release();
    return best;
}

// A deterministic textured pattern, 0..1: three octaves of value noise, fine to coarse, so the
// engine has detail to match at every block size.
std::vector<float> Pattern() {
    auto hash = [](int x, int y) {
        unsigned int h = (unsigned int)x * 374761393u + (unsigned int)y * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
    };
    auto octave = [&](int x, int y, int cell) {
        const float fx = (float)x / cell, fy = (float)y / cell;
        const int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
        const float tx = fx - ix, ty = fy - iy;
        const float top = hash(ix, iy) + (hash(ix + 1, iy) - hash(ix, iy)) * tx;
        const float bottom = hash(ix, iy + 1) + (hash(ix + 1, iy + 1) - hash(ix, iy + 1)) * tx;
        return top + (bottom - top) * ty;
    };
    std::vector<float> v((size_t)kWidth * kHeight);
    for (int y = 0; y < kHeight; y++) {
        for (int x = 0; x < kWidth; x++) {
            v[(size_t)y * kWidth + x] =
                0.5f * octave(x, y, 6) + 0.3f * octave(x, y, 24) + 0.2f * octave(x, y, 96);
        }
    }
    return v;
}

// Uploads the pattern, moved by (dx, dy), as an 8-bit texture of format: one channel for R8,
// grey in R, G and B (alpha 1) for the four-channel formats.
bool MakeFrame(ID3D11Device* dev, const std::vector<float>& pattern, int dx, int dy,
               DXGI_FORMAT format, Tex* out) {
    const int bpp = format == DXGI_FORMAT_R8_UNORM ? 1 : 4;
    std::vector<unsigned char> bytes((size_t)kWidth * kHeight * bpp);
    for (int y = 0; y < kHeight; y++) {
        for (int x = 0; x < kWidth; x++) {
            const int sx = std::min(std::max(x - dx, 0), kWidth - 1);
            const int sy = std::min(std::max(y - dy, 0), kHeight - 1);
            const unsigned char g = (unsigned char)(pattern[(size_t)sy * kWidth + sx] * 255.0f);
            unsigned char* p = &bytes[((size_t)y * kWidth + x) * bpp];
            p[0] = g;
            if (bpp == 4) {
                p[1] = g;
                p[2] = g;
                p[3] = 255;
            }
        }
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kWidth;
    td.Height = kHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = bytes.data();
    init.SysMemPitch = (UINT)(kWidth * bpp);
    return SUCCEEDED(dev->CreateTexture2D(&td, &init, &out->tex));
}

bool MakeOutput(ID3D11Device* dev, int grid, DXGI_FORMAT format, Tex* out) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)((kWidth + grid - 1) / grid);
    td.Height = (UINT)((kHeight + grid - 1) / grid);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(dev->CreateTexture2D(&td, NULL, &out->tex));
}

// The formats the engine takes for a buffer usage, as DXGI formats, on one line.
void LogSurfaceFormats(NV_OF_D3D11_API_FUNCTION_LIST& of, NvOFHandle h, NV_OF_BUFFER_USAGE usage,
                       const char* name, DXGI_FORMAT* first) {
    uint32_t count = 0;
    *first = DXGI_FORMAT_UNKNOWN;
    if (of.nvOFGetSurfaceFormatCountD3D11(h, usage, NV_OF_MODE_OPTICALFLOW, &count) !=
            NV_OF_SUCCESS || count == 0 || count > 32) {
        LOG("ofprobe: %s formats: none reported", name);
        return;
    }
    DXGI_FORMAT formats[32] = {};
    if (of.nvOFGetSurfaceFormatD3D11(h, usage, NV_OF_MODE_OPTICALFLOW, formats) != NV_OF_SUCCESS) {
        LOG("ofprobe: %s formats: the list could not be read", name);
        return;
    }
    char line[256] = {};
    int used = 0;
    for (uint32_t i = 0; i < count && used < (int)sizeof(line) - 8; i++) {
        used += snprintf(line + used, sizeof(line) - used, "%s%d", i ? ", " : "", (int)formats[i]);
    }
    *first = formats[0];
    LOG("ofprobe: %s formats (DXGI): %s", name, line);
}

void LogCaps(NV_OF_D3D11_API_FUNCTION_LIST& of, NvOFHandle h) {
    struct Cap { NV_OF_CAPS cap; const char* name; };
    const Cap caps[] = {
        {NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, "output grid sizes"},
        {NV_OF_CAPS_SUPPORTED_HINT_GRID_SIZES, "hint grid sizes"},
        {NV_OF_CAPS_SUPPORT_HINT_WITH_OF_MODE, "hints in flow mode"},
        {NV_OF_CAPS_WIDTH_MIN, "width min"},
        {NV_OF_CAPS_HEIGHT_MIN, "height min"},
        {NV_OF_CAPS_WIDTH_MAX, "width max"},
        {NV_OF_CAPS_HEIGHT_MAX, "height max"},
        {NV_OF_CAPS_SUPPORT_ROI, "regions of interest"},
        {NV_OF_CAPS_SUPPORT_ROI_MAX_NUM, "regions of interest max"},
    };
    for (const Cap& c : caps) {
        uint32_t size = 0;
        if (of.nvOFGetCaps(h, c.cap, NULL, &size) != NV_OF_SUCCESS || size == 0 || size > 16) {
            LOG("ofprobe: cap %s: not reported", c.name);
            continue;
        }
        uint32_t values[16] = {};
        if (of.nvOFGetCaps(h, c.cap, values, &size) != NV_OF_SUCCESS) {
            LOG("ofprobe: cap %s: could not be read", c.name);
            continue;
        }
        char line[128] = {};
        int used = 0;
        for (uint32_t i = 0; i < size && used < (int)sizeof(line) - 12; i++) {
            used += snprintf(line + used, sizeof(line) - used, "%s%u", i ? ", " : "", values[i]);
        }
        LOG("ofprobe: cap %s: %s", c.name, line);
    }
}

// Mean of a flow texture over its interior (cells within 16 px of the border left out, where
// the moved pattern clamps) and the share of cells within kRightWithinPx of want.
bool ReadFlow(Device& d, ID3D11Texture2D* flow, int grid, float wantX, float wantY,
              float* meanX, float* meanY, float* right) {
    D3D11_TEXTURE2D_DESC td = {};
    flow->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Tex staging;
    if (FAILED(d.dev->CreateTexture2D(&td, NULL, &staging.tex))) return false;
    d.ctx->CopyResource(staging.tex, flow);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(d.ctx->Map(staging.tex, 0, D3D11_MAP_READ, 0, &m))) return false;
    const int margin = (16 + grid - 1) / grid;
    double sx = 0.0, sy = 0.0;
    long long n = 0, ok = 0;
    for (UINT y = (UINT)margin; y + margin < td.Height; y++) {
        const short* row = (const short*)((const unsigned char*)m.pData + (size_t)y * m.RowPitch);
        for (UINT x = (UINT)margin; x + margin < td.Width; x++) {
            const float vx = row[2 * x] / 32.0f;
            const float vy = row[2 * x + 1] / 32.0f;
            sx += vx;
            sy += vy;
            n++;
            if (std::fabs(vx - wantX) <= kRightWithinPx && std::fabs(vy - wantY) <= kRightWithinPx) {
                ok++;
            }
        }
    }
    d.ctx->Unmap(staging.tex, 0);
    if (n == 0) return false;
    *meanX = (float)(sx / n);
    *meanY = (float)(sy / n);
    *right = (float)ok / (float)n;
    return true;
}

struct Config {
    NV_OF_BUFFER_FORMAT input;
    DXGI_FORMAT inputDxgi;
    int grid;
    NV_OF_PERF_LEVEL perf;
    bool both;
    bool cost;
};

// One flow session at cfg, ready to execute on the given frames.
struct Session {
    NV_OF_D3D11_API_FUNCTION_LIST* of = NULL;
    NvOFHandle handle = NULL;
    NvOFGPUBufferHandle before = NULL, after = NULL, fwd = NULL, bwd = NULL, fwdCost = NULL,
                        bwdCost = NULL;
    Tex fwdTex, bwdTex, fwdCostTex, bwdCostTex;
    ~Session() {
        if (!of) return;
        const NvOFGPUBufferHandle all[] = {before, after, fwd, bwd, fwdCost, bwdCost};
        for (NvOFGPUBufferHandle b : all) {
            if (b) of->nvOFUnregisterResourceD3D11(b);
        }
        if (handle) of->nvOFDestroy(handle);
    }
};

// Creates the session and registers its buffers; on failure logs which step and why.
bool OpenSession(NV_OF_D3D11_API_FUNCTION_LIST& of, Device& d, const Config& cfg,
                 ID3D11Texture2D* before, ID3D11Texture2D* after, DXGI_FORMAT flowFormat,
                 DXGI_FORMAT costFormat, Session* s, const char* label) {
    s->of = &of;
    NV_OF_STATUS st = of.nvCreateOpticalFlowD3D11(d.dev, d.ctx, &s->handle);
    if (st != NV_OF_SUCCESS) {
        LOG("ofprobe: %s: session not created (status %d)", label, (int)st);
        return false;
    }
    NV_OF_INIT_PARAMS init = {};
    init.width = kWidth;
    init.height = kHeight;
    init.outGridSize = (NV_OF_OUTPUT_VECTOR_GRID_SIZE)cfg.grid;
    init.mode = NV_OF_MODE_OPTICALFLOW;
    init.perfLevel = cfg.perf;
    init.enableOutputCost = cfg.cost ? NV_OF_TRUE : NV_OF_FALSE;
    init.predDirection = cfg.both ? NV_OF_PRED_DIRECTION_BOTH : NV_OF_PRED_DIRECTION_FORWARD;
    init.inputBufferFormat = cfg.input;
    st = of.nvOFInit(s->handle, &init);
    if (st != NV_OF_SUCCESS) {
        LOG("ofprobe: %s: init refused (status %d)", label, (int)st);
        return false;
    }
    if (!MakeOutput(d.dev, cfg.grid, flowFormat, &s->fwdTex) ||
        (cfg.both && !MakeOutput(d.dev, cfg.grid, flowFormat, &s->bwdTex)) ||
        (cfg.cost && !MakeOutput(d.dev, cfg.grid, costFormat, &s->fwdCostTex)) ||
        (cfg.cost && cfg.both && !MakeOutput(d.dev, cfg.grid, costFormat, &s->bwdCostTex))) {
        LOG("ofprobe: %s: output textures could not be created", label);
        return false;
    }
    struct Reg { ID3D11Texture2D* tex; NvOFGPUBufferHandle* handle; const char* what; };
    const Reg regs[] = {
        {before, &s->before, "before frame"},
        {after, &s->after, "after frame"},
        {s->fwdTex.tex, &s->fwd, "forward flow"},
        {s->bwdTex.tex, &s->bwd, "backward flow"},
        {s->fwdCostTex.tex, &s->fwdCost, "forward cost"},
        {s->bwdCostTex.tex, &s->bwdCost, "backward cost"},
    };
    for (const Reg& r : regs) {
        if (!r.tex) continue;
        st = of.nvOFRegisterResourceD3D11(s->handle, r.tex, r.handle);
        if (st != NV_OF_SUCCESS) {
            LOG("ofprobe: %s: %s not registered (status %d)", label, r.what, (int)st);
            return false;
        }
    }
    return true;
}

bool Execute(Session& s, bool hints) {
    NV_OF_EXECUTE_INPUT_PARAMS in = {};
    in.inputFrame = s.before;
    in.referenceFrame = s.after;
    in.disableTemporalHints = hints ? NV_OF_FALSE : NV_OF_TRUE;
    NV_OF_EXECUTE_OUTPUT_PARAMS out = {};
    out.outputBuffer = s.fwd;
    out.outputCostBuffer = s.fwdCost;
    out.bwdOutputBuffer = s.bwd;
    out.bwdOutputCostBuffer = s.bwdCost;
    return s.of->nvOFExecute(s.handle, &in, &out) == NV_OF_SUCCESS;
}

// Waits for everything submitted on the context to finish. Returns false after two seconds,
// which no flow execute comes near.
bool WaitIdle(Device& d, ID3D11Query* event) {
    d.ctx->End(event);
    d.ctx->Flush();
    LARGE_INTEGER start, now, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    BOOL done = FALSE;
    while (d.ctx->GetData(event, &done, sizeof(done), 0) != S_OK || !done) {
        QueryPerformanceCounter(&now);
        if (now.QuadPart - start.QuadPart > 2 * freq.QuadPart) return false;
        YieldProcessor();
    }
    return true;
}

double Percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t i = (size_t)std::min((double)(v.size() - 1), q * (double)(v.size() - 1) + 0.5);
    return v[i];
}

// A full-screen draw of a constant colour: the loop's positive control.
struct Draw {
    Tex target;
    ID3D11VertexShader* vs = NULL;
    ID3D11PixelShader* ps = NULL;
    ~Draw() {
        if (ps) ps->Release();
        if (vs) vs->Release();
    }
};

bool MakeDraw(Device& d, Draw* draw) {
    static const char* kVs =
        "void main(uint id : SV_VertexID, out float4 pos : SV_Position) {\n"
        "    float2 uv = float2((id << 1) & 2, id & 2);\n"
        "    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "}\n";
    static const char* kPs = "float4 main() : SV_Target { return float4(0.2, 0.4, 0.6, 1.0); }\n";
    ID3DBlob* blob = NULL;
    if (FAILED(D3DCompile(kVs, strlen(kVs), "ProbeVS", NULL, NULL, "main", "vs_5_0", 0, 0, &blob,
                          NULL))) {
        return false;
    }
    HRESULT hr = d.dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL,
                                           &draw->vs);
    blob->Release();
    if (FAILED(hr)) return false;
    if (FAILED(D3DCompile(kPs, strlen(kPs), "ProbePS", NULL, NULL, "main", "ps_5_0", 0, 0, &blob,
                          NULL))) {
        return false;
    }
    hr = d.dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &draw->ps);
    blob->Release();
    if (FAILED(hr)) return false;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kWidth;
    td.Height = kHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(d.dev->CreateTexture2D(&td, NULL, &draw->target.tex))) return false;
    return SUCCEEDED(d.dev->CreateRenderTargetView(draw->target.tex, NULL, &draw->target.rtv));
}

void RunDraw(Device& d, Draw& draw) {
    D3D11_VIEWPORT vp = {};
    vp.Width = (FLOAT)kWidth;
    vp.Height = (FLOAT)kHeight;
    vp.MaxDepth = 1.0f;
    d.ctx->OMSetRenderTargets(1, &draw.target.rtv, NULL);
    d.ctx->RSSetViewports(1, &vp);
    d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.ctx->IASetInputLayout(NULL);
    d.ctx->VSSetShader(draw.vs, NULL, 0);
    d.ctx->PSSetShader(draw.ps, NULL, 0);
    d.ctx->Draw(3, 0);
}

// ---------------------------------------------------------------------------------
// THE WARP SELF-TEST: the shader against FlowWarpMath.h, then on the engine's own flow.
// ---------------------------------------------------------------------------------

const int kTestW = 320;
const int kTestH = 96;
const int kSquareX0 = 120, kSquareX1 = 200, kSquareY0 = 24, kSquareY1 = 72, kSquareMove = 12;

float TestBackground(int x, int y) {
    return 0.3f + 0.15f * std::sin(0.45f * x) + 0.05f * std::cos(0.3f * y);
}

bool InSquare(int x, int y, int shift) {
    return x >= kSquareX0 + shift && x < kSquareX1 + shift && y >= kSquareY0 && y < kSquareY1;
}

// One generated frame, 8-bit grey, as the GPU will see it: the bytes, and their values on 0..1
// for the CPU reference.
struct GreyFrame {
    std::vector<unsigned char> rgba;
    std::vector<float> value;
    float At(int x, int y) const {
        x = std::min(std::max(x, 0), kTestW - 1);
        y = std::min(std::max(y, 0), kTestH - 1);
        return value[(size_t)y * kTestW + x];
    }
    // As the GPU's linear clamp sampler, at a position in pixels.
    float Sample(flowwarp::Vec2 p) const {
        const float u = p.x - 0.5f, v = p.y - 0.5f;
        const float fu = std::floor(u), fv = std::floor(v);
        const int x = (int)fu, y = (int)fv;
        const float tx = u - fu, ty = v - fv;
        const float top = At(x, y) + (At(x + 1, y) - At(x, y)) * tx;
        const float bottom = At(x, y + 1) + (At(x + 1, y + 1) - At(x, y + 1)) * tx;
        return top + (bottom - top) * ty;
    }
};

GreyFrame SquareFrame(int shift, bool withSquare) {
    GreyFrame f;
    f.rgba.resize((size_t)kTestW * kTestH * 4);
    f.value.resize((size_t)kTestW * kTestH);
    for (int y = 0; y < kTestH; y++) {
        for (int x = 0; x < kTestW; x++) {
            const float v = withSquare && InSquare(x, y, shift) ? 0.95f : TestBackground(x, y);
            const unsigned char g = (unsigned char)(v * 255.0f + 0.5f);
            unsigned char* p = &f.rgba[((size_t)y * kTestW + x) * 4];
            p[0] = p[1] = p[2] = g;
            p[3] = 255;
            f.value[(size_t)y * kTestW + x] = g / 255.0f;
        }
    }
    return f;
}

bool MakeTexture(ID3D11Device* dev, int w, int h, DXGI_FORMAT format, const void* data,
                 UINT pitch, UINT bind, Tex* out) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = data;
    init.SysMemPitch = pitch;
    if (FAILED(dev->CreateTexture2D(&td, data ? &init : NULL, &out->tex))) return false;
    if (bind & D3D11_BIND_RENDER_TARGET) {
        return SUCCEEDED(dev->CreateRenderTargetView(out->tex, NULL, &out->rtv));
    }
    return true;
}

// The warp shader and what it draws with.
struct WarpRig {
    ID3D11VertexShader* vs = NULL;
    ID3D11PixelShader* ps = NULL;
    ID3D11SamplerState* sampler = NULL;
    ID3D11Buffer* cb = NULL;
    Tex target;
    ~WarpRig() {
        if (cb) cb->Release();
        if (sampler) sampler->Release();
        if (ps) ps->Release();
        if (vs) vs->Release();
    }
};

bool MakeWarpRig(Device& d, WarpRig* rig) {
    static const char* kVs =
        "void main(uint id : SV_VertexID, out float4 pos : SV_Position) {\n"
        "    float2 uv = float2((id << 1) & 2, id & 2);\n"
        "    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "}\n";
    ID3DBlob* blob = NULL;
    ID3DBlob* err = NULL;
    if (FAILED(D3DCompile(kVs, strlen(kVs), "WarpTestVS", NULL, NULL, "main", "vs_5_0", 0, 0,
                          &blob, NULL))) {
        LOG("ofprobe: warp self-test: vertex shader did not compile");
        return false;
    }
    HRESULT hr = d.dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL,
                                           &rig->vs);
    blob->Release();
    if (FAILED(hr)) return false;
    const std::string ps = FlowWarpShaderSource();
    if (FAILED(D3DCompile(ps.c_str(), ps.size(), "FlowWarpPS", NULL, NULL, "main", "ps_5_0", 0, 0,
                          &blob, &err))) {
        LOG("ofprobe: warp self-test: the warp shader did not compile: %s",
            err ? (const char*)err->GetBufferPointer() : "no message");
        if (err) err->Release();
        return false;
    }
    hr = d.dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &rig->ps);
    blob->Release();
    if (FAILED(hr)) return false;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(d.dev->CreateSamplerState(&sd, &rig->sampler))) return false;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(FlowWarpParams);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(d.dev->CreateBuffer(&bd, NULL, &rig->cb))) return false;
    return MakeTexture(d.dev, kTestW, kTestH, DXGI_FORMAT_R32G32B32A32_FLOAT, NULL, 0,
                       D3D11_BIND_RENDER_TARGET, &rig->target);
}

// Draws the warp at w and returns the red channel of the result, row major.
bool RunWarp(Device& d, WarpRig& rig, ID3D11ShaderResourceView* before,
             ID3D11ShaderResourceView* after, ID3D11ShaderResourceView* fwd,
             ID3D11ShaderResourceView* bwd, int grid, float w, std::vector<float>* out) {
    FlowWarpParams params = {};
    params.w = w;
    params.grid = (float)grid;
    params.width = (float)kTestW;
    params.height = (float)kTestH;
    params.cellsX = (kTestW + grid - 1) / grid;
    params.cellsY = (kTestH + grid - 1) / grid;
    d.ctx->UpdateSubresource(rig.cb, 0, NULL, &params, 0, 0);
    D3D11_VIEWPORT vp = {};
    vp.Width = (FLOAT)kTestW;
    vp.Height = (FLOAT)kTestH;
    vp.MaxDepth = 1.0f;
    d.ctx->OMSetRenderTargets(1, &rig.target.rtv, NULL);
    d.ctx->RSSetViewports(1, &vp);
    d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.ctx->IASetInputLayout(NULL);
    d.ctx->VSSetShader(rig.vs, NULL, 0);
    d.ctx->PSSetShader(rig.ps, NULL, 0);
    ID3D11ShaderResourceView* srvs[4] = {before, after, fwd, bwd};
    d.ctx->PSSetShaderResources(0, 4, srvs);
    d.ctx->PSSetSamplers(0, 1, &rig.sampler);
    d.ctx->PSSetConstantBuffers(0, 1, &rig.cb);
    d.ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[4] = {NULL, NULL, NULL, NULL};
    d.ctx->PSSetShaderResources(0, 4, nulls);
    ID3D11RenderTargetView* noTarget = NULL;
    d.ctx->OMSetRenderTargets(1, &noTarget, NULL);

    D3D11_TEXTURE2D_DESC td = {};
    rig.target.tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Tex staging;
    if (FAILED(d.dev->CreateTexture2D(&td, NULL, &staging.tex))) return false;
    d.ctx->CopyResource(staging.tex, rig.target.tex);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(d.ctx->Map(staging.tex, 0, D3D11_MAP_READ, 0, &m))) return false;
    out->resize((size_t)kTestW * kTestH);
    for (int y = 0; y < kTestH; y++) {
        const float* row = (const float*)((const unsigned char*)m.pData + (size_t)y * m.RowPitch);
        for (int x = 0; x < kTestW; x++) (*out)[(size_t)y * kTestW + x] = row[4 * x];
    }
    d.ctx->Unmap(staging.tex, 0);
    return true;
}

struct Srv {
    ID3D11ShaderResourceView* view = NULL;
    ~Srv() {
        if (view) view->Release();
    }
};

// Flow fields as the engine writes them: one S10.5 vector per cell.
bool MakeField(Device& d, const std::vector<flowwarp::Vec2>& v, int cellsX, int cellsY, Tex* tex,
               Srv* srv) {
    std::vector<short> raw(v.size() * 2);
    for (size_t i = 0; i < v.size(); i++) {
        raw[2 * i] = (short)std::lround(v[i].x * 32.0f);
        raw[2 * i + 1] = (short)std::lround(v[i].y * 32.0f);
    }
    if (!MakeTexture(d.dev, cellsX, cellsY, DXGI_FORMAT_R16G16_SINT, raw.data(),
                     (UINT)(cellsX * 4), D3D11_BIND_SHADER_RESOURCE, tex)) {
        return false;
    }
    return SUCCEEDED(d.dev->CreateShaderResourceView(tex->tex, NULL, &srv->view));
}

// The largest difference between the GPU's output and the CPU reference's, over every pixel.
float WorstAgainstReference(const std::vector<float>& gpu, const GreyFrame& a, const GreyFrame& b,
                            const std::vector<flowwarp::Vec2>& fwd,
                            const std::vector<flowwarp::Vec2>& bwd, float w) {
    flowwarp::FlowField f, g;
    f.cellsX = g.cellsX = kTestW;
    f.cellsY = g.cellsY = kTestH;
    f.vectors = fwd.data();
    g.vectors = bwd.data();
    auto before = [&](flowwarp::Vec2 p) { return a.Sample(p); };
    auto after = [&](flowwarp::Vec2 p) { return b.Sample(p); };
    float worst = 0.0f;
    for (int y = 0; y < kTestH; y++) {
        for (int x = 0; x < kTestW; x++) {
            const float ref = flowwarp::WarpPixel(before, after, f, g, {x + 0.5f, y + 0.5f}, w).value;
            worst = std::max(worst, std::fabs(gpu[(size_t)y * kTestW + x] - ref));
        }
    }
    return worst;
}

// Runs the self-test and logs one line per check and a verdict.
void WarpSelfTest(NV_OF_D3D11_API_FUNCTION_LIST& of, Device& d) {
    WarpRig rig;
    if (!MakeWarpRig(d, &rig)) {
        LOG("ofprobe: warp self-test: RESULT could not be set up");
        return;
    }
    const GreyFrame f0 = SquareFrame(0, true);
    const GreyFrame f1 = SquareFrame(kSquareMove, true);
    Tex t0, t1;
    Srv s0, s1;
    if (!MakeTexture(d.dev, kTestW, kTestH, DXGI_FORMAT_R8G8B8A8_UNORM, f0.rgba.data(), kTestW * 4,
                     D3D11_BIND_SHADER_RESOURCE, &t0) ||
        !MakeTexture(d.dev, kTestW, kTestH, DXGI_FORMAT_R8G8B8A8_UNORM, f1.rgba.data(), kTestW * 4,
                     D3D11_BIND_SHADER_RESOURCE, &t1) ||
        FAILED(d.dev->CreateShaderResourceView(t0.tex, NULL, &s0.view)) ||
        FAILED(d.dev->CreateShaderResourceView(t1.tex, NULL, &s1.view))) {
        LOG("ofprobe: warp self-test: RESULT the frames could not be created");
        return;
    }
    std::vector<flowwarp::Vec2> fwd((size_t)kTestW * kTestH), bwd(fwd.size()), zero(fwd.size()),
        fwdFlipped(fwd.size()), bwdFlipped(fwd.size());
    for (int y = 0; y < kTestH; y++) {
        for (int x = 0; x < kTestW; x++) {
            const size_t i = (size_t)y * kTestW + x;
            fwd[i] = InSquare(x, y, 0) ? flowwarp::Vec2{(float)kSquareMove, 0.0f} : flowwarp::Vec2{};
            bwd[i] = InSquare(x, y, kSquareMove) ? flowwarp::Vec2{-(float)kSquareMove, 0.0f}
                                                 : flowwarp::Vec2{};
            fwdFlipped[i] = flowwarp::Scale(fwd[i], -1.0f);
            bwdFlipped[i] = flowwarp::Scale(bwd[i], -1.0f);
        }
    }
    Tex fwdT, bwdT, zeroT, fwdFT, bwdFT;
    Srv fwdS, bwdS, zeroS, fwdFS, bwdFS;
    if (!MakeField(d, fwd, kTestW, kTestH, &fwdT, &fwdS) ||
        !MakeField(d, bwd, kTestW, kTestH, &bwdT, &bwdS) ||
        !MakeField(d, zero, kTestW, kTestH, &zeroT, &zeroS) ||
        !MakeField(d, fwdFlipped, kTestW, kTestH, &fwdFT, &fwdFS) ||
        !MakeField(d, bwdFlipped, kTestW, kTestH, &bwdFT, &bwdFS)) {
        LOG("ofprobe: warp self-test: RESULT the flow fields could not be created");
        return;
    }

    std::vector<float> out;
    bool pass = true;
    // The shader against the reference on the moving square, at three weights.
    float worstSquare = 0.0f;
    for (float w : {0.25f, 0.5f, 0.75f}) {
        if (!RunWarp(d, rig, s0.view, s1.view, fwdS.view, bwdS.view, 1, w, &out)) {
            LOG("ofprobe: warp self-test: RESULT the warp could not be drawn");
            return;
        }
        worstSquare = std::max(worstSquare, WorstAgainstReference(out, f0, f1, fwd, bwd, w));
    }
    pass = pass && worstSquare < 0.01f;
    LOG("ofprobe: warp self-test: moving square, shader against reference: worst %.4f (want < 0.01)",
        worstSquare);
    // Control: the same frame twice with no motion comes back as it went in.
    RunWarp(d, rig, s0.view, s0.view, zeroS.view, zeroS.view, 1, 0.5f, &out);
    float worstSame = 0.0f;
    for (size_t i = 0; i < out.size(); i++) worstSame = std::max(worstSame, std::fabs(out[i] - f0.value[i]));
    pass = pass && worstSame < 0.001f;
    LOG("ofprobe: warp self-test: one frame twice, no motion: worst %.4f (want < 0.001)", worstSame);
    // Control: w = 0 is the before frame, whatever the flow.
    RunWarp(d, rig, s0.view, s1.view, fwdS.view, bwdS.view, 1, 0.0f, &out);
    float worstStart = 0.0f;
    for (size_t i = 0; i < out.size(); i++) worstStart = std::max(worstStart, std::fabs(out[i] - f0.value[i]));
    pass = pass && worstStart < 0.001f;
    LOG("ofprobe: warp self-test: w = 0 against the before frame: worst %.4f (want < 0.001)",
        worstStart);
    // Control that must FAIL: fields of the wrong sign, compared with the reference on the right
    // ones. If this matched, the comparison above could not see a wrong field.
    RunWarp(d, rig, s0.view, s1.view, fwdFS.view, bwdFS.view, 1, 0.5f, &out);
    const float worstWrong = WorstAgainstReference(out, f0, f1, fwd, bwd, 0.5f);
    pass = pass && worstWrong > 0.2f;
    LOG("ofprobe: warp self-test: wrong-sign fields against the reference: worst %.4f (must be "
        "> 0.2)", worstWrong);
    LOG("ofprobe: warp self-test: RESULT %s", pass ? "PASS" : "FAIL");

    // The warp on the engine's own flow for the same square, scored against the truth beside the
    // crossfade, away from the square's edges. A reading, not a pass mark: the engine's flow is
    // an estimate at its grid.
    Tex g0, g1;
    std::vector<unsigned char> grey0((size_t)kTestW * kTestH), grey1(grey0.size());
    for (size_t i = 0; i < grey0.size(); i++) {
        grey0[i] = f0.rgba[i * 4];
        grey1[i] = f1.rgba[i * 4];
    }
    if (!MakeTexture(d.dev, kTestW, kTestH, DXGI_FORMAT_R8_UNORM, grey0.data(), kTestW,
                     D3D11_BIND_SHADER_RESOURCE, &g0) ||
        !MakeTexture(d.dev, kTestW, kTestH, DXGI_FORMAT_R8_UNORM, grey1.data(), kTestW,
                     D3D11_BIND_SHADER_RESOURCE, &g1)) {
        return;
    }
    for (int grid : {4, 2}) {
        Config cfg = {NV_OF_BUFFER_FORMAT_GRAYSCALE8, DXGI_FORMAT_R8_UNORM, grid,
                      NV_OF_PERF_LEVEL_SLOW, true, false};
        Session s;
        char label[64];
        snprintf(label, sizeof(label), "warp on engine flow, grid %d", grid);
        // The probe's sessions are full size; this one is the test frames' size.
        s.of = &of;
        if (of.nvCreateOpticalFlowD3D11(d.dev, d.ctx, &s.handle) != NV_OF_SUCCESS) continue;
        NV_OF_INIT_PARAMS init = {};
        init.width = kTestW;
        init.height = kTestH;
        init.outGridSize = (NV_OF_OUTPUT_VECTOR_GRID_SIZE)cfg.grid;
        init.mode = NV_OF_MODE_OPTICALFLOW;
        init.perfLevel = cfg.perf;
        init.predDirection = NV_OF_PRED_DIRECTION_BOTH;
        init.inputBufferFormat = cfg.input;
        if (of.nvOFInit(s.handle, &init) != NV_OF_SUCCESS) {
            LOG("ofprobe: %s: init refused at %dx%d", label, kTestW, kTestH);
            continue;
        }
        const int cx = (kTestW + grid - 1) / grid, cy = (kTestH + grid - 1) / grid;
        if (!MakeTexture(d.dev, cx, cy, DXGI_FORMAT_R16G16_SINT, NULL, 0,
                         D3D11_BIND_SHADER_RESOURCE, &s.fwdTex) ||
            !MakeTexture(d.dev, cx, cy, DXGI_FORMAT_R16G16_SINT, NULL, 0,
                         D3D11_BIND_SHADER_RESOURCE, &s.bwdTex) ||
            of.nvOFRegisterResourceD3D11(s.handle, g0.tex, &s.before) != NV_OF_SUCCESS ||
            of.nvOFRegisterResourceD3D11(s.handle, g1.tex, &s.after) != NV_OF_SUCCESS ||
            of.nvOFRegisterResourceD3D11(s.handle, s.fwdTex.tex, &s.fwd) != NV_OF_SUCCESS ||
            of.nvOFRegisterResourceD3D11(s.handle, s.bwdTex.tex, &s.bwd) != NV_OF_SUCCESS ||
            !Execute(s, false)) {
            LOG("ofprobe: %s: could not run", label);
            continue;
        }
        Srv fs, bs;
        if (FAILED(d.dev->CreateShaderResourceView(s.fwdTex.tex, NULL, &fs.view)) ||
            FAILED(d.dev->CreateShaderResourceView(s.bwdTex.tex, NULL, &bs.view)) ||
            !RunWarp(d, rig, s0.view, s1.view, fs.view, bs.view, grid, 0.5f, &out)) {
            continue;
        }
        double warpErr = 0.0, blendErr = 0.0;
        int n = 0;
        const int edges[] = {kSquareX0, kSquareX0 + kSquareMove / 2, kSquareX0 + kSquareMove,
                             kSquareX1, kSquareX1 + kSquareMove / 2, kSquareX1 + kSquareMove};
        for (int y = kSquareY0 + 4; y < kSquareY1 - 4; y++) {
            for (int x = 8; x < kTestW - 8; x++) {
                bool nearEdge = false;
                for (int e : edges) {
                    if (x >= e - 4 && x < e + 4) nearEdge = true;
                }
                if (nearEdge) continue;
                const float truth = InSquare(x, y, kSquareMove / 2) ? 0.95f : TestBackground(x, y);
                const size_t i = (size_t)y * kTestW + x;
                warpErr += std::fabs(out[i] - truth);
                blendErr += std::fabs(0.5f * (f0.value[i] + f1.value[i]) - truth);
                n++;
            }
        }
        LOG("ofprobe: %s: mean error %.4f against the truth, crossfade %.4f (%d pixels, 4 px "
            "from each edge left out)", label, n ? warpErr / n : 0.0, n ? blendErr / n : 0.0, n);
    }
}

}  // namespace

FlowProbeMode::FlowProbeMode(int loopSeconds, bool drawControl)
    : m_loopSeconds(loopSeconds), m_drawControl(drawControl) {}

UINT FlowProbeMode::GetPresentationInterval() const { return D3DPRESENT_INTERVAL_IMMEDIATE; }

MaybeFailure FlowProbeMode::Setup(const RelayContext& /*ctx*/) { return std::nullopt; }

const char* FlowProbeMode::GetModeName() const {
    return m_drawControl ? "Optical flow engine probe, with a draw per flow"
                         : "Optical flow engine probe";
}

MaybeFailure FlowProbeMode::Run(RelayContext& /*ctx*/,
                                NVFBC_TODX9VID_GRAB_FRAME_PARAMS* /*grabParams*/) {
    LOG("ofprobe: %dx%d, generated frames moved by (%d, %d) px, %d timed executes per "
        "configuration", kWidth, kHeight, kMoveX, kMoveY, kTimed);

    // The device, on the game's adapter.
    Device d;
    IDXGIAdapter* adapter = PickAdapter();
    if (adapter) {
        DXGI_ADAPTER_DESC desc = {};
        adapter->GetDesc(&desc);
        char name[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, (int)sizeof(name) - 1, NULL,
                            NULL);
        LOG("ofprobe: adapter %s, %llu MB", name,
            (unsigned long long)(desc.DedicatedVideoMemory >> 20));
    } else {
        LOG("ofprobe: no NVIDIA adapter found; using the default adapter");
    }
    const HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN
                                                          : D3D_DRIVER_TYPE_HARDWARE,
                                         NULL, 0, NULL, 0, D3D11_SDK_VERSION, &d.dev, NULL,
                                         &d.ctx);
    if (adapter) adapter->Release();
    if (FAILED(hr)) {
        LOG("ofprobe: RESULT no D3D11 device (0x%08x)", (unsigned)hr);
        return std::nullopt;
    }

    // P1: the engine's interface, what it takes, what it can do.
    NV_OF_D3D11_API_FUNCTION_LIST of = {};
    NV_OF_STATUS st = NvOFAPICreateInstanceD3D11(NV_OF_API_VERSION, &of);
    if (st != NV_OF_SUCCESS) {
        LOG("ofprobe: RESULT the driver's optical flow interface refused API version %d.%d "
            "(status %d)", NV_OF_API_MAJOR_VERSION, NV_OF_API_MINOR_VERSION, (int)st);
        return std::nullopt;
    }
    DXGI_FORMAT flowFormat = DXGI_FORMAT_UNKNOWN, costFormat = DXGI_FORMAT_UNKNOWN,
                inputFormat = DXGI_FORMAT_UNKNOWN;
    {
        NvOFHandle h = NULL;
        st = of.nvCreateOpticalFlowD3D11(d.dev, d.ctx, &h);
        if (st != NV_OF_SUCCESS) {
            LOG("ofprobe: RESULT no optical flow session on this adapter (status %d)", (int)st);
            return std::nullopt;
        }
        LogSurfaceFormats(of, h, NV_OF_BUFFER_USAGE_INPUT, "input", &inputFormat);
        LogSurfaceFormats(of, h, NV_OF_BUFFER_USAGE_OUTPUT, "flow output", &flowFormat);
        LogSurfaceFormats(of, h, NV_OF_BUFFER_USAGE_COST, "cost output", &costFormat);
        LogCaps(of, h);
        of.nvOFDestroy(h);
    }
    if (flowFormat == DXGI_FORMAT_UNKNOWN) flowFormat = DXGI_FORMAT_R16G16_SINT;
    if (costFormat == DXGI_FORMAT_UNKNOWN) costFormat = DXGI_FORMAT_R8_UINT;

    // The looping spellings go straight to the loop, so a trace taken a fixed time into a
    // benchmark finds the loop and not the sweep; the plain spelling does the sweep alone.
    if (m_loopSeconds <= 0) WarpSelfTest(of, d);

    // The frames: grey pattern as luma, as RGBA (what ABGR8 names) and as BGRA (what WGC
    // delivers), each before and after the move.
    const std::vector<float> pattern = Pattern();
    Tex lumaA, lumaB, rgbaA, rgbaB, bgraA, bgraB;
    if (!MakeFrame(d.dev, pattern, 0, 0, DXGI_FORMAT_R8_UNORM, &lumaA) ||
        !MakeFrame(d.dev, pattern, kMoveX, kMoveY, DXGI_FORMAT_R8_UNORM, &lumaB) ||
        !MakeFrame(d.dev, pattern, 0, 0, DXGI_FORMAT_R8G8B8A8_UNORM, &rgbaA) ||
        !MakeFrame(d.dev, pattern, kMoveX, kMoveY, DXGI_FORMAT_R8G8B8A8_UNORM, &rgbaB) ||
        !MakeFrame(d.dev, pattern, 0, 0, DXGI_FORMAT_B8G8R8A8_UNORM, &bgraA) ||
        !MakeFrame(d.dev, pattern, kMoveX, kMoveY, DXGI_FORMAT_B8G8R8A8_UNORM, &bgraB)) {
        LOG("ofprobe: RESULT the generated frames could not be created");
        return std::nullopt;
    }

    ID3D11Query* event = NULL;
    ID3D11Query* disjoint = NULL;
    ID3D11Query* t0 = NULL;
    ID3D11Query* t1 = NULL;
    D3D11_QUERY_DESC qd = {};
    qd.Query = D3D11_QUERY_EVENT;
    d.dev->CreateQuery(&qd, &event);
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    d.dev->CreateQuery(&qd, &disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    d.dev->CreateQuery(&qd, &t0);
    d.dev->CreateQuery(&qd, &t1);
    struct Queries {
        ID3D11Query* q[4];
        ~Queries() {
            for (ID3D11Query* x : q) {
                if (x) x->Release();
            }
        }
    } queries = {{event, disjoint, t0, t1}};
    if (!event || !disjoint || !t0 || !t1) {
        LOG("ofprobe: RESULT the timing queries could not be created");
        return std::nullopt;
    }
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    // P3 and P4: each configuration timed and checked. Luma across every grid, preset and
    // direction; the compositor's likely setting again with cost off and with the two
    // four-channel inputs.
    std::vector<Config> configs;
    for (int grid : {4, 2, 1}) {
        if (m_loopSeconds > 0) break;
        for (NV_OF_PERF_LEVEL perf : {NV_OF_PERF_LEVEL_FAST, NV_OF_PERF_LEVEL_MEDIUM,
                                      NV_OF_PERF_LEVEL_SLOW}) {
            for (bool both : {false, true}) {
                configs.push_back({NV_OF_BUFFER_FORMAT_GRAYSCALE8, DXGI_FORMAT_R8_UNORM, grid, perf,
                                   both, true});
            }
        }
    }
    if (m_loopSeconds <= 0) {
        configs.push_back({NV_OF_BUFFER_FORMAT_GRAYSCALE8, DXGI_FORMAT_R8_UNORM, 4,
                           NV_OF_PERF_LEVEL_MEDIUM, true, false});
        configs.push_back({NV_OF_BUFFER_FORMAT_ABGR8, DXGI_FORMAT_R8G8B8A8_UNORM, 4,
                           NV_OF_PERF_LEVEL_MEDIUM, true, true});
        configs.push_back({NV_OF_BUFFER_FORMAT_ABGR8, DXGI_FORMAT_B8G8R8A8_UNORM, 4,
                           NV_OF_PERF_LEVEL_MEDIUM, true, true});
    }

    bool bgraWorks = false;
    for (const Config& cfg : configs) {
        if (!PumpMessages()) {
            LOG("ofprobe: the output window closed; stopping");
            return std::nullopt;
        }
        char label[128];
        snprintf(label, sizeof(label), "%s as %s grid %d %s %s cost %s",
                 TextureName(cfg.inputDxgi), FormatName(cfg.input), cfg.grid, PerfName(cfg.perf),
                 cfg.both ? "both ways" : "forward", cfg.cost ? "on" : "off");
        Tex* a = &lumaA;
        Tex* b = &lumaB;
        if (cfg.inputDxgi == DXGI_FORMAT_R8G8B8A8_UNORM) {
            a = &rgbaA;
            b = &rgbaB;
        } else if (cfg.inputDxgi == DXGI_FORMAT_B8G8R8A8_UNORM) {
            a = &bgraA;
            b = &bgraB;
        }
        Session s;
        if (!OpenSession(of, d, cfg, a->tex, b->tex, flowFormat, costFormat, &s, label)) continue;

        std::vector<double> wall, gpu, submit;
        bool failed = false;
        for (int i = 0; i < kWarmup + kTimed && !failed; i++) {
            LARGE_INTEGER c0, c1, c2;
            d.ctx->Begin(disjoint);
            d.ctx->End(t0);
            QueryPerformanceCounter(&c0);
            if (!Execute(s, i > 0)) {
                LOG("ofprobe: %s: execute failed", label);
                failed = true;
                break;
            }
            QueryPerformanceCounter(&c1);
            d.ctx->End(t1);
            d.ctx->End(disjoint);
            if (!WaitIdle(d, event)) {
                LOG("ofprobe: %s: the execute did not finish within 2 s", label);
                failed = true;
                break;
            }
            QueryPerformanceCounter(&c2);
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
            UINT64 g0 = 0, g1 = 0;
            while (d.ctx->GetData(disjoint, &dj, sizeof(dj), 0) != S_OK) YieldProcessor();
            d.ctx->GetData(t0, &g0, sizeof(g0), 0);
            d.ctx->GetData(t1, &g1, sizeof(g1), 0);
            if (i < kWarmup) continue;
            submit.push_back(1000.0 * (double)(c1.QuadPart - c0.QuadPart) / (double)freq.QuadPart);
            wall.push_back(1000.0 * (double)(c2.QuadPart - c0.QuadPart) / (double)freq.QuadPart);
            if (!dj.Disjoint && dj.Frequency > 0 && g1 >= g0) {
                gpu.push_back(1000.0 * (double)(g1 - g0) / (double)dj.Frequency);
            }
        }
        if (failed) continue;

        float fx = 0, fy = 0, fRight = 0, bx = 0, by = 0, bRight = 0;
        const bool fwdRead = ReadFlow(d, s.fwdTex.tex, cfg.grid, (float)kMoveX, (float)kMoveY,
                                      &fx, &fy, &fRight);
        const bool bwdRead = cfg.both && ReadFlow(d, s.bwdTex.tex, cfg.grid, -(float)kMoveX,
                                                  -(float)kMoveY, &bx, &by, &bRight);
        LOG("ofprobe: %s: to done p50 %.2f p90 %.2f max %.2f ms; submit p50 %.3f ms; graphics "
            "timestamps p50 %.2f ms; forward mean (%.2f, %.2f) %.1f%% right%s",
            label, Percentile(wall, 0.5), Percentile(wall, 0.9), Percentile(wall, 1.0),
            Percentile(submit, 0.5), Percentile(gpu, 0.5), fwdRead ? fx : 0.0f,
            fwdRead ? fy : 0.0f, fwdRead ? 100.0f * fRight : 0.0f, fwdRead ? "" : " (unread)");
        if (cfg.both) {
            LOG("ofprobe: %s: backward mean (%.2f, %.2f) %.1f%% right%s", label,
                bwdRead ? bx : 0.0f, bwdRead ? by : 0.0f, bwdRead ? 100.0f * bRight : 0.0f,
                bwdRead ? "" : " (unread)");
        }
        if (cfg.inputDxgi == DXGI_FORMAT_B8G8R8A8_UNORM && fwdRead && fRight > 0.9f) {
            bgraWorks = true;
        }
    }

    if (m_loopSeconds <= 0) {
        LOG("ofprobe: done");
        return std::nullopt;
    }

    // P2: the compositor's likely configuration at 60 flows a second for the trace, on BGRA
    // frames registered as ABGR8 when that worked above, on luma otherwise.
    Config loop = {NV_OF_BUFFER_FORMAT_GRAYSCALE8, DXGI_FORMAT_R8_UNORM, 4, NV_OF_PERF_LEVEL_MEDIUM,
                   true, true};
    Tex* a = &lumaA;
    Tex* b = &lumaB;
    if (bgraWorks) {
        loop.input = NV_OF_BUFFER_FORMAT_ABGR8;
        loop.inputDxgi = DXGI_FORMAT_B8G8R8A8_UNORM;
        a = &bgraA;
        b = &bgraB;
    }
    Session s;
    if (!OpenSession(of, d, loop, a->tex, b->tex, flowFormat, costFormat, &s, "loop")) {
        LOG("ofprobe: done");
        return std::nullopt;
    }
    Draw draw;
    if (m_drawControl && !MakeDraw(d, &draw)) {
        LOG("ofprobe: the control draw could not be set up; looping without it");
        m_drawControl = false;
    }
    LOG("ofprobe: loop: %d s at %d flows a second, %s input, grid 4 MEDIUM both ways, cost on%s",
        m_loopSeconds, kLoopRate, bgraWorks ? "BGRA8 as ABGR8" : "R8 as GRAYSCALE8",
        m_drawControl ? ", one full-screen draw before each flow" : "");
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    const LONGLONG period = freq.QuadPart / kLoopRate;
    long long n = 0;
    std::vector<double> second;
    for (int sec = 0; sec < m_loopSeconds; sec++) {
        second.clear();
        for (int k = 0; k < kLoopRate; k++, n++) {
            const LONGLONG due = start.QuadPart + n * period;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            while (now.QuadPart < due) {
                const LONGLONG ms = (due - now.QuadPart) * 1000 / freq.QuadPart;
                if (ms > 1) Sleep((DWORD)(ms - 1));
                else YieldProcessor();
                QueryPerformanceCounter(&now);
            }
            if (m_drawControl) RunDraw(d, draw);
            LARGE_INTEGER c0, c1;
            QueryPerformanceCounter(&c0);
            if (!Execute(s, true) || !WaitIdle(d, event)) {
                LOG("ofprobe: loop: an execute failed or did not finish; stopping");
                LOG("ofprobe: done");
                return std::nullopt;
            }
            QueryPerformanceCounter(&c1);
            second.push_back(1000.0 * (double)(c1.QuadPart - c0.QuadPart) / (double)freq.QuadPart);
        }
        LOG("ofprobe: loop second %d: %d flows, to done p50 %.2f max %.2f ms", sec + 1, kLoopRate,
            Percentile(second, 0.5), Percentile(second, 1.0));
        if (!PumpMessages()) {
            LOG("ofprobe: the output window closed; stopping");
            break;
        }
    }
    LOG("ofprobe: done");
    return std::nullopt;
}
