#pragma once

// Scaling in the D3D11 present, for WGC frames of another size than the output, drawn on the
// present's own context in place of its composite. Bilinear is one pass with the GPU's
// bilinear sampler. Bicubic (Catmull-Rom) and Lanczos-3 run in two passes, across into an
// intermediate texture the output's width and the frame's height, then down. On a downscale
// those two kernels widen with the ratio, so detail finer than the output can hold is averaged,
// not folded back as moire.

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <stdio.h>
#include <string.h>

#include "SimpleLogger.h"

class WgcScaler {
public:
    // The numbers are -wgcscale's (launch::WgcScaleNames).
    enum Filter { Bilinear = 0, Bicubic = 1, Lanczos = 2 };

    static const char* Name(int filter) {
        static const char* const kNames[] = { "bilinear", "bicubic", "lanczos" };
        return filter >= Bilinear && filter <= Lanczos ? kNames[filter] : "?";
    }

    ~WgcScaler() { Release(); }

    // Readies the filter for an output of outW x outH. Called again, it starts over.
    bool Setup(ID3D11Device* dev, ID3D11DeviceContext* ctx, int filter, int outW, int outH) {
        Release();
        m_dev = dev;
        m_ctx = ctx;
        m_filter = filter;
        m_outW = outW;
        m_outH = outH;
        D3D11_BUFFER_DESC bd;
        ZeroMemory(&bd, sizeof(bd));
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = m_dev->CreateBuffer(&bd, NULL, &m_cb);
        if (FAILED(hr)) {
            LOGERR("WGC scaling %s: no constant buffer (0x%08x)", Name(m_filter), (unsigned)hr);
            return false;
        }
        if (filter == Bilinear) {
            if (!CompilePs(kBilinearBody, "", &m_psBilinear)) return false;
            D3D11_SAMPLER_DESC sm;
            ZeroMemory(&sm, sizeof(sm));
            sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sm.ComparisonFunc = D3D11_COMPARISON_NEVER;
            sm.MaxLOD = D3D11_FLOAT32_MAX;
            hr = m_dev->CreateSamplerState(&sm, &m_linear);
            if (FAILED(hr)) {
                LOGERR("WGC scaling %s: no bilinear sampler (0x%08x)", Name(m_filter),
                       (unsigned)hr);
                return false;
            }
            return true;
        }
        const int radius = filter == Lanczos ? 3 : 2;
        char defs[96];
        snprintf(defs, sizeof(defs), "#define FILTER %d\n#define RADIUS %d\n#define PASS 1\n",
                 filter, radius);
        if (!CompilePs(kTwoPassBody, defs, &m_psAcross)) return false;
        snprintf(defs, sizeof(defs), "#define FILTER %d\n#define RADIUS %d\n#define PASS 2\n",
                 filter, radius);
        return CompilePs(kTwoPassBody, defs, &m_psDown);
    }

