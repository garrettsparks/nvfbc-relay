#pragma once

#include "IFrameCaptureMode.h"
#include <cuda.h>
#include <cuda_runtime.h>

// NvOF SDK headers for optical flow
#include "NvOFSDK/NvOFCuda.h"

// Forward declarations - avoids circular includes
extern IDirect3D9Ex* g_pD3DEx;
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;

// Optical Flow based frame interpolation mode
// Phase 4: Uses NvOFA for motion vectors + custom CUDA kernel for interpolation
class FrucCaptureMode : public IFrameCaptureMode {
private:
    // Frame history entry - stores captured frame with timestamp
    static const int FRAME_HISTORY_SIZE = 2;  // Need 2 frames for interpolation
    struct FrameHistoryEntry {
        IDirect3DSurface9* d3dSurface;           // Owned D3D9 surface for this frame history slot
        uint8_t* hostBuffer;                      // Host-side staging buffer for D3D9 Lock data
        size_t hostBufferSize;                    // Size of host buffer in bytes
        LARGE_INTEGER timestamp;                  // High-precision capture timestamp
        bool valid;                               // Whether entry contains valid data
    };

    // ===== Configuration =====
    float m_targetFramerate;
    bool m_isVsyncMode;
    int m_width;
    int m_height;
    int m_flowWidth;                            // Width of flow vector grid
    int m_flowHeight;                           // Height of flow vector grid

    // ===== CUDA Resources =====
    CUcontext m_cuContext;
    CUdevice m_cuDevice;
    CUstream m_cudaStream;
    bool m_cudaInitialized;

    // ===== NvOF (Optical Flow) =====
    NvOFObj m_nvOF;                              // NvOF CUDA object
    std::vector<NvOFBufferObj> m_inputBuffers;  // 2 input frame buffers for NvOF
    std::vector<NvOFBufferObj> m_outputBuffers; // 1 output flow vector buffer
    bool m_nvofInitialized;
    uint32_t m_gridSize;                        // Flow vector grid size (e.g., 4 = 4x4 pixel blocks)

    // ===== Frame History Ring Buffer =====
    FrameHistoryEntry m_frameHistory[FRAME_HISTORY_SIZE];
    int m_currentHistoryIndex;                  // Write index for ring buffer
    int m_capturedFrameCount;                   // Total frames captured

    // ===== Output Buffers =====
    CUdeviceptr m_cudaFrame0;                   // GPU copy of frame 0 (for warp kernel)
    CUdeviceptr m_cudaFrame1;                   // GPU copy of frame 1 (for warp kernel)
    CUdeviceptr m_cudaOutputFrame;              // GPU interpolated output frame
    uint8_t* m_hostOutputBuffer;                // Host-side output for D3D9 copy
    IDirect3DSurface9* m_outputSurface;         // D3D9 surface for final output

    // ===== D3D9 Resources =====
    IDirect3DDevice9Ex* m_device;
    IDirect3DSurface9* m_captureTarget;         // Single NvFBC capture target (NvFBC always writes here)

    // ===== Timing =====
    LARGE_INTEGER m_perfFreq;                   // Performance counter frequency
    LARGE_INTEGER m_lastPresentTime;            // Last presentation timestamp

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
    bool InitNvOF();
    bool AllocateBuffers();
    void Cleanup();

    // ===== Frame Management =====
    bool CaptureFrame(NvFBCToDx9Vid* nvfbcDx9, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grabParams);
    bool CopyFrameToHost(int historyIndex);
    FrameHistoryEntry* GetFrame(int offset);  // 0 = most recent, 1 = previous

    // ===== Optical Flow & Interpolation =====
    bool ComputeOpticalFlow();
    bool InterpolateFrame(float weight);

    // ===== Utility =====
    void LogCudaError(const char* operation, CUresult result);
    void LogNvOFError(const char* operation, const std::exception& e);
};
