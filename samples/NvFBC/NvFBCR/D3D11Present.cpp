#include "D3D11Present.h"

#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <string.h>
#include <stdio.h>
#include <SimpleLogger.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

// Fullscreen triangle from the vertex id: no vertex buffer, no input layout, no state to
// leak. uv runs 0..1 across the viewport, whatever rectangle the viewport covers, so the
// same vertex shader serves the full-frame composite and the marker strip.
const char* kVs =
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut main(uint id : SV_VertexID) {\n"
    "  VSOut o;\n"
    "  float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = uv;\n"
    "  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "  return o;\n"
    "}\n";

// One shader for every composite op. A passthrough binds the same slot to both inputs with
// w = 0, so there is a single code path and the swapchain format may differ from the ring's;
// a CopyResource would require them to match exactly.
//
// Alpha is forced to 1: NvFBC's desktop alpha is unspecified, and passing garbage alpha to a
// compositor that may honour it is how frames end up dimmed.
const char* kPs =
    "Texture2D texA : register(t0);\n"
    "Texture2D texB : register(t1);\n"
    "SamplerState samp : register(s0);\n"
    "cbuffer Params : register(b0) { float4 params; };\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "  float3 a = texA.Sample(samp, uv).rgb;\n"
    "  float3 b = texB.Sample(samp, uv).rgb;\n"
    "  return float4(lerp(a, b, params.x), 1.0);\n"
    "}\n";

// Marker strip: the viewport is the strip rectangle, so uv locates the cell directly by
// fraction, the same geometry the decoder uses. Cells are packed LSB-first into the first
// two words of the constant buffer. Row 1 of the grid carries the cells; rows 0 and 2 and
// the outer columns are the quiet zone. %d fields are the grid constants, in order: grid
// width, grid height, cell count.
const char* kMarkerPsFmt =
    "cbuffer Params : register(b0) { uint4 cells; };\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "  uint cx = (uint)(uv.x * %d);\n"
    "  uint cy = (uint)(uv.y * %d);\n"
    "  bool white = false;\n"
    "  if (cy == 1 && cx >= 1 && cx <= %d) {\n"
    "    uint i = cx - 1;\n"
    "    uint word = (i < 32) ? cells.x : cells.y;\n"
    "    white = ((word >> (i & 31)) & 1) != 0;\n"
    "  }\n"
    "  return white ? float4(1, 1, 1, 1) : float4(0, 0, 0, 1);\n"
    "}\n";

bool Compile(const char* src, const char* name, const char* profile, ID3DBlob** out) {
    ID3DBlob* err = NULL;
    const HRESULT hr = D3DCompile(src, strlen(src), name, NULL, NULL, "main", profile, 0, 0,
                                  out, &err);
    if (FAILED(hr)) {
        LOGERR("D3D11Present: %s failed to compile (0x%08x): %s", name, hr,
               err ? (const char*)err->GetBufferPointer() : "(no message)");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    return true;
}

const char* FormatName(DXGI_FORMAT f) {
    if (f == DXGI_FORMAT_R10G10B10A2_UNORM) return "R10G10B10A2";
    if (f == DXGI_FORMAT_B8G8R8A8_UNORM) return "B8G8R8A8";
    return "other";
}

// Bits per color channel of the output the window sits on, 0 when it cannot be read.
UINT OutputBitsPerColor(IDXGIAdapter* adapter, HWND hwnd) {
    const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    UINT bits = 0;
    for (UINT i = 0; bits == 0; i++) {
        IDXGIOutput* out = NULL;
        if (FAILED(adapter->EnumOutputs(i, &out)) || !out) break;
        DXGI_OUTPUT_DESC od;
        if (SUCCEEDED(out->GetDesc(&od)) && od.Monitor == mon) {
            IDXGIOutput6* out6 = NULL;
            if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput6), (void**)&out6)) && out6) {
                DXGI_OUTPUT_DESC1 od1;
                if (SUCCEEDED(out6->GetDesc1(&od1))) bits = od1.BitsPerColor;
                out6->Release();
            }
        }
        out->Release();
    }
    return bits;
}

}  // namespace