    // The composite of a and b (blend weight w), scaled into rtv, the output. The caller has
    // set the input layout, topology and vertex shader; this sets the rest.
    bool Draw(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, float w,
              ID3D11RenderTargetView* rtv) {
        if (m_filter == Bilinear) return DrawBilinear(a, b, w, rtv);
        return DrawTwoPass(a, b, w, rtv);
    }

private:
    bool DrawBilinear(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, float w,
                      ID3D11RenderTargetView* rtv) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_ctx->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            float* p = (float*)mapped.pData;
            p[0] = w; p[1] = 0.0f; p[2] = 0.0f; p[3] = 0.0f;
            m_ctx->Unmap(m_cb, 0);
        }
        D3D11_VIEWPORT vp;
        vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
        vp.Width = (float)m_outW; vp.Height = (float)m_outH;
        vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
        ID3D11ShaderResourceView* srvs[2] = { a, b };
        m_ctx->OMSetRenderTargets(1, &rtv, NULL);
        m_ctx->RSSetViewports(1, &vp);
        m_ctx->PSSetShader(m_psBilinear, NULL, 0);
        m_ctx->PSSetShaderResources(0, 2, srvs);
        m_ctx->PSSetSamplers(0, 1, &m_linear);
        m_ctx->PSSetConstantBuffers(0, 1, &m_cb);
        m_ctx->Draw(3, 0);
        return true;
    }

    bool DrawTwoPass(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, float w,
                     ID3D11RenderTargetView* rtv) {
        ID3D11Resource* res = NULL;
        a->GetResource(&res);
        D3D11_TEXTURE2D_DESC d;
        ((ID3D11Texture2D*)res)->GetDesc(&d);
        res->Release();
        if (!Intermediate((int)d.Height)) return false;
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_ctx->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            float* p = (float*)mapped.pData;
            p[0] = w; p[1] = (float)m_outW; p[2] = (float)m_outH; p[3] = (float)m_interH;
            m_ctx->Unmap(m_cb, 0);
        }
        D3D11_VIEWPORT vp;
        vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
        vp.Width = (float)m_outW; vp.Height = (float)m_interH;
        vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
        ID3D11ShaderResourceView* srvs[2] = { a, b };
        m_ctx->OMSetRenderTargets(1, &m_interRtv, NULL);
        m_ctx->RSSetViewports(1, &vp);
        m_ctx->PSSetShader(m_psAcross, NULL, 0);
        m_ctx->PSSetShaderResources(0, 2, srvs);
        m_ctx->PSSetConstantBuffers(0, 1, &m_cb);
        m_ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none[2] = { NULL, NULL };
        m_ctx->PSSetShaderResources(0, 2, none);
        vp.Height = (float)m_outH;
        m_ctx->OMSetRenderTargets(1, &rtv, NULL);
        m_ctx->RSSetViewports(1, &vp);
        m_ctx->PSSetShader(m_psDown, NULL, 0);
        m_ctx->PSSetShaderResources(0, 1, &m_interSrv);
        m_ctx->Draw(3, 0);
        m_ctx->PSSetShaderResources(0, 1, none);
        return true;
    }

    bool CompilePs(const char* body, const char* defines, ID3D11PixelShader** out) {
        const size_t n = strlen(defines) + strlen(body) + 1;
        char* src = new char[n];
        snprintf(src, n, "%s%s", defines, body);
        ID3DBlob* blob = NULL;
        ID3DBlob* err = NULL;
        HRESULT hr = D3DCompile(src, strlen(src), "WgcScalerPS", NULL, NULL, "main", "ps_5_0", 0,
                                0, &blob, &err);
        delete[] src;
        if (FAILED(hr)) {
            LOGERR("WGC scaling %s: a shader did not compile (0x%08x): %s", Name(m_filter),
                   (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "(no message)");
            if (err) err->Release();
            return false;
        }
        if (err) err->Release();
        hr = m_dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, out);
        blob->Release();
        if (FAILED(hr)) {
            LOGERR("WGC scaling %s: no pixel shader (0x%08x)", Name(m_filter), (unsigned)hr);
            return false;
        }
        return true;
    }

    // The across pass's target, the output's width and the frame's height, made at the first
    // frame that needs it and again if a frame's height changes.
    bool Intermediate(int height) {
        if (m_interTex && m_interH == height) return true;
        if (m_interSrv) m_interSrv->Release();
        if (m_interRtv) m_interRtv->Release();
        if (m_interTex) m_interTex->Release();
        m_interSrv = NULL;
        m_interRtv = NULL;
        m_interTex = NULL;
        D3D11_TEXTURE2D_DESC td;
        ZeroMemory(&td, sizeof(td));
        td.Width = (UINT)m_outW;
        td.Height = (UINT)height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = m_dev->CreateTexture2D(&td, NULL, &m_interTex);
        if (SUCCEEDED(hr)) hr = m_dev->CreateRenderTargetView(m_interTex, NULL, &m_interRtv);
        if (SUCCEEDED(hr)) hr = m_dev->CreateShaderResourceView(m_interTex, NULL, &m_interSrv);
        if (FAILED(hr)) {
            LOGERR("WGC scaling %s: no intermediate texture %dx%d (0x%08x)", Name(m_filter),
                   m_outW, height, (unsigned)hr);
            return false;
        }
        m_interH = height;
        LOG("WGC scaling %s: intermediate texture %dx%d", Name(m_filter), m_outW, height);
        return true;
    }

    void Release() {
        if (m_psBilinear) m_psBilinear->Release();
        if (m_linear) m_linear->Release();
        if (m_psAcross) m_psAcross->Release();
        if (m_psDown) m_psDown->Release();
        if (m_cb) m_cb->Release();
        if (m_interSrv) m_interSrv->Release();
        if (m_interRtv) m_interRtv->Release();
        if (m_interTex) m_interTex->Release();
        m_psBilinear = NULL;
        m_linear = NULL;
        m_psAcross = NULL;
        m_psDown = NULL;
        m_cb = NULL;
        m_interSrv = NULL;
        m_interRtv = NULL;
        m_interTex = NULL;
        m_interH = 0;
    }

    // params.x is the blend weight. A present of one frame (weight 0) samples it once.
    static constexpr const char* kBilinearBody =
        "Texture2D texA : register(t0);\n"
        "Texture2D texB : register(t1);\n"
        "SamplerState samp : register(s0);\n"
        "cbuffer Params : register(b0) { float4 params; };\n"
        "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
        "  float3 a = texA.SampleLevel(samp, uv, 0).rgb;\n"
        "  if (params.x <= 0.0) return float4(a, 1.0);\n"
        "  return float4(lerp(a, texB.SampleLevel(samp, uv, 0).rgb, params.x), 1.0);\n"
        "}\n";

    // params.x is the blend weight, yz the output's size, w the intermediate's height. Across:
    // each intermediate pixel filters its source row; a blend filters both frames and mixes
    // them. Down: each output pixel filters its intermediate column. Reads are Loads, so the
    // frame's own size comes from the texture.
    static constexpr const char* kTwoPassBody =
        "Texture2D texA : register(t0);\n"
        "Texture2D texB : register(t1);\n"
        "cbuffer Params : register(b0) { float4 params; };\n"
        "float K(float x) {\n"
        "  x = abs(x);\n"
        "#if FILTER == 1\n"
        "  if (x < 1.0) return (1.5 * x - 2.5) * x * x + 1.0;\n"
        "  if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;\n"
        "  return 0.0;\n"
        "#else\n"
        "  if (x < 1e-4) return 1.0;\n"
        "  if (x >= 3.0) return 0.0;\n"
        "  float px = 3.14159265 * x;\n"
        "  return 3.0 * sin(px) * sin(px / 3.0) / (px * px);\n"
        "#endif\n"
        "}\n"
        "#if PASS == 1\n"
        "float3 Across(Texture2D t, float2 pos) {\n"
        "  uint w, h;\n"
        "  t.GetDimensions(w, h);\n"
        "  int row = min(int(pos.y * h / params.w), int(h) - 1);\n"
        "  float scale = w / params.y;\n"
        "  float s = max(scale, 1.0);\n"
        "  float p = pos.x * scale - 0.5;\n"
        "  float r = RADIUS * s;\n"
        "  int a = int(ceil(p - r));\n"
        "  int b = int(floor(p + r));\n"
        "  float3 sum = 0;\n"
        "  float tw = 0;\n"
        "  [loop] for (int x = a; x <= b; x++) {\n"
        "    float k = K((x - p) / s);\n"
        "    sum += k * t.Load(int3(clamp(x, 0, int(w) - 1), row, 0)).rgb;\n"
        "    tw += k;\n"
        "  }\n"
        "  return sum / tw;\n"
        "}\n"
        "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
        "  float3 a = Across(texA, pos.xy);\n"
        "  if (params.x <= 0.0) return float4(a, 1.0);\n"
        "  return float4(lerp(a, Across(texB, pos.xy), params.x), 1.0);\n"
        "}\n"
        "#else\n"
        "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
        "  uint w, h;\n"
        "  texA.GetDimensions(w, h);\n"
        "  int col = min(int(pos.x), int(w) - 1);\n"
        "  float scale = h / params.z;\n"
        "  float s = max(scale, 1.0);\n"
        "  float p = pos.y * scale - 0.5;\n"
        "  float r = RADIUS * s;\n"
        "  int a = int(ceil(p - r));\n"
        "  int b = int(floor(p + r));\n"
        "  float3 sum = 0;\n"
        "  float tw = 0;\n"
        "  [loop] for (int y = a; y <= b; y++) {\n"
        "    float k = K((y - p) / s);\n"
        "    sum += k * texA.Load(int3(col, clamp(y, 0, int(h) - 1), 0)).rgb;\n"
        "    tw += k;\n"
        "  }\n"
        "  return float4(saturate(sum / tw), 1.0);\n"
        "}\n"
        "#endif\n";

    ID3D11Device* m_dev = NULL;
    ID3D11DeviceContext* m_ctx = NULL;
    int m_filter = Bilinear;
    int m_outW = 0, m_outH = 0;
    ID3D11Buffer* m_cb = NULL;
    ID3D11PixelShader* m_psBilinear = NULL;
    ID3D11SamplerState* m_linear = NULL;
    ID3D11PixelShader* m_psAcross = NULL;
    ID3D11PixelShader* m_psDown = NULL;
    ID3D11Texture2D* m_interTex = NULL;
    ID3D11RenderTargetView* m_interRtv = NULL;
    ID3D11ShaderResourceView* m_interSrv = NULL;
    int m_interH = 0;
};
