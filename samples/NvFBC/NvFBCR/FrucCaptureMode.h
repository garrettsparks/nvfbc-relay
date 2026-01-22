#pragma once

#include "IFrameCaptureMode.h"
#include <cuda.h>
#include <cuda_runtime.h>
#include <cuda_d3d9_interop.h>

// FRUC and Optical Flow SDK headers
#include "NvOFFRUC.h"
#include "nvOpticalFlowCuda.h"
#include "nvOpticalFlowCommon.h"

// Forward declarations - avoids circular includes
extern IDirect3D9Ex* g_pD3DEx;
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;

// FRUC (Frame Rate Up-Conversion) based optical flow interpolation mode
// Phase 3: Async capture with frame history (zero-copy via D3D9-CUDA interop)
class FrucCaptureMode : public IFrameCaptureMode {
private:
    // NOTE: FRUC maintains its own internal frame cache.
    // We only need 1 NvFBC buffer + 1 CUDA buffer (not multi-buffer history)

    struct FrameBuffer {
        IDirect3DSurface9* d3dSurface;           // D3D9 surface
        struct cudaGraphicsResource* cudaResource; // CUDA Runtime API interop resource
        void* cudaPtr;                           // Mapped CUDA pointer (when mapped)
        size_t pitch;                            // Buffer pitch in bytes
        bool isMapped;                           // Whether CUDA resource is currently mapped
    };

    // ===== Configuration =====
    float m_targetFramerate;
    bool m_isVsyncMode;
    int m_width;
    int m_height;

    // ===== CUDA Resources =====
    CUcontext m_cuContext;
    CUdevice m_cuDevice;
    bool m_cudaInitialized;

    // ===== Frame Buffers =====
    FrameBuffer m_nvfbcBuffer;              // NvFBC captures here (not CUDA-registered)
    FrameBuffer m_cudaBuffer;               // CUDA-registered surface (for Phase 4 FRUC)
    int m_capturedFrameCount;               // Total frames captured

    // ===== D3D9 Resources =====
    IDirect3DDevice9Ex* m_device;

    // ===== Timing =====
    LARGE_INTEGER m_perfFreq;               // Performance counter frequency
    LARGE_INTEGER m_lastPresentTime;        // Last presentation timestamp

public:
    FrucCaptureMode(float framerate);
    virtual ~FrucCaptureMode();

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
    void Cleanup();

    // ===== Frame Management =====
    bool CaptureFrame(NvFBCToDx9Vid* nvfbcDx9, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams);

    // ===== Utility =====
    void LogCudaError(const char* operation, CUresult result);
};
