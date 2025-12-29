/*!
 * \defgroup NvFBCR
 * \brief
 * Nvidia Framebuffer Capture Relay
 *
 * This is a simple application that captures display to a DirectX 9 surface,
 * sets up a pseudo-fullscreen window on another display,
 * and presents that surface on the window through the same DX9 context.
 *
 * It is very efficient because everything remains in VRAM and as few copies as possible are done to achieve this.
 * Many features and frills typical of capture software are omitted for speed and downstream flexibility.
 *
 * The process is as follows:
 *
 * Discover all physical heads on the system and ask the user to choose a capture and target display with a capture interval.
 * Create a window (in windowed mode) on the target display in pseudo-fullscreen, i.e., no borders or bar.
 * Create a DX9 context for that window and get a pointer to its single backbuffer as a DX9 surface (using immediate presentation).
 * Set up NvFBC capture device with the window's backbuffer as the target capture surface.
 * In a loop,
 *      Call NvFBC capture, thereby writing the captured frame directly to the window's backbuffer with no copies involved.
 *      Call Present.
 *
 *
 * Potential improvement is to use FLIPEX presentation model. Because this is a DX9 window not using FLIPEX presentation,
 *      a blt copy is performed as DWM manages the render. I couldn't get FLIPEX to work, primarily because
 *
 * https://learn.microsoft.com/en-us/windows/win32/direct3darticles/direct3d-9ex-improvements#direct3d-9ex-flip-mode-presentation
 * "If Windowed is set to TRUE and SwapEffect is set to D3DSWAPEFFECT_FLIPEX, the runtime creates one extra back buffer and rotates
 * whichever handle belongs to the buffer that becomes the front buffer at presentation time."
 *
 * This appears to be true - when modified with a backbuffer count of 2 as suggested in the FLIPEX documentation, every third frame
 * appears to be blank, which seems evidential of this extra back buffer. But I can't get a surface pointer to this buffer
 * through the dx9 device like I can for the buffers I explicitly requested on device creation.
 *
 * Stranger still, when I hold the NvFBC output buffer constant to one back buffer and just copy it to the other back buffer
 * using StretchRect on every frame, this works and does not skip every third frame, without obviously touching this third implicit back buffer.
 * But doing this copy defeats the purpose of using FLIPEX, which is to eliminate the extra copy.
 *
 */


#include <windows.h>

#include <iostream>
#include <assert.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <vector>

#include <NvFBCLibrary.h>
#include <SimpleLogger.h>
#include <NvFBC/NvFBCToDx9vid.h>
#include <AdminCheck.h>

using namespace std;


#define _CRT_SECURE_NO_WARNINGS  1


// DirectX resources
IDirect3D9Ex        *g_pD3DEx = NULL;
IDirect3DDevice9Ex  *g_pD3D9Device = NULL;
IDirect3DSurface9* g_backbuffer = NULL;

bool g_bNvFBCLibLoaded = false;

NvFBCToDx9Vid *NvFBCDX9 = NULL;
NvFBCLibrary *pNVFBCLib;

int BUF_WIDTH;
int BUF_HEIGHT;

HANDLE timer;
LARGE_INTEGER li;
int framerate = 60;

// Frame blending resources
#define FRAME_HISTORY_SIZE 2  // Reduced from 3 for better performance
struct FrameHistoryEntry {
    IDirect3DSurface9* surface;
    LARGE_INTEGER timestamp;
    bool valid;
};
FrameHistoryEntry g_frameHistory[FRAME_HISTORY_SIZE];
int g_currentHistoryIndex = 0;
IDirect3DSurface9* g_captureTarget = NULL;  // Intermediate capture surface
LARGE_INTEGER g_perfFreq;

// Shader interpolation resources
IDirect3DTexture9* g_frameTextures[FRAME_HISTORY_SIZE];
IDirect3DTexture9* g_captureTexture = NULL;
IDirect3DVertexShader9* g_vertexShader = NULL;
IDirect3DPixelShader9* g_pixelShader = NULL;
IDirect3DVertexDeclaration9* g_vertexDeclaration = NULL;
IDirect3DVertexBuffer9* g_quadVertexBuffer = NULL;
bool g_shaderInterpolationAvailable = false;

struct QuadVertex {
    float x, y, z;
    float u, v;
};

struct DisplayPosition {
    int dxAdapterIndex;
    RECT position;
    char deviceName[32];
    string friendlyName;
} source, target;

vector <DisplayPosition> displays;

