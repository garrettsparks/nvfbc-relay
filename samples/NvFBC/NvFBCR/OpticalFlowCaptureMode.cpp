#include "OpticalFlowCaptureMode.h"
#include <SimpleLogger.h>
#include <d3dcompiler.h>
#include <string>

// NVIDIA Optical Flow SDK headers
#include "nvOpticalFlowCuda.h"
#include "nvOpticalFlowCommon.h"

// External globals
extern IDirect3D9Ex* g_pD3DEx;
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;
extern DisplayPosition target;

OpticalFlowCaptureMode::OpticalFlowCaptureMode(float framerate)
    : m_cuContext(nullptr)
    , m_cuDevice(0)
    , m_opticalFlow(nullptr)
    , m_ofHandle(nullptr)
    , m_flowForwardBuffer(0)
    , m_flowBackwardBuffer(0)
    , m_flowVectorsPitch(0)
    , m_flowWidth(0)
    , m_flowHeight(0)
    , m_currentHistoryIndex(0)
    , m_captureTarget(nullptr)
    , m_captureTexture(nullptr)
    , m_captureCudaResource(nullptr)
    , m_device(nullptr)
    , m_vertexShader(nullptr)
    , m_pixelShader(nullptr)
    , m_vertexDeclaration(nullptr)
    , m_quadVertexBuffer(nullptr)
    , m_motionVectorForwardTexture(nullptr)
    , m_motionVectorBackwardTexture(nullptr)
    , m_motionVectorForwardCudaResource(nullptr)
    , m_motionVectorBackwardCudaResource(nullptr)
    , m_targetFramerate(framerate)
    , m_isVsyncMode(framerate == 0.0f)
{
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        m_frameHistory[i].d3dSurface = nullptr;
        m_frameHistory[i].d3dTexture = nullptr;
        m_frameHistory[i].cudaResource = nullptr;
        m_frameHistory[i].cudaBuffer = 0;
        m_frameHistory[i].timestamp.QuadPart = 0;
        m_frameHistory[i].valid = false;
    }
    m_perfFreq.QuadPart = 0;
}

OpticalFlowCaptureMode::~OpticalFlowCaptureMode() {
    // Release CUDA optical flow resources
    if (m_flowForwardBuffer) {
        cuMemFree(m_flowForwardBuffer);
        m_flowForwardBuffer = 0;
    }

    if (m_flowBackwardBuffer) {
        cuMemFree(m_flowBackwardBuffer);
        m_flowBackwardBuffer = 0;
    }

    if (m_opticalFlow) {
        delete m_opticalFlow;
        m_opticalFlow = nullptr;
    }

    // Unregister CUDA resources
    if (m_captureCudaResource) {
        UnregisterD3DResource(m_captureCudaResource);
    }

    if (m_motionVectorForwardCudaResource) {
        UnregisterD3DResource(m_motionVectorForwardCudaResource);
    }

    if (m_motionVectorBackwardCudaResource) {
        UnregisterD3DResource(m_motionVectorBackwardCudaResource);
    }

    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        if (m_frameHistory[i].cudaResource) {
            UnregisterD3DResource(m_frameHistory[i].cudaResource);
        }
        if (m_frameHistory[i].d3dTexture) {
            m_frameHistory[i].d3dTexture->Release();
        }
        if (m_frameHistory[i].d3dSurface) {
            m_frameHistory[i].d3dSurface->Release();
        }
    }

    // Release shader resources
    if (m_pixelShader) m_pixelShader->Release();
    if (m_vertexShader) m_vertexShader->Release();
    if (m_vertexDeclaration) m_vertexDeclaration->Release();
    if (m_quadVertexBuffer) m_quadVertexBuffer->Release();

    // Release textures
    if (m_motionVectorForwardTexture) m_motionVectorForwardTexture->Release();
    if (m_motionVectorBackwardTexture) m_motionVectorBackwardTexture->Release();
    if (m_captureTexture) m_captureTexture->Release();
    if (m_captureTarget) m_captureTarget->Release();

    // Release CUDA context
    if (m_cuContext) {
        cuCtxDestroy(m_cuContext);
    }
}

UINT OpticalFlowCaptureMode::GetPresentationInterval() const {
    return m_isVsyncMode ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
}

