#include "InterpSidecar.h"
#include <SimpleLogger.h>
#include <d3dcompiler.h>
#include "../../../third_party/NvOFSDK/NvOFFRUC.h"

#pragma comment(lib, "d3d11.lib")

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
    , m_frucOutput(NULL), m_sharedOut11(NULL)
    , m_outTexture9(NULL), m_outSurface9(NULL)
    , m_frucLib(NULL), m_frucHandle(NULL)
    , m_fnCreate(NULL), m_fnRegister(NULL), m_fnUnregister(NULL), m_fnProcess(NULL), m_fnDestroy(NULL)
    , m_width(0), m_height(0), m_freqQpc(0)
    , m_inputIdx(0), m_lastFedTs(0)
    , m_consecutiveFailures(0), m_enabled(false), m_lastProcessUs(0)
{
    for (int i = 0; i < CaptureRing::RING_SIZE; i++) { m_ringAlias[i] = NULL; m_ringSrv[i] = NULL; }
    for (int i = 0; i < 2; i++) { m_frucInput[i] = NULL; m_frucInputRtv[i] = NULL; }
    m_baseQpc.QuadPart = 0;
}

InterpSidecar::~InterpSidecar() {
    if (m_frucHandle && m_fnDestroy) {
        ((PtrToFuncNvOFFRUCDestroy)m_fnDestroy)((NvOFFRUCHandle)m_frucHandle);
    }
    if (m_frucLib) FreeLibrary(m_frucLib);
    if (m_outSurface9) m_outSurface9->Release();
    if (m_outTexture9) m_outTexture9->Release();
    if (m_sharedOut11) m_sharedOut11->Release();
    if (m_frucOutput) m_frucOutput->Release();
    for (int i = 0; i < 2; i++) {
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

    if (!CreateDeviceAndRingAliases(ring)) return false;
    if (!CreateConversionPipeline()) return false;
    if (!CreateOutputShare(presentDevice)) return false;
    if (!CreateFruc()) return false;

    m_enabled = true;
    LOG("InterpSidecar initialized - D3D11 sidecar + NvOFFRUC, %dx%d BGRA8 (alpha forced 1.0)",
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
    // Probe TEST 1 territory: 10-bit cross-API sharing - fail loud here if the driver refuses.
    for (int i = 0; i < CaptureRing::RING_SIZE; i++) {
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

    // FRUC-registered textures: BGRA8 (the classic "ARGB32" layout the prior CUDA attempt
    // used via D3DFMT_A8R8G8B8). Byte-order expectation of FRUC's ARGBSurface is a verify
    // point - if colors come out channel-swapped, switch to R8G8B8A8 here and re-run.
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
    }
    HRESULT hr3 = m_dev11->CreateTexture2D(&td, NULL, &m_frucOutput);
    if (FAILED(hr3)) { LOGERR("InterpSidecar: fruc output failed (0x%08x)", hr3); return false; }
    return true;
}

bool InterpSidecar::CreateOutputShare(IDirect3DDevice9Ex* presentDevice) {
    // FRUC output -> CopyResource -> this shared texture -> opened on the D3D9 present
    // device. The copy decouples "texture FRUC writes" from "texture crossing the API
    // boundary" (registering a MISC_SHARED texture with FRUC is an unnecessary unknown).
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = m_width; td.Height = m_height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;   // legacy share: D3D9-compatible, no keyed mutex
    HRESULT hr = m_dev11->CreateTexture2D(&td, NULL, &m_sharedOut11);
    if (FAILED(hr)) { LOGERR("InterpSidecar: shared output create failed (0x%08x)", hr); return false; }

    IDXGIResource* res = NULL;
    HANDLE shared = NULL;
    hr = m_sharedOut11->QueryInterface(__uuidof(IDXGIResource), (void**)&res);
    if (SUCCEEDED(hr) && res) { res->GetSharedHandle(&shared); res->Release(); }
    if (!shared) { LOGERR("InterpSidecar: no shared handle from output"); return false; }

    // Reverse-direction open (11-created -> 9-opened): probe TEST 3 territory. Fail loud.
    hr = presentDevice->CreateTexture(m_width, m_height, 1, D3DUSAGE_RENDERTARGET,
        D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_outTexture9, &shared);
    if (FAILED(hr)) {
        LOGERR("InterpSidecar: opening shared output on D3D9 failed (0x%08x) - see probe TEST 3", hr);
        return false;
    }
    hr = m_outTexture9->GetSurfaceLevel(0, &m_outSurface9);
    if (FAILED(hr)) { LOGERR("InterpSidecar: output surface failed (0x%08x)", hr); return false; }
    return true;
}

bool InterpSidecar::CreateFruc() {
    // Try app-local first (SDK redistributable beside the exe), then system paths (the
    // archived guide claims driver-shipped - contested; whichever loads, we log it).
    m_frucLib = LoadLibraryA("NvOFFRUC.dll");
    if (!m_frucLib) m_frucLib = LoadLibraryA("NvFRUC.dll");
    if (!m_frucLib) {
        LOGERR("InterpSidecar: FRUC library not found (NvOFFRUC.dll / NvFRUC.dll) - place the SDK redistributable beside the exe");
        return false;
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m_frucLib, path, MAX_PATH);
    LOG("InterpSidecar: FRUC library loaded from %s", path);

    m_fnCreate = (void*)GetProcAddress(m_frucLib, CreateProcName);
    m_fnRegister = (void*)GetProcAddress(m_frucLib, RegisterResourceProcName);
    m_fnUnregister = (void*)GetProcAddress(m_frucLib, UnregisterResourceProcName);
    m_fnProcess = (void*)GetProcAddress(m_frucLib, ProcessProcName);
    m_fnDestroy = (void*)GetProcAddress(m_frucLib, DestroyProcName);
    if (!m_fnCreate || !m_fnRegister || !m_fnProcess || !m_fnDestroy) {
        LOGERR("InterpSidecar: FRUC exports missing (create=%p reg=%p proc=%p destroy=%p)",
            m_fnCreate, m_fnRegister, m_fnProcess, m_fnDestroy);
        return false;
    }

    NvOFFRUC_CREATE_PARAM cp = {};
    cp.uiWidth = (uint32_t)m_width;
    cp.uiHeight = (uint32_t)m_height;
    cp.pDevice = m_dev11;
    cp.eResourceType = DirectX11Resource;
    cp.eSurfaceFormat = ARGBSurface;
    cp.eCUDAResourceType = CudaResourceTypeUndefined;
    NvOFFRUCHandle h = NULL;
    NvOFFRUC_STATUS st = ((PtrToFuncNvOFFRUCCreate)m_fnCreate)(&cp, &h);
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
    st = ((PtrToFuncNvOFFRUCRegisterResource)m_fnRegister)(h, &rp);
    if (st != NvOFFRUC_SUCCESS) {
        LOGERR("InterpSidecar: NvOFFRUCRegisterResource failed (status %d)", (int)st);
        return false;
    }
    return true;
}

bool InterpSidecar::ConvertSlotToBgra(int ringSlot, int inputIdx) {
    if (ringSlot < 0 || ringSlot >= CaptureRing::RING_SIZE) return false;
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
    if (!bracket.hasBefore || !bracket.hasAfter) return false;

    // Feed any bracket frame FRUC hasn't seen (in timestamp order), then request the output
    // at the target instant with the newest feed. FRUC keeps its own history; feeding only
    // bracket members means it always interpolates between exactly our before/after.
    NvOFFRUC_STATUS st = NvOFFRUC_SUCCESS;
    bool fedSomething = false;

    if (bracket.beforeTs > m_lastFedTs) {
        if (!ConvertSlotToBgra(bracket.beforeSlot, m_inputIdx)) return false;
        NvOFFRUC_PROCESS_IN_PARAMS in = {};
        NvOFFRUC_PROCESS_OUT_PARAMS out = {};
        bool repeated = false;
        in.stFrameDataInput.pFrame = m_frucInput[m_inputIdx];
        in.stFrameDataInput.nTimeStamp = QpcToSeconds(bracket.beforeTs);
        in.stFrameDataInput.bHasFrameRepetitionOccurred = &repeated;
        in.bSkipWarp = 1;   // state update only; output comes with the after-feed
        st = ((PtrToFuncNvOFFRUCProcess)m_fnProcess)((NvOFFRUCHandle)m_frucHandle, &in, &out);
        if (st != NvOFFRUC_SUCCESS) goto fail;
        m_lastFedTs = bracket.beforeTs;
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
        in.stFrameDataInput.nTimeStamp = QpcToSeconds(bracket.afterTs);
        in.stFrameDataInput.bHasFrameRepetitionOccurred = &repeated;
        out.stFrameDataOutput.pFrame = m_frucOutput;
        out.stFrameDataOutput.nTimeStamp = QpcToSeconds(targetQpc);
        st = ((PtrToFuncNvOFFRUCProcess)m_fnProcess)((NvOFFRUCHandle)m_frucHandle, &in, &out);
        QueryPerformanceCounter(&t1);
        m_lastProcessUs = (t1.QuadPart - t0.QuadPart) * 1000000 / m_freqQpc;
        if (st != NvOFFRUC_SUCCESS) goto fail;
        if (bracket.afterTs > m_lastFedTs) {
            m_lastFedTs = bracket.afterTs;
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
        LOGERR("InterpSidecar: disabling for this session - falling back to blend");
        m_enabled = false;
    }
    return false;
}