void Cleanup()
{
    LOG("Cleanup started");

    //! Release the NvFBCDX9 instance
    if (NvFBCDX9)
    {
        NvFBCDX9->NvFBCToDx9VidRelease();
        NvFBCDX9 = NULL;
    }

    // Release shader resources
    if (g_pixelShader)
    {
        g_pixelShader->Release();
        g_pixelShader = NULL;
    }

    if (g_vertexShader)
    {
        g_vertexShader->Release();
        g_vertexShader = NULL;
    }

    if (g_vertexDeclaration)
    {
        g_vertexDeclaration->Release();
        g_vertexDeclaration = NULL;
    }

    if (g_quadVertexBuffer)
    {
        g_quadVertexBuffer->Release();
        g_quadVertexBuffer = NULL;
    }

    // Release frame textures
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++)
    {
        if (g_frameTextures[i])
        {
            g_frameTextures[i]->Release();
            g_frameTextures[i] = NULL;
        }
    }

    if (g_captureTexture)
    {
        g_captureTexture->Release();
        g_captureTexture = NULL;
    }

    // Release frame history surfaces
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++)
    {
        if (g_frameHistory[i].surface)
        {
            g_frameHistory[i].surface->Release();
            g_frameHistory[i].surface = NULL;
        }
    }

    if (g_captureTarget)
    {
        g_captureTarget->Release();
        g_captureTarget = NULL;
    }

    if (g_backbuffer) {
        g_backbuffer->Release();
        g_backbuffer = NULL;
    }

    if (g_pD3D9Device)
    {
        g_pD3D9Device->Release();
        g_pD3D9Device = NULL;
    }

    if (g_pD3DEx)
    {
        g_pD3DEx->Release();
        g_pD3DEx = NULL;
    }

    if (g_bNvFBCLibLoaded)
    {
        pNVFBCLib->close();
    }

    if (pNVFBCLib)
    {
        delete pNVFBCLib;
        pNVFBCLib = NULL;
    }
    if (timer)
    {
        CloseHandle(timer);
    }

    LOG("Cleanup completed");
    SimpleLogger::getInstance().flush();
}

int InitDisplays() {

    Direct3DCreate9Ex(D3D_SDK_VERSION, &g_pD3DEx);
    int adapterCount = static_cast<int>(g_pD3DEx->GetAdapterCount());

    for (int i = 0; i < adapterCount; i++) {
        MONITORINFOEX mi;
        ZeroMemory(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);
        GetMonitorInfo(g_pD3DEx->GetAdapterMonitor(i), &mi);

        DisplayPosition newMon = {
                    i,
                    mi.rcMonitor
        };
        strcpy(newMon.deviceName, mi.szDevice);
        displays.push_back(newMon);

    }


    vector<DISPLAYCONFIG_PATH_INFO> paths;
    vector<DISPLAYCONFIG_MODE_INFO> modes;
    UINT32 flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    LONG result = ERROR_SUCCESS;

    do
    {
        // Determine how many path and mode structures to allocate
        UINT32 pathCount, modeCount;
        result = GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount);

        if (result != ERROR_SUCCESS)
        {
            LOGERR("GetDisplayConfigBufferSizes failed (error: 0x%08x)", result);
            return HRESULT_FROM_WIN32(result);
        }

        // Allocate the path and mode arrays
        paths.resize(pathCount);
        modes.resize(modeCount);

        // Get all active paths and their modes
        result = QueryDisplayConfig(flags, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);

        // The function may have returned fewer paths/modes than estimated
        paths.resize(pathCount);
        modes.resize(modeCount);

        // It's possible that between the call to GetDisplayConfigBufferSizes and QueryDisplayConfig
        // that the display state changed, so loop on the case of ERROR_INSUFFICIENT_BUFFER.
    } while (result == ERROR_INSUFFICIENT_BUFFER);

    if (result != ERROR_SUCCESS)
    {
        LOGERR("QueryDisplayConfig failed (error: 0x%08x)", result);
        return HRESULT_FROM_WIN32(result);
    }

    int i = 0;
    // For each active path
    for (int i = 0; i < paths.size(); i++)
    {
        // Find the target (monitor) friendly name
        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName = {};
        targetName.header.adapterId = paths[i].targetInfo.adapterId;
        targetName.header.id = paths[i].targetInfo.id;
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        result = DisplayConfigGetDeviceInfo(&targetName.header);

        if (result != ERROR_SUCCESS)
        {
            return HRESULT_FROM_WIN32(result);
        }

        char devNameStr[64];
        char DefChar = ' ';
        WideCharToMultiByte(CP_ACP, 0, targetName.monitorFriendlyDeviceName, -1, devNameStr, 64, &DefChar, NULL);

        displays[i].friendlyName = string(devNameStr);
    }
    return 1;
}

HRESULT InitD3D9(unsigned int deviceID, HWND hwnd)
{
    HRESULT hr = S_OK;
    D3DPRESENT_PARAMETERS d3dpp;

    // Create the Direct3D9 device and the swap chain.
    ZeroMemory(&d3dpp, sizeof(d3dpp));

    d3dpp.Windowed = TRUE;
    d3dpp.BackBufferFormat = D3DFMT_A2R10G10B10;
    //d3dpp.BackBufferFormat = D3DFMT_X8R8G8B8;

    d3dpp.BackBufferWidth  = BUF_WIDTH;
    d3dpp.BackBufferHeight = BUF_HEIGHT;
    d3dpp.BackBufferCount = 1;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    //d3dpp.SwapEffect = D3DSWAPEFFECT_FLIPEX;
    d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    //d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    //d3dpp.Flags = D3DPRESENTFLAG_VIDEO;
    d3dpp.hDeviceWindow = hwnd;
    DWORD dwBehaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING;

    hr = g_pD3DEx->CreateDeviceEx(
        deviceID,
        D3DDEVTYPE_HAL,
        hwnd,
        dwBehaviorFlags,
        &d3dpp,
        NULL,
        &g_pD3D9Device);

    assert(SUCCEEDED(hr));

    return hr;
}

HRESULT InitD3D9Surfaces()
{
    HRESULT hr = E_FAIL;

    if (g_pD3D9Device)
    {

        hr = g_pD3D9Device->CreateOffscreenPlainSurface(BUF_WIDTH, BUF_HEIGHT, D3DFMT_A2B10G10R10, D3DPOOL_DEFAULT, &g_backbuffer, NULL); //D3DFMT_A8R8G8B8 D3DFMT_A2B10G10R10
        if (FAILED(hr))
        {
            LOGERR("Failed to create D3D9 surface D3DFMT_A2B10G10R10 (error: 0x%08x)", hr);
        }
        g_pD3D9Device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_backbuffer);
    }

    return hr;
}

