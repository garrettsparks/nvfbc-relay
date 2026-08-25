#include "InterpSidecar.h"
#include <SimpleLogger.h>
#include <d3dcompiler.h>
#include "../../../third_party/NvOFSDK/NvOFFRUC.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "NvOFFRUC.lib")

// NvOFFRUC.h publishes pointer typedefs and GetProcAddress name macros only; these
// prototypes mirror the typedefs so the entry points resolve as implicit imports.
// The import library is generated from NvOFFRUC.def at build time, which makes
// NvOFFRUC.dll a load-time requirement of the process.
extern "C" {
    NvOFFRUC_STATUS CALLBACK NvOFFRUCCreate(const NvOFFRUC_CREATE_PARAM*, NvOFFRUCHandle*);
    NvOFFRUC_STATUS CALLBACK NvOFFRUCRegisterResource(NvOFFRUCHandle, const NvOFFRUC_REGISTER_RESOURCE_PARAM*);
    NvOFFRUC_STATUS CALLBACK NvOFFRUCUnregisterResource(NvOFFRUCHandle, const NvOFFRUC_UNREGISTER_RESOURCE_PARAM*);
    NvOFFRUC_STATUS CALLBACK NvOFFRUCProcess(NvOFFRUCHandle, const NvOFFRUC_PROCESS_IN_PARAMS*, const NvOFFRUC_PROCESS_OUT_PARAMS*);
    NvOFFRUC_STATUS CALLBACK NvOFFRUCDestroy(NvOFFRUCHandle);
}

extern int g_interpBackend;   // NvFBCR.cpp: -interp fruc|flow

static const int kMaxConsecutiveFailures = 3;

// Fullscreen-triangle conversion: 10-bit ring texture -> 8-bit BGRA with alpha FORCED to 1.0.
// Rendered (not compute) because B8G8R8A8 render targets are universally supported while
// typed BGRA UAVs are an optional capability. Alpha forcing is deliberate: NvFBC's desktop
// alpha is unspecified, and alpha-weighted math inside FRUC was the prime suspect for the
// prior attempt's dimmed output. Gamma note: values pass through untouched (no degamma) —
// FRUC receives the same encoding the display shows.
static const char* kConvVs =
    "void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {\n"
    "    uv = float2((id << 1) & 2, id & 2);\n"
    "    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n";
static const char* kConvPs =
    "Texture2D srcTex : register(t0);\n"
    "SamplerState srcSamp : register(s0);\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "    float4 c = srcTex.SampleLevel(srcSamp, uv, 0);\n"
    "    return float4(c.rgb, 1.0);\n"
    "}\n";

InterpSidecar::InterpSidecar()
    : m_dev11(NULL), m_ctx11(NULL)
    , m_convVs(NULL), m_convPs(NULL), m_convSampler(NULL), m_flushQuery(NULL)
    , m_frucOutput(NULL), m_sharedOutRtv(NULL), m_sharedOut11(NULL)
    , m_backend(0)
    , m_outTexture9(NULL), m_outSurface9(NULL)
    , m_frucHandle(NULL)
    , m_width(0), m_height(0), m_freqQpc(0)
    , m_inputIdx(0), m_lastFedTs(0)
    , m_consecutiveFailures(0), m_enabled(false), m_lastProcessUs(0)
{
    for (int i = 0; i < CaptureRing::RING_SIZE; i++) { m_ringAlias[i] = NULL; m_ringSrv[i] = NULL; }
    for (int i = 0; i < 2; i++) { m_frucInput[i] = NULL; m_frucInputRtv[i] = NULL; m_frucInputSrv[i] = NULL; }
    m_baseQpc.QuadPart = 0;
}

InterpSidecar::~InterpSidecar() {
    if (m_frucHandle) NvOFFRUCDestroy((NvOFFRUCHandle)m_frucHandle);
    if (m_outSurface9) m_outSurface9->Release();
    if (m_outTexture9) m_outTexture9->Release();
    if (m_sharedOutRtv) m_sharedOutRtv->Release();
    if (m_sharedOut11) m_sharedOut11->Release();
    if (m_frucOutput) m_frucOutput->Release();
    for (int i = 0; i < 2; i++) {
        if (m_frucInputSrv[i]) m_frucInputSrv[i]->Release();
        if (m_frucInputRtv[i]) m_frucInputRtv[i]->Release();
        if (m_frucInput[i]) m_frucInput[i]->Release();
    }
    for (int i = 0; i < CaptureRing::RING_SIZE; i++) {
        if (m_ringSrv[i]) m_ringSrv[i]->Release();
        if (m_ringAlias[i]) m_ringAlias[i]->Release();
    }
    if (m_flushQuery) m_flushQuery->Release();
    if (m_convSampler) m_convSampler->Release();
    if (m_convPs) m_convPs->Release();
    if (m_convVs) m_convVs->Release();
    if (m_ctx11) m_ctx11->Release();
    if (m_dev11) m_dev11->Release();
}

