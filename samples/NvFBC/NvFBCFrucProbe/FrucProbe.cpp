/*
 * FrucProbe — standalone answers for the five verifies gating NVOFA/FRUC integration
 * (docs/nvofa-interpolation-spec.md). Console app; NOT part of the relay or its solution.
 *
 * Part 1 (always builds, Windows SDK only) — cross-API sharing probes:
 *   TEST 1  D3D9Ex-created shared A2B10G10R10 RT texture opens on D3D11?      (verify #1)
 *   TEST 2  pixels written by D3D9 + event-flush are coherent when read via D3D11?
 *   TEST 3  D3D11-created shared R10G10B10A2 opens on D3D9Ex? (reverse direction, verify #2)
 *   TEST 4  same pair for 8-bit BGRA (the formats FRUC actually consumes)
 *
 * Part 2 (requires the NVIDIA Optical Flow SDK; define NVFRUC_SDK_AVAILABLE and provide
 * NvFRUC.h + NvFRUC.dll) — FRUC latency/quality probes (verify #3/#4/#5). The code below the
 * ifdef is a SKELETON written against the FRUC programming guide's documented call sequence;
 * struct/signature details MUST be checked against the real NvFRUC.h when the SDK is dropped
 * in (license gate — read the SDK EULA before redistributing NvFRUC.dll).
 */

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <cstdio>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")

static int g_failures = 0;

static void Report(const char* name, bool pass, HRESULT hr) {
    printf("%-58s %s (hr=0x%08lx)\n", name, pass ? "PASS" : "FAIL", (unsigned long)hr);
    if (!pass) g_failures++;
}

// Drain an event query with D3DGETDATA_FLUSH — the same manual coherency discipline the
// relay uses (T4): D3D9Ex shared surfaces are unsynchronized across APIs by specification.
static void FlushD3D9(IDirect3DDevice9Ex* dev) {
    IDirect3DQuery9* q = NULL;
    if (SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &q)) && q) {
        q->Issue(D3DISSUE_END);
        while (q->GetData(NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) {}
        q->Release();
    }
}