HRESULT CompileAndCreateShaders()
{
    HRESULT hr = S_OK;
    ID3DBlob* vsBlob = NULL;
    ID3DBlob* psBlob = NULL;
    ID3DBlob* errorBlob = NULL;

    // Vertex Shader: Transform position and pass through texture coordinates
    const char* vertexShaderCode =
        "struct VS_INPUT {\n"
        "    float3 pos : POSITION;\n"
        "    float2 uv : TEXCOORD0;\n"
        "};\n"
        "struct VS_OUTPUT {\n"
        "    float4 pos : POSITION;\n"
        "    float2 uv : TEXCOORD0;\n"
        "};\n"
        "VS_OUTPUT main(VS_INPUT input) {\n"
        "    VS_OUTPUT output;\n"
        "    output.pos = float4(input.pos, 1.0);\n"
        "    output.uv = input.uv;\n"
        "    return output;\n"
        "}\n";

    // Pixel Shader: Sample two textures and blend based on weight
    const char* pixelShaderCode =
        "sampler2D texBefore : register(s0);\n"
        "sampler2D texAfter : register(s1);\n"
        "float blendWeight : register(c0);  // 0.0 = all before, 1.0 = all after\n"
        "struct PS_INPUT {\n"
        "    float2 uv : TEXCOORD0;\n"
        "};\n"
        "float4 main(PS_INPUT input) : COLOR0 {\n"
        "    float4 colorBefore = tex2D(texBefore, input.uv);\n"
        "    float4 colorAfter = tex2D(texAfter, input.uv);\n"
        "    return lerp(colorBefore, colorAfter, blendWeight);\n"
        "}\n";

    // Compile vertex shader
    hr = D3DCompile(
        vertexShaderCode,
        strlen(vertexShaderCode),
        "VertexShader",
        NULL,
        NULL,
        "main",
        "vs_3_0",  // Shader Model 3.0 for D3D9
        0,
        0,
        &vsBlob,
        &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            LOGERR("Vertex shader compile error: %s", (char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        }
        return hr;
    }

    // Compile pixel shader
    hr = D3DCompile(
        pixelShaderCode,
        strlen(pixelShaderCode),
        "PixelShader",
        NULL,
        NULL,
        "main",
        "ps_3_0",  // Shader Model 3.0 for D3D9
        0,
        0,
        &psBlob,
        &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            LOGERR("Pixel shader compile error: %s", (char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        }
        if (vsBlob) vsBlob->Release();
        return hr;
    }

    // Create vertex shader
    hr = g_pD3D9Device->CreateVertexShader(
        (DWORD*)vsBlob->GetBufferPointer(),
        &g_vertexShader);

    if (FAILED(hr))
    {
        LOGERR("Failed to create vertex shader (error: 0x%08x)", hr);
        vsBlob->Release();
        psBlob->Release();
        return hr;
    }

    // Create pixel shader
    hr = g_pD3D9Device->CreatePixelShader(
        (DWORD*)psBlob->GetBufferPointer(),
        &g_pixelShader);

    if (FAILED(hr))
    {
        LOGERR("Failed to create pixel shader (error: 0x%08x)", hr);
        g_vertexShader->Release();
        g_vertexShader = NULL;
        vsBlob->Release();
        psBlob->Release();
        return hr;
    }

    vsBlob->Release();
    psBlob->Release();

    // Create vertex declaration for programmable vertex shader
    D3DVERTEXELEMENT9 vertexElements[] = {
        { 0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };

    hr = g_pD3D9Device->CreateVertexDeclaration(vertexElements, &g_vertexDeclaration);
    if (FAILED(hr))
    {
        LOGERR("Failed to create vertex declaration (error: 0x%08x)", hr);
        g_vertexShader->Release();
        g_vertexShader = NULL;
        g_pixelShader->Release();
        g_pixelShader = NULL;
        return hr;
    }

    // Create vertex buffer for fullscreen quad (eliminates per-frame DrawPrimitiveUP overhead)
    hr = g_pD3D9Device->CreateVertexBuffer(
        6 * sizeof(QuadVertex),  // 6 vertices (2 triangles)
        D3DUSAGE_WRITEONLY,
        0,  // FVF not used (we have vertex declaration)
        D3DPOOL_DEFAULT,
        &g_quadVertexBuffer,
        NULL);

    if (FAILED(hr))
    {
        LOGERR("Failed to create vertex buffer (error: 0x%08x)", hr);
        g_vertexDeclaration->Release();
        g_vertexDeclaration = NULL;
        g_vertexShader->Release();
        g_vertexShader = NULL;
        g_pixelShader->Release();
        g_pixelShader = NULL;
        return hr;
    }

    // Fill vertex buffer with fullscreen quad data
    // D3D9 screen space: (-1,-1) is bottom-left, (1,1) is top-right
    // UV space: (0,0) is top-left, (1,1) is bottom-right
    QuadVertex* pVertices = NULL;
    hr = g_quadVertexBuffer->Lock(0, 0, (void**)&pVertices, 0);
    if (SUCCEEDED(hr))
    {
        // Triangle 1
        pVertices[0] = { -1.0f,  1.0f, 0.5f,  0.0f, 0.0f };  // Top-left
        pVertices[1] = {  1.0f,  1.0f, 0.5f,  1.0f, 0.0f };  // Top-right
        pVertices[2] = { -1.0f, -1.0f, 0.5f,  0.0f, 1.0f };  // Bottom-left

        // Triangle 2
        pVertices[3] = {  1.0f,  1.0f, 0.5f,  1.0f, 0.0f };  // Top-right
        pVertices[4] = {  1.0f, -1.0f, 0.5f,  1.0f, 1.0f };  // Bottom-right
        pVertices[5] = { -1.0f, -1.0f, 0.5f,  0.0f, 1.0f };  // Bottom-left

        g_quadVertexBuffer->Unlock();
    }
    else
    {
        LOGERR("Failed to lock vertex buffer (error: 0x%08x)", hr);
        g_quadVertexBuffer->Release();
        g_quadVertexBuffer = NULL;
        g_vertexDeclaration->Release();
        g_vertexDeclaration = NULL;
        g_vertexShader->Release();
        g_vertexShader = NULL;
        g_pixelShader->Release();
        g_pixelShader = NULL;
        return hr;
    }

    LOG("Shaders compiled and created successfully");
    return S_OK;
}

HRESULT InitFrameBlending()
{
    HRESULT hr = S_OK;

    // Initialize frame history
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++)
    {
        g_frameHistory[i].surface = NULL;
        g_frameHistory[i].valid = false;
        g_frameHistory[i].timestamp.QuadPart = 0;
        g_frameTextures[i] = NULL;
    }

    // Try to create shaders first
    hr = CompileAndCreateShaders();
    if (SUCCEEDED(hr))
    {
        g_shaderInterpolationAvailable = true;
        LOG("Shader interpolation enabled");

        // Create capture texture (where NvFBC will write)
        hr = g_pD3D9Device->CreateTexture(
            BUF_WIDTH, BUF_HEIGHT,
            1,  // mip levels
            D3DUSAGE_RENDERTARGET,
            D3DFMT_A2B10G10R10,
            D3DPOOL_DEFAULT,
            &g_captureTexture,
            NULL);

        if (FAILED(hr))
        {
            LOGERR("Failed to create capture texture (error: 0x%08x), falling back to surface mode", hr);
            g_shaderInterpolationAvailable = false;
        }
        else
        {
            // Get surface from texture for NvFBC to write to
            hr = g_captureTexture->GetSurfaceLevel(0, &g_captureTarget);
            if (FAILED(hr))
            {
                LOGERR("Failed to get surface from capture texture (error: 0x%08x)", hr);
                g_captureTexture->Release();
                g_captureTexture = NULL;
                g_shaderInterpolationAvailable = false;
            }
        }

        // Create frame history textures
        if (g_shaderInterpolationAvailable)
        {
            for (int i = 0; i < FRAME_HISTORY_SIZE; i++)
            {
                hr = g_pD3D9Device->CreateTexture(
                    BUF_WIDTH, BUF_HEIGHT,
                    1,  // mip levels
                    D3DUSAGE_RENDERTARGET,
                    D3DFMT_A2B10G10R10,
                    D3DPOOL_DEFAULT,
                    &g_frameTextures[i],
                    NULL);

                if (FAILED(hr))
                {
                    LOGERR("Failed to create frame texture %d (error: 0x%08x), falling back", i, hr);
                    // Clean up any textures we created
                    for (int j = 0; j < i; j++)
                    {
                        if (g_frameTextures[j])
                        {
                            g_frameTextures[j]->Release();
                            g_frameTextures[j] = NULL;
                        }
                    }
                    if (g_captureTarget)
                    {
                        g_captureTarget->Release();
                        g_captureTarget = NULL;
                    }
                    if (g_captureTexture)
                    {
                        g_captureTexture->Release();
                        g_captureTexture = NULL;
                    }
                    g_shaderInterpolationAvailable = false;
                    break;
                }

                // Get surface level for StretchRect operations
                hr = g_frameTextures[i]->GetSurfaceLevel(0, &g_frameHistory[i].surface);
                if (FAILED(hr))
                {
                    LOGERR("Failed to get surface from frame texture %d (error: 0x%08x)", i, hr);
                    // Continue with cleanup as above
                    for (int j = 0; j <= i; j++)
                    {
                        if (g_frameTextures[j])
                        {
                            g_frameTextures[j]->Release();
                            g_frameTextures[j] = NULL;
                        }
                    }
                    if (g_captureTarget)
                    {
                        g_captureTarget->Release();
                        g_captureTarget = NULL;
                    }
                    if (g_captureTexture)
                    {
                        g_captureTexture->Release();
                        g_captureTexture = NULL;
                    }
                    g_shaderInterpolationAvailable = false;
                    break;
                }
            }
        }
    }
    else
    {
        LOG("Shader compilation failed, using fallback mode");
        g_shaderInterpolationAvailable = false;
    }

    // Fallback: create plain surfaces if shader path failed
    if (!g_shaderInterpolationAvailable)
    {
        LOG("Initializing fallback surface-based blending");

        // Create capture target surface (where NvFBC will write)
        hr = g_pD3D9Device->CreateOffscreenPlainSurface(
            BUF_WIDTH, BUF_HEIGHT,
            D3DFMT_A2B10G10R10,
            D3DPOOL_DEFAULT,
            &g_captureTarget,
            NULL);

        if (FAILED(hr))
        {
            LOGERR("Failed to create capture target surface (error: 0x%08x)", hr);
            return hr;
        }

        // Create frame history surfaces
        for (int i = 0; i < FRAME_HISTORY_SIZE; i++)
        {
            hr = g_pD3D9Device->CreateOffscreenPlainSurface(
                BUF_WIDTH, BUF_HEIGHT,
                D3DFMT_A2B10G10R10,
                D3DPOOL_DEFAULT,
                &g_frameHistory[i].surface,
                NULL);

            if (FAILED(hr))
            {
                LOGERR("Failed to create frame history surface %d (error: 0x%08x)", i, hr);
                return hr;
            }
        }
    }

    QueryPerformanceFrequency(&g_perfFreq);

    LOG("Frame blending initialized (shader mode: %s, history size: %d)",
        g_shaderInterpolationAvailable ? "enabled" : "fallback", FRAME_HISTORY_SIZE);
    return S_OK;
}

void BlendFramesToBackbuffer(LARGE_INTEGER targetTime)
{
    // Find the two frames that bracket the target time
    int bestBefore = -1;
    int bestAfter = -1;
    LONGLONG smallestBeforeDiff = LLONG_MAX;
    LONGLONG smallestAfterDiff = LLONG_MAX;

    for (int i = 0; i < FRAME_HISTORY_SIZE; i++)
    {
        if (!g_frameHistory[i].valid) continue;

        LONGLONG diff = targetTime.QuadPart - g_frameHistory[i].timestamp.QuadPart;

        if (diff >= 0 && diff < smallestBeforeDiff)
        {
            smallestBeforeDiff = diff;
            bestBefore = i;
        }
        else if (diff < 0 && -diff < smallestAfterDiff)
        {
            smallestAfterDiff = -diff;
            bestAfter = i;
        }
    }

    RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };

    // If we have frames to blend and shader interpolation is available
    if (bestBefore >= 0 && bestAfter >= 0 && g_shaderInterpolationAvailable)
    {
        // Calculate blend weight (0.0 = use before frame, 1.0 = use after frame)
        double totalDiff = (double)(g_frameHistory[bestAfter].timestamp.QuadPart -
                                    g_frameHistory[bestBefore].timestamp.QuadPart);
        float weight = totalDiff > 0 ? (float)((double)smallestBeforeDiff / totalDiff) : 0.5f;

        // Set up rendering state
        g_pD3D9Device->SetRenderTarget(0, g_backbuffer);

        // Set textures
        g_pD3D9Device->SetTexture(0, g_frameTextures[bestBefore]);
        g_pD3D9Device->SetTexture(1, g_frameTextures[bestAfter]);

        // Configure texture sampling (linear filtering for quality)
        g_pD3D9Device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        g_pD3D9Device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        g_pD3D9Device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        g_pD3D9Device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        g_pD3D9Device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        g_pD3D9Device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        g_pD3D9Device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        g_pD3D9Device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        // Set shaders
        g_pD3D9Device->SetVertexShader(g_vertexShader);
        g_pD3D9Device->SetPixelShader(g_pixelShader);

        // Set blend weight constant
        float constants[4] = { weight, 0.0f, 0.0f, 0.0f };
        g_pD3D9Device->SetPixelShaderConstantF(0, constants, 1);

        // Set render states for proper blending
        g_pD3D9Device->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_pD3D9Device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        g_pD3D9Device->SetRenderState(D3DRS_LIGHTING, FALSE);

        // Set vertex declaration and stream source
        g_pD3D9Device->SetVertexDeclaration(g_vertexDeclaration);
        g_pD3D9Device->SetStreamSource(0, g_quadVertexBuffer, 0, sizeof(QuadVertex));

        // Begin scene for rendering
        HRESULT hr = g_pD3D9Device->BeginScene();
        if (SUCCEEDED(hr))
        {
            // Draw the quad from vertex buffer
            hr = g_pD3D9Device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

            g_pD3D9Device->EndScene();

            if (FAILED(hr))
            {
                LOGERR("DrawPrimitive failed (error: 0x%08x), falling back to StretchRect", hr);
                // Fallback to simple copy
                g_pD3D9Device->StretchRect(
                    g_frameHistory[bestBefore].surface,
                    &srcRect,
                    g_backbuffer,
                    &srcRect,
                    D3DTEXF_NONE);
            }
        }
        else
        {
            LOGERR("BeginScene failed (error: 0x%08x), falling back to StretchRect", hr);
            // Fallback to simple copy
            g_pD3D9Device->StretchRect(
                g_frameHistory[bestBefore].surface,
                &srcRect,
                g_backbuffer,
                &srcRect,
                D3DTEXF_NONE);
        }

        // Clean up state
        g_pD3D9Device->SetVertexShader(NULL);
        g_pD3D9Device->SetPixelShader(NULL);
        g_pD3D9Device->SetTexture(0, NULL);
        g_pD3D9Device->SetTexture(1, NULL);
    }
    else if (bestBefore >= 0 && bestAfter >= 0 && !g_shaderInterpolationAvailable)
    {
        // Fallback mode: just use nearest neighbor (before frame)
        g_pD3D9Device->StretchRect(
            g_frameHistory[bestBefore].surface,
            &srcRect,
            g_backbuffer,
            &srcRect,
            D3DTEXF_NONE);
    }
    else if (bestBefore >= 0)
    {
        // Only have a "before" frame, use it
        g_pD3D9Device->StretchRect(
            g_frameHistory[bestBefore].surface,
            &srcRect,
            g_backbuffer,
            &srcRect,
            D3DTEXF_NONE);
    }
    else if (bestAfter >= 0)
    {
        // Only have an "after" frame, use it
        g_pD3D9Device->StretchRect(
            g_frameHistory[bestAfter].surface,
            &srcRect,
            g_backbuffer,
            &srcRect,
            D3DTEXF_NONE);
    }
    // If no valid frames, backbuffer will just show whatever was there before
}

// this is the main message handler for the program
LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_DESTROY:
    {
        PostQuitMessage(0);
        return 0;
    } break;
    }

    return DefWindowProc(hWnd, message, wParam, lParam);
}

