#pragma once

#include "IFrameCaptureMode.h"
#include <cuda.h>
#include <cuda_runtime.h>

// Forward declarations for NVIDIA Optical Flow SDK
class NvOFCuda;

// Optical Flow capture mode - Motion-compensated frame interpolation for VRR sources
class OpticalFlowCaptureMode : public IFrameCaptureMode {
private:
    static const int FRAME_HISTORY_SIZE = 3;  // Need at least 2 for interpolation, 3 for safety

    struct FrameHistoryEntry {
        IDirect3DSurface9* surface;
        IDirect3DTexture9* texture;
        LARGE_INTEGER timestamp;
        bool valid;
    };

    struct QuadVertex {
        float x, y, z;
        float u, v;
    };

    // ===== Frame History =====
    FrameHistoryEntry m_frameHistory[FRAME_HISTORY_SIZE];
    int m_currentHistoryIndex;

    // ===== D3D9 Resources =====
    IDirect3DSurface9* m_captureTarget;
    IDirect3DTexture9* m_captureTexture;
    IDirect3DDevice9Ex* m_device;
    IDirect3DVertexShader9* m_vertexShader;
    IDirect3DPixelShader9* m_pixelShader;
    IDirect3DVertexDeclaration9* m_vertexDeclaration;
    IDirect3DVertexBuffer9* m_quadVertexBuffer;

    // ===== CUDA Resources (placeholders for now) =====
    CUcontext m_cuContext;
    CUdevice m_cuDevice;
    bool m_cudaInitialized;

    // ===== Optical Flow (placeholders for now) =====
    NvOFCuda* m_opticalFlow;
    bool m_opticalFlowInitialized;

    // ===== Timing =====
    LARGE_INTEGER m_perfFreq;
    float m_targetFramerate;
    bool m_isVsyncMode;

public:
    OpticalFlowCaptureMode(float framerate);  // framerate=0.0 for vsync mode
    virtual ~OpticalFlowCaptureMode();

    // IFrameCaptureMode interface
    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;

private:
    // ===== Initialization =====
    bool InitCuda();
    bool InitOpticalFlow();
    bool CreateFrameHistoryResources();
    bool CompileAndCreateShaders();
    bool InitBlendingRenderStates();

    // ===== Cleanup =====
    void CleanupCuda();
    void CleanupOpticalFlow();

    // ===== Frame Processing =====
    void CaptureFrameToHistory(IDirect3DSurface9* source, LARGE_INTEGER timestamp);
    void BlendFramesWithOpticalFlow(LARGE_INTEGER targetTime, IDirect3DSurface9* backbuffer);
};