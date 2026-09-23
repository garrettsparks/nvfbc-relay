#pragma once

#include <windows.h>
#include <d3d9.h>

// GPU lerp of two ring textures onto the current render target (the backbuffer).
//
// Fullscreen quad + ps_3_0 `lerp(t0, t1, w)`. Stateless per present and decision-free:
// the caller picks the textures and the weight; this class only draws. The lerp runs in
// the stored encoding (gamma space), accepted for adjacent video frames whose endpoints
// are a small fraction of a frame period apart; a linear-light upgrade needs a measured
// artifact to justify the conversion cost.
//
// Not copyable: owns D3D9 COM objects.
class BlendRenderer {
public:
    BlendRenderer();
    ~BlendRenderer();

    BlendRenderer(const BlendRenderer&) = delete;
    BlendRenderer& operator=(const BlendRenderer&) = delete;

    // Compile shaders, build the quad, set the static sampler/render state. tint compiles
    // the extra debug variant as well. Loud failure (LOGERR + false) so callers refuse the
    // mode rather than run degraded.
    bool Setup(IDirect3DDevice9Ex* device, bool tint = false);

    // Draw lerp(before, after, weight) over the current render target.
    // weight 0 -> pure before, 1 -> pure after. Returns false on draw failure.
    //
    // With the debug tint enabled at Setup, this binds a SECOND pixel shader that also
    // stamps a coloured border, marking the frame as synthesized so blends are obvious at
    // playback speed. It is a separate shader rather than a branch on a constant so the
    // normal path executes the exact same instructions it does in an untinted build: the
    // cost of a debug aid must not land on the frames being measured. The tint rides the
    // existing full-screen pass, so it adds no draw call, no state change and no fill.
    bool Blend(IDirect3DTexture9* before, IDirect3DTexture9* after, float weight);

private:
    IDirect3DDevice9Ex* m_device;
    IDirect3DVertexShader9* m_vertexShader;
    IDirect3DPixelShader9* m_pixelShader;
    IDirect3DPixelShader9* m_tintPixelShader;   // NULL unless the debug tint was requested
    IDirect3DVertexDeclaration9* m_vertexDeclaration;
    IDirect3DVertexBuffer9* m_quadVertexBuffer;
};