bool OpticalFlowCaptureMode::Setup() {
    m_device = g_pD3D9Device;

    // Detect target framerate if vsync mode
    if (m_isVsyncMode) {
        D3DDISPLAYMODE displayMode;
        HRESULT hr = g_pD3DEx->GetAdapterDisplayMode(target.dxAdapterIndex, &displayMode);
        if (SUCCEEDED(hr)) {
            m_targetFramerate = (float)displayMode.RefreshRate;
            LOG("Optical flow vsync mode detected refresh rate: %.2f Hz", m_targetFramerate);
        } else {
            LOG("Failed to detect refresh rate, defaulting to 60.0 Hz");
            m_targetFramerate = 60.0f;
        }
    }

    QueryPerformanceFrequency(&m_perfFreq);

    // Initialize CUDA
    if (!InitCuda()) {
        LOGERR("Failed to initialize CUDA");
        return false;
    }

    // Initialize NVIDIA Optical Flow
    if (!InitOpticalFlow()) {
        LOGERR("Failed to initialize NVIDIA Optical Flow");
        return false;
    }

    // Create D3D9 resources
    if (!CreateFrameHistoryResources()) {
        LOGERR("Failed to create frame history resources");
        return false;
    }

    // Compile shaders
    if (!CompileAndCreateShaders()) {
        LOGERR("Failed to create shaders");
        return false;
    }

    // Initialize render states
    if (!InitBlendingRenderStates()) {
        LOGERR("Failed to initialize render states");
        return false;
    }

    LOG("Optical flow mode initialized - target: %.2f fps", m_targetFramerate);
    return true;
}

bool OpticalFlowCaptureMode::InitCuda() {
    CUresult result = cuInit(0);
    if (result != CUDA_SUCCESS) {
        LOGERR("cuInit failed: %d", result);
        return false;
    }

    result = cuDeviceGet(&m_cuDevice, 0);
    if (result != CUDA_SUCCESS) {
        LOGERR("cuDeviceGet failed: %d", result);
        return false;
    }

    // Create CUDA context with D3D9 interop flags
    result = cuD3D9CtxCreate(&m_cuContext, &m_cuDevice, CU_CTX_SCHED_AUTO, m_device);
    if (result != CUDA_SUCCESS) {
        LOGERR("cuD3D9CtxCreate failed: %d", result);
        return false;
    }

    LOG("CUDA initialized successfully");
    return true;
}

bool OpticalFlowCaptureMode::InitOpticalFlow() {
    try {
        // Create optical flow instance
        m_opticalFlow = NvOFCuda::Create(
            m_cuContext,
            BUF_WIDTH,
            BUF_HEIGHT,
            NV_OF_BUFFER_FORMAT_ABGR,
            NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR,
            NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR,
            NV_OF_MODE_OPTICALFLOW);

        if (!m_opticalFlow) {
            LOGERR("Failed to create NvOFCuda instance");
            return false;
        }

        // Get optical flow grid size
        m_opticalFlow->GetOutputGridSizes(&m_flowWidth, &m_flowHeight);

        LOG("Optical flow initialized - grid size: %ux%u", m_flowWidth, m_flowHeight);

        // Allocate buffers for both forward and backward motion vectors
        m_flowVectorsPitch = m_flowWidth * 2 * sizeof(int16_t);

        CUresult result = cuMemAllocPitch(
            &m_flowForwardBuffer,
            &m_flowVectorsPitch,
            m_flowWidth * 2 * sizeof(int16_t),
            m_flowHeight,
            16);

        if (result != CUDA_SUCCESS) {
            LOGERR("Failed to allocate forward flow buffer: %d", result);
            return false;
        }

        result = cuMemAllocPitch(
            &m_flowBackwardBuffer,
            &m_flowVectorsPitch,
            m_flowWidth * 2 * sizeof(int16_t),
            m_flowHeight,
            16);

        if (result != CUDA_SUCCESS) {
            LOGERR("Failed to allocate backward flow buffer: %d", result);
            return false;
        }

        return true;
    }
    catch (const std::exception& e) {
        LOGERR("Exception initializing optical flow: %s", e.what());
        return false;
    }
}