int ReadIntFromCmd(string prompt) {
    cout << prompt;
    string cinString;
    getline(cin, cinString);
    return cinString.empty() ? -1 : stoi(cinString);
}

bool ParseCommandLineArgs(LPSTR lpCmdLine, int* sourceIndex, int* targetIndex, int* framerateValue) {
    *sourceIndex = -1;
    *targetIndex = -1;
    *framerateValue = -1;

    if (!lpCmdLine || strlen(lpCmdLine) == 0) {
        return false;
    }

    string cmdLine(lpCmdLine);
    vector<string> args;

    // Split command line into tokens
    size_t pos = 0;
    while (pos < cmdLine.length()) {
        // Skip whitespace
        while (pos < cmdLine.length() && cmdLine[pos] == ' ') {
            pos++;
        }
        if (pos >= cmdLine.length()) break;

        // Extract token
        size_t tokenStart = pos;
        while (pos < cmdLine.length() && cmdLine[pos] != ' ') {
            pos++;
        }
        args.push_back(cmdLine.substr(tokenStart, pos - tokenStart));
    }

    bool foundAny = false;

    // Parse arguments
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "-source" && i + 1 < args.size()) {
            *sourceIndex = stoi(args[i + 1]);
            foundAny = true;
            i++; // Skip the value
        }
        else if (args[i] == "-target" && i + 1 < args.size()) {
            *targetIndex = stoi(args[i + 1]);
            foundAny = true;
            i++; // Skip the value
        }
        else if (args[i] == "-framerate" && i + 1 < args.size()) {
            *framerateValue = stoi(args[i + 1]);
            foundAny = true;
            i++; // Skip the value
        }
    }

    return foundAny;
}