double InterpSidecar::QpcToSeconds(LONGLONG qpc) const {
    return (double)(qpc - m_baseQpc.QuadPart) / (double)m_freqQpc;
}

bool InterpSidecar::Setup(IDirect3DDevice9Ex* presentDevice, CaptureRing* ring,
                          int width, int height, LARGE_INTEGER baseQpc, LONGLONG freqQpc) {
    m_width = width; m_height = height; m_baseQpc = baseQpc; m_freqQpc = freqQpc;

    m_backend = g_interpBackend;

    if (!CreateDeviceAndRingAliases(ring)) return false;
    if (!CreateConversionPipeline()) return false;
    if (!CreateOutputShare(presentDevice)) return false;

    // Chosen backend only - a failed backend falls back to blend (attribution stays clean),
    // never silently to the other backend.
    if (m_backend == kInterpBackendFlow) {
        if (!m_flow.Setup(m_dev11, m_ctx11, m_width, m_height)) return false;
    } else {
        if (!CreateFruc()) return false;
    }

    m_enabled = true;
    LOG("InterpSidecar initialized - D3D11 sidecar + %s, %dx%d BGRA8 (alpha forced 1.0)",
        m_backend == kInterpBackendFlow ? "NVOFA raw flow + warp v1" : "NvOFFRUC",
        m_width, m_height);
    return true;
}

bool InterpSidecar::CreateDeviceAndRingAliases(CaptureRing* ring) {
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
        D3D11_SDK_VERSION, &m_dev11, NULL, &m_ctx11);
    if (FAILED(hr)) {
        LOGERR("InterpSidecar: D3D11CreateDevice failed (0x%08x)", hr);
        return false;
    }
    // Open every ring slot on the interp device via the handles CaptureRing retained.
    // 10-bit cross-API sharing is a risky hand-off - fail loud here if the driver refuses.
    // SlotsInUse, not RING_SIZE: the ring allocates only the slots the bracketing lag calls
    // for, so the array's tail is legitimately unallocated and aliasing it would fail here.
    m_ringSlots = ring->SlotsInUse();
    for (int i = 0; i < m_ringSlots; i++) {
        HANDLE h = ring->SlotSharedHandle(i);
        if (!h) { LOGERR("InterpSidecar: ring slot %d has no shared handle", i); return false; }
        hr = m_dev11->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void**)&m_ringAlias[i]);
        if (FAILED(hr)) {
            LOGERR("InterpSidecar: OpenSharedResource on ring slot %d failed (0x%08x)", i, hr);
            return false;
        }
        hr = m_dev11->CreateShaderResourceView(m_ringAlias[i], NULL, &m_ringSrv[i]);
        if (FAILED(hr)) {
            LOGERR("InterpSidecar: SRV for ring slot %d failed (0x%08x)", i, hr);
            return false;
        }
    }
    D3D11_QUERY_DESC qd = {}; qd.Query = D3D11_QUERY_EVENT;
    hr = m_dev11->CreateQuery(&qd, &m_flushQuery);
    if (FAILED(hr)) { LOGERR("InterpSidecar: flush query failed (0x%08x)", hr); return false; }
    return true;
}