bool OpticalFlowCaptureMode::CreateFrameHistoryResources() {
    HRESULT hr;

    // Create capture texture
    hr = m_device->CreateTexture(
        BUF_WIDTH, BUF_HEIGHT, 1,
        D3DUSAGE_RENDERTARGET,
        D3DFMT_A2B10G10R10,
        D3DPOOL_DEFAULT,
        &m_captureTexture,
        NULL);

    if (FAILED(hr)) {
        LOGERR("Failed to create capture texture: 0x%08x", hr);
        return false;
    }

    hr = m_captureTexture->GetSurfaceLevel(0, &m_captureTarget);
    if (FAILED(hr)) {
        LOGERR("Failed to get capture surface: 0x%08x", hr);
        return false;
    }

    RegisterD3DTextureWithCuda(m_captureTexture, &m_captureCudaResource);

    // Create frame history textures
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
        hr = m_device->CreateTexture(
            BUF_WIDTH, BUF_HEIGHT, 1,
            D3DUSAGE_RENDERTARGET,
            D3DFMT_A2B10G10R10,
            D3DPOOL_DEFAULT,
            &m_frameHistory[i].d3dTexture,
            NULL);

        if (FAILED(hr)) {
            LOGERR("Failed to create frame texture %d: 0x%08x", i, hr);
            return false;
        }

        hr = m_frameHistory[i].d3dTexture->GetSurfaceLevel(0, &m_frameHistory[i].d3dSurface);
        if (FAILED(hr)) {
            LOGERR("Failed to get frame surface %d: 0x%08x", i, hr);
            return false;
        }

        RegisterD3DTextureWithCuda(
            m_frameHistory[i].d3dTexture,
            &m_frameHistory[i].cudaResource);
    }

    // Create forward motion vector texture
    hr = m_device->CreateTexture(
        m_flowWidth, m_flowHeight, 1,
        D3DUSAGE_DYNAMIC,
        D3DFMT_G32R32F,
        D3DPOOL_DEFAULT,
        &m_motionVectorForwardTexture,
        NULL);

    if (FAILED(hr)) {
        LOGERR("Failed to create forward motion vector texture: 0x%08x", hr);
        return false;
    }

    // Create backward motion vector texture
    hr = m_device->CreateTexture(
        m_flowWidth, m_flowHeight, 1,
        D3DUSAGE_DYNAMIC,
        D3DFMT_G32R32F,
        D3DPOOL_DEFAULT,
        &m_motionVectorBackwardTexture,
        NULL);

    if (FAILED(hr)) {
        LOGERR("Failed to create backward motion vector texture: 0x%08x", hr);
        return false;
    }

    LOG("Frame history resources created successfully");
    return true;
}

void OpticalFlowCaptureMode::RegisterD3DTextureWithCuda(
    IDirect3DTexture9* texture,
    CUgraphicsResource* resource)
{
    CUresult result = cuGraphicsD3D9RegisterResource(
        resource,
        texture,
        CU_GRAPHICS_REGISTER_FLAGS_NONE);

    if (result != CUDA_SUCCESS) {
        LOGERR("Failed to register D3D texture with CUDA: %d", result);
    }
}

void OpticalFlowCaptureMode::UnregisterD3DResource(CUgraphicsResource resource) {
    if (resource) {
        cuGraphicsUnregisterResource(resource);
    }
}

