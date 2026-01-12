#include "OpticalFlowCaptureMode.h"
#include <SimpleLogger.h>
#include <d3dcompiler.h>
#include <string>

// External global variables
extern IDirect3D9Ex* g_pD3D9;
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;

OpticalFlowCaptureMode::OpticalFlowCaptureMode(float framerate)
    : m_currentHistoryIndex(0)
    , m_captureTarget(NULL)
    , m_captureTexture(NULL)
    , m_device(NULL)
    , m_vertexShader(NULL)
    , m_pixelShader(NULL)
    , m_vertexDeclaration(NULL)
    , m_quadVertexBuffer(NULL)
    , m_cuContext(NULL)
    , m_cuDevice(0)
    , m_cudaInitialized(false)
    , m_opticalFlow(NULL)
    , m_opticalFlowInitialized(false)
    , m_targetFramerate(framerate)
    , m_isVsyncMode(framerate == 0.0f)
{
    QueryPerformanceFrequency(&m_perfFreq);

    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        m_frameHistory[i].surface = NULL;
        m_frameHistory[i].texture = NULL;
        m_frameHistory[i].valid = false;
    }

    LOG("OpticalFlowCaptureMode created (framerate: %s)",
        m_isVsyncMode ? "vsync" : std::to_string(m_targetFramerate).c_str());
}

OpticalFlowCaptureMode::~OpticalFlowCaptureMode() {
    // Cleanup optical flow
    CleanupOpticalFlow();

    // Cleanup CUDA
    CleanupCuda();

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

    // Release frame history
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        if (m_frameHistory[i].texture) {
            m_frameHistory[i].texture->Release();
            m_frameHistory[i].texture = NULL;
        }
        if (m_frameHistory[i].surface) {
            m_frameHistory[i].surface->Release();
            m_frameHistory[i].surface = NULL;
        }
    }

    // Release capture resources
    if (m_captureTexture) {
        m_captureTexture->Release();
        m_captureTexture = NULL;
    }
    if (m_captureTarget) {
        m_captureTarget->Release();
        m_captureTarget = NULL;
    }
}

UINT OpticalFlowCaptureMode::GetPresentationInterval() const {
    return m_isVsyncMode ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
}

bool OpticalFlowCaptureMode::Setup() {
    m_device = g_pD3D9Device;

    LOG("Setting up OpticalFlowCaptureMode...");

    // Create capture texture and surface
    HRESULT hr = m_device->CreateTexture(
        BUF_WIDTH, BUF_HEIGHT, 1, D3DUSAGE_RENDERTARGET,
        D3DFMT_A2B10G10R10, D3DPOOL_DEFAULT, &m_captureTexture, NULL);
    if (FAILED(hr)) {
        LOGERR("Failed to create capture texture: 0x%08x", hr);
        return false;
    }

    hr = m_captureTexture->GetSurfaceLevel(0, &m_captureTarget);
    if (FAILED(hr)) {
        LOGERR("Failed to get capture surface: 0x%08x", hr);
        return false;
    }

    // Create frame history resources
    if (!CreateFrameHistoryResources()) {
        LOGERR("Failed to create frame history resources");
        return false;
    }

    // Initialize CUDA (stubbed for now)
    LOG("Initializing CUDA...");
    if (!InitCuda()) {
        LOGERR("Failed to initialize CUDA - optical flow will be disabled");
        // Continue anyway - we'll fall back to simple blending
    }

    // Initialize Optical Flow (stubbed for now)
    if (m_cudaInitialized) {
        LOG("Initializing Optical Flow...");
        if (!InitOpticalFlow()) {
            LOGERR("Failed to initialize optical flow - falling back to simple blending");
        }
    }

    // Compile and create shaders
    if (!CompileAndCreateShaders()) {
        LOGERR("Failed to create shaders");
        return false;
    }

    // Initialize render states
    if (!InitBlendingRenderStates()) {
        LOGERR("Failed to initialize render states");
        return false;
    }

    LOG("OpticalFlowCaptureMode setup complete (CUDA: %s, OpticalFlow: %s)",
        m_cudaInitialized ? "enabled" : "disabled",
        m_opticalFlowInitialized ? "enabled" : "disabled");

    return true;
}