bool InterpSidecar::CreateConversionPipeline() {
    ID3DBlob* blob = NULL; ID3DBlob* err = NULL;
    HRESULT hr = D3DCompile(kConvVs, strlen(kConvVs), "ConvVS", NULL, NULL, "main", "vs_5_0", 0, 0, &blob, &err);
    if (FAILED(hr)) {
        LOGERR("InterpSidecar: conv VS compile failed: %s", err ? (char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return false;
    }
    hr = m_dev11->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &m_convVs);
    blob->Release();
    if (FAILED(hr)) { LOGERR("InterpSidecar: conv VS create failed (0x%08x)", hr); return false; }

    hr = D3DCompile(kConvPs, strlen(kConvPs), "ConvPS", NULL, NULL, "main", "ps_5_0", 0, 0, &blob, &err);
    if (FAILED(hr)) {
        LOGERR("InterpSidecar: conv PS compile failed: %s", err ? (char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return false;
    }
    hr = m_dev11->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &m_convPs);
    blob->Release();
    if (FAILED(hr)) { LOGERR("InterpSidecar: conv PS create failed (0x%08x)", hr); return false; }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = m_dev11->CreateSamplerState(&sd, &m_convSampler);
    if (FAILED(hr)) { LOGERR("InterpSidecar: sampler failed (0x%08x)", hr); return false; }

    // Engine input textures: BGRA8, the byte layout FRUC's ARGBSurface names. The
    // byte-order expectation is a verify point - if colors come out channel-swapped,
    // switch to R8G8B8A8 here and re-run.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = m_width; td.Height = m_height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < 2; i++) {
        HRESULT hr2 = m_dev11->CreateTexture2D(&td, NULL, &m_frucInput[i]);
        if (FAILED(hr2)) { LOGERR("InterpSidecar: fruc input %d failed (0x%08x)", i, hr2); return false; }
        hr2 = m_dev11->CreateRenderTargetView(m_frucInput[i], NULL, &m_frucInputRtv[i]);
        if (FAILED(hr2)) { LOGERR("InterpSidecar: fruc input RTV %d failed (0x%08x)", i, hr2); return false; }
        hr2 = m_dev11->CreateShaderResourceView(m_frucInput[i], NULL, &m_frucInputSrv[i]);
        if (FAILED(hr2)) { LOGERR("InterpSidecar: fruc input SRV %d failed (0x%08x)", i, hr2); return false; }
    }
    HRESULT hr3 = m_dev11->CreateTexture2D(&td, NULL, &m_frucOutput);
    if (FAILED(hr3)) { LOGERR("InterpSidecar: fruc output failed (0x%08x)", hr3); return false; }
    return true;
}

bool InterpSidecar::CreateOutputShare(IDirect3DDevice9Ex* presentDevice) {
    // The flow backend renders its warp straight into the share, so the share's
    // format is the output precision: try 10-bit first and drop to 8-bit if the
    // driver refuses the reverse-direction 10-bit open. FRUC writes 8-bit ARGB into
    // its own registered texture and CopyResource into the share requires matching
    // formats, so its share is 8-bit unconditionally.
    if (m_backend == kInterpBackendFlow) {
        if (TryCreateOutputShare(presentDevice, DXGI_FORMAT_R10G10B10A2_UNORM,
                                 D3DFMT_A2B10G10R10)) {
            LOG("InterpSidecar: output share R10G10B10A2 - warp output stays 10-bit");
            return true;
        }
        LOGERR("InterpSidecar: 10-bit output share refused - warp output drops to 8-bit");
    }
    if (!TryCreateOutputShare(presentDevice, DXGI_FORMAT_B8G8R8A8_UNORM,
                              D3DFMT_A8R8G8B8)) {
        return false;
    }
    LOG("InterpSidecar: output share B8G8R8A8");
    return true;
}

bool InterpSidecar::TryCreateOutputShare(IDirect3DDevice9Ex* presentDevice,
                                         DXGI_FORMAT fmt11, D3DFORMAT fmt9) {
    // Engine output -> this shared texture -> opened on the D3D9 present device (for
    // FRUC via CopyResource; the flow warp renders into it directly). Cleans up after
    // itself on failure so the caller can retry in another format.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = m_width; td.Height = m_height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt11;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;   // legacy share: D3D9-compatible, no keyed mutex
    HRESULT hr = m_dev11->CreateTexture2D(&td, NULL, &m_sharedOut11);
    if (FAILED(hr)) {
        LOGERR("InterpSidecar: shared output create failed (DXGI fmt %d, 0x%08x)", (int)fmt11, hr);
        return false;
    }

    IDXGIResource* res = NULL;
    HANDLE shared = NULL;
    hr = m_sharedOut11->QueryInterface(__uuidof(IDXGIResource), (void**)&res);
    if (SUCCEEDED(hr) && res) { res->GetSharedHandle(&shared); res->Release(); }
    if (!shared) {
        LOGERR("InterpSidecar: no shared handle from output");
        ReleaseOutputShare();
        return false;
    }

    // Reverse-direction open (11-created -> 9-opened) is the other risky hand-off.
    hr = presentDevice->CreateTexture(m_width, m_height, 1, D3DUSAGE_RENDERTARGET,
        fmt9, D3DPOOL_DEFAULT, &m_outTexture9, &shared);
    if (FAILED(hr)) {
        LOGERR("InterpSidecar: opening shared output on D3D9 failed (D3D9 fmt %d, 0x%08x)",
            (int)fmt9, hr);
        ReleaseOutputShare();
        return false;
    }
    hr = m_outTexture9->GetSurfaceLevel(0, &m_outSurface9);
    if (FAILED(hr)) {
        LOGERR("InterpSidecar: output surface failed (0x%08x)", hr);
        ReleaseOutputShare();
        return false;
    }
    hr = m_dev11->CreateRenderTargetView(m_sharedOut11, NULL, &m_sharedOutRtv);
    if (FAILED(hr)) {
        LOGERR("InterpSidecar: shared output RTV failed (0x%08x)", hr);
        ReleaseOutputShare();
        return false;
    }
    return true;
}

void InterpSidecar::ReleaseOutputShare() {
    if (m_sharedOutRtv) { m_sharedOutRtv->Release(); m_sharedOutRtv = NULL; }
    if (m_outSurface9) { m_outSurface9->Release(); m_outSurface9 = NULL; }
    if (m_outTexture9) { m_outTexture9->Release(); m_outTexture9 = NULL; }
    if (m_sharedOut11) { m_sharedOut11->Release(); m_sharedOut11 = NULL; }
}

bool InterpSidecar::CreateFruc() {
    // The DLL is an implicit import resolved when the process loads (exe directory
    // first in the loader search order), so reaching this code means it is present;
    // log which copy won.
    HMODULE frucMod = GetModuleHandleA("NvOFFRUC.dll");
    if (frucMod) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(frucMod, path, MAX_PATH);
        LOG("InterpSidecar: FRUC library %s", path);
    }

    NvOFFRUC_CREATE_PARAM cp = {};
    cp.uiWidth = (uint32_t)m_width;
    cp.uiHeight = (uint32_t)m_height;
    cp.pDevice = m_dev11;
    cp.eResourceType = DirectX11Resource;
    cp.eSurfaceFormat = ARGBSurface;
    cp.eCUDAResourceType = CudaResourceTypeUndefined;
    NvOFFRUCHandle h = NULL;
    NvOFFRUC_STATUS st = NvOFFRUCCreate(&cp, &h);
    if (st != NvOFFRUC_SUCCESS || !h) {
        LOGERR("InterpSidecar: NvOFFRUCCreate failed (status %d)", (int)st);
        return false;
    }
    m_frucHandle = h;

    NvOFFRUC_REGISTER_RESOURCE_PARAM rp = {};
    rp.pArrResource[0] = m_frucInput[0];
    rp.pArrResource[1] = m_frucInput[1];
    rp.pArrResource[2] = m_frucOutput;
    rp.uiCount = 3;   // NvOFFRUC_MIN_RESOURCE
    rp.pD3D11FenceObj = NULL;
    st = NvOFFRUCRegisterResource(h, &rp);
    if (st != NvOFFRUC_SUCCESS) {
        LOGERR("InterpSidecar: NvOFFRUCRegisterResource failed (status %d)", (int)st);
        return false;
    }
    return true;
}

