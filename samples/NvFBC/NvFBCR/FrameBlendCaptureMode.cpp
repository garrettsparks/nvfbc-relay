#include "FrameBlendCaptureMode.h"
#include <SimpleLogger.h>
#include <d3dcompiler.h>
#include <string>

// External global variables
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;

FrameBlendCaptureMode::FrameBlendCaptureMode(float framerate)
    : m_currentHistoryIndex(0)
    , m_captureTarget(NULL)
    , m_captureTexture(NULL)
    , m_vertexShader(NULL)
    , m_pixelShader(NULL)
    , m_vertexDeclaration(NULL)
    , m_quadVertexBuffer(NULL)
    , m_targetFramerate(framerate)
    , m_device(NULL)
    , m_shaderAvailable(false)
{
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        m_frameHistory[i].surface = NULL;
        m_frameHistory[i].valid = false;
        m_frameHistory[i].timestamp.QuadPart = 0;
        m_frameTextures[i] = NULL;
    }
    m_perfFreq.QuadPart = 0;
    m_lastGrabTime.QuadPart = 0;
}

FrameBlendCaptureMode::~FrameBlendCaptureMode() {
    // Release shader resources
    if (m_pixelShader) {
        m_pixelShader->Release();
        m_pixelShader = NULL;
    }
    if (m_vertexShader) {
        m_vertexShader->Release();
        m_vertexShader = NULL;
    }
    if (m_vertexDeclaration) {
        m_vertexDeclaration->Release();
        m_vertexDeclaration = NULL;
    }
    if (m_quadVertexBuffer) {
        m_quadVertexBuffer->Release();
        m_quadVertexBuffer = NULL;
    }

    // Release textures and surfaces
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        if (m_frameTextures[i]) {
            m_frameTextures[i]->Release();
            m_frameTextures[i] = NULL;
        }
        if (m_frameHistory[i].surface) {
            m_frameHistory[i].surface->Release();
            m_frameHistory[i].surface = NULL;
        }
    }
    if (m_captureTexture) {
        m_captureTexture->Release();
        m_captureTexture = NULL;
    }
    if (m_captureTarget) {
        m_captureTarget->Release();
        m_captureTarget = NULL;
    }
}

UINT FrameBlendCaptureMode::GetPresentationInterval() const {
    return D3DPRESENT_INTERVAL_IMMEDIATE;
}

bool FrameBlendCaptureMode::Setup() {
    m_device = g_pD3D9Device;

    // Compile and create shaders
    if (!CompileAndCreateShaders()) {
        LOGERR("Failed to create shaders for blend mode");
        return false;
    }

    // Create capture texture (where NvFBC will write)
    HRESULT hr = m_device->CreateTexture(
        BUF_WIDTH, BUF_HEIGHT,
        1,
        D3DUSAGE_RENDERTARGET,
        D3DFMT_A2B10G10R10,
        D3DPOOL_DEFAULT,
        &m_captureTexture,
        NULL);

    if (FAILED(hr)) {
        LOGERR("Failed to create capture texture (error: 0x%08x)", hr);
        return false;
    }

    hr = m_captureTexture->GetSurfaceLevel(0, &m_captureTarget);
    if (FAILED(hr)) {
        LOGERR("Failed to get surface from capture texture (error: 0x%08x)", hr);
        return false;
    }

    // Create frame history textures
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        hr = m_device->CreateTexture(
            BUF_WIDTH, BUF_HEIGHT,
            1,
            D3DUSAGE_RENDERTARGET,
            D3DFMT_A2B10G10R10,
            D3DPOOL_DEFAULT,
            &m_frameTextures[i],
            NULL);

        if (FAILED(hr)) {
            LOGERR("Failed to create frame texture %d (error: 0x%08x)", i, hr);
            return false;
        }

        hr = m_frameTextures[i]->GetSurfaceLevel(0, &m_frameHistory[i].surface);
        if (FAILED(hr)) {
            LOGERR("Failed to get surface from frame texture %d (error: 0x%08x)", i, hr);
            return false;
        }
    }

    QueryPerformanceFrequency(&m_perfFreq);

    // Initialize static render states
    if (!InitBlendingRenderStates()) {
        LOGERR("Failed to initialize blending render states");
        return false;
    }

    m_shaderAvailable = true;

    LOG("Frame blend mode initialized - target framerate: %.2f fps", m_targetFramerate);
    LOG("GPU pixel shader blending enabled (blend threshold: %.1f)", BLEND_WEIGHT_THRESHOLD);
    return true;
}

void FrameBlendCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    MSG msg;
    LARGE_INTEGER nextPresentTime, currentTime;
    QueryPerformanceCounter(&nextPresentTime);
    LONGLONG ticksPerFrame = (LONGLONG)(m_perfFreq.QuadPart / m_targetFramerate);

    // Update NvFBC to write to our capture target
    NVFBC_TODX9VID_OUT_BUF outBuf[1];
    outBuf[0].pPrimary = m_captureTarget;

    NVFBC_TODX9VID_SETUP_PARAMS setupParams = {};
    setupParams.dwVersion = NVFBC_TODX9VID_SETUP_PARAMS_V3_VER;
    setupParams.bWithHWCursor = 1;
    setupParams.bStereoGrab = 0;
    setupParams.bDiffMap = 0;
    setupParams.ppBuffer = outBuf;
    setupParams.eMode = NVFBC_TODX9VID_ARGB10;
    setupParams.dwNumBuffers = 1;
    setupParams.bHDRRequest = TRUE;

    if (NVFBC_SUCCESS != nvfbcDx9->NvFBCToDx9VidSetUp(&setupParams)) {
        LOGERR("Failed to reconfigure NvFBC for frame blend mode");
        return;
    }

    // Calculate minimum interval between grabs (3x output rate)
    LONGLONG minGrabInterval = (LONGLONG)(m_perfFreq.QuadPart / (m_targetFramerate * 3.0f));

    while (TRUE)
    {
        QueryPerformanceCounter(&currentTime);

        LONGLONG timeUntilPresent = nextPresentTime.QuadPart - currentTime.QuadPart;
        DWORD msUntilPresent = timeUntilPresent > 0 ?
            (DWORD)((timeUntilPresent * 1000) / m_perfFreq.QuadPart) : 0;

        if (msUntilPresent > 5) {
            Sleep(msUntilPresent - 4);
            continue;
        }

        // Rate limit grabs to 3x output rate to ensure temporal separation
        LONGLONG timeSinceLastGrab = currentTime.QuadPart - m_lastGrabTime.QuadPart;
        if (timeSinceLastGrab < minGrabInterval) {
            Sleep(1);
            continue;
        }

        NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);
        m_lastGrabTime = currentTime;

        if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("NvFBC session invalidated - session needs to be recreated");
            break;
        }

        // Store all successful grabs to build frame history
        if (fbcRes == NVFBC_SUCCESS) {
            QueryPerformanceCounter(&currentTime);

            RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };
            device->StretchRect(
                m_captureTarget,
                &srcRect,
                m_frameHistory[m_currentHistoryIndex].surface,
                &srcRect,
                D3DTEXF_NONE);

            m_frameHistory[m_currentHistoryIndex].timestamp = currentTime;
            m_frameHistory[m_currentHistoryIndex].valid = true;

            m_currentHistoryIndex = (m_currentHistoryIndex + 1) % FRAME_HISTORY_SIZE;
        }

        QueryPerformanceCounter(&currentTime);
        if (currentTime.QuadPart >= nextPresentTime.QuadPart) {
            // Check if we have a frame AFTER the target present time for interpolation
            bool hasAfterFrame = false;
            for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
                if (m_frameHistory[i].valid &&
                    m_frameHistory[i].timestamp.QuadPart > nextPresentTime.QuadPart) {
                    hasAfterFrame = true;
                    break;
                }
            }

            // Only present if we have frames bracketing the present time
            // This adds latency but gives true temporal interpolation
            if (hasAfterFrame) {
                BlendFramesToBackbuffer(nextPresentTime, g_backbuffer);
                device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);

                nextPresentTime.QuadPart += ticksPerFrame;

                if (nextPresentTime.QuadPart < currentTime.QuadPart) {
                    nextPresentTime = currentTime;
                }
            }
            // Otherwise keep looping to capture more frames
        }

        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (msg.message == WM_QUIT)
            break;
    }
}