D3D11PresentBackend::D3D11PresentBackend()
    : m_enabled(false)
    , m_width(0), m_height(0), m_ringSlots(0)
    , m_cfg(NULL), m_subGen(false)
    , m_dev(NULL), m_ctx(NULL), m_swapChain(NULL), m_rtv(NULL)
    , m_vs(NULL), m_ps(NULL), m_markerPs(NULL), m_cb(NULL), m_sampler(NULL)
    , m_lastSlotA(-1), m_lastSlotB(-1), m_lastWeight(0.0f), m_lastExec(0), m_haveLast(false)
    , m_mark(false)
    , m_lastSyncRefresh(0), m_missedRefreshes(0), m_statsSamples(0)
    , m_presentFailures(0), m_drawFailures(0), m_presents(0)
{
    for (int i = 0; i < CaptureRing::RING_SIZE; i++) {
        m_ringAlias[i] = NULL;
        m_ringSrv[i] = NULL;
    }
    m_markerRect.left = m_markerRect.top = m_markerRect.right = m_markerRect.bottom = 0;
}

D3D11PresentBackend::~D3D11PresentBackend() {
    for (int i = 0; i < CaptureRing::RING_SIZE; i++) {
        if (m_ringSrv[i]) m_ringSrv[i]->Release();
        if (m_ringAlias[i]) m_ringAlias[i]->Release();
    }
    if (m_sampler) m_sampler->Release();
    if (m_cb) m_cb->Release();
    if (m_markerPs) m_markerPs->Release();
    if (m_ps) m_ps->Release();
    if (m_vs) m_vs->Release();
    if (m_rtv) m_rtv->Release();
    if (m_swapChain) m_swapChain->Release();
    if (m_ctx) m_ctx->Release();
    if (m_dev) m_dev->Release();
}

bool D3D11PresentBackend::Setup(HWND hwnd, CaptureRing* ring, int width, int height,
                                const policy::PolicyConfig* cfg, bool subGen, bool mark,
                                unsigned int markFrames) {
    m_width = width;
    m_height = height;
    m_cfg = cfg;
    m_subGen = subGen;
    if (!CreateDeviceAndSwapChain(hwnd, width, height)) return false;
    if (!OpenRingAliases(ring)) return false;
    if (!CreatePipeline()) return false;
    if (mark) {
        m_marker.InitCounter(markFrames);
        m_markerRect = FrameMarker::StripRect(width, height);
        m_mark = true;
    }
    m_enabled = true;
    return true;
}