/*
bool OpticalFlowCaptureMode::CompileAndCreateShaders1d() {
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

    // Motion-compensated blending pixel shader
    const char* pixelShaderCode =
        "sampler2D texBefore : register(s0);\n"
        "sampler2D texAfter : register(s1);\n"
        "sampler2D motionVectors : register(s2);\n"
        "float blendWeight : register(c0);\n"
        "float2 flowScale : register(c1);  // Scale motion vectors to pixel space\n"
        "\n"
        "struct PS_INPUT {\n"
        "    float2 uv : TEXCOORD0;\n"
        "};\n"
        "\n"
        "float4 main(PS_INPUT input) : COLOR0 {\n"
        "    // Sample motion vector (already in normalized coordinates)\n"
        "    float2 motion = tex2D(motionVectors, input.uv).xy;\n"
        "    \n"
        "    // Scale motion vector by blend weight to get intermediate position\n"
        "    // motion is in pixels/frame, so we scale it by the temporal position\n"
        "    float2 motionScaled = motion * flowScale * blendWeight;\n"
        "    \n"
        "    // Warp the 'before' frame forward in time\n"
        "    float2 uvWarped = input.uv + motionScaled;\n"
        "    \n"
        "    // Sample both frames\n"
        "    float4 colorBefore = tex2D(texBefore, uvWarped);\n"
        "    float4 colorAfter = tex2D(texAfter, input.uv);\n"
        "    \n"
        "    // Blend between warped 'before' and 'after'\n"
        "    // Could also warp 'after' backwards for bidirectional flow\n"
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
        LOGERR("Failed to create vertex shader: 0x%08x", hr);
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    hr = m_device->CreatePixelShader((DWORD*)psBlob->GetBufferPointer(), &m_pixelShader);
    if (FAILED(hr)) {
        LOGERR("Failed to create pixel shader: 0x%08x", hr);
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    vsBlob->Release();
    psBlob->Release();

    // Create vertex declaration
    D3DVERTEXELEMENT9 vertexElements[] = {
        { 0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };

    hr = m_device->CreateVertexDeclaration(vertexElements, &m_vertexDeclaration);
    if (FAILED(hr)) {
        LOGERR("Failed to create vertex declaration: 0x%08x", hr);
        return false;
    }

    // Create fullscreen quad vertex buffer
    hr = m_device->CreateVertexBuffer(
        6 * sizeof(QuadVertex),
        D3DUSAGE_WRITEONLY,
        0,
        D3DPOOL_DEFAULT,
        &m_quadVertexBuffer,
        NULL);

    if (FAILED(hr)) {
        LOGERR("Failed to create vertex buffer: 0x%08x", hr);
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
    }

    LOG("Shaders compiled and created successfully");
    return true;
}
*/