const char* FrameBlendCaptureMode::GetModeName() const {
    return "FrameBlend";
}

bool FrameBlendCaptureMode::CompileAndCreateShaders() {
    ID3DBlob* vsBlob = NULL;
    ID3DBlob* psBlob = NULL;
    ID3DBlob* errorBlob = NULL;

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

    const char* pixelShaderCode =
        "sampler2D texBefore : register(s0);\n"
        "sampler2D texAfter : register(s1);\n"
        "float blendWeight : register(c0);\n"
        "struct PS_INPUT {\n"
        "    float2 uv : TEXCOORD0;\n"
        "};\n"
        "float4 main(PS_INPUT input) : COLOR0 {\n"
        "    float4 colorBefore = tex2D(texBefore, input.uv);\n"
        "    float4 colorAfter = tex2D(texAfter, input.uv);\n"
        "    return lerp(colorBefore, colorAfter, blendWeight);\n"
        "}\n";

    HRESULT hr = D3DCompile(
        vertexShaderCode,
        strlen(vertexShaderCode),
        "VertexShader",
        NULL, NULL, "main", "vs_3_0",
        0, 0,
        &vsBlob,
        &errorBlob);

    if (FAILED(hr)) {
        if (errorBlob) {
            LOGERR("Vertex shader compile error: %s", (char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        }
        return false;
    }

    hr = D3DCompile(
        pixelShaderCode,
        strlen(pixelShaderCode),
        "PixelShader",
        NULL, NULL, "main", "ps_3_0",
        0, 0,
        &psBlob,
        &errorBlob);

    if (FAILED(hr)) {
        if (errorBlob) {
            LOGERR("Pixel shader compile error: %s", (char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        }
        if (vsBlob) vsBlob->Release();
        return false;
    }

    hr = m_device->CreateVertexShader((DWORD*)vsBlob->GetBufferPointer(), &m_vertexShader);
    if (FAILED(hr)) {
        LOGERR("Failed to create vertex shader (error: 0x%08x)", hr);
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    hr = m_device->CreatePixelShader((DWORD*)psBlob->GetBufferPointer(), &m_pixelShader);
    if (FAILED(hr)) {
        LOGERR("Failed to create pixel shader (error: 0x%08x)", hr);
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    vsBlob->Release();
    psBlob->Release();

    D3DVERTEXELEMENT9 vertexElements[] = {
        { 0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };

    hr = m_device->CreateVertexDeclaration(vertexElements, &m_vertexDeclaration);
    if (FAILED(hr)) {
        LOGERR("Failed to create vertex declaration (error: 0x%08x)", hr);
        return false;
    }

    // Create vertex buffer for fullscreen quad
    hr = m_device->CreateVertexBuffer(
        6 * sizeof(QuadVertex),
        D3DUSAGE_WRITEONLY,
        0,
        D3DPOOL_DEFAULT,
        &m_quadVertexBuffer,
        NULL);

    if (FAILED(hr)) {
        LOGERR("Failed to create vertex buffer (error: 0x%08x)", hr);
        return false;
    }

    QuadVertex* pVertices = NULL;
    hr = m_quadVertexBuffer->Lock(0, 0, (void**)&pVertices, 0);
    if (SUCCEEDED(hr)) {
        pVertices[0] = { -1.0f,  1.0f, 0.5f,  0.0f, 0.0f };
        pVertices[1] = {  1.0f,  1.0f, 0.5f,  1.0f, 0.0f };
        pVertices[2] = { -1.0f, -1.0f, 0.5f,  0.0f, 1.0f };
        pVertices[3] = {  1.0f,  1.0f, 0.5f,  1.0f, 0.0f };
        pVertices[4] = {  1.0f, -1.0f, 0.5f,  1.0f, 1.0f };
        pVertices[5] = { -1.0f, -1.0f, 0.5f,  0.0f, 1.0f };
        m_quadVertexBuffer->Unlock();
    } else {
        LOGERR("Failed to lock vertex buffer (error: 0x%08x)", hr);
        return false;
    }

    LOG("Shaders compiled and created successfully");
    return true;
}

bool FrameBlendCaptureMode::InitBlendingRenderStates() {
    // Configure sampler states with LINEAR filtering
    m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    m_device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    m_device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    // Set static render states
    m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_LIGHTING, FALSE);

    // Set vertex declaration and stream source
    m_device->SetVertexDeclaration(m_vertexDeclaration);
    m_device->SetStreamSource(0, m_quadVertexBuffer, 0, sizeof(QuadVertex));

    // Set shaders
    m_device->SetVertexShader(m_vertexShader);
    m_device->SetPixelShader(m_pixelShader);

    LOG("Static render states initialized (LINEAR filtering)");
    return true;
}

void FrameBlendCaptureMode::BlendFramesToBackbuffer(LARGE_INTEGER targetTime, IDirect3DSurface9* backbuffer) {
    int bestBefore = -1;
    int bestAfter = -1;
    LONGLONG smallestBeforeDiff = LLONG_MAX;
    LONGLONG smallestAfterDiff = LLONG_MAX;

    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        if (!m_frameHistory[i].valid) continue;

        LONGLONG diff = targetTime.QuadPart - m_frameHistory[i].timestamp.QuadPart;

        if (diff >= 0 && diff < smallestBeforeDiff) {
            smallestBeforeDiff = diff;
            bestBefore = i;
        }
        else if (diff < 0 && -diff < smallestAfterDiff) {
            smallestAfterDiff = -diff;
            bestAfter = i;
        }
    }

    RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };

    if (bestBefore >= 0 && bestAfter >= 0 && m_shaderAvailable) {
        LONGLONG totalDiff = m_frameHistory[bestAfter].timestamp.QuadPart -
                             m_frameHistory[bestBefore].timestamp.QuadPart;
        float weight = totalDiff > 0 ? (float)smallestBeforeDiff / (float)totalDiff : 0.5f;

        // Skip GPU blending if weight is extreme - just use the dominant frame
        if (weight < BLEND_WEIGHT_THRESHOLD) {
            // Target time very close to "before" frame - just copy it
            m_device->StretchRect(
                m_frameHistory[bestBefore].surface,
                &srcRect,
                backbuffer,
                &srcRect,
                D3DTEXF_NONE);
        }
        else if (weight > (1.0f - BLEND_WEIGHT_THRESHOLD)) {
            // Target time very close to "after" frame - just copy it
            m_device->StretchRect(
                m_frameHistory[bestAfter].surface,
                &srcRect,
                backbuffer,
                &srcRect,
                D3DTEXF_NONE);
        }
        else {
            // Do GPU blending
            m_device->SetRenderTarget(0, backbuffer);
            m_device->SetTexture(0, m_frameTextures[bestBefore]);
            m_device->SetTexture(1, m_frameTextures[bestAfter]);

            float constants[4] = { weight, 0.0f, 0.0f, 0.0f };
            m_device->SetPixelShaderConstantF(0, constants, 1);

            HRESULT hr = m_device->BeginScene();
            if (SUCCEEDED(hr)) {
                hr = m_device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                m_device->EndScene();

                if (FAILED(hr)) {
                    LOGERR("DrawPrimitive failed (error: 0x%08x), falling back to StretchRect", hr);
                    m_device->StretchRect(
                        m_frameHistory[bestBefore].surface,
                        &srcRect,
                        backbuffer,
                        &srcRect,
                        D3DTEXF_NONE);
                }
            } else {
                LOGERR("BeginScene failed (error: 0x%08x), falling back to StretchRect", hr);
                m_device->StretchRect(
                    m_frameHistory[bestBefore].surface,
                    &srcRect,
                    backbuffer,
                    &srcRect,
                    D3DTEXF_NONE);
            }

            m_device->SetTexture(0, NULL);
            m_device->SetTexture(1, NULL);
        }
    }
    else if (bestBefore >= 0) {
        m_device->StretchRect(
            m_frameHistory[bestBefore].surface,
            &srcRect,
            backbuffer,
            &srcRect,
            D3DTEXF_NONE);
    }
    else if (bestAfter >= 0) {
        m_device->StretchRect(
            m_frameHistory[bestAfter].surface,
            &srcRect,
            backbuffer,
            &srcRect,
            D3DTEXF_NONE);
    }
}