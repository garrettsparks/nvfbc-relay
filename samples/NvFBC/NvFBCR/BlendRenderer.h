#pragma once

#include <windows.h>
#include <d3d9.h>

// GPU lerp of two ring textures onto the current render target (the backbuffer).
//
// Fullscreen quad + ps_3_0 `lerp(t0, t1, w)` — the shader machinery validated by the pre-ring
// blend modes, re-hosted as a standalone renderer so TemporalCaptureMode composes it per
// present. The lerp runs in the stored encoding (gamma-space); measurably fine for adjacent
// video frames — see docs/blend-mode-spec.md "Color math" before upgrading.
//
// Not copyable: owns D3D9 COM objects.
class BlendRenderer {
public:
    BlendRenderer();
    ~BlendRenderer();

    BlendRenderer(const BlendRenderer&) = delete;
    BlendRenderer& operator=(const BlendRenderer&) = delete;

    // Compile shaders, build the quad, set the static sampler/render state.
    // Loud failure (LOGERR + false) — callers refuse the mode rather than run degraded.
    bool Setup(IDirect3DDevice9Ex* device);

    // Draw lerp(before, after, weight) over the current render target.
    // weight 0 -> pure before, 1 -> pure after. Returns false on draw failure.
    bool Blend(IDirect3DTexture9* before, IDirect3DTexture9* after, float weight);

private:
    IDirect3DDevice9Ex* m_device;
    IDirect3DVertexShader9* m_vertexShader;
    IDirect3DPixelShader9* m_pixelShader;
    IDirect3DVertexDeclaration9* m_vertexDeclaration;
    IDirect3DVertexBuffer9* m_quadVertexBuffer;
};