void ConsoleUserInput() {
    AllocConsole();
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONIN$", "r", stdin);
    cout.clear();
    clog.clear();
    cin.clear();
    cout << endl;
    for (vector<DisplayPosition>::iterator iter = displays.begin(); iter < displays.end(); iter++) {

        cout << "Adapter index [" << iter->dxAdapterIndex << "]"
            << endl << "\t"
            << "Scaled Position Top Left [" << iter->position.left << "," << iter->position.top << "]"
            << " | Scaled Position Bottom Right [" << iter->position.right << "," << iter->position.bottom << "]"
            << endl << "\t"
            << "Identifier [" << iter->deviceName << "]"
            << endl << "\t"
            << "Name [" << iter->friendlyName << "]"
            << endl;
    }

    int sourceIndex;
    int outputIndex;
    for (sourceIndex = ReadIntFromCmd("Capture Display Index ? "); sourceIndex < 0 || sourceIndex > displays.size() - 1;) {
        sourceIndex = ReadIntFromCmd("Capture Display Index ? ");
    }
    for (outputIndex = ReadIntFromCmd("Output Display Index ? "); outputIndex < 0 || outputIndex > displays.size() - 1;) {
        outputIndex = ReadIntFromCmd("Output Display Index ? ");
    }
    cout << "Capture/Present framerate (blank to default 60fps) ? ";
    string cinString;
    getline(cin, cinString);
    if (!cinString.empty())
        framerate = stoi(cinString);

    for (vector<DisplayPosition>::iterator iter = displays.begin(); iter < displays.end(); iter++) {

        if (iter->dxAdapterIndex == outputIndex) {
            target = *iter;
        }
        if (iter->dxAdapterIndex == sourceIndex) {
            source = *iter;
        }
    }

    fclose(stdin);
    fclose(stdout);
    FreeConsole();
}