void OpticalFlowCaptureMode::Run(
    NvFBCToDx9Vid* nvfbcDx9,
    NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
    IDirect3DDevice9Ex* device,
    HWND hwnd)
{
    MSG msg;
    LARGE_INTEGER nextPresentTime, currentTime;
    QueryPerformanceCounter(&nextPresentTime);

    LONGLONG ticksPerFrame = m_isVsyncMode ? 0 : (LONGLONG)(m_perfFreq.QuadPart / m_targetFramerate);

    // Reconfigure NvFBC to write to our capture target
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
        LOGERR("Failed to reconfigure NvFBC");
        return;
    }

    LOG("Entering optical flow capture loop (mode: %s)", m_isVsyncMode ? "vsync" : "timed");

    while (TRUE)
    {
        // Continuously capture frames (NOWAIT - never blocks)
        NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);

        if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("NvFBC session invalidated");
            break;
        }

        // Store EVERY captured frame with timestamp
        if (fbcRes == NVFBC_SUCCESS) {
            QueryPerformanceCounter(&currentTime);
            CaptureFrameToHistory(m_captureTarget, currentTime);
        }

        // Check if it's time to present
        QueryPerformanceCounter(&currentTime);
        bool shouldPresent = m_isVsyncMode || (currentTime.QuadPart >= nextPresentTime.QuadPart);

        if (shouldPresent) {
            // Blend frames with optical flow (or fall back to simple blending)
            BlendFramesWithOpticalFlow(currentTime, g_backbuffer);

            // Present
            device->PresentEx(NULL, NULL, NULL, NULL, GetPresentationInterval());

            // Schedule next present for timed mode
            if (!m_isVsyncMode) {
                QueryPerformanceCounter(&currentTime);
                nextPresentTime.QuadPart = currentTime.QuadPart + ticksPerFrame;
            }
        }

        // Process Windows messages
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (msg.message == WM_QUIT)
            break;
    }
}

const char* OpticalFlowCaptureMode::GetModeName() const {
    return "OpticalFlow";
}

// ===== Initialization Methods =====

bool OpticalFlowCaptureMode::InitCuda() {
    // TODO: Implement CUDA initialization
    // For now, just stub it out
    LOG("CUDA initialization stubbed - will implement later");
    m_cudaInitialized = false;
    return false;
}

bool OpticalFlowCaptureMode::InitOpticalFlow() {
    // TODO: Implement optical flow initialization
    // For now, just stub it out
    LOG("Optical flow initialization stubbed - will implement later");
    m_opticalFlowInitialized = false;
    return false;
}

bool OpticalFlowCaptureMode::CreateFrameHistoryResources() {
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        HRESULT hr = m_device->CreateTexture(
            BUF_WIDTH, BUF_HEIGHT, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A2B10G10R10, D3DPOOL_DEFAULT, &m_frameHistory[i].texture, NULL);
        if (FAILED(hr)) {
            LOGERR("Failed to create frame history texture %d: 0x%08x", i, hr);
            return false;
        }

        hr = m_frameHistory[i].texture->GetSurfaceLevel(0, &m_frameHistory[i].surface);
        if (FAILED(hr)) {
            LOGERR("Failed to get frame history surface %d: 0x%08x", i, hr);
            return false;
        }

        m_frameHistory[i].valid = false;
    }

    LOG("Created %d frame history buffers", FRAME_HISTORY_SIZE);
    return true;
}