bool OpticalFlowCaptureMode::CompileAndCreateShaders() {
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

    // Bidirectional motion-compensated blending pixel shader
    const char* pixelShaderCode =
        "sampler2D texBefore : register(s0);\n"
        "sampler2D texAfter : register(s1);\n"
        "sampler2D motionForward : register(s2);  // before -> after\n"
        "sampler2D motionBackward : register(s3); // after -> before\n"
        "float blendWeight : register(c0);        // 0.0 = before, 1.0 = after\n"
        "float2 flowScale : register(c1);         // Scale motion vectors to texture space\n"
        "\n"
        "struct PS_INPUT {\n"
        "    float2 uv : TEXCOORD0;\n"
        "};\n"
        "\n"
        "float4 main(PS_INPUT input) : COLOR0 {\n"
        "    // Sample both motion vector fields\n"
        "    float2 mvForward = tex2D(motionForward, input.uv).xy;\n"
        "    float2 mvBackward = tex2D(motionBackward, input.uv).xy;\n"
        "    \n"
        "    // Warp 'before' frame forward to target time\n"
        "    float2 uvForward = input.uv + mvForward * flowScale * blendWeight;\n"
        "    float4 colorForward = tex2D(texBefore, uvForward);\n"
        "    \n"
        "    // Warp 'after' frame backward to target time\n"
        "    // Note: backward motion is already in the 'after->before' direction,\n"
        "    // so we scale by (1-weight) to get to target time\n"
        "    float2 uvBackward = input.uv + mvBackward * flowScale * (1.0 - blendWeight);\n"
        "    float4 colorBackward = tex2D(texAfter, uvBackward);\n"
        "    \n"
        "    // Simple occlusion detection using forward-backward consistency\n"
        "    // Warp the backward motion to the forward frame's position\n"
        "    float2 mvBackwardAtForward = tex2D(motionBackward, uvForward).xy;\n"
        "    \n"
        "    // Check if forward and backward flows are consistent\n"
        "    // If mvForward + mvBackward ≈ 0, then the flow is consistent (no occlusion)\n"
        "    float2 flowDiff = mvForward * flowScale + mvBackwardAtForward * flowScale;\n"
        "    float consistency = length(flowDiff);\n"
        "    \n"
        "    // If consistency is good (small flowDiff), blend equally\n"
        "    // If consistency is bad (large flowDiff), favor one direction based on weight\n"
        "    float consistencyThreshold = 0.01; // Tune this value\n"
        "    float alpha;\n"
        "    \n"
        "    if (consistency < consistencyThreshold) {\n"
        "        // Good consistency - blend based on temporal position\n"
        "        alpha = blendWeight;\n"
        "    } else {\n"
        "        // Occlusion detected - bias toward the more reliable direction\n"
        "        // Near before frame (weight < 0.5): trust forward warp more\n"
        "        // Near after frame (weight > 0.5): trust backward warp more\n"
        "        alpha = smoothstep(0.3, 0.7, blendWeight);\n"
        "    }\n"
        "    \n"
        "    // Blend between forward-warped and backward-warped results\n"
        "    return lerp(colorForward, colorBackward, alpha);\n"
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
        LOGERR("Failed to create vertex shader: 0x%08x", hr);
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    hr = m_device->CreatePixelShader((DWORD*)psBlob->GetBufferPointer(), &m_pixelShader);
    if (FAILED(hr)) {
        LOGERR("Failed to create pixel shader: 0x%08x", hr);
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    vsBlob->Release();
    psBlob->Release();

    // Create vertex declaration
    D3DVERTEXELEMENT9 vertexElements[] = {
        { 0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };

    hr = m_device->CreateVertexDeclaration(vertexElements, &m_vertexDeclaration);
    if (FAILED(hr)) {
        LOGERR("Failed to create vertex declaration: 0x%08x", hr);
        return false;
    }

    // Create fullscreen quad vertex buffer
    hr = m_device->CreateVertexBuffer(
        6 * sizeof(QuadVertex),
        D3DUSAGE_WRITEONLY,
        0,
        D3DPOOL_DEFAULT,
        &m_quadVertexBuffer,
        NULL);

    if (FAILED(hr)) {
        LOGERR("Failed to create vertex buffer: 0x%08x", hr);
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
    }

    LOG("Bidirectional optical flow shaders compiled and created successfully");
    return true;
}

bool OpticalFlowCaptureMode::InitBlendingRenderStates() {
    // Configure sampler states for all 4 textures
    for (int i = 0; i < 4; i++) {
        m_device->SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        m_device->SetSamplerState(i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }

    // Set render states
    m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_LIGHTING, FALSE);

    // Set vertex declaration and stream
    m_device->SetVertexDeclaration(m_vertexDeclaration);
    m_device->SetStreamSource(0, m_quadVertexBuffer, 0, sizeof(QuadVertex));

    // Set shaders
    m_device->SetVertexShader(m_vertexShader);
    m_device->SetPixelShader(m_pixelShader);

    LOG("Render states initialized for bidirectional flow");
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
    LONGLONG ticksPerFrame = (LONGLONG)(m_perfFreq.QuadPart / m_targetFramerate);

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
        LOGERR("Failed to reconfigure NvFBC for optical flow mode");
        return;
    }

    LOG("Entering optical flow capture loop");

    while (TRUE)
    {
        // Continuously capture frames (NOWAIT - never blocks)
        NVFBCRESULT fbcRes = nvfbcDx9->NvFBCToDx9VidGrabFrame(grabParams);

        if (fbcRes == NVFBC_ERROR_INVALIDATED_SESSION) {
            LOGERR("NvFBC session invalidated");
            break;
        }

        // Store every captured frame
        if (fbcRes == NVFBC_SUCCESS) {
            QueryPerformanceCounter(&currentTime);
            CaptureFrameToHistory(m_captureTarget, currentTime);
        }

        // Check if it's time to present
        QueryPerformanceCounter(&currentTime);
        if (currentTime.QuadPart >= nextPresentTime.QuadPart) {
            // Blend frames with optical flow
            BlendFramesWithOpticalFlow(nextPresentTime, g_backbuffer);

            // Present
            if (m_isVsyncMode) {
                device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_ONE);
            } else {
                device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);
            }

            // Update next present time
            QueryPerformanceCounter(&currentTime);
            nextPresentTime.QuadPart = currentTime.QuadPart + ticksPerFrame;
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
    static char modeName[64];
    if (m_isVsyncMode) {
        sprintf_s(modeName, sizeof(modeName), "OpticalFlow-VSync");
    } else {
        sprintf_s(modeName, sizeof(modeName), "OpticalFlow-%.2f", m_targetFramerate);
    }
    return modeName;
}

void OpticalFlowCaptureMode::CaptureFrameToHistory(
    IDirect3DSurface9* source,
    LARGE_INTEGER timestamp)
{
    int idx = m_currentHistoryIndex;

    // Copy to history surface
    RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };
    m_device->StretchRect(
        source,
        &srcRect,
        m_frameHistory[idx].d3dSurface,
        &srcRect,
        D3DTEXF_NONE);

    m_frameHistory[idx].timestamp = timestamp;
    m_frameHistory[idx].valid = true;

    m_currentHistoryIndex = (m_currentHistoryIndex + 1) % FRAME_HISTORY_SIZE;
}