int main() {
    const UINT W = 1920, H = 1080;

    // ---- D3D9Ex device (source adapter 0, windowed against the desktop window) ----
    IDirect3D9Ex* d3d9 = NULL;
    Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d9);
    if (!d3d9) { printf("Direct3DCreate9Ex failed\n"); return 99; }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.BackBufferFormat = D3DFMT_A2R10G10B10;
    pp.BackBufferWidth = 4;
    pp.BackBufferHeight = 4;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.hDeviceWindow = GetDesktopWindow();

    IDirect3DDevice9Ex* dev9 = NULL;
    HRESULT hr = d3d9->CreateDeviceEx(0, D3DDEVTYPE_HAL, pp.hDeviceWindow,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, NULL, &dev9);
    if (FAILED(hr)) { printf("D3D9Ex CreateDeviceEx failed 0x%08lx\n", (unsigned long)hr); return 99; }

    // ---- D3D11 device (same adapter — default) ----
    ID3D11Device* dev11 = NULL;
    ID3D11DeviceContext* ctx11 = NULL;
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
        D3D11_SDK_VERSION, &dev11, NULL, &ctx11);
    if (FAILED(hr)) { printf("D3D11CreateDevice failed 0x%08lx\n", (unsigned long)hr); return 99; }

    // ================= TEST 1: 10-bit 9->11 share =================
    IDirect3DTexture9* tex9_10 = NULL;
    HANDLE share10 = NULL;
    hr = dev9->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A2B10G10R10,
        D3DPOOL_DEFAULT, &tex9_10, &share10);
    Report("TEST 1a: D3D9Ex shared A2B10G10R10 RT create", SUCCEEDED(hr) && share10, hr);

    ID3D11Texture2D* tex11_10 = NULL;
    if (share10) {
        hr = dev11->OpenSharedResource(share10, __uuidof(ID3D11Texture2D), (void**)&tex11_10);
        Report("TEST 1b: open on D3D11 (expect R10G10B10A2)", SUCCEEDED(hr), hr);
        if (tex11_10) {
            D3D11_TEXTURE2D_DESC d = {};
            tex11_10->GetDesc(&d);
            printf("         D3D11 sees format %d, %ux%u\n", (int)d.Format, d.Width, d.Height);
        }
    }

    // ================= TEST 2: content coherency 9 -> flush -> 11 =================
    if (tex9_10 && tex11_10) {
        IDirect3DSurface9* surf9 = NULL;
        tex9_10->GetSurfaceLevel(0, &surf9);
        // A2B10G10R10 layout: R in the low bits. 0x3FF => pure red at full 10-bit intensity.
        dev9->ColorFill(surf9, NULL, D3DCOLOR_ARGB(255, 255, 0, 0));
        FlushD3D9(dev9);

        D3D11_TEXTURE2D_DESC sd = {};
        tex11_10->GetDesc(&sd);
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        ID3D11Texture2D* staging = NULL;
        hr = dev11->CreateTexture2D(&sd, NULL, &staging);
        if (staging) {
            ctx11->CopyResource(staging, tex11_10);
            D3D11_MAPPED_SUBRESOURCE map = {};
            hr = ctx11->Map(staging, 0, D3D11_MAP_READ, 0, &map);
            if (SUCCEEDED(hr)) {
                UINT32 px = *(const UINT32*)map.pData;
                // Expect red at high intensity in R10G10B10A2 (R = low 10 bits).
                bool redish = ((px & 0x3FF) > 0x300) && (((px >> 10) & 0x3FF) < 0x080);
                Report("TEST 2 : D3D9 write + flush visible via D3D11", redish, S_OK);
                printf("         first pixel raw = 0x%08X\n", px);
                ctx11->Unmap(staging, 0);
            } else {
                Report("TEST 2 : staging map", false, hr);
            }
            staging->Release();
        } else {
            Report("TEST 2 : staging create", false, hr);
        }
        if (surf9) surf9->Release();
    } else {
        Report("TEST 2 : skipped (TEST 1 failed)", false, E_FAIL);
    }

    // ================= TEST 3: 10-bit 11->9 share (reverse direction) =================
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;   // legacy share (D3D9-compatible; no keyed mutex)
        ID3D11Texture2D* tex11r = NULL;
        hr = dev11->CreateTexture2D(&td, NULL, &tex11r);
        Report("TEST 3a: D3D11 shared R10G10B10A2 create", SUCCEEDED(hr), hr);

        HANDLE share11 = NULL;
        if (tex11r) {
            IDXGIResource* dxgiRes = NULL;
            tex11r->QueryInterface(__uuidof(IDXGIResource), (void**)&dxgiRes);
            if (dxgiRes) { dxgiRes->GetSharedHandle(&share11); dxgiRes->Release(); }
        }
        IDirect3DTexture9* tex9r = NULL;
        if (share11) {
            hr = dev9->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A2B10G10R10,
                D3DPOOL_DEFAULT, &tex9r, &share11);   // open-existing via pSharedHandle
            Report("TEST 3b: open D3D11 share on D3D9Ex", SUCCEEDED(hr), hr);
        } else {
            Report("TEST 3b: no shared handle from D3D11", false, E_FAIL);
        }
        if (tex9r) tex9r->Release();
        if (tex11r) tex11r->Release();
    }

    // ================= TEST 4: 8-bit BGRA pair (FRUC's input format family) =================
    {
        IDirect3DTexture9* tex9_8 = NULL;
        HANDLE share8 = NULL;
        hr = dev9->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT, &tex9_8, &share8);
        Report("TEST 4a: D3D9Ex shared A8R8G8B8 create", SUCCEEDED(hr) && share8, hr);
        ID3D11Texture2D* tex11_8 = NULL;
        if (share8) {
            hr = dev11->OpenSharedResource(share8, __uuidof(ID3D11Texture2D), (void**)&tex11_8);
            Report("TEST 4b: open on D3D11 (expect B8G8R8A8)", SUCCEEDED(hr), hr);
        }
        if (tex11_8) tex11_8->Release();
        if (tex9_8) tex9_8->Release();
    }

#ifdef NVFRUC_SDK_AVAILABLE
    // ================= Part 2: FRUC probes (verify #3/#4/#5) =================
    // SKELETON — written against the NVOFA FRUC programming guide's documented sequence:
    //   LoadLibrary("NvFRUC.dll") -> NvFRUCCreate -> NvFRUCRegisterResource(D3D11 textures)
    //   -> loop NvFRUCProcess(before, after, phase) -> Unregister -> Destroy.
    // BEFORE first build with the real SDK:
    //   1. Read the SDK EULA (redistribution of NvFRUC.dll — verify #5).
    //   2. Diff this skeleton's struct usage against the shipped NvFRUC.h; the guide
    //      documents the flow, not byte-exact layouts.
    // Probe plan:
    //   - two synthetic ARGB8 frames (moving box, known velocity)
    //   - NvFRUCProcess at phases 0.25 / 0.50 / 0.75 -> dump BMPs (verify #4: box lands at
    //     the interpolated position, no smearing artifacts on the synthetic content)
    //   - 1000-iteration QPC timing loop at 1920x1080 -> median/p95 per-call wall time
    //     (verify #3: budget analysis inline vs pipelined vs third-thread)
    //   - repeat with NV12 inputs (format comparison, open question #5 in the spec)
    // #include <NvFRUC.h> at file scope when enabling this section.
    // ... implementation deliberately deferred until the header is present ...
#else
    printf("\nPart 2 (FRUC latency/quality) skipped: build with NVFRUC_SDK_AVAILABLE and the\n"
           "Optical Flow SDK's NvFRUC.h/NvFRUC.dll to enable. See docs/nvofa-interpolation-spec.md.\n");
#endif

    printf("\n%d failure(s)\n", g_failures);

    if (dev11) dev11->Release();
    if (ctx11) ctx11->Release();
    if (dev9) dev9->Release();
    if (d3d9) d3d9->Release();
    return g_failures;
}
