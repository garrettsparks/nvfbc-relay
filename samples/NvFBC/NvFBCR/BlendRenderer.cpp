#include "BlendRenderer.h"
#include <SimpleLogger.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

// Vertex layout for the fullscreen quad (position + UV). No half-texel offset: at 1:1
// texture-to-backbuffer sizes with LINEAR filtering the cost is a sub-texel sampling
// shift, invisible in the blended output.
struct QuadVertex {
    float x, y, z;
    float u, v;
};

BlendRenderer::BlendRenderer()
    : m_device(NULL)
    , m_vertexShader(NULL)
    , m_pixelShader(NULL)
    , m_vertexDeclaration(NULL)
    , m_quadVertexBuffer(NULL)
{
}

BlendRenderer::~BlendRenderer() {
    if (m_quadVertexBuffer) { m_quadVertexBuffer->Release(); m_quadVertexBuffer = NULL; }
    if (m_vertexDeclaration) { m_vertexDeclaration->Release(); m_vertexDeclaration = NULL; }
    if (m_pixelShader) { m_pixelShader->Release(); m_pixelShader = NULL; }
    if (m_vertexShader) { m_vertexShader->Release(); m_vertexShader = NULL; }
}

bool BlendRenderer::Setup(IDirect3DDevice9Ex* device) {
    m_device = device;

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

    HRESULT hr = D3DCompile(vertexShaderCode, strlen(vertexShaderCode),
        "BlendVS", NULL, NULL, "main", "vs_3_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            LOGERR("BlendRenderer: vertex shader compile error: %s", (char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        }
        return false;
    }

    hr = D3DCompile(pixelShaderCode, strlen(pixelShaderCode),
        "BlendPS", NULL, NULL, "main", "ps_3_0", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            LOGERR("BlendRenderer: pixel shader compile error: %s", (char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        }
        vsBlob->Release();
        return false;
    }

    hr = m_device->CreateVertexShader((DWORD*)vsBlob->GetBufferPointer(), &m_vertexShader);
    if (FAILED(hr)) {
        LOGERR("BlendRenderer: failed to create vertex shader (error: 0x%08x)", hr);
        vsBlob->Release(); psBlob->Release();
        return false;
    }
    hr = m_device->CreatePixelShader((DWORD*)psBlob->GetBufferPointer(), &m_pixelShader);
    if (FAILED(hr)) {
        LOGERR("BlendRenderer: failed to create pixel shader (error: 0x%08x)", hr);
        vsBlob->Release(); psBlob->Release();
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
        LOGERR("BlendRenderer: failed to create vertex declaration (error: 0x%08x)", hr);
        return false;
    }

    hr = m_device->CreateVertexBuffer(6 * sizeof(QuadVertex), D3DUSAGE_WRITEONLY, 0,
        D3DPOOL_DEFAULT, &m_quadVertexBuffer, NULL);
    if (FAILED(hr)) {
        LOGERR("BlendRenderer: failed to create vertex buffer (error: 0x%08x)", hr);
        return false;
    }
    QuadVertex* pVertices = NULL;
    hr = m_quadVertexBuffer->Lock(0, 0, (void**)&pVertices, 0);
    if (FAILED(hr)) {
        LOGERR("BlendRenderer: failed to lock vertex buffer (error: 0x%08x)", hr);
        return false;
    }
    pVertices[0] = { -1.0f,  1.0f, 0.5f,  0.0f, 0.0f };
    pVertices[1] = {  1.0f,  1.0f, 0.5f,  1.0f, 0.0f };
    pVertices[2] = { -1.0f, -1.0f, 0.5f,  0.0f, 1.0f };
    pVertices[3] = {  1.0f,  1.0f, 0.5f,  1.0f, 0.0f };
    pVertices[4] = {  1.0f, -1.0f, 0.5f,  1.0f, 1.0f };
    pVertices[5] = { -1.0f, -1.0f, 0.5f,  0.0f, 1.0f };
    m_quadVertexBuffer->Unlock();

    // Static state: LINEAR 1:1 sampling, clamped, no z/cull/lighting. Set once; nothing else
    // in the present path draws, so device state persists across frames.
    for (DWORD s = 0; s < 2; s++) {
        m_device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        m_device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    LOG("BlendRenderer initialized - ps_3_0 lerp compositor");
    return true;
}

bool BlendRenderer::Blend(IDirect3DTexture9* before, IDirect3DTexture9* after, float weight) {
    float w4[4] = { weight, 0.0f, 0.0f, 0.0f };

    HRESULT hr = m_device->BeginScene();
    if (FAILED(hr)) return false;

    m_device->SetVertexDeclaration(m_vertexDeclaration);
    m_device->SetStreamSource(0, m_quadVertexBuffer, 0, sizeof(QuadVertex));
    m_device->SetVertexShader(m_vertexShader);
    m_device->SetPixelShader(m_pixelShader);
    m_device->SetTexture(0, before);
    m_device->SetTexture(1, after);
    m_device->SetPixelShaderConstantF(0, w4, 1);

    hr = m_device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

    m_device->SetTexture(0, NULL);
    m_device->SetTexture(1, NULL);
    m_device->EndScene();

    return SUCCEEDED(hr);
}