void OpticalFlowCaptureMode::ComputeOpticalFlow(int beforeIdx, int afterIdx) {
    // Map D3D textures to CUDA
    CUresult result;

    result = cuGraphicsMapResources(1, &m_frameHistory[beforeIdx].cudaResource, 0);
    if (result != CUDA_SUCCESS) {
        LOGERR("Failed to map before frame to CUDA: %d", result);
        return;
    }

    result = cuGraphicsMapResources(1, &m_frameHistory[afterIdx].cudaResource, 0);
    if (result != CUDA_SUCCESS) {
        LOGERR("Failed to map after frame to CUDA: %d", result);
        cuGraphicsUnmapResources(1, &m_frameHistory[beforeIdx].cudaResource, 0);
        return;
    }

    // Get CUDA array pointers
    CUarray beforeArray, afterArray;
    result = cuGraphicsSubResourceGetMappedArray(&beforeArray, m_frameHistory[beforeIdx].cudaResource, 0, 0);
    if (result != CUDA_SUCCESS) {
        LOGERR("Failed to get before array: %d", result);
        cuGraphicsUnmapResources(1, &m_frameHistory[beforeIdx].cudaResource, 0);
        cuGraphicsUnmapResources(1, &m_frameHistory[afterIdx].cudaResource, 0);
        return;
    }

    result = cuGraphicsSubResourceGetMappedArray(&afterArray, m_frameHistory[afterIdx].cudaResource, 0, 0);
    if (result != CUDA_SUCCESS) {
        LOGERR("Failed to get after array: %d", result);
        cuGraphicsUnmapResources(1, &m_frameHistory[beforeIdx].cudaResource, 0);
        cuGraphicsUnmapResources(1, &m_frameHistory[afterIdx].cudaResource, 0);
        return;
    }

    // We need to copy from CUarray to linear buffers for optical flow
    // This is a bit tricky - optical flow expects linear device pointers
    // Allocate temporary buffers if needed
    CUdeviceptr beforeBuffer = 0, afterBuffer = 0;
    size_t pitch = BUF_WIDTH * 4;  // 4 bytes per pixel (RGBA)

    cuMemAllocPitch(&beforeBuffer, &pitch, BUF_WIDTH * 4, BUF_HEIGHT, 16);
    cuMemAllocPitch(&afterBuffer, &pitch, BUF_WIDTH * 4, BUF_HEIGHT, 16);

    // Copy from array to linear buffer
    CUDA_MEMCPY2D copyParams = {};
    copyParams.srcMemoryType = CU_MEMORYTYPE_ARRAY;
    copyParams.srcArray = beforeArray;
    copyParams.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copyParams.dstDevice = beforeBuffer;
    copyParams.dstPitch = pitch;
    copyParams.WidthInBytes = BUF_WIDTH * 4;
    copyParams.Height = BUF_HEIGHT;
    cuMemcpy2D(&copyParams);

    copyParams.srcArray = afterArray;
    copyParams.dstDevice = afterBuffer;
    cuMemcpy2D(&copyParams);

    // Execute optical flow
    try {
        m_opticalFlow->Execute(
            beforeBuffer,
            afterBuffer,
            m_flowVectorsBuffer,
            NV_OF_EXECUTE_PARAMS());
    }
    catch (const std::exception& e) {
        LOGERR("Optical flow execution failed: %s", e.what());
    }

    // Copy motion vectors to D3D texture
    CopyMotionVectorsToTexture();

    // Clean up
    cuMemFree(beforeBuffer);
    cuMemFree(afterBuffer);

    cuGraphicsUnmapResources(1, &m_frameHistory[beforeIdx].cudaResource, 0);
    cuGraphicsUnmapResources(1, &m_frameHistory[afterIdx].cudaResource, 0);
}