// the entry point for any Windows program
_Use_decl_annotations_ int WINAPI WinMain(HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPSTR lpCmdLine,
    int nCmdShow)
{

    if (!InitDisplays()) {
        LOGERR("Unable to enumerate display adapters");
        return -1;
    }

    // Try to parse command line arguments
    LOG("Command line received: '%s'", lpCmdLine ? lpCmdLine : "(null)");
    int argSourceIndex = -1;
    int argTargetIndex = -1;
    int argFramerate = -1;
    bool hasArgs = ParseCommandLineArgs(lpCmdLine, &argSourceIndex, &argTargetIndex, &argFramerate);
    LOG("Parsed args - hasArgs: %d, source: %d, target: %d, framerate: %d", hasArgs, argSourceIndex, argTargetIndex, argFramerate);

    // If all required args are provided via command line, use them
    if (hasArgs && argSourceIndex >= 0 && argTargetIndex >= 0 &&
        argSourceIndex < static_cast<int>(displays.size()) && argTargetIndex < static_cast<int>(displays.size())) {
        // Set source and target from command line args
        for (vector<DisplayPosition>::iterator iter = displays.begin(); iter < displays.end(); iter++) {
            if (iter->dxAdapterIndex == argTargetIndex) {
                target = *iter;
            }
            if (iter->dxAdapterIndex == argSourceIndex) {
                source = *iter;
            }
        }

        // Set framerate if provided, otherwise use default
        if (argFramerate > 0) {
            framerate = argFramerate;
        }
    } else {
        // Fall back to interactive console input
        ConsoleUserInput();
    }

    LOG("=== NvFBCR Starting ===");
    LOG("Source display: [%d] %s (%s)", source.dxAdapterIndex, source.friendlyName.c_str(), source.deviceName);
    LOG("Target display: [%d] %s (%s)", target.dxAdapterIndex, target.friendlyName.c_str(), target.deviceName);
    LOG("Framerate: %d fps", framerate);
    LOG("Buffer size: %dx%d", BUF_WIDTH, BUF_HEIGHT);

    BUF_WIDTH = target.position.right - target.position.left;
    BUF_HEIGHT = target.position.bottom - target.position.top;

    HWND hWnd;
    WNDCLASSEX wc;

    DWORD maxDisplayWidth = -1, maxDisplayHeight = -1;

    ZeroMemory(&wc, sizeof(WNDCLASSEX));

    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "WindowClass";

    // register the window class
    RegisterClassEx(&wc);

    hWnd = CreateWindowEx(NULL,
        "WindowClass",
        "NvFBCR",
        WS_EX_TOPMOST | WS_POPUP, // pseudo fullscreen //WS_OVERLAPPEDWINDOW for windowed
        target.position.left, target.position.top,    // the starting x and y positions
        BUF_WIDTH, BUF_HEIGHT,
        NULL,
        NULL,
        hInstance,
        NULL);

    ShowWindow(hWnd, nCmdShow);

    NvFBCFrameGrabInfo frameGrabInfo = { 0 };

    //! DX9 resources
    NVFBC_TODX9VID_OUT_BUF NvFBC_OutBuf[1];

    //! Load the nvfbc Library
    pNVFBCLib = new NvFBCLibrary();
    if (!pNVFBCLib->load())
    {
        LOGERR("Unable to load the NvFBC library");
        return -1;
    }

    g_bNvFBCLibLoaded = true;
    if (!SUCCEEDED(InitD3D9(source.dxAdapterIndex, hWnd)))
    {
        LOGERR("Unable to create D3D9Ex Device");
        Cleanup();
        return -1;
    }

    if (!SUCCEEDED(InitD3D9Surfaces()))
    {
        LOGERR("Unable to create D3D9Ex surfaces");
        Cleanup();
        return -1;
    }

    if (!SUCCEEDED(InitFrameBlending()))
    {
        LOGERR("Unable to initialize frame blending");
        Cleanup();
        return -1;
    }

    //! Create an instance of the NvFBCDX9 class, the first argument specifies the frame buffer
    NvFBCDX9 = (NvFBCToDx9Vid*)pNVFBCLib->create(NVFBC_TO_DX9_VID, &maxDisplayWidth, &maxDisplayHeight, 0, (void*)g_pD3D9Device);
    if (!NvFBCDX9)
    {
        // Check if NvFBC is not enabled
        NvFBCStatusEx status = {0};
        status.dwVersion = NVFBC_STATUS_VER;
        status.dwAdapterIdx = 0;
        NVFBCRESULT statusRes = pNVFBCLib->getStatus(&status);

        if (statusRes == NVFBC_SUCCESS && !status.bIsCapturePossible)
        {
            LOG("NvFBC not enabled (bIsCapturePossible=false), attempting to enable...");

            // Check if running as admin before attempting to enable
            if (!IsRunningAsAdmin())
            {
                LOGERR("Failed to enable NvFBC: Administrator privileges required");
                LOGERR("Please run NvFBCR.exe as Administrator to enable NvFBC");
                LOGERR("Alternatively, run NvFBCEnable.exe as Administrator first, then run NvFBCR.exe");

                // Show error to user in console
                AllocConsole();
                FILE* fDummy;
                freopen_s(&fDummy, "CONOUT$", "w", stdout);
                freopen_s(&fDummy, "CONIN$", "r", stdin);
                cout.clear();
                cin.clear();

                cout << "\nERROR: Failed to enable NvFBC\n";
                cout << "---------------------------------------\n";
                cout << "Administrator privileges are required to enable NvFBC.\n\n";
                cout << "Please either:\n";
                cout << "  1. Run NvFBCR.exe as Administrator, OR\n";
                cout << "  2. Run NvFBCEnable.exe as Administrator first, then run NvFBCR.exe normally\n\n";
                cout << "Press Enter to exit...";
                cin.get();

                fclose(stdin);
                fclose(stdout);
                FreeConsole();

                Cleanup();
                return -1;
            }

            NVFBCRESULT enableRes = pNVFBCLib->enable(NVFBC_STATE_ENABLE);

            if (enableRes != NVFBC_SUCCESS)
            {
                LOGERR("Failed to enable NvFBC (result: 0x%X) - cannot proceed", enableRes);
                Cleanup();
                return -1;
            }

            LOG("NvFBC enabled successfully - restarting application...");
            Cleanup();

            // Restart the process with current configuration
            STARTUPINFO si = { 0 };
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi = { 0 };
            char exePath[MAX_PATH];
            GetModuleFileNameA(NULL, exePath, MAX_PATH);

            // Build command line with current values
            // Note: lpCommandLine must include the exe name as first token when lpApplicationName is non-NULL
            // Windows will strip the first token and pass the rest to WinMain's lpCmdLine
            char newCmdLine[512];
            sprintf_s(newCmdLine, sizeof(newCmdLine), "\"%s\" -source %d -target %d -framerate %d",
                exePath, source.dxAdapterIndex, target.dxAdapterIndex, framerate);

            LOG("Relaunching with command line: '%s'", newCmdLine);
            LOG("Executable path: '%s'", exePath);

            if (CreateProcessA(NULL, newCmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
            {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                return 0;
            }
            else
            {
                LOGERR("Failed to restart process (error: %d)", GetLastError());
                return -1;
            }
        }

        LOGERR("Failed to create NvFBCToDx9Vid instance");
        LOGERR("    Requirement 1) Driver R355+ with Tesla/Quadro/GRID");
        LOGERR("    Requirement 2) Run 'NvFBCEnable -enable' after driver installation");
        Cleanup();
        return -1;
    }

    LOG("NvFBCToDX9Vid instance created successfully");

    // NvFBC writes to capture target, not directly to backbuffer
    NvFBC_OutBuf[0].pPrimary = g_captureTarget;


    NVFBC_TODX9VID_SETUP_PARAMS DX9SetupParams = {};
    DX9SetupParams.dwVersion = NVFBC_TODX9VID_SETUP_PARAMS_V3_VER;
    DX9SetupParams.bWithHWCursor = 1;
    DX9SetupParams.bStereoGrab = 0;
    DX9SetupParams.bDiffMap = 0;
    DX9SetupParams.ppBuffer = NvFBC_OutBuf;
    DX9SetupParams.eMode = NVFBC_TODX9VID_ARGB10; //NVFBC_TODX9VID_ARGB10; //NVFBC_TODX9VID_ARGB;
    DX9SetupParams.dwNumBuffers = 1;
    DX9SetupParams.bHDRRequest = TRUE;


    if (NVFBC_SUCCESS != NvFBCDX9->NvFBCToDx9VidSetUp(&DX9SetupParams))
    {
        LOGERR("Failed calling NvFBCToDx9VidSetUp()");
        Cleanup();
        return -1;
    }

    MSG msg;

    //! Setup NvFBC Grab Parameters
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS fbcDX9GrabParams = { 0 };
    NVFBCRESULT fbcRes = NVFBC_SUCCESS;
    {
        fbcDX9GrabParams.dwVersion = NVFBC_TODX9VID_GRAB_FRAME_PARAMS_V1_VER;
        fbcDX9GrabParams.dwFlags = NVFBC_TODX9VID_NOWAIT;
        fbcDX9GrabParams.eGMode = NVFBC_TODX9VID_SOURCEMODE_SCALE;
        fbcDX9GrabParams.dwTargetWidth = BUF_WIDTH;
        fbcDX9GrabParams.dwTargetHeight = BUF_HEIGHT;
        fbcDX9GrabParams.pNvFBCFrameGrabInfo = &frameGrabInfo;
    }

    LOG("Entering capture loop (frame blending enabled, history size: %d)", FRAME_HISTORY_SIZE);

    LARGE_INTEGER nextPresentTime, currentTime;
    QueryPerformanceCounter(&nextPresentTime);
    LONGLONG ticksPerFrame = g_perfFreq.QuadPart / framerate;

    while (TRUE)
    {
        QueryPerformanceCounter(&currentTime);

        // Calculate time until next present
        LONGLONG timeUntilPresent = nextPresentTime.QuadPart - currentTime.QuadPart;
        DWORD msUntilPresent = timeUntilPresent > 0 ?
            (DWORD)((timeUntilPresent * 1000) / g_perfFreq.QuadPart) : 0;

        // Smart sleep: if we're far from present time, sleep most of it
        if (msUntilPresent > 5)
        {
            Sleep(msUntilPresent - 4);  // Wake up 4ms before present time
            continue;  // Skip frame capture, just loop back to check timing
        }

        // Poll for latest frame (NOWAIT - never blocks)
        fbcRes = NvFBCDX9->NvFBCToDx9VidGrabFrame(&fbcDX9GrabParams);

        if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION)
        {
            LOGERR("NvFBC session invalidated - session needs to be recreated");
            break;
        }

        // Only store frame if we're close to present time (within 2 frame periods)
        // This reduces copies from ~224/sec to ~120/sec
        if (fbcRes == NVFBC_SUCCESS && timeUntilPresent < (ticksPerFrame * 2))
        {
            QueryPerformanceCounter(&currentTime);

            // Copy captured frame to current history slot
            RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };
            g_pD3D9Device->StretchRect(
                g_captureTarget,
                &srcRect,
                g_frameHistory[g_currentHistoryIndex].surface,
                &srcRect,
                D3DTEXF_NONE);

            // Update timestamp and mark valid
            g_frameHistory[g_currentHistoryIndex].timestamp = currentTime;
            g_frameHistory[g_currentHistoryIndex].valid = true;

            // Advance to next slot
            g_currentHistoryIndex = (g_currentHistoryIndex + 1) % FRAME_HISTORY_SIZE;
        }

        // Check if it's time to present
        QueryPerformanceCounter(&currentTime);
        if (currentTime.QuadPart >= nextPresentTime.QuadPart)
        {
            // Blend frames from history to backbuffer based on target present time
            BlendFramesToBackbuffer(nextPresentTime);

            // Present the blended result
            g_pD3D9Device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);

            // Schedule next present
            nextPresentTime.QuadPart += ticksPerFrame;

            // Prevent falling too far behind
            if (nextPresentTime.QuadPart < currentTime.QuadPart)
            {
                nextPresentTime = currentTime;
            }
        }

        // Process Windows messages
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (msg.message == WM_QUIT)
            break;
    }

    Cleanup();

    return static_cast<int>(msg.wParam);
}