bool InterpSidecar::ConvertSlotToBgra(int ringSlot, int inputIdx) {
    if (ringSlot < 0 || ringSlot >= m_ringSlots) return false;
    D3D11_VIEWPORT vp = {};
    vp.Width = (FLOAT)m_width; vp.Height = (FLOAT)m_height; vp.MaxDepth = 1.0f;
    m_ctx11->OMSetRenderTargets(1, &m_frucInputRtv[inputIdx], NULL);
    m_ctx11->RSSetViewports(1, &vp);
    m_ctx11->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx11->IASetInputLayout(NULL);
    m_ctx11->VSSetShader(m_convVs, NULL, 0);
    m_ctx11->PSSetShader(m_convPs, NULL, 0);
    m_ctx11->PSSetShaderResources(0, 1, &m_ringSrv[ringSlot]);
    m_ctx11->PSSetSamplers(0, 1, &m_convSampler);
    m_ctx11->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = NULL;
    m_ctx11->PSSetShaderResources(0, 1, &nullSrv);
    return true;
}

void InterpSidecar::FlushD3D11() {
    m_ctx11->End(m_flushQuery);
    m_ctx11->Flush();
    BOOL done = FALSE;
    while (m_ctx11->GetData(m_flushQuery, &done, sizeof(done), 0) != S_OK) {}
}

