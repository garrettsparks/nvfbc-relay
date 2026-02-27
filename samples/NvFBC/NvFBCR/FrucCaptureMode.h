#pragma once

#include "IFrameCaptureMode.h"
#include <atomic>
#include <cuda.h>
#include <cuda_runtime.h>
#include <cudaD3D9.h>
#include <NvFBC/nvFBCCuda.h>

// NvOF SDK headers for optical flow
#include "NvOFSDK/NvOFCuda.h"

// Forward declarations - avoids circular includes
extern IDirect3D9Ex* g_pD3DEx;
extern IDirect3DDevice9Ex* g_pD3D9Device;
extern IDirect3DSurface9* g_backbuffer;
extern int BUF_WIDTH;
extern int BUF_HEIGHT;
extern NvFBCLibrary* pNVFBCLib;

// Optical Flow based frame interpolation mode
// GPU-resident pipeline: NvFBC → CUDA → NvOF → interpolation → D3D9, zero PCIe crossings
//
// V4 architecture: NvFBCCuda grabs directly to CUDA ring buffer slots — no D3D9
// on capture thread at all. Eliminates D3DCREATE_MULTITHREADED lock contention.
// Present thread reads ring metadata via atomics, selects bracket pair straddling
// prevVBlankTime, then reads ring slots directly for flow/interp.
class FrucCaptureMode : public IFrameCaptureMode {
private:
    // Frame history entry - timestamp and validity for ring buffer slots
    static const int RING_SIZE = 6;  // 6 slots: 5 completed + 1 write, enough history to bracket prevVBlankTime
    struct FrameHistoryEntry {
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
    CUstream m_cudaStream;                      // Present thread stream (flow/interp/output)
    bool m_cudaInitialized;

    // ===== CUDA-D3D9 Interop (output only) =====
    CUgraphicsResource m_cudaOutputResource;    // Registered output surface

    // ===== NvOF (Optical Flow) =====
    NvOFObj m_nvOF;                              // NvOF CUDA object
    std::vector<NvOFBufferObj> m_inputBuffers;  // 2 input frame buffers for NvOF
    std::vector<NvOFBufferObj> m_outputBuffers; // 1 output flow vector buffer
    bool m_nvofInitialized;
    uint32_t m_gridSize;                        // Flow vector grid size (e.g., 4 = 4x4 pixel blocks)

    // ===== Ring Buffer (written by capture thread) =====
    CUdeviceptr m_cudaFrames[RING_SIZE];        // CUDA ring buffer slots (GPU-resident)
    FrameHistoryEntry m_frameHistory[RING_SIZE]; // Timestamps per slot
    std::atomic<int> m_ringWriteIndex;          // Next slot to write (capture thread advances)
    std::atomic<int> m_capturedFrameCount;      // Total frames captured (atomic, cross-thread)

    // ===== GPU Buffers =====
    CUdeviceptr m_cudaOutputFrame;              // GPU interpolated output frame
    IDirect3DSurface9* m_outputSurface;         // D3D9 surface for final output

    // ===== D3D9 Resources =====
    IDirect3DDevice9Ex* m_device;

    // ===== Timing =====
    LARGE_INTEGER m_perfFreq;                   // Performance counter frequency

    // ===== Threading =====
    HANDLE m_captureThread;                     // Capture thread handle
    std::atomic<bool> m_captureRunning;         // Shutdown signal for capture thread
    std::atomic<bool> m_sessionInvalidated;     // Fatal error from capture thread
    std::atomic<int> m_captureGrabCount;        // For capture rate metric

    // ===== NvFBCCuda =====
    NvFBCCuda* m_nvfbcCuda;                     // NvFBCCuda instance (used by capture thread)
    NVFBC_CUDA_GRAB_FRAME_PARAMS m_cudaGrabParams;  // Grab params template
    NvFBCFrameGrabInfo m_grabInfo;              // Filled by grab — stride detection

    // Stride handling: if dwBufferWidth != dwWidth, grab to temp then cuMemcpy2D to ring slot
    bool m_strideChecked;
    bool m_needsStrideCopy;
    int m_grabStride;                           // dwBufferWidth * 4 bytes
    CUdeviceptr m_grabTempBuffer;               // Temp buffer for padded grabs (0 if not needed)

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
    virtual bool ManagesOwnCapture() const override { return true; }

private:
    // ===== Initialization =====
    bool InitCuda();
    bool InitNvOF();
    bool AllocateBuffers();
    bool CreateNvFBCCuda();
    void Cleanup();

    // ===== Threading =====
    static DWORD WINAPI CaptureThreadProc(LPVOID param);

    // ===== Optical Flow & Interpolation =====
    bool ComputeOpticalFlow(CUdeviceptr framePrev, CUdeviceptr frameCurr);
    bool InterpolateFrame(float weight, CUdeviceptr framePrev, CUdeviceptr frameCurr);

    // ===== GPU-Resident Present =====
    bool PresentFromGPU(IDirect3DDevice9Ex* device, LARGE_INTEGER* pPresentExStart);

    // ===== Utility =====
    void LogCudaError(const char* operation, CUresult result);
    void LogNvOFError(const char* operation, const std::exception& e);
};