bool D3D11PresentBackend::CreateDeviceAndSwapChain(HWND hwnd, int width, int height) {
    // Default adapter, matching InterpSidecar. Correct while the relay is single-GPU; a
    // multi-adapter box would want the adapter driving the TARGET output, which is a
    // different (and currently hypothetical) selection problem.
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                   D3D11_SDK_VERSION, &m_dev, NULL, &m_ctx);
    if (FAILED(hr)) {
        LOGERR("D3D11Present: D3D11CreateDevice failed (0x%08x)", hr);
        return false;
    }

    IDXGIDevice1* dxgiDev = NULL;
    IDXGIAdapter* adapter = NULL;
    IDXGIFactory2* factory = NULL;
    hr = m_dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDev);
    if (SUCCEEDED(hr)) hr = dxgiDev->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    if (FAILED(hr) || !factory) {
        LOGERR("D3D11Present: could not reach IDXGIFactory2 (0x%08x)", hr);
        if (factory) factory->Release();
        if (adapter) adapter->Release();
        if (dxgiDev) dxgiDev->Release();
        return false;
    }

    // FRAME LATENCY: how many presents may be queued before Present blocks. The default of
    // 3 lets the loop run three frames ahead of the display, at which point Present stops
    // being backpressure and the vsync present is a queue push rather than a clock. 1 means
    // "block until the previous frame has been consumed", which is what makes the present the
    // frame-pacing wait.
    hr = dxgiDev->SetMaximumFrameLatency(1);
    if (FAILED(hr)) {
        LOGERR("D3D11Present: SetMaximumFrameLatency(1) failed (0x%08x); presents will queue "
               "and pacing will not be trustworthy", hr);
    }

    // The swapchain follows the OUTPUT's bit depth. A back buffer the display cannot scan
    // out natively is composed by DWM with a conversion instead of flipped, which is the
    // outcome this backend exists to avoid, and an 8-bit output discards the ring's extra
    // bits wherever the conversion happens. Compositing goes through the shader rather
    // than CopyResource, so the format costs nothing but the precision the output already
    // lacks. The other format is the fallback if the runtime refuses the first.
    const UINT bitsPerColor = OutputBitsPerColor(adapter, hwnd);
    DXGI_FORMAT formats[2] = { DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM };
    if (bitsPerColor >= 10) {
        formats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
        formats[1] = DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    LOG("D3D11Present: output reports %u bits per color; trying %s then %s",
        bitsPerColor, FormatName(formats[0]), FormatName(formats[1]));
    for (int f = 0; f < 2 && !m_swapChain; f++) {
        DXGI_SWAP_CHAIN_DESC1 sd;
        ZeroMemory(&sd, sizeof(sd));
        sd.Width = width;
        sd.Height = height;
        sd.Format = formats[f];
        sd.SampleDesc.Count = 1;             // MSAA is not allowed with flip model
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;                  // flip model requires >= 2
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        // Stretching disqualifies a swapchain from independent flip, and the ring already
        // captures at the target's size, so the buffers match the output exactly.
        sd.Scaling = DXGI_SCALING_NONE;
        hr = factory->CreateSwapChainForHwnd(m_dev, hwnd, &sd, NULL, NULL, &m_swapChain);
        if (FAILED(hr)) {
            LOGERR("D3D11Present: CreateSwapChainForHwnd with %s failed (0x%08x)%s",
                   FormatName(formats[f]), hr, f == 0 ? "; retrying the other format" : "");
            m_swapChain = NULL;
        } else {
            LOG("D3D11Present: flip-model swapchain %dx%d, %s, 2 buffers, FLIP_DISCARD, "
                "frame latency 1", width, height, FormatName(formats[f]));
        }
    }
    // The relay owns alt-enter; DXGI must not transition us on its own.
    if (m_swapChain) {
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    }
    factory->Release();
    adapter->Release();
    dxgiDev->Release();
    return m_swapChain != NULL;
}

bool D3D11PresentBackend::OpenRingAliases(CaptureRing* ring) {
    // SlotsInUse, not RING_SIZE: the ring allocates only what the bracketing lag calls for,
    // and aliasing an unallocated slot fails.
    m_ringSlots = ring->SlotsInUse();
    for (int i = 0; i < m_ringSlots; i++) {
        HANDLE h = ring->SlotSharedHandle(i);
        if (!h) {
            LOGERR("D3D11Present: ring slot %d has no shared handle", i);
            return false;
        }
        HRESULT hr = m_dev->OpenSharedResource(h, __uuidof(ID3D11Texture2D),
                                               (void**)&m_ringAlias[i]);
        if (FAILED(hr)) {
            LOGERR("D3D11Present: OpenSharedResource on ring slot %d failed (0x%08x)", i, hr);
            return false;
        }
        hr = m_dev->CreateShaderResourceView(m_ringAlias[i], NULL, &m_ringSrv[i]);
        if (FAILED(hr)) {
            LOGERR("D3D11Present: SRV for ring slot %d failed (0x%08x)", i, hr);
            return false;
        }
    }
    LOG("D3D11Present: opened %d ring slots as D3D11 aliases", m_ringSlots);
    return true;
}

