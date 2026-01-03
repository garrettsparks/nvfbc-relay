#pragma once

#include "IFrameCaptureMode.h"
#include <cuda.h>
#include <cuda_runtime.h>
#include <cuda_d3d9_interop.h>

// Forward declarations for NVIDIA Optical Flow SDK
class NvOFCuda;
typedef void* NvOFHandle;

class OpticalFlowCaptureMode : public IFrameCaptureMode {
private:
    static const int FRAME_HISTORY_SIZE = 3;  // Need at least 2, extra for safety

    struct FrameHistoryEntry {
        IDirect3DSurface9* d3dSurface;
        IDirect3DTexture9* d3dTexture;
        CUgraphicsResource cudaResource;  // For D3D-CUDA interop
        CUdeviceptr cudaBuffer;           // Mapped CUDA buffer
        LARGE_INTEGER timestamp;
        bool valid;
    };

    struct QuadVertex {
        float x, y, z;
        float u, v;
    };

    // CUDA/Optical Flow resources
    CUcontext m_cuContext;
    CUdevice m_cuDevice;
    NvOFCuda* m_opticalFlow;
    NvOFHandle m_ofHandle;

    // Optical flow buffers
    CUdeviceptr m_flowVectorsBuffer;      // Motion vectors from optical flow
    size_t m_flowVectorsPitch;
    uint32_t m_flowWidth;
    uint32_t m_flowHeight;

    // Frame history
    FrameHistoryEntry m_frameHistory[FRAME_HISTORY_SIZE];
    int m_currentHistoryIndex;

    // D3D9 capture resources
    IDirect3DSurface9* m_captureTarget;
    IDirect3DTexture9* m_captureTexture;
    CUgraphicsResource m_captureCudaResource;

    // D3D9 rendering resources
    IDirect3DDevice9Ex* m_device;
    IDirect3DVertexShader9* m_vertexShader;
    IDirect3DPixelShader9* m_pixelShader;
    IDirect3DVertexDeclaration9* m_vertexDeclaration;
    IDirect3DVertexBuffer9* m_quadVertexBuffer;

    // Motion vector texture (for pixel shader)
    IDirect3DTexture9* m_motionVectorTexture;
    CUgraphicsResource m_motionVectorCudaResource;

    // Timing
    LARGE_INTEGER m_perfFreq;
    float m_targetFramerate;
    bool m_isVsyncMode;

public:
    OpticalFlowCaptureMode(float framerate);  // 0.0 for vsync mode
    virtual ~OpticalFlowCaptureMode();

    virtual UINT GetPresentationInterval() const override;
    virtual bool Setup() override;
    virtual void Run(
        NvFBCToDx9Vid* nvfbcDx9,
        NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams,
        IDirect3DDevice9Ex* device,
        HWND hwnd) override;
    virtual const char* GetModeName() const override;

private:
    bool InitCuda();
    bool InitOpticalFlow();
    bool CompileAndCreateShaders();
    bool InitBlendingRenderStates();
    bool CreateFrameHistoryResources();

    void ComputeOpticalFlow(int beforeIdx, int afterIdx);
    void CopyMotionVectorsToTexture();
    void BlendFramesWithOpticalFlow(
        LARGE_INTEGER targetTime,
        IDirect3DSurface9* backbuffer);

    void CaptureFrameToHistory(IDirect3DSurface9* source, LARGE_INTEGER timestamp);
    void RegisterD3DTextureWithCuda(IDirect3DTexture9* texture, CUgraphicsResource* resource);
    void UnregisterD3DResource(CUgraphicsResource resource);
};
