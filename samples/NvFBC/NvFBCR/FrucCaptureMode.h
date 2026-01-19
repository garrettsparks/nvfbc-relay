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
    // Frame history entry - stores captured frame with timestamp
    static const int FRAME_HISTORY_SIZE = 2;  // Testing with 2 buffers first
    struct FrameHistoryEntry {
        IDirect3DSurface9* d3dSurface;           // Pointer to NvFBC output buffer surface (not owned)
        struct cudaGraphicsResource* cudaResource; // CUDA Runtime API interop resource (modern API)
        void* cudaPtr;                           // Mapped CUDA pointer (when mapped)
        size_t pitch;                            // Buffer pitch in bytes
        LARGE_INTEGER timestamp;                 // High-precision capture timestamp
        bool valid;                              // Whether entry contains valid data
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

    // ===== Frame History Ring Buffer =====
    FrameHistoryEntry m_frameHistory[FRAME_HISTORY_SIZE];
    int m_currentHistoryIndex;              // Write index for ring buffer
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
    FrameHistoryEntry* GetMostRecentFrame();

    // ===== Utility =====
    void LogCudaError(const char* operation, CUresult result);
};