bool D3D11PresentBackend::CreatePipeline() {
    char markerPs[1024];
    snprintf(markerPs, sizeof(markerPs), kMarkerPsFmt,
             FrameMarker::kGridW, FrameMarker::kGridH, FrameMarker::kCells);

    ID3DBlob* vsBlob = NULL;
    ID3DBlob* psBlob = NULL;
    ID3DBlob* markerBlob = NULL;
    if (!Compile(kVs, "D3D11PresentVS", "vs_5_0", &vsBlob)) return false;
    if (!Compile(kPs, "D3D11PresentPS", "ps_5_0", &psBlob)) {
        vsBlob->Release();
        return false;
    }
    if (!Compile(markerPs, "D3D11PresentMarkerPS", "ps_5_0", &markerBlob)) {
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    HRESULT hr = m_dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                           NULL, &m_vs);
    if (SUCCEEDED(hr)) {
        hr = m_dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                      NULL, &m_ps);
    }
    if (SUCCEEDED(hr)) {
        hr = m_dev->CreatePixelShader(markerBlob->GetBufferPointer(),
                                      markerBlob->GetBufferSize(), NULL, &m_markerPs);
    }
    vsBlob->Release();
    psBlob->Release();
    markerBlob->Release();
    if (FAILED(hr)) {
        LOGERR("D3D11Present: shader creation failed (0x%08x)", hr);
        return false;
    }

    D3D11_BUFFER_DESC bd;
    ZeroMemory(&bd, sizeof(bd));
    bd.ByteWidth = 16;                       // one 16-byte register, the constant-buffer minimum
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_dev->CreateBuffer(&bd, NULL, &m_cb);
    if (FAILED(hr)) {
        LOGERR("D3D11Present: constant buffer failed (0x%08x)", hr);
        return false;
    }

    // Point sampling: ring slots are captured at the target's resolution, so this is a 1:1
    // blit and any filtering would only soften it.
    D3D11_SAMPLER_DESC sm;
    ZeroMemory(&sm, sizeof(sm));
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sm.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    hr = m_dev->CreateSamplerState(&sm, &m_sampler);
    if (FAILED(hr)) {
        LOGERR("D3D11Present: sampler failed (0x%08x)", hr);
        return false;
    }
    return true;
}

bool D3D11PresentBackend::AcquireBackBuffer() {
    // Back buffer 0 is re-acquired every present: under flip model the identities of the
    // back buffers change after each Present, so a cached view would render into a surface
    // that is no longer the one about to be scanned out.
    ID3D11Texture2D* back = NULL;
    HRESULT hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    if (FAILED(hr) || !back) {
        LOGERR("D3D11Present: GetBuffer failed (0x%08x)", hr);
        return false;
    }
    if (m_rtv) {
        m_rtv->Release();
        m_rtv = NULL;
    }
    hr = m_dev->CreateRenderTargetView(back, NULL, &m_rtv);
    back->Release();
    if (FAILED(hr)) {
        LOGERR("D3D11Present: CreateRenderTargetView failed (0x%08x)", hr);
        return false;
    }
    return true;
}

bool D3D11PresentBackend::Draw(int slotA, int slotB, float w) {
    if (slotA < 0 || slotA >= m_ringSlots || slotB < 0 || slotB >= m_ringSlots) return false;
    if (!m_rtv) return false;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_ctx->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        float* p = (float*)mapped.pData;
        p[0] = w; p[1] = 0.0f; p[2] = 0.0f; p[3] = 0.0f;
        m_ctx->Unmap(m_cb, 0);
    }

    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = (float)m_width; vp.Height = (float)m_height;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;

    ID3D11ShaderResourceView* srvs[2] = { m_ringSrv[slotA], m_ringSrv[slotB] };
    m_ctx->OMSetRenderTargets(1, &m_rtv, NULL);
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetInputLayout(NULL);
    m_ctx->VSSetShader(m_vs, NULL, 0);
    m_ctx->PSSetShader(m_ps, NULL, 0);
    m_ctx->PSSetShaderResources(0, 2, srvs);
    m_ctx->PSSetSamplers(0, 1, &m_sampler);
    m_ctx->PSSetConstantBuffers(0, 1, &m_cb);
    m_ctx->Draw(3, 0);

    // Unbind so the ring aliases are not still bound as inputs next present (they are also
    // render targets on the capture device, and a stale binding is a silent hazard).
    ID3D11ShaderResourceView* nullSrvs[2] = { NULL, NULL };
    m_ctx->PSSetShaderResources(0, 2, nullSrvs);
    return true;
}

