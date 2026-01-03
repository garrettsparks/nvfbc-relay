#pragma once

#include "IFrameCaptureMode.h"

// Frame blend capture mode - GPU pixel shader blending for smooth output
class FrameBlendCaptureMode : public IFrameCaptureMode {
private:
    static const int FRAME_HISTORY_SIZE = 2;
    static constexpr float BLEND_WEIGHT_THRESHOLD = 0.05f;  // Skip GPU blending if weight < this or > (1.0 - this)

    struct FrameHistoryEntry {
        IDirect3DSurface9* surface;
        LARGE_INTEGER timestamp;
        bool valid;
    };

    struct QuadVertex {
        float x, y, z;
        float u, v;
    };

    FrameHistoryEntry m_frameHistory[FRAME_HISTORY_SIZE];
    int m_currentHistoryIndex;
    IDirect3DSurface9* m_captureTarget;
    IDirect3DTexture9* m_frameTextures[FRAME_HISTORY_SIZE];
    IDirect3DTexture9* m_captureTexture;
    IDirect3DVertexShader9* m_vertexShader;
    IDirect3DPixelShader9* m_pixelShader;
    IDirect3DVertexDeclaration9* m_vertexDeclaration;
    IDirect3DVertexBuffer9* m_quadVertexBuffer;
    LARGE_INTEGER m_perfFreq;
    float m_targetFramerate;
    IDirect3DDevice9Ex* m_device;
    LARGE_INTEGER m_lastGrabTime;
    bool m_shaderAvailable;

public:
    FrameBlendCaptureMode(float framerate);
    virtual ~FrameBlendCaptureMode();

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;

private:
    bool CompileAndCreateShaders();
    bool InitBlendingRenderStates();
    void BlendFramesToBackbuffer(LARGE_INTEGER targetTime, IDirect3DSurface9* backbuffer);
};