bool OpticalFlowCaptureMode::CompileAndCreateShaders() {
    // Simple passthrough shaders for now (will add optical flow blending later)
    ID3DBlob* vsBlob = NULL;
    ID3DBlob* psBlob = NULL;
    ID3DBlob* errorBlob = NULL;

    const char* vertexShaderCode =
        "struct VS_INPUT { float3 pos : POSITION; float2 uv : TEXCOORD0; };\n"
        "struct VS_OUTPUT { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
        "VS_OUTPUT main(VS_INPUT input) {\n"
        "    VS_OUTPUT output;\n"
        "    output.pos = float4(input.pos, 1.0);\n"
        "    output.uv = input.uv;\n"
        "    return output;\n"
        "}\n";

    const char* pixelShaderCode =
        "sampler2D tex0 : register(s0);\n"
        "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "    return tex2D(tex0, uv);\n"
        "}\n";

    // Compile vertex shader
    HRESULT hr = D3DCompile(vertexShaderCode, strlen(vertexShaderCode), NULL, NULL, NULL,
        "main", "vs_3_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            LOGERR("Vertex shader compile error: %s", (char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        }
        return false;
    }

    hr = m_device->CreateVertexShader((DWORD*)vsBlob->GetBufferPointer(), &m_vertexShader);
    vsBlob->Release();
    if (FAILED(hr)) {
        LOGERR("Failed to create vertex shader: 0x%08x", hr);
        return false;
    }

    // Compile pixel shader
    hr = D3DCompile(pixelShaderCode, strlen(pixelShaderCode), NULL, NULL, NULL,
        "main", "ps_3_0", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            LOGERR("Pixel shader compile error: %s", (char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        }
        return false;
    }

    hr = m_device->CreatePixelShader((DWORD*)psBlob->GetBufferPointer(), &m_pixelShader);
    psBlob->Release();
    if (FAILED(hr)) {
        LOGERR("Failed to create pixel shader: 0x%08x", hr);
        return false;
    }

    // Create vertex declaration
    D3DVERTEXELEMENT9 vertexElements[] = {
        { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };

    hr = m_device->CreateVertexDeclaration(vertexElements, &m_vertexDeclaration);
    if (FAILED(hr)) {
        LOGERR("Failed to create vertex declaration: 0x%08x", hr);
        return false;
    }

    // Create fullscreen quad vertex buffer
    QuadVertex quadVertices[] = {
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f }
    };

    hr = m_device->CreateVertexBuffer(sizeof(quadVertices), D3DUSAGE_WRITEONLY,
        0, D3DPOOL_DEFAULT, &m_quadVertexBuffer, NULL);
    if (FAILED(hr)) {
        LOGERR("Failed to create vertex buffer: 0x%08x", hr);
        return false;
    }

    void* pData;
    hr = m_quadVertexBuffer->Lock(0, 0, &pData, 0);
    if (SUCCEEDED(hr)) {
        memcpy(pData, quadVertices, sizeof(quadVertices));
        m_quadVertexBuffer->Unlock();
    }

    LOG("Shaders compiled and created successfully");
    return true;
}

bool OpticalFlowCaptureMode::InitBlendingRenderStates() {
    m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);

    m_device->SetVertexShader(m_vertexShader);
    m_device->SetPixelShader(m_pixelShader);
    m_device->SetVertexDeclaration(m_vertexDeclaration);
    m_device->SetStreamSource(0, m_quadVertexBuffer, 0, sizeof(QuadVertex));

    return true;
}

// ===== Cleanup Methods =====

void OpticalFlowCaptureMode::CleanupCuda() {
    if (m_cudaInitialized) {
        // TODO: Cleanup CUDA resources
        LOG("CUDA cleanup stubbed");
        m_cudaInitialized = false;
    }
}

void OpticalFlowCaptureMode::CleanupOpticalFlow() {
    if (m_opticalFlowInitialized) {
        // TODO: Cleanup optical flow resources
        LOG("Optical flow cleanup stubbed");
        m_opticalFlowInitialized = false;
    }
}

// ===== Frame Processing Methods =====

void OpticalFlowCaptureMode::CaptureFrameToHistory(IDirect3DSurface9* source, LARGE_INTEGER timestamp) {
    // Copy captured frame to next history slot
    m_currentHistoryIndex = (m_currentHistoryIndex + 1) % FRAME_HISTORY_SIZE;

    RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };
    HRESULT hr = m_device->StretchRect(source, &srcRect,
        m_frameHistory[m_currentHistoryIndex].surface, &srcRect, D3DTEXF_NONE);

    if (SUCCEEDED(hr)) {
        m_frameHistory[m_currentHistoryIndex].timestamp = timestamp;
        m_frameHistory[m_currentHistoryIndex].valid = true;
    }
}

void OpticalFlowCaptureMode::BlendFramesWithOpticalFlow(LARGE_INTEGER targetTime, IDirect3DSurface9* backbuffer) {
    // TODO: Implement optical flow blending
    // For now, just copy the most recent valid frame

    int mostRecent = -1;
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        if (m_frameHistory[i].valid) {
            mostRecent = i;
            break;
        }
    }

    if (mostRecent >= 0) {
        // Simple copy for now
        m_device->SetRenderTarget(0, backbuffer);
        m_device->SetTexture(0, m_frameHistory[mostRecent].texture);

        HRESULT hr = m_device->BeginScene();
        if (SUCCEEDED(hr)) {
            m_device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
            m_device->EndScene();
        }

        m_device->SetTexture(0, NULL);
    }
}