void OpticalFlowCaptureMode::ComputeBidirectionalOpticalFlow(int beforeIdx, int afterIdx) {
    // Map D3D textures to CUDA
    CUresult result;

    result = cuGraphicsMapResources(1, &m_frameHistory[beforeIdx].cudaResource, 0);
    if (result != CUDA_SUCCESS) {
        LOGERR("Failed to map before frame: %d", result);
        return;
    }

    result = cuGraphicsMapResources(1, &m_frameHistory[afterIdx].cudaResource, 0);
    if (result != CUDA_SUCCESS) {
        LOGERR("Failed to map after frame: %d", result);
        cuGraphicsUnmapResources(1, &m_frameHistory[beforeIdx].cudaResource, 0);
        return;
    }

    // Get CUDA arrays
    CUarray beforeArray, afterArray;
    cuGraphicsSubResourceGetMappedArray(&beforeArray, m_frameHistory[beforeIdx].cudaResource, 0, 0);
    cuGraphicsSubResourceGetMappedArray(&afterArray, m_frameHistory[afterIdx].cudaResource, 0, 0);

    // Allocate temporary linear buffers for optical flow
    CUdeviceptr beforeBuffer = 0, afterBuffer = 0;
    size_t pitch = BUF_WIDTH * 4;

    cuMemAllocPitch(&beforeBuffer, &pitch, BUF_WIDTH * 4, BUF_HEIGHT, 16);
    cuMemAllocPitch(&afterBuffer, &pitch, BUF_WIDTH * 4, BUF_HEIGHT, 16);

    // Copy from arrays to linear buffers
    CUDA_MEMCPY2D copyParams = {};
    copyParams.srcMemoryType = CU_MEMORYTYPE_ARRAY;
    copyParams.srcArray = beforeArray;
    copyParams.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copyParams.dstDevice = beforeBuffer;
    copyParams.dstPitch = pitch;
    copyParams.WidthInBytes = BUF_WIDTH * 4;
    copyParams.Height = BUF_HEIGHT;
    cuMemcpy2D(&copyParams);

    copyParams.srcArray = afterArray;
    copyParams.dstDevice = afterBuffer;
    cuMemcpy2D(&copyParams);

    // Compute forward flow: before -> after
    try {
        m_opticalFlow->Execute(
            beforeBuffer,
            afterBuffer,
            m_flowForwardBuffer,
            NV_OF_EXECUTE_PARAMS());

        LOG("Forward optical flow computed");
    }
    catch (const std::exception& e) {
        LOGERR("Forward optical flow failed: %s", e.what());
    }

    // Compute backward flow: after -> before
    try {
        m_opticalFlow->Execute(
            afterBuffer,
            beforeBuffer,
            m_flowBackwardBuffer,
            NV_OF_EXECUTE_PARAMS());

        LOG("Backward optical flow computed");
    }
    catch (const std::exception& e) {
        LOGERR("Backward optical flow failed: %s", e.what());
    }

    // Copy motion vectors to D3D textures
    CopyMotionVectorsToTexture(m_flowForwardBuffer, m_motionVectorForwardTexture);
    CopyMotionVectorsToTexture(m_flowBackwardBuffer, m_motionVectorBackwardTexture);

    // Clean up
    cuMemFree(beforeBuffer);
    cuMemFree(afterBuffer);
    cuGraphicsUnmapResources(1, &m_frameHistory[beforeIdx].cudaResource, 0);
    cuGraphicsUnmapResources(1, &m_frameHistory[afterIdx].cudaResource, 0);
}