bool InterpSidecar::Interpolate(const FrameBracket& bracket, LONGLONG targetQpc) {
    if (!m_enabled) return false;
    if (!bracket.info.hasBefore || !bracket.info.hasAfter) return false;

    if (m_backend == kInterpBackendFlow) {
        // Stateless per present: convert both bracket frames for the FLOW ENGINE (its
        // inputs must be 8-bit), then flow + warp straight into the shared output and
        // flush the cross-API boundary. The WARP samples the original 10-bit ring
        // aliases directly: the 8-bit hop exists only inside flow estimation, where
        // vector precision hides it, never in the output pixels.
        if (!ConvertSlotToBgra(bracket.beforeSlot, 0)) return false;
        if (!ConvertSlotToBgra(bracket.afterSlot, 1)) return false;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        bool ok = m_flow.Interpolate(m_ringSrv[bracket.beforeSlot], m_ringSrv[bracket.afterSlot],
                                     m_frucInput[0], m_frucInput[1],
                                     (float)bracket.weight, m_sharedOutRtv);
        QueryPerformanceCounter(&t1);
        m_lastProcessUs = (t1.QuadPart - t0.QuadPart) * 1000000 / m_freqQpc;
        if (!ok) {
            if (++m_consecutiveFailures >= kMaxConsecutiveFailures) {
                LOGERR("InterpSidecar: flow backend failing - disabling for this session");
                m_enabled = false;
            }
            return false;
        }
        FlushD3D11();
        m_consecutiveFailures = 0;
        return true;
    }

    // Feed any bracket frame FRUC hasn't seen (in timestamp order), then request the output
    // at the target instant with the newest feed. FRUC keeps its own history; feeding only
    // bracket members means it always interpolates between exactly our before/after.
    NvOFFRUC_STATUS st = NvOFFRUC_SUCCESS;
    bool fedSomething = false;

    if (bracket.info.beforeTs > m_lastFedTs) {
        if (!ConvertSlotToBgra(bracket.beforeSlot, m_inputIdx)) return false;
        NvOFFRUC_PROCESS_IN_PARAMS in = {};
        NvOFFRUC_PROCESS_OUT_PARAMS out = {};
        bool repeated = false;
        in.stFrameDataInput.pFrame = m_frucInput[m_inputIdx];
        in.stFrameDataInput.nTimeStamp = QpcToSeconds(bracket.info.beforeTs);
        in.stFrameDataInput.bHasFrameRepetitionOccurred = &repeated;
        in.bSkipWarp = 1;   // state update only; output comes with the after-feed
        st = NvOFFRUCProcess((NvOFFRUCHandle)m_frucHandle, &in, &out);
        if (st != NvOFFRUC_SUCCESS) goto fail;
        m_lastFedTs = bracket.info.beforeTs;
        m_inputIdx ^= 1;
        fedSomething = true;
    }

    {
        // Feed (or re-feed) the after frame and request the interpolated output at target.
        if (!ConvertSlotToBgra(bracket.afterSlot, m_inputIdx)) return false;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        NvOFFRUC_PROCESS_IN_PARAMS in = {};
        NvOFFRUC_PROCESS_OUT_PARAMS out = {};
        bool repeated = false;
        in.stFrameDataInput.pFrame = m_frucInput[m_inputIdx];
        in.stFrameDataInput.nTimeStamp = QpcToSeconds(bracket.info.afterTs);
        in.stFrameDataInput.bHasFrameRepetitionOccurred = &repeated;
        out.stFrameDataOutput.pFrame = m_frucOutput;
        out.stFrameDataOutput.nTimeStamp = QpcToSeconds(targetQpc);
        st = NvOFFRUCProcess((NvOFFRUCHandle)m_frucHandle, &in, &out);
        QueryPerformanceCounter(&t1);
        m_lastProcessUs = (t1.QuadPart - t0.QuadPart) * 1000000 / m_freqQpc;
        if (st != NvOFFRUC_SUCCESS) goto fail;
        if (bracket.info.afterTs > m_lastFedTs) {
            m_lastFedTs = bracket.info.afterTs;
            m_inputIdx ^= 1;
        }
        (void)fedSomething;
    }

    // Cross back to D3D9: copy into the shared texture and drain the GPU before the present
    // device reads it (T4 discipline; D3D9Ex shares are unsynchronized by spec).
    m_ctx11->CopyResource(m_sharedOut11, m_frucOutput);
    FlushD3D11();

    m_consecutiveFailures = 0;
    return true;

fail:
    m_consecutiveFailures++;
    if (m_consecutiveFailures == 1 || m_consecutiveFailures == kMaxConsecutiveFailures) {
        LOGERR("InterpSidecar: NvOFFRUCProcess failed (status %d, consecutive %d)",
            (int)st, m_consecutiveFailures);
    }
    if (m_consecutiveFailures >= kMaxConsecutiveFailures) {
        LOGERR("InterpSidecar: disabling for this session - caller falls back to lerp");
        m_enabled = false;
    }
    return false;
}