bool D3D11PresentBackend::DrawMarker(const bool cells[FrameMarker::kCells]) {
    if (!m_rtv) return false;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(m_ctx->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    UINT* words = (UINT*)mapped.pData;
    words[0] = words[1] = words[2] = words[3] = 0;
    for (int i = 0; i < FrameMarker::kCells; i++) {
        if (cells[i]) words[i >> 5] |= 1u << (i & 31);
    }
    m_ctx->Unmap(m_cb, 0);

    // The viewport IS the strip: the shader reads cell positions off uv, so the strip lands
    // at exactly the fraction of the width the decoder computes.
    D3D11_VIEWPORT vp;
    vp.TopLeftX = (float)m_markerRect.left;
    vp.TopLeftY = (float)m_markerRect.top;
    vp.Width = (float)(m_markerRect.right - m_markerRect.left);
    vp.Height = (float)(m_markerRect.bottom - m_markerRect.top);
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;

    m_ctx->OMSetRenderTargets(1, &m_rtv, NULL);
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetInputLayout(NULL);
    m_ctx->VSSetShader(m_vs, NULL, 0);
    m_ctx->PSSetShader(m_markerPs, NULL, 0);
    m_ctx->PSSetConstantBuffers(0, 1, &m_cb);
    m_ctx->Draw(3, 0);
    return true;
}

void D3D11PresentBackend::SampleStats() {
    DXGI_FRAME_STATISTICS fs;
    ZeroMemory(&fs, sizeof(fs));
    const HRESULT hr = m_swapChain->GetFrameStatistics(&fs);
    if (FAILED(hr)) {
        // DXGI_ERROR_FRAME_STATISTICS_DISJOINT on the first call and across mode changes is
        // a sequence break, not an error: drop the anchor and start a new run.
        m_lastSyncRefresh = 0;
        return;
    }
    if (m_lastSyncRefresh != 0 && fs.SyncRefreshCount > m_lastSyncRefresh) {
        const UINT elapsed = fs.SyncRefreshCount - m_lastSyncRefresh;
        if (elapsed > 1) m_missedRefreshes += (elapsed - 1);
    }
    m_lastSyncRefresh = fs.SyncRefreshCount;
    m_statsSamples++;
    if ((m_statsSamples % 1800) == 0) {
        LOG("d3d11 presentstats: present=%u presentRefresh=%u syncRefresh=%u "
            "missedRefreshes=%lld over %lld presents",
            fs.PresentCount, fs.PresentRefreshCount, fs.SyncRefreshCount,
            m_missedRefreshes, m_statsSamples);
    }
}

void D3D11PresentBackend::Compose(const FrameBracket& bracket, CompositeOutcome* out) {
    // The generated frame is offered to the policy only after PLACEMENT says a substitution
    // is on the table, and only when the ring already screened it against the driver's
    // change map. There is no content check on this device: the D3D9 compositor's GPU
    // readback is the expensive path the change map exists to replace, and an unscreened
    // candidate is skipped rather than trusted.
    policy::BracketInfo info = bracket.info;
    if (m_subGen && info.hasGen) {
        if (policy::GeneratedCandidateOnTarget(info, m_compState, *m_cfg)) {
            m_genSub.offered++;
            info.genUsable = bracket.genScreened;
            if (!bracket.genScreened) m_genSub.skippedUnscreened++;
        }
    } else {
        info.hasGen = false;   // disarmed: the policy must not see a candidate at all
    }
    const policy::CompositeDecision d = policy::DecideComposite(info, m_compState, *m_cfg);

    out->pickLabel = policy::PickLabel(policy::Pick::None);
    out->opLabel = policy::CompositeLabel(d.op);
    out->pickCode = 0;
    out->weightQ = 0;
    out->synthesized = false;
    out->opWeight = 0.0;
    out->synthUs = -1;
    out->synthExec = NULL;
    out->pixelExec = 0;

    int slotA = -1, slotB = -1;
    float w = 0.0f;
    int exec = 0;
    switch (d.op) {
        case policy::CompositeOp::PassthroughBefore:
            slotA = slotB = bracket.beforeSlot;
            break;
        case policy::CompositeOp::PassthroughAfter:
            slotA = slotB = bracket.afterSlot;
            out->weightQ = 15;
            out->opWeight = 1.0;
            break;
        case policy::CompositeOp::PassthroughGenerated:
            // A frame the driver rendered at this instant, presented sharp. Its pixels are
            // one real frame, so synthesized/pixelExec stay at the passthrough values and
            // op=pass-gen carries the provenance.
            slotA = slotB = bracket.genSlot;
            m_genSub.substituted++;
            break;
        case policy::CompositeOp::Synthesize:
            slotA = bracket.beforeSlot;
            slotB = bracket.afterSlot;
            w = (float)d.weight;
            exec = kExecBlend;
            out->synthesized = true;
            out->weightQ = (int)(d.weight * 15.0 + 0.5);
            out->opWeight = d.weight;
            out->synthExec = SynthExecLabel(kExecBlend);
            out->pixelExec = kExecBlend;
            break;
        default:
            // Hold / HoldComb: re-draw what the last present drew. The fallback chain covers
            // startup, before anything has been output.
            if (m_haveLast) {
                slotA = m_lastSlotA;
                slotB = m_lastSlotB;
                w = m_lastWeight;
                exec = m_lastExec;
            } else if (bracket.info.hasBefore) {
                slotA = slotB = bracket.beforeSlot;
            } else if (bracket.info.hasAfter) {
                slotA = slotB = bracket.afterSlot;
            }
            out->synthesized = exec != 0;
            out->pixelExec = exec;
            break;
    }

    // Acquired even when there is nothing to draw, so the marker lands on THIS present's
    // buffer and never on the one already handed to the display.
    if (!AcquireBackBuffer()) {
        m_drawFailures++;
        return;
    }
    if (slotA < 0) return;   // nothing presentable yet; the buffer stays as created
    if (!Draw(slotA, slotB, w)) {
        m_drawFailures++;
        if (m_drawFailures == 1 || (m_drawFailures % 600) == 0) {
            LOGERR("D3D11Present: draw failed (%lld so far); the buffer presents as is",
                   m_drawFailures);
        }
        return;
    }
    m_lastSlotA = slotA;
    m_lastSlotB = slotB;
    m_lastWeight = w;
    m_lastExec = exec;
    m_haveLast = true;
}

long long D3D11PresentBackend::BurnMarker(const CompositeOutcome& out) {
    if (!m_mark) return -1;
    unsigned int burned = 0;
    bool cells[FrameMarker::kCells];
    if (!m_marker.Next(out.pickCode, out.weightQ, out.synthesized, kCompositorIdBlend,
                       out.pixelExec, &burned, cells)) {
        return burned;
    }
    if (!DrawMarker(cells)) {
        m_drawFailures++;
    }
    return burned;
}

void D3D11PresentBackend::Present(bool vsync) {
    if (!m_enabled) return;
    // Sync interval 1 IS the frame-pacing wait, and under independent flip it is the sink's
    // vblank rather than DWM's compose clock. That substitution is the entire point of this
    // backend.
    const HRESULT hr = m_swapChain->Present(vsync ? 1 : 0, 0);
    m_presents++;
    if (hr == DXGI_STATUS_OCCLUDED) {
        // Nothing was shown and nothing was waited on, so without this the loop spins at
        // full speed for as long as the window stays hidden.
        Sleep(1);
    }
    if (FAILED(hr) || hr == DXGI_STATUS_OCCLUDED) {
        m_presentFailures++;
        if (m_presentFailures == 1 || (m_presentFailures % 600) == 0) {
            LOGERR("D3D11Present: Present returned 0x%08lx (%lld so far)",
                   (unsigned long)hr, m_presentFailures);
        }
    }
    SampleStats();
}

void D3D11PresentBackend::LogSummary() const {
    if (!m_enabled) return;
    LOG("d3d11 presentstats summary: %lld refreshes showed no new frame over %lld sampled "
        "presents (%.3f/s at a 60 Hz sink); %lld presents, %lld reported a problem, "
        "%lld draws failed",
        m_missedRefreshes, m_statsSamples,
        m_statsSamples > 0 ? (double)m_missedRefreshes * 60.0 / (double)m_statsSamples : 0.0,
        m_presents, m_presentFailures, m_drawFailures);
    if (m_subGen) {
        LOG("subgen summary: %lld substituted, %lld offered, %lld skipped unscreened "
            "(no content check on the D3D11 present path)",
            m_genSub.substituted, m_genSub.offered, m_genSub.skippedUnscreened);
    }
}