void OpticalFlowCaptureMode::CopyMotionVectorsToTexture(
    CUdeviceptr flowBuffer,
    IDirect3DTexture9* texture)
{
    // Lock the texture
    D3DLOCKED_RECT lockedRect;
    HRESULT hr = texture->LockRect(0, &lockedRect, NULL, 0);
    if (FAILED(hr)) {
        LOGERR("Failed to lock motion vector texture: 0x%08x", hr);
        return;
    }

    // Allocate host buffer for motion vectors
    int16_t* cudaData = new int16_t[m_flowWidth * m_flowHeight * 2];

    // Copy from CUDA device to host
    CUDA_MEMCPY2D copyParams = {};
    copyParams.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copyParams.srcDevice = flowBuffer;
    copyParams.srcPitch = m_flowVectorsPitch;
    copyParams.dstMemoryType = CU_MEMORYTYPE_HOST;
    copyParams.dstHost = cudaData;
    copyParams.dstPitch = m_flowWidth * 2 * sizeof(int16_t);
    copyParams.WidthInBytes = m_flowWidth * 2 * sizeof(int16_t);
    copyParams.Height = m_flowHeight;

    cuMemcpy2D(&copyParams);

    // Convert to float and normalize to texture coordinates
    float* texData = (float*)lockedRect.pBits;
    for (uint32_t y = 0; y < m_flowHeight; y++) {
        for (uint32_t x = 0; x < m_flowWidth; x++) {
            int idx = (y * m_flowWidth + x) * 2;

            // Motion vectors from optical flow are typically in 1/32 pixel units
            // Convert to normalized texture coordinates
            float mvX = (float)cudaData[idx] / 32.0f / (float)BUF_WIDTH;
            float mvY = (float)cudaData[idx + 1] / 32.0f / (float)BUF_HEIGHT;

            int texIdx = (y * (lockedRect.Pitch / sizeof(float))) + x * 2;
            texData[texIdx] = mvX;
            texData[texIdx + 1] = mvY;
        }
    }

    delete[] cudaData;
    texture->UnlockRect(0);
}

void OpticalFlowCaptureMode::BlendFramesWithOpticalFlow(
    LARGE_INTEGER targetTime,
    IDirect3DSurface9* backbuffer)
{
    // Find best before/after frames
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

    // If we have both frames, do bidirectional optical flow blending
    if (bestBefore >= 0 && bestAfter >= 0) {
        // Compute bidirectional optical flow
        ComputeBidirectionalOpticalFlow(bestBefore, bestAfter);

        // Calculate blend weight
        LONGLONG totalDiff = m_frameHistory[bestAfter].timestamp.QuadPart -
                            m_frameHistory[bestBefore].timestamp.QuadPart;
        float weight = totalDiff > 0 ? (float)smallestBeforeDiff / (float)totalDiff : 0.5f;

        // Render with bidirectional optical flow shader
        m_device->SetRenderTarget(0, backbuffer);
        m_device->SetTexture(0, m_frameHistory[bestBefore].d3dTexture);
        m_device->SetTexture(1, m_frameHistory[bestAfter].d3dTexture);
        m_device->SetTexture(2, m_motionVectorForwardTexture);
        m_device->SetTexture(3, m_motionVectorBackwardTexture);

        // Set shader constants
        float constants[4] = { weight, 0.0f, 0.0f, 0.0f };
        m_device->SetPixelShaderConstantF(0, constants, 1);

        // Flow scale factor
        float flowScale[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
        m_device->SetPixelShaderConstantF(1, flowScale, 1);

        HRESULT hr = m_device->BeginScene();
        if (SUCCEEDED(hr)) {
            m_device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
            m_device->EndScene();
        }

        m_device->SetTexture(0, NULL);
        m_device->SetTexture(1, NULL);
        m_device->SetTexture(2, NULL);
        m_device->SetTexture(3, NULL);
    }
    else if (bestBefore >= 0) {
        // Fallback: copy before frame
        RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };
        m_device->StretchRect(
            m_frameHistory[bestBefore].d3dSurface,
            &srcRect,
            backbuffer,
            &srcRect,
            D3DTEXF_NONE);
    }
    else if (bestAfter >= 0) {
        // Fallback: copy after frame
        RECT srcRect = { 0, 0, (LONG)BUF_WIDTH, (LONG)BUF_HEIGHT };
        m_device->StretchRect(
            m_frameHistory[bestAfter].d3dSurface,
            &srcRect,
            backbuffer,
            &srcRect,
            D3DTEXF_NONE);
    }
